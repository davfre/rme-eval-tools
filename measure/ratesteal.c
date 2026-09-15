// ratesteal - reproduce the clock steal on demand.
//
// Holds a capture stream at one rate and measures it continuously.  Part way
// through, a second process opens PLAYBACK on the same card at a different
// rate.  Both directions share one clock, so on a driver that retunes on
// request the capture rate visibly steps mid-run, even though nothing touched
// the capture stream.
//
// Build:  gcc -O2 -Wall -o ratesteal ratesteal.c -lasound
// Run:    ./ratesteal [capture_rate] [intruder_rate] [device]
//         ./ratesteal 96000 48000
//
// Free the card first, or PipeWire is a third party in the experiment:
//   pactl list cards short
//   pactl set-card-profile <n> off        # ... and back afterwards
//
// Expected, driver that retunes (current behaviour):
//   the measured capture rate steps to the intruder's rate and stays there.
// Expected, driver that locks the rate while streaming:
//   the intruder is refused or constrained to the running rate, and the
//   measured capture rate never moves.

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
#include <math.h>
#include <alsa/asoundlib.h>

#define CHANS    2
#define BUCKET_S 0.40		/* one measurement row */
#define ROWS     16
#define ROW_INTRUDE 5		/* the second client arrives before this row */

static double now_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* The second client: open playback at `rate` and push silence for a while.
 * Runs in a child so the capture stream in the parent stays untouched. */
static void intruder(const char *dev, unsigned int rate)
{
	snd_pcm_t *pcm = NULL;
	snd_pcm_hw_params_t *hw;
	snd_pcm_uframes_t period = 0;
	unsigned int ch = CHANS, got = rate;
	int err;
	int32_t *buf;
	double t0;

	err = snd_pcm_open(&pcm, dev, SND_PCM_STREAM_PLAYBACK, 0);
	if (err < 0) {
		fprintf(stderr, "  [intruder] open: %s\n", snd_strerror(err));
		return;
	}
	snd_pcm_hw_params_alloca(&hw);
	snd_pcm_hw_params_any(pcm, hw);
	snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
	snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S32_LE);

	/* Ask for the rate exactly.  A driver that locks the rate while
	 * streaming should refuse here, or the constraint should already have
	 * narrowed the list so this rate is not on offer. */
	err = snd_pcm_hw_params_set_rate(pcm, hw, rate, 0);
	if (err < 0) {
		fprintf(stderr, "  [intruder] %u Hz refused at hw_params: %s"
			"   <-- the rate is locked, good\n",
			rate, snd_strerror(err));
		snd_pcm_close(pcm);
		return;
	}
	snd_pcm_hw_params_set_channels_near(pcm, hw, &ch);
	err = snd_pcm_hw_params(pcm, hw);
	if (err < 0) {
		fprintf(stderr, "  [intruder] hw_params: %s   <-- refused, good\n",
			snd_strerror(err));
		snd_pcm_close(pcm);
		return;
	}
	snd_pcm_hw_params_get_rate(hw, &got, NULL);
	snd_pcm_hw_params_get_period_size(hw, &period, NULL);
	fprintf(stderr, "  [intruder] playback open at %u Hz (asked %u)"
		"   <-- accepted\n", got, rate);

	buf = calloc(period * ch, sizeof(*buf));
	if (!buf) {
		snd_pcm_close(pcm);
		return;
	}
	snd_pcm_prepare(pcm);
	t0 = now_s();
	while (now_s() - t0 < 4.0) {
		snd_pcm_sframes_t n = snd_pcm_writei(pcm, buf, period);

		if (n < 0 && snd_pcm_recover(pcm, (int)n, 1) < 0)
			break;
	}
	free(buf);
	snd_pcm_close(pcm);
	fprintf(stderr, "  [intruder] gone\n");
}

int main(int argc, char **argv)
{
	unsigned int cap_rate = argc > 1 ? (unsigned int)atoi(argv[1]) : 96000;
	unsigned int int_rate = argc > 2 ? (unsigned int)atoi(argv[2]) : 48000;
	const char *dev = argc > 3 ? argv[3] : "hw:4";
	snd_pcm_t *pcm = NULL;
	snd_pcm_hw_params_t *hw;
	snd_pcm_uframes_t period = 0;
	unsigned int ch = CHANS;
	int err, row;
	int32_t *buf;
	pid_t kid = -1;
	double first = 0, worst = 0;

	printf("capture %u Hz on %s, second client will ask for %u Hz\n\n",
	       cap_rate, dev, int_rate);

	err = snd_pcm_open(&pcm, dev, SND_PCM_STREAM_CAPTURE, 0);
	if (err < 0) {
		fprintf(stderr, "capture open: %s\n"
			"Free the card first: pactl set-card-profile <n> off\n",
			snd_strerror(err));
		return 1;
	}
	snd_pcm_hw_params_alloca(&hw);
	snd_pcm_hw_params_any(pcm, hw);
	snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
	snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S32_LE);
	err = snd_pcm_hw_params_set_rate(pcm, hw, cap_rate, 0);
	if (err < 0) {
		fprintf(stderr, "capture rate %u: %s\n", cap_rate,
			snd_strerror(err));
		return 1;
	}
	snd_pcm_hw_params_set_channels_near(pcm, hw, &ch);
	err = snd_pcm_hw_params(pcm, hw);
	if (err < 0) {
		fprintf(stderr, "capture hw_params: %s\n", snd_strerror(err));
		return 1;
	}
	snd_pcm_hw_params_get_period_size(hw, &period, NULL);
	buf = malloc(period * ch * sizeof(*buf));
	if (!buf)
		return 1;
	snd_pcm_prepare(pcm);
	snd_pcm_start(pcm);

	/* settle */
	for (double t0 = now_s(); now_s() - t0 < 0.5; )
		if (snd_pcm_readi(pcm, buf, period) < 0)
			snd_pcm_recover(pcm, -EPIPE, 1);

	printf("  row   measured Hz   note\n");
	for (row = 0; row < ROWS; row++) {
		double t0, t1;
		unsigned long long frames = 0;
		const char *note = "";

		if (row == ROW_INTRUDE) {
			kid = fork();
			if (kid == 0) {
				intruder(dev, int_rate);
				_exit(0);
			}
			note = "<-- second client opens here";
		}

		/* both ends of the window on read completions */
		while (snd_pcm_readi(pcm, buf, period) < 0)
			if (snd_pcm_recover(pcm, -EPIPE, 1) < 0)
				break;
		t0 = now_s();
		t1 = t0;
		while (t1 - t0 < BUCKET_S) {
			snd_pcm_sframes_t n = snd_pcm_readi(pcm, buf, period);

			if (n < 0) {
				if (snd_pcm_recover(pcm, (int)n, 1) < 0)
					break;
				note = "xrun";
				continue;
			}
			frames += (unsigned long long)n;
			t1 = now_s();
		}
		if (t1 > t0) {
			double hz = frames / (t1 - t0);

			if (!first) {
				first = hz;
				worst = hz;
			}
			if (fabs(hz - first) > fabs(worst - first))
				worst = hz;
			printf("  %3d   %9.0f     %s\n", row, hz, note);
			fflush(stdout);
		}
	}

	if (kid > 0)
		waitpid(kid, NULL, 0);
	free(buf);
	snd_pcm_close(pcm);

	printf("\nfirst row %.0f Hz, furthest row %.0f Hz (%+.2f %%)\n",
	       first, worst, first ? 100.0 * (worst - first) / first : 0.0);
	if (first && fabs(worst - first) > first * 0.01) {
		puts("The capture clock moved while a second client opened "
		     "playback.");
		puts("Nothing touched the capture stream. That is the steal.");
	} else {
		puts("The capture clock held. Either the rate is locked, or the "
		     "second client");
		puts("was refused - see its line above.");
	}
	return 0;
}
