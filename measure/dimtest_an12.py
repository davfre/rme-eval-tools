#!/usr/bin/env python3
"""Relative DIM test, step 2 of notes/test-plan-relative-dim.md.

The real feature/relative-dim build, which dims AN1/2.  Nothing measures
the AN1/2 output here, so every source into AN1/2 is cleared first and
the output carries nothing.  Speakers off all the same.  The script
toggles DIM through the control and then asks for two front-panel
presses, logging the controls; a usbmon capture taken alongside shows
the writes.  The mixer is saved first and restored at the end.

usage: python3 dimtest_an12.py LOGFILE
"""
import subprocess
import sys
import time

sys.path.insert(0, __file__.rsplit('/', 1)[0])
from wheel_probe import Card, SOURCES  # noqa: E402

CARD = 'BabyfacePro'
V10 = 2591
card = Card(CARD)
log = open(sys.argv[1], 'w')
fails = []


def say(msg):
    print(msg, flush=True)
    log.write(msg + '\n')
    log.flush()


def check(name, ok, detail):
    say('  %s  %s: %s' % ('PASS' if ok else 'FAIL', name, detail))
    if not ok:
        fails.append(name)


def state():
    return {'vol': card.cget('AN1/2 Playback Volume')[0],
            'dim': card.cget('Dim Switch')[0],
            'unit': card.cget('Front Panel Dim')[0],
            'count': card.cget('DIM Button Press Count')[0]}


def mark(tag):
    # A recognisable read in the usbmon capture is not possible from
    # here, so the log keeps the wall time of each step instead.
    say('%.3f %s' % (time.time(), tag))


def main():
    saved = sys.argv[1] + '.mixer-before.state'
    subprocess.run(['alsactl', '-f', saved, 'store', CARD], check=True)
    try:
        card.cset('Dim Switch', 'off')
        card.cset('AN1/2 Playback Switch', 'off,off')
        for src in range(14):
            card.cset(SOURCES[src] + ' Playback Volume', '0,0', index=src)
        card.cset('AN1/2 Playback Volume', '%d,%d' % (V10, V10))
        card.cset('AN1/2 Playback Switch', 'on,on')
        time.sleep(1)
        s0 = state()
        say('start: %s' % s0)

        mark('A: Dim Switch on')
        card.cset('Dim Switch', 'on')
        time.sleep(1)
        s1 = state()
        say('dimmed: %s' % s1)
        check('A dimmed', s1['dim'] == 1 and s1['vol'] == round(V10 / 10)
              and s1['unit'] == 1, str(s1))

        mark('B: Dim Switch off')
        card.cset('Dim Switch', 'off')
        time.sleep(1)
        s2 = state()
        say('released: %s' % s2)
        check('B released', s2['dim'] == 0 and s2['vol'] == V10 and
              s2['unit'] == 0, str(s2))

        mark('C: waiting for a front-panel press')
        input('>>> Press DIM on the unit ONCE, then Enter ')
        time.sleep(0.5)
        s3 = state()
        lit = input('>>> DIM LEDs lit? [y/n] ').strip().lower().startswith('y')
        say('after press 1: %s, LEDs lit: %s' % (s3, lit))
        check('C press dims', s3['dim'] == 1 and s3['vol'] == round(V10 / 10)
              and s3['unit'] == 1 and s3['count'] == s0['count'] + 1 and lit,
              str(s3))

        mark('D: waiting for a second press')
        input('>>> Press DIM on the unit ONCE more, then Enter ')
        time.sleep(0.5)
        s4 = state()
        off = input('>>> DIM LEDs off? [y/n] ').strip().lower().startswith('y')
        say('after press 2: %s, LEDs off: %s' % (s4, off))
        check('D press releases', s4['dim'] == 0 and s4['vol'] == V10 and
              s4['unit'] == 0 and s4['count'] == s0['count'] + 2 and off,
              str(s4))
        mark('E: done')
    finally:
        card.cset('AN1/2 Playback Switch', 'off,off')
        subprocess.run(['alsactl', '-f', saved, 'restore', CARD])
        say('Mixer restored.')
    say('RESULT: %d failed%s' % (len(fails), (': ' + ', '.join(fails))
                                 if fails else ''))


if __name__ == '__main__':
    main()
