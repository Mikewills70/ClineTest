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

**Still to come (Step C):** Effects / Momentum / Distance in the playlist item and the timeline
cell, so a cell can carry an effect per strip and a row change re-arms it.

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

