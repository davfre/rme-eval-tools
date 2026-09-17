# rme-eval-tools

Local tooling for the Babyface Pro driver work. Outside both upstream
repositories on purpose: none of it is part of a patch.

    make            # builds the three measurement tools into this directory

## measure/

Three tools at two different layers. When they disagree, the disagreement is
the finding.

| tool | layer | what it answers |
|---|---|---|
| `ratesweep` | usbdevfs, driver detached | What does the *hardware* do when told X? Claims interface 5, runs the cold-init, pumps the IN endpoint and measures its byte rate. Byte rate / frame size is the sample rate. |
| `ratecheck` | ALSA, through the driver | Does the *driver* deliver the rate it says it does? Asks for each rate, times the capture stream between read completions. |
| `ratesteal` | ALSA, two clients | Can a second client move the clock under a running stream? Holds capture at one rate, then opens playback at another from a forked child. |

All three need the card to themselves:

```sh
pactl list cards short              # the index changes on every module reload
pactl set-card-profile <n> off
./ratecheck
pactl set-card-profile <n> <profile>
```

`ratesweep` additionally detaches the kernel driver from interface 5 and
re-attaches on exit. If the re-attach fails it says so; recover with
`sudo modprobe -r snd_usb_babyface_pro && sudo modprobe snd_usb_babyface_pro`.

Monitors off for all of them: they arm the stream and change the rate.

`ratesweep` has modes (`sweep`, `table`, `dds`, `ddsscan`, `pitch`, `bank3`,
`bank`, `combo`, `idx`, `alts`, `order`, `clobber`, `scanbase`, `rearm`); run
it with an unknown one to list them. Each corresponds to a hypothesis in
`../rate-analysis.md`. `ratesweep quads` needs no hardware: it prints the
DDS quads the three candidate models produce at each base, for comparing
against a capture. `ratesweep ppm <q16|exact|driver> [seconds] [base]` is
the long run that decides between them: ten minutes at 44.1 kHz per model,
timed between IN URB completions so URB granularity does not enter it.
Check `chronyc tracking` first; the result is only as good as the host
clock, and run the models alternately rather than back to back.

## driver/

Build, install and reload helpers, driven from the `justfile` one level up:

    just driver-build          # build for the running kernel
    just driver-load-local     # build, reload temporarily, leave outputs muted
    just driver-install-dkms   # persistent install via DKMS
    just status                # built vs installed vs loaded versions

`reload-driver.sh` snapshots the mixer state into `state/` before each reload.

## state/

Mixer snapshots from `reload-driver.sh`. Not tracked; safe to delete.
