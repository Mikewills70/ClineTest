# Baseline Take on Me — 2026-10-31

**The best run yet on track 3 ("Take on Me", 169 BPM).** This is the state to fall
back to before anything else is changed.

Parent: `baseline\Baseline Audio FreshBeat 2 2026-10-30\` — the same engine without
the lock guard below, which flipped tempo 17 times on this track.

Frozen copy of `Fresh_Beat2\Fresh_Beat2.ino`.

| | |
|---|---|
| SHA-256 | `C6473E7170B185F5380A1720FBB77B1A8AE9E190D26D2DCD0BF6D223BA1D2A69` |
| size | 28,552 bytes, 602 lines |
| compiles | 314,606 bytes program (10%) / 25,848 bytes static RAM (7%) |
| source | `Fresh_Beat2\Fresh_Beat2.ino` |

Build / upload:

```
--fqbn esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=huge_app,FlashMode=qio,USBMode=hwcdc,CDCOnBoot=cdc
port COM5
```

`CDCOnBoot=cdc` is REQUIRED. Kill any Arduino IDE / serial monitor first, or it holds
COM5 and the upload fails silently one second after a successful build.

---

## What changed from FreshBeat 2 — the lock guard

The parent baseline's measured fault: on this track the lock flip-flopped between
`per=355` (169.0 BPM) and `per=535` (**112.1 BPM = exactly TWO THIRDS** of it) — 17
lock changes in one run, each taking 1.5 s to commit and 1.5 s to undo, with the tempo
margin at the moment of the switch down at **1–3 per cent**. A 1.5x grid lands
alternately on the beat and halfway between beats, and this track carries energy in
both places, so score alone could never settle it.

**Units:** `per` is **milliseconds** throughout this project — 355 ms = 71 of the 5 ms
frames, 535 ms = 107 frames. (An earlier version of this README called those numbers
frames; they are not.)

Two guards were added to the lock decision and nothing else was touched:

```c
if (lockedP != 0 && chosen != lockedP) {
  const int32_t inc = pScore[lockedP];
  if ((inc > 0 && pScore[chosen] < inc + inc / 8) ||   // not decisively better: hold the lock
      nearRatio(chosen, lockedP)) {                    // 1.5x / 2x / 3x: a ratio error, not a tempo
    chosen = lockedP;
  }
}
```

- **A challenger must beat the locked period's own score by 12.5 %** before it can even
  count towards a tempo change. At `mar=1` it never can, so the lock holds.
- **A candidate at 1.5x, 2x or 3x of the locked period (or 2/3, 1/2, 1/3) is refused
  outright**, however well it scores — a record's tempo never jumps to a ratio of itself.
- `nearRatio()` uses a **3-frame tolerance**, because the period is quantised to 5 ms:
  1.5 x 355 = 532.5 lands on 535, which is the exact value the old lock walked to.

The user's rules are untouched: **3** agreeing searches to lock, **15** consecutive to
change. The detector, the phase, the 30 ms flash offset and both 50 ms pulse widths are
unchanged. `IO48` GPIO48 on every beat, `CH10` GPIO42 on beats 1 and 3.

## USER FEEDBACK (this build)

> "That was the best effort yet on Track 3. I started ontime, got a little lost at the
> same place and took a while to recover. During the playback where there is a strong
> beat it catches up eventually. It looses it during the quiter beat sections which is
> understandable."

## NOT MEASURED — there is no log for this build

**This baseline rests on the ear, not on counters.** The capture died 107 seconds after
it was started (`%TEMP%\fb2g.log`, 18:32:15 → 18:34:00) having recorded silence only
(`beat=0` for its whole life), and the test itself was played the next morning with
nothing recording. No `chg`, no `per`, no `mar` exists for this run.

## A trap found on 31 Oct 2026: opening COM5 RESETS the board

The counters cannot be read after the fact. Opening the port reboots the board, so the
counters live only as long as the session that opened it.

Proven: opening COM5 with **DTR and RTS both false** still rebooted it — the first line
read back was `sch=17` climbing 10/s, i.e. 1.7 s of uptime, where a session left running
since the previous evening would have shown ~576,000 searches. The data is written
either way, so the port can be opened without DTR; the reset is unavoidable.

**Consequence: the capture must be running for the whole test.** There is no way to ask
the board afterwards what happened. (This also explains the boot banner at the head of
every capture file.)

## What is left, honestly — the quiet sections

Two different things are being conflated by the ear, and only one of them is fixable:

1. **A quiet passage with no low-frequency onset in it cannot be timed from this
   signal.** Measured on this hardware: the microphone delivers 1–3 counts in silence
   and roughly 5–30 counts on a bass beat, inside a 40–150 Hz band. If the kick is
   filtered out, gated, or the arrangement drops it, there is no beat in the band to
   find. Nothing in the DSP can recover it — waiting for the beat to come back, which it
   does ("it catches up eventually"), is the correct behaviour.

2. **The RECOVERY TIME after a quiet passage is fixable, and is probably what is being
   heard.** The phase is re-measured on every search and the grid is moved the FULL
   distance to each new measurement (`nextBeatUs += d`, 100 % gain, ten times a second).
   During a quiet passage the phase measurement degrades — the comb peak is weak and
   broad — and the grid **follows that weak measurement off the beat**, then has to
   converge back when the music returns. So a quiet section does not merely leave the
   lights alone; it actively drags them away.

   The two levers, in order of preference:

   | lever | what it does | risk |
   |---|---|---|
   | **Hold the phase when the fit is weak** — do not move the grid at all on a search whose margin is low | the grid free-runs on the last good tempo through the quiet passage, exactly as a human keeps time through a breakdown | none to the strong sections; a genuine phase change during a quiet passage would be ignored |
   | **Partial snap** — apply, say, 25 % of `d` per search instead of 100 % | rejects measurement jitter in general, time constant ~0.4 s | the grid lags a real tempo change by a few tenths of a second |

   Both keep the "measured, not tracked" property: the grid is still pulled to a
   measured phase, it just stops being dragged around by a measurement that is not
   trustworthy. Neither touches the detector, the ratio guard, or the output timing.

## Serial log format

One `FT` line per second: `FT bpm per lock ph fit sec mar we rp ons cnt beat io48 ch10
sil sch rej chg pch lk` — see the FreshBeat 2 baseline for the field list.
One `BB` line per beat: `BB c per bpm lock ph`.

**`ph` aliases** — the search runs every 100 ms and the beat every 355 ms, so the logged
phase repeats on a fixed 20-beat cycle whatever the lock is doing. It is not a measure
of phase stability.

## Related

- `Fresh_Beat2\Fresh_Beat2.ino` — the live source. This baseline is a frozen copy of it.
- `baseline\Baseline Audio FreshBeat 2 2026-10-30\` — the same engine WITHOUT the lock
  guard; it flip-flopped to two-thirds tempo 17 times on this track.
- `baseline\Baseline Audio Feedback 3 Tracks 2026-10-30\` — the previous engine
  (`Fresh_Beat`), last build with the narrow band.
