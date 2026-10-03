# Mute PH3/4 through ALSA, play a tone into PH3/4 (-> patch cable -> IN3/IN4),
# and log the IN3/IN4 level and the driver's PH3/4 mute/master every 0.5 s.
# The user turns the OUT wheel (Phones selected) after ~5 s.
import sys, os, subprocess, struct, math, time, threading
sys.path.insert(0, os.path.expanduser('~/ws/dev/audio/rme/rme-eval-tools/measure'))
from wheel_probe import Card, TARGETS, setup, SOURCES, FADER_0DB, PB6_SRC
card = Card('BabyfacePro'); out = 1
C = subprocess.run(['sh', '-c', "awk '/BabyfacePro/{print $1}' /proc/asound/cards | head -1"], capture_output=True, text=True).stdout.strip()
dev = 'hw:%s,0' % C
SAVED = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'wheelmute-before.state')
NOSETUP = '--no-setup' in sys.argv
if not NOSETUP:
    subprocess.run(['alsactl', '-f', SAVED, 'store', 'BabyfacePro'], check=True)
    setup(card, TARGETS['phones'])
    card.cset(SOURCES[PB6_SRC] + ' Playback Volume', '0,0', index=out * 14 + PB6_SRC)
    card.cset('PB1 Playback Volume', '%d,%d' % (FADER_0DB, FADER_0DB), index=out * 14 + 8)
card.cset('PH3/4 Playback Volume', '819,819', index=out)   # -20 dB
card.cset('PH3/4 Playback Switch', 'off', index=out)       # ALSA mute
R = 48000; CH = 12
amp = (10 ** (-12 / 20)) * 0x7fffff; w = 2 * math.pi * 1000 / R
blk = bytearray()
for f in range(R):
    v = struct.pack('<i', int(round(amp * math.sin(w * f))) << 8)
    blk += v + v + b'\0' * 4 * (CH - 2)
blk = bytes(blk)
play = subprocess.Popen(['aplay', '-q', '-D', dev, '-t', 'raw', '-f', 'S32_LE', '-c', str(CH), '-r', str(R)], stdin=subprocess.PIPE)
stop = threading.Event()
def feed():
    try:
        while not stop.is_set(): play.stdin.write(blk)
    except (BrokenPipeError, ValueError): pass
threading.Thread(target=feed, daemon=True).start()
time.sleep(0.5)
rec = subprocess.Popen(['arecord', '-q', '-D', dev, '-t', 'raw', '-f', 'S32_LE', '-c', str(CH), '-r', str(R), '-d', '61'], stdout=subprocess.PIPE)
fb = CH * 4; half = R // 2 * fb
print("Setup done. Follow the >>> NOW lines. PH3/4 starts muted.", flush=True)
print("t(s)   IN3 dBFS  IN4 dBFS   driver: PH3/4 switch, master   panel wheel counter", flush=True)
t0 = time.time()
for i in range(120):
    d = rec.stdout.read(half)
    if len(d) < half: break
    vals = struct.unpack_from('<%di' % (len(d) // 4), d)
    lv = []
    for c in (2, 3):
        x = vals[c::CH]; ms = sum((q / 2**31) ** 2 for q in x) / len(x)
        lv.append(10 * math.log10(ms) if ms else -180)
    sw = card.cget('PH3/4 Playback Switch', index=out); ms_ = card.cget('PH3/4 Playback Volume', index=out)
    wh = card.cget('Front Panel Wheel')
    note = ''
    if i == 9:
        note = '   >>> NOW: hold SELECT and turn a few clicks either way (balance, muted)'
    elif i == 29:
        note = '   >>> NOW: release SELECT, turn UP about 10 clicks (still muted)'
    elif i == 59:
        card.cset('PH3/4 Playback Switch', 'on', index=out)
        note = '   <- script unmutes PH3/4 here'
    elif i == 69:
        note = '   >>> NOW: turn DOWN about 5 clicks (unmuted)'
    print("%5.1f  %8.1f  %8.1f   %s %-6s %s%s" % ((i + 1) / 2, lv[0], lv[1], 'on ' if sw[0] else 'off', ms_[0], wh[0], note), flush=True)
stop.set()
for proc in (rec, play):
    proc.kill()
    try:
        proc.wait(timeout=3)
    except subprocess.TimeoutExpired:
        pass
subprocess.run(['alsactl', '-f', SAVED, 'restore', 'BabyfacePro'], capture_output=True)
print("mixer restored from", SAVED)
