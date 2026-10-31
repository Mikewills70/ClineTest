# Baseline Billy Jean — 2026-10-29

**The first build that held beats 1 and 3 for the whole track, with the beat in time.**

| | |
|---|---|
| SHA-256 | `93244B42C6C851D8FC7C58BAEDED2179F17E1A1EAB84B16D2FD3B566BA4759ED` |
| size | 133,682 bytes, 2,430 lines |
| compiles | 323,066 bytes program / 24,432 bytes static RAM |
| source | `beat_simple\beat_simple.ino` |

Frozen on the user's instruction after the run that "locked in on 1 & 3 and stayed".

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
CH10   GPIO 42   100 ms flash on every OTHER beat  (dark when the beat drops)
```

## Verified by measurement in that run

```
beat timing   : in time from the start - the 8-bar off-beat acquisition is gone
mg            : 2 for 223 of 224 music seconds, and no mid-track movement at all
acq           : reaches 1 within a beat or two of the music starting
rsy           : 0     - no resyncs, because the resync needs a verified target
rlt           : 0     - no phrase boundary was ever measured
log           : clean - six tags, no watchdog spam (2326 lines/12 s -> 44)
```

## THE TWO THINGS THAT FIXED THE HOLD - both were regressions of mine

`mg` used to show **45 skipped marks** and `rsy` climbed to **61** - CH10 went
irregular and the group moved repeatedly mid-track. Both are gone:

1. The mark is consumed **per GRID EMISSION** but was briefly gated **per accepted
   onset**, so it skipped marks whenever the two were not exactly 1:1 - 45 of them -
   and each skipped mark moved the next one onto the other group.
2. The resync servo fired on any mismatch, so it thrashed (`rsy` 61) and each firing
   flipped the group. It now needs the mismatch to persist 16 consecutive onsets, and
   it needs a **verified target** (`rlt > 0`) before it may act at all.

## WHAT IS NOT SOLVED - read this before trusting the group

**The group is still a coin flip, and this run happened to land on 1 and 3.**

The latch takes its parity from whichever beat was marked first. Nothing in the audio
has been found that can choose between the two groups, and the run that prompted this
baseline measured exactly that:

```
ap 343  mid=XXXXXXXXXXXXXXXX (16)   2grp=100%/100%
ap 426  mid=XXXXXXXXXXXXXXXX (16)   2grp=100%/100%
```

`2grp` is the MID accent rate of each of the two alternating beat groups over the last
64 beats. **100%/100% means an accent on every beat - a split of ZERO.** With no split
there is nothing to steer the group with, and `rate` (which read 100%) is meaningless:
it is only informative when `2grp` shows a gap.

**A fresh boot may therefore latch 2 and 4 and hold it.** If it does, press `F` once
over serial - the group is then held and stable for the rest of the track.

### What was tried, and what it measured

| cue | source | measured result |
|---|---|---|
| low-band energy per bar position | Audio.md Stage 4 | **flat** - `ErL` 100%, no cue |
| MID energy per bar position (argmin rule) | restored | contrast ~115%, **argmin wanders**, `cnf` never reached 8 |
| MID accent rate of the marked beats | this session | **no split** - `2grp` 100/100 on a track with a clear snare |
| HIGH band accent (5-10 kHz) | Audio.md §1 | fires ~3/s on the hi-hats - not a group cue |
| §3 phrase anchor: structural / centroid / crash | Audio.md §3 | events fire (str 20, cmk 11, xmk 61) but **no repeated phrase period** - `gap` never settled, `rlt` stayed 0 |

Audio.md §2 states this limit itself:

> "Pure audio DSP cannot mathematically distinguish 'Beat 1' (the downbeat) from
> 'Beat 3' in a standard 4/4 symmetrical loop using audio frequency data alone; it
> only knows a beat occurred."

**Every cue Audio.md names has now been measured, and none is reliable on this
material.** This baseline does not solve that; it makes the system *stable* given that
it cannot be solved from the audio.

### The most promising untried lever

`2grp` = 100/100 means the accent detector fires on **every** beat - i.e. the threshold
(`ACCENT_MARGIN_PCT = 250`) is too permissive for this material, so nothing stands out.
**Raising it** (to ~400-600) so only the strongest transients qualify is the cheapest
next test: a clap on 2 and 4 should then show up as a real split in `2grp`, and the
group could finally be steered automatically. The MID bins were also moved in this
build from Audio.md's 500-2000 Hz to **1500-4000 Hz** for the same reason, but the
split did not appear at either range on the material tested.

## Known imperfection, observed and NOT fixed

**The lights run slightly AHEAD of the beat.** Recorded here so it is not lost. `pd`
(the mean signed onset-to-grid offset, ms) is the instrument for it: the grid is being
held slightly early, so the fix belongs in the phase pull. Small and separate, and it
was deliberately not attempted while the group decision was still being closed out.

## Serial log format - six lines per second

```
BT  <sec>  fs  dc  nov  thr  ons  per  bpm  pd=<ms>/<n>  lock  beats  marks  mg
           rej  free  stall  cal
ee  <sec>  ErM  ErL  mE=<4>  win  pOff  anc  bars  act  cnf  mPk  pp
an  <sec>  str  phr  amv  gap  srun  cmk  xmk  cand  ely  vld  rej  gN gA
           fEnv  sEnv  lEnv
ap  <sec>  beats  accMid  accHigh  mid=<pattern>  high=<pattern>  2grp=<A>/<B>
mk  <sec>  inv  rate  dec  last  rule=<n>% of <n> marked  tgt=<l>/<p>
           rsy  run  acq=<s>/<r>  rlt
```

The fields that matter most:

| field | meaning |
|---|---|
| `mg` | beats between CH10 marks - **must stay 2**; anything else is a real event |
| `pd` | mean SIGNED onset-to-grid offset, ms - the phase measurement, and the key to the drift noted above |
| `tgt` | the latched marker group `latched/parity`; once 1 it should never change |
| `rsy` | resyncs. 0 is healthy. Each one was a grid slip that would have moved CH10 half a bar |
| `acq` | `settled/consecutive-onsets-within-10ms` - whether IO48 is actually ON the beat |
| `rlt` | group re-latches from a measured phrase boundary. 0 means none was ever measured |
| `2grp` | MID accent rate of the two beat groups - **the direct test for whether the cue is in the material** |
| `gap` / `srun` | the measured phrase period in beats, and consecutive repeats |
| `inv` / `rate` | the accent verdict and the accent rate of the marked beats. **The accent verdict does NOT steer** - it was measured to have a material-dependent sign, so it is reported only |

`F` over serial inverts the marker's group. It is the manual escape hatch, and with the
resync gated off the group is otherwise held stable for the whole track.

## Capture

```powershell
Start-Process powershell -ArgumentList '-NoProfile','-ExecutionPolicy','Bypass','-File',"$env:TEMP\bt_capture.ps1"
```

Reads COM5 at 115200 into `%TEMP%\bt_capture.txt` for 15 minutes, reconnecting if the
port is lost, and clears the file at start.

## Files

```
beat_simple.ino            the frozen firmware
beat_simple.ino.sha256     its SHA-256 (VERIFIED on creation)
run_log_billy_jean.txt     the serial log from the run this baseline records -
                           the raw evidence behind every number quoted above
README.md                  this file
```

## Related

- `beat_simple\beat_simple.ino` - the live source. Edits go there, not here.
- `baseline\Baseline 1 & 3 Working 2 2026-10-28\` - the previous stable build, before
  the dark/locked behaviour, the group latch and the acquisition fix.
- `baseline\Baseline Professional 2026-10-28\` - the full `Audio.md` implementation,
  kept for reference.
- `Audio.md` - the engineering brief. §2 states the limit this baseline works around.


