# The three broken rates — analysis before testing

2026-09-15. Written after the maintainer's hardware results on PR #7
(ismail-bahloul/babyface-pro-linux#7). Nothing here is confirmed; it is a
reading of the evidence already in the tree, to be checked before it goes
anywhere near the PR thread.

## The result grid

Maintainer's wall-clock measurements, all nine rates, arranged by the PR's
own model (family register 0x0030 = base, SET_INTERFACE alt = multiplier):

|            | alt1 (x1) | alt2 (x2)  | alt3 (x4)    |
|------------|-----------|------------|--------------|
| **0x0000** | 32 ok     | 64 -> 48   | 128 -> 96    |
| **0x0010** | 44.1 ok   | 88.2 ok    | 176.4 -> 192 |
| **0x0020** | 48 ok     | 96 ok      | 192 ok       |

The failures form a lower-left triangle, and every one of them lands on a
**48 kHz-family rate**: 48, 96, 192. That is systematic, not three unrelated
bugs. Whatever happens, the device ends up on base 0x0020.

Per-failure detail:

- 64 kHz (0x0000, alt2) -> 48 kHz = 0x0020 at alt1. Neither the base nor the
  multiplier took.
- 128 kHz (0x0000, alt3) -> 96 kHz = 0x0020 at alt2. Base didn't take, alt came
  back one step.
- 176.4 kHz (0x0010, alt3) -> 192 kHz = 0x0020 at alt3. Alt took; base didn't.

So 176.4 is a different failure from 64/128, and should be chased separately.

## The contradiction with PROTOCOL.md

`babyface-pro-linux/tools/usbdump/PROTOCOL.md`, section "Sample rate / clock
(cap_rates.pcap + cap_rates2.pcap, 2026-08-22)", says two things that the PR's
model cannot both accommodate.

**1. The alt is a bandwidth class, not a multiplier.** Hardware-verified table
from the 9-rate sweep:

| alt | rates | packet | frame |
|-----|-------|--------|-------|
| 1 | 32 / 44.1 / 48 / **64** / **88.2** | 448 B | 14 ch x 4 = 56 B |
| 2 | 96 / **128** | 640 B | 10 ch x 4 = 40 B |
| 3 | 176.4 / 192 | 1024 B | 8 ch x 4 = 32 B |

with the rule "alt = smallest packet size where 8 x size >= rate x ch x 4 B/ms
(14ch <=64k, 10ch at 88.2-128k, 8ch at 176.4/192k)". 64 kHz and 88.2 kHz sit in
the *same* class as 32/44.1/48 — impossible if the alt were the x1/x2/x4
selector. The PR regroups them (alt1 = 32/44.1/48, alt2 = 64/88.2/96,
alt3 = 128/176.4/192), which moves 64 and 128 out of their captured classes.

Two of the three broken rates are exactly rates whose alt the PR moved. 88.2 was
moved too and still works — but it moved *up* a class (448 -> 640 B), which is
bandwidth-wise harmless, whereas 64 and 128 also moved up and broke. So the alt
regrouping is a strong lead, not a complete explanation.

**2. PROTOCOL.md claims 0x0030 is not a rate selector at all**: "Traffic around
each change: `0x10 wVal=0x0010/0x0020/0x0000 wIdx=0x0030` on a ~7-15 s cycle
(phase counter, NOT rate-specific — the wVal cycles 0x0010 -> 0x0020 -> 0x0000
regardless)", and "A sample-rate change is PURELY SET_INTERFACE(5, alt N) — no
vendor writes at all".

That reading is now falsified for 32/44.1/48: writing the register fixed a real,
reproducible 44.1 kHz bug on hardware. The earlier analysis most likely mistook a
rate selector for a counter because the sweep it was based on was monotonic —
which is precisely why `research/babyface-clock-capture-prep-20260914` specifies
a deliberately non-monotonic order (48 -> 32 -> 44.1 -> 48 -> 44.1 -> 32 -> 48 -> ...)
to tell a selector from a periodic counter. **PROTOCOL.md's section needs
correcting either way**, and the correction should say what the register does at
the other six rates, which is exactly what is unknown.

## What is missing

**The 2026-09-14 capture is not in the tree.** `research/babyface-clock-capture-
prep-20260914/Archive.zip` contains only the three preparation scripts; the only
.pcap files anywhere under `rme/` are the old gain captures in
`_archive/babyface-pro-linux/nonfs/captures/`. The commit message for the rate
fix cites "the Windows 2026-09-14 capture", so it exists somewhere off-tree. It
has to come back before the "maybe there's more in your capture" question can be
answered, and the rate-events CSV with it, since the correlation is what makes
the capture readable.

## Measured, 2026-09-15 (first `ratesweep sweep` run)

```
32k    base=0x0000 alt1   2684 kB/s -> 47923 Hz @56B   0x11=2  MISMATCH
44.1k  base=0x0010 alt1   2684 kB/s -> 47923 Hz @56B   0x11=2  MISMATCH
48k    base=0x0020 alt1   2684 kB/s -> 47923 Hz @56B   0x11=2  OK
64k    base=0x0000 alt2   3838 kB/s -> 95949 Hz @40B   0x11=6  MISMATCH
88.2k  base=0x0010 alt2   3838 kB/s -> 95949 Hz @40B   0x11=6  MISMATCH
96k    base=0x0020 alt2   3838 kB/s -> 95949 Hz @40B   0x11=6  OK
128k   base=0x0000 alt3   6141 kB/s -> 191898 Hz @32B  0x11=A  MISMATCH
176.4k base=0x0010 alt3   6141 kB/s -> 191898 Hz @32B  0x11=A  MISMATCH
192k   base=0x0020 alt3   6141 kB/s -> 191898 Hz @32B  0x11=A  OK
```

Three things, all of them useful:

1. **The base register had no effect at all.** Within each alt, all three
   base values gave a byte rate identical to four significant figures. The
   device ran at 48 / 96 / 192 kHz and nothing else.
2. **The alt IS the x1/x2/x4 multiplier**, measured directly: 48, 96, 192 at
   alt1/2/3 on one unchanging base. PROTOCOL.md's "the alt is a BANDWIDTH
   class, alt1 = 32/44.1/48/64/88.2" is wrong — it was inferred from counting
   SET_INTERFACE transitions in a sweep, never measured per rate. The PR's
   regrouping is right and the old table is the thing to correct.
3. **The 0x11 readback tracked the alt every time** (2 / 6 / A = 2^alt plus
   the poll counter), so the alt always took. Nothing is being rejected.

**But this run did not reproduce the driver's order.** It ran cold-init and
then SET_INTERFACE; the driver does SET_INTERFACE in `hw_params()` and the
cold-init (with `bf_clock_write` inside it) afterwards, from `stream_work`.
`sweep` now matches the driver and has to be re-run before "the base register
is inert" can stand — upstream measured 32 and 44.1 kHz as correct on the real
driver, and this run says they cannot be.

## The DDS quad is the better candidate

PROTOCOL.md's pitch section is fully decoded and says the 0x1B quad **is** the
device clock: "PITCH SHIFTS THE SAMPLE RATE DYNAMICALLY ... the stream rate
changes WITHOUT any SET_INTERFACE", `DDS_24 = round(50000 x 256 / (1 + p/100))`,
bank1 = `round(DDS_16 x 0.72562)`, bank2 = `round(DDS_16 x 2/3)`, bank3 =
`0x7CFF`, each quad committed by `0x10 0x0001 0x05CF`. Synthesising the quad for
48 kHz from that formula reproduces the cold-init's own verbatim quad:

| bank | synthesised | captured |
|------|-------------|----------|
| 0 | 0xC350 / 0x00 | 0xC350 / 0x00 |
| 1 | 0x8DB9 | 0x8DB8 |
| 2 | 0x8235 | 0x8234 |
| 3 | 0x7CFF | 0x7CFF |

(+-1 on banks 1/2, exactly the tolerance PROTOCOL.md quotes, and
`pitchformula.c` already proved the bank 1-3 fractions do not matter.)

So the model that fits the measurement is **base rate = the DDS quad,
multiplier = the alt** — the PR's 3 x 3 structure with a different selector.
The driver sends the 48 kHz quad unconditionally, which is exactly the 48 /
96 / 192 that came out. `ratesweep dds` tests it: synthesise the quad for
32 / 44.1 / 48 kHz and sweep the alts. If all nine cells hit, the fix is to
make the cold-init quad rate-dependent and drop the 0x0030 write.

## Measured, run 2: the DDS is the clock

`sweep` in the driver's own order (SET_INTERFACE, then cold-init with the base
write) gave **byte-for-byte the same numbers** as the first run: 48 / 96 / 192 kHz
at alt1/2/3, base register ignored. So the order is not the issue and
`0x10 wVal wIdx=0x0030` does nothing on its own, at any alt.

`dds` moved the clock. Measured at alt1, against the 48 kHz quad:

| quad written | measured | ratio |
|---|---|---|
| "32 kHz" | 55911 Hz | x1.1665 |
| "44.1 kHz" | 52232 Hz | x1.0898 |
| 48 kHz (verbatim) | 47929 Hz | x1.0000 |

The rate went **up** where I expected it to go down, by very close to the
reciprocal of what I asked for. That is the pitch formula being a *period*:
`DDS_24 = round(50000 x 256 / (1 + p/100))` divides, so a smaller DDS is a
higher rate. Corrected:

```
DDS_24(R) = round(12800000 x 48000 / R)
```

For 44.1 kHz that predicts x1.0884 and measured x1.0898 — 0.13% out, which
settles it: **the 0x1B quad is the sample clock.** The 32 kHz row does not
follow the law (predicted x1.5, measured x1.1665), consistent with being far
outside the +-5% range the pitch sweep validated, where `bank3` is quantised and
was left at its 0% value.

### The constraint that reframes everything

bank0's integer part is a 16-bit `wValue`, so `DDS_16 <= 65535`:

```
48000 -> DDS_16 = 50000
44100 -> DDS_16 = 54421
36621 -> DDS_16 = 65535   <- the floor
32000 -> DDS_16 = 75000   <- unreachable
```

**32 kHz cannot come from this quad at all.** So the 3 x 3 grid is not
DDS x alt: the DDS can produce the 44.1 and 48 families, and something else has
to produce the 32 kHz family. Which finally gives `0x0030` a plausible job —
a coarse family or pre-divider that only does anything once a real quad is in
play. `ratesweep combo` tests exactly that, and `ratesweep ddsscan` maps the
DDS -> rate curve so the working range and the clamp points are measured rather
than assumed.

Working model, to be confirmed:

- **alt** = x1 / x2 / x4. Measured, certain.
- **0x1B quad** = the base rate, over roughly 36.6 kHz and up. Measured, certain
  near 48 kHz.
- **0x0030** = coarse family, needed for 32 kHz and inert on its own. Untested.

## Measured, run 3: the model holds

`dds`, with the formula the right way up — six of six, exact:

```
dds=44100 alt1 ->  44032 Hz   OK      dds=48000 alt1 ->  47923 Hz   OK
dds=44100 alt2 ->  88166 Hz   OK      dds=48000 alt2 ->  95949 Hz   OK
dds=44100 alt3 -> 176333 Hz   OK      dds=48000 alt3 -> 191898 Hz   OK
```

`combo` — the 0x0030 register is **inert**. All three values, against both
quads, produced byte-identical rates. It is not a coarse family selector, not
a pre-divider, not anything. It comes out of the driver.

`ddsscan` — the law is exact across the whole range tested, 13 points from
DDS_16 = 40000 to 64000 (59904 Hz down to 37478 Hz), every one within ~0.15%
of `48000 x 50000 / DDS_16`. No clamping, no quantisation steps.

### Eight of the nine rates fall out

| rate | DDS base | alt | DDS_16 | |
|---|---|---|---|---|
| 32000 | 32000 | 1 | 75000 | **out of range** |
| 44100 | 44100 | 1 | 54421 | |
| 48000 | 48000 | 1 | 50000 | |
| 64000 | 64000 | 1 | 37500 | |
| 88200 | 44100 | 2 | 54421 | |
| 96000 | 48000 | 2 | 50000 | |
| 128000 | 64000 | 2 | 37500 | |
| 176400 | 44100 | 3 | 54421 | |
| 192000 | 48000 | 3 | 50000 | |

Three bases, three alts. 64 kHz is not "32 kHz doubled" — it is its own base at
alt1, and 128 kHz is that same base at alt2. Which also reconciles the two alt
tables that seemed to contradict each other: 64 kHz x 56 B = 3584 kB/s, exactly
alt1's 448-byte packet ceiling, so the old capture was right to see 64 kHz on
alt1, and right for the bandwidth reason it gave.

### 32 kHz is the one hole

bank0's integer part is a 16-bit `wValue`; 32 kHz needs 75000. The remaining
candidate is **bank3**, the one bank PROTOCOL.md could not derive: "QUANTIZED in
steps (sticks at values like 0x7C01 over DDS ranges) - bank3 needs a lookup
table from the sweep". Quantised steps over ranges is what a coarse octave
selector looks like. `ratesweep bank3` sweeps it from the 64 kHz base, looking
for anything that halves the clock.

If nothing does, 32 kHz may simply not be reachable in proprietary mode with
this quad format, and the honest move is to drop it from the driver's rate list
rather than advertise a rate that silently runs at another.

### The driver already has this arithmetic

`babyface_restore_state()` and the pitch control both synthesise the quad
already, from `DDS_24 = round(12800000000 / (1000 + p))` — p in 0.1% steps, and
p = 0 gives exactly the 48 kHz quad. The rate fix is that same expression with
the base folded in:

```
DDS_24 = round(12800000000 x 48000 / (base x (1000 + pitch)))
```

So rate and pitch stop being separate mechanisms: they are one register, and
each has to re-apply when the other changes. That is a smaller patch than PR #7
already is, and it deletes `BF_REG_RATE_FAMILY`, `bf_clock_write`'s register
write and the `family` field rather than adding anything.

## Measured, run 4: all nine rates

```
  32000 = base  32000 x alt1    31949 Hz   OK
  44100 = base  44100 x alt1    44032 Hz   OK
  48000 = base  48000 x alt1    47923 Hz   OK
  64000 = base  64000 x alt1    63898 Hz   OK
  88200 = base  44100 x alt2    88166 Hz   OK
  96000 = base  48000 x alt2    95949 Hz   OK
 128000 = base  64000 x alt2   128000 Hz   OK
 176400 = base  44100 x alt3   176333 Hz   OK
 192000 = base  48000 x alt3   192000 Hz   OK
```

**32 kHz works, and my "out of range" call was wrong.** Its DDS integer part is
75000, which does not fit bank0's 16-bit `wValue`; the harness wrote the low
word (0x24F8) anyway, and the device still clocked 32 kHz exactly. Banks 1 and 2
are derived from the untruncated 75000 and both stay in range (0xD496, 0xC350),
and rate x bank2 is constant at 1.6e9 across every rate measured. So the device
takes the rate from banks 1/2, not bank0 — bank0's wrap is harmless.

`bank3` is inert: 22 values, including 0x0000, 0xFFFF, half and double of
0x7CFF, and a high-byte scan, all produced 63898 Hz to the last digit. It stays
at its 0 % value.

So the complete picture, all of it measured:

- **alt** = x1 / x2 / x4.
- **0x1B quad** = the base rate, `DDS_24 = round(12800000000 x 48000 / (base x (1000 + pitch)))`.
- **bank3** = irrelevant to the rate.
- **0x0030** = does nothing.

Four bases (32/44.1/48/64 kHz) at x1, three of them again at x2, two at x4.

## The patch

Written into the tree (uncommitted), `tools/kernel/`:

- `bf_rates[]` carries a `base` instead of a `family`, and 64 kHz moves to alt 1,
  128 kHz to alt 2 — where 64000 x 56 B = 3584 kB/s sits exactly on alt 1's
  448-byte ceiling, which is why the old capture saw it there.
- `bf_clock_write()` synthesises the quad from base and pitch and drops the
  0x0030 write; `BF_REG_RATE_FAMILY` and the `family` field are gone.
- `bf_cold_init()` no longer hardcodes the 48 kHz quad — at 48 kHz with pitch 0
  the synthesised quad is that quad.
- `babyface_restore_state()` and `bf_pitch_put()` both defer to
  `bf_clock_write()`, so rate and pitch stop being two mechanisms fighting over
  one register.

The kernel's integer arithmetic reproduces the exact quads `ratesweep` sent for
all nine rates (checked off-target; bank1/bank2 land +-1 from the captured
48 kHz quad, the tolerance PROTOCOL.md already documents and which measured
correct). It has not been built against a real kernel or run on hardware.

## Verified through the driver, 2026-09-15

Patched module loaded, card freed from PipeWire, `ratecheck`:

```
  requested  measured     error
  32000      32001         +0.002 %   ok
  44100      44100         +0.001 %   ok
  48000      48001         +0.002 %   ok
  64000      63974         -0.041 %   ok
  88200      88201         +0.001 %   ok
  96000      96002         +0.002 %   ok
  128000     128002        +0.002 %   ok
  176400     176401        +0.001 %   ok
  192000     192012        +0.006 %   ok
```

Re-run after the 3 x 3 table fix, with the card suspended: 9/9, worst error
0.005 %, and 9/9 again at +5 % varispeed. The two layers agree at every rate.

An earlier run had 44.1 kHz reading exactly 48000 Hz. That was PipeWire, not the
driver: `ratecheck` holds the only capture substream, but a playback open at
48 kHz still retunes the shared clock through `hw_params`. The clock steal,
caught in the wild. It is the case the rate-lock follow-up fixes.

64 kHz is the largest error and the one to keep an eye on - at alt 1 it sits
exactly on the 448-byte packet ceiling (64000 x 56 B = 3584 kB/s), with no
headroom.

### The first ratecheck run was wrong, and worth recording why

It reported every rate about 2.34 % low. That was the measurement, not the
driver: `snd_pcm_readi` only wakes on period boundaries, so "read exactly R
frames and time it" ends somewhere inside a period rather than on the frame
asked for. At a 1024-frame period that is 21-24 ms - a constant time offset,
which shows up as the same percentage error at every rate. The giveaway was
that the implied offset was ~24 ms at 32 kHz and at 192 kHz alike.

Fixed by putting both endpoints on read completions and counting the frames
actually returned in between, so the interval covers exactly those frames.

44.1 kHz was a separate outlier at 114 ms, about five periods - an xrun, which
loses an unknown number of frames and silently drags the number down. The tool
now detects a recover inside the window and retries rather than reporting it as
a clock error.

## The varispeed test caught a table bug the rate sweep could not

With the patch loaded, `Varispeed Pitch = 50` (+5 %) and `ratecheck`: seven
rates read +5.002 %, but 64 kHz and 128 kHz read **-5.001 %** - exactly
mirrored, and both from the same DDS base.

The measured 60799 Hz implies DDS_int = 39474, which is precisely the
pitch **-50** quad (39473). So the device acted on a different quad than the
one intended, rather than responding oddly to the right one.

The cause was the first version of this table putting 64 kHz at base 64000 on
alt 1, where `64000 x 56 B = 3584 kB/s` is **exactly** alt 1's 448-byte packet
ceiling. Zero headroom, so any positive varispeed runs off the end of the bus.
Same for 128 kHz on alt 2.

The fix is a clean 3 x 3 - three DDS bases, three interface speeds:

| base | x1 (alt1) | x2 (alt2) | x4 (alt3) |
|---|---|---|---|
| 32000 | 32000 | **64000** | **128000** |
| 44100 | 44100 | 88200 | 176400 |
| 48000 | 48000 | 96000 | 192000 |

At x2 the frame is 40 B, so 64 kHz costs 2560 kB/s against a 5120 kB/s ceiling;
128 kHz at x4 costs 4096 against 8192. Room for varispeed at both. This is also
PR #7's *original* alt grouping - the only thing that PR had wrong was the
selector, not the speed classes.

Confirmed on the wire, all nine rates on the 3 x 3 table, and varispeed at
-5/0/+5 % tracking correctly at all three bases with no sign flip.

**Worth keeping in the writeup:** the nine-rate sweep could not have found this.
At pitch 0 the broken entries measure correct - 64 kHz passed at -0.041 %, the
largest error in the table but still inside tolerance. Sitting exactly on a
limit looks fine until something nudges it.

## Two tools, two layers

`rme-eval-tools/measure/ratesweep.c` measures the **device**, over usbdevfs, with the
driver detached. It answers "what does the hardware do when told X".

`rme-eval-tools/measure/ratecheck.c` measures the **driver**, through ALSA, the way an
application sees it. Ask for R Hz, read exactly R frames, time it: `readi`
blocks until the device has really produced them, so a device running at D
takes R/D seconds and the measured rate comes back as D. Same idea as
upstream's wall-clock test - worth having our own copy so both sides are
measuring the same thing.

```sh
gcc -O2 -Wall -o ratecheck rme-eval-tools/measure/ratecheck.c -lasound
./ratecheck                 # auto-finds the card
```

The card must be free: `pactl suspend-card <card> 1` if PipeWire holds it, and
`0` afterwards. It asks for each rate *exactly* rather than letting ALSA pick a
near one, because silent substitution is the failure being looked for.

## The harness

`rme-eval-tools/measure/ratesweep.c` runs all of the experiments below from user space.
It extends `_archive/.../tools/usbdump/ratetest.c`, the tool that produced
PROTOCOL.md's hardware-verified alt table: claim interface 5, run the cold-init,
pump the IN endpoint, measure its byte rate. Byte rate / frame size *is* the
sample rate, so each question gets a number rather than an argument. No module
rebuild, no reboot, no Windows.

```sh
gcc -O2 -Wall -o ratesweep rme-eval-tools/measure/ratesweep.c
sudo modprobe -r snd_usb_babyface_pro     # it must not hold the device
sudo ./ratesweep sweep                    # then: alts, order, clobber, idx, scanbase
```

Monitors off — it arms the stream and changes the rate. Start with `sweep`: if it
reproduces the maintainer's 64/128/176.4 results from user space, everything
after it is cheap. `alts` is the one most likely to fix 64 and 128 outright.

## Experiments, cheapest first

**A. The 0x11 alt readback — no Windows needed** (`ratesweep` prints it on every
line). PROTOCOL.md: "after
SET_INTERFACE(5, alt N), read 0x11 and check byte 3 & 0x0F for 2/4/8 to confirm
the alt took effect" (2^alt plus a poll counter, reset to the 2^alt base on each
SET_IFACE). Run it after each of the nine rate changes. This separates "the alt
never took" from "the alt took and the base didn't" directly, and it should
predict the split already visible above: 64 and 128 failing on the alt, 176.4 on
the base.

**B. Restore the captured alt table and re-test 64/128** (`ratesweep alts`). Decouple the frame
layout from the alt — PROTOCOL ties words-per-frame to the *rate* (14/10/8 ch)
and the alt to the resulting bandwidth, whereas the PR's table makes both a
function of the alt. If 64 and 128 come right, 176.4 is left alone as a base
selector problem.

**C. Preceding-rate control for 64 and 128,** the way the maintainer already did
it for 176.4. If they track whatever ran before them rather than landing on a
fixed rate, they are rejections (device stays put), not mis-tuning.

**D. Ordering** (`ratesweep order`, and `ratesweep clobber`). The driver writes the alt in `babyface_pcm_hw_params()` and the
base later, from `bf_cold_init()` inside `babyface_stream_work()`. If the device
latches the rate at SET_INTERFACE using the base current at that moment, the
order is backwards. Worth trying base-then-alt.

**E. Re-capture on Windows** if A-D do not settle it. The prep folder is ready
and the sequence already covers all nine rates.

## Open

- PROTOCOL.md's rule-of-thumb puts 88.2 kHz at 10 ch while its hardware table
  puts alt1 at 14 ch. 88.2 is alt1 in the captured table. One of the two is
  wrong, and 88.2 is one of the rates that survived the PR by accident.
- Whether the base register has more than three values (a 9-step index rather
  than a 3 x 3 grid) is untested and would explain the triangle directly.
  `ratesweep scanbase` answers it.
- `bf_cold_init()` writes `0x10 wVal=0x0000 wIdx=0x3000` twice, *after*
  `bf_clock_write()` has written the base to wIndex `0x0030`. If those are the
  same register seen through a byte-order mix-up, cold-init wipes the base it
  just set. `ratesweep clobber` and `ratesweep idx` test both halves of that.
