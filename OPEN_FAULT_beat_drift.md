# OPEN FAULT — beat selects the wrong beat, drifts, and can miss a flash

**Status: PARKED by the user, 23 Oct 2026.** The audio clean-up exercise continues around it. Do not
change anything in the BASS BEAT protected region while this is open and un-diagnosed.

## What the user reported

1. Playing **the same tune** that previously gave a perfect run, for **half the song the beat was on the
   HALF BEAT and did not correct at all**.
2. It then **drifted through being on beat and went off beat again**.
3. **After a silent period it started on beat again and held the timing.**
4. **IO48 missed a beat — one flash simply did not happen.** The flashes that did happen were on time.
5. The LED strips and 240V were working correctly throughout.

## What is PROVEN

**The beat code is byte-for-byte identical to `baseline-hitthatperfectbeat-2026-10-23`.** Verified by
extracting the same span from the baseline tag and the current file and applying the identical
bar-state-block deletion to the baseline copy:

```
baseline region: 55093 chars      current region: 54419 chars
RESULT: the only difference is ONE trailing blank line
CONFIRMED IDENTICAL
```

So this is **not a regression from an edit to the beat.** The `wls_beatguard.ps1` hash history is the
standing proof, and it should be re-run before and after any future investigation.

## The evidence pointing at the frame cadence

The board reports its own frame instruments. Striped across the saved captures and the current build:

| Run | `fLedUs` (strip transmit per loop) | `odfHz` | Beat behaviour |
|---|---|---|---|
| `beat_t26` drift hunting | 2145 µs | — | drifting |
| `beat_t29` drift hunting | 2218 µs | 88.3 | drifting |
| `beat_t32` drift hunting | 2244 µs | 86.7 | drifting |
| **"Perfect run" (IO48-only switch ON)** | **~0** | ~100 | **perfect** |
| **Now (strips live)** | **3140 µs** | **88.8** | **off time, missed flash** |

**Every session with the strips transmitting drifted. The one session with them dark was perfect.**
The strip transmit is now **89% of every loop pass** (`fLedUs` 3140 µs of `fLoopUs` 3508 µs).

The detector is designed to run at **100 Hz (a 10 ms frame)**. It is running at **88.8 Hz**.

## The hypothesis to test (agreed framing)

**The fault can genuinely live in the protected code, without the code having changed.** The detector
**assumes a fixed 10 ms frame cadence and never measures or verifies it**. Everything built on frames
shifts with the real cadence:

- the novelty history (`ODF_HIST`, 32 frames — 320 ms at 100 Hz, 360 ms at 88.8 Hz),
- the local mean and therefore the adaptive threshold,
- the envelope **width**, which the midpoint correction is built from — and the midpoint is exactly what
  made the tempo accurate and amplitude-invariant.

A jittery width gives a jittery midpoint, which biases the fitted period, which produces a slow phase
slide. A dropped detection frame drops an onset outright — **which is the missed IO48 flash.** One
mechanism explains all four reported symptoms.

## Tests to run when we return (Rule 5 — measure, do not guess)

1. **Record `/onsets` through the failing passage with the display live.** `/onsets` gives leading edge,
   trailing edge and midpoint per onset, plus `mask`, `nov`, `thr`. This shows directly whether the
   detector fires on the beat, the off-beat, or both, and whether the cadence is ragged. **No flash
   needed.**
2. **Measure the real cadence.** `/diag` `odfHz`, `odfFr`, `odfLo`, `odfHi` during the passage, alongside
   `fLedUs` and `fLoopUs`. `odfHi` is the worst frame interval — that is where a dropped onset will show.
3. **A/B the same passage, dark vs live.** Temporarily restore the documented `WLS_IO48_ONLY` switch and
   record the same tune. Same music, same beat code, one variable.
4. **Correlate a missed flash with a missed onset** using the tap tooling (`wls_tap.ps1`) to label beats
   against the user's ear. This is the direct test of "IO48 missed a beat".

## Candidate fixes, in order of preference (NOT yet agreed, NOT yet started)

1. **Rate-limit the strip transmit.** The loop runs at ~285 Hz and transmits all ten strips every pass.
   LED strips need ~60 fps, not 285 — the transmit is pure waste. This frees the CPU **and** makes the
   cadence regular, and the display stays live. Display-side, low risk.
2. **Make the detector cadence-independent** — measure the real elapsed time per frame instead of
   assuming 10 ms. **This is inside the protected region, so RULE 10 applies and it needs the user's
   written authorisation.**
3. Keep the display dark while measuring the beat — a workaround, not a fix.

## Instruments that MUST be kept until this is closed

- `/onsets` and `wls_onsrec.ps1` — the onset recorder is the **beat's own** instrument. Do not remove it
  with the beat-1 instrumentation.
- `wls_tap.ps1` / `wls_tap_send.ps1` — laptop-side only, no flash cost. They are the only way to label
  beats against the user's ear. **Delete them with this fault, not before it.**
