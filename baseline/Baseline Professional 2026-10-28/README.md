# Baseline Professional — 2026-10-28

Frozen copy of `beat_track/beat_track.ino` as it stood on **28 October 2026**.

    beat_track.ino          the firmware
    beat_track.ino.sha256   SHA-256 of the above

| | |
|---|---|
| SHA-256 | `61F88BD7EBF08E7CD9BD502CC236056FC03A4E46D25AA59D92F486F602D84677` |
| size | 119,655 bytes |
| compiles | 326,446 bytes program / 28,048 bytes static RAM |

Build / upload (Arduino CLI):

```
--fqbn esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=huge_app,FlashMode=qio,USBMode=hwcdc,CDCOnBoot=cdc
```

`CDCOnBoot=cdc` is required — without it `Serial` is UART0 and the USB serial log is not dependable.

---

## What this baseline DOES, verified by measurement

| Stage | Status | Evidence |
|---|---|---|
| DMA ADC front end | works | `fs=23975` stable, `dc`≈`dcE` (DC removal verified) |
| 40–150 Hz biquad + rectify + `Δx` | works | exact per `Audio.md` Stage 1 |
| Adaptive threshold + refractory | works | onset train mode at 460 ms = 130 BPM |
| Three-band fusion (low/mid/high) | works | `Audio.md` §1 as written |
| **Stage 3 tempo lock** | **works** | `comb=6-7@129` at `cap` 75–100%; `per` holds 11040–11154 (129–130 BPM) for 25 s+ |
| Agent pool + promotion | works | `a0=130` dominant, other agents at 0 |
| **Damped PLL** | **works** | `ad` (mean absolute onset-to-grid distance) bounded at 0–45 ms |
| Modulo-4 bar counter | works | cycles 1-2-3-4, never stalls |
| IO48 + channels 1–4 | works | 100 ms flash per beat, CH1-4 by beat number |
| Dual-timescale normaliser | works | `gN` tracks long-term level |
| AGC expander | works | `gA` responds to fast/short excess |
| 5 s silent calibration | works | 5 IO48 flashes at 1 Hz, then `CAL_DONE` |
| TWDT | works | `to=0 err=0 rec=0` across all runs |
| `'D'` telemetry | works | novelty / threshold / pulse stream |
| Onset flash during acquisition | works | IO48 pulses per onset until the first beat exists |

## What this baseline does NOT do

**1. Beat 1 is not reliably the musical downbeat.** It settles stably, but on the
wrong beat (Beat 3 instead of Beat 1 on the 130 BPM test track).

The cause is measured, not assumed: the four beats' low-band energies converge to
within ~10% of each other.

```
Er (winner / runner-up) : 1587% at the start transient  ->  109% once the average fills
mE (normalised means)   : 716 / 755 / 583 / 824          (within 41%, ordering unstable)
```

There is no four-beat energy cue in the low band on this material, so the Stage 4
"highest-energy transient" rule has nothing to find. `Audio.md` §2 states this
limit itself:

> "Pure audio DSP cannot mathematically distinguish 'Beat 1' (the downbeat) from
> 'Beat 3' in a standard 4/4 symmetrical loop using audio frequency data alone."

**2. `Audio.md` §3's phrase anchor is only partly built.** The energy drop-out/rise
cue exists; the spectral centroid and crash-decay cues do not. The anchor has fired
**once** in a whole track (`ph=1 pg=16`) against 22 structural events.

**3. TWDT recovery is detection-only.** `Audio.md` asks for a routine that "safely
resets the audio processing task"; this baseline counts the fault but does not
reset the task.

## Known drift from `Audio.md`

| Item | Document | This baseline |
|---|---|---|
| sample rate | "e.g. 10 kHz to 16 kHz" | 24 kHz |
| TWDT | reset the task | detect + count only |
| §3 phrase cues | drop-outs, **vocal entries**, **cymbal crashes**, **spectral centroid shifts** | energy drop-out/rise only |
| Stage 4 rule | transient *following* a structural shift | normalised mean over bars |
| `AGENT_GAIN`/`AGENT_LOSS` | "score each agent on how accurately its grid matches onsets" (no formula) | 8 / 8 |

## Measured constants (do not change without a logged hypothesis)

```
TARGET_FS            24000     NOV_MARGIN_PCT      800  (8x)
AUDIO_PIN            2         NOV_FLOOR_MUL       8    (8 sigma)
IO48                 48        REFRACTORY_MS       150
CH_PINS              11,12,13,14   BPM_MIN/MAX     60/180
NAGENT               4         PLL_TOL_BINS        10   (+/-50 ms lock window)
DOWNBEAT_MARGIN_PCT  120       PLL_PULL_BINS       24   (+/-120 ms capture range)
DOWNBEAT_MIN_BARS    2         PLL_DAMP            8    (12.5% per onset)
FLASH_MS             100       AGENT_GAIN/LOSS     8/8
CAL_SECONDS          5         STRUCT_DROP/RISE    50/180 %
```

## Serial log fields

```
RUN  fs dc dcE ac pp rms mean neg maxPP act ripMC to err rec
OND  band novPk avg thr flr sig cand refr
FUS  cand valid rej midAcc midFluxPk midThr highAcc highFluxPk highThr tot
TEM  bpm conf comb=score@bpm ph ons cap ad beats | a0..a3=bpm/score
BAR  beat down total seq flash anchors struct short long gN gA per pbeats db
     ph pg ct cm xm pd pp Ew Er mE E
IOI  n <inter-onset intervals, ms>
```

Key diagnostics:
- `ad` — agent 0's mean absolute onset-to-grid distance. Should be 0–45 ms.
- `cap` — the comb's best period as a % of the onsets in its 4 s window.
- `ph`/`pg` — qualified phrase boundaries and their spacing in beats. 0 = none found.
- `Er` — winner ÷ runner-up for the downbeat rule. ~100% means no cue exists.
- `pd`/`hits` — promoted agent's phase offset and hit count.
- `pp` — where in the beat interval the novelty peak falls.

---

## What came AFTER this baseline

This baseline is frozen at the state above. The change made immediately afterwards
completed `Audio.md` §3's phrase anchor:

- **Spectral centroid** — the magnitude-weighted mean frequency across the mid +
  high bands, against its own ~10 s baseline. A sustained shift of >= 25% held for
  300 ms is a phrase marker. Covers the document's *"vocal entries"* and *"sudden
  spectral centroid shifts"*.
- **Crash decay** — the high band held >= 60% above its own baseline for 400 ms.
  Detected by **decay**, which is what separates a crash from a hi-hat. Covers the
  document's *"cymbal crashes on downbeats"*.
- Both route into the existing structural path, so the **16/32-beat qualification**
  still decides which events are true phrase boundaries.
- Both are gated on real signal (`longEnv > NORM_MIN_E`). **MEASURED:** without that
  gate they fired 5 and 1 times during *silence*, because the centroid of room
  noise wanders as much as a musical phrase change does.

Neither cue depends on the onset train, so **neither can disturb the tempo lock**
that this baseline had just achieved.

New `BAR` fields: `ct=<centroid>/<baseline> cm=<centroid markers> xm=<crash markers>`.

If `cm` and `xm` both stay at **0** across a whole track, that is a real answer: the
track contains no spectral phrase marker, and Beat 1 cannot be resolved from
`Audio.md` §3 on that material.

