# HANDOVER — LED Display Colour Selection

> **READ THIS FIRST.** This is a handover for the next session.
> **Do not make changes until the state has been read and a plan agreed.**

---

# 1. WHERE THINGS STAND

**Repository:** `github.com/Mikewills70/ClineTest` (private)
**Local:** `d:\Dell 7720\Arduino\Desktop Disco\10 Channel Controller\WIFI_LightShow`

**Baselines (all pushed):**

| Tag | State |
|---|---|
| `baseline-2026-10-14` | first working baseline |
| `baseline-led-speed-2026-10-15` | 200 fps, all ten ports, stall-free |
| `baseline-beat-2026-10-15` | onset-detection beat response |
| `baseline-rgb-16-ports-2026-10-15` | X16 driver, all ten strips, no dropped pixels |
| **`baseline-colours-2026-10-15`** | **32 colours stored independently; device is master on startup** |

**Hardware:** ESP32-S3, ten WS2811 strips, 100 addressable LEDs each, driven by
NeoPixelBus `NeoEsp32LcdX16Ws2811Method` (LCD_CAM parallel, one DMA transfer for
all ten strips). **No RMT anywhere.**

**Verified working:** all ten strips, ~217 frames/second, `skip=0` (no dropped
pixels), `tx` ~4,450 µs, `copy` ~78 µs, heap flat.

---

# 2. THE WORKING METHOD — follow this

When a fault is reported:

1. **Do not guess.** Guessing wasted many hours and is explicitly not wanted.
2. **Read the code line by line and look for MULTIPLE reasons the fault could
   exist, not just one.** List them as candidates.
3. **Determine the variables required to separate those candidates** — what needs
   logging, and at which stage.
4. **Add only the instruments, then give the test parameters** and say exactly
   when to say "read it".
5. **Fix, or discuss.** Do not change five things and hope.
6. **Once fixed, remove all test code and scaffolding.**

**Two hard-won lessons:**

- **Verify the instrument before acting on a surprising reading.** Twice my own
  logging was wrong (a timer computed before the call it measured, and a
  mislabelled lookup table). Both cost a round.
- **The user's hardware reasoning has been more reliable than my measurements.**
  Several faults were found by them reasoning about the driver or system rather
  than by instrumentation.

---

# 3. BUILD, FLASH AND READING THE LOG

```
$cli='C:\Program Files\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe'
compile --fqbn esp32:esp32:esp32s3 --board-options CDCOnBoot=cdc
        --libraries "D:\Dell 7720\Arduino\libraries"
        --build-path "$env:TEMP\wls_build_v5" "<sketch dir>"

upload -p COM5 -b esp32:esp32:esp32s3 --board-options CDCOnBoot=cdc
       --input-dir "$env:TEMP\wls_build_v5"
```

**Reading the log:** `read_log_serial.ps1` opens COM5, sends `L`, captures the
output. It writes `board_log.txt`, so it counts as a file modification.

Live read without writing files:

```powershell
$port=New-Object System.IO.Ports.SerialPort('COM5',115200,'None',8,'One')
$port.DtrEnable=$false; $port.RtsEnable=$false; $port.Open()
Start-Sleep -Milliseconds 400; $port.Write('L')
$sb=New-Object System.Text.StringBuilder; $end=(Get-Date).AddSeconds(16)
while((Get-Date) -lt $end){ if($port.BytesToRead -gt 0){ [void]$sb.Append($port.ReadExisting()) }; Start-Sleep -Milliseconds 150 }
$port.Close()
($sb.ToString() -split "`n") | Where-Object { $_ -match 'KEYWORD' } | Select-Object -Last 12
```

**IMPORTANT — git pushes:** git writes progress to **stderr**, which makes
PowerShell abort the rest of a command, so tag pushes silently half-fail. Use:

```
cmd /c "git push -f origin refs/tags/<tag> 2>nul"
```


---

# 4. CRITICAL LIBRARY FACTS

**NeoPixelBus is PATCHED and the patch lives OUTSIDE the repo** — see
`LIBRARY_PATCHES.md`. `NeoEsp32LcdXMethod.h` needs `gpio_hal_iomux_func_sel()`
replaced with `esp_rom_gpio_pad_select_gpio()` for ESP-IDF 5.x.
**Reinstalling the library loses this patch.**

**The driver MUST be the X16 variant:**

```cpp
typedef NeoPixelBus<NeoGrbFeature, NeoEsp32LcdX16Ws2811Method> Ws2811Strip;
```

The X8 variant sets `BusMaxCount = sizeof(uint8_t) * 8 = 8` mux channels and
returns `InvalidMuxId` beyond that. Each strip registers its own mux, so with
**ten** strips ports 9 and 10 were never encoded into the DMA — the render ran,
pixels were set, flags were correct, and nothing lit. **Invisible to every
application-layer measurement.**

**FastLED cannot be used here** — 3.10.7 ships its S3 parallel driver with all 12
files `.disabled`.

**`NeoGrbFeature` is a GRB device.** Calibration order 3 swaps G and B; the two
cancel, which is why order 3 renders as RGB on these strips.

---

# 5. THE MODEL THAT IS ALREADY BUILT

## Beat detection
Onset detection function: band-pass 40–250 Hz (4-pole) → rectify →
**frame-to-frame** difference (10 ms window) → **absolute** threshold.
No normalisation and no local mean — both made it hypersensitive in dense mixes.

The dot is latched from the **onset**, never from the tempo clock. Previously the
display was a metronome: onset → tempo → clock → lights.

- `beatSens`: global, **reversed scale** (higher = more beats), default 0.20.
  `thr = (1 - beatSens) * 60`
- `beatOffsetMs`: 20 ms
- **Outstanding: the bass response still needs work.** The tempo diagnostics are
  deliberately still in the build for this.

## Colours and themes
- **Four themes × eight colours = 32, stored independently.** Each slot is its own
  NVS key (`th0c0`…`th3c7`).
- **The device is the master on startup.** `/settings` returns `themeColorsAll`
  (all four themes) and the UI adopts them before display. Editing is provisional
  until saved.
- **Repeating 1–4 into 5–8 on three themes is a deliberate user choice** — the
  per-slot store supports either approach.
- `/settings` used to return only the *current* theme, which let corrupted values
  recirculate. That was the cause of "lost" colours.

## Calibration
- Per-port colour order, stored per port (`port_ord_N`).
- **TEST shows the STORED value, read from the device** (`/calibrate-show`).
- **SAVE stores what is on the strip** (`/calibrate-savecurrent`).
- Changing port blanks the strips (`/calibrate-stop` sets `blankReq`).
- Save shows an off → on → off flash.
- Table: `0:RGB 1:GRB 2:BRG 3:RBG 4:GBR 5:BGR`. Index 3 is correct for these
  strips.

## Audio
- Sample rate ~17.4 kHz. `FFT_SAMPLES` 1024.
- **ADC-DMA is disabled** (`AUDIO_USE_DMA 0`) — `adc_continuous_start()` reported
  success but delivered nothing. `adc=` in the heartbeat reports which path is
  live.
- `logAppend()` writes to **both** the RAM ring and USB serial. The flash log is
  deliberately **not** drained while a display runs: a LittleFS write is a flash
  erase, which freezes **both** cores for 20–40 ms. **That was the display stall.**


---

# 6. THE WORK TO DO — COLOUR SELECTION FOR THE LED DISPLAY PAGE

## What exists now
The LED page has a **large custom-colour box** (`paramColourLED0..3` — four boxes,
with 2–4 hidden for non-VU patterns). The firmware holds `colourLED[4]` and
`PortCfg.colour[4]` as **raw `uint32_t`** — **there is no reference model yet.**

## The reference model to build

A colour is stored as a **reference**, not a resolved value:

| Stored | Meaning | On theme change |
|---|---|---|
| `"T0"`…`"T7"` | theme palette index | **resolves live — updates** |
| `"RB"` | rainbow | **fixed — warns on selection** |
| `"#RRGGBB"` | custom | **fixed — warns on selection** |

**Storing a reference is what makes theme changes flow through** to every stored
playlist item and timeline cell. A custom or rainbow selection is a value, so it
stays put — hence the warning. **The warning fires for BOTH rainbow and custom**
(confirmed by the user).

## The UI change — the main task

**Remove the large custom box. Replace with TEN smaller boxes occupying the SAME
total width on the row:**

- **Boxes 1–8 — the eight theme colours, in theme order**
- **Box 9 — rainbow**
- **Box 10 — custom colour**

**The swatch row needs a light border so black/dark theme colours are visible**
against the dark UI.

## The VU meter / EQ — ANSWERED, DO NOT RE-ASK

**The VU meter has four colour boxes** — three body sections plus the max peak.
**Each of those four, when selected, opens the same ten options** (8 theme +
rainbow + custom). So any part of the VU meter or EQ can be defined from a theme
colour, rainbow, or custom.

**Rainbow blending is critical:** if all the body parts are set to rainbow they
must render as **ONE continuous rainbow**, not restarting per zone. The rainbow
must therefore be computed from **LED position along the strip**, not per zone.

**Also required — a toggle for rainbow start points:**

- **One blanket rainbow** — a single continuous rainbow across the whole meter
- **Individual start points** — each zone's rainbow starts at a different point

## Also outstanding
- **"Confirmed" button and gating:** Patterns and 240V stay greyed out until every
  port has a saved calibration and every theme has its colours stored. **Black
  does not count as saved.**
- **Reset ports 2–10** so each must be recalibrated deliberately.
- **A per-port "calibrated" flag** — a cleared port currently reports `2` as if it
  were a saved setting.
- **Timeline icons** — static, one per pattern; tap shows the item name, tap the
  name jumps to the playlist item for editing.
- **Peak hold** — three sliders (hold delay, fall-back speed, size 1–3 px), never
  falls below the body.
- **Graphic EQ** — 10 bands (vertical/STAR) or 100 bands (horizontal, J1→A1
  matrix), smooth↔stepped blend, colour by height.
- **Live preview** of the selected strip while editing, all strips once stored.

---

# 7. THINGS TO BE CAREFUL OF

- **Do not touch the 240V path** unless asked. Its clamp is `< 20` ms and is
  deliberately unchanged.
- **Do not reintroduce frame skipping** in `ledShowAll()`. `Show()` must block —
  that guarantees every pixel is drawn. A skipped frame loses a pixel position
  because the step has already advanced.
- **Arduino auto-prototype trap:** the IDE inserts prototypes above the sketch's
  first function. Any new function whose **signature** mentions a type declared
  below that point (e.g. `PortCfg`) breaks the whole build. Keep such functions
  below the type declarations.
- **A frame transmit is a hard ~4.4 ms** and cannot be shortened by code.
- **`CAL_TEST_MS = 3000`** on the calibration test may be too short.
- **Read and push access to the repo works directly** — credentials are cached.
  **Do not create a baseline unless the user asks.**
- **The `/leaktest` handler is deliberately retained for now.** Review it
  periodically and advise the user when it looks redundant.
