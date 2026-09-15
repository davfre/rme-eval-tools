// ratecheck - does the DRIVER deliver the rate it says it does?
//
// ratesweep measures the device on the wire, bypassing the driver entirely.
// This is the other half: it goes through ALSA, the way an application does,
// and times the capture stream against the clock.
//
// Method (the same idea upstream used to catch the 44.1 kHz bug, which is
// what makes it worth having our own copy): ask for R Hz, read exactly R
// frames, and time how long that takes. snd_pcm_readi blocks until the
// device has actually produced them, so if the hardware is running at D
// instead of R the read takes R/D seconds instead of one, and the measured
// rate comes back as D. No signal analysis, no test tone.
//
// Build:  gcc -O2 -Wall -o ratecheck ratecheck.c -lasound
// Run:    ./ratecheck            (auto-finds the Babyface card)
//         ./ratecheck hw:2       (or name it)
//
// The card must be free: stop anything recording, and if PipeWire holds it,
//   pactl list cards short
//   pactl suspend-card <card> 1        # ... and 0 afterwards

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <alsa/asoundlib.h>

static const unsigned int rates[] = {
	32000, 44100, 48000, 64000, 88200, 96000, 128000, 176400, 192000,
};
#define NRATES ((int)(sizeof(rates) / sizeof(rates[0])))

#define CHANS 2
#define SETTLE_S 0.30		/* let the rate change settle after start */
#define WARM_S   0.30		/* then discard this much before timing */
#define WINDOW_S 2.00		/* and time over this long */

static double now_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* First card whose /proc/asound/cards line mentions the Babyface. */
static int find_card(char *out, size_t n)
{
	char line[256];
	FILE *f = fopen("/proc/asound/cards", "r");
	int idx = -1, last = -1;

	if (!f)
		return -1;
	/* Each card is two lines: " 2 [Pro            ]: ..." then its long
	 * name. Track the most recent index seen and match on either line. */
	while (fgets(line, sizeof(line), f)) {
		int i;

		if (sscanf(line, " %d ", &i) == 1)
			last = i;
		if (last >= 0 && (strcasestr(line, "babyface") ||
				  strcasestr(line, "bbf"))) {
			idx = last;
			break;
		}
	}
	fclose(f);
	if (idx < 0)
		return -1;
	snprintf(out, n, "hw:%d", idx);
	return 0;
}

/* Returns the measured rate, 0 on failure (reason printed), or -1 when the
 * card is busy. Sets *xrun when the window was spoilt by an xrun. */
static double measure(const char *dev, unsigned int want, int *xrun)
{
	snd_pcm_t *pcm = NULL;
	snd_pcm_hw_params_t *hw;
	unsigned int got = want;
	snd_pcm_uframes_t period = 0;
	unsigned int ch = CHANS;
	int err;
	int32_t *buf;
	double t0, t1;
	snd_pcm_sframes_t n;
	unsigned long long frames;

	err = snd_pcm_open(&pcm, dev, SND_PCM_STREAM_CAPTURE, 0);
	if (err < 0) {
		printf("open: %s\n", snd_strerror(err));
		/* The driver has exactly one capture substream, so a sound
		 * server holding it locks this out completely. Say so once
		 * and stop, rather than repeating the same line nine times. */
		if (err == -EBUSY)
			return -1;
		return 0;
	}
	snd_pcm_hw_params_alloca(&hw);
	snd_pcm_hw_params_any(pcm, hw);
	snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
	snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S32_LE);

	/* Exact rate only: a driver that silently substitutes another rate is
	 * precisely the failure being looked for, so do not let ALSA pick a
	 * near rate and hide it. */
	err = snd_pcm_hw_params_set_rate(pcm, hw, want, 0);
	if (err < 0) {
		printf("rate refused: %s\n", snd_strerror(err));
		snd_pcm_close(pcm);
		return 0;
	}
	if (snd_pcm_hw_params_set_channels_near(pcm, hw, &ch) < 0)
		ch = CHANS;
	err = snd_pcm_hw_params(pcm, hw);
	if (err < 0) {
		printf("hw_params: %s\n", snd_strerror(err));
		snd_pcm_close(pcm);
		return 0;
	}
	snd_pcm_hw_params_get_rate(hw, &got, NULL);
	snd_pcm_hw_params_get_period_size(hw, &period, NULL);
	if (got != want)
		printf("[ALSA reports %u] ", got);

	buf = malloc(period * ch * sizeof(*buf));
	if (!buf) {
		snd_pcm_close(pcm);
		return 0;
	}
	if ((err = snd_pcm_prepare(pcm)) < 0 ||
	    (err = snd_pcm_start(pcm)) < 0) {
		printf("start: %s\n", snd_strerror(err));
		free(buf);
		snd_pcm_close(pcm);
		return 0;
	}

	/* A rate change restarts the stream, so give it a moment before
	 * reading anything at all, then discard a while longer. */
	t0 = now_s();
	while (now_s() - t0 < SETTLE_S + WARM_S) {
		n = snd_pcm_readi(pcm, buf, period);
		if (n < 0 && snd_pcm_recover(pcm, (int)n, 1) < 0)
			break;
	}

	/* Time between two READ COMPLETIONS, and count the frames actually
	 * returned in between.
	 *
	 * The obvious version - "read exactly R frames and see if it takes a
	 * second" - is wrong by up to one period, because readi only wakes on
	 * period boundaries: the window ends somewhere inside a period rather
	 * than on the frame asked for. At a 1024-frame period that is ~21-24
	 * ms, which reads as a 2.3 % rate error at every rate alike. Taking
	 * both endpoints on completions makes the interval cover exactly the
	 * frames counted, with no partial period at either end.
	 */
	for (;;) {
		n = snd_pcm_readi(pcm, buf, period);
		if (n < 0) {
			if (snd_pcm_recover(pcm, (int)n, 1) < 0) {
				printf("read: %s\n", snd_strerror((int)n));
				free(buf);
				snd_pcm_close(pcm);
				return 0;
			}
			continue;
		}
		break;
	}
	t0 = now_s();
	t1 = t0;
	frames = 0;
	while (t1 - t0 < WINDOW_S) {
		n = snd_pcm_readi(pcm, buf, period);
		if (n < 0) {
			/* An xrun mid-window loses an unknown number of frames,
			 * so the count no longer matches the elapsed time.
			 * Report it rather than quietly returning a low rate. */
			if (snd_pcm_recover(pcm, (int)n, 1) < 0) {
				printf("read: %s\n", snd_strerror((int)n));
				free(buf);
				snd_pcm_close(pcm);
				return 0;
			}
			*xrun = 1;
			free(buf);
			snd_pcm_close(pcm);
			return 0;
		}
		frames += (unsigned long long)n;
		t1 = now_s();
	}

	free(buf);
	snd_pcm_close(pcm);
	return frames / (t1 - t0);
}

int main(int argc, char **argv)
{
	char dev[32];
	int i, bad = 0, xrun = 0;

	if (argc > 1) {
		snprintf(dev, sizeof(dev), "%s", argv[1]);
	} else if (find_card(dev, sizeof(dev)) < 0) {
		fprintf(stderr, "no Babyface in /proc/asound/cards - "
			"is the driver loaded?  Pass the device, e.g. hw:2\n");
		return 1;
	}
	printf("device: %s\n\n", dev);
	printf("  %-10s %-12s %-9s\n", "requested", "measured", "error");

	for (i = 0; i < NRATES; i++) {
		double hz;

		printf("  %-10u ", rates[i]);
		fflush(stdout);
		hz = measure(dev, rates[i], &xrun);
		if (xrun) {
			printf("xrun during the window - retrying\n");
			printf("  %-10u ", rates[i]);
			fflush(stdout);
			xrun = 0;
			hz = measure(dev, rates[i], &xrun);
			if (xrun) {
				printf("xrun again - result not trustworthy\n");
				bad++;
				continue;
			}
		}
		if (hz < 0) {
			printf("\nThe card is held by something else - almost"
			       " certainly PipeWire.\n"
			       "Free it for the duration of the test:\n\n"
			       "  pactl list cards short\n"
			       "  pactl set-card-profile <card> off\n"
			       "  ./ratecheck\n"
			       "  pactl set-card-profile <card> <profile>\n\n"
			       "or, if that is not enough:\n\n"
			       "  systemctl --user stop wireplumber pipewire.socket pipewire\n"
			       "  ./ratecheck\n"
			       "  systemctl --user start pipewire.socket wireplumber\n");
			return 1;
		}
		if (hz == 0)
			continue;
		printf("%-12.0f %+7.3f %%   %s\n", hz,
		       100.0 * (hz - rates[i]) / rates[i],
		       (hz > rates[i] * 0.995 && hz < rates[i] * 1.005)
		       ? "ok" : "WRONG");
		if (!(hz > rates[i] * 0.995 && hz < rates[i] * 1.005))
			bad++;
	}
	printf("\n%d of %d rates wrong.\n", bad, NRATES);
	return bad ? 1 : 0;
}
