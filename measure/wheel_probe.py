#!/usr/bin/env python3
"""wheel_probe.py - measure what the front-panel OUT wheel does to the audio.

Plays a steady 997 Hz tone into the optical output (ADAT7/8) only,
records that output back through the device loopback, and asks you to
turn the wheel with the panel's OUT selection on Opt.  Nothing reaches
the analog outputs: AN1/2, PH3/4 and the other digital outputs are
muted for the duration and the mixer is restored afterwards.

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

Usage: wheel_probe.py [--card BabyfacePro] [--out DIR] [--seconds 12]
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
OUT = 5                   # ADAT7/8
REC_CH = (8, 9)           # loopback of out 5 = words 10/11 = capture ch 8/9
PB6_SRC = 13
FADER_0DB = 0x16a0
MASTER_START = 0x0333     # -20 dB, room to turn both ways
PARAM = "/sys/module/snd_usb_babyface_pro/parameters/wheel_mode"

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
                return [int(v) for v in line.split("=", 1)[1].split(",")]
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


def setup(card):
    # Silence everything except the optical output.
    for i, name in enumerate(("AN1/2", "PH3/4", "AS1/2", "ADAT3/4",
                              "ADAT5/6")):
        card.cset(name + " Playback Switch", "off", index=i)
    card.cset("ADAT7/8 Playback Switch", "on", index=OUT)
    # Only PB6 into the optical output, at 0 dB.
    for src in range(14):
        names = ("AN1", "AN2", "AN3", "AN4", "AS1/2", "ADAT3/4", "ADAT5/6",
                 "ADAT7/8", "PB1", "PB2", "PB3", "PB4", "PB5", "PB6")
        val = FADER_0DB if src == PB6_SRC else 0
        card.cset(names[src] + " Playback Volume", "%d,%d" % (val, val),
                  index=OUT * 14 + src)
    for i in range(6):
        card.cset("Loopback Switch", "on" if i == OUT else "off", index=i)


def start_master(card):
    card.cset("ADAT7/8 Playback Volume",
              "%d,%d" % (MASTER_START, MASTER_START), index=OUT)


def tone_second():
    """One second of tone; 997 Hz is exactly 997 cycles per second at
    48 kHz, so the block repeats without a seam."""
    amp = (10 ** (TONE_DBFS / 20)) * 0x7fffff
    w = 2 * math.pi * FREQ / RATE
    out = bytearray(RATE * CH * 4)
    for f in range(RATE):
        b = struct.pack("<i", int(round(amp * math.sin(w * f))) << 8)
        base = f * CH * 4
        for c in PB_CH:
            out[base + c * 4:base + c * 4 + 4] = b
    return bytes(out)


def run(card, dev, seconds, path):
    """Play the tone and record `seconds` of all capture channels."""
    play = subprocess.Popen(["aplay", "-q", "-D", dev, "-t", "raw",
                             "-f", "S32_LE", "-c", str(CH), "-r", str(RATE)],
                            stdin=subprocess.PIPE)
    stop = threading.Event()
    block = tone_second()

    def feed():
        try:
            while not stop.is_set():
                play.stdin.write(block)
        except (BrokenPipeError, ValueError):
            pass

    t = threading.Thread(target=feed, daemon=True)
    t.start()
    time.sleep(0.8)
    subprocess.run(["arecord", "-q", "-D", dev, "-t", "raw", "-f", "S32_LE",
                    "-c", str(CH), "-r", str(RATE), "-d", str(seconds), path],
                   check=True)
    stop.set()
    play.terminate()
    play.wait()


def load(path, ch):
    with open(path, "rb") as f:
        data = f.read()
    frames = len(data) // (CH * 4)
    vals = struct.unpack_from("<%di" % (frames * CH), data)
    return [v / 2147483648.0 for v in vals[ch::CH]]


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


RUNS = [
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


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--card", default="BabyfacePro")
    ap.add_argument("--out", default=None, help="keep recordings here")
    ap.add_argument("--seconds", type=int, default=12)
    args = ap.parse_args()
    card = Card(args.card)
    dev = "hw:%s,0" % args.card
    outdir = args.out or tempfile.mkdtemp(prefix="wheel-probe-")
    os.makedirs(outdir, exist_ok=True)

    if card.counters() is None:
        sys.exit("No 'Debug Stream Counters' control: load the wheel-debug "
                 "driver first.")
    if not os.path.exists(PARAM):
        sys.exit("No wheel_mode parameter: load the wheel-debug driver first.")
    print("Nothing may be connected to the optical output. The analog "
          "outputs stay muted.")
    if input("Type yes to continue: ").strip() != "yes":
        return

    saved = os.path.join(outdir, "mixer-before.state")
    subprocess.run(["alsactl", "-f", saved, "store", args.card], check=True)
    old_mode = open(PARAM).read().strip()
    results = []
    try:
        setup(card)
        print("\nOn the Babyface, press OUT until Opt is selected "
              "(the wheel must control the optical output).")
        input("Press Enter when done. ")
        for label, mode, what in RUNS:
            set_mode(mode)
            start_master(card)
            print("\n-- %s\n   %s" % (label, what))
            input("   Press Enter, then start within a second "
                  "(%d s recording). " % args.seconds)
            before = card.counters()
            raw = os.path.join(outdir, "run%d.raw" % len(results))
            run(card, dev, args.seconds, raw)
            after = card.counters()
            left, right = load(raw, REC_CH[0]), load(raw, REC_CH[1])
            save_wav(raw[:-4] + ".wav", left, right)
            os.remove(raw)
            results.append(report(label, analyse(left), before, after))
    finally:
        set_mode(old_mode)
        subprocess.run(["alsactl", "-f", saved, "restore", args.card])
    print("\nRecordings (loopback of the optical output): %s" % outdir)
    print("Mixer restored, wheel_mode back to %s." % old_mode)


if __name__ == "__main__":
    main()
