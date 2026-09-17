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
//   bank      which of the four banks does the device actually read?
//             Send quads where one bank belongs to one base and the other
//             three to another, and see which base the clock follows
//   quads     print the quads the three models produce at each base,
//             no hardware touched (for comparing against captures)
//   family    does the FIRMWARE derive the clock from the 0x0030 family
//             register when no quad is sent?  Leave the device at 48 kHz
//             with a quad, then a session that writes family 0x0010 and
//             NO quad.  44.1 kHz means the device does the arithmetic and
//             the quad is only for pitch; 48 kHz means the host must send
//             the quad and RME's driver does so at stream start (the
//             2026-09-14 Windows captures were idle and saw no quad).
//   famrb     does a 0x0030 write register at all from Linux?  Polls the
//             0x11 readback after each family write, with three
//             keepalive variants (none / 0x0001 / Windows' 0x0441).
//             Measured: yes, at once, no keepalive needed
//   famclob   which write in the cold-init tail resets the family
//             register back to 48k?  Measured: 0x10 0x0021 0x05FF
//   family9   all nine rates by family + alt, no quad, RME's exact
//             rate-change sequence.  Does the family path cover
//             64/128/176.4, which fell to 48/96/192 on PR #7 v1?
//   f5ff      is 0x05FF (= 0x05CF | 0x0030) the settings word and the
//             family register in one?  Writes bits 4-5 and reads 0x11
//   pitchfam  varispeed on top of family 44.1 kHz: is the quad an
//             absolute clock or a pitch relative to the family?
//   ppm       long timed run to see which quad lands on nominal:
//               ratesweep ppm <q16|exact|driver|family> [seconds] [base]
//             `family` uses the family register and no quad, to see
//             how exact the firmware's own derivation is
//             default 600 s at 44100.  Times between IN URB completions
//             so the URB granularity does not enter the result; needs
//             the host clock NTP-disciplined (chronyc tracking)

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
#include <time.h>
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
	/* (multiplier << 2) | family, multiplier 0/1/2 = x1/x2/x4, family
	 * 0/1/2 = 32k/44.1k/48k - decoded from the 2026-09-14 Windows rate
	 * captures, where it followed every 0x0030 write.  It echoes the
	 * family REGISTER, not the clock: in `bank` it read 2 with the
	 * clock at 44.1 kHz.  PROTOCOL.md's "2^alt + counter" is wrong. */
	return b[3] & 0x0F;
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

/* ---- explicit quads ---------------------------------------------- */

/* RME's own arithmetic, recovered from the coldplug capture (2026-09-16).
 * Bank 0 is the exact period; banks 1-3 are bank 0 scaled by a fixed Q16
 * constant and truncated.  With
 *
 *   K = floor(65536 x 32000 / F),  F = 32000, 44100, 48000, 50000
 *     = 0x10000, 0xB9C2, 0xAAAA, 0xA3D7
 *
 * this reproduces the captured 48 kHz quad byte for byte, fraction bytes
 * included.  0xAAAA is 2/3 in Q16: these are hand-written constants, not
 * the residue of a division.  The four banks are one period in four
 * references, which is why bank 2 at the 32 kHz base (49999) is the
 * 48 kHz bank 0, and bank 1 (54421) the 44.1 kHz bank 0.
 *
 * Which of the four the device reads is what mode `bank` measures. */
static const unsigned long long q16_k[4] = { 65536, 47554, 43690, 41943 };

static void quad_q16(int base_hz, int pitch, unsigned int out[4])
{
	double den = (double)base_hz * (1000.0 + pitch);
	unsigned long long b0 =
		(unsigned long long)(12800000000.0 * 48000.0 / den + 0.5);
	int k;

	out[0] = (unsigned int)b0;
	for (k = 1; k < 4; k++)
		out[k] = (unsigned int)((b0 * q16_k[k]) >> 16);
}

/* Every bank exact, rounded to the nearest 16.8 (the "compute exactly"
 * proposal).  Differs from Q16 by up to 130 in the fraction byte. */
static void quad_exact(int base_hz, int pitch, unsigned int out[4])
{
	static const double F[4] = { 32000, 44100, 48000, 50000 };
	int k;

	for (k = 0; k < 4; k++) {
		double den = (double)base_hz * (1000.0 + pitch) * F[k];
		out[k] = (unsigned int)(12800000000.0 * 48000.0 * 32000.0 / den
					+ 0.5);
	}
}

/* What dds_write() above sends today: integer-only banks 1/2 from fitted
 * constants, bank 3 pinned at 0x7CFF.  Kept so the three can be printed
 * side by side. */
static void quad_driver(int base_hz, int pitch, unsigned int out[4])
{
	double den = (double)base_hz * (1000.0 + pitch);
	unsigned int dds24 = (unsigned int)(12800000000.0 * 48000.0 / den + 0.5);
	unsigned int d16 = dds24 >> 8;

	out[0] = dds24;
	out[1] = (unsigned int)(d16 * 0.72562 + 0.5) << 8;
	out[2] = (unsigned int)(d16 * 2.0 / 3.0 + 0.5) << 8;
	out[3] = 0x7CFF << 8;
}

/* Send four explicit 24-bit bank values.  wValue is 16 bits, so at the
 * 32 kHz base bank 0's integer part (75000) goes out truncated to its low
 * word, the same as the driver, and as RME's software must. */
static void quad_write(const unsigned int b[4])
{
	int k;

	for (k = 0; k < 4; k++)
		ctl(0x1B, (b[k] >> 8) & 0xFFFF, ((b[k] & 0xFF) << 8) | k);
	ctl(0x10, 0x0001, 0x05CF);	/* commit, or the quad does not apply */
}

/* If set, cold_init() sends this quad instead of synthesising one. */
static const unsigned int *g_quad;
/* If set, cold_init() sends NO quad at all (mode `family`). */
static int g_noquad;
/* If set, trial2() skips cold_init() entirely: the session sends only
 * what RME's driver sends on a rate change (family + 0x0441 keepalive),
 * then SET_INTERFACE and the arm.  Needs a previous session to have
 * initialised the device. */
static int g_minimal;
static int g_minimal_family;

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
	if (g_noquad) {
		/* nothing: is the family register alone enough? */
	} else if (g_quad) {
		quad_write(g_quad);
	} else if (dds_hz) {
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

	if (g_minimal) {
		/* What RME's driver sends on a rate change (2026-09-14
		 * Windows capture, idle): the family, the 0x0441 keepalive,
		 * SET_INTERFACE if the multiplier changed.  Then we arm. */
		ctl(0x10, g_minimal_family, 0x0030);
		ctl(0x10, 0x0441, 0x05CF);
		setif(alt);
		usleep(50000);
	} else if (base_first) {
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

/* Which bank does the device read?  The four banks carry the same period
 * in four references, so a consistent quad cannot tell.  Send a quad in
 * which three banks belong to base B and one belongs to base A, and see
 * which base the clock follows.  Four trials per direction, plus the two
 * all-same controls.
 *
 * A = 48000 and B = 44100 are 8.8 % apart, far outside the measurement's
 * 0.1 %, and both are real bases with every bank in range.
 *
 * Outcomes:
 *   exactly one bank pulls the clock to A   -> that is the register; the
 *                                              other three are don't-care
 *   none does alone                         -> the device wants agreement
 *                                              (or reads several)
 *   the rate is neither A nor B             -> print it; it is data */
static void mode_bank(void)
{
	static const int A = 48000, B = 44100;
	unsigned int qa[4], qb[4], q[4];
	int k, dir;

	quad_q16(A, 0, qa);
	quad_q16(B, 0, qb);

	puts("\n== bank: which bank does the device read?  alt1 ==");
	printf("   A = %d: %06X %06X %06X %06X\n", A, qa[0], qa[1], qa[2], qa[3]);
	printf("   B = %d: %06X %06X %06X %06X\n", B, qb[0], qb[1], qb[2], qb[3]);

	g_quad = qa;
	trial2("control: all four from A", A, -1, 0x0030, 0, 1, 0, 0);
	g_quad = qb;
	trial2("control: all four from B", B, -1, 0x0030, 0, 1, 0, 0);

	for (dir = 0; dir < 2; dir++) {
		const unsigned int *one = dir ? qb : qa;
		const unsigned int *rest = dir ? qa : qb;
		int want = dir ? B : A;

		for (k = 0; k < 4; k++) {
			char l[64];

			memcpy(q, rest, sizeof(q));
			q[k] = one[k];
			snprintf(l, sizeof(l), "bank%d from %c, rest from %c",
				 k, dir ? 'B' : 'A', dir ? 'A' : 'B');
			g_quad = q;
			trial2(l, want, -1, 0x0030, 0, 1, 0, 0);
		}
	}
	g_quad = NULL;
}

/* Does the firmware derive the DDS word from the 0x0030 family register
 * on its own?  The 2026-09-14 Windows captures (idle) show RME's driver
 * writing ONLY the family register on a rate change, never a quad, and
 * the 0x11 status byte echoing it.  On Linux, `combo` found the family
 * register inert - but every `combo` session also sent a quad afterwards,
 * which would override anything the firmware derived.  This sends the
 * family with no quad at all.
 *
 * Sequence: a 48 kHz session with a quad (known state), then family
 * 0x0010 with no quad, then family 0x0000 with no quad, then back.  If
 * the rate follows the family, the device does the arithmetic and the
 * quad is only for pitch.  If it stays at 48 kHz, the host must send the
 * quad and RME's driver does so at stream start. */
static void mode_family(void)
{
	static const struct { int fam; int hz; const char *name; } t[] = {
		{ 0x0010, 44100, "44.1k" },
		{ 0x0000, 32000, "32k" },
		{ 0x0020, 48000, "48k" },
		{ 0x0010, 44100, "44.1k" },
	};
	int i;

	puts("\n== family: what RME sends on a rate change, then a stream start ==");
	puts("   reference session: full cold-init + 48 kHz quad.  Then sessions");
	puts("   that send ONLY family + 0x0441 keepalive + SET_INTERFACE + arm:");
	puts("   no cold-init, no quad, none of the tail our driver replays.");
	puts("   (famrb: the family write registers at once.  The first run of");
	puts("    this mode replayed the cold-init tail after it and 0x11 read 2");
	puts("    on every row - our own sequence clobbered the register.)");
	g_noquad = 0;
	g_minimal = 0;
	trial2("reference: 48 kHz quad, family 0x0020", 48000, 0x0020, 0x0030,
	       0, 1, 0, 48000);
	g_minimal = 1;
	for (i = 0; i < (int)(sizeof(t) / sizeof(t[0])); i++) {
		char l[64];

		g_minimal_family = t[i].fam;
		snprintf(l, sizeof(l), "family 0x%04X + 0x0441 only, want %s",
			 t[i].fam, t[i].name);
		trial2(l, t[i].hz, -1, 0x0030, 0, 1, 0, 0);
	}
	g_minimal = 0;
	puts("   0x11 must read 1 / 0 / 2 / 1 down the rows.  Then the Hz column:");
	puts("   follows the family = firmware derives the clock, quad is pitch only;");
	puts("   stays 47923      = the quad is the clock and RME sends it at stream start.");
}

/* Does a 0x0030 family write REGISTER from Linux?  On Windows the 0x11
 * status byte followed every family write within 30 ms.  In `family` it
 * read 2 on every row while the family went 0x0010 / 0x0000 / 0x0020, so
 * either ratesweep reads it too soon, or the write is not taking.  This
 * writes the family and polls 0x11 for a second, with three variants of
 * the keepalive that follows: none, the driver's 0x0001, and Windows'
 * 0x0441.  No stream is armed. */
static void mode_famrb(void)
{
	static const int keep[3] = { -1, 0x0001, 0x0441 };
	static const int fams[3] = { 0x0010, 0x0000, 0x0020 };
	int k, f, i;

	puts("\n== famrb: does 0x0030 register?  0x11 byte 3 polled after each write ==");
	for (k = 0; k < 3; k++) {
		if (keep[k] < 0)
			puts("  keepalive: none");
		else
			printf("  keepalive: 0x10 0x%04X 0x05CF\n", keep[k]);
		for (f = 0; f < 3; f++) {
			printf("    family 0x%04X:", fams[f]);
			ctl(0x10, fams[f], 0x0030);
			if (keep[k] >= 0)
				ctl(0x10, keep[k], 0x05CF);
			for (i = 0; i < 25; i++) {
				printf(" %X", rd11());
				fflush(stdout);
				usleep(40000);
			}
			putchar('\n');
		}
	}
	puts("  (expect the last hex to become 1 / 0 / 2 for 0x0010 / 0x0000 / 0x0020)");
}

/* Which write in the cold-init tail resets the family register?  Write
 * family 0x0010, confirm 0x11 = 1, then replay the tail one write at a
 * time, reading 0x11 after each.  The first write after which it stops
 * being 1 is the one.  The driver's bf_cold_init() runs this same tail,
 * so whatever it is, the driver does it too. */
static void mode_famclob(void)
{
	static const struct { int req, val, idx; const char *what; } tail[] = {
		{ 0x1C, 0x0000, 0x0000, "0x1C 0x0000 0x0000" },
		{ 0x10, 0x0021, 0x05FF, "0x10 0x0021 0x05FF" },
		{ 0x17, 0x000C, 0x0000, "0x17 0x000C 0x0000" },
		{ 0x21, 0x0000, 0x0000, "0x21 0x0000 0x0000" },
		{ 0x10, 0x0000, 0x3000, "0x10 0x0000 0x3000 (1st)" },
		{ 0x10, 0x0000, 0x3000, "0x10 0x0000 0x3000 (2nd)" },
		{ 0x10, 0x0800, 0x0800, "0x10 0x0800 0x0800 (1st)" },
		{ 0x10, 0x0800, 0x0800, "0x10 0x0800 0x0800 (2nd)" },
		{ 0x10, 0x0800, 0x0800, "0x10 0x0800 0x0800 (3rd)" },
	};
	int i, k;

	puts("\n== famclob: which cold-init write resets the family register? ==");
	puts("   also the 0x16 clear loop, run first, in case it is that");

	ctl(0x10, 0x0010, 0x0030);
	usleep(50000);
	printf("  after family 0x0010:            0x11=%X\n", rd11());
	for (i = 0; i <= 0x3D; i++) {
		if (i == 0x1E || i == 0x1F)
			continue;
		ctl(0x16, 0x0000, i);
	}
	usleep(50000);
	printf("  after the 0x16 clear loop:      0x11=%X\n", rd11());

	ctl(0x10, 0x0010, 0x0030);
	usleep(50000);
	printf("  after family 0x0010 again:      0x11=%X\n", rd11());
	for (k = 0; k < (int)(sizeof(tail) / sizeof(tail[0])); k++) {
		ctl(tail[k].req, tail[k].val, tail[k].idx);
		usleep(50000);
		printf("  after %-28s 0x11=%X\n", tail[k].what, rd11());
	}
	puts("  (the first line that is not 1 names the clobbering write)");
}

/* All nine rates through RME's own mechanism: family register + alt,
 * no quad, no cold-init, sessions that send only what the Windows
 * capture shows on a rate change.  On PR #7 v1, whose family write
 * survived by accident (restore_state re-wrote it after cold-init had
 * clobbered it), Ismail measured 64 -> 48, 128 -> 96, 176.4 -> 192:
 * family 0 at x2/x4 and family 1 at x4 all fell to the 48 kHz family.
 * `family` only tested alt 1.  This is the same test at all three alts:
 * does the firmware's family-derived clock cover the whole table, or
 * does the host have to send a quad for three of the nine? */
static void mode_family9(void)
{
	static const struct { int fam, alt, hz; } t[] = {
		{ 0x0000, 1,  32000 }, { 0x0010, 1,  44100 }, { 0x0020, 1,  48000 },
		{ 0x0000, 2,  64000 }, { 0x0010, 2,  88200 }, { 0x0020, 2,  96000 },
		{ 0x0000, 3, 128000 }, { 0x0010, 3, 176400 }, { 0x0020, 3, 192000 },
	};
	int i;

	puts("\n== family9: all nine rates by family register + alt, no quad ==");
	g_noquad = 0;
	g_minimal = 0;
	trial2("reference: 48 kHz quad, family 0x0020", 48000, 0x0020, 0x0030,
	       0, 1, 0, 48000);
	g_minimal = 1;
	for (i = 0; i < (int)(sizeof(t) / sizeof(t[0])); i++) {
		char l[64];

		g_minimal_family = t[i].fam;
		snprintf(l, sizeof(l), "family 0x%04X alt%d, want %d",
			 t[i].fam, t[i].alt, t[i].hz);
		trial2(l, t[i].hz, -1, 0x0030, 0, t[i].alt, 0, 0);
	}
	g_minimal = 0;
	puts("   any row landing on a 48 kHz-family rate instead of the one asked");
	puts("   is a rate the family register cannot reach on its own.");
}

/* Is 0x05FF the settings word and the family register in one?
 * 0x05FF = 0x05CF | 0x0030, and the cold-init value 0x0021 has bits
 * 4-5 = 2 (48 kHz family) and bit 0 = internal clock.  Write three
 * values with different bits 4-5 and read 0x11 after each. */
static void mode_f5ff(void)
{
	static const int vals[4] = { 0x0011, 0x0001, 0x0021, 0x0011 };
	int i;

	puts("\n== f5ff: does 0x05FF carry the family in bits 4-5? ==");
	ctl(0x10, 0x0020, 0x0030);
	usleep(50000);
	printf("  family 0x0020 via 0x0030:   0x11=%X\n", rd11());
	for (i = 0; i < 4; i++) {
		ctl(0x10, vals[i], 0x05FF);
		usleep(50000);
		printf("  0x%04X -> 0x05FF:           0x11=%X   (bits 4-5 = %d)\n",
		       vals[i], rd11(), (vals[i] >> 4) & 3);
	}
	puts("  (0x11 following bits 4-5 means 0x05FF writes the family too)");
}

/* Varispeed at a base other than 48 kHz: is the quad an ABSOLUTE clock
 * that overrides the family, or a pitch RELATIVE to it?  TotalMix's
 * pitch capture was at 48 kHz, where the two are indistinguishable.
 * Every session here: RME's rate-change sequence for family 44.1 kHz,
 * then a quad, then arm.
 *
 *   48 kHz-period quad, pitch 0     absolute -> 48000   relative -> 44100
 *   48 kHz-period quad, +5 %        absolute -> 50400   relative -> 46305
 *   44.1 kHz-period quad, +5 %      absolute -> 46305   relative -> ~42542
 *
 * Decides how bf_pitch_put() has to compute its word. */
static void mode_pitchfam(void)
{
	static const struct { int base, pitch; const char *what; } t[] = {
		{ 48000,  0, "48k-period quad, pitch 0   (abs 48000 / rel 44100)" },
		{ 48000, 50, "48k-period quad, +5 %      (abs 50400 / rel 46305)" },
		{ 44100, 50, "44.1k-period quad, +5 %    (abs 46305 / rel ~42542)" },
		{ 44100,  0, "44.1k-period quad, pitch 0 (abs 44100 / rel ~40517)" },
	};
	unsigned int q[4];
	int i;

	puts("\n== pitchfam: quad on top of family 44.1 kHz - absolute or relative? ==");
	setif(1);
	cold_init(-1, 0x0030, 0, 48000);
	arm(1);
	measure(500, urbsize_for_alt(1));
	disarm();

	for (i = 0; i < (int)(sizeof(t) / sizeof(t[0])); i++) {
		int rb;

		quad_q16(t[i].base, t[i].pitch, q);
		ctl(0x10, 0x0010, 0x0030);
		ctl(0x10, 0x0441, 0x05CF);
		setif(1);
		quad_write(q);
		usleep(50000);
		rb = rd11();
		arm(1);
		report(t[i].what, 0, measure(MEAS_MS, urbsize_for_alt(1)), 1, rb);
		disarm();
	}
	puts("   and the control, family 44.1 with no quad at all:");
	ctl(0x10, 0x0010, 0x0030);
	ctl(0x10, 0x0441, 0x05CF);
	setif(1);
	usleep(50000);
	arm(1);
	report("family 44.1, no quad (expect 44100)", 44100,
	       measure(MEAS_MS, urbsize_for_alt(1)), 1, rd11());
	disarm();
}

/* ---- the ppm run ------------------------------------------------- */

static long long now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* Measure the IN byte rate over a long window, timing between URB
 * completions.  The bytes in URB i arrive between completion i-1 and
 * completion i, so: take the first completion as t0 and count nothing,
 * then count every later URB's bytes and take the last completion as
 * t1.  bytes / (t1 - t0) then has no start/stop quantisation at all;
 * the only limit left is the host clock, which CLOCK_MONOTONIC keeps
 * NTP-disciplined in frequency.  Blocking reaps, so it does not spin. */
static double measure_long(int seconds, int sz, int frame)
{
	long long t0 = -1, t1 = 0, bytes = 0, next_report;
	long long deadline = now_ns() + (long long)seconds * 1000000000LL;
	int n = 0;

	next_report = now_ns() + 60000000000LL;
	for (;;) {
		struct usbdevfs_urb *done = NULL;
		long long t;

		if (ioctl(fd, USBDEVFS_REAPURB, &done) < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "  ! reap: %s\n", strerror(errno));
			break;
		}
		t = now_ns();
		if (done->endpoint == EP_IN) {
			if (t0 < 0) {
				t0 = t;
			} else {
				bytes += done->actual_length;
				t1 = t;
				n++;
			}
		}
		done->buffer_length = sz;
		ioctl(fd, USBDEVFS_SUBMITURB, done);

		if (t >= next_report && t0 >= 0 && n > 0) {
			double hz = bytes / ((t1 - t0) / 1e9) / frame;

			printf("    %4lld s  %10.3f Hz\n", (t1 - t0) / 1000000000LL, hz);
			fflush(stdout);
			next_report += 60000000000LL;
		}
		if (t >= deadline && t0 >= 0 && n > 0)
			break;
	}
	if (t1 <= t0)
		return 0;
	return bytes / ((t1 - t0) / 1e9);
}

static void mode_ppm(const char *model, int seconds, int base)
{
	unsigned int q[4];
	double bps, hz, ppm;
	int family_mode = !strcmp(model, "family");

	if (family_mode) {
		/* RME's own mechanism: family register, no quad.  The
		 * firmware derives the DDS word; this measures how exactly. */
	} else if (!strcmp(model, "q16"))
		quad_q16(base, 0, q);
	else if (!strcmp(model, "exact"))
		quad_exact(base, 0, q);
	else if (!strcmp(model, "driver"))
		quad_driver(base, 0, q);
	else {
		fprintf(stderr, "ppm: model must be q16, exact, driver or family\n");
		return;
	}

	printf("\n== ppm: %s at %d, %d s, alt1 ==\n", model, base, seconds);
	if (family_mode)
		puts("   family register only, no quad (a 48 kHz cold-init first)");
	else
		printf("   quad: %06X %06X %06X %06X\n", q[0], q[1], q[2], q[3]);
	puts("   (check the host clock is disciplined: chronyc tracking)");

	if (family_mode) {
		int fam = base == 32000 ? 0x0000 : base == 44100 ? 0x0010 : 0x0020;
		unsigned int one[4];

		/* The quad is a pitch RATIO against the device's 48 kHz
		 * reference and it is sticky (pitchfam, 2026-09-16), so the
		 * reference session must leave an EXACT x1.0 quad in place,
		 * not the driver's approximation (x1.00001) and not RME's Q16
		 * word (x1.000015).  Then the family alone sets the rate and
		 * this measures the firmware's own derivation. */
		quad_exact(48000, 0, one);
		g_quad = one;
		setif(1);
		cold_init(-1, 0x0030, 0, 0);
		g_quad = NULL;
		arm(1);
		measure(500, urbsize_for_alt(1));
		disarm();
		/* then exactly what RME sends on a rate change */
		ctl(0x10, fam, 0x0030);
		ctl(0x10, 0x0441, 0x05CF);
		setif(1);
		usleep(50000);
	} else {
		g_quad = q;
		setif(1);
		cold_init(-1, 0x0030, 0, 0);
	}
	arm(1);
	bps = measure_long(seconds, urbsize_for_alt(1), frame_for_alt(1));
	disarm();
	g_quad = NULL;

	hz = bps / frame_for_alt(1);
	ppm = (hz / base - 1.0) * 1e6;
	printf("\n   %s @ %d: %.4f Hz  (%+.2f ppm)\n", model, base, hz, ppm);
	puts("   Run q16 and exact alternately, twice each, and compare.");
}

/* No hardware: print what the three models send at each base, so a
 * Windows capture can be compared against them by eye. */
static void mode_quads(void)
{
	static const int bases[3] = { 32000, 44100, 48000 };
	static const int pitches[3] = { -50, 0, 50 };
	int b, p;

	puts("base    pitch  model    bank0    bank1    bank2    bank3");
	for (b = 0; b < 3; b++) {
		for (p = 0; p < 3; p++) {
			unsigned int q[4];

			quad_q16(bases[b], pitches[p], q);
			printf("%-7d %+4d   q16     %06X   %06X   %06X   %06X\n",
			       bases[b], pitches[p], q[0], q[1], q[2], q[3]);
			quad_exact(bases[b], pitches[p], q);
			printf("%-7d %+4d   exact   %06X   %06X   %06X   %06X\n",
			       bases[b], pitches[p], q[0], q[1], q[2], q[3]);
			quad_driver(bases[b], pitches[p], q);
			printf("%-7d %+4d   driver  %06X   %06X   %06X   %06X\n",
			       bases[b], pitches[p], q[0], q[1], q[2], q[3]);
		}
	}
	puts("\nbank0 above 0xFFFFFF (the 32 kHz base) goes on the wire as its"
	     " low 16 bits of integer part; the others are in range.");
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
	int ppm_secs = 600, ppm_base = 44100;
	const char *ppm_model = NULL;

	if (!strcmp(mode, "quads")) {
		mode_quads();
		return 0;
	}
	if (!strcmp(mode, "ppm")) {
		/* ppm <model> [seconds] [base]; the device is auto-detected */
		if (argc < 3) {
			fprintf(stderr, "usage: ratesweep ppm <q16|exact|driver> "
				"[seconds] [base]\n");
			return 1;
		}
		ppm_model = argv[2];
		dev = NULL;
		if (argc > 3)
			ppm_secs = atoi(argv[3]);
		if (argc > 4)
			ppm_base = atoi(argv[4]);
	}

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
	else if (!strcmp(mode, "bank"))     mode_bank();
	else if (!strcmp(mode, "ppm"))      mode_ppm(ppm_model, ppm_secs, ppm_base);
	else if (!strcmp(mode, "family"))   mode_family();
	else if (!strcmp(mode, "famrb"))    mode_famrb();
	else if (!strcmp(mode, "famclob"))  mode_famclob();
	else if (!strcmp(mode, "family9"))  mode_family9();
	else if (!strcmp(mode, "f5ff"))     mode_f5ff();
	else if (!strcmp(mode, "pitchfam")) mode_pitchfam();
	else {
		fprintf(stderr, "unknown mode: %s\n"
			"modes: sweep alts order clobber scanbase idx dds "
			"ddsscan combo table bank3 pitch rearm bank quads ppm "
			"family famrb famclob family9 f5ff pitchfam\n", mode);
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
