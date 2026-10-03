#!/usr/bin/env python3
"""session_probe.py - does a session start without the cold init carry audio?

For each session_init mode of the exp/session-start driver, plays a 1 kHz
tone on PB6 into one output and records it back, plus the fixed playback
tap (capture 10/11).

  --target phones   PH3/4 through a patch cable from a headphone jack into
                    IN3: the real analog output.  Every input is removed
                    from every output, so IN3 cannot feed back.
  --target optical  ADAT7/8 through the device loopback (capture 8/9).
                    Carries only a leak of the tone at about -111 dBFS on
                    this unit, not the output level: not a valid check.  Each
measurement is its own session (aplay/arecord open and close the PCM), so
every row is one session start.

Per mode:
  - 48, 44.1, 32, 48 kHz: every family, and a rate change between sessions;
  - the output master moved +10 dB while no session runs, then one more
    session: the change must show up in it;
  - the master moved back, one more session.

Every output other than the target stays muted.  The mixer is restored afterwards.

With the exp/session-start experiment driver, the session_init parameter
must be writable by this user; any other driver gets one pass as loaded:
    sudo chown $USER /sys/module/snd_usb_babyface_pro/parameters/session_init
and the card released by PipeWire (just card-off).

Usage: session_probe.py [--target phones|optical] [--modes 0 1 2 3]
                        [--seconds 2]
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

from wheel_probe import Card, OUTPUTS, TARGETS, setup

PARAM = "/sys/module/snd_usb_babyface_pro/parameters/session_init"
CH = 12
FREQ = 1000.0
TONE_DBFS = -12.0
PB_CH = (10, 11)
REC_CH = (8, 9)
TAP_CH = (10, 11)
MASTER_A = 0x0333         # -20 dB
MASTER_B = 0x0a1e         # -10 dB (0x0333 * 10^(10/20))
RATES = (48000, 44100, 32000, 48000)
REC_FOUND = [None]


def tone_second(rate):
    """One second of tone: 1000 cycles fit any integer rate exactly."""
    amp = (10 ** (TONE_DBFS / 20)) * 0x7fffff
    w = 2 * math.pi * FREQ / rate
    out = bytearray(rate * CH * 4)
    for f in range(rate):
        b = struct.pack("<i", int(round(amp * math.sin(w * f))) << 8)
        base = f * CH * 4
        for c in PB_CH:
            out[base + c * 4:base + c * 4 + 4] = b
    return bytes(out)


def session(dev, rate, seconds, path, block):
    play = subprocess.Popen(["aplay", "-q", "-D", dev, "-t", "raw",
                             "-f", "S32_LE", "-c", str(CH), "-r", str(rate)],
                            stdin=subprocess.PIPE)
    stop = threading.Event()

    def feed():
        try:
            while not stop.is_set():
                play.stdin.write(block)
        except (BrokenPipeError, ValueError):
            pass

    t = threading.Thread(target=feed, daemon=True)
    t.start()
    time.sleep(0.5)
    rec = subprocess.run(["arecord", "-q", "-D", dev, "-t", "raw",
                          "-f", "S32_LE", "-c", str(CH), "-r", str(rate),
                          "-d", str(seconds), path])
    stop.set()
    play.terminate()
    play.wait()
    if rec.returncode:
        raise subprocess.CalledProcessError(rec.returncode, "arecord")


def rms_db(path, rate):
    """RMS per capture channel in dBFS, after the first 0.25 s."""
    with open(path, "rb") as f:
        data = f.read()
    frames = len(data) // (CH * 4)
    vals = struct.unpack_from("<%di" % (frames * CH), data)
    out = []
    for c in range(CH):
        x = vals[c::CH][rate // 4:]
        if not x:
            out.append(-180.0)
            continue
        ms = sum((v / 2 ** 31) ** 2 for v in x) / len(x)
        out.append(10 * math.log10(ms) if ms > 0 else -180.0)
    return out


def kmsg_exp(since):
    out = subprocess.run(["journalctl", "-k", "--no-pager", "-o", "cat",
                          "--since", since], capture_output=True,
                         text=True).stdout
    return [ln for ln in out.splitlines() if "EXP session_init" in ln]



def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--modes", type=int, nargs="+", default=[0, 1, 2, 3])
    ap.add_argument("--seconds", type=int, default=2)
    ap.add_argument("--card", default="BabyfacePro")
    ap.add_argument("--target", choices=("phones", "optical"),
                    default="phones")
    args = ap.parse_args()
    card = Card(args.card)
    dev = "hw:%s,0" % args.card
    has_param = os.path.exists(PARAM)
    if has_param and not os.access(PARAM, os.W_OK):
        sys.exit("%s is not writable: chown it to this user." % PARAM)
    if not has_param:
        # A driver without the experiment parameter: one pass as loaded.
        args.modes = [-1]
    outdir = tempfile.mkdtemp(prefix="session-probe-")
    saved = os.path.join(outdir, "mixer-before.state")
    subprocess.run(["alsactl", "-f", saved, "store", args.card], check=True)
    old_mode = open(PARAM).read().strip() if has_param else None
    target = TARGETS[args.target]
    out = target["out"]
    master = OUTPUTS[out] + " Playback Volume"
    blocks = {r: tone_second(r) for r in set(RATES)}
    rows = []
    try:
        setup(card, target)
        for mode in args.modes:
            if has_param:
                with open(PARAM, "w") as f:
                    f.write(str(mode))
            card.cset(master, "%d,%d" % (MASTER_A, MASTER_A), index=out)
            steps = [(r, "") for r in RATES]
            steps += [(48000, "master +10 dB while idle"),
                      (48000, "master back while idle")]
            for i, (rate, note) in enumerate(steps):
                if note.startswith("master +10"):
                    card.cset(master, "%d,%d" % (MASTER_B, MASTER_B),
                              index=out)
                elif note.startswith("master back"):
                    card.cset(master, "%d,%d" % (MASTER_A, MASTER_A),
                              index=out)
                since = time.strftime("%Y-%m-%d %H:%M:%S")
                path = os.path.join(outdir, "m%d-%d-%d.raw" % (mode, i, rate))
                session(dev, rate, args.seconds, path, blocks[rate])
                db = rms_db(path, rate)
                rec = REC_CH
                if target["rec"] is None:
                    # The patched input: the loudest non-tap channel of
                    # the first session, kept for the rest of the run.
                    if not rows and REC_FOUND[0] is None:
                        best = max((c for c in range(CH) if c not in TAP_CH),
                                   key=lambda c: db[c])
                        REC_FOUND[0] = (best - best % 2, best - best % 2 + 1)
                        print("tone found on capture ch %s" % (REC_FOUND[0],))
                    rec = REC_FOUND[0]
                exp = kmsg_exp(since)
                total = exp[-1].rsplit("total ", 1)[-1] if exp else "?"
                row = (mode, rate, db[rec[0]], db[rec[1]],
                       db[TAP_CH[0]], db[TAP_CH[1]], total, note)
                rows.append(row)
                print("mode %d %6d Hz  output %6.1f %6.1f dBFS  "
                      "tap %6.1f %6.1f dBFS  start %s  %s" % row, flush=True)
                time.sleep(0.3)
    finally:
        if has_param:
            with open(PARAM, "w") as f:
                f.write(old_mode)
        subprocess.run(["alsactl", "-f", saved, "restore", args.card])
    print("\nrecordings and mixer snapshot: %s" % outdir)


if __name__ == "__main__":
    main()
