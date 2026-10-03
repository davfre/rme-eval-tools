#!/usr/bin/env python3
"""Relative DIM test, step 1 of notes/test-plan-relative-dim.md.

Needs the TEST build of feature/relative-dim with BF_DIM_OUT 1, which
dims PH3/4 instead of AN1/2, and PH3/4 cabled into IN3.  Plays a tone on
PB6 routed only into PH3/4, measures IN3, and steps through the plan's
cases: the automated ones first, then the front-panel ones, which ask
for a DIM press or the wheel.  The mixer is saved first and restored at
the end, also on an error or Ctrl-C.

Speakers off, headphones out, TuxMix closed, and the card freed from
PipeWire (just card-off) before starting.

usage: python3 dimtest.py LOGFILE
"""
import math
import os
import subprocess
import sys
import threading
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from wheel_probe import Card, TARGETS, setup, CH, RATE, PB_CH  # noqa: E402

CARD = 'BabyfacePro'
DEV = 'hw:BabyfacePro'
OUT = 1                     # PH3/4
REC_CH = 2                  # IN3
TONE_DBFS = -12.0
V10, V30, V60 = 2591, 259, 8  # 16-bit masters for -10, -30, -60 dB
TOL_DIM, TOL_BACK = 0.3, 0.1  # dB

card = Card(CARD)
log = open(sys.argv[1], 'w')
fails = []


def say(msg=''):
    print(msg, flush=True)
    log.write(msg + '\n')
    log.flush()


def check(name, ok, detail):
    say('  %s  %s: %s' % ('PASS' if ok else 'FAIL', name, detail))
    if not ok:
        fails.append(name)


# Controls.
def vol(v=None):
    if v is not None:
        card.cset('PH3/4 Playback Volume', '%d,%d' % (v, v), index=OUT)
    return card.cget('PH3/4 Playback Volume', index=OUT)[0]


def dim(on=None):
    if on is not None:
        card.cset('Dim Switch', 'on' if on else 'off')
    return card.cget('Dim Switch')[0]


def mute(on):
    card.cset('PH3/4 Playback Switch', 'off,off' if on else 'on,on',
              index=OUT)


def presses():
    return card.cget('DIM Button Press Count')[0]


def action(report_only):
    card.cset('DIM Button Action', 1 if report_only else 0)


# Audio: a tone on PB6 into PH3/4, and IN3 levels in 50 ms blocks.
class Audio:
    def __init__(self):
        n = np.arange(RATE)
        tone = np.round(10 ** (TONE_DBFS / 20) * 0x7fffff *
                        np.sin(2 * np.pi * 997.0 * n / RATE)).astype('<i4')
        frames = np.zeros((RATE, CH), dtype='<i4')
        frames[:, PB_CH[0]] = tone << 8
        frames[:, PB_CH[1]] = tone << 8
        self.second = frames.tobytes()
        self.blocks = []
        self.lock = threading.Lock()
        self.stop = threading.Event()
        fmt = ['-q', '-D', DEV, '-t', 'raw', '-f', 'S32_LE', '-c', str(CH),
               '-r', str(RATE)]
        self.play = subprocess.Popen(['aplay'] + fmt, stdin=subprocess.PIPE)
        threading.Thread(target=self._feed, daemon=True).start()
        time.sleep(0.3)
        self.rec = subprocess.Popen(['arecord'] + fmt, stdout=subprocess.PIPE)
        threading.Thread(target=self._read, daemon=True).start()

    def _feed(self):
        try:
            while not self.stop.is_set():
                self.play.stdin.write(self.second)
        except (BrokenPipeError, ValueError):
            pass

    def _read(self):
        size = RATE // 20 * CH * 4
        while not self.stop.is_set():
            d = self.rec.stdout.read(size)
            if len(d) < size:
                return
            x = np.frombuffer(d, dtype='<i4').reshape(-1, CH)[:, REC_CH]
            p = np.mean((x / 2.0 ** 31) ** 2)
            with self.lock:
                self.blocks.append((time.time(), p))

    def level(self, settle=0.8, span=0.5):
        """IN3 in dBFS, averaged over span seconds after settle."""
        t0 = time.time() + settle
        time.sleep(settle + span + 0.1)
        with self.lock:
            p = [b[1] for b in self.blocks if t0 <= b[0] <= t0 + span]
        if not p:
            raise RuntimeError('no capture blocks: is arecord running?')
        return 10 * math.log10(max(np.mean(p), 1e-20))

    def close(self):
        self.stop.set()
        for proc in (self.play, self.rec):
            proc.kill()
            proc.wait()


def dim_cycle(audio, name, start, expect_drop):
    dim(False)
    mute(False)
    vol(start)
    l0 = audio.level()
    dim(True)
    l1, c1 = audio.level(), vol()
    dim(False)
    l2, c2 = audio.level(), vol()
    say('%s: start %d (%.1f dB): %.2f dBFS, dimmed %.2f (vol %d), '
        'released %.2f (vol %d)' % (name, start, 20 * math.log10(start / 8192),
                                     l0, l1, c1, l2, c2))
    if expect_drop:
        check(name + ' drop', abs(l0 - l1 - 20) <= TOL_DIM,
              '%.2f dB, want 20 +-%.1f' % (l0 - l1, TOL_DIM))
    else:
        say('  INFO  %s drop: %.2f dB (8-bit floor)' % (name, l0 - l1))
    check(name + ' dimmed vol', c1 == round(start / 10),
          '%d, want %d' % (c1, round(start / 10)))
    check(name + ' return', abs(l2 - l0) <= (TOL_BACK if expect_drop else 0.5),
          '%+.2f dB' % (l2 - l0))
    check(name + ' restored vol', c2 == start, '%d, want %d' % (c2, start))
    return l0


def automated(audio, floor):
    l0 = dim_cycle(audio, '1a', V10, True)
    dim_cycle(audio, '1b', V30, True)
    dim_cycle(audio, '1c', V60, False)

    # 1f: a change while dimmed moves the dimmed level only.
    vol(V10)
    dim(True)
    vol(2 * round(V10 / 10))                    # dimmed level +6 dB
    l1 = audio.level()
    dim(False)
    l2, c2 = audio.level(), vol()
    say('1f: dimmed +6 dB from amixer %.2f, released %.2f (vol %d)'
        % (l1, l2, c2))
    check('1f raised while dimmed', abs(l0 - l1 - 14) <= TOL_DIM,
          '%.2f below start, want 14' % (l0 - l1))
    check('1f restores the level from before DIM', c2 == V10 and
          abs(l2 - l0) <= TOL_BACK, 'vol %d, %+.2f dB' % (c2, l2 - l0))

    # 1g: dimmed, mute, DIM off, unmute.
    vol(V10)
    dim(True)
    mute(True)
    lm = audio.level()
    dim(False)
    lm2, c2 = audio.level(), vol()
    mute(False)
    l3 = audio.level()
    say('1g: muted %.2f, DIM off while muted %.2f (vol %d), unmuted %.2f'
        % (lm, lm2, c2, l3))
    check('1g silent while muted', max(lm, lm2) <= floor + 3,
          'max %.2f, floor %.2f' % (max(lm, lm2), floor))
    check('1g unmutes to the level from before DIM', c2 == V10 and
          abs(l3 - l0) <= TOL_BACK, 'vol %d, %+.2f dB' % (c2, l3 - l0))

    # 1h: muted, DIM on, unmute, DIM off.
    vol(V10)
    mute(True)
    dim(True)
    lm = audio.level()
    mute(False)
    l1, c1 = audio.level(), vol()
    dim(False)
    l2, c2 = audio.level(), vol()
    say('1h: muted+dimmed %.2f, unmuted %.2f (vol %d), released %.2f '
        '(vol %d)' % (lm, l1, c1, l2, c2))
    check('1h silent while muted', lm <= floor + 3, '%.2f' % lm)
    check('1h unmutes dimmed', abs(l0 - l1 - 20) <= TOL_DIM and
          c1 == round(V10 / 10), '%.2f below start, vol %d' % (l0 - l1, c1))
    check('1h release', c2 == V10 and abs(l2 - l0) <= TOL_BACK,
          'vol %d, %+.2f dB' % (c2, l2 - l0))
    return l0


def ask(prompt):
    say('>>> ' + prompt)
    reply = input('    [Enter when done] ')
    log.write('    reply: %r\n' % reply)
    return reply


def yes(prompt):
    reply = input('>>> %s [y/n] ' % prompt).strip().lower()
    log.write('>>> %s: %r\n' % (prompt, reply))
    return reply.startswith('y')


def manual(audio, l0):
    # 1d: front-panel presses.
    vol(V10)
    dim(False)
    n0 = presses()
    ask('Press DIM on the unit ONCE.')
    l1, d1, n1 = audio.level(), dim(), presses()
    lit = yes('Are the DIM LEDs lit?')
    ask('Press DIM on the unit ONCE more.')
    l2, d2, n2 = audio.level(), dim(), presses()
    unlit = yes('Are the DIM LEDs off now?')
    say('1d: after press 1 %.2f (Dim %d, count +%d), after press 2 %.2f '
        '(Dim %d, count +%d)' % (l1, d1, n1 - n0, l2, d2, n2 - n0))
    check('1d press dims', d1 == 1 and abs(l0 - l1 - 20) <= TOL_DIM and
          n1 - n0 == 1, '%.2f dB down' % (l0 - l1))
    check('1d press releases', d2 == 0 and abs(l2 - l0) <= TOL_BACK and
          n2 - n0 == 2, '%+.2f dB' % (l2 - l0))
    check('1d LEDs', lit and unlit, 'lit %s, off %s' % (lit, unlit))

    # 1e: the wheel while dimmed.
    ask('Press OUT until the wheel controls Phones (PH3/4).')
    vol(V10)
    dim(True)
    l1 = audio.level()
    ask('Turn the wheel UP 5 clicks, slowly.')
    l2, c2 = audio.level(), vol()
    dim(False)
    l3, c3 = audio.level(), vol()
    say('1e: dimmed %.2f, after the wheel %.2f (vol %d), released %.2f '
        '(vol %d)' % (l1, l2, c2, l3, c3))
    check('1e wheel moved the dimmed level', l2 > l1 + 1,
          '%+.2f dB' % (l2 - l1))
    check('1e restores the level from before DIM', c3 == V10 and
          abs(l3 - l0) <= TOL_BACK and l3 <= l0 + TOL_BACK,
          'vol %d, %+.2f dB' % (c3, l3 - l0))

    # 1i: Report Only.
    vol(V10)
    action(True)
    n0 = presses()
    ask('Press DIM on the unit ONCE (Report Only: nothing should change).')
    l1, d1, n1 = audio.level(), dim(), presses()
    unlit = yes('Are the DIM LEDs still off?')
    action(False)
    say('1i: %.2f (Dim %d, count +%d)' % (l1, d1, n1 - n0))
    check('1i Report Only', d1 == 0 and n1 - n0 == 1 and unlit and
          abs(l1 - l0) <= TOL_BACK, '%+.2f dB' % (l1 - l0))


def main():
    saved = sys.argv[1] + '.mixer-before.state'
    subprocess.run(['alsactl', '-f', saved, 'store', CARD], check=True)
    say('Saved mixer: ' + saved)
    audio = None
    try:
        vol(V30)                    # quiet before setup unmutes PH3/4
        dim(False)
        setup(card, TARGETS['phones'])
        mute(False)
        audio = Audio()
        mute(True)
        floor = audio.level()
        mute(False)
        say('IN3 noise floor with PH3/4 muted: %.2f dBFS' % floor)
        l0 = automated(audio, floor)
        manual(audio, l0)
    finally:
        if audio:
            audio.close()
        mute(True)
        subprocess.run(['alsactl', '-f', saved, 'restore', CARD])
        say('Mixer restored.')
    say('RESULT: %d failed%s' % (len(fails), (': ' + ', '.join(fails))
                                 if fails else ''))


if __name__ == '__main__':
    main()
