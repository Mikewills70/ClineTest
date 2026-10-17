# CODE NOTES — the reasoning behind the code

The firmware lives in a 1.3 MB flash partition and is now at ~92%, so long explanations
cannot stay in the source. This file holds the reasoning; the code keeps only what you need
at the point of change, with a pointer here when the reasoning is longer than a line or two.

**Rule of thumb:** the code says WHAT and the one-line WHY. Anything that needs a paragraph
to justify belongs here, with a matching note in the code saying where to look.

---

## 1. Audio: two independent paths (do not confuse them)

| Path | How | Used by |
|---|---|---|
| **ODF chains** (4) | Time domain: 4-pole band-pass → rectify → frame differencing, per chain | The **beat** response (Moving Dot, the antifreeze) |
| **FFT bands** | Per FFT window: sum the bins in a band, subtract the tracked floor, normalise over 24 dB | `audioBass` / `audioMid` / `audioTreble` / `audioLevel` — the **VU meters**, and the Chase's movement |

Computing a new band level belongs on the **FFT** path (bin sums), never by adding filters to
the ODF path — that keeps the established beat response untouched.

**Nyquist:** fs ≈ 17 kHz, so nothing above ~8.5 kHz exists in the analyser. The band selector
tops out at 8 kHz for that reason. Going higher means a higher sample rate, which is a much
bigger change. FFT bin width ≈ 17 Hz, so the lowest bands are coarse (the log shows
`band=16-61Hz`).

**The audio task's stack is 4 KB.** Anything added there must use statics like `odfChain[]`,
never large locals.

## 2. The Chase — two patterns in one, chosen by Wrap

- **Wrap OFF (default):** the movement follows the **level** of the selected band, scaled
  against Speed, which is the CAP (Speed is **LEDs per second**, 1–50, not the seconds per
  step the dot patterns use). Silence = still, and a still chase draws and transmits nothing.
  At the end it **restarts at the beginning**.
- **Wrap ON:** the **metronome**. Each real beat starts a traverse that lands at the far end
  as the next beat arrives, then aims back. A fast tempo skips LEDs (chunky) by nature.
  Speed is not used and is greyed out.
- **Sub-beats are ignored** by the Chase: the half-way beat would reverse the traverse twice
  per beat.
- **Triggers are debounced** to ≥60% of the last measured beat. Without it, an extra onset
  mid-traverse reversed the dot and it appeared to return to the end it came from.
- A trigger **aims for the far end from wherever the dot is**; blind alternation cannot
  recover from an extra beat.
- **No beat for `antifreeze` seconds re-arms the traverse**, which is what keeps a silent
  show bouncing.
- **Minimum sensitivity means "do not let sound drive this pattern"** — for EVERY
  beat-driven pattern. The music has no effect and the pattern's own timer takes over
  (for the Chase, a constant Speed rate; for Moving Dot, it stops being flipped).
- **The antifreeze stands in for a missing beat** and moves the Chase in either wrap mode.
- An **old saved Speed** (seconds, e.g. 0.005) is indistinguishable from a deliberate 1 once
  clamped, so anything below 1 takes a default of 20 instead of crawling at 1 LED/s.

## 3. Traps that have cost real time

- **`doc["x"] | 1.0f` returns the DEFAULT for a JSON string.** The page sends most values as
  strings, so use `jsonNumF()` / `jsonNumI()`. This silently discarded the timeline delay
  (the show always ran at 20 s while the grid said 10 s) and would have done the same to the
  Speed slider.
- **A hidden element has no width.** The timeline cell names are measured with
  `getBoundingClientRect()` to decide which scroll; a grid built while its tab is hidden
  measures zero, so the grid is rebuilt when the tab is opened.
- **`scrollWidth`/`clientWidth` are rounded**, so a marquee that travels exactly that far
  eats the last character. Measure fractionally and add an end margin.
- **A port flagged `dark` is skipped by the render loop**, so anything that flags it must
  also clear the flag — STOP does, or a stopped show leaves those strips unable to be lit.
- **The device is the master.** Labels, times and the playhead are derived from what the
  board reports (`/diag`), never from a page-side assumption about what was saved.
- **A full frame transmit is a hard ~4.7 ms** and cannot be shortened by code.
- **Arduino auto-prototypes** are inserted above the sketch's first function, so any new
  function whose signature mentions a type declared lower down breaks the build.

## 5. STAGE 2 — the exact edit list (agreed, not yet built)

**The tail (Chase's Effect).** Currently drawn only for the legacy `knight_rider`, as a flat
50% stub. It becomes: drawn when the port's **effect** flag is set (or the pattern is the
legacy knight_rider), and **faded along its length** — full strength at the dot falling to
nothing over ~6 LEDs — so it reads as a trail. The colour keeps sampling each tail LED's own
position, so a rainbow leaves a rainbow tail (already correct, do not change).

**The Effect is a SLIDER, not a checkbox** (decided 2026-10-17): minimum = no effect, so a
single control both enables and sizes it — and there is nothing to grey out, because off is
simply the bottom of the slider. The unit is **per pattern**; for the Chase it is the
**tail's length in LEDs** (0 = off), which is what "how long the effect lasts" means for a
trail behind a moving dot. The tail is then faded along that length — full strength at the
dot falling to nothing at its end — instead of the old flat 50% stub.

**The Effect flag, end to end** (mirror `wrap`/`invert` exactly — they are the template):
- firmware: `PortCfg.effect`, a `effectLED` global, `TlItem.effect` (-1 = "the cell carries
  none") + `loadTimeline()` reads `it["effect"]`, `applyTimelineRow()` a `wantEffect`
  diff+assign (cell value, else the page global), `handleLiveTweak()` reads `effect`,
  `/settings` reports `effectLED`, and `saveSettings()`/`loadSettings()` persist it.
- config.h: `PlaylistItem.effectLED`.
- page: a `prowLED-effect` slider row (`paramEffectLED`, 0-10, 0 = off), `PARAM_ROW_IDS.effect`,
  `collectCurrentParameters()` sends `effect: ...value`, and the `/settings` sync sets it.

**Momentum** (Visual block slider, 0–0.5 s, **0 = off**, greyed with Wrap ON):
- it is the **release of the level→motion response**: per port keep `chaseEffRate` and move
  it toward the level-driven target by `min(1, dt/tau)` each frame, where tau is the
  momentum time. `tau = 0` gives today's behaviour exactly (the brick wall), and a longer tau
  keeps the dot travelling after the level falls — the trailing travel.
- plumbing the same shape as the Effect flag (`momentumLED`, `TlItem.momentum` with -1 =
  "the cell carries none", `PlaylistItem.momentumLED`, a `prowLED-momentum` slider).

**Why it is split from Stage 1:** momentum's meaning depends on the level-driven movement,
and the tail is a rendering change; neither touches the beat path, so each is independently
verifiable (the tail visually, momentum by timing a traverse after a loud passage).

## 6. Open questions for the user (not yet answered)

- The tail's fade length and brightness (a default is in Stage 2: ~6 LEDs, full to nothing).
- Each other pattern's effect (Moving Dot is still to be described; the checkbox exists for
  all patterns but does nothing until its effect is defined).


- **Per-row timeline timings**: the slider is the DEFAULT; a row may override it; labels run
  cumulatively; over 60 s they read `1m30s`; the END OF DISPLAY row has no time and rows
  above it always follow the slider; the slider is relabelled "Default Time Delay (Seconds)".
- **Momentum** (0–0.5 s, 0 = off, greyed with Wrap ON): the trailing travel added on top of
  the level-driven move — the release of the level→motion response.
- **The Effect flag** (per pattern; for the Chase it adds the tail, whose colour continues
  through the tail): stored on the port config, playlist items and timeline cells.
- **The Animations theme**: to be removed entirely (button, boxes, colours and storage) —
  the effects theme it existed for is not needed.
- **Tunable band for the Chase** (Stage 1b): swap the level source from `audioBass` to the
  port's own band, 40 Hz–8 kHz, one octave wide, with the Frequency row becoming a band
  centre for this pattern.
