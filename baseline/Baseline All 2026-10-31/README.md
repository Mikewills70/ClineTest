# Baseline All — 2026-10-31

**The whole system as judged by ear over four tracks. This is the state to fall back to.**

Parent: `baseline\Baseline Take on Me 2026-10-31\` (the same engine before the HOLD, the
DARK-HOLD, the track-change rule and the damper).

Frozen copy of `Fresh_Beat2\Fresh_Beat2.ino`.

| | |
|---|---|
| SHA-256 | `D4AC669BFD3B1ADD59158EF1D8FAE27C3B27526EAAD66DE973385A565A67FF39` |
| size | 40,460 bytes, 780 lines |
| source | `Fresh_Beat2\Fresh_Beat2.ino` |
| log | `%TEMP%\fb2l.log` (one `FT` line per second, one `BB` line per beat) |

## USER VERDICT — four tracks, this build

| track | fitted | verdict |
|---|---|---|
| 1 | 515–520 ms = **116.5–117.6 BPM** | on time |
| 2 | 445–450 ms = **133.3–134.8 BPM** | "appeared to be ontime but behind the beat (to me)" |
| 3 | 355 ms = **169.0 BPM** (Take on Me) | "started ontime, then snapped to a faster tempo that was not on the beat, then settled back onto the beat. After the quiet section went too fast again for a while and finally settled back on beat" |
| 4 | 440 ms = **136.4 BPM** | **"very good"** |

## What this build adds over Take on Me

1. **HOLD** — the grid is only *moved* when the measurement is trustworthy. While it is
   not, the grid free-runs, keeps firing on the last good tempo, and **keeps listening**.
2. **DARK-HOLD** — after **2 bars** of that, the lights go dark while the **timing keeps
   running** (the tempo, the phase and the bar count are all retained), because that is
   what an instrumental or vocal passage looks like.
3. **DARK** — no signal at all for 1.5 s: lights off, timing retained.
4. **NEW TRACK** — a decisive challenger (`mar` ≥ 20 %, different, non-ratio) is adopted
   after 8 searches instead of 15.
5. **`gridLocked` and `lightsOn` are now separate flags.** With one flag, going dark threw
   the tempo away and the board had to find the beat again from scratch.
6. **A HOLD IS A DAMPER, NOT A FREEZE** — it applies one third of the correction. A freeze
   was measured to leave the flash at whatever offset the last untrustworthy measurement
   had produced, heard as "drifting out of time".
7. **A HOLD KEEPS THE TEMPO TOO** — the silent ±2-frame follow no longer runs while holding
   (measured: it walked the lock 355 → 350 → 345 ms while `mar` = 0).
8. **A BIG PHASE MOVE MUST BE CORROBORATED** — a move larger than a quarter period is made
   only when the phase is clear (`pm` ≥ 25 %) and the same answer persists 0.8 s.

## Measured evidence

- **`chg` — nine counted tempo changes in four tracks**, each at a track boundary or a
  settling moment. No tempo ever changed inside a track *by the change rules*.
- **The off-beat seconds (grid ≥ 150 ms from the measured beat = half a period) are
  track-specific:**

| track | locked seconds | off-beat seconds |
|---|---|---|
| 1 (116 BPM) | 135 | **5** |
| 2 / 4 (133–136 BPM) | 313 | **9** |
| 3 (169 BPM) | 213 | **67** |

- Track 3 is the eighth-note-bass case; the other two tracks are at 3–4 %.

## Known LIMITATION — the beat vs the off-beat. Accepted, not a fault.

When a track's bass runs in **eighths**, the onset train has a strong peak every **half**
beat and two grids score the same, so the comb cannot always tell the beat from the
off-beat. Measured on track 3: **67 of 213 locked seconds** sat 160–179 ms from the
measured beat — half a period is 177.5 ms — and in **64 of those 77 seconds the comb's own
phase margin was below 25 %**, i.e. genuinely ambiguous, not confidently wrong. The grid
reaches the off-beat by *walking* there in small steps, so neither a confidence rule nor a
jump limit prevents it.

Resolving it would mean tuning the band to where one track's kick lives, which would cost
accuracy on every other track. **Decision (user, 31 Oct 2026): accept it. The lights may
mark the off-beat for part of some tracks.** The same result across the other tracks was
3–4 %, so this is one track's character and not the engine's.

## Known FAULT — the period still moves in small steps. Fixed AFTER this baseline.

From `%TEMP%\fb2l.log`:

```
t=312s  per 450 -> 345 ms (134.8 -> 173.9 BPM)   mar=20  <- COUNTED change, decisive path
t=319s  per 345 -> 335 ms (173.9 -> 179.1 BPM)   mar= 9  <- SILENT walk
t=549s  per 355 -> 350 ms (169.0 -> 171.4 BPM)   mar=20  <- SILENT walk, after the quiet section
```

Two causes, both in the lock rule:

1. **A 3/4 ratio slip.** 345/450 = 0.767. `nearRatio()` covers 1.5x, 2x, 3x and their
   inverses but **not** 3/4 or 4/3, so a dotted-rhythm mis-lock passes straight through.
2. **The ±2-frame "same tempo, follow it silently" rule.** Two frames is 10 ms — 2.8 per
   cent at 355 ms — far looser than the user's own rule (*"a record's BPM holds to about
   ±1 BPM"*), so the lock can walk 355 → 350 → 345 without a single counted change.

The first is almost certainly what the user heard on **track 2** as *"ontime but behind the
beat"*: a 3/4 grid lands progressively behind, then ahead, then behind. `per=335` and
`per=345` are on the wire at t=319–328, inside track 2's stretch.

## A trap, 31 Oct 2026: opening COM5 RESETS the board

The counters cannot be read after the fact. Opening the port reboots the board, so they
live only as long as the session that opened it. Proven: opening COM5 with **DTR and RTS
both false** still rebooted it — the first line came back `sch=17` climbing 10/s, i.e. 1.7 s
of uptime where a session running since the previous evening would have shown ~576,000.
**The capture must be running for the whole test.**

## Serial log format

One `FT` line per second:

```
FT bpm per lock ph fit sec mar we rp ons cnt beat io48 ch10 sil sch rej chg pch lk pm hold dark dmax big
```

| field | meaning |
|---|---|
| `bpm` / `per` | fitted tempo / period **in milliseconds** (355 = 355 ms = 71 of the 5 ms frames) |
| `lock` / `ph` | locked period in FRAMES / beat age in frames |
| `fit` / `sec` / `mar` | best score, second-best, margin % between them (**tempo** confidence) |
| `we` / `rp` | window onset energy / raw peak (`rp` is the silence gate: silence 27–42, music 375–717) |
| `ons` | onset frames this second (8–24/s on a busy track, against a 2.8/s beat) |
| `cnt` | bar count 1..4 |
| `beat` / `io48` / `ch10` | flashes issued — `io48` equals `beat` when lit, `ch10` is half of it. **`beat − io48` = beats fired while dark** |
| `sil` | times the lights went dark for want of signal |
| `sch` / `rej` | searches run / fits rejected |
| `chg` / `pch` | tempo changes, total / in the last second |
| `lk` | 1 while the grid is running |
| `pm` | **PHASE** confidence % — the phase's own margin, not the tempo's |
| `hold` | searches spent holding in the last second (of 10) |
| `dark` | 0 = lit, 1 = no signal, 2 = held 2 bars |
| `dmax` | largest grid correction that second, in µs, BEFORE damping = the grid's true error |
| `big` | highest consecutive count of big-move corroboration seen that second |

One `BB` line per beat: `BB c per bpm lock ph d` — count, tempo, phase, and the dark reason.

**`ph` aliases** — the search runs every 100 ms and the beat every 355 ms, so the logged
phase repeats on a fixed 20-beat cycle whatever the lock is doing. It is not a measure of
phase stability, and a constant timing offset cannot be seen in it at all.

## REVERTED — 31 Oct 2026: this is the build the board was returned to

The live file is again **byte-identical** to the copy in this folder (SHA-256 verified
`D4AC669B…`, compiles 315,446 bytes).

**Everything tried after this baseline was reverted**, including three changes that were
real and verified in the data:

1. **The tempo walk** — `SMALL_DRIFT_FRAMES`, `REAL_CHANGE_FRAMES`, and 4/3 joining the
   ratio guard. Verified: `per` held 355 ms for all 155 locked seconds of track 3, where
   the run before had 21 seconds of 335/345/350 ms excursions.
2. **A 3/4 dotted-rhythm slip** (450 → 345 ms, ratio 0.767) that the ratio guard did not
   cover, and was counted as a real tempo change.
3. **The hold keeping the tempo**, which stopped the period walking to 345 ms while holding.

**And one change that failed and caused a regression, which is why this revert exists.**
An attempt to stop the lights marking the off-beat by bounding how far the grid may move
from a phase anchored early in a track:

```
MEASURED (fb2o.log, track 1, per = 520 ms)
t=58s  lock placed
t=61s  phase fixed from a VERY clear measurement (pmx = 75)     gdr =      0 us
t=67s                                                           gdr = -89748 us  (-89.7 ms)
t=69s onward   pinned at -99526 us  (-99.5 ms, 19.1% of the period)
```

The anchor was **correct**. The grid then followed the measurement 100 ms away from it and
sat there for the whole track, because the allowance it had been given — a quarter period,
130 ms — was five to ten times larger than the difference the ear can hear. The user heard
exactly that: *"drifting behind the beat again on track 1 after a few seconds"*. **The
number was invented; there was no measurement behind it.** This build snaps the grid fully
to the measurement every search, which is why tracks 1, 2 and 4 are on time in it.

## THE LIMIT OF THIS DETECTOR — stated once, not to be re-litigated

**The comb's own phase estimate wanders by about 100 ms during a track.** Whatever the grid
follows, the marking inherits that wander. Freezing the phase instead drifts out by the
period's quantisation. **Tracking the measurement fully — what this build does — is the best
available behaviour**, and it is the state with the best verdicts on record: track 1 on
time, track 2 on time, track 4 "very good".

**The off-beat marking** — measured at **36–47 % of locked time on track 3, against 0–2 % on
the other tracks** — is a property of the material: a bass line in eighths puts a strong
onset every *half* beat, and the comb genuinely cannot tell the beat from the off-beat (its
own phase margin is below 25 % in 55 of the 68 off-beat seconds). Resolving it requires the
detector to hear something different — a narrower band around the kick — which would trade
accuracy on every other track. **User's decision, 31 Oct 2026: accept it, and do not tune
the detector to one track.**

## Related

- `Fresh_Beat2\Fresh_Beat2.ino` — the live source. This baseline is a frozen copy of it.
- `baseline\Baseline Take on Me 2026-10-31\` — the same engine before the HOLD/DARK-HOLD.
- `baseline\Baseline Audio FreshBeat 2 2026-10-30\` — the first build that kept time on
  tracks 1 and 2.
