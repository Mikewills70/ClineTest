# Baseline Audio FreshBeat 2 — 2026-10-30

**The first engine that keeps time on tracks 1 and 2. Track 3 has a measured fault
(see below) — this baseline is the state to fall back to before that is fixed.**

Frozen copy of `Fresh_Beat2\Fresh_Beat2.ino`.

| | |
|---|---|
| SHA-256 | `F38FDF44CE431C4A356E202357C22E183EE7BE5A5D8D089C0085BAA224D1A65F` |
| size | 26,666 bytes, 567 lines |
| compiles | 314,502 bytes program (9%) / 25,848 bytes static RAM (7%) |
| source | `Fresh_Beat2\Fresh_Beat2.ino` |

Build / upload:

```
--fqbn esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=huge_app,FlashMode=qio,USBMode=hwcdc,CDCOnBoot=cdc
port COM5
```

`CDCOnBoot=cdc` is REQUIRED — without it `Serial` is UART0 and the USB log is dead.
Kill any Arduino IDE / serial monitor first, or it holds COM5 and the upload fails
**silently one second after a successful build** — the board then keeps running the
old firmware and it looks exactly like a code fault.

---

## What it does

```
IO48   GPIO 48   50 ms flash on EVERY beat
CH10   GPIO 42   50 ms flash on beats 1 and 3    (every OTHER beat)
AUDIO  GPIO  2   MAX9814 microphone, analogue, ADC1
```

## The engine — the beat is MEASURED, not tracked

There is no PLL, no state machine, no free-running clock. Ten times a second the
whole 80–180 BPM range is re-scored against the newest 6 seconds, so nothing can
accumulate or drift.

| Part | Setting |
|---|---|
| onset frames | 5 ms, 200 per second, ring of 1200 = **6 s of history** |
| front end | DC removal → 40–150 Hz Butterworth biquad → ~15 ms envelope |
| onset | the envelope's **rise over the last 20 ms**, one value per frame |
| search | every 100 ms, every period 67–150 frames (180–80 BPM), every phase |
| score | **excess over chance**: `sum_on_grid - (nGrid/RING_N)*sum_all`, so a shorter period cannot win just by having more grid points |
| tempo lock | **3** agreeing searches to lock; a different tempo needs **15 consecutive** (~1.5 s) — a record's BPM never jumps |
| shorter on a tie | only for a true 2x or 3x relationship (`best/2`, `best/3`) |
| phase | re-measured at the locked period on **every** search, grid snapped to it |
| flash | at the beat + **30 ms** (the middle of the beat) |
| silence gate | `rp > 120` (measured: silence 27–42, music 375–700) |
| fit lost | no confident fit for 1500 ms → lights off, count restarts |

## Measured evidence

Three tracks, log `%TEMP%\fb2d.log` (one `FT` line per second, one `BB` line per beat):

| track | fitted tempo | `mar` margin | `ons`/s | user verdict |
|---|---|---|---|---|
| 1 | per 510–520 = **115.4–117.6 BPM** | 35–46 | ~11 | on time, no faults |
| 2 | per 445–450 = **133.3–134.8 BPM** | 39–51 | ~14 | started on double time (179.1 BPM), corrected itself, then on time |
| 3 | per 355 = **169.0 BPM** | **25** | ~13 | "pretty close with a couple of skips" |

- **347 consecutive beats on track 3 at `per=355`** in that run — the tempo itself is
  rock steady, so a wobble is never the tempo.
- `ch10` is **exactly half** of `beat` on every line — Channel 10 marks 1 and 3.
- No lost flashes: `beat - io48` never exceeds 1, and that 1 is the 30 ms gap between
  the beat and the flash crossing a one-second sample boundary.

## Known fault — NOT fixed in this baseline

**Track 3's lock flip-flops between 169 BPM and its TWO THIRDS.** From
`%TEMP%\fb2f.log`, every change of `per` while locked:

```
per ->  375 (160.0 BPM)  mar=  1 chg=9
per ->  355 (169.0 BPM)  mar= 30 chg=10
per ->  535 (112.1 BPM)  mar=  1 chg=11      <-- 535 = 1.5 x 355 exactly
per ->  355 (169.0 BPM)  mar= 39 chg=12
per ->  535 (112.1 BPM)  mar= 12 chg=13
per ->  355 (169.0 BPM)  mar= 28 chg=14
per ->  360 (166.7 BPM)  mar=  4 chg=14
per ->  355 (169.0 BPM)  mar=  3 chg=14
per ->  535 (112.1 BPM)  mar= 18 chg=15
per ->  355 (169.0 BPM)  mar= 16 chg=16
per ->  360 (166.7 BPM)  mar= 32 chg=17
per ->  355 (169.0 BPM)  mar=  0 chg=17
```

**17 lock changes in one run.** Each one takes 15 searches (1.5 s) to commit and
1.5 s to undo, so the lights sit at two-thirds tempo for seconds at a time. The
`mar=1` and `mar=3` entries are the cause: at those moments the comb cannot separate
the two tempos at all — a two-thirds grid puts its points alternately on the beat and
halfway between beats, and this track carries energy in both places.

**USER FEEDBACK (this build):** *"It started perfectly ontime and then lost it again.
There are periods where it goes ontime and periods where it looses it."*

Not yet fixed: the lock rule has no defence against a candidate that is a simple
**ratio** of the locked period (1.5x, 2x, 3x and their inverses), and the incumbent
period is not given any advantage over a challenger.

## Serial log format

One `FT` line per second:

```
FT bpm per lock ph fit sec mar we rp ons cnt beat io48 ch10 sil sch rej chg pch lk
```

| field | meaning |
|---|---|
| `bpm` / `per` | fitted tempo and its period in 5 ms frames |
| `lock` / `ph` | locked period in frames / beat age in frames |
| `fit` / `sec` / `mar` | best score, second-best, and the margin % between them |
| `we` / `rp` | window onset energy / raw peak (the silence gate reads `rp`) |
| `ons` | onset frames this second |
| `cnt` | beat count 1..4 |
| `beat` / `io48` / `ch10` | flashes issued — `io48` should equal `beat`, `ch10` half of it |
| `sil` / `sch` / `rej` | fits lost / searches run / fits rejected |
| `chg` / `pch` | tempo changes / changes in the last second |
| `lk` | 1 while following a fit |

One `BB` line per beat: `BB c per bpm lock ph` — the count and the tempo actually used
for THAT flash.

**Careful:** `ph` aliases. The search runs every 100 ms and the beat every 355 ms, so
the logged phase repeats on a fixed 20-beat cycle whatever the lock is doing. It
cannot be read as phase stability.

## Related

- `Fresh_Beat2\Fresh_Beat2.ino` — the live source. This baseline is a frozen copy of
  it; edits go in the live file, not here.
- `baseline\Baseline Audio Feedback 3 Tracks 2026-10-30\` — the previous engine
  (`Fresh_Beat`), the last build with the narrow band.
