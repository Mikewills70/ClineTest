# Required library patch — NeoPixelBus

**A baseline tagged in this repo is NOT reproducible without the patch below.**

All 10 strips are driven by NeoPixelBus's `NeoEsp32LcdX8Ws2811Method` (LCD_CAM
parallel output, ESP32-S3 only, all 10 channels in one DMA transfer). There is no
RMT anywhere in the current build.

## The patch

File: `libraries/NeoPixelBus/src/internal/methods/NeoEsp32LcdXMethod.h` (~line 545)

NeoPixelBus 2.8.4 does not build against ESP-IDF 5.x. `gpio_hal_iomux_func_sel()`
no longer exists in IDF 5, so the call must be replaced:

```cpp
// before (IDF 4.x only):
gpio_hal_iomux_func_sel(GPIO_PIN_MUX_REG[pin], PIN_FUNC_GPIO);

// after (IDF 5.x):
esp_rom_gpio_pad_select_gpio(pin);
```

**Reinstalling or updating the NeoPixelBus library LOSES this patch.** If a build
suddenly fails with an error about `gpio_hal_iomux_func_sel`, re-apply the above.

## Why not FastLED

FastLED 3.10.7 ships its ESP32-S3 parallel clockless driver with all 12 files
renamed `.disabled` and absent from `lcd_cam/_build.cpp.hpp`, so FastLED can only
fall back to RMT on this board. NeoPixelBus is the only usable parallel driver.

A `build_opt.h` forcing FastLED's parallel driver was tried and deleted — it
cannot work, because the driver is not present in the released library.

## Driver-capability note

A single ten-strip parallel transmit takes a **hard 4.7 ms** (100 LEDs x 24 bits x
1.25 us = 3 ms of bit time, plus overhead). That caps the frame rate at roughly
213 fps, and the frame rate is what sets the maximum dot speed — one pixel is
advanced per frame. Pushing past ~200 fps leaves no headroom.
