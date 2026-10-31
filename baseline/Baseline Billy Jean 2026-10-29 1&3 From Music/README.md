# Baseline Billy Jean — 2026-10-29 — "1&3 FROM THE MUSIC"

**The first build that keeps time AND derives beats 1 and 3 from the audio itself.**

| | |
|---|---|
| SHA-256 | `AA535FC2A6F9D36B721D5CF204DE4D166D03FFD653AAF637C3301F8DA55C8384` |
| size | 169,672 bytes, 3,003 lines |
| compiles | 325,498 bytes program / 24,680 bytes static RAM |
| source | `beat_simple\beat_simple.ino` |

Frozen after the run in which the user reported: *"It started on 2&4, and after a
delay it swapped and maintained 1&3. I did not press F."*

Build / upload:

```
--fqbn esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=huge_app,FlashMode=qio,USBMode=hwcdc,CDCOnBoot=cdc
-p COM5
```

`CDCOnBoot=cdc` is REQUIRED or `Serial` is UART0 and the USB log is dead.

---

## What it does

```
IO48   GPIO 48   100 ms flash on EVERY beat        (dark when the beat drops)
CH10   GPIO 42   100 ms flash on beats 1 and 3     (dark when the beat drops)
```

## MEASURED on the run this baseline records

```
phase error (raw, unclamped) : mean +0.5 ms      worst 250 ms
by third                     : 0.0 / -2.7 / +4.4 ms
clamp events                 : 1
group steer                  : ONE transition, sec 192 -> gsT=1, then held 116 s
the deciding reading         : gsA=335  gsB=1334  gap=999 (% of local average)
F pressed                    : no
gate widened (gw)            : 0 seconds, tight-reject peak tr=5
```

---

## THE TWO FIXES THIS BUILD CONTAINS

### 1. The period span, which is what fixed the drift

The phase loop is `nextBeat += dd / PHASE_DAMP`, so in steady state the grid error
settles at `PHASE_DAMP x (T - period)`. **A period error is multiplied by eight into
visible phase.** MEASURED before the fix: `per` wandered 11,618-12,425 (6.57%) on a
drum machine that cannot change tempo, and the phase error reached the +/-128 ms
bound - `pd` was sitting IN the clamp on 14 of 115 seconds, and the last third of the
track averaged +80 ms against -2 ms in the first of three-thirds.

The cause of the wander: an interval is measured between two DETECTED onsets, and the
detector's latency is not the same for a kick as for a snare. Those alternate every
beat on this material, so the intervals alternate too, and the old single-sample 1/8
leaky average settled wherever the alternation happened to be rather than on its
middle.

The fix: **average the period over 8 accepted intervals (two bars)**, so each group
appears an equal number of times and the per-group latency cancels exactly. The leaky
adaptation still runs first, so the first beats after a lock use a sane period, and 4
beats is the fallback while the ring fills. The standard deviation of the period went
**8.27 ms (single interval) -> 7.30 (4 beats) -> 5.20 (8 beats)**, which is why 8 is
the active span rather than the 4 the user originally suggested.

**Also fixed here:** a two-tier interval gate. +/-25% while acquiring, +/-12% once the
phase has SETTLED. MEASURED: every "IO48 jumped ahead then was dragged back" event
coincided with 3.0 onsets/s against a steady 2.0, and `per` dropping ~8% (12,300 ->
11,300) for a few seconds. The cause is an EARLY spurious onset - a flam or ghost note
just before the beat - passing the wide gate as a beat and shortening the period. At
+/-12% (~61 ms at 117 BPM) it is rejected outright and touches neither period nor
phase. It HEALS: six tight rejections in a row widens it back, so a genuine tempo move
cannot lock the grid out of the music.

### 2. The group steer, which is why 1&3 is now found rather than latched

**The fault it replaces:** the group came from `targetParity`, latched from the
ONSET-COUNT parity at the first mark, which includes every spurious detection before
the music starts. Nothing tied it to the track, so one run held 1&3 and the next
landed on 2&4. It was a latch wearing the clothes of a measurement. **The start of the
track was never used as an anchor and must never be** - where the music begins says
nothing about which pair is 1&3.

**Why the previous accent steer could not work, with the evidence:** its input was fed
at the ONSET FOLD while the mark decision is made at the GRID EMISSION, and those are
not 1:1. MEASURED: the marked positions were strictly every-other in only **2 of 153**
16-beat strings. The statistic was measuring the wrong beats, which is very likely why
applying it before "turned a marker that started correctly on 1&3 into 2&4" - the sign
was never proven wrong, the pairing was.

**The mechanism that works:** the accent is accumulated PER FRAME into the group of the
grid beat CURRENTLY SOUNDING, taken from the grid's own alternation (`beatCount`), so
there is no fold/mark pairing that can drift. Every `GS_WINDOW` (16) beats per group it
compares the two groups' mean MID-band accent; if the difference clears `GS_MARGIN`
(100% of the local average) it moves CH10 to the chosen side.

## WHICH SIDE OF THE SPLIT IS 1&3 — SETTLED BY THE USER'S EAR, NOT ASSUMED

This is the most valuable fact in the file, and it was established by accident plus the
user's hearing, on real material:

> **The stronger MID-band group is 2 and 4. The quieter group is 1 and 3.**

The first steer build had its comparison inverted and therefore marked the STRONGER
group. The user listened to that run and reported **2&4**. That is the backbeat
CONFIRMED on this track: the clap/snare accents 2 and 4, so the pair carrying the
bigger mid-band accent is 2&4 and the quieter pair is 1&3.

**The default therefore marks the QUIETER group.** `F` inverts it *and teaches the
steer*, so a single keypress fixes the preference for the rest of the material, and the
steer then holds the corrected side from the audio. No track-start assumption is made
anywhere, and none may be introduced.

## WHAT IS NOT SOLVED — read this before trusting the group

- **The steer took ~90-130 seconds to engage in this run.** The user noticed the delay
  ("after a delay it swapped"). The accumulation is gated on the MID detector's
  calibrated floor (8x the silent noise sigma) and the mid band in the intro was below
  it, so nothing accumulated until the mid content rose at sec 192. The window needs
  only 32 beats (~16 s), so the delay is the GATE, not the window. Known, understood,
  and deliberately not changed in this build - this is the frozen known-good state.
- **The steer HOLDS its last decision when the split vanishes.** By design: a wrong or
  flickering group is worse than a stale one, and the alternation itself is stable.
  MEASURED on this track the split ran 999% at the start, then 394% / 200%, and down to
  25% and 6% late, where there is genuinely nothing to steer with.
- **CH10 moves ONCE, when the steer first engages** (sec 192 here). That is the music
  taking over, not a fault - but it does mean the first ~30-90 s may be on the wrong
  group. `gsV=0` on the `st` line means no reading yet.
- **Brief phase excursions remain**: 29 seconds above 25 ms, worst 250 ms, and 1 clamp
  event in 218 music seconds. All short; the user reported "no beat deviations noticed"
  on the previous build and did not mention these.
- The MID/HIGH binary X/. accent flags are still SATURATED on this material (all-X),
  so `2grp` on the `ap` line remains uninformative. The strength codes (`st`) are the
  working instrument.

## Serial log - the lines this build added

```
st <sec>  L=<16>  M=<16>  H=<16>  mx=<l>/<m>/<h>  gs=<A>/<B> gsT=<t> gsV=<v>
tm <sec>  per= per4= per8=  pd=<ms>/<n> raw=<ms>  clamp=<n>  gw=<0/1> tr=<n>
iv <sec>  <last 8 accepted intervals, samples>
dd <sec>  <last 8 RAW phase errors, ms>
```

| field | meaning |
|---|---|
| `L/M/H` | accent STRENGTH per beat, base 36 in 50% steps of that band's local average. `'0'` <50%, `'9'` 450-499%, `'A'` 500-549% ... `'Z'` 1750%+. Read as a PATTERN: alternating codes = the two groups differ; flat = no cue in that band at ANY threshold. LOW = kick, MID = snare/clap, HIGH = hats. |
| `gs` | **the group steer's reading** - the MID accent mean of each group. A clear gap IS the factor that separates 1&3 from 2&4. `gsV=1` means the music is deciding; `gsV=0` means it has no reading and the default stands. |
| `gsT` | the `beatCount` value CH10 marks. `F` inverts it immediately and clears the window. |
| `raw` | the mean UNCLAMPED phase error, ms. `pd` saturates at +/-period/4 and hides anything larger - this does not. |
| `clamp` | how many phase samples hit that bound. Non-zero means the pull could not keep up, stated by the servo rather than inferred. |
| `gw` / `tr` | the tight gate: `gw=1` it has widened itself back to +/-25% (the tempo moved); `tr` the running count of tight rejections. Both stay 0 on a steady track. |
| `iv` / `dd` | the last 8 intervals and the last 8 raw phase errors. ALTERNATION is a per-group detector latency; a steady one-sided run is a period bias. |

## Files

```
beat_simple.ino                   the frozen firmware
beat_simple.ino.sha256            its SHA-256 (VERIFIED on creation)
run_log_1and3_from_music.txt      the serial log of the run this baseline records
README.md                         this file
```

## Related

- `beat_simple\beat_simple.ino` - the live source. Edits go there, not here.
- `baseline\Baseline Billy Jean 2026-10-29\` - the previous build: the drift fix
  WITHOUT the group steer. It could not choose a group and said so.
- `baseline\Baseline 1 & 3 Working 2 2026-10-28\`, `Baseline Professional 2026-10-28\` -
  earlier stable points, before the timing work.
- `Audio.md` - the engineering brief. Note §2's claim that the two groups cannot be
  separated from audio: this build MEASURES a 999% split on real material, so the cue
  exists; what §2 correctly warns about is deriving an ABSOLUTE bar phase, which is a
  different question from telling the two pairs apart.
