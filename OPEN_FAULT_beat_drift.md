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

## MEASURED 23 Oct 2026, 14:58 — the fault quantified with wls_beatmon.ps1

User's report of that run: **"Started on-time, ran slow and drifted out of time again."** And separately,
earlier: *"consistently slow... did not correct after over 60 seconds of play."*

| Measurement | Value | Source |
|---|---|---|
| **The music** | **446 ms median = 134.5 BPM** | 308 onset intervals (leading edges) |
| **Clock 2 (the fit)** | **128 BPM = 468.75 ms — pinned, never moved** | `fitBpm` in /trav |
| **Clock 1 (what the display runs on)** | **468 ms** | `p0iv` in /diag |
| **ERROR** | **+22 ms per beat = 4.9% slow** | |

**22 ms per beat slips a full beat every ~20 beats — about 9 seconds.** That reproduces "started on time,
ran slow, drifted out of time" including its rate.

```
fitAcc = 14        fitRej = 236 → 321
```

**Clock 2 accepted 14 onsets and rejected 321 — a 96% rejection rate.** THE USER'S ARCHITECTURE, stated
exactly: *"the intro locks on to whatever, but the 2nd clock keeps monitoring until it finds a stronger
(more confident beat) and hands that over. That was what we tested and what worked."*

**So Clock 2 is still hunting — it is rejecting everything it sees.** With its input polluted it cannot
find a candidate it trusts, so it keeps the only lock it has, the wrong one. It is not broken; it is
starved of clean evidence.

## Clock 2 HAS NOT BEEN CHANGED — checked twice, including outside the guard's fence

The fence covers **L3284 → L4171** (the ODF section through `fitBeatStep`). **The width measurement
(L4680-4705) and the fit's CALL SITE (L4669) are OUTSIDE it**, so the guard could not have caught a
change there. It was therefore diffed against the baseline by hand:

**The baseline's width block is byte-identical to today's**, including `if (w >= 10 && w <= 2000)`.
The only difference in that block is the beat-1 vote removed in B2, which READ `w`, `g_fitPer` and
`g_fitN` and wrote none of them. `git diff` confirms: 13 lines out, 6 comment lines in, no functional change.

**GAP IN THE GUARD, worth remembering:** it protects `fitBeatStep` but NOT what feeds it. Any future
claim that "the guard proves the beat is unchanged" must say *"the fenced region"*, not *"the beat"*.

## The one input that HAS changed: the display load

| | `odfHz` | `fLedUs` |
|---|---|---|
| Baseline era (strips DARK, IO48-only switch ON) | ~100 | ~0 |
| Now (strips live, switch removed in B1) | **87** | **3140 µs** |

The width is `millis() - g_widStart`, closed when the envelope falls to half its peak. **A coarser frame
cadence detects that fall later, so the width inflates** — and `g_widLast / 2` is the midpoint offset the
fit is fed. Measured today: **median 30 ms, but outliers at 1000-1949 ms**, i.e. a ~975 ms offset on
those beats, against a beat period of 446 ms.

**This is the leading HYPOTHESIS, not a proven cause** — there is no baseline sample of `wid` from a
dark-strips run to compare against. The A/B test below settles it.

## ALSO FOUND: `p0lst` can WRAP, and that can suppress a flash

The monitor crashed with `Cannot convert value "4294967287" to type "System.Int32"`.
`4294967287 = 0xFFFFFFF7 = -9` signed. So `p0lst = nowMs - L.lastReal` came out **-9**: `lastReal` was
**9 ms AHEAD** of the reader's `millis()`.

**It matters because `musicNow` uses the identical unsigned comparison:**

```c
bool musicNow = !(L.lastReal == 0 || (now - L.lastReal) > 1500UL);
```

If `now < lastReal`, the gap becomes ~4.29e9 > 1500, so **`musicNow` reads FALSE and the flash is
suppressed.** That is a SECOND, independent cause of a missed flash — and unlike the 1500 ms gate, it
would drop a SINGLE flash. **Not yet investigated: why `lastReal` can be stamped ahead of `now`.**

## THE A/B TEST — the next action, and it needs no beat-code change

**Run the same track with the strips DARK** (temporarily restore the documented `WLS_IO48_ONLY` switch).
Same music, same frozen Clock 2, one variable. The prediction:

- **`odfHz` returns to ~100 AND Clock 2 locks to ~134 BPM** → the load is proven, and the fix is on the
  DISPLAY side (rate-limit the strip transmit) — **not in the protected region at all.**
- **`odfHz` ~100 and it STILL locks to 128** → the load is exonerated; look next at the BPM bell, whose
  centre is 120 (128 sits between the music's 134.5 and the bell's 120 — the prime suspect, unproven).

## Instruments that MUST be kept until this is closed

- `/onsets` and `wls_onsrec.ps1` — the onset recorder is the **beat's own** instrument. Do not remove it
  with the beat-1 instrumentation.
- `wls_tap.ps1` / `wls_tap_send.ps1` — laptop-side only, no flash cost. They are the only way to label
  beats against the user's ear. **Delete them with this fault, not before it.**
