#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <nvs_flash.h>
#include <ArduinoJson.h>
// ============================================================================
// RMT IS DISABLED. THE PARALLEL (LCD_CAM / I80) DRIVER IS THE ONLY OUTPUT.
// ============================================================================
// The ESP32-S3 has only FOUR RMT TX channels, so ten strips on RMT must be sent in
// groups of four, waiting for each group to drain. On this board that leaves only
// the first strip of each show ever updating - which is the RMT channel limit, as
// warned from the start.
//
// The ESP32-S3's LCD_CAM peripheral in I80 mode clocks every strip out
// SIMULTANEOUSLY in hardware: no channel limit, no grouping, nothing to drain
// between strips. FastLED supports it natively on this chip
// (FASTLED_ESP32_HAS_I2S_LCD_CAM defaults to 1 for ESP32-S3).
//
// Two independent settings are needed:
//   * FASTLED_ESP32_HAS_RMT 0          - remove the RMT driver from the build
//     altogether, so there is no 4-channel path left to fall back to.
//   * FASTLED_ESP32_FORCE_I2S_LCD_CAM  - raise the I2S LCD_CAM driver to the
//     highest priority in FastLED's channel manager so it is the one selected.
// Both must be defined BEFORE <FastLED.h>.
#define FASTLED_ESP32_HAS_RMT 0
#define FASTLED_RMT5 0
#define FASTLED_ESP32_FORCE_I2S_LCD_CAM 1
#include <FastLED.h>
#include <arduinoFFT.h>
#include <esp_timer.h>
#include <esp_task_wdt.h>

// Maximum playlist items
#define MAX_PLAYLIST_ITEMS 20

// History size for audio beat prediction
#define HISTORY_SIZE 10

// WS2811 Configuration Constants
#define NUM_WS2811_PORTS 10
#define NUM_LEDS_PER_STRIP 100 // 300 physical LEDs mapped to 100 addressable IC nodes

// Paired Playlist Item Structure (240V + WS2811 + Off/On States)
struct PlaylistItem {
  char name[32];
  bool selected;
  
  // 240V Side
  bool enable240V;
  char pattern240V[32];
  bool audioEnabled240V;
  int antifreeze240V;
  float speed240V;
  int qty240V;
  int size240V;
  char music240V[16];
  int freq240V;
  int mult240V;
  float attack240V;
  float release240V;

  // WS2811 Side
  bool enableLED;
  char patternLED[32];
  bool audioEnabledLED;
  float speedLED;
  int brightnessLED;
  char paletteLED[16];
  char musicLED[16];
  int freqLED;
  int qtyLED;
  int sizeLED;
  int multLED;
  float attackLED;
  float releaseLED;
};

#endif