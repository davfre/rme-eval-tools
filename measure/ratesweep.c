// ratesweep - find out what actually selects the sample rate on a Babyface
// Pro in proprietary mode, from user space, without rebuilding the module.
//
// Extends tools/usbdump/ratetest.c (2026-08-22), which established the
// alt table in PROTOCOL.md.  Same method: claim interface 5, run the
// cold-init, pump the IN endpoint and measure its byte rate.  The byte
// rate divided by the frame size IS the sample rate, so every question
// below gets a number rather than an opinion.
//
//   sample_rate = bytes_per_second / frame_bytes
//   frame_bytes = 56 (14 ch), 40 (10 ch) or 32 (8 ch)
//
// Background: PR #7 fixed 44.1 kHz by writing a "rate family" to
// 0x10 wIdx=0x0030, and models the rate as family x alt-multiplier.
// On hardware 64, 128 and 176.4 kHz still come out wrong (48, 96, 192),
// and PROTOCOL.md's own captured table says the alt is a BANDWIDTH class
// (alt1 = 32/44.1/48/64/88.2, alt2 = 96/128, alt3 = 176.4/192), not a
// multiplier.  Both cannot be right.  See ../rate-analysis.md.
//
// Build:  gcc -O2 -Wall -o ratesweep ratesweep.c
// Run:    sudo ./ratesweep <mode>
//
// The tool detaches the kernel driver from interface 5 itself and puts it
// back on exit, so the module does not need unloading.  Stop anything
// actively playing first (PipeWire will see the card disappear), and
// KEEP MONITORS OFF: this arms the stream and changes the rate.
//
// Modes:
//   sweep     all 9 rates the way the driver does it now (reproduce the
//             maintainer's result from user space)
//   alts      the 3 broken rates using PROTOCOL.md's alt table instead
//   order     base-before-SET_INTERFACE vs after, on the 3 broken rates
//   clobber   does the 0x10 0x0000 0x3000 pair late in cold-init wipe the
//             base written earlier by bf_clock_write?
//   scanbase  write wValue 0x0000..0x0080 step 0x10 at each alt and see
//             which values move the clock (is it a 3x3 grid or a 9-step
//             index?)
//   idx       write the base to wIndex 0x0030 vs 0x3000
//   dds       the other model entirely: set the base rate by synthesising
//             the 0x1B DDS quad (PROTOCOL.md's decoded pitch formula) and
//             let the alt be the x1/x2/x4 multiplier
//   ddsscan   sweep the DDS and see what rate actually comes out - the
//             transfer function, instead of another guessed formula
//   combo     0x0030 base register x DDS quad (measured: inert)
//   table     all 9 rates as (DDS base x alt) under the measured model
//   bank3     hunt for whatever could halve the clock, for 32 kHz
//   pitch     varispeed at each base - does +5 %% really mean +5 %% at all
//             four bases, on the wire?
//   rearm     repeated sessions with and without the 0x13 disarm, to test
//             whether issue #5's silent restart is a missing disarm

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <linux/usbdevice_fs.h>

#define VID 0x2a39
#define PID 0x3fc0
#define NQ  9
#define IFACE 5
#define EP_IN  0x82
#define EP_OUT 0x01
#define MEAS_MS 2500

static int fd = -1;
static int detached;
static struct usbdevfs_urb *outs[NQ], *ins[NQ];

/* ---- plumbing ---------------------------------------------------- */

static void ctl(int req, int val, int idx)
{
	struct usbdevfs_ctrltransfer c;
	memset(&c, 0, sizeof(c));
	c.bRequestType = 0x40;
	c.bRequest = req;
	c.wValue = val;
	c.wIndex = idx;
	c.timeout = 1000;
	if (ioctl(fd, USBDEVFS_CONTROL, &c) < 0)
		fprintf(stderr, "  ! ctl(0x%02x,0x%04x,0x%04x): %s\n",
			req, val, idx, strerror(errno));
}

static int rd11(void)
{
	unsigned char b[4] = {0};
	struct usbdevfs_ctrltransfer c;
	memset(&c, 0, sizeof(c));
	c.bRequestType = 0xC0;
	c.bRequest = 0x11;
	c.wLength = 4;
	c.timeout = 1000;
	c.data = b;
	if (ioctl(fd, USBDEVFS_CONTROL, &c) != 4)
		return -1;
	return b[3] & 0x0F;	/* 2^alt + poll counter; see PROTOCOL.md */
}

static long long us(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return tv.tv_sec * 1000000LL + tv.tv_usec;
}

static int setif(int alt)
{
	struct usbdevfs_setinterface si = { IFACE, alt };
	return ioctl(fd, USBDEVFS_SETINTERFACE, &si);
}

/* URB buffer must be a whole number of the alt's packets, or the stream
 * stalls (PROTOCOL.md: 14336 is not a multiple of alt2's 640 B). */
static int urbsize_for_alt(int alt)
{
	switch (alt) {
	case 1: return 14336;	/* 56 B x 256 */
	case 2: return 10240;	/* 40 B x 256 */
	default: return 8192;	/* 32 B x 256 */
	}
}

static double measure(int ms, int sz)
{
	long long t0 = us(), bytes = 0;

	while (us() - t0 < (long long)ms * 1000) {
		struct usbdevfs_urb *done = NULL;

		errno = 0;
		if (ioctl(fd, USBDEVFS_REAPURBNDELAY, &done) == 0) {
			if (done->endpoint == EP_IN)
				bytes += done->actual_length;
			done->buffer_length = sz;
			ioctl(fd, USBDEVFS_SUBMITURB, done);
		}
	}
	return bytes / ((us() - t0) / 1e6);
}

/* Print the measured byte rate as a sample rate under each candidate
 * frame layout, and flag whichever one lands on the requested rate. */
/* The frame layout is fixed per alt, measured: alt1 = 56 B, alt2 = 40 B,
 * alt3 = 32 B (the 48 kHz base reads 48000/96000/192000 exactly on those).
 * So the alt gives the divisor; no need to guess among three. */
static int frame_for_alt(int alt)
{
	return alt == 1 ? 56 : alt == 2 ? 40 : 32;
}

static void report(const char *label, int want_hz, double bps, int alt,
		   int alt_rb)
{
	double hz = bps / frame_for_alt(alt);
	int hit = want_hz && hz > want_hz * 0.99 && hz < want_hz * 1.01;

	printf("  %-32s %7.1f kB/s -> %7.0f Hz (@%dB)  0x11=%X  %s\n",
	       label, bps / 1000.0, hz, frame_for_alt(alt), alt_rb,
	       want_hz ? (hit ? "OK" : "off") : "");
}

/* ---- the DDS clock quad ------------------------------------------ */

/* PROTOCOL.md, "Pitch / varispeed - the 0x1B quad is 24-bit fixed point"
 * (cap_pitch.pcap, full sweep decoded, and confirmed on Linux by
 * pitchformula.c): the 0x1B quad IS the device clock.
 *
 *   bank0 = DDS as 16.8 fixed point, wValue = integer part,
 *           wIndex high byte = fraction, wIndex low byte = bank
 *   bank1 = round(DDS_16 x 0.72562)
 *   bank2 = round(DDS_16 x 2/3)
 *   bank3 = 0x7CFF (fraction 0; the 0% value works across the range)
 *
 * At 48 kHz, DDS_24 = 50000.000 -> 0xC350 0x0000, byte-identical to the
 * quad the cold-init has always sent.
 *
 * MEASURED 2026-09-15: the DDS is a PERIOD, not a frequency - the pitch
 * formula divides by (1 + p/100), so a SMALLER DDS means a HIGHER rate:
 *
 *   DDS_24(R) = round(12800000 x 48000 / R)
 *
 * The first run had this the wrong way round and the clock still moved, by
 * the predicted amount in the opposite direction, which is what confirms
 * the DDS drives it. Note the hard limit: bank0's integer part is 16 bits,
 * so DDS_16 <= 65535 and the lowest reachable base is about 36.6 kHz.
 * 44.1 kHz fits (DDS_16 = 54421); 32 kHz does NOT, so 32/64/128 kHz cannot
 * come from this quad alone.
 *
 * Every quad must be followed by the 0x10 0x0001 0x05CF keepalive, or it
 * does not apply.
 */
/* bank3 is the one bank PROTOCOL.md could not derive: "QUANTIZED in steps
 * (sticks at values like 0x7C01 over DDS ranges) - bank3 needs a lookup
 * table from the sweep". 0x7CFF is its 0% value and works across the whole
 * range measured so far. If anything selects a coarse octave - which is
 * what 32 kHz would need, being out of bank0's reach - this is the
 * candidate. */
static unsigned int g_bank3 = 0x7CFF;
/* Varispeed, in 0.1 % steps, exactly as the driver's control takes it. */
static int g_pitch;
/* Send the 0x13 session disarm when tearing a session down?  The driver's
 * babyface_stream_kill() does not: it kills the URBs and clears a flag, and
 * 0x13 is never sent at all.  Every probe in tools/usbdump does send it.
 * Setting this to 0 reproduces what the driver does. */
static int g_disarm = 1;

static void dds_write(int base_hz)
{
	/* The driver's own expression:
	 *   DDS_24 = round(12800000000 * 48000 / (base * (1000 + pitch)))
	 */
	double den = (double)base_hz * (1000.0 + g_pitch);
	unsigned int dds24 = (unsigned int)(12800000000.0 * 48000.0 / den + 0.5);
	unsigned int d16 = dds24 >> 8, frac = dds24 & 0xFF;
	unsigned int b1 = (unsigned int)(d16 * 0.72562 + 0.5);
	unsigned int b2 = (unsigned int)(d16 * 2.0 / 3.0 + 0.5);

	ctl(0x1B, d16 & 0xFFFF, (frac << 8) | 0x00);
	ctl(0x1B, b1 & 0xFFFF, 0x0001);
	ctl(0x1B, b2 & 0xFFFF, 0x0002);
	ctl(0x1B, g_bank3 & 0xFFFF, 0x0003);
	ctl(0x10, 0x0001, 0x05CF);	/* commit, or the quad does not apply */
}

/* ---- session ----------------------------------------------------- */

/* The cold-start init, verbatim from babyfacepro.c's bf_cold_init().
 * `base` >= 0 writes the rate base at `base_idx` in bf_clock_write()'s
 * position; `base_late` repeats it after the 0x3000 pair instead. */
static void cold_init(int base, int base_idx, int base_late, int dds_hz)
{
	int i;

	for (i = 0; i <= 0x3D; i++) {
		if (i == 0x1E || i == 0x1F)
			continue;
		ctl(0x16, 0x0000, i);
	}
	if (dds_hz) {
		dds_write(dds_hz);
	} else {
		/* the verbatim 48 kHz coldplug quad the driver still sends */
		ctl(0x1B, 0xC350, 0x0000);
		ctl(0x1B, 0x8DB8, 0xD201);
		ctl(0x1B, 0x8234, 0xD302);
		ctl(0x1B, 0x7CFF, 0xF803);
	}

	if (base >= 0 && !base_late) {
		ctl(0x10, base, base_idx);	/* bf_clock_write */
		ctl(0x10, 0x0001, 0x05CF);	/* bf_settings_write */
	}

	ctl(0x1C, 0x0000, 0x0000);
	ctl(0x10, 0x0021, 0x05FF);
	ctl(0x17, 0x000C, 0x0000);
	ctl(0x21, 0x0000, 0x0000);
	ctl(0x10, 0x0000, 0x3000);	/* <- suspect: same register as 0x0030? */
	ctl(0x10, 0x0000, 0x3000);
	ctl(0x10, 0x0800, 0x0800);
	ctl(0x10, 0x0800, 0x0800);
	ctl(0x10, 0x0800, 0x0800);

	if (base >= 0 && base_late) {
		ctl(0x10, base, base_idx);
		ctl(0x10, 0x0001, 0x05CF);
	}
}

static void arm(int alt)
{
	int i, sz = urbsize_for_alt(alt);

	ctl(0x10, 0x0000, 0x8000);
	ctl(0x1D, 0x0000, 0x0000);
	for (i = 0; i < NQ; i++) {
		outs[i]->buffer_length = sz;
		ins[i]->buffer_length = sz;
		ioctl(fd, USBDEVFS_SUBMITURB, outs[i]);
		ioctl(fd, USBDEVFS_SUBMITURB, ins[i]);
	}
	ctl(0x14, 0x0000, 0xC000);
}

static void disarm(void)
{
	int i;

	if (g_disarm)
		ctl(0x13, 0x0000, 0xC000);
	for (i = 0; i < NQ; i++) {
		ioctl(fd, USBDEVFS_DISCARDURB, outs[i]);
		ioctl(fd, USBDEVFS_DISCARDURB, ins[i]);
	}
	for (i = 0; i < 2 * NQ; i++) {
		struct usbdevfs_urb *d = NULL;
		ioctl(fd, USBDEVFS_REAPURBNDELAY, &d);
	}
	usleep(200000);
}

/* One full measurement from a clean session. */
static void trial2(const char *label, int want_hz, int base, int base_idx,
		   int base_late, int alt, int base_first, int dds_hz)
{
	int rb;

	if (base_first) {
		cold_init(base, base_idx, base_late, dds_hz);
		setif(alt);
	} else {
		/* The driver's real order: hw_params does SET_INTERFACE, then
		 * stream_work runs bf_cold_init (which contains
		 * bf_clock_write), then arms. */
		setif(alt);
		cold_init(base, base_idx, base_late, dds_hz);
	}
	rb = rd11();
	arm(alt);
	report(label, want_hz, measure(MEAS_MS, urbsize_for_alt(alt)), alt, rb);
	disarm();
}

static void trial(const char *label, int want_hz, int base, int base_idx,
		  int base_late, int alt, int base_first)
{
	trial2(label, want_hz, base, base_idx, base_late, alt, base_first, 0);
}

/* ---- rate tables ------------------------------------------------- */

struct rate { int hz; int alt_pr; int alt_proto; int base; const char *name; };

/* alt_pr: the current driver's grouping.  alt_proto: PROTOCOL.md's
 * captured, hardware-verified grouping. */
static const struct rate rates[] = {
	{  32000, 1, 1, 0x0000, "32k"    },
	{  44100, 1, 1, 0x0010, "44.1k"  },
	{  48000, 1, 1, 0x0020, "48k"    },
	{  64000, 2, 1, 0x0000, "64k"    },
	{  88200, 2, 1, 0x0010, "88.2k"  },
	{  96000, 2, 2, 0x0020, "96k"    },
	{ 128000, 3, 2, 0x0000, "128k"   },
	{ 176400, 3, 3, 0x0010, "176.4k" },
	{ 192000, 3, 3, 0x0020, "192k"   },
};
#define NRATES ((int)(sizeof(rates) / sizeof(rates[0])))
/* 64k, 128k, 176.4k */
static const int broken[3] = { 3, 6, 7 };

/* ---- modes ------------------------------------------------------- */

static void mode_sweep(void)
{
	int i;

	puts("\n== sweep: all 9 rates in the driver's own order ==");
	puts("   (SET_INTERFACE first, then cold-init with the base write)");
	for (i = 0; i < NRATES; i++) {
		char l[64];
		snprintf(l, sizeof(l), "%s base=0x%04X alt%d",
			 rates[i].name, rates[i].base, rates[i].alt_pr);
		trial(l, rates[i].hz, rates[i].base, 0x0030, 0,
		      rates[i].alt_pr, 0);
	}
}

static void mode_alts(void)
{
	int k;

	puts("\n== alts: the 3 broken rates with PROTOCOL.md's alt table ==");
	for (k = 0; k < 3; k++) {
		const struct rate *r = &rates[broken[k]];
		char l[64];

		snprintf(l, sizeof(l), "%s base=0x%04X alt%d (proto)",
			 r->name, r->base, r->alt_proto);
		trial(l, r->hz, r->base, 0x0030, 0, r->alt_proto, 1);
	}
}

static void mode_order(void)
{
	int k;

	puts("\n== order: does the base have to precede SET_INTERFACE? ==");
	for (k = 0; k < 3; k++) {
		const struct rate *r = &rates[broken[k]];
		char l[64];

		snprintf(l, sizeof(l), "%s base first", r->name);
		trial(l, r->hz, r->base, 0x0030, 0, r->alt_pr, 1);
		snprintf(l, sizeof(l), "%s alt first", r->name);
		trial(l, r->hz, r->base, 0x0030, 0, r->alt_pr, 0);
	}
}

static void mode_clobber(void)
{
	int k;

	puts("\n== clobber: is the base wiped by 0x10 0x0000 0x3000? ==");
	for (k = 0; k < 3; k++) {
		const struct rate *r = &rates[broken[k]];
		char l[64];

		snprintf(l, sizeof(l), "%s base early (as driver)", r->name);
		trial(l, r->hz, r->base, 0x0030, 0, r->alt_pr, 1);
		snprintf(l, sizeof(l), "%s base after the 0x3000 pair", r->name);
		trial(l, r->hz, r->base, 0x0030, 1, r->alt_pr, 1);
	}
}

static void mode_scanbase(void)
{
	int alt, v;

	puts("\n== scanbase: which wValues at 0x0030 move the clock? ==");
	for (alt = 1; alt <= 3; alt++) {
		for (v = 0x0000; v <= 0x0080; v += 0x0010) {
			char l[64];
			snprintf(l, sizeof(l), "alt%d base=0x%04X", alt, v);
			trial(l, 0, v, 0x0030, 0, alt, 1);
		}
	}
}

/* The alternative model: the DDS quad is the base and the alt is the
 * x1/x2/x4 multiplier. If this is right, all nine cells hit. */
static void mode_dds(void)
{
	static const int bases[2] = { 44100, 48000 };
	int b, alt;

	puts("\n== dds: base rate from the 0x1B quad, alt as the multiplier ==");
	puts("   (32 kHz is out of range for bank0's 16-bit integer part -");
	puts("    try mode 'combo' for where 32/64/128 might come from)");
	for (b = 0; b < 2; b++) {
		for (alt = 1; alt <= 3; alt++) {
			char l[64];
			int want = bases[b] << (alt - 1);

			snprintf(l, sizeof(l), "dds=%d alt%d -> want %d",
				 bases[b], alt, want);
			trial2(l, want, -1, 0x0030, 0, alt, 0, bases[b]);
		}
	}
}

/* Does varispeed move the clock the same way at every base?  Through ALSA
 * the base-64000 rates came back with the pitch INVERTED (+5 % asked, -5 %
 * delivered) while every other base was correct.  This writes the exact
 * quads the driver would write, straight to the device, so the device and
 * the driver can be told apart. */
static void mode_pitch(void)
{
	static const int bases[3] = { 32000, 44100, 48000 };
	static const int pitches[3] = { -50, 0, 50 };
	int b, i, save = g_pitch;

	puts("\n== pitch: varispeed at each base, alt1 ==");
	for (b = 0; b < 3; b++) {
		for (i = 0; i < 3; i++) {
			char l[64];
			int want;

			g_pitch = pitches[i];
			/* rate scales WITH (1000 + pitch) */
			want = (int)((double)bases[b] * (1000 + pitches[i]) / 1000.0 + 0.5);
			snprintf(l, sizeof(l), "base %d pitch %+d.%d%% -> %d",
				 bases[b], pitches[i] / 10, abs(pitches[i]) % 10,
				 want);
			trial2(l, want, -1, 0x0030, 0, 1, 0, bases[b]);
		}
	}
	g_pitch = save;
}

/* Arm the same session repeatedly, with and without the 0x13 disarm in
 * between.  Issue #5 reports that a runtime buffer change - which makes the
 * driver stop and restart the session - leaves the stream "started" but
 * silent until the module is reloaded.  The driver never sends 0x13.  If the
 * firmware needs the old session closed before it will honour a new arm, the
 * no-disarm column here goes quiet after the first row and the disarm column
 * does not. */
static void mode_rearm(void)
{
	int pass, i;

	puts("\n== rearm: repeated sessions, with and without the 0x13 disarm ==");
	for (pass = 0; pass < 2; pass++) {
		g_disarm = pass == 0;
		printf("\n  disarm between sessions: %s\n",
		       g_disarm ? "yes (what the probes do)"
				: "NO  (what the driver does)");
		for (i = 0; i < 4; i++) {
			char l[64];

			snprintf(l, sizeof(l), "session %d", i + 1);
			trial2(l, 48000, -1, 0x0030, 0, 1, 0, 48000);
		}
	}
	g_disarm = 1;
}

/* The whole rate table under the measured model:
 *   rate = (48000 x 50000 / DDS_16) x 2^(alt-1)
 * Three bases cover eight of the nine rates. 32 kHz needs DDS_16 = 75000,
 * which does not fit bank0's 16-bit wValue, so it is expected to fail here
 * and is included to show what it does instead. */
static void mode_table(void)
{
	/* A clean 3 x 3: three bases, three speeds.  64 kHz is base 32000 at
	 * x2, NOT base 64000 at x1 - both give 64 kHz at pitch 0, but base
	 * 64000 on alt 1 sits exactly on the 448-byte packet ceiling
	 * (64000 x 56 B = 3584 kB/s), so any positive varispeed runs off the
	 * end of the bus.  Same for 128 kHz on alt 2.  At x2/x3 the frame is
	 * smaller and there is room to spare. */
	static const struct { int hz, base, alt; } t[] = {
		{  32000, 32000, 1 },
		{  44100, 44100, 1 },
		{  48000, 48000, 1 },
		{  64000, 32000, 2 },
		{  88200, 44100, 2 },
		{  96000, 48000, 2 },
		{ 128000, 32000, 3 },
		{ 176400, 44100, 3 },
		{ 192000, 48000, 3 },
	};
	int i;

	puts("\n== table: all 9 rates as (DDS base x alt) ==");
	for (i = 0; i < (int)(sizeof(t) / sizeof(t[0])); i++) {
		char l[64];

		snprintf(l, sizeof(l), "%d = base %d x alt%d",
			 t[i].hz, t[i].base, t[i].alt);
		trial2(l, t[i].hz, -1, 0x0030, 0, t[i].alt, 0, t[i].base);
	}
}

/* Hunt for 32 kHz. bank0 cannot reach it, so if the device can do 32 kHz at
 * all, something coarse must halve it. Start from the 64 kHz DDS and sweep
 * bank3 looking for a rate that drops by a factor of two. */
static void mode_bank3(void)
{
	static const unsigned int probes[] = {
		0x7CFF, 0x3E7F, 0xF9FE, 0x7C01, 0x0000, 0xFFFF,
	};
	unsigned int save = g_bank3;
	int i;

	puts("\n== bank3: can anything halve the clock?  DDS base 64000, alt1 ==");
	puts("   (looking for ~32000 Hz instead of ~64000)");
	for (i = 0; i < (int)(sizeof(probes) / sizeof(probes[0])); i++) {
		char l[64];

		g_bank3 = probes[i];
		snprintf(l, sizeof(l), "bank3=0x%04X", probes[i]);
		trial2(l, 0, -1, 0x0030, 0, 1, 0, 64000);
	}
	puts("   high-byte scan:");
	for (i = 0; i < 16; i++) {
		char l[64];

		g_bank3 = (i << 12) | 0x0FF;
		snprintf(l, sizeof(l), "bank3=0x%04X", g_bank3);
		trial2(l, 0, -1, 0x0030, 0, 1, 0, 64000);
	}
	g_bank3 = save;
}

/* Map the DDS -> rate transfer function directly rather than guessing at
 * another formula: sweep bank0 and see what comes out, including where it
 * stops following (the pitch sweep only validated +-5%, and bank3 is
 * quantised outside that). */
static void mode_ddsscan(void)
{
	int i;

	puts("\n== ddsscan: measured rate vs DDS, alt1 ==");
	for (i = 0; i < 14; i++) {
		unsigned int d16 = 40000 + i * 2000;
		char l[64];
		int predicted;

		if (d16 > 65535)
			break;
		predicted = (int)(48000.0 * 50000.0 / d16 + 0.5);
		snprintf(l, sizeof(l), "DDS_16=%u pred %d Hz", d16, predicted);
		trial2(l, predicted, -1, 0x0030, 0, 1, 0, predicted);
	}
}

/* Does the 0x0030 base register do anything once a real DDS quad is in
 * play?  If 0x0000 is a coarse pre-divider, that is where 32/64/128 kHz
 * would come from. */
static void mode_combo(void)
{
	static const int bases[3] = { 0x0000, 0x0010, 0x0020 };
	static const int dds[2] = { 44100, 48000 };
	int b, d;

	puts("\n== combo: 0x0030 base x DDS quad, alt1 ==");
	for (d = 0; d < 2; d++) {
		for (b = 0; b < 3; b++) {
			char l[64];

			snprintf(l, sizeof(l), "base=0x%04X dds=%d",
				 bases[b], dds[d]);
			trial2(l, 0, bases[b], 0x0030, 0, 1, 0, dds[d]);
		}
	}
}

static void mode_idx(void)
{
	int k;

	puts("\n== idx: base at wIndex 0x0030 vs 0x3000 ==");
	for (k = 0; k < 3; k++) {
		const struct rate *r = &rates[broken[k]];
		char l[64];

		snprintf(l, sizeof(l), "%s idx=0x0030", r->name);
		trial(l, r->hz, r->base, 0x0030, 0, r->alt_pr, 1);
		snprintf(l, sizeof(l), "%s idx=0x3000", r->name);
		trial(l, r->hz, r->base, 0x3000, 0, r->alt_pr, 1);
	}
}

/* ---- device discovery -------------------------------------------- */

static int open_device(const char *forced)
{
	char path[512], dirp[288];
	DIR *bd, *dd;
	struct dirent *b, *d;

	if (forced)
		return open(forced, O_RDWR);

	bd = opendir("/dev/bus/usb");
	if (!bd)
		return -1;
	while ((b = readdir(bd))) {
		if (b->d_name[0] == '.')
			continue;
		snprintf(dirp, sizeof(dirp), "/dev/bus/usb/%.255s", b->d_name);
		dd = opendir(dirp);
		if (!dd)
			continue;
		while ((d = readdir(dd))) {
			unsigned char desc[18];
			int f;

			if (d->d_name[0] == '.')
				continue;
			snprintf(path, sizeof(path), "%.287s/%.200s",
				 dirp, d->d_name);
			f = open(path, O_RDWR);
			if (f < 0)
				continue;
			if (read(f, desc, sizeof(desc)) == sizeof(desc) &&
			    (desc[8] | desc[9] << 8) == VID &&
			    (desc[10] | desc[11] << 8) == PID) {
				printf("device: %s\n", path);
				closedir(dd);
				closedir(bd);
				return f;
			}
			close(f);
		}
		closedir(dd);
	}
	closedir(bd);
	return -1;
}

int main(int argc, char **argv)
{
	const char *mode = argc > 1 ? argv[1] : "sweep";
	const char *dev = argc > 2 ? argv[2] : NULL;
	int i, iface = IFACE;

	fd = open_device(dev);
	if (fd < 0) {
		fprintf(stderr,
			"no Babyface at %04x:%04x (or permission denied).\n"
			"Run as root.\n", VID, PID);
		return 1;
	}
	/* Take interface 5 away from whatever kernel driver holds it, in one
	 * step, rather than asking the user to unload a module that PipeWire
	 * or a running TuxMix is keeping busy.  This is what libusb's
	 * detach_kernel_driver does.  The driver is re-attached on exit.
	 */
	{
		struct usbdevfs_disconnect_claim dc;

		memset(&dc, 0, sizeof(dc));
		dc.interface = IFACE;
		dc.flags = 0;		/* detach whatever is bound */
		if (ioctl(fd, USBDEVFS_DISCONNECT_CLAIM, &dc) == 0) {
			detached = 1;
			printf("detached the kernel driver from interface %d\n",
			       IFACE);
		} else if (ioctl(fd, USBDEVFS_CLAIMINTERFACE, &iface) < 0) {
			fprintf(stderr,
				"claim interface %d: %s\n\n"
				"Something still holds the card. Find it with:\n"
				"  sudo fuser -v /dev/snd/*\n"
				"  lsof /dev/snd/* 2>/dev/null\n"
				"Usually PipeWire or a running TuxMix. Then either close it,\n"
				"or suspend the card:  pactl suspend-card <card> 1\n",
				IFACE, strerror(errno));
			return 1;
		}
	}

	for (i = 0; i < NQ; i++) {
		outs[i] = calloc(1, sizeof(*outs[i]));
		outs[i]->type = USBDEVFS_URB_TYPE_INTERRUPT;
		outs[i]->endpoint = EP_OUT;
		outs[i]->buffer = calloc(1, 20480);
		ins[i] = calloc(1, sizeof(*ins[i]));
		ins[i]->type = USBDEVFS_URB_TYPE_INTERRUPT;
		ins[i]->endpoint = EP_IN;
		ins[i]->buffer = calloc(1, 20480);
	}

	printf("\nMonitors off?  This arms the stream and changes the rate.\n");
	printf("Each line: measured IN byte rate -> the sample rate it implies\n");
	printf("under each frame layout.  OK = one of them is the rate asked for.\n");

	if (!strcmp(mode, "sweep"))         mode_sweep();
	else if (!strcmp(mode, "alts"))     mode_alts();
	else if (!strcmp(mode, "order"))    mode_order();
	else if (!strcmp(mode, "clobber"))  mode_clobber();
	else if (!strcmp(mode, "scanbase")) mode_scanbase();
	else if (!strcmp(mode, "dds"))      mode_dds();
	else if (!strcmp(mode, "ddsscan"))  mode_ddsscan();
	else if (!strcmp(mode, "combo"))    mode_combo();
	else if (!strcmp(mode, "table"))    mode_table();
	else if (!strcmp(mode, "bank3"))    mode_bank3();
	else if (!strcmp(mode, "pitch"))    mode_pitch();
	else if (!strcmp(mode, "rearm"))    mode_rearm();
	else if (!strcmp(mode, "idx"))      mode_idx();
	else {
		fprintf(stderr, "unknown mode: %s\n"
			"modes: sweep alts order clobber scanbase idx dds "
			"ddsscan combo table bank3 pitch rearm\n", mode);
		return 1;
	}

	setif(1);
	if (detached) {
		/* Hand interface 5 back so the card returns without a replug.
		 * USBDEVFS_CONNECT is a sub-ioctl of USBDEVFS_IOCTL; it wants
		 * the interface released first on some kernels and still
		 * claimed on others, so try both before giving up. */
		struct usbdevfs_ioctl cmd;
		int ok;

		memset(&cmd, 0, sizeof(cmd));
		cmd.ifno = IFACE;
		cmd.ioctl_code = USBDEVFS_CONNECT;
		ioctl(fd, USBDEVFS_RELEASEINTERFACE, &iface);
		usleep(300000);		/* let the URBs finish retiring */
		ok = ioctl(fd, USBDEVFS_IOCTL, &cmd) == 0;
		if (!ok) {
			int e1 = errno;

			usleep(500000);
			ok = ioctl(fd, USBDEVFS_IOCTL, &cmd) == 0;
			if (!ok)
				printf("could not re-attach (%s / %s)\n",
				       strerror(e1), strerror(errno));
		}
		if (ok)
			puts("re-attached the kernel driver");
		else
			puts("get the card back with:\n"
			     "  sudo modprobe -r snd_usb_babyface_pro && "
			     "sudo modprobe snd_usb_babyface_pro");
	} else {
		ioctl(fd, USBDEVFS_RELEASEINTERFACE, &iface);
	}
	close(fd);
	puts("\ndone.");
	return 0;
}
