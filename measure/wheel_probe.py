#!/usr/bin/env python3
"""wheel_probe.py - measure what the front-panel OUT wheel does to the audio.

Plays a steady 997 Hz tone into one output and records it back while you
turn the wheel with the panel's OUT selection on that output.

  --target optical  (default) ADAT7/8, recorded through the device
                    loopback: the digital signal only.  The analog
                    outputs stay muted.
  --target phones   PH3/4, recorded through a patch cable from the
                    headphone jack into IN3: the real analog output.
                    AN1/2 and the digital outputs are muted, and every
                    input is removed from every output first, so IN3
                    cannot feed back.  Nothing else may be plugged into
                    either headphone jack.

The mixer is restored afterwards.

The recording is analysed sample by sample.  A pure sine obeys
x[n] = 2cos(w) x[n-1] - x[n-2]; the residual of that prediction is
zero except where the gain changes or the stream breaks:

  step    a clean gain change; its width in samples shows whether the
          device smooths the change (1-3 samples = instant)
  glitch  a discontinuity far larger than any gain step can produce:
          a dropout, a repeated or missing block

It also reads the driver's "Debug Stream Counters" before and after
each run, and the wheel_mode module parameter selects how the driver
writes the volume (see the local wheel-debug driver commit).

Needs: the local wheel-debug driver loaded, the card released by
PipeWire (`just eval-wheel` does both the release and the hand-back),
alsa-utils, sudo for the module parameter, and nothing connected to the
optical output.

Usage: wheel_probe.py [--target optical|phones] [--card BabyfacePro]
                      [--out DIR] [--seconds 12]
"""
import argparse
import math
import os
import struct
import subprocess
import sys
import tempfile
import threading
import time
import wave

RATE = 48000
CH = 12
FREQ = 997.0
TONE_DBFS = -12.0
PB_CH = (10, 11)          # PB6 = playback words 10/11
PB6_SRC = 13
SOURCES = ("AN1", "AN2", "AN3", "AN4", "AS1/2", "ADAT3/4", "ADAT5/6",
           "ADAT7/8", "PB1", "PB2", "PB3", "PB4", "PB5", "PB6")
OUTPUTS = ("AN1/2", "PH3/4", "AS1/2", "ADAT3/4", "ADAT5/6", "ADAT7/8")

# out: output index; rec: capture channels to analyse (None = find the
# channel carrying the tone); loopback: use the device loopback.
TARGETS = {
    "optical": {"out": 5, "rec": (8, 9), "loopback": True,
                "panel": "Opt"},
    "phones": {"out": 1, "rec": None, "loopback": False,
               "panel": "Phones"},
    # Optical out cabled to optical in: the real optical signal.
    "optical-cable": {"out": 5, "rec": None, "loopback": False,
                      "panel": "Opt"},
}
# Capture channels 10/11 carry a fixed-gain playback tap, never the
# patched input.
TAP_CH = (10, 11)
FADER_0DB = 0x16a0
MASTER_START = 0x0333     # -20 dB, room to turn both ways
PARAM = "/sys/module/snd_usb_babyface_pro/parameters/wheel_mode"

# Tone level for run(); the trace runs lower it for headroom up to +6 dB.
RUN_TONE_DBFS = [None]

STEP_THR = 0.003          # residual / amplitude: about 0.05 dB
GLITCH_THR = 0.3          # far beyond a 0.5 dB step (~0.06-0.12)


class Card:
    def __init__(self, card):
        self.card = card

    def cset(self, name, value, index=0, iface="MIXER"):
        ident = "iface=%s,name='%s',index=%d" % (iface, name, index)
        subprocess.run(["amixer", "-q", "-c", self.card, "cset", ident,
                        str(value)], check=True)

    def cget(self, name, index=0, iface="MIXER"):
        ident = "iface=%s,name='%s',index=%d" % (iface, name, index)
        out = subprocess.run(["amixer", "-c", self.card, "cget", ident],
                             check=True, capture_output=True, text=True).stdout
        for line in out.splitlines():
            line = line.strip()
            if line.startswith(": values="):
                return [1 if v == "on" else 0 if v == "off" else int(v)
                        for v in line.split("=", 1)[1].split(",")]
        raise RuntimeError("no values for " + ident)

    def counters(self):
        try:
            return self.cget("Debug Stream Counters", iface="CARD")
        except (subprocess.CalledProcessError, RuntimeError):
            return None


COUNTER_NAMES = ("in_err", "in_short", "out_err", "out_starved",
                 "ctl_writes", "ctl_max_us", "resubmit_err")


def set_mode(mode):
    if mode is None:
        return
    subprocess.run(["sudo", "tee", PARAM], input=str(mode), text=True,
                   stdout=subprocess.DEVNULL, check=True)


def setup(card, target):
    out = target["out"]
    # Every output silent except the target.
    for i, name in enumerate(OUTPUTS):
        card.cset(name + " Playback Switch", "on" if i == out else "off",
                  index=i)
    # Remove every source from every output, then PB6 into the target
    # only.  Clearing the inputs everywhere is what keeps a cable from
    # an output back into an input from feeding back.
    for o in range(6):
        for src in range(14):
            val = FADER_0DB if (o == out and src == PB6_SRC) else 0
            card.cset(SOURCES[src] + " Playback Volume",
                      "%d,%d" % (val, val), index=o * 14 + src)
    for o in range(6):
        for src in range(8):
            got = card.cget(SOURCES[src] + " Playback Volume",
                            index=o * 14 + src)
            if any(got):
                raise RuntimeError("input %s still routed to %s"
                                   % (SOURCES[src], OUTPUTS[o]))
    for i in range(6):
        card.cset("Loopback Switch",
                  "on" if (target["loopback"] and i == out) else "off",
                  index=i)


def start_master(card, target):
    out = target["out"]
    card.cset(OUTPUTS[out] + " Playback Volume",
              "%d,%d" % (MASTER_START, MASTER_START), index=out)


def tone_second(dbfs=None):
    """One second of tone; 997 Hz is exactly 997 cycles per second at
    48 kHz, so the block repeats without a seam."""
    amp = (10 ** ((TONE_DBFS if dbfs is None else dbfs) / 20)) * 0x7fffff
    w = 2 * math.pi * FREQ / RATE
    out = bytearray(RATE * CH * 4)
    for f in range(RATE):
        b = struct.pack("<i", int(round(amp * math.sin(w * f))) << 8)
        base = f * CH * 4
        for c in PB_CH:
            out[base + c * 4:base + c * 4 + 4] = b
    return bytes(out)


def run(card, dev, seconds, path, go=None, before_rec=None):
    """Play the tone and record `seconds` of all capture channels.
    `go` is called 1.5 s into the recording, after the start level."""
    play = subprocess.Popen(["aplay", "-q", "-D", dev, "-t", "raw",
                             "-f", "S32_LE", "-c", str(CH), "-r", str(RATE)],
                            stdin=subprocess.PIPE)
    stop = threading.Event()
    block = tone_second(RUN_TONE_DBFS[0])

    def feed():
        try:
            while not stop.is_set():
                play.stdin.write(block)
        except (BrokenPipeError, ValueError):
            pass

    t = threading.Thread(target=feed, daemon=True)
    t.start()
    time.sleep(0.8)
    if before_rec:
        before_rec()
    rec = subprocess.Popen(["arecord", "-q", "-D", dev, "-t", "raw",
                            "-f", "S32_LE", "-c", str(CH), "-r", str(RATE),
                            "-d", str(seconds), path])
    if go:
        time.sleep(1.5)
        go()
    if rec.wait():
        raise subprocess.CalledProcessError(rec.returncode, "arecord")
    stop.set()
    play.terminate()
    play.wait()


def channel_peaks(path):
    """Peak per capture channel in dBFS, after the first half second."""
    with open(path, "rb") as f:
        data = f.read()
    frames = len(data) // (CH * 4)
    vals = struct.unpack_from("<%di" % (frames * CH), data)
    out = []
    for c in range(CH):
        p = max((abs(v) for v in vals[c::CH][RATE // 2:]), default=0)
        out.append(20 * math.log10(p / 2 ** 31) if p else -180.0)
    return out


def load(path, chans):
    """The recording's channels `chans`; with None, the two capture
    channels carrying the most signal (the patched input)."""
    with open(path, "rb") as f:
        data = f.read()
    frames = len(data) // (CH * 4)
    vals = struct.unpack_from("<%di" % (frames * CH), data)
    if chans is None:
        peak = [max(abs(v) for v in vals[c::CH][RATE // 2:]) for c in range(CH)]
        chans = sorted((c for c in range(CH) if c not in TAP_CH),
                       key=lambda c: -peak[c])[:2]
        chans.sort()
        print("   tone found on capture ch %s (peak %.1f dBFS)"
              % (chans, 20 * math.log10(max(peak[chans[0]], 1) / 2 ** 31)))
    return [[v / 2147483648.0 for v in vals[c::CH]] for c in chans]


def save_wav(path, left, right):
    with wave.open(path, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(3)
        w.setframerate(RATE)
        buf = bytearray()
        for a, b in zip(left, right):
            for v in (a, b):
                buf += struct.pack("<i", int(max(-1, min(1, v)) * 0x7fffff))[:3]
        w.writeframes(bytes(buf))


def analyse(x):
    skip = RATE // 3
    x = x[skip:]
    n = len(x)
    c = 2 * math.cos(2 * math.pi * FREQ / RATE)
    blk = RATE // 200                  # 5 ms
    env = []
    for i in range(0, n, blk):
        seg = x[i:i + blk]
        env.append(max(abs(v) for v in seg) if seg else 0.0)

    def amp(i):
        return max(env[min(i // blk, len(env) - 1)], 1e-9)

    events = []
    cur = None
    for i in range(2, n):
        r = abs(x[i] - c * x[i - 1] + x[i - 2]) / amp(i)
        if r > STEP_THR:
            if cur and i - cur["end"] <= RATE // 1000:
                cur["end"] = i
                cur["width"] += 1
                cur["peak"] = max(cur["peak"], r)
            else:
                cur = {"start": i, "end": i, "width": 1, "peak": r}
                events.append(cur)

    def db(v):
        return 20 * math.log10(v) if v > 1e-9 else -180.0

    steps, glitches = [], []
    for e in events:
        b0 = max(0, e["start"] // blk - 2)
        b1 = min(len(env) - 1, e["end"] // blk + 2)
        e["delta_db"] = db(env[b1]) - db(env[b0])
        e["t"] = (e["start"] + skip) / RATE
        (glitches if e["peak"] >= GLITCH_THR else steps).append(e)

    live = [v for v in env if v > 1e-6]
    return {
        "level_db": (db(min(live)) if live else -180,
                     db(max(live)) if live else -180),
        "silent_blocks": sum(1 for v in env if v < 1e-6),
        "steps": steps,
        "glitches": glitches,
    }


def kmsg_mark(tag):
    subprocess.run(["sudo", "tee", "/dev/kmsg"], input="wheel_probe %s\n" % tag,
                   text=True, stdout=subprocess.DEVNULL, check=True)


def kmsg_since(tag):
    """Kernel log lines after our marker (the ring buffer rotates, so
    line counts are not usable)."""
    out = subprocess.run(["sudo", "dmesg"], capture_output=True, text=True,
                         check=True).stdout.splitlines()
    for i in range(len(out) - 1, -1, -1):
        if "wheel_probe %s" % tag in out[i]:
            return out[i + 1:]
    return []


def wheel_counts(lines):
    """Sum the wheel counter deltas in "panel a b c d -> e f g h" lines,
    the same way the driver does (low nibble of byte 2, 4-bit wrap, only
    within one mode class)."""
    def cls(b):
        return 1 if b >> 4 in (8, 9) else 0 if b >> 4 == 0 else 2
    up = down = 0
    polls = []
    for line in lines:
        if " panel " not in line or "->" not in line:
            continue
        try:
            a, b = line.split(" panel ", 1)[1].split("->")
            p = [int(v, 16) for v in a.split()]
            q = [int(v, 16) for v in b.split()]
        except ValueError:
            continue
        d = (q[2] & 15) - (p[2] & 15)
        if d > 8:
            d -= 16
        elif d < -8:
            d += 16
        if d and cls(q[2]) == cls(p[2]):
            polls.append(d)
            if d > 0:
                up += d
            else:
                down -= d
    return up, down, polls


def kmsg_events(lines, tag):
    """(timestamp, delta) for each wheel count change after the marker,
    and the marker's own timestamp."""
    import re
    ts_re = re.compile(r"^\[\s*([0-9.]+)\]")
    t0 = None
    events = []

    def cls(b):
        return 1 if b >> 4 in (8, 9) else 0 if b >> 4 == 0 else 2
    for line in lines:
        m = ts_re.match(line)
        if not m:
            continue
        t = float(m.group(1))
        if "wheel_probe %s" % tag in line:
            t0 = t
            events = []
            continue
        if t0 is None or " panel " not in line or "->" not in line:
            continue
        try:
            a, b = line.split(" panel ", 1)[1].split("->")
            p = [int(v, 16) for v in a.split()]
            q = [int(v, 16) for v in b.split()]
        except ValueError:
            continue
        d = (q[2] & 15) - (p[2] & 15)
        if d > 8:
            d -= 16
        elif d < -8:
            d += 16
        if d and cls(q[2]) == cls(p[2]):
            events.append((round(t - t0, 4), d))
    return t0, events


def run_trace(card, dev, target, seconds, outdir, repeats, check=False,
              balance=False):
    """Record turns with the driver silent (trace) or, with check, in
    wheel_mode 1, and compare the driver's cached level with the level
    the device ends at."""
    import json
    name = "check" if check else "balance" if balance else "trace"
    kinds = (CHECK_KINDS if check else
             BALANCE_KINDS if balance else TRACE_KINDS)
    set_mode(1 if check else 5)
    sides = recorded_sides(card, dev, target, outdir) if balance else {}
    side = min(sides) if sides else 0
    subprocess.run(["sudo", "tee", PANEL_DEBUG], input="1", text=True,
                   stdout=subprocess.DEVNULL, check=True)
    RUN_TONE_DBFS[0] = TRACE_TONE_DBFS
    runs = [k for _ in range(repeats) for k in kinds]
    meta = []
    try:
        for n, (label, start_db, what) in enumerate(runs):
            out = target["out"]
            if isinstance(start_db, tuple):
                pair = start_db
                start_db = pair[side]
                force_master(card, out, tuple(master_for_db(d)
                                              for d in pair))
            else:
                pair = None
                force_master(card, out, master_for_db(start_db))
            time.sleep(0.3)
            print("\n-- %d/%d %s\n   %s, then stop." % (n + 1, len(runs),
                                                     label, what))
            input("   Press Enter, hands off, and wait for GO. ")
            tag = "%s%d-%d" % (name, os.getpid(), n)
            raw = os.path.join(outdir, "%s-%02d.raw" % (name, n))
            run(card, dev, seconds, raw,
                go=lambda: print("   >>> GO <<<", flush=True),
                before_rec=lambda: kmsg_mark(tag))
            out_lines = subprocess.run(["sudo", "dmesg"], capture_output=True,
                                       text=True, check=True).stdout
            t0, events = kmsg_events(out_lines.splitlines(), tag)
            if pair:
                # Both sides when split; the wav is L/R (a missing side
                # repeats the other).
                chans = load(raw, SPLIT_CH)
                by_side = {sd: chans[SPLIT_CH.index(ch)]
                           for sd, ch in sides.items()}
                x = by_side[side]
                other = by_side.get(1 - side, x)
                lr = (x, other) if side == 0 else (other, x)
            else:
                x = max(load(raw, target["rec"]),
                        key=lambda ch: max(abs(v) for v in ch))
                lr = (x, x)
            os.remove(raw)
            wav = os.path.join(outdir, "%s-%02d.wav" % (name, n))
            save_wav(wav, *lr)
            db0, db1 = settled_db(x, True), settled_db(x, False)
            net = sum(d for _, d in events)
            clip = max(abs(v) for v in x) > 0.95
            print("   counts %+d in %d polls, level %.1f -> %.1f dB%s" %
                  (net, len(events), db0, db1,
                   "  (CLIPPED)" if clip else ""))
            entry = {}
            if pair:
                ends = {}
                for sd, xs in sorted(by_side.items()):
                    d0, d1 = settled_db(xs, True), settled_db(xs, False)
                    ends[sd] = pair[sd] + d1 - d0
                    print("   %s: %+.1f -> %+.1f dB (moved %+.1f)" %
                          (("left", "right")[sd], pair[sd], ends[sd],
                           ends[sd] - pair[sd]))
                entry["end_lr"] = [ends.get(0), ends.get(1)]
            if check:
                raw16 = card.cget(OUTPUTS[out] + " Playback Volume",
                                  index=out)
                cache_db = [6 * math.log2(v / 0x2000) if v else -180.0
                            for v in raw16]
                device_db = start_db + (db1 - db0)
                # The resync write comes 150 ms after the last click;
                # measure the device's own level just before it.
                pre_db = None
                if events:
                    a = int((events[-1][0] + 0.04) * RATE)
                    if a + int(0.08 * RATE) <= len(x):
                        pre_db = start_db + (tone_db(
                            x[a:a + int(0.08 * RATE)]) - db0)
                pre = pre_db if pre_db is not None else device_db
                print("   device %.1f dB before the resync, %.1f after; "
                      "driver cache %s  %s" %
                      (pre, device_db,
                       "/".join("%.1f" % c for c in cache_db),
                       "OK" if abs(cache_db[0] - pre) < 0.25
                       else "MISMATCH (resync moved it %+.1f dB)"
                       % (device_db - pre)))
                entry = {"cache_raw": raw16, "cache_db": cache_db,
                         "device_db": device_db, "pre_resync_db": pre_db}
            if pair:
                entry["start_lr"] = pair
                entry["sides"] = {str(k): v for k, v in sides.items()}
                entry["wav_channels"] = "left, right"
            meta.append({"n": n, "label": label, "start_db": start_db,
                         "master": master_for_db(start_db),
                         "tone_dbfs": TRACE_TONE_DBFS,
                         "wav": os.path.basename(wav), "marker_found":
                         t0 is not None, "events": events,
                         "level_start": db0, "level_end": db1,
                         "clipped": clip, **entry})
            with open(os.path.join(outdir, name + ".json"), "w") as f:
                json.dump({"tone_dbfs": TRACE_TONE_DBFS, "rate": RATE,
                           "go_s": 1.5, "runs": meta}, f, indent=1)
    finally:
        RUN_TONE_DBFS[0] = None
        subprocess.run(["sudo", "tee", PANEL_DEBUG], input="0", text=True,
                       stdout=subprocess.DEVNULL)
    print("\nSaved %d recordings and %s.json in %s" % (len(meta), name,
                                                       outdir))


def settled_db(x, first):
    """Tone level before GO (0.5-1.3 s) or over the last 300 ms."""
    seg = x[RATE // 2:int(1.3 * RATE)] if first else x[-int(0.3 * RATE):]
    return tone_db(seg)


def tone_db(seg):
    """Level of the tone in `seg`, dB of full scale."""
    c = math.cos(2 * math.pi * FREQ / RATE)
    s0 = s1 = 0.0
    for v in seg:
        s0, s1 = v + 2 * c * s0 - s1, s0
    power = s0 * s0 + s1 * s1 - 2 * c * s0 * s1
    amp = 2 * math.sqrt(max(power, 0)) / len(seg)
    return 20 * math.log10(amp) if amp > 0 else -180.0


def force_master(card, out, value):
    """Set the master even when the driver's cache already holds it (the
    driver skips a write of an unchanged value).  `value` is one raw
    value for both sides or an (L, R) pair."""
    name = OUTPUTS[out] + " Playback Volume"
    l, r = value if isinstance(value, tuple) else (value, value)
    card.cset(name, "%d,%d" % (l + 1, r + 1), index=out)
    card.cset(name, "%d,%d" % (l, r), index=out)


# IN3 and IN4: a stereo-to-mono split cable puts one side on each.
SPLIT_CH = (2, 3)


def recorded_sides(card, dev, target, outdir):
    """Which side of the output each of IN3/IN4 carries, as
    {side: channel}, side 0 left, 1 right.  With a single mono cable
    only one side is found."""
    out = target["out"]
    levels = []
    for side in (0, 1):
        loud = master_for_db(-10)
        quiet = master_for_db(-50)
        force_master(card, out, (loud, quiet) if side == 0
                     else (quiet, loud))
        time.sleep(0.3)
        raw = os.path.join(outdir, "side%d.raw" % side)
        run(card, dev, 2, raw)
        levels.append([tone_db(x[RATE // 2:])
                       for x in load(raw, SPLIT_CH)])
        os.remove(raw)
    sides = {}
    for i, ch in enumerate(SPLIT_CH):
        diff = levels[0][i] - levels[1][i]
        name = "-"
        if diff > 20:
            sides[0] = ch
            name = "left"
        elif diff < -20:
            sides[1] = ch
            name = "right"
        print("   IN%d: %.1f dB with left loud, %.1f dB with right loud "
              "-> %s" % (ch + 1, levels[0][i], levels[1][i], name))
    if not sides:
        raise RuntimeError("neither IN3 nor IN4 carries one side")
    return sides


def run_steps(card, dev, target, seconds, outdir, repeats):
    set_mode(5)
    subprocess.run(["sudo", "tee", PANEL_DEBUG], input="1", text=True,
                   stdout=subprocess.DEVNULL, check=True)
    runs = [k for _ in range(repeats) for k in STEP_KINDS]
    rows = []
    try:
        for n, (label, start, what) in enumerate(runs):
            out = target["out"]
            force_master(card, out, start)
            time.sleep(0.3)
            print("\n-- %d/%d %s: %s, then stop." % (n + 1, len(runs),
                                                  label, what))
            input("   Press Enter, hands off, and wait for GO. ")
            tag = "run%d-%d" % (os.getpid(), n)
            kmsg_mark(tag)
            raw = os.path.join(outdir, "steps-%d.raw" % n)
            run(card, dev, seconds, raw,
                go=lambda: print("   >>> GO <<<", flush=True))
            up, down, polls = wheel_counts(kmsg_since(tag))
            x = max(load(raw, target["rec"]),
                    key=lambda ch: max(abs(v) for v in ch))
            os.remove(raw)
            db0, db1 = settled_db(x, True), settled_db(x, False)
            net = up - down
            per = abs(db1 - db0) / abs(net) if net else float("nan")
            clip = max(abs(v) for v in x) > 0.95
            print("   counts %+d (polls: %s)" %
                  (net, " ".join("%+d" % d for d in polls[:40])))
            print("   level %.1f -> %.1f dB: %+.1f dB, %.2f dB per count%s" %
                  (db0, db1, db1 - db0, per,
                   "  (CLIPPED, ignore)" if clip else ""))
            rows.append((label, net, db1 - db0, per,
                         max(map(abs, polls), default=0), clip))
    finally:
        subprocess.run(["sudo", "tee", PANEL_DEBUG], input="0", text=True,
                       stdout=subprocess.DEVNULL)
    print("\n== step size per count")
    for label, net, ddb, per, big, clip in rows:
        print("  %-11s counts %+3d  level %+6.1f dB  %.2f dB/count  "
              "max per poll %d%s" % (label, net, ddb, per, big,
                                     "  CLIPPED" if clip else ""))
    for kind, _, _ in STEP_KINDS:
        vals = [r[3] for r in rows if r[0] == kind and r[1] and not r[5]]
        if vals:
            print("  %-11s mean %.2f dB/count over %d runs"
                  % (kind, sum(vals) / len(vals), len(vals)))


def median(vals):
    vals = sorted(vals)
    return vals[len(vals) // 2] if vals else 0


def report(label, res, before, after):
    s, g = res["steps"], res["glitches"]
    print("\n== %s" % label)
    print("  level range   %.1f .. %.1f dBFS" % res["level_db"])
    print("  gain steps    %d  (median width %d samples, median |delta| "
          "%.2f dB, max width %d)"
          % (len(s), median([e["width"] for e in s]),
             median([abs(e["delta_db"]) for e in s]),
             max([e["width"] for e in s], default=0)))
    print("  glitches      %d%s" % (len(g), "" if not g else
          "  at " + ", ".join("%.3fs" % e["t"] for e in g[:10])))
    if res["silent_blocks"]:
        print("  silent 5 ms blocks: %d" % res["silent_blocks"])
    if before and after:
        diff = ["%s=%d" % (k, b - a) for k, a, b in
                zip(COUNTER_NAMES, before, after) if k != "ctl_max_us"]
        print("  driver        " + " ".join(diff) +
              "  ctl_max_us=%d (since load)" % after[5])
    return {"label": label, "steps": len(s), "glitches": len(g)}


RUNS_OPTICAL = [
    ("baseline, wheel untouched", 1,
     "Do NOT touch the wheel during this run."),
    ("mode 0 (direct), five single clicks", 0,
     "Turn the wheel ONE click, wait a second, repeat about five times."),
    ("mode 0 (direct), turning", 0,
     "Turn the wheel slowly up and down, then quickly, until it stops."),
    ("mode 1 (ramp), turning", 1,
     "Turn the wheel slowly up and down, then quickly, until it stops."),
    ("mode 2 (16-bit, 8-bit at rest), turning", 2,
     "Turn the wheel slowly up and down, then quickly, until it stops."),
    ("mode 3 (8-bit only), turning", 3,
     "Turn the wheel slowly up and down, then quickly, until it stops."),
]

TURN = "Turn the wheel slowly up and down, then quickly, until it stops."
RUNS_PHONES = [
    ("baseline, wheel untouched", 1,
     "Do NOT touch the wheel during this run."),
    ("mode 0 (direct), five single clicks", 0,
     "Turn the wheel ONE click, wait a second, repeat about five times."),
    ("mode 3 (8-bit only), five single clicks", 3,
     "Turn the wheel ONE click, wait a second, repeat about five times."),
    ("mode 4 (16-bit only), five single clicks", 4,
     "Turn the wheel ONE click, wait a second, repeat about five times."),
    ("mode 0 (direct), turning", 0, TURN),
    ("mode 3 (8-bit only), turning", 3, TURN),
    ("mode 4 (16-bit only), turning", 4, TURN),
    ("mode 1 (ramp), turning", 1, TURN),
]
# Does the device move the volume itself?  Mode 5 writes nothing.
RUNS_DEVICE = [
    ("mode 5 (driver silent), five single clicks", 5,
     "Turn the wheel ONE click, wait a second, repeat about five times."),
    ("mode 5 (driver silent), turning", 5, TURN),
    ("mode 0 (direct), turning, for comparison", 0, TURN),
]
# The fix (mode 1: 16-bit only, 1 dB per click) against the old driver
# (mode 0) and the device alone (mode 5).
RUNS_FIX = [
    ("mode 1 (fix), five single clicks", 1,
     "Turn the wheel ONE click, wait a second, repeat about five times."),
    ("mode 1 (fix), turning", 1, TURN),
    ("mode 0 (old driver), turning", 0, TURN),
    ("mode 5 (driver silent), turning", 5, TURN),
]
# How far the device moves per wheel count, slow and fast.  Driver
# silent (mode 5); the panel log gives the counts, the recording the dB.
# (label, start master, instruction)
STEP_UP_START = 0x0103       # -30 dB: room to go up without clipping IN3
STEP_DOWN_START = 0x1000     # -6 dB: room to go down
STEP_KINDS = [
    ("slow, up", STEP_UP_START,
     "turn CLOCKWISE slowly, one click at a time, about 10 clicks"),
    ("fast, up", STEP_UP_START,
     "spin CLOCKWISE quickly, about 10 clicks in one flick"),
    ("slow, down", STEP_DOWN_START,
     "turn ANTICLOCKWISE slowly, one click at a time, about 10 clicks"),
    ("fast, down", STEP_DOWN_START,
     "spin ANTICLOCKWISE quickly, about 10 clicks in one flick"),
]
PANEL_DEBUG = "/sys/module/snd_usb_babyface_pro/parameters/panel_debug"


def master_for_db(db):
    return int(round(0x2000 * 2 ** (db / 6)))


# (label, start dB, instruction).  Starts on both sides of -10 dB, where
# the slow step seems to change from 1 dB to 0.5 dB.
SLOW = "turn slowly, one click at a time with a short pause, about 12 clicks"
STEADY = "turn steadily without pausing, about 2 clicks per second, 12 clicks"
FAST = "spin quickly, about 12 clicks in one flick"
TRACE_KINDS = [
    ("slow up from -40", -40, "CLOCKWISE: " + SLOW),
    ("slow up from -14", -14, "CLOCKWISE: " + SLOW),
    ("slow down from +4", 4, "ANTICLOCKWISE: " + SLOW),
    ("slow down from -12", -12, "ANTICLOCKWISE: " + SLOW),
    ("steady up from -40", -40, "CLOCKWISE: " + STEADY),
    ("steady down from +4", 4, "ANTICLOCKWISE: " + STEADY),
    ("fast up from -40", -40, "CLOCKWISE: " + FAST),
    ("fast down from +4", 4, "ANTICLOCKWISE: " + FAST),
]
# check: the driver follows the wheel (wheel_mode 1).  Mixed speeds, so
# the speed guess and the resync at rest both get exercised.
MIXED = ("a few slow clicks, then a quick flick, then a few more slow "
         "clicks, about 12 clicks in all")
CHECK_KINDS = [
    ("check slow up from -40", -40, "CLOCKWISE: " + SLOW),
    ("check steady down from +4", 4, "ANTICLOCKWISE: " + STEADY),
    ("check fast up from -40", -40, "CLOCKWISE: " + FAST),
    ("check fast down from +4", 4, "ANTICLOCKWISE: " + FAST),
    ("check mixed up from -30", -30, "CLOCKWISE: " + MIXED),
    ("check mixed down from 0", 0, "ANTICLOCKWISE: " + MIXED),
]
# balance: the two sides of the output set apart, driver silent.  Each
# pair (L dB, R dB) runs twice with the sides swapped, so the recorded
# side is once the louder and once the quieter one.
SLOW10 = ("turn slowly, one click at a time with a short pause, "
          "about 10 clicks")
BALANCE_KINDS = [
    ("balance slow up L-20 R-32", (-20, -32), "CLOCKWISE: " + SLOW10),
    ("balance slow up L-32 R-20", (-32, -20), "CLOCKWISE: " + SLOW10),
    ("balance slow down L0 R-14", (0, -14), "ANTICLOCKWISE: " + SLOW10),
    ("balance slow down L-14 R0", (-14, 0), "ANTICLOCKWISE: " + SLOW10),
    ("balance fast up L-40 R-28", (-40, -28), "CLOCKWISE: " + FAST),
    ("balance fast up L-28 R-40", (-28, -40), "CLOCKWISE: " + FAST),
]
TRACE_TONE_DBFS = -24.0
RUNS = RUNS_OPTICAL


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--target", choices=sorted(TARGETS), default="optical")
    ap.add_argument("--runs",
                    choices=("default", "device", "fix", "steps", "trace",
                             "check", "balance"),
                    default="default",
                    help="device: only the driver-silent comparison")
    ap.add_argument("--repeats", type=int, default=2,
                    help="steps: how many times to repeat the set")
    ap.add_argument("--card", default="BabyfacePro")
    ap.add_argument("--out", default=None, help="keep recordings here")
    ap.add_argument("--seconds", type=int, default=12)
    args = ap.parse_args()
    card = Card(args.card)
    target = TARGETS[args.target]
    runs = RUNS_PHONES if args.target == "phones" else RUNS_OPTICAL
    if args.runs == "device":
        runs = RUNS_DEVICE
    elif args.runs == "fix":
        runs = RUNS_FIX
    dev = "hw:%s,0" % args.card
    outdir = args.out or tempfile.mkdtemp(prefix="wheel-probe-")
    os.makedirs(outdir, exist_ok=True)

    if card.counters() is None:
        sys.exit("No 'Debug Stream Counters' control: load the wheel-debug "
                 "driver first.")
    if not os.path.exists(PARAM):
        sys.exit("No wheel_mode parameter: load the wheel-debug driver first.")
    if args.target == "optical-cable":
        print("Optical cable from the optical output into the optical "
              "input. The analog outputs stay muted.")
    elif args.target == "phones":
        print("Patch cable from a headphone jack into IN3. Nothing else in "
              "either headphone jack.\nKeep the turns moderate: very high "
              "levels can clip IN3.")
    else:
        print("Nothing may be connected to the optical output. The analog "
              "outputs stay muted.")
    if input("Type yes to continue: ").strip() != "yes":
        return

    saved = os.path.join(outdir, "mixer-before.state")
    subprocess.run(["alsactl", "-f", saved, "store", args.card], check=True)
    old_mode = open(PARAM).read().strip()
    results = []
    try:
        setup(card, target)
        out = target["out"]
        print("   %s master %s, switch %s, loopback %s, PB6 fader %s" % (
            OUTPUTS[out],
            card.cget(OUTPUTS[out] + " Playback Volume", index=out),
            card.cget(OUTPUTS[out] + " Playback Switch", index=out),
            card.cget("Loopback Switch", index=out),
            card.cget("PB6 Playback Volume", index=out * 14 + PB6_SRC)))
        print("\nOn the Babyface, press OUT until %s is selected "
              "(the wheel must control that output)." % target["panel"])
        input("Press Enter when done. ")
        if args.runs in ("trace", "check", "balance"):
            run_trace(card, dev, target, args.seconds, outdir, args.repeats,
                      check=args.runs == "check",
                      balance=args.runs == "balance")
            runs = []
        elif args.runs == "steps":
            run_steps(card, dev, target, args.seconds, outdir,
                      args.repeats)
            runs = []
        for label, mode, what in runs:
            set_mode(mode)
            start_master(card, target)
            print("\n-- %s\n   %s" % (label, what))
            input("   Press Enter, then start within a second "
                  "(%d s recording). " % args.seconds)
            before = card.counters()
            raw = os.path.join(outdir, "run%d.raw" % len(results))
            run(card, dev, args.seconds, raw)
            after = card.counters()
            peaks = channel_peaks(raw)
            print("   capture peaks dBFS: " +
                  " ".join("ch%d %.0f" % (c, p) for c, p in enumerate(peaks)))
            if target["rec"] and max(peaks[c] for c in target["rec"]) < -70:
                print("   WARNING: no tone on ch %s - loopback or routing "
                      "not working, this run is not valid" % (target["rec"],))
            left, right = sorted(load(raw, target["rec"]),
                                 key=lambda ch: -max(abs(v) for v in ch))
            save_wav(raw[:-4] + ".wav", left, right)
            os.remove(raw)
            results.append(report(label, analyse(left), before, after))
    finally:
        set_mode(old_mode)
        subprocess.run(["alsactl", "-f", saved, "restore", args.card])
    print("\nRecordings (%s): %s" % (args.target, outdir))
    print("Mixer restored, wheel_mode back to %s." % old_mode)


if __name__ == "__main__":
    main()
