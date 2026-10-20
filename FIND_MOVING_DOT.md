# FIND THE MOVING DOT STALL

The Moving Dot in the **timeline** has been seen to stall (dot stops instead of reversing and
carrying on). It is **intermittent** — a later check showed it behaving. A fault that comes and goes
cannot be caught by hand, so the board now records everything needed to identify it. This file is the
test procedure. **Nothing about the display's behaviour has been changed by this build** — it only
watches and reports.

## What was proved, so it does not get re-investigated

- The Moving Dot's **stored** values are correct: the timeline cell for row 0 holds
  `speed = 0.002` (from `board_timeline.json`). The fault is NOT a bad saved value.
- The **erosion** is correctly gated: with Effects = 0 the dot sheds nothing and is drawn at full
  length (`mdEffectOn` requires `effect > 0 && distance > 0`). Confirmed by test.
- **None** of the recent Chase work touches the Moving Dot's motion code — verified by diffing
  against `baseline-effects-2026-10-17`: the differences in `mdShed`/`mdAdvanceAll` are comments only.
- The step interval is `speed x 1000 ms`. Moving Dot's speed is seconds per step (0.002-0.1), so it
  steps every 2-100 ms. **A speed of 1.0 would step once per second — indistinguishable from stopped.**

## What the board now reports

`/diag` (and therefore `board_diag.txt`) carries one compact line **per port**, `m1`..`m10`:

| field | meaning |
|---|---|
| `pat` | the pattern that port is REALLY running (compare with what you expect) |
| `sp` | the live speed — the step interval is `sp x 1000` ms |
| `sz` `ef` `ds` `mo` | size, effect, distance, momentum the port is holding |
| `lost` | `mdLost` — pixels the dot has lost to erosion. **The timeline never clears this.** |
| `dot` | the length actually DRAWN. **`dot=0` is an invisible dot — it looks stopped.** |
| `dead` | 1 = it exploded and is waiting for its 3 s restart |
| `fre` | how many times the motion restart has re-pinned this port |
| `cell` | the id of the cell owning the port (`-` = the page owns it) |
| `gs` | pixels this port has advanced — **two readings apart answer "is it moving?"** |

Plus `selPat` (the pattern selected on the page) and `tlPlaying`, `tlRow`, `tlElapsedMs`.

The board also writes its own **`STALL`** line to the log, **once per port per 10 s**, whenever a port
that is not dark and not level-driven has not advanced for **4 seconds** — carrying the same fields.
So an unattended stall is recorded rather than missed.

## The test

1. Put the PC on the board's Wi-Fi (`ESP32-LightShow` / `12345678`).
2. Load the timeline the way you normally would, and let it run through the Moving Dot row at least
   twice. **Note the display numbers** the Moving Dot is on (you said displays 1 and 2).
3. Watch the Moving Dot strip. If it stalls, **leave it running** — do not touch the page, because
   touching it may clear the fault (that is itself a clue).
4. Let it run for a minute or two after any stall, so the watchdog has time to write `STALL` lines.
5. Then run, from this folder:

   ```powershell
   powershell -ExecutionPolicy Bypass -File read_board_log.ps1
   ```

6. Send me **`board_diag.txt`** and **`board_log.txt`**. If the fault did not appear, send them
   anyway — the `m1`/`m2` lines alone tell us a lot.

## How the reading will be decided

| what the log shows | what it means |
|---|---|
| `dot=0` on the stalled port | the erosion state is the fault: `mdLost` was left elevated and **the timeline never resets it** (the header claims a timeline row should). Fix: reset the erosion when a row is applied. |
| `sp` around 1.0 (not 0.002) | the port is holding another pattern's speed: the cell's value or the adoption is wrong. Fix: trace that value's source. |
| `pat` is the Chase (not Moving Dot) | the port is running the CHASE on the Moving Dot row — the item-name resolver or the cell's id. Fix: the resolution path. |
| `fre` climbing while `gs` stands still | the motion restart is re-pinning the dot every frame. Fix: the adoption comparison (`motionDiff`). |
| `gs` climbing but you saw no movement | the dot IS advancing — so it is a DRAWING/visibility problem, not motion. |
| `STALL` lines absent and `gs` climbing | the fault did not reproduce, and the board was moving the whole time. Send the files anyway. |

**Rule 5 in action:** nothing is changed until one of those rows is proved by the numbers.
