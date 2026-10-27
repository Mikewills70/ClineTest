# THE ENVELOPE REWRITE — design and the complete deletion list

**Status: DESIGNED, NOT FINISHED.** The chain struct and the sample loop were changed, which left the
file not compiling because the publish loop, the listeners and the readouts still referenced the
removed fields. The `.ino` was reverted to the last working commit so the tree stays buildable.
This file is the specification for the pass that finishes it.

## 1. The discovery (27 Oct 2026)

The figure the beat was measured on was **not an envelope**. It was `winMax` — the **PEAK** of the
rectified band signal within each 10 ms frame (`if (bpRect > ch.winMax) ch.winMax = bpRect;`). Across
a 40–120 Hz band that pulses at **twice** the band frequency — **every 4 to 12 ms**. So the leading and
trailing edges were found on the waveform's pulses: the measured Beat Lengths of **10, 19, 48 ms** were
pulses. Real beats, when the level sat mostly below the line, measured **97, 107, 118 ms**.

User's goal, verbatim: *"It is the length of the bass beat that we need to measure, not the frequency of
the pulses that make up that beat. We must create a smoothed envelope of each beat and measure the beat
length of each, and then use those figures to determine the BPM."*

## 2. The figure

```c
#define ENV_RELEASE  0.9355f      // exp(-10 ms / 150 ms), once per 10 ms frame
float bpRect = fabsf(ch.lp4);
ch.env = (bpRect > ch.env) ? bpRect : (ch.env * ENV_RELEASE);
```

Release time is bounded by the hardware: **<50 ms** lets the pulses through again; **>300 ms** bleeds
adjacent beats together (at 135 BPM beats are 444 ms apart); **150 ms** gives one hump per beat.
**This is the ONLY quantity a beat is measured from.**

## 3. The calculation — nothing else

```
envelope rises through the line   -> LEADING EDGE
envelope falls back through it    -> TRAILING EDGE
trailing - leading                -> BEAT LENGTH
leading + (Beat Length / 2)       -> MIDPOINT
midpoint to midpoint              -> the interval, and the BPM
last measured midpoint + period   -> the PREDICTED beat -> IO48 flashes there
measured - predicted              -> X, fed back so the next X is smaller
```

The leading edge also drives the lights, so ONE detection drives everything.

## 4. THE COMPLETE DELETION LIST

| What | Refs | Why |
|---|---|---|
| `winMax` | 11 | the waveform peak — replaced by `ch.env` |
| `prevWin` | 4 | existed only to make the novelty |
| `flux` | — | the novelty's input |
| `novelty` | 43 | the novelty itself |
| `novPeak` | 14 | a PEAK of the novelty |
| `NOV_PEAK_DECAY`, `NOV_RISE_EPS` | 2 | its constants |
| `lastNov`, `riseAtMs` | 2+6 | the novelty rise start |
| `odfNovelty` | 4 | chain 0's novelty |
| `sensFrac`, `L.thr`, `R.thr` | many | fractions of the novelty peak |
| `SENS_FRAC_MAX`, `SENS_FRAC_MIN` | 2 | the novelty fractions |
| `thrNow = L.sensFrac * ch.novPeak` and `if (ch.novelty > thrNow ...)` | — | THE ONSET TEST — replaced by the line crossing |
| `nov` fields (`OnsRec`, `TravRec`, /trace) | — | novelty readouts |
| `bNov`, `novPk`, `snNov`, `mdNov` | — | novelty readouts |
| `envMax`, `envMin`, `beatMax`, `beatMin`, `envP2P` | — | peak/min instruments |
| `odfAHi` | — | unused since the per-chain low-pass |
| `widthConfidence`, `bpmPrior`, `rejScore`, `spread` | — | statistical scoring, not the calculation |

**Kept:** the sensitivity line, the local envelope window, the Beat Length window, the interval window,
the median period, the 444–632 limit, X and the prediction, the one-bar timeout, the 1-and-3 comparison,
channel 10.

## 5. The two additions

**Per-chain edges:** `above`, `leadMs`, `beatLen`, `midMs`, `edgeSeq` — bumped at every leading edge so
the listeners can trigger on it.

**A fixed flash pulse:** the user — *"it should just be a flash to mark the beat."* Today the duration
comes from the CLOCK's period, which in the idle pace is 2000 ms, so it hits the 300 ms cap and
consecutive flashes merge into one long glow. Replace with a fixed short pulse.

## 6. Order of work, and the check

1. The chain struct and the sample loop.
2. The per-frame publish: remove the novelty block, publish `ch.env`, run the two edges per chain.
3. The listeners: trigger on `edgeSeq`, not the novelty threshold.
4. The readouts: delete every novelty/peak field from `/diag`, `/trace` and the heartbeat line.
5. The flash: a fixed pulse.
6. **Then read the whole audio path line by line and report.** That cannot honestly be claimed until
   the file compiles and every count in section 4 is zero.
