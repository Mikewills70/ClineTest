// ============================================================================
//  link_test.ino  -  CLEAN-SLATE HARDWARE LINK TEST
// ----------------------------------------------------------------------------
//  Purpose: prove the PC can build and upload to this board, and that the
//  IO48 indicator and 240V channels 1-4 can be driven directly.
//
//  NO audio. NO Wi-Fi. NO libraries. NO NVS/Preferences.
//  NO include of, or reference to, any previous file in this project.
//
//  Behaviour, repeating forever:
//    channel 1 flashes 100 ms, pause 500 ms,
//    channel 2 flashes 100 ms, pause 500 ms,
//    channel 3 flashes 100 ms, pause 500 ms,
//    channel 4 flashes 100 ms, pause 500 ms,  -> back to channel 1
//    IO48 is ON for the same 100 ms as each channel.
//
//  Timing: one step = 100 + 500 = 600 ms.  Full cycle = 4 x 600 = 2400 ms.
//
//  Board:  ESP32-S3 N16R8 (16 MB flash, 8 MB octal PSRAM), port COM5
//  FQBN:   esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=huge_app,
//          FlashMode=qio,USBMode=hwcdc
// ============================================================================

// ---------------------------------------------------------------------------
//  PINS  -  taken from HARDWARE_CONNECTION.md
// ---------------------------------------------------------------------------
const uint8_t IO48_PIN = 48;   // onboard status LED, one flash per channel

const uint8_t CH_PINS[4] = { 11, 12, 13, 14 };   // 240V channels 1, 2, 3, 4
const uint8_t NUM_CH = 4;

// ---------------------------------------------------------------------------
//  TIMING  -  every flash is 100 ms long, followed by a 500 ms pause
// ---------------------------------------------------------------------------
const uint16_t FLASH_MS = 100;   // 100 ms is clearly visible; 300 ms merges
const uint16_t PAUSE_MS = 500;   // pause after each flash

// ---------------------------------------------------------------------------
//  setup  -  drive everything LOW before making the pins outputs, so nothing
//  glitches on at power-up.
// ---------------------------------------------------------------------------
void setup() {
  digitalWrite(IO48_PIN, LOW);
  pinMode(IO48_PIN, OUTPUT);

  for (uint8_t i = 0; i < NUM_CH; i++) {
    digitalWrite(CH_PINS[i], LOW);
    pinMode(CH_PINS[i], OUTPUT);
  }
}

// ---------------------------------------------------------------------------
//  loop  -  chase 1, 2, 3, 4 and repeat.  Deliberately simple: the channel is
//  written AT the flash, never from a slow engine, so the 100 ms window is
//  always honoured.
// ---------------------------------------------------------------------------
void loop() {
  for (uint8_t i = 0; i < NUM_CH; i++) {
    // --- flash: channel and IO48 on together for 100 ms ---
    digitalWrite(CH_PINS[i], HIGH);
    digitalWrite(IO48_PIN, HIGH);
    delay(FLASH_MS);

    digitalWrite(CH_PINS[i], LOW);
    digitalWrite(IO48_PIN, LOW);

    // --- pause: 500 ms, everything off ---
    delay(PAUSE_MS);
  }
}
