# Baseline Beat — 2026-10-31

**The beat engine as handed over to the lightshow.** `BEAT` and `BEAT_STEP` are the
interface, and **only IO48 still flashes** — the Channel 10 relay is no longer driven.

Frozen copy of `Fresh_Beat2\Fresh_Beat2.ino`.

| | |
|---|---|
| SHA-256 | `C2A4F7686254811B92CA3BB60E4929D7C5127564A35D7B980C86F4323325F723` |
| size | 41,441 bytes, 791 lines |
| compiles | **315,418 bytes program (10%) / 25,864 bytes static RAM (7%)** |
| parent | `Baseline All 2026-10-31` (`77873b8`), plus the two changes below |

## THE INTERFACE — `BEAT` and `BEAT_STEP`

```c
static volatile bool BEAT = false, BEAT_STEP = false;
```

| variable | true for | when | what it is |
|---|---|---|---|
| **`BEAT`** | **25 ms** | **every beat** | the IO48 flash |
| **`BEAT_STEP`** | **25 ms** | **every other beat — counts 1 and 3** | what used to drive Channel 10 |

- The window is centred on the **MIDDLE** of the beat: `BEAT_WIN_MS = 25`, `winStartUs =
  flashCentre - 12.5 ms`, `winEndUs = flashCentre + 12.5 ms`. It therefore opens **12.5 ms
  before** IO48 goes high and closes **12.5 ms after**.
- `IO48_MS = 50` is only the pin pulse length, so the flash can be seen. The flag window is
  25 ms and is deliberately different.
- Both read **false while the lights are dark** (no signal, or two bars of holding) — they
  mimic the state of the LEDs.
- These two variables **define the tempo for the lightshow**. Unchanged from every previous
  build in this project.
- Both are `static`, so the lightshow must live in **this same file** to read them
  (confirmed: it will).
- The count runs 1…4 and restarts whenever the motor re-locks, so the *parity* of
  `BEAT_STEP` can land on 2 and 4 instead of 1 and 3. **Accepted by the user.**

## What changed in this baseline — two things, and nothing else

**1. Channel 10 is no longer driven.** Only IO48 flashes from now on. In the
`if (stepThisBeat)` block of `serviceOutputs()` the two statements that wrote `PIN_CH10`
were removed; `ch10Flashes++` is kept, so the 1&3 marker is still counted and the `ch10`
column in the log still proves the marks are landing. Every remaining reference to
`PIN_CH10` in the file is a **LOW** write, so the relay can never be driven:

```
 53: static const uint8_t PIN_CH10 = 42;
631: digitalWrite(PIN_CH10, LOW); ...     <- dark path
676: if (ch10Active && ...) { digitalWrite(PIN_CH10, LOW); ... }   <- inert
722: pinMode(PIN_CH10, OUTPUT); digitalWrite(PIN_CH10, LOW);       <- setup
```

**2. `REM` notes beside the now-redundant variables.** `CH10_MS`, `ch10Active` and
`ch10OffUs` are no longer used by anything. They are **kept**, with a `REM` comment giving
the exact two lines to restore if the channel-10 pulse is ever needed again:

```c
digitalWrite(PIN_CH10, HIGH); ch10Active = true;
ch10OffUs = flashCentreUs + (uint32_t)CH10_MS * 1000UL;
```

### Proof that nothing else changed

Rebuilt from identical source, the sketch reports exactly the same figures as the flashed
build: **315,418 bytes program, 25,864 bytes RAM, 315,568-byte image.** No code was added
or altered — only comments. Two builds of *identical* source were compared to be sure of
this: they produce **different image hashes but the same 315,568-byte size**, so the
toolchain stamps each image and the size is the honest evidence.

## The engine in this build — unchanged from Baseline All

- **The beat is MEASURED, not tracked.** The whole 80–180 BPM range is re-scored every
  100 ms against the newest 6 s ring of 5 ms onset frames. No PLL, no free-running clock.
- Front end: DC removal → 40–150 Hz Butterworth → ~15 ms envelope → onset = the rise over
  the last 20 ms, one value per 5 ms frame.
- Score is **excess over chance**; the shorter period wins only on a true 2× / 3× tie.
- **Tempo lock:** 3 agreeing searches to lock, 15 consecutive to change; the incumbent must
  be beaten by 12.5 %, and a candidate that is a **ratio** of the lock (1.5×, 2×, 3× and
  their inverses) is refused outright.
- The phase is re-measured at the locked period on **every** search and the grid snapped to
  it in full.
- **HOLD / DARK-HOLD / DARK / damper / freshEars** — see the Baseline All README for the
  measured reasons behind each.
- Flash at the beat **+ 30 ms**; IO48 pulse 50 ms; silence gate `rp > 120`.

**Not in this build** (tried and reverted on 31 Oct, see the Baseline All README): the
period-walk fix, the 4/3 ratio guard, and the phase-anchor attempt.

## How it sounds — on record

From the four-track judgement this build matches, apart from the two changes above:
**track 1 on time, track 2 on time, track 3 marks the off-beat for part of the track,
track 4 "very good".**

## THE LIMIT — stated once, not to be re-litigated

The comb's own phase estimate **wanders by about 100 ms** during a track, so whatever the
grid follows, the marking inherits that wander. The off-beat marking is **36–47 % of locked
time on track 3 against 0–2 % on every other track** — a property of material whose bass
runs in eighths. Resolving it would require the detector to hear something different, which
would trade accuracy on the other tracks. **User's decision: accept it, and do not tune the
detector to one track.**

## A trap: opening COM5 RESETS the board

The counters cannot be read after the fact. Opening the port reboots the board. Proven
31 Oct 2026: opening COM5 with DTR and RTS both false still rebooted it. **The capture must
be running for the whole test.**

## Serial log format

One `FT` line per second — this build's exact field list:

```
FT bpm per lock ph fit sec mar we rp ons cnt beat io48 ch10 sil sch rej chg pch lk pm hold dark dmax big
```

| field | meaning |
|---|---|
| `bpm` / `per` | fitted tempo / period **in milliseconds** (355 = 355 ms = 71 of the 5 ms frames) |
| `lock` / `ph` | locked period in frames / beat age in frames |
| `mar` | **tempo** confidence, % |
| `rp` | raw peak — the silence gate (silence 27–42, music 375–717) |
| `cnt` | bar count 1..4 |
| `beat` / `io48` / `ch10` | beats fired / IO48 pulses / **1&3 markers counted** — with CH10 no longer driven, `ch10` counts the markers while `io48` counts the flashes |
| `chg` | tempo changes, total |
| `pm` | **phase** confidence, % |
| `hold` / `dark` | holding searches / 0 lit, 1 no signal, 2 held 2 bars |
| `dmax` | the measurement's distance from the grid, µs |

One `BB` line per beat: `BB c per bpm lock ph d`.

## Related

- `Fresh_Beat2\Fresh_Beat2.ino` — the live source. This baseline is a frozen copy of it.
- `baseline\Baseline All 2026-10-31\` — the same engine, before Channel 10 was retired, and
  the record of everything tried and reverted on 31 Oct 2026.
- `baseline\Baseline Take on Me 2026-10-31\` and
  `baseline\Baseline Audio FreshBeat 2 2026-10-30\` — earlier engine states.
