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
- **`dbgStage` is written ONLY by `loop()` and `runLEDPattern()`.** The `/diag` legend that
  claimed "1 = web server, 5 = timeline, 9 = brightness save" was fiction: 5 and 9 are never
  written, and the web task writes nothing. `prevStage=1` means "the top of `loop()`" — the
  exact window the heap-floor guard (`ESP.getFreeHeap() < 15000 -> ESP.restart()`) sits in.
  The web task's breadcrumbs are `dbgReq` (request in flight) and `dbgSub` (checkpoint).
- **`POWER_ON` (reason 1) WIPES RTC.** Every RTC breadcrumb — including a boot line that
  reads back as garbage like `prevUp=2960664895s` — is meaningless after one, and so is
  "BOOT #2" (the counter lives in RTC too). That is a supply interruption, not a memory
  overwrite. Reason 2 is the EN pin, 4 a panic, 6 the task WDT, 11 USB.
- **IO48 carries THREE different signals**, which made "4 flashes" ambiguous and cost a test
  round: 3 fast power-up flashes; 150 ms command blinks (1 = /live-tweak, 2 =
  /calibrate-test, 3 = /theme-preview, 4 = /init-off); and the boot report. The report is now
  slow (a 1 s marker, then 500 ms blinks), starts 1.5 s in, and cannot be cancelled — a
  command blink waits for it instead of silently truncating it.
- **Navigation sends requests by itself.** Leaving the Patterns tab fires `/init-off`
  (`displayActive = false`, `blankReq = true`); opening any tab except Colours fires
  `/calibrate-stop`, which blanks UNCONDITIONALLY even when no test was running. So a page
  opening is NOT evidence that a tweak was sent — read the `tweak` line in the log.
- **`handleLiveTweak()` has no 240V branch at all.** The 240V assignments (`activePattern240V
  = id`, `speed240V`, `quantity240V`, `freq240V`, `mult240V`, `music240V`, `attack240V`,
  `decay240V`, `wrap240V`, `invert240V`) sit INSIDE `if (section == "LED")`. Consequences: an
  LED tweak overwrites every 240V global with the LED page's values (`speed240V` = Speed in
  LEDs/s, which `run240VDisplayPattern()` then reads as SECONDS per step), and a 240V tweak
  updates nothing but the global beat sensitivity. The comments in the function assume the
  branch exists. Introduced 2026-10-14 (3a94152); no baseline ever had it. NOT proven to be
  the reset — it is a separate real bug, and the page's single shared pattern selection means
  the shared *id* is intended.

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

**BUILT 2026-10-17 (controls only — the plumbing above is NOT done).** Three rows now exist in
the Visual block of the LED page, with the ids this section names, and all three read minimum
as OFF:

| Row | id | Control id | Range | Notes |
|---|---|---|---|---|
| Effects | `prowLED-effect` | `paramEffectLED` | 0–10, step 1, default 0 | unit is per pattern; for the Chase it is the tail's length in LEDs |
| Momentum | `prowLED-momentum` | `paramMomentumLED` | 0–0.5 s, step 0.01, default 0 | release of the level→motion response |
| Distance | `prowLED-distance` | `paramDistanceLED` | 0–100, step 1, default 100 | **added 17 Oct by the user**: the intended replacement for Speed in the Chase |

They register in `PARAM_ROW_IDS` (so visibility/greying works) and are listed in the Visual
list of the five dot/Chase patterns, so they are live for those and greyed for VU/EQ. The
values are **page-side only**: they are not in `collectCurrentParameters()`, nothing is stored,
and the firmware does not read them — moving them changes nothing on the strips. `Speed` is
left in place for every pattern, including the Chase, until Distance's behaviour is defined.

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

## 7. Crash instruments — what to read, and in what order

Added 2026-10-17 (session 2). Read them in this order:

1. **The boot report on IO48** (needs no PC). Three fast power-up flashes, then 1.5 s, then
   ONE LONG 1 s flash ("this is the boot report"), then the previous reset reason in 500 ms
   blinks: 1 = POWER_ON, 2 = EN pin, 3 = software (`ESP.restart()` — the heap-floor guard is
   the one that sits at stage 1), 4 = PANIC, 6 = task WDT, 9 = brown-out, 11 = USB.
   A burst of FAST flashes with no 3-flash preamble is a COMMAND code (4 = /init-off), not a
   reset. That distinction is the whole point of the marker flash.
2. **`prevStage` / `prevReq` / `prevSub`** — `/diag`, and the `BOOT #` line in `/log.txt`.
   Meaningful when the run ended in a panic or a watchdog. GARBAGE after POWER_ON.
3. **`lastReq` / `lastReqUpSec` / `lastReqHeap`** — `/diag`. Kept in NVS, so they survive a
   power cut: the only breadcrumb that means anything after a supply dip. Together with
   `boots`/`erases` they say whether the run died on power rather than in the firmware.
4. **The `tweak` line** in `/log.txt` (change-only, so a slider drag cannot flood it):
   `tweak <section>|<pattern>|<colour count> bytes=… heap=…`. If opening a page produces no
   such line, that page sends no tweak at all — and the fault is in `/calibrate-stop` or
   `/init-off`, both of which blank on their own.

## 10. Per-pattern parameters — NOTHING is shared (STEP A, built 17 Oct)

THE RULE (user): every pattern is unique. Nothing — no value, no colour, no band, no threshold
— is shared between patterns, on either section. Changing anything in Moving Dot must have no
effect on Chase or any other pattern.

**Firmware**
- `LedPatParams`: ONE RECORD PER LED PATTERN, six of them — `moving_dot`, `chase_1_10`,
  `center_out_chase`, `knight_rider`, `vu_meter`, `eq`. Each holds colour[4], speed, qty,
  size, wrap, invert, mult, freqHz, beatSens, attack, decay, peakDelay/peakSpeed/peakDir,
  vuDir, effect, momentum, distance.
- The record is the STORAGE. The old `speedLED`/`colourLED`/… globals are a **MIRROR of the
  SELECTED pattern's record** (`mirrorLedPat()`), so every existing reader is unchanged.
- `applyTimelineRow()` falls back to the selected **record** (not the global, which a 240V
  tweak could move). A timeline cell still overrides per strip, as before.
- NVS: one blob per pattern (`lp0`…`lp5`), written on **Save** and on a **pattern change** —
  the latter so a reboot before a Save cannot re-seed the new selection from the legacy keys.
- Migration: the pre-record single-value keys seed the SELECTED pattern's record once; every
  other pattern starts from `ledPatDefaultFor()`. The boot line prints
  `LED records MIGRATED: selected seeded from pre-record settings`.
- `ledPatNormalise()`: **Moving Dot's Wrap is forced OFF in the firmware too**, so a Wrap
  ticked before records existed (or by an old timeline cell) cannot make the dot bounce.
- `/live-tweak` now has a real `else if (section == "240V")` branch. The LED branch used to
  write the 240V globals, which is why the 240V page's own sliders did nothing — and it is the
  cross-section contamination the rule forbids.
- `/settings` reports the selected pattern as before PLUS **`ledPatAll`** (all six records).

**Page**
- `ledPatStore` (one set per pattern NAME) + `LED_PAT_STORE_IDS`; `saveLedPatternSet()` when
  leaving a pattern, `loadLedPatternSet()` when entering one — and the load happens AFTER
  `applyParamRanges()`, or the outgoing pattern's range clamps the incoming values.
- Seeded from `ledPatAll`, so a reload restores EVERY pattern. Without it, the first tweak
  after a switch would have written defaults over another pattern's stored values.
- `captureMarkupDefaults()` snaps the markup defaults at startup, so a pattern with no stored
  set starts from those and never inherits the previous pattern's controls.
- **ONE DEBOUNCE TIMER PER SECTION** (`window.liveTimers[section]`). The single shared
  `window.liveTimer` cancelled the 240V tweak whenever `choosePattern()` fired both, so the
  240V page only ever appeared to work through the firmware's LED branch.
- Distance carries its own per-pattern range (Moving Dot **0–20**, default 0–100) and is in
  `PARAM_ROW_DEFAULTS`, or the 20 would leak into the next pattern — the trap Speed had.

**Still open (deliberate, as agreed):** the 240V patterns share one set until their own
parameter list is defined; the page's per-pattern store is LED-only for now.

## 11. Moving Dot's effect — shed debris (STEP B, built 17 Oct)

**Trigger: EVERY reversal of the dot** — a real beat, or a sub-beat when Beat Multiplier = 2.
Wrap is forced off for Moving Dot, so the end-of-strip jump is not a reversal and never sheds.

Per reversal `mdShed()` spawns **1..Distance/5 pieces (max 4, randomised)**, each about **one
pixel of light**, at the dot's position, travelling in the direction the dot **was** going (the
eruption comes from the point of reversal) — and every **TENTH** reversal the dot loses one
**whole** pixel (`mdLost`), all of its dots together. At 0 length the dot draws nothing and
**stays dark** until re-armed.

`mdAdvanceAll()` runs **EVERY FRAME** — the debris is a cloud, not a dot; tied to the step timer
it would stutter. **Momentum is WEIGHT and drives speed AND brightness together** (the user's
decision): at its maximum a piece stops within ~1 px and is gone at once; at its minimum it
keeps the dot's starting speed (1 / Speed, in pixels per second) for the whole flight and fades
gently. It dies at the **Distance** extent, at the strip end, or when brightness reaches 2%.
**8 pieces per strip, oldest recycled** — a fast passage reverses ~10x a second.

**Effects = 0 or Distance = 0** → `mdEffectOn()` false → **no debris AND no erosion**: the
pattern behaves exactly as it did before. Drawing: debris **under** the dot, in the dot's own
colour reference sampled at each pixel, dimmed by its own brightness.

**Re-arm** (`mdResetAll()`): a timeline **ROW change**, **PLAY**, and **selecting the pattern**
again. Deliberately NOT on a slider move — a tweak also calls `applyTimelineRow()`, and
resetting there would resurrect a dead dot every time any control moved.

**Log:** `effect flips/lost/frags` per port, beside `motion`, plus a live piece's position,
speed, brightness and travel distance — an erased dot is otherwise invisible, because it simply
draws nothing. The live values are what settled "the debris is not moving" without guessing.

### Revised 17 Oct (after the user's first look at it)

- **The count is the EFFECT SLIDER, exactly**: 1..10 pieces per direction change, **no
  randomisation at all**. It used to be `Distance/5` capped at 4 and randomised. Effects = 0 is
  still OFF (their earlier rule: "if effect is at 0 nothing changes, and Momentum and Distance
  are greyed"), so the slider is 0-10 with 0 = off.
- **THE DEATH BLAST.** When the tenth reversal takes the dot's LAST pixel the strip explodes
  from the position where it died: 24 one-pixel pieces, radiating BOTH ways, with a spread of
  speeds (50-310 px/s) so they arrive at different times and reach the far ends. It ignores the
  **Distance** extent (it spans the whole strip) and its decay ignores **Momentum** (a heavy
  setting must not turn the ending into a fizzle). It also **clears the old debris**: with the
  dot gone there is nothing left moving, so the cloud dies with it — the display is then over
  until the pattern is applied afresh (a timeline row, PLAY, or re-selecting it).
- A dot that is already gone sheds nothing further and **cannot explode twice**
  (`mdShed` returns immediately once `Size - lost <= 0`).
- **EVERY DOT SHEDS (19 Oct).** The debris used to spawn at `ledPosArr[p]` alone — dot 0's
  position — so with Quantity > 1 only one dot of the three (or eight) broke up. `mdShed()` now
  loops the port's dots and spawns the full Effects count at each, using **the same offset
  arithmetic as `drawPortPattern()`** (`(pos + q*N)/qty % N`), and `mdExplode()` erupts from every
  dot's position too (MD_BLAST split across them, at least 4 each). The pool is `MD_FRAGS` 64:
  a burst is Quantity x Effects pieces, and past 64 the OLDEST are recycled rather than the newest
  being dropped. Erosion still shrinks all the dots together, and a port still expires once.
  **Trap:** any effect that acts "on the dot" must ask where EACH dot is, not where the port is.

### TRAP that cost a round: a per-frame step rounded to whole pixels freezes slow movement

`int step = (int)(speed * dtS + 0.5f);` looks harmless, but at 57 px/s and a 5 ms frame the step
is **0.28 px — which rounds to 0 every single frame**, so the piece never moves and merely fades
where it spawned. The user saw exactly that ("stationary dots at the position the dot changes
direction"). **Positions that move slowly must be floats** (`MdFrag.posf`), with the integer
computed only at draw time. The same trap applies to `travel`/`moved`, and to anything else
advanced per frame in this firmware.

### Second trap from the same round: a blur cannot throw visible debris

The pieces started at the dot's speed, and the dot at 0.002 s/step is **500 px/s** — a piece
would cross the whole 20-pixel Distance extent in **40 ms**. Debris is only debris if it can be
seen, so the start speed is the dot's **only when that is slow enough**: capped so a piece
always takes at least ~0.5 s to cross the extent (`visCap = Distance / 0.5`). The log prints the
raw and the capped figure (`effect speeds dot=… cap=…`).

## 13. Timeline: column 1 follows the delay slider, live (19 Oct)

The row labels in column 1 ("0-20s", "20-40s", …) were drawn from **`tlSavedDelaySec`** — the delay
the BOARD had been given — so dragging the delay slider changed only the readout and the grid
disagreed with the slider until the save landed, which read as the slider doing nothing. Both the
per-row labels and a grid rebuild now use **`tlDraftDelaySec()`**: the slider's own value, live.
The readout still shows `Xs - press SAVE` in amber while the draft differs from the board's, so the
draft and what the board is actually running can never be confused.

Labels are formatted by **`fmtTlTime()`**: `45s` below a minute, `1m30s` past it — the recorded
answer for long delays — so a 60 s slider reads `7m0s-8m0s` instead of `420-480s`.

Still open from that group (the answers are in the code comments above `tlSavedDelaySec`): a row's
own duration in the playhead prediction, and the slider relabelled "Default Time Delay (Seconds)".

## 12. Step C — the effect travels with the cell; and the rainbow drifts (17 Oct)

**Effects / Momentum / Distance now travel all the way**: playlist item → playlist JSON →
timeline cell → `TlItem.effect / momentum / distance` (**-1 = "the cell carries none"**) → parsed
in `loadTimeline()` → `applyTimelineRow()` puts them on the **port**
(`PortCfg.effect / momentum / distance`), falling back to the port's **OWN pattern's record** when
the cell carries none. The effect code (`mdEffectOn`, `mdShed`, `mdAdvanceAll`, `mdDotLen`) reads
the **port's** values now, not the selected pattern's — which is what lets one strip in a row have
its own effect while the next strip does not. The page needed **no** change: `timelineDraftJson()`
serialises the whole item object, and `collectCurrentParameters()` has carried the three values
since Step A. A timeline row change / PLAY re-arms the effect, as before.

**The dot restarts ITSELF (user, 19 Oct).** The rules are: **every port follows the timeline**, an
**EMPTY cell is dark for that port**, and two consecutive rows holding the **same item simply
continue** (no restart). The old "a dead dot waits for the next row" contradicted that third
rule, so instead the dot comes back on its own: `MD_RESTART_MS` (3000) after the explosion it is
restored at full size **where it already is and still travelling**, and `mdResetAll()` still
clears any pending restart when a fresh cell is applied. Each port's expiry timer is its own
(`mdExpiredAt[NUM_PORTS]`), so **one strip dying can never disturb another**. The EQ meter is the
deliberate exception — it uses all strips and all LEDs — and will be handled separately.

**Log lines added for exactly this question:** `effect EXPIRED port=… at=… size=…` (one per
death), `effect RESTART port=…`, and `row N cells eff/dist/mom …` at every row change — the last
one lists what each port will use in that row, `-` where the cell carries none (record in force).

`dbgSub` inside `handleLiveTweak()`: 1 = JSON parsed, 2 = scalars read, 3 = section branch
finished, 4 = per-port config rebuilt, 5 = reply about to go. A `prevReq=/live-tweak` with a
`prevSub` below 5 names the branch, the colour parse or `applyTimelineRow` as the culprit.

## 8. Speed: one value, four units (root cause found 17 Oct)

`advancePort()`'s step timer clamps the shared Speed to **seconds per step**:

    unsigned long interval = constrain(c.speed, 0.002f, 5.0f) * 1000.0;   // LED step timer

The Chase's Speed is **LEDs per second** (1-50) and the dot patterns' Speed is **seconds per
step** (0.002-0.5), but `speedLED` is ONE variable. So a Chase Speed of 20 arriving at Moving
Dot means 20 s/step, the timer clamps it to its 5 s ceiling, and the dot crawls — the "it runs
very slowly" report, confirmed twice by the user (Moving Dot and Explosion).

Fix — **WITHDRAWN, and the direction corrected by the user (17 Oct)**: a page-side rescale was
built first (replacing an out-of-range Speed with a per-pattern default on a pattern change).
The user rejected it, and correctly: **Moving Dot's behaviour is baseline-correct and must not
be altered.** Speed = seconds per step is right for the dot family; what is wrong is that the
CHASE — whose Speed is LEDs per second — writes into the same variable and the same port field.
**So the Chase is what must be corrected, not the dot patterns.** The rescale was reverted
(`applyParamRanges()` is byte-identical to the baseline again), and the pattern-range specs
(`PATTERN_PARAMS`) were never touched.

`portCfg[].speed` is read as seconds per step by the generic LED step timer and as LEDs per
second by `stepChase()` — one field, two units. The three ways to fix that, none of them
touching the dot path:

- **(A) Give the Chase its own parameter** (`chaseRate`, LEDs/s) with its own control, stored
  separately through NVS / playlist item / timeline cell, so the shared value is never written
  by the Chase and the dot family never sees a Chase value. Structurally the honest fix; the
  plumbing mirrors the `effect`/`momentum` list in §5.
- **(B) Convert on a pattern change, both ways** (seconds/step <-> LEDs/s = 1/rate), page-side.
  Mathematically equivalent motion, but Moving Dot's stored number still changes — which is
  what the user objected to.
- **(C) The same conversion in the firmware** rather than the page.

Awaiting the user's choice. The `tweak` log line now carries the Speed the page sent, so
whichever fix is chosen can be verified from the log rather than argued about:

The `tweak` log line now carries the value, so the next test proves it end to end:

    tweak LED|moving_dot|4 speed=0.020 bytes=302 heap=…

## 9. `/tail` — the live log, readable WHILE A SHOW RUNS

`logService()` deliberately never writes flash during a show (a LittleFS write is an erase that
freezes both cores), so `/log.txt` goes **deaf exactly when the instrument matters**: a run
whose log stopped at 50 s while the board was at 264 s is what exposed this. `logAppend()`
therefore also keeps the last 40 lines in RAM (`logRecent`, 6.4 KB, written under the same
mutex) and `/tail` serves them in order. `/log.txt` is untouched — it is the durable record;
`/tail` is the live one. `read_board_log.ps1` fetches all three and prints the tail in full.
`/diag` and `/tail` are fetched with caching defeated: Windows will otherwise replay a `/diag`
from ten minutes ago, which cost a diagnostic round.

`req` marks now name every request (`req /init-off heap=…`), `display ON/OFF`, `blank`, and a
page-side `PAGE ERROR` (via `/pagerr`) — so a request, a blanking, a display transition and a
browser exception all land in the same ordered stream.

### What the first read of the log actually showed (v21 run, 595 s)

- **No reset and no stall.** One `BOOT #` line for the whole run; heartbeats every 30 s from
  30 s to 570 s; heap flat 178-184 KB (min 156 KB); no `HEAP FLOOR`, no `[slow]` render; and
  after the run ended, `/diag` reported `prevUpSec=595 prevStage=4 prevReq="none"
  prevHeap=178356`. Stage 4 is the idle tail of `loop()` — the "crash" left no trace of a
  fault at all, which is exactly why IO48 stayed quiet.
- **The LED Display page sends NO `/live-tweak`.** The whole run contains exactly one tweak
  line: `[86s] tweak LED|moving_dot|4 bytes=302` (selecting Moving Dot on the Patterns tab).
  So the colour array of the LED branch — the handover's prime suspect — is never sent when
  the page is opened, and cannot be the cause of a page-open fault. Opening a tab sends only
  `/calibrate-stop` (every tab but Colours) and `/init-off` (leaving Patterns); BOTH blank.
- **The 240V tweak is swallowed by the page.** `liveParamChanged()` uses ONE shared
  `window.liveTimer` for both sections, so `choosePattern()`'s `liveParamChanged('240V')`
  immediately followed by `liveParamChanged('LED')` cancels the 240V request: only the LED
  tweak is ever sent. That is why the log never shows a `240V|…` tweak, and it explains why
  the missing 240V branch in `handleLiveTweak()` went unnoticed — the LED branch writes both
  engines, so the 240V side still followed the pattern by accident. **Restoring the branch
  must therefore be done together with the debounce fix** (one timer per section, or send
  both), or the 240V engine would stop following the pattern selection altogether.

### What the second read showed — and the two fixes it produced (v23, 17 Oct)

The user's report was "Moving Dot was selected, the display went dark a few seconds later".
The log of that run, with the new instruments:

    28s req /settings                                    (page loaded)
    32s req /calibrate-stop      blank displayActive=0    (switching TO the Patterns tab)
    33s req /live-tweak          tweak LED|moving_dot|4 bytes=302
    33s display ON pLED=moving_dot p240=moving_dot        (Moving Dot selected)
    38s req /diag
    38s req /init-off            display OFF              (THE PRESS — 5 s after selecting)
    38s req /calibrate-stop      blank displayActive=0
    47s 59s 71s req /diag                                 (page alive and polling normally)

So the "crash" was the firmware being TOLD to switch the display off: `/init-off`, fired by
the page on the way out of the Patterns tab, with `displayActive = false`. No reset (one
BOOT line, unbroken uptime), no `PAGE ERROR`, no stall. Two fixes followed:

1. **`switchTab()` no longer fires `/init-off` when the destination is a DISPLAY page**
   (`ws2811` = LED Display, `240v`). Those are the pages the pattern is programmed on, so its
   output IS the show; blanking on the way in is the fault. Other destinations still clear the
   Patterns preview.
2. **`handleCalibrateStop()` blanks only when there is something to clear**
   (`wasPreview || !displayActive`). It is sent by EVERY tab switch; blanking unconditionally
   darkened a running show. The `!displayActive` half keeps the original purpose — a stale
   TEST FRAME must still be cleared when no pattern is driving the strips.

Also seen in that run, and NOT explained by any of the above: the run started with
`reason=1 (POWER_ON)` and the RTC erased, and the run before it had been **idle**
(`shows=0`, nothing lit) when it died. So a supply interruption happened with no load
switching at that moment — which rules the strips' inrush out as the cause of that one and
points at the supply/cable. The boot report is the instrument for it: 3 fast flashes always
mean a real reboot, and the slow count after the marker is the reason (1 = power, 9 = brownout,
4 = panic).

---

## 14. THE CHASE FOOTBALL (Wrap OFF) - and the Moving Dot revert (19 Oct)

### The false lead, recorded so it is not repeated

The user's first description of this effect said "Moving Dot with Wrap on is good, Wrap off
is not working as expected". It was about the **CHASE**. Three builds went into Moving Dot
(absolute-speed kick, then a speed-doubling kick) before the board's own log settled it:

    pat led=moving_dot 240=moving_dot tl=1 ports=0000000000

That log line says which pattern every port is REALLY running (`pat led=`/`ports=`), and it
is the fastest way to catch a wrong-pattern assumption. **Read it before changing a
pattern's behaviour.**

Two other things that log taught us, both worth remembering:

* **A kick whose speed is ABSOLUTE is invisible.** The first attempt set the kick's speed in
  pixels/second (Distance / stopping time = ~18 px/s) against a drift already at 91 px/s -
  so the "kick" was five times slower than the thing it was added to. Every kick speed must
  be expressed RELATIVE to the current movement, or bounded by a measurable ceiling.
* **`beats onsets=0 | acts real=0 | flips led=0 | taken 0 0 0 ...`** means no beat reached any
  port, so nothing could move whatever the code did. Check that line before debugging motion.

### The final model (user's words, 19 Oct)

Chase with Wrap OFF is a **football driven by the BASS LEVEL**. Not beat-driven: the level
drives it, which is what the user asked for first and confirmed they prefer.

    Speed     the ROLLING speed - the floor the dot always moves at, so the display never
              stalls. (Was previously a CAP that the level scaled against.)
    Distance  the CREDIT's maximum: the furthest ONE kick may carry the dot (1-50 px).
    Level     how much of that credit is on offer, and how hard the kick is applied.
    Effect    the SPEED of the kicked movement: 1 = twice the roll, 10 = 500 px/s.
    Momentum  the DECELERATION: 0 = full speed until the credit is spent then a dead stop;
              0.5 = the SAME pixels as a gentler ~0.6 s slide.

**The credit is what makes Distance a hard maximum.** Both the fast phase and Momentum's
decay spend the SAME credit, so raising Momentum cannot make a kick travel further - it only
changes how those pixels are delivered. A *rising* level grants `Distance x level` credit
immediately (so the dot answers the bass at once); a spent credit also trickles back at
`Distance / 0.5 s` so a sustained loud passage keeps kicking instead of kicking once.

The ceiling, `KICK_CEIL = 500 px/s`, is the user's own definition of the fastest a dot may
move without the display stepping: the Moving Dot display's minimum Speed of 0.002 s per
step. Effect is stepped LOGARITHMICALLY from 2x roll to that ceiling, so every step is a
similar jump. Effect 0 means no kick - just the roll.

Traps in this code:

* **Chase only.** `isChaseKick()` is `pat == LP_CHASE_1_10 && !wrap`. Chase Wrap ON (the
  metronome) is untouched, and so is every other pattern.
* **Moving Dot was reverted completely** (beat reversal, its own shed/erode effect, Distance
  back to 0-20). If any Moving Dot behaviour looks different from before this session, that
  is a bug, not a feature.
* **The wrap-OFF antifreeze nudge was removed**: the rolling Speed keeps the display alive,
  so the antifreeze's stand-in beat is no longer needed there. It still drives Wrap ON.
* `chaseBeat()`'s wrap-OFF branch is now unreachable - left in place because `chaseBeat()` is
  also the Wrap ON metronome's entry point.
* The rate is still sanitised (a NaN rate once wrote outside the strip buffer); the fallback
  is now `roll`, which is always 1-50 px/s, rather than 0.
* `powf()` in `chaseKickSpeed()` costs about 4 KB of flash. Flash is at 95%; check the build
  size if anything else large is added.

### Effects are PATTERN SPECIFIC - they never collide

The user was explicit: "When a moving dot is selected with effect, the moving dot effect is
added. When the chase effect is requested, the chase effect is added. They are not the same
effect." So:

* `Effects` on **Moving Dot** = the shed/erode effect (distance 0-20, 0 = off).
* `Effects` on the **Chase** = the SPEED of the kick (distance 1-50).
* The two keep separate stored values (`ledPatAll`, per-pattern records) and separate ranges.
* The row label follows the pattern: `PATTERN_ROW_LABELS` / `applyParamRowLabels()` rename it
  to **"Kick Speed"** on the Chase. Any future pattern that reuses these three rows gets its
  own entry in that map and its own `PATTERN_PARAMS` range - never another pattern's.

### Verification (needs the PC on the board's Wi-Fi)

    chase kick p=0 lvl=0.62 roll=20 spd=248 eff=5 cr=7.3/16 mom=0.28 i=0.71

`lvl` should rise and fall with the bass, `spd` is what EFFECT asks for (2x `roll` at 1, 500
at 10), `cr` is the remaining credit and can never exceed the Distance shown after it, and
`i` is the kick's intensity (it decays while the credit is spent, which is Momentum at work).
`kicks=N` in the `beats` line counts armed kicks. On the strips: Chase, Wrap **off**, Speed
~20-50, Effect 3-5, Distance 10-20, Momentum ~0.25 - and remember the stored Chase record
had Effect 0, which means NO kick until it is raised.



### THE THREE AUDIO LEVELS (19 Oct) - and the Bass Level band that was missing

The user's model, now implemented:

| Level | Band | Used by |
|---|---|---|
| **Bass Beat** | 61-250 Hz, time-domain, every 10 ms (lower edge = the Frequency slider, top = `ODF_LP_HZ`) | Moving Dot, Chase Wrap ON. **Do not change - it is known to work.** |
| **Bass Level** | **61-500 Hz** (lower edge = the Frequency slider, top = `FFT_BASSLEVEL_MAX`) | **Chase Wrap OFF**, and the **VU meters** |
| **Full Level** | whole spectrum (`audioLevel`) | the EQ, to come |

The FFT's own "bass" band is 17-61 Hz - **below what the MAX9814 can hear** (the code says so
itself, where the VU meters fold in the mid band). That band is why the Chase "shot in
silence": the level was noise, and `sqrt()` then lifted it (0.07 -> 0.26, i.e. a 26% kick at a
10x speed range). **`audioBassLvl` is that missing band, added as a separate accumulator** so
the bass/mid/treble bands and everything reading them stayed untouched.

With the VU meters switched to `audioBassLvl`, the **mid and treble bands now have no consumer
at all** (they are only logged; the EQ's ten bands are separate). They can be deleted whenever
the user wants, leaving exactly the three levels above.

### The Chase's gate

The gate is `portCfg[p].beatSens * 0.5` - the same per-pattern field the beat threshold uses,
re-used deliberately ("if you can repurpose an unused option then do"): in Chase Wrap OFF the
beat drives nothing, so the field is free there. The row **relabels to "Bass Level"** on the
Chase (higher = more level required). WATCH OUT: it is still the FIELD the Wrap ON metronome
threshold uses (`L.thr = (1 - beatSens) * 60`), so a value chosen as a gate will also change
the metronome's sensitivity when Wrap is switched ON. If that proves annoying, give the gate
its own stored field in `ledPatAll` + the cell + a page row.

The level is smoothed (a shared `g_bassLvlEnv`: 15 ms attack, 150 ms release) and LINEAR above
the gate. The `sqrt()` is gone on purpose: it was for when the level drove the speed directly.

**THE REFERENCE TRAP (found the same day, from "very little response").** The kick was normalised
against a full-scale level of 1.0. **The microphone never produces 1.0** - so even a strong hit
mapped to a small percentage, and the dot barely moved. A VU meter hides this (a bar moving a
little is still visible); a *speed* does not. `KICK_LVL_REF = 0.25` is the level that counts as a
FULL kick, and it is a constant precisely so it can be tuned from `blvl=` in the heartbeat. The
hit detector's threshold also moved off the gated value onto the **raw envelope** (0.04): after
the gate and the reference, a real hit is a small number, so a threshold there misses hits and
the kick never re-arms.

`hb ... bass=0.07 blvl=0.31 env=0.28 ...` now reports both the raw Bass Level and the smoothed
figure the gate sees, so the gate default can be set from real music rather than a guess.

### The gate is AUTOMATIC (19 Oct, after two failed hand-set versions)

A HAND-SET gate cannot work and both attempts failed for opposite reasons:

* gate too low -> it sits inside the room noise (measured: the 61-500 Hz band idles at ~0.10-0.15)
  and the dot shoots in silence;
* gate too high -> **the slider's maximum asked for 0.46 while the music peaked at 0.38**, so
  almost nothing could kick. That was reported as "all I see is a slow moving dot".

So the gate is now derived from the music:

    g_bassSlow  a slow peak of the envelope (rises at once, falls over ~2.5 s, in the audio path)
    peak        max(g_bassSlow, 0.16) - the 0.16 floor is above this room's measured idle level
    gate        peak * (1 - 0.45 * sens)   sens 1 -> 55% of peak, sens 0.2 -> 91%
    ref         peak, so a hit at the music's own full height is a FULL kick at any volume

The row is **"Bass Sensitivity"** on the Chase (a LEVEL pattern) and "Beat Sensitivity" elsewhere:
the same stored per-pattern value, a different meaning, and the map does the relabelling.
**The sensitivity also sets the HIT THRESHOLD** (`riseNeeded = 0.12 * (1.2 - sens)`, floor 0.03):
setting only the gate made the slider feel dead once the gate was already below the music.

### MOMENTUM is a COAST after the kick, not a decay during it

It was invisible because the speed decayed WHILE the credit was being spent - and the credit is
gone in tens of milliseconds at kick speed (10 px at 240 px/s = 42 ms), so the decay never got
going and the dot snapped straight back to the roll. It now works in two phases:

1. the kick: full kick speed until the Distance x hit-strength credit is spent;
2. the coast: the excess speed decays (tau = momentum x 12 s) back down to the roll.

The multiplier was 1.2 s (0.6 s at the slider's maximum) and the user found it far too fast, asking
for "a factor of 10" - hence 12. **The slider's steps are now 0.12 s**, so the usable range is the
bottom of the slider (0.01-0.20 -> 0.12-2.4 s). Momentum 0 = no coast, a dead stop exactly at
Distance; 0.5 = tau 6 s, a very long glide.
**MOMENTUM IS MEASURED IN DISTANCE NOW (final, 19 Oct).** It sets how far the glide after a kick
runs: `Distance x 6 x momentum` - 0 = no glide at all, 0.5 = 3 x Distance. The glide starts **at
the kick speed**, so the dot decelerates smoothly rather than braking and then crawling, and the
decay time follows: `tau = glide distance / starting speed`.

Two earlier attempts and why they failed:

* capping the coast **speed** at `Distance / tau` held the total under 2 x Distance, but it left
  momentum able to change only a 10-pixel crawl -> "momentum still has very little effect";
* a plain 0-6 s settling time let the glide run for hundreds of pixels (kick speed 10 = 500 px/s
  with momentum 0.05 coasted **300 px on a 100-px strip**).
Momentum now scales something the eye can read - DISTANCE - and the total is bounded at
`Distance + 3 x Distance` = 4 x Distance at the slider's maximum.

### CHASE SPEED IS SECONDS PER PIXEL (19 Oct)

`chaseSpeed()` returns px/s for its callers, but the slider is 1-10 **seconds per pixel**: 1 = 1
px/s (fastest the user wants), 10 = 0.1 px/s. "1 px/s is still too fast" - this now matches the
dot patterns, whose Speed is seconds per step. An item saved under the old meaning (pixels per
second, 1-50) simply clamps.

**The kick speed ramp is ABSOLUTE** (`KICK_MIN_PX_S` 40 px/s at Effect 1, `KICK_CEIL` 500 at 10).
It used to be "1 = twice the rolling speed", which is meaningless once the roll can be 0.1 px/s.

### THE THREE AUDIO SOURCES - SET IN STONE (19 Oct)

Declared in the sketch immediately above the `LedPattern` enum, with the pattern-to-source table
written out in full. **There are always three measurements of the music, and a pattern selects one
of them IN CODE.** The three never change; their bands may be retuned.

| Source | Meaning | Band |
|---|---|---|
| `AUD_BASS_BEAT` | onset detector - WHEN a beat lands | Frequency row .. 250 Hz |
| `AUD_BASS_LEVEL` | FFT band level - HOW LOUD the bass is | Frequency row .. 500 Hz |
| `AUD_FULL` | whole-spectrum level | every frequency |

| Pattern | Source |
|---|---|
| Moving Dot, Knight Rider, Explosion | `AUD_BASS_BEAT` |
| Chase, Wrap ON (metronome) | `AUD_BASS_BEAT` |
| **Chase, Wrap OFF (football)** | **`AUD_BASS_LEVEL`** (the Bass Beat onset is its TRIGGER) |
| VU meters | `AUD_BASS_LEVEL` |
| EQ | `AUD_FULL` (not built yet) |

`audioSourceForPattern()` is the single place a source is decided; `patternListensToBeats()` says
whether a pattern consumes onsets (true for the beat sources and for the Chase with Wrap OFF).

**DO NOT CONFLATE THE THREE SOURCES WITH THE BAND.** A misreading of "the music output Bass Beat,
Bass Level and FFT need to remain constant. It is the patterns themselves that need to adjust their
sensitivity" once produced `L.chain = 0` for every listener - which silently ignored the per-pattern
**Frequency** row. The three SOURCES are what must remain constant; the per-pattern band is a real
setting and is honoured by `chainForBand(portCfg[p].freqHz)`.

**THE TRICKLE WAS THE CULPRIT (19 Oct).** Even after the coast was added, momentum at maximum
still finished "in a fraction of a second". The cause was the continuous credit top-up
(`KICK_REFILL`, Distance px per 0.5 s): it refilled the moment the kick had spent the credit, so
`chaseCredit > 0` was true again on the next frame, the code went straight back into the kick
branch and the coast was cancelled. **There is no trickle any more** - the credit comes only from
hits (a level rise or a Bass Beat onset). If a top-up is ever added back, this is what will break.

### THE HYBRID TRIGGER, TIMED TO THE CENTRE OF THE BEAT (19 Oct)

The onset decides WHEN; the level decides HOW HARD (the credit is Distance x n). But **WHEN the
level is read matters**, and that was the real bug:

* the onset detector fires at the **leading edge** of the beat;
* the Bass Level envelope takes ~80 ms to rise.

So granting the credit on the onset took the level from **before** the beat - small kicks, and
often no kick at all, which is the "still missing a lot of beats even with sensitivity at max".
A beat now arms `chaseGrantAt[p] = now + 100 ms` and the credit is taken from the level at that
moment: the **centre (peak) of the beat**. The LENGTH of the beat therefore has no bearing on the
distance - a long note does not push the dot further.

One `hit` per beat: the level's own rise is only a fallback (a steady groove barely rises between
hits), rate-limited by `CHASE_FALLBACK_GAP_MS` = 200 ms so the rise that follows an onset cannot
fire the same beat twice. Antifreeze pulses arm a grant too; the gate keeps them silent in a quiet
room.

**Frequency matters here**: at 100 Hz the detector was discarding the kick drum's fundamental -
the very thing it times on. The user found 50 Hz made "a big difference to the number of beats
recognised". Keep the Chase's Cut Off Below Frequency low (40-60 Hz).

### A SAVED CELL COULD SILENTLY DEAFEN A PATTERN (19 Oct)

"when I play the timeline my moving dot is no longer responding to the beat. That is a major
fault."

A timeline cell OWNS its port while the timeline plays, and it carries its own `beatSens`. These
cells were saved when Bass Sensitivity sat at **0.09**, i.e. `thr = (1 - 0.09) x 60 = 54.6` counts
- and a band peak only reaches **5-40 counts** in music. So onsets stopped dead the instant PLAY
was pressed, while the page worked (slider at maximum -> thr 0). The Chase survived it because its
kick has a LEVEL fallback; Moving Dot has no fallback, so its beat reversal simply stopped. **The
Chase looking fine while Moving Dot was dead was the clue.**

Fixed by making the item's PATTERN the floor for sensitivity: a cell may be MORE sensitive than its
pattern, never less. Nothing saved earlier can deafen a pattern again. (The underlying scale is
fine: the flux and the threshold are both in ADC counts - the comment in the audio core says band
peaks reach 5-40, which is why 54.6 blocks everything.)

### ORIENTATION, AND FOUR TRAPS UNDER IT (19 Oct)

**Orientation** is a checkbox in the Visual block that runs a pattern from the OTHER END of the
strip. For the Chase with Wrap OFF the dot STARTS at the far end and travels toward pixel 0; kicks
push it that way; reaching 0 it restarts at the far end. It is stored with the pattern, with the
playlist item, and in the timeline cell (the cell's own value wins; -1 = "carries none, inherit the
pattern"). Greyed for Moving Dot and for the Chase with Wrap ON, and `ledPatNormalise` CLEARS the
value in both cases, so a stale setting can never act on a greyed control. Explosion is live and
inert - its meaning is still to be defined.

**TRAP 1 - a JSON BOOLEAN is not a number.** `jsonNumI()` reads strings and numbers only. `orient`
arrives from the page as a real boolean, fell through to its default, and read back as 0 for ever -
the toggle did nothing at all. Use `.as<bool>()` for booleans.

**TRAP 2 - a zero threshold listens to the room.** Bass Sensitivity at MAXIMUM gives
`(1 - 1) x 60 = 0`, and a zero threshold fires on ANY novelty above zero, so the Wrap-ON metronome
beat on the noise floor: "a mess of flashing lights in silence". The threshold now has a floor of
**5 counts** - band peaks measure 5-40 - so every musical beat still passes and the room does not.
That ONE FIELD serves two jobs though: the beat threshold (Wrap ON) and the level gate (Wrap OFF),
where a HIGH value is wanted. Splitting it is the clean fix if they fight again.

**TRAP 3 - `white-space: nowrap` beats a fixed column width.** `.param-table td.name-col` was
`nowrap` WITH `width: 130px`. A table cell cannot be narrower than its unwrappable content, so the
long JS label "Cut Off Below Frequency" widened that column to about 200px and squashed the colour
swatches beside it - which looked exactly like "the swatches are the wrong size", and two attempts
to fix the SWATCHES made it worse. The column is `white-space: normal` + `overflow-wrap: anywhere`
now, so long labels WRAP and the width holds. The rule for this page, in the user's words: **wrap
the text, never change the column width.**

**TRAP 4 - shell text substitution on source files.** Two batches of edits were made with PowerShell
`.Replace` and the escape sequences (`"+"\`r\`n`) were written LITERALLY into the .ino, corrupting
`stepChase` and the timeline restart. The file was restored from `baseline-memory-2026-10-19` and
redone with the file editor. A SINGLE-LINE literal replacement with no escapes is safe; anything
multi-line is not. **Use the editor for source.**

### THE MOVING DOT FREEZE, AND THE USB LOG PATH (20 Oct)

**SYMPTOM:** with the timeline playing, the Moving Dot travelled to one end and HELD there - "more like the
Chase with wrap on" - while its colour and every value stayed correct. Page preview was always fine, so it
looked intermittent.

**CAUSE (measured, not guessed):** the per-port `/diag` line showed `pat=0 sp=0.0020 dot=10 lost=0 dead=0
cell=moving_dot gs` climbing ~196/s while **`pos` sat at 0 and `age` grew to 3 s**: the dot was stepping and
the POSITION never changed. With `orient=1` and Wrap OFF, advancePort's end handling restarts the dot at
pixel 0 and does **not** flip the direction, so it steps to -1 and is put back, for ever. That state needs
`orient=1` (at orient=0 the restart end is the FAR end). So the precondition was an orientation reaching a
Moving Dot port - and it arrived via `wantOrient`'s fallback to **`ledSel()`, the pattern SELECTED ON THE
PAGE** (the Chase, whose Orientation was ticked). The timeline was applying another pattern's parameter.
Fix: a cell with no orientation of its own now means NO orientation.

**THE RULE THAT CAME OUT OF IT:** the AI may not change the behaviour of patterns without permission. (I
had made the dot reverse at the ends - inventing a behaviour. The BASELINES were the answer: the dot never
wraps, and reverses ONLY on a music beat.)

**WHY SERIAL READINGS KEPT FAILING:** `USBMode=hwcdc` makes the sketch's `Serial` the hardware CDC, which
resets/drops when a host asserts DTR - so a host reads the boot banner and then nothing, and a command
looks ignored. The build script's own comment already said `USBMode=default   the USB-serial log path`:
the knowledge existed and was lost between sessions. Both scripts now use `default`.

**`CCMP replay detected` FLOOD:** an iPhone with Private Wi-Fi Address ON uses a rotating MAC, and this
access point logs a replay error continuously - hundreds of lines that bury everything (and make the serial
Monitor look dead). `esp_log_level_set("wifi", ESP_LOG_NONE)` in setup silences it; turning that iPhone
setting off for this network fixes it at source. Unrelated to any pattern.

**SERIAL COMMANDS (no Wi-Fi needed):** `D` prints every port's state - `pat sp sz ef ds mo or lost dot dead
fre cell pos dir dark age gs` - and `L` dumps the log. The STALL watchdog writes the same fields whenever a
non-dark port has not advanced for 4 s, so an unattended stall is recorded. read_board_log.ps1 needs the PC
joined to the ESP32's own AP, which Windows will not hold while that AP has no internet; the Arduino Serial
Monitor is the practical tool.

**STILL OPEN (20 Oct):**
- `wantSens` in applyTimelineRow still falls back to `ledSel()` - the SAME class of fault as the orientation
  one, and the last place a port can take a parameter from the page's selected pattern.
- A timeline cell does not carry every parameter: `attack`, `decay`, `peakDelay`, `peakSpeed`, `peakDir`,
  `vuDir` and `layout` are in the playlist item's JSON but are NOT loaded into `TlItem`, so they still come
  from the pattern's own record.
- A cell is a COPY taken when the item is placed: editing the playlist item afterwards does NOT reach the
  timeline. The intended model is that the playlist entry is the master.

### MEMORY, PARTITIONS AND THE BOARD OPTIONS (19 Oct)

The board is an **ESP32-S3 N16R8**: 16 MB flash and 8 MB OCTAL PSRAM. The default board options
assume 4 MB and no PSRAM, which is why the sketch sat at **96% of a 1.25 MB app slot with 47 KB to
spare**. RAM was never the problem (30% static, ~150 KB free heap, minimum steady over ten minutes,
no leak).

Fixed in the fqbn, now in BOTH `wls_build.ps1` and `wls_upload.ps1` - and they MUST MATCH, because
the partition scheme decides the flash layout:

    esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=huge_app,PSRAM=opi,FlashMode=qio,USBMode=hwcdc

Result: a **3,145,728-byte app slot with the sketch at 40%, 1.88 MB free**. The same five settings
belong in the IDE's Tools menu, or the IDE's builds will differ from these scripts'.

**CHANGING THE PARTITION SCHEME REFORMATS LittleFS AND MOVES NVS** - the saved settings and the
timeline are wiped. Back them up first via `/load-timeline`, `/load-playlist` and `/settings`
(they land as `board_*.json`, which `board_timeline.json` / `board_playlist.json` already ignore).

### THE TIMELINE LOCKS THE TABS (19 Oct)

"The timeline must be stopped before the other tabs become available." `applyTimelineLock()`
already disabled PLAY, SAVE and the delay; it now disables **every tab except Timeline** too.
Timeline stays live BECAUSE STOP LIVES THERE - disabling it would trap the user inside a running
show. The tab buttons carry `data-tab` attributes so the lock can find them; the existing
save/restore returns each tab to its PREVIOUS state (the 240V and LED Display tabs are already
disabled until a pattern is chosen, and must come back disabled); and `switchTab()` carries a hard
guard so a click inside the one-poll window after PLAY is refused rather than silently ignored.

**A LESSON ABOUT GREPS.** The lock was wrongly reported as missing because the search was truncated
at 30 hits and `color-block` / `timeline-block` filled it, so it never reached the line that calls
it. When something "does not exist", first check that the search was not cut off.

### TIMING: WHERE TWO IDENTICAL CELLS COULD STILL DIVERGE (19 Oct)

Measured on hardware with two ports running the same playlist entry: the parameters applied to both
were **IDENTICAL** - speed 1, size 10, qty 2, wrap 0, effect 10, distance 20, momentum 0.3,
beatSens 1, freq 50, checked field by field in `/diag` - yet port 0 had advanced **8367** pixels
against port 1's **8355**, with the chase accumulators 0.72 against 0.83.

The cause was the last per-port asymmetry in the chain: the Chase's onset hit was held as **per-port
pending state** (`chaseGrantAt[p]`), and a row change **wiped it per port**. A grant pending on one
port and not the other cost that port a kick - a whole Distance - which showed as "the chase on port
1 is a full dot length different to port 2". `kMiss` in `/diag` had recorded 44 such wipes.

**THE RULE: a beat is a property of the MUSIC, so a hit is decided ONCE PER FRAME and every port
that should take it takes it in that same frame.** There is now NO per-port hit state at all
(`g_chaseOnsetHit`, `g_chaseFallback`), so nothing can be wiped unevenly. Strength stays per port
(from `n`), so different parameters still behave differently - only identical cells are forced to
agree, and only because they are fed identical data. The user's words: *"sync should just be a
byproduct, not a written exception to the code"*.

Diagnostics that proved it, all in `/diag`:

* `st0`/`st1` - pixels advanced. **Must be EQUAL for identical cells.**
* `v0xx`/`v1xx` - the parameters actually APPLIED, so "identical cells" is verified, not assumed.
* `pairDiff`/`pairA`/`pairB`/`pairRow` - worst position divergence and the row it happened in.
* `sP0..sG3` - per-port position, direction, chase accumulator and step age.

A **full dot length** apart = a missed or extra kick. A few pixels (0-4) = the fractional phase, and
normal. Do not "fix" a few pixels with an exception in the code; that was explicitly rejected.

### AN ITEM'S NAME DECIDED ITS PATTERN - AND EXTRA WORDS BROKE IT (19 Oct)

"the chase is not moving as it should to music", while the SAME parameters on the page worked.
The live counters settled it in one read:

    bOns 198   bActs 1795   bFlips 204   <- detector healthy, Moving Dot working
    bKicks 0                              <- the Chase never kicked in two minutes
    p0pat 0                               <- that port was MOVING DOT

`bKicks 0` with `bOns 198` can only mean **no port was ever a Chase**, so `stepChase()` never ran.

The cause: a timeline item's pattern is resolved from its **name**, and `patternIdFromName()` did
EXACT matching only. Items are named freely - "Chase Green x2", "Moving Dot Red Fast" - so none of
those matched, and the fallback returns the name's own lowercased text, which matches no pattern.
`renderBranchFor()` then **degrades an unknown id to MOVING DOT** - as its own comment warns. So a
Chase item played as a Moving Dot. On the PAGE it worked because the page sends the exact pattern
name ("Chase"), which the exact matcher does resolve.

Fixed with a **CONTAINS pass** in `patternIdFromName()`, after every exact match and before the
fallback, so any name that MENTIONS a pattern resolves. An exact name still wins, so nothing that
worked changes. `/diag` now also reports `p0pat..p3pat` (what each port is really running) and
`c0id..c2id` (what the current row's cells are called), so this is verifiable in one read.

**Any future mapping of names to patterns must keep the contains pass, or a freely-named item will
silently degrade to Moving Dot again.**

### EVERY PORT RESTARTS TOGETHER (the gap between display 1 and 2)

`adoptTimelineRow` used to reset a port's motion **only if that port's motion had changed**, so
with a timeline row loaded (whose cells legitimately differ per port) some ports restarted to 0
while others carried on - a permanent phase gap. It now sets `g_restartMotion` and restarts ALL
ports once, after the loop, including the chase's own state. The reset is deliberately not done
inside the loop: that is what caused the gap.

### Local baseline

`baseline-chase-2026-10-19` (local tag, commit 9378fce) is the version the user said "did start to
give the effect I'm looking for", taken BEFORE the momentum coast. It is not pushed.
