# Baseline 1&3 Working — 2026-10-28

**A working version. Do not lose this.**

Frozen copy of `beat_simple/beat_simple.ino` — the minimal beat flasher that was
measured working on the 130 BPM test track.

| | |
|---|---|
| SHA-256 | `78519A43481256ED16368135AC4CC86D5707290A9041766E723B09CCD2DEABCF` |
| size | 23,335 bytes, 565 lines |
| compiles | 315,534 bytes program / 23,808 bytes static RAM |
| source | `beat_simple\beat_simple.ino` |

Build / upload:

```
--fqbn esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=huge_app,FlashMode=qio,USBMode=hwcdc,CDCOnBoot=cdc
```

`CDCOnBoot=cdc` is REQUIRED — without it `Serial` is UART0 and the USB log is
dead. Kill any Arduino IDE / serial monitor first, or it holds COM5 and the
upload fails silently one second after a successful build.

---

## What it does

```
IO48      GPIO 48    100 ms flash on EVERY beat
CH10      GPIO 42    100 ms flash on beats 1 and 3   (every OTHER beat)
```

To move the marker to beats 2 and 4, change **one value** and nothing else:

```
static const bool MARK_ON_ODD = true;    // false => 2 and 4
```

## Measured evidence that it works

From the run of 28 Oct 2026 (log tag `BT`, one line per second):

```
beat rate   : mean 2.207 beats/s over 87 samples    (stable; the 1 and 3
              min/max are the 1 Hz sampling of a 2.2/s rate, not tempo jumps)

marks/beats : 96 / 192 = 0.500                      (channel 10 marks exactly
                                                     every other beat)

free        : 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,2,2,2,2,2,2,3,3,3,3,3,3,3,3,
              3,3,3,3,3,4,4,4,4       out of ~85 beats
            => 81 of 85 beats were CONFIRMED by a real onset (95%)

fs          : 23,921 - 24,147 (24,047 typical)      stable
dc          : 1434 - 1438                           stable, converged
stall       : 0                                     no DSP stalls
cal         : 1                                     calibration completed
rej         : 38 of ~150 onsets thrown away         the gate is working
```

## Why it works when the earlier firmware did not

The previous design seeded its beat grid's **phase** from a comb-filter estimate
that slid (measured: 14-30 bins/s where the geometry demanded 4). The grid ended
up **half a beat out**, and at a half-beat offset the pull direction is
ambiguous, so it could never recover. IO48 flashed between the beats for the
whole run.

This version removes that failure mode by construction:

1. **The grid is SEEDED ON AN ONSET.** `nextBeat = absN` at the first detected
   beat. It starts *on* the beat and cannot begin half a beat out.
2. **An onset only adjusts the grid if its interval is within ±25% of the current
   period.** Anything else is ignored for **both** the period and the phase, so
   the spurious onsets that were 58% of the old onset train cannot drag it. The
   rejected count is published as `rej`.
3. **The 40-150 Hz band is cascaded to 24 dB/octave.** The single-biquad
   12 dB/octave version attenuated a 500 Hz snare by only ~21 dB, so snare and
   hi-hat leaked into the novelty curve and the "low band" was not isolating
   kicks at all (measured: 58% of validated onsets were spurious).

## What is NOT in this sketch, by design

Removed, and none of it is needed for the requirement: comb filter bank, tempo
lock, agent/hypothesis pool, modulo-4 bar counter, downbeat rule,
phrase-boundary anchor, spectral centroid and crash cues, mid and high bands,
three-band fusion, dual-timescale normaliser, AGC expander, Goertzel probes,
`'D'` plot stream, Wi-Fi / web server.

82.6% of the previous firmware's bytes removed (134,082 → 23,335), 76.9% of its
lines (2,446 → 565).

## What is kept, and why each part is here

| Part | Why |
|---|---|
| DMA ADC front end | `analogRead()` blocks and jitters; fs measured 24,047 Hz |
| Fractional DC tracker | the mic idles at mid-rail; a whole-count step can only move DOWN, so it would converge to the signal minimum |
| 40-150 Hz, **24 dB/oct** | isolates the kick; 12 dB/oct leaked snare and hi-hat |
| Rectified novelty `|x[n]| - |x[n-1]|` | the onset curve |
| Adaptive threshold + refractory | 8x the rolling average, floored at 8x the calibrated silent sigma, 150 ms lockout |
| ONE beat grid | period + phase, damped 1/8 per accepted onset |
| 5 s silent calibration | measures the room and sets the floor before arming |
| Dual-core split | DSP on Core 0, GPIO flashing on Core 1, so a flash cannot delay the DSP |
| Counters in the log | every claim above is a counter, not an opinion |

## Known imperfections — visible in the data, NOT fixed in this baseline

**1. The reported period disagrees with the actual emission rate.**

```
per = 10617  ->  24047/10617 = 135.9 BPM
actual emission rate         = 132.2 BPM        (~3% mismatch)
```

So the grid is held on the beat by the **phase pull on each onset** rather than
by a correct period. It looks right, but every beat is being corrected. If the
onsets stop, the grid will be 3% out.

**2. The grid free-runs for ever when the music stops.**

```
sec 130..141   free = 82, 84, 86, 88, 91, 93, 95, 98, 100, 102, 104, 107
```

Two free-runs per second with no music — the lights keep flashing in silence.
This was deliberate (free-run rather than die) but indefinite is wrong.

Neither of these touches the beat path, so both can be changed without risking
what works.

## Serial log format

One line per second, tag `BT`:

```
BT <sec>  fs  dc  nov  thr  ons  per  bpm  beats  marks  rej  free  stall  cal
```

| field | meaning |
|---|---|
| `fs` | measured sample rate |
| `dc` | DC estimate, whole counts |
| `nov` | last novelty value |
| `thr` | current adaptive threshold |
| `ons` | onsets since boot |
| `per` | beat period, samples |
| `bpm` | 60 x fs / per |
| `beats` | beat flashes issued |
| `marks` | channel-10 flashes issued — should be about half of `beats` |
| `rej` | onsets rejected by the ±25% interval gate |
| `free` | beats emitted with no onset behind them — should stay low |
| `stall` | DSP read stalls |
| `cal` | 1 once calibration is complete |

## Capture

```powershell
Start-Process powershell -ArgumentList '-NoProfile','-ExecutionPolicy','Bypass','-File',"$env:TEMP\bt_capture.ps1"
```

Reads COM5 at 115200 and appends to `%TEMP%\bt_capture.txt`.

## Related

- `beat_simple\beat_simple.ino` — the live source. This baseline is a frozen copy
  of it; edits go in the live file, not here.
- `beat_track\beat_track.ino` — the full `Audio.md` implementation. Kept, but its
  beat grid drifted out of phase and it is **not** the working version.
- `baseline\Baseline Professional 2026-10-28\` — the full implementation frozen,
  with its own README recording what did and did not work.

