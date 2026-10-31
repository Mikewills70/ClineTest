# Baseline 1 & 3 Working 2 — 2026-10-28

Frozen copy of `beat_simple/beat_simple.ino` — the minimal beat flasher with the
dark/locked behaviour and the stable marker parity.

| | |
|---|---|
| SHA-256 | `82690E1A8F6C0C2FD8BEFDC67A64CFC1924AAE87B3935BAE0574DE08806A92C4` |
| size | 34,675 bytes, 776 lines |
| compiles | 316,490 bytes program / 23,840 bytes static RAM |
| source | `beat_simple\beat_simple.ino` |

Build / upload:

```
--fqbn esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=huge_app,FlashMode=qio,USBMode=hwcdc,CDCOnBoot=cdc
```

`CDCOnBoot=cdc` is REQUIRED or `Serial` is UART0 and the USB log is dead.
Kill any Arduino IDE / serial monitor first, or it holds COM5 and the upload fails
silently one second after a successful build.

---

## What it does

```
IO48   GPIO 48   100 ms flash on every beat        (dark when the beat drops)
CH10   GPIO 42   100 ms flash on every OTHER beat  (dark when the beat drops)
```

## Measured state at this baseline

```
beat rate      : 2.2 beats/s = 132 BPM, stable within a locked section
                 (sec 207 beats=184 -> sec 217 beats=206)
per            : 10,453 - 10,790 samples, i.e. 133-137 BPM
mg             : 2 for every mark except across the quiet sections
                 => the INTERNAL parity never changed
fs             : 23,938 - 24,048
dc             : 1434 - 1439
stall          : 0
```

## What works

- IO48 on the beat, at the right rate, within a locked section.
- CH10 on every other beat, and the **internal** parity is rock stable (`mg=2`).
- Goes dark when the beat drops; IO48 then marks raw onsets only; CH10 out.
- IO48 can never stick on (100 ms on, 100 ms guaranteed off, no retrigger).
- Re-locks in about one and a half beats after a quiet section.
- The learned PERIOD survives a quiet section (it is not reset to the seed).
- The grid keeps counting through a gap so the parity is not re-derived.

## What does NOT work — the one remaining fault

**CH10's parity can change across a quiet section, so it lands on 2 and 4 for some
sections of the track.** Measured on the 3.5-minute 4-section track: sections 1
and 2 on 1 and 3, section 3 on 2 and 4.

Cause, and it is a real limit rather than a coding slip: **the bar phase of the
music during a quiet section is not present in the audio.** The grid can only
count through at its learned period, and the quiet section's duration is not an
exact number of beats, so the count can end up an odd number out.

The measured period wanders between 10,453 and 10,790 samples - about 3%. Over a
quiet section of 20 beats that is 0.6 of a beat of accumulated error, which is
enough to cross the half-beat ambiguity and flip the parity.

**The next change addresses exactly that**: replace the leaky single-interval
period adaptation with the MEAN OF THE LAST FEW BEAT INTERVALS, so the period
carried through a quiet section is a far more accurate estimate of the real beat
time. Less drift through the gap means the parity survives.

## A manual escape hatch that exists but is NOT the answer

Sending `F` over the serial port flips the marker between 1-and-3 and 2-and-4.
**This must not be relied on** — the requirement is that it works automatically.

## Serial log format

One line per second, tag `BT`:

```
BT <sec>  fs  dc  nov  thr  ons  per  bpm  pd=<ms>/<n>  lock  beats  marks  mg
          rej  free  stall  cal
```

| field | meaning |
|---|---|
| `per` | beat period in samples — the CLOCK |
| `bpm` | 60 x fs / per |
| `pd` | **mean SIGNED onset-to-grid offset, ms** — the phase measurement. Near 0 = the grid is on the music. Near +/-half a period (~225 ms) = it has slipped a whole beat |
| `lock` | 1 = metronome running, 0 = dark |
| `mg` | beats between marks — **the INTERNAL parity only**; it cannot see the marker's position relative to the music |
| `rej` | onsets rejected by the ±25% interval gate |
| `free` | beats emitted with no onset behind them |

## Related

- `beat_simple\beat_simple.ino` — the live source.
- `baseline\Baseline 1&3 Working 2026-10-28\` — the earlier baseline, before the
  dark/locked behaviour and before the parity was stabilised.
- `baseline\Baseline Professional 2026-10-28\` — the full `Audio.md`
  implementation, kept for reference.
