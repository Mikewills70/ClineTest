#define HTTP_MAX_SEND_WAIT 20000
#define HTTP_MAX_CLOSE_WAIT 20000
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <FastLED.h>
#include "config.h"
#include "index.h"
#include <LittleFS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// ==========================================
// CONFIGURABLE SETTINGS & HARDWARE PINS
// ==========================================
const char* apSSID = "ESP32-LightShow";
const char* apPassword = "12345678";
const int AUDIO_PIN = 2;                        // MAX9814 microphone (ADC1)
const int POWER_LED_PIN = 48;                   // Onboard status LED

#define NUM_LEDS_PER_STRIP 100                  // 100 physical IC nodes per strip
#define NUM_PORTS 10
#define CHIPSET WS2811

const int ledDataPins[NUM_PORTS] = {10, 9, 8, 18, 17, 16, 15, 7, 6, 5};
CRGB leds[NUM_PORTS][NUM_LEDS_PER_STRIP];

int portColorOrders[NUM_PORTS] = {2, 2, 2, 2, 2, 2, 2, 2, 2, 2}; // Default to BRG

// --- LED OUTPUT -----------------------------------------------------------
// FastLED drives all ten strips through the ESP32-S3's PARALLEL output driver
// (LCD_CAM / I80), selected in build_opt.h. No RMT anywhere in this project.


uint8_t ledBrightnessScale = 255;    // master brightness as 0..255

// NOTE: ledShowAll()/ledClearAll() are defined further DOWN, after the pattern
// type declarations. The Arduino builder inserts its automatic prototypes just
// before the FIRST function definition in the sketch, so defining a function up
// here would push every prototype above "enum LedPattern" and break the build.

const int channelPins[10] = {11, 12, 13, 14, 21, 38, 39, 40, 41, 42};

Preferences preferences;
WebServer server(80);

// ---- Global runtime settings ----
bool audioEnabled = true;
float clockDelay = 1.0;
int antifreeze = 2;

// Separate active pattern per section (240V relay channels vs WS2811 strips).
// Values are internal pattern IDs (e.g. "moving_dot", "chase_1_10", "vu_meter").
String activePattern240V = "moving_dot";
String activePatternLED  = "moving_dot";

// ---- Per-section pattern parameters (updated live via /live-tweak) ----
float speed240V = 1.0;  int quantity240V = 1; int size240V = 1;
int   freq240V  = 50;   int mult240V     = 2; String music240V = "BASS";
float attack240V = 0.15; float decay240V = 0.01;
bool  wrap240V = false; bool invert240V = false;

float speedLED = 0.005;   int quantityLED = 1; int sizeLED = 1;
int   freqLED  = 50;    int multLED     = 2; String musicLED = "BASS";
float attackLED = 0.15; float decayLED = 0.01;
bool  wrapLED = false;  bool invertLED = false;
// LED page colour, 0xRRGGBB. [0] drives every non-VU pattern; [0..3] are the VU
// meter's four bands, low level first. Defaults reproduce the previous look.
uint32_t colourLED[4] = { 0xFFFFFF, 0xFFD000, 0xFF7000, 0xFF0000 };

// BEAT SENSITIVITY - the absolute part of the onset threshold, and the control
// that actually matters. thr = local_mean * ODF_K + beatSens.
//
// The novelty curve spends most of its time near zero, so local_mean sits around
// 0.11 and the adaptive term contributes almost nothing. A value of 0.02 put the
// threshold at ~0.12 while peaks reach 0.43, so nearly every peak fired - 4-5
// detections a SECOND. At 0.15 the bar sits mid-peak and only real kicks clear it.
//
// GLOBAL: one detector drives both the strips and the 240V. Exposed in the UI as
// "Beat Sensitivity" with the scale REVERSED so the slider's right-hand end is the
// most sensitive (most beats), which is how such a control should read.
// thr = (1 - beatSens) * 60, so 0.2 is the equivalent of the old 0.8.
float beatSens = 0.2f;

// ---- Playlist ----
PlaylistItem playlist[MAX_PLAYLIST_ITEMS];
int playlistCount = 0;
bool playlistPlaying = false;

// ---- Active audio detector parameters (shared) ----
int freqThreshold = 50;
int multiplier = 2;
String currentMusicMode = "BASS";
volatile bool musicFullMode = false;   // "FULL RANGE" selected (audio task reads this)

// ---- 240V relay engine state ----
unsigned long lastStepTime = 0;
unsigned long lastBeatTime = 0;
unsigned long powerLedOffAt = 0;   // IO48 flash-off timestamp (0 = already off)
int dotPos240 = 0;
int dotDirection = 1;
// A beat must be LATCHED per consumer: the 240V display and the strips step at
// their own rates, so a beat delivered on a single loop pass was usually gone
// before the next step ran - which is why the direction so rarely changed.
// Each consumer clears its own latch when it acts on it.
volatile bool beatLatch240 = false;
volatile bool beatLatchLED = false;

// ---- WS2811 LED engine state ----
int ledStep = 0;
int ledDirection = 1;
int ledExplosionStep = 0;

// ---- Audio analysis (FFT) ----------------------------------------------------
// A 256-point FFT over ~32 ms of audio (~31 Hz per bin). This replaces the old
// RC-filter envelope detector, which could not be tuned sensibly and made the
// UI's Frequency slider meaningless. Band magnitudes are referenced to a
// slow-tracking noise floor, so the 0..1 levels self-calibrate to the room
// instead of needing a magic gain.
// A 128-point FFT (~16 ms of audio, ~62 Hz per bin). The window was halved from
// 256 points: the beat detector needs to know about a hit as soon as it happens,
// and a 32 ms window put every detection a third of a beat late at speed. 62 Hz
// bins are coarse, but the FFT now only feeds the VU meters - the beat itself
// comes from the time-domain envelope below.
// 512, not 128: at ~10.6 kHz sampling a 128-point FFT has bins 83 Hz wide, so
// the "bass band" the Frequency slider selects was a SINGLE bin and the control
// could not be made to mean anything. 512 points gives ~21 Hz bins, enough to
// resolve 30-100 Hz kick content properly. The window becomes ~48 ms, which is
// still fast enough for beat timing.
#define FFT_SAMPLES   1024
#define FFT_MID_MAX   2000.0f      // upper edge of the "mid" band, in Hz
float fftReal[FFT_SAMPLES];
float fftImag[FFT_SAMPLES];

volatile float audioBass   = 0.0f;   // 0..1 band levels, read by the render loop
volatile float audioMid    = 0.0f;
volatile float audioTreble = 0.0f;
volatile float audioLevel  = 0.0f;   // whole-spectrum level
volatile float audioFs     = 8000.0f;// measured sample rate of the last window
volatile float rawBass = 0.0f;   // RAW band magnitudes: logged, and used for the
volatile float rawMid  = 0.0f;   // calibration-free onset test (no dB mapping)
volatile float rawTreb = 0.0f;
volatile float rawAll  = 0.0f;

// ---- Original envelope detector (RESTORED) ---------------------------------
// This is the proven, preferred "musicality" response: an RC low-pass whose
// cutoff comes from the Frequency slider, then the peak-to-peak of the filtered
// signal over the window, with a 120-count threshold. It is fed from the same
// ADC samples as the FFT, so both run together.
float    envFiltered = 2048.0f;
volatile int envP2P  = 0;
static int envMax = 0, envMin = 4095;

// ---- Sample-level onset trigger --------------------------------------------
// A level threshold fires at a different point on each hit's rise (jitter) and is
// only evaluated at the end of the analysis window (drift, because that delay
// varies with CPU load). This is a fast attack / slow release envelope follower
// with its own decaying peak: it fires on the ATTACK of a hit, consistently, and
// the instant is stamped when it happens so the interval maths is accurate.
volatile bool          beatTriggered = false;
volatile unsigned long beatTriggerMs = 0;
float                  envA = 0.0f;      // fast envelope
float                  envPeak = 0.0f;   // its decaying peak (the reference)
float                  dcAvg = 2048.0f;  // slow DC tracker (mic sits at mid-rail)
unsigned long          lastTrigMs = 0;
unsigned long          lastDetectMs = 0;  // last time the detector actually fired

// ---- Tempo prediction (4/4 grid) -------------------------------------------
// Interval between FIRED beats (the actual output timing), kept as a short ring
// so the log shows the drift pattern directly instead of it being inferred.
unsigned long fireInt[8] = {0, 0, 0, 0, 0, 0, 0, 0};
int           fireIntIdx = 0;

// ---- Beat clock (phase-locked prediction) ----------------------------------
// The detector cannot be relied on to catch every beat - a soft hit, or one
// masked by another transient. A free-running clock therefore predicts each beat.
// A beat that is EXPECTED but not detected is INSERTED, so the display never
// misses one; a real detection corrects the clock's phase and period, which
// follows the drift. Tempo changes are assumed to land on beat 1, so re-phasing
// on a large error is safe.
unsigned long clockNextBeat = 0;      // when the next beat is expected
unsigned long clockPeriod   = 0;      // the tracked beat period
unsigned long clockMisses   = 0;      // inserted beats since the last log line
unsigned long clockHits     = 0;      // detected beats since the last log line

// Display event offset: the direction change is applied this long AFTER the beat
// is detected, so it lands in the MIDDLE of the beat pulse rather than on the
// leading edge (which reads as early). The detection time itself is untouched, so
// the clock, tempo and pairing maths stay accurate - only the display event is
// offset. Tunable at runtime with /beatoffset?ms=N and persisted.
unsigned long beatOffsetMs = 50;
unsigned long fireAtMs     = 0;
bool          firePending  = false;

// ---- Tempo by AUTOCORRELATION ----------------------------------------------
// Individual transients cannot be trusted with this microphone (the logs show
// plenty of false hits), but MUSIC IS PERIODIC. A short history of onset strength
// is correlated against itself and the lag with the strongest peak IS the beat
// period. That yields a metronome-steady tempo and ignores one-off transients,
// which threshold triggers fundamentally cannot.
#define ONSET_LEN 384            // 3.84 s of onset history at 100 Hz
#define ONSET_HZ  100
float         onsetBuf[ONSET_LEN];
int           onsetIdx   = 0;
int           onsetCount = 0;
float         onsetPrev  = 0.0f;
unsigned long acTempoMs  = 0;    // estimated beat period
float         acScore    = 0.0f; // its correlation strength (logged)

// IO48 = RAW DETECTOR INDICATOR. It flashes on EVERY detection so the detector
// can be judged directly by eye - does it catch every clap, is it picking up
// transients - completely independently of what the lights are doing. The lights
// follow the clock below, which only lets a STRONG detection steer it.
volatile bool rawBeatFlag = false;

// Consecutive beats the clock has had to insert. After two, inserting stops: in
// silence, or when the music is lost, the display simply chases one way until a
// real beat returns rather than inventing a metronome forever.
int insertRun = 0;

// ---- HEAP ATTRIBUTION INSTRUMENT -------------------------------------------
// Measures the heap either side of EVERY FastLED.show() call and accumulates the
// delta, so the per-show allocation can be attributed to the show call itself (or
// shown to be elsewhere) instead of guessed at. Read from /log.txt after a test.
volatile int32_t  g_showHeapDelta = 0;   // sum of (after - before), bytes
volatile uint32_t g_showDeltaN    = 0;   // number of shows measured

unsigned long nextPredictedBeat = 0;   // when the next beat on the grid is due
bool          predictedBeat = false;   // the current beat came from the grid
float         p2pAvg = 0.0f;           // running average level (adaptive thresh)
float         p2pThr = 40.0f;          // current trigger threshold (logged)

// ---- BASS-BAND BEAT DETECTION (the Frequency slider's real job) -------------
// The FFT's low band - its top set by the Frequency control - is the beat source.
// These are its onset metrics: how far the band sits above the room's tracked
// quiet level, and how fast it is rising. A beat is a fast RISE that is clearly
// above the quiet level. That is what separates a kick drum (all its energy low)
// from a cymbal or hi-hat (energy high), which is what was making IO48 flicker
// and, worse, advancing the 1-and-3 / 2-and-4 pairing by an extra step so the
// direction changes appeared to swap and lose time.
float          bassDbLevel = -100.0f;  // current bass band level (dB)
float          bassDbPrev  = -100.0f;  // previous window's level (for the rise)
volatile float bassRiseDb  = 0.0f;     // onset strength (dB per window)
volatile float bassOverDb  = 0.0f;     // dB above the tracked quiet level
float          bassOverThr = 9.0f;     // raise for kick-only, lower to catch claps
float          bassBandLo  = 0.0f;     // the band actually in use (logged)
float          bassBandHi  = 0.0f;

// ---- 4/4 downbeat tracking --------------------------------------------------
// In 4/4 the bar's first beat ("the 1") is normally the loudest and is marked by
// the other instruments too, so tally the level of the beats landing on each of
// the four grid slots: the slot with the highest average IS the downbeat. The
// direction then changes on the "1" only. An infill is a cluster of hits much
// faster than the learnt tempo - those change direction on every hit (they sound
// wrong otherwise) and are deliberately kept out of the statistics.
int           barPos = 0;                        // which grid slot is next
int           downbeatPos = -1;                  // the learnt "1" (-1 = none)
int           lockedDownbeat = -1;               // once chosen, never changes
int           beatToggle = 0;                    // stable every-other-beat fire
int           offTempoRun = 0;                   // consecutive off-tempo beats
float         barEnergySum[4] = {0, 0, 0, 0};    // average level per slot
int           barEnergyCnt[4] = {0, 0, 0, 0};
bool          infillMode = false;
unsigned long firedCount = 0;                    // direction changes, per log line
static float bandFloorAll = 0, bandFloorBass = 0, bandFloorMid = 0, bandFloorTreble = 0;

unsigned long lastRealBeatTime = 0;
unsigned long lastAcceptedBeatTime = 0;
#define HISTORY_SIZE 10
unsigned long beatIntervalHistory[HISTORY_SIZE];
int historyIndex = 0;
int historyCount = 0;

int generatedSubBeatsCount = 0;
unsigned long nextSubBeatTargetTime = 0;
unsigned long establishedInterval = 0;

// ---- VU-meter smoothing state ----
float vuSmooth240V = 0.0;
int   vuPeakLevel240V = 0;
unsigned long lastVuPeakFall240V = 0;
float vuSmoothLED = 0.0;

// Theme preview palette from the UI (up to 8 colours), persisted across reboots
CRGB previewColors[8];
int previewColorCount = 0;
String themeName = "Disco";
// 0 = normal rendering, 1 = hold RGB calibration test, 2 = hold theme preview
int previewMode = 0;
unsigned long lastPreviewRender = 0;   // periodic re-assert of held previews

// Quiet startup: nothing is displayed until a pattern is selected in the UI.
// This keeps the strips dark (and the CPU free) so the web page always loads.
volatile bool displayActive = false;

// ---------------------------------------------------------------------------
// Cross-core hand-off. Core 0 = audio/FFT only. Core 1 = display task + web UI.
// The display task is the ONLY place that renders, so FastLED is never called
// from two tasks, and the web server never blocks a frame.
// ---------------------------------------------------------------------------
volatile bool beatFlag     = false;  // set by the audio task, consumed by display
volatile bool previewDirty = false;  // a new preview frame is ready to draw
volatile bool blankReq     = false;  // request: blank all outputs
volatile bool flashReq     = false;  // request: do the "saved" confirmation blink
volatile int  calPort      = 0;      // RGB calibration test: port
volatile int  calOrder     = 2;      // RGB calibration test: colour order

// ---------------------------------------------------------------------------
// Per-strip pattern engine + Timeline + master brightness.
// Declarations live here so every handler can see them; implementations are
// further down with the LED engine.
// ---------------------------------------------------------------------------
enum LedPattern : int {
  LP_MOVING_DOT = 0, LP_CHASE_1_10, LP_CHASE_10_1, LP_CENTER_OUT,
  LP_KNIGHT_RIDER, LP_VU_METER, LP_VU_REV, LP_VU_DUAL, LP_VU_CENTER
};

struct PortCfg {
  int   pat    = LP_MOVING_DOT;
  float speed  = 0.2f;
  int   qty    = 1;
  int   size   = 1;
  bool  wrap   = false;
  bool  invert = false;
  // Per-port colour, 0xRRGGBB.
  //   colour[0]          - the colour used by every non-VU pattern (dot, chase,
  //                        centre-out, knight rider) and by Invert.
  //   colour[0]..[3]     - the four bands of the VU meters, low level first.
  // Defaults preserve the previous look exactly: white for the dot patterns,
  // and the classic green -> yellow -> orange -> red VU gradient.
  uint32_t colour[4] = { 0xFFFFFF, 0xFFD000, 0xFF7000, 0xFF0000 };
};

PortCfg portCfg[NUM_PORTS];

// "#rrggbb" (or "rrggbb") -> 0xRRGGBB. Returns `dflt` if unparseable.
//
// NOTE: this deliberately sits BELOW the type declarations. The Arduino IDE
// inserts auto-generated function prototypes just before the sketch's FIRST
// function, so defining any function above LedPattern/PortCfg moves that block
// above them and the build fails with "<type> does not name a type".
uint32_t parseHexColor(const char* s, uint32_t dflt) {
  if (!s) return dflt;
  while (*s == ' ' || *s == '#') s++;
  if (!*s) return dflt;
  return (uint32_t)strtoul(s, nullptr, 16) & 0xFFFFFFUL;
}

int           ledPosArr[NUM_PORTS]  = {0};
// Direction of travel per strip. EVERY element must be non-zero: the initialiser
// {1} sets only element 0 and leaves the rest at 0, and a zero direction means
// advancePort() adds 0 to the position forever - the dot is drawn but never moves.
// That is exactly why every port except port 1 showed stationary LEDs, while
// Explosion worked (it uses ledExplArr, which is incremented and never reads this).
int           ledDirArr[NUM_PORTS]  = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
int           ledExplArr[NUM_PORTS] = {0};
unsigned long ledStepMs[NUM_PORTS]  = {0};

#include <NeoPixelBus.h>

// --- LED OUTPUT -------------------------------------------------------------
// NeoPixelBus, LCD_CAM parallel method (ESP32-S3 only). Up to 16 channels are
// clocked out in a single DMA transfer, so there is no RMT and no grouping. This
// is the only complete parallel clockless driver available for this chip: FastLED
// 3.10.7 ships one but it is disabled in the library (all files .disabled and
// absent from lcd_cam/_build.cpp.hpp).
// NeoGrbFeature is used because leds[][] is ALREADY in each port's wire order.
typedef NeoPixelBus<NeoGrbFeature, NeoEsp32LcdX8Ws2811Method> Ws2811Strip;
Ws2811Strip *g_strip[NUM_PORTS] = {nullptr};

// Bring-up probe: logged so we can see whether the instances register, whether the
// driver reports itself ready, and whether Show() actually reached the transfer.
volatile int g_npbReady   = -1;    // -1 = not sampled, else CanShow() result
volatile int g_npbCreated = 0;     // instances successfully created
volatile int g_npbShows   = 0;     // Show() calls completed

// How long one complete 10-strip show ACTUALLY takes, in microseconds. This is the
// number that sets the maximum dot speed: a step must be displayed, so the frame
// interval can only be as short as a show lasts. With true parallel output all ten
// strips are clocked in one DMA transfer, so this should be a few milliseconds -
// not the ~30 ms a serial transmission would need.
volatile int32_t  g_showUsMax = 0;
volatile int32_t  g_showUsSum = 0;
volatile uint32_t g_showUsN   = 0;

// Split of the show time, so the spike and the per-frame overhead can be seen
// separately rather than hidden in one aggregate: copy = filling NeoPixelBus's
// pixel buffers, tx = the Show() calls and their wait for the LCD DMA.
volatile int32_t  g_copyUsSum = 0, g_copyUsMax = 0;
volatile int32_t  g_txUsSum   = 0, g_txUsMax   = 0;

// How often a strip actually advances. If the calculated interval says 5 ms but
// this counts ~48 steps/second, then something other than the shown interval is
// gating the motion.
volatile uint32_t g_stepCount = 0;
volatile uint32_t g_showSkip  = 0;   // frames skipped because the hardware was busy

// Loop timing, split by stage, so a slow loop can be attributed instead of
// guessed at. g_loop* is the whole pass; g_client* is the web server's share;
// g_render* is the pattern engine's share (which includes ledShowAll).
volatile int32_t  g_loopSumUs = 0,   g_loopMaxUs = 0;
volatile int32_t  g_clientSumUs = 0, g_clientMaxUs = 0;
volatile int32_t  g_renderSumUs = 0, g_renderMaxUs = 0;
volatile uint32_t g_loopN = 0;

// Per-port "this strip's buffer changed since the last show". Copying all 1000
// pixels every frame was ~5 ms of the frame time even when nine strips were
// unchanged; only the strips that actually stepped need copying now, so the
// per-frame cost drops roughly tenfold and the frame rate (which sets the maximum
// dot speed) is no longer dragged down by redundant work.
volatile bool ledDirty[NUM_PORTS] = {false};

void ledShowAll() {
  // NEVER BLOCK. NeoPixelBus's Show() waits for the previous LCD DMA transfer to
  // finish, and that wait is precisely what shows up as a 30-40 ms spike while the
  // average frame is only ~5 ms. CanShow() reports whether the hardware is ready,
  // so a frame is simply skipped when it is not: the display then runs at exactly
  // the rate the hardware can sustain, with no stalls. This works identically for
  // all ten ports, since they are transmitted together in one parallel transfer.
  if (g_strip[0] && !g_strip[0]->CanShow()) { g_showSkip++; return; }

  int64_t t0 = esp_timer_get_time();
  // Fast path: copy only the strips that stepped. Fallback: if NO strip is marked
  // dirty, something wrote leds[][] directly (blank, preview, save-flash,
  // calibration) instead of through drawPortPattern(), so copy them all. That makes
  // this self-correcting rather than depending on every writer remembering to mark.
  bool anyDirty = false;
  for (int p = 0; p < NUM_PORTS; p++) { if (ledDirty[p]) { anyDirty = true; break; } }
  if (!anyDirty) for (int p = 0; p < NUM_PORTS; p++) ledDirty[p] = true;

  for (int p = 0; p < NUM_PORTS; p++) {
    Ws2811Strip *s = g_strip[p];
    if (!s) continue;
    if (ledDirty[p]) {
      ledDirty[p] = false;
      for (int i = 0; i < NUM_LEDS_PER_STRIP; i++)
        s->SetPixelColor(i, RgbColor(leds[p][i].r, leds[p][i].g, leds[p][i].b));
    }
  }
  int64_t t1 = esp_timer_get_time();
  for (int p = 0; p < NUM_PORTS; p++) {
    if (g_strip[p]) g_strip[p]->Show();
  }
  int64_t t2 = esp_timer_get_time();
  {
    int32_t cu = (int32_t)(t1 - t0), xu = (int32_t)(t2 - t1);
    g_copyUsSum += cu; if (cu > g_copyUsMax) g_copyUsMax = cu;
    g_txUsSum   += xu; if (xu > g_txUsMax)   g_txUsMax   = xu;
  }
  g_npbShows++;
  int32_t us = (int32_t)(t2 - t0);
  g_showUsSum += us;
  g_showUsN++;
  if (us > g_showUsMax) g_showUsMax = us;
}

// Blank every strip buffer. FastLED.clear() only clears REGISTERED controllers
// and with our own driver nothing is registered, so do it explicitly.
void ledClearAll() {
  for (int p = 0; p < NUM_PORTS; p++) {
    for (int i = 0; i < NUM_LEDS_PER_STRIP; i++) leds[p][i] = CRGB::Black;
    ledDirty[p] = true;              // the clear must be copied out too
  }
}

#define TL_ROWS 8
#define TL_COLS 12

struct TlItem {
  char  id[24] = "";
  float speed  = 0.2f;
  int   qty    = 1;
  int   size   = 1;
  bool  wrap   = false;
  bool  invert = false;
  bool  used   = false;
};

TlItem tlData[TL_ROWS][TL_COLS];
float tlDelaySec = 20.0f;
int   tlRow = 0;
unsigned long tlRowStart = 0;
bool  tlLoaded = false;

// Master brightness: 0-100 UI value -> FastLED 0-255. This is the MAXIMUM;
// individual displays/animations may scale down from it.
volatile int masterBrightness = 100;

// Diagnostics: count ledShowAll() calls so the heartbeat can report them.
volatile unsigned long ledShowCount = 0;

// ---------------------------------------------------------------------------
// Serial-free diagnostics. IO48 blinks a code for each UI command received,
// so the firmware's view can be read WITHOUT opening the serial port (opening
// it asserts the ESP32-S3 reset line and disturbs the board).
//   1 blink  = /live-tweak      (pattern selected)
//   2 blinks = /calibrate-test  (RGB calibration)
//   3 blinks = /theme-preview   (colour theme)
//   4 blinks = /init-off
// ---------------------------------------------------------------------------
volatile int  statusBlinks = 0;      // toggles remaining
unsigned long statusNext   = 0;
bool          statusOn     = false;

void showStatus(int n) {
  statusBlinks = n * 2;              // n blinks = 2n toggles (on/off pairs)
  statusOn = false;
  statusNext = 0;
}

// Set true for one loop pass when a beat is detected (single shared detector).
bool beatDetectedThisLoop = false;

// ---------------------------------------------------------------------------
// CRASH / RUN DIAGNOSTICS  (readable from the browser at /diag)
// The serial port must NEVER be opened - DTR/RTS on the ESP32-S3 USB-Serial/
// JTAG are wired to reset/boot - so the final state of a crashed run is kept in
// RTC memory, which survives a panic / watchdog / brown-out reset (only a real
// power cycle clears it). setup() copies it into NVS, so /diag can report it at
// any time afterwards.
//   dbgStage 1 = web server  2 = render  3 = status blinks  4 = diagnostics
// ---------------------------------------------------------------------------
#define DBG_MAGIC 0x4C533951UL
RTC_NOINIT_ATTR uint32_t dbgMagic;
RTC_NOINIT_ATTR uint32_t dbgStage;
RTC_NOINIT_ATTR uint32_t dbgUpSec;
RTC_NOINIT_ATTR uint32_t dbgHeap;
RTC_NOINIT_ATTR uint32_t dbgLoops;
RTC_NOINIT_ATTR uint32_t dbgBoots;      // boots since the last real power cycle
RTC_NOINIT_ATTR uint32_t dbgNvsErase;   // how many of those erased the NVS

// Why the PREVIOUS run ended. Set in setup() and slow-blinked on IO48 after
// boot so the cause is visible with no serial port AND no web page.
int g_prevReason = 0;

// Deferred NVS write for the brightness slider (0 = nothing pending).
unsigned long brSaveAt = 0;
// Set by the render loop, performed by the audio task (core 0): flash writes
// must never block the display, or the result is a visible freeze.
volatile bool saveSettingsPending = false;

const char* resetReasonText(int r) {
  switch (r) {
    case 1:  return "POWER_ON";
    case 2:  return "EXTERNAL_PIN";
    case 3:  return "SOFTWARE";
    case 4:  return "PANIC (firmware crash)";
    case 5:  return "INT_WATCHDOG";
    case 6:  return "TASK_WATCHDOG";
    case 7:  return "WATCHDOG";
    case 8:  return "DEEP_SLEEP";
    case 9:  return "BROWNOUT (supply dipped)";
    case 10: return "SDIO";
    case 11: return "USB";
    case 12: return "JTAG";
    case 13: return "EFUSE";
    case 14: return "POWER_GLITCH";
    default: return "UNKNOWN";
  }
}

void loadPortCalibrations();
void loadSettings();

// ==========================================
// HELPERS
// ==========================================

// Reorder a colour according to a calibration index (0:RGB 1:GRB 2:BRG 3:RBG 4:GBR 5:BGR)
CRGB applyColorOrder(CRGB inputColor, int orderIndex) {
  uint8_t r = inputColor.r;
  uint8_t g = inputColor.g;
  uint8_t b = inputColor.b;
  switch (orderIndex) {
    case 0: return CRGB(r, g, b); // RGB
    case 1: return CRGB(g, r, b); // GRB
    case 2: return CRGB(b, r, g); // BRG
    case 3: return CRGB(r, b, g); // RBG
    case 4: return CRGB(g, b, r); // GBR
    case 5: return CRGB(b, g, r); // BGR
    default: return CRGB(r, g, b);
  }
}

// Alias kept so both call sites compile
CRGB applyCalibrationOrder(CRGB color, int orderIndex) {
  return applyColorOrder(color, orderIndex);
}

// ---------------------------------------------------------------------------
// The UI sends EVERY slider value as a JSON *string* ("0.50", "1", ...).
// ArduinoJson's "|" default operator uses is<T>(), which is false for a string,
// so doc["speed"] | 1.0f would silently return the default and the slider would
// have no effect. These helpers convert strings as well.
// ---------------------------------------------------------------------------
float jsonNumF(JsonVariant v, float def) {
  if (v.is<const char*>()) return String(v.as<const char*>()).toFloat();
  if (v.is<float>() || v.is<int>() || v.is<long>() || v.is<unsigned int>()) return v.as<float>();
  return def;
}

int jsonNumI(JsonVariant v, int def) {
  if (v.is<const char*>()) return String(v.as<const char*>()).toInt();
  if (v.is<int>() || v.is<long>() || v.is<float>() || v.is<unsigned int>()) return v.as<int>();
  return def;
}

// The UI sends human-readable pattern names; convert them to internal IDs.
// Also accepts already-internal IDs (e.g. from a saved playlist).
String patternIdFromName(String name) {
  String n = name;
  n.trim();
  if (n.equalsIgnoreCase("Moving Dot")      || n.equalsIgnoreCase("moving_dot"))       return "moving_dot";
  if (n.equalsIgnoreCase("Chase 1 - 10")    || n.equalsIgnoreCase("chase_1_10"))       return "chase_1_10";
  if (n.equalsIgnoreCase("Chase 10 - 1")    || n.equalsIgnoreCase("chase_10_1"))       return "chase_10_1";
  if (n.equalsIgnoreCase("Explosion")       || n.equalsIgnoreCase("center_out_chase")) return "center_out_chase";
  if (n.equalsIgnoreCase("Knight Rider")    || n.equalsIgnoreCase("knight_rider"))     return "knight_rider";
  if (n.equalsIgnoreCase("VU Meter Left")   || n.equalsIgnoreCase("vu_meter"))         return "vu_meter";
  if (n.equalsIgnoreCase("VU Meter Right")  || n.equalsIgnoreCase("vu_meter_rev"))     return "vu_meter_rev";
  if (n.equalsIgnoreCase("VU Meter Dual")   || n.equalsIgnoreCase("vu_dual"))          return "vu_dual";
  if (n.equalsIgnoreCase("VU Meter Centre") || n.equalsIgnoreCase("vu_center_out"))    return "vu_center_out";
  n.toLowerCase();
  return n;
}

// Convert a hex colour string ("#RRGGBB") to CRGB
CRGB hexToCRGB(String hex) {
  if (hex.length() < 7 || hex[0] != '#') return CRGB::Black;
  long number = strtol(&hex[1], NULL, 16);
  return CRGB(number >> 16, (number >> 8) & 0xFF, number & 0xFF);
}

// Smooth a 0..1 band level into a 0..10 VU level with attack/decay ballistics.
// Music Source selects the source: FULL RANGE = whole spectrum, BASS = the low
// band. The MAX9814 barely responds below ~150 Hz, so a silent low band folds
// in the mid band rather than leaving the meters dead.
int computeVuLevel(float &smooth, float attack, float decay) {
  float src;
  if (musicFullMode) {
    src = audioLevel;
  } else {
    src = (audioBass > audioMid) ? audioBass : audioMid;
  }
  float target = constrain(src, 0.0f, 1.0f) * 10.0f;
  float a = constrain(attack, 0.01f, 1.0f);
  float d = constrain(decay, 0.01f, 1.0f);
  if (target > smooth) smooth += a * (target - smooth);
  else                 smooth += d * (target - smooth);
  if (smooth < 0.3f) smooth = 0;
  return (int)(smooth + 0.5f);
}

// Drive all 10 relay channels low
void clearAllChannels() {
  for (int i = 0; i < 10; i++) {
    digitalWrite(channelPins[i], LOW);
  }
}

// Persist all runtime settings to NVS
void saveSettings() {
  preferences.begin("lightshow", false);
  preferences.putBool("audioEnabled", audioEnabled);
  preferences.putFloat("clockDelay", clockDelay);
  preferences.putInt("antifreeze", antifreeze);
  preferences.putString("pattern240", activePattern240V);
  preferences.putString("patternLED", activePatternLED);
  preferences.putFloat("speed240", speed240V);
  preferences.putInt("qty240", quantity240V);
  preferences.putInt("size240", size240V);
  preferences.putInt("freq240", freq240V);
  preferences.putInt("mult240", mult240V);
  preferences.putString("music240", music240V);
  preferences.putFloat("atk240", attack240V);
  preferences.putFloat("dec240", decay240V);
  preferences.putFloat("speedLED", speedLED);
  preferences.putInt("qtyLED", quantityLED);
  preferences.putInt("sizeLED", sizeLED);
  preferences.putInt("freqLED", freqLED);
  preferences.putInt("multLED", multLED);
  preferences.putString("musicLED", musicLED);
  preferences.putFloat("atkLED", attackLED);
  preferences.putFloat("decLED", decayLED);
  preferences.putInt("brightness", masterBrightness);
  preferences.putBool("wrap240", wrap240V);
  preferences.putInt("beatOff", (int)beatOffsetMs);
  preferences.putBool("inv240", invert240V);
  preferences.putBool("wrapLED", wrapLED);
  preferences.putBool("invLED", invertLED);
  preferences.putFloat("beatSens2", beatSens);
  for (int i = 0; i < 4; i++)
    preferences.putUInt((String("colLED") + i).c_str(), colourLED[i]);
  preferences.putString("themeName", themeName);
  preferences.putInt("themeCnt", previewColorCount);
  for (int i = 0; i < 8; i++) {
    uint32_t c = ((uint32_t)previewColors[i].r << 16) | ((uint32_t)previewColors[i].g << 8) | previewColors[i].b;
    preferences.putUInt(("thCol" + String(i)).c_str(), c);
  }
  preferences.end();
}

// Restore runtime settings from NVS
void loadSettings() {
  preferences.begin("lightshow", true);
  audioEnabled = preferences.getBool("audioEnabled", true);
  clockDelay = preferences.getFloat("clockDelay", 1.0);
  antifreeze = preferences.getInt("antifreeze", 2);
  activePattern240V = preferences.getString("pattern240", "moving_dot");
  activePatternLED  = preferences.getString("patternLED", "moving_dot");
  speed240V = preferences.getFloat("speed240", 1.0);
  quantity240V = preferences.getInt("qty240", 1);
  size240V = preferences.getInt("size240", 1);
  freq240V = preferences.getInt("freq240", 50);
  mult240V = preferences.getInt("mult240", 2);
  music240V = preferences.getString("music240", "BASS");
  attack240V = preferences.getFloat("atk240", 0.15);
  decay240V = preferences.getFloat("dec240", 0.01);
  speedLED = preferences.getFloat("speedLED", 1.0);
  quantityLED = preferences.getInt("qtyLED", 1);
  sizeLED = preferences.getInt("sizeLED", 1);
  freqLED = preferences.getInt("freqLED", 50);
  multLED = preferences.getInt("multLED", 2);
  musicLED = preferences.getString("musicLED", "BASS");
  attackLED = preferences.getFloat("atkLED", 0.15);
  decayLED = preferences.getFloat("decLED", 0.01);
  masterBrightness = preferences.getInt("brightness", 100);
  wrap240V = preferences.getBool("wrap240", false);
  // Beat offset default is now 20 ms, NOT 200.
  //
  // The 200 ms default was latency compensation for the CLOCK path, where the
  // visible event lagged detection. On the onset detector it is simply 200 ms of
  // ADDED lag, and against a 300-600 ms beat interval that reads as sluggish -
  // exactly the reported symptom.
  //
  // Genuine latency in this path is ~20 ms: about 16 ms of group delay in the
  // 4-pole high-pass at low frequencies (4/(2*pi*f) at 40 Hz), ~3 ms in the
  // low-pass, and ~5 ms from the 10 ms ODF window.
  //
  // A NEW KEY is used deliberately: the old 200 is persisted in NVS, so changing
  // the default alone would leave it in place. Clamped so it cannot drift back.
  beatOffsetMs = (unsigned long)preferences.getInt("beatOff2", 20);
  if (beatOffsetMs > 80) beatOffsetMs = 80;

  invert240V = preferences.getBool("inv240", false);
  wrapLED = preferences.getBool("wrapLED", false);
  invertLED = preferences.getBool("invLED", false);
  // NEW KEY: the old value was on the un-reversed scale (0.8 = strict), so loading
  // it here would make the detector almost maximally sensitive. 0.2 is the
  // equivalent of that old 0.8 on the reversed scale.
  beatSens  = preferences.getFloat("beatSens2", 0.2f);
  if (beatSens < 0.01f) beatSens = 0.01f;
  if (beatSens > 1.00f) beatSens = 1.00f;
  for (int i = 0; i < 4; i++)
    colourLED[i] = preferences.getUInt((String("colLED") + i).c_str(),
                                       colourLED[i]);
  themeName = preferences.getString("themeName", "Disco");
  previewColorCount = constrain(preferences.getInt("themeCnt", 0), 0, 8);
  for (int i = 0; i < 8; i++) {
    uint32_t c = preferences.getUInt(("thCol" + String(i)).c_str(), 0);
    previewColors[i] = CRGB((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
  }
  preferences.end();

  freqThreshold = constrain(freqLED, 20, 100);
  multiplier = constrain(multLED, 1, 9);
  currentMusicMode = musicLED;
}

// ==========================================
// WEB HANDLERS
// ==========================================

void handleRoot() {
  // MUST use send_P for the large page: server.send() casts the content to a
  // String, which fails to allocate ~68 KB on the heap and yields an empty page
  // (Content-Length: 0 -> white screen). send_P streams straight from flash.
  //
  // No-cache headers: without these, Safari serves a STALE cached copy of the
  // page after a firmware update, so the UI's requests silently do nothing.
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "0");
  server.send_P(200, "text/html", MAIN_PAGE);
}

void handlePing() {
  JsonDocument doc;
  doc["status"] = "pong";
  doc["playlistPlaying"] = playlistPlaying;
  String output;
  serializeJson(doc, output);
  server.send(200, "application/json", output);
}

void handleGetSettings() {
  JsonDocument doc;
  doc["audioEnabled"] = audioEnabled;
  doc["clockDelay"] = clockDelay;
  doc["antifreeze"] = antifreeze;
  doc["pattern240"] = activePattern240V;
  doc["patternLED"] = activePatternLED;
  doc["speed240"] = speed240V;
  doc["speedLED"] = speedLED;
  doc["freq240"] = freq240V;
  doc["freqLED"] = freqLED;
  // These used to be omitted, so the UI could only ever show its HTML defaults
  // after a reload - which made correctly-saved settings look "not retained".
  doc["qty240"] = quantity240V;   doc["qtyLED"] = quantityLED;
  doc["size240"] = size240V;      doc["sizeLED"] = sizeLED;
  doc["mult240"] = mult240V;      doc["multLED"] = multLED;
  doc["atk240"] = attack240V;     doc["atkLED"] = attackLED;
  doc["dec240"] = decay240V;      doc["decLED"] = decayLED;
  doc["wrap240"] = wrap240V;      doc["wrapLED"] = wrapLED;
  doc["inv240"] = invert240V;     doc["invLED"] = invertLED;
  doc["music240"] = music240V;    doc["musicLED"] = musicLED;
  doc["beatSens"] = beatSens;      // global: one detector drives both displays
  // LED colour, so a reload shows the saved colours rather than the HTML defaults.
  {
    JsonArray ca = doc["colourLED"].to<JsonArray>();
    char buf[8];
    for (int i = 0; i < 4; i++) {
      snprintf(buf, sizeof(buf), "#%06lx", (unsigned long)(colourLED[i] & 0xFFFFFFUL));
      ca.add(buf);
    }
  }
  doc["brightness"] = masterBrightness;
  doc["theme"] = themeName;
  doc["themeCount"] = previewColorCount;
  JsonArray cols = doc["themeColors"].to<JsonArray>();
  for (int i = 0; i < previewColorCount && i < 8; i++) {
    char buf[8];
    snprintf(buf, sizeof(buf), "#%02X%02X%02X", previewColors[i].r, previewColors[i].g, previewColors[i].b);
    cols.add(buf);
  }
  String output;
  serializeJson(doc, output);
  server.send(200, "application/json", output);
}

void loadPortCalibrations() {
  Preferences prefs;
  prefs.begin("lightshow", true);
  for (int i = 0; i < NUM_PORTS; i++) {
    portColorOrders[i] = prefs.getInt(("port_ord_" + String(i)).c_str(), 2);
  }
  prefs.end();
}

void handleCalibrateTest() {
  if (server.hasArg("port") && server.hasArg("order")) {
    calPort = server.arg("port").toInt();
    calOrder = server.arg("order").toInt();
    previewMode = 1;      // held by the display task until another selection
    previewDirty = true;
    showStatus(2);        // IO48: 2 blinks = calibrate-test received
    server.send(200, "text/plain", "Calibration Test Active");
  } else {
    server.send(400, "text/plain", "Missing arguments");
  }
}

void handleCalibrateSave() {
  if (server.hasArg("port") && server.hasArg("order")) {
    int port = server.arg("port").toInt();
    int order = server.arg("order").toInt();

    if (port >= 0 && port < NUM_PORTS) {
      portColorOrders[port] = order;
      preferences.begin("lightshow", false);
      preferences.putInt(("port_ord_" + String(port)).c_str(), order);
      preferences.end();
      server.send(200, "text/plain", "Port " + String(port + 1) + " Calibration Saved!");
    } else {
      server.send(400, "text/plain", "Invalid port index");
    }
  } else {
    server.send(400, "text/plain", "Missing arguments");
  }
}

// Legacy endpoint: apply global audio/timing settings
void handleUpdate() {
  if (!server.hasArg("plain")) {
    server.send(400, "text/plain", "Body not received");
    return;
  }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "text/plain", "JSON parsing failed");
    return;
  }

  if (doc.containsKey("audioEnabled")) audioEnabled = doc["audioEnabled"];
  if (doc.containsKey("clockDelay"))   clockDelay = doc["clockDelay"];
  if (doc.containsKey("antifreeze"))   antifreeze = doc["antifreeze"];

  saveSettings();
  server.send(200, "text/plain", "OK");
}

// Legacy endpoint: persist current settings
void handleSave() {
  saveSettings();
  server.send(200, "text/plain", "Saved");
}

// Store the theme palette and preview it across every strip
void handleThemePreview() {
  if (!server.hasArg("plain")) { server.send(400, "text/plain", "Bad Request"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "text/plain", "Invalid JSON");
    return;
  }

  int count = doc["count"] | 0;
  count = constrain(count, 0, 8);
  previewColorCount = count;

  String tn = String((const char*)(doc["theme"] | "Disco"));
  if (tn.length()) themeName = tn;

  for (int i = 0; i < 8; i++) previewColors[i] = CRGB::Black;
  JsonArray colors = doc["colors"].as<JsonArray>();
  int idx = 0;
  for (JsonVariant v : colors) {
    if (idx >= 8) break;
    previewColors[idx] = hexToCRGB(v.as<String>());
    idx++;
  }

  previewMode = 2;      // held by the display task
  previewDirty = true;
  showStatus(3);        // IO48: 3 blinks = theme-preview received
  // Persisted only on an explicit Save (see /save-flash).

  server.send(200, "text/plain", "Theme Preview Active");
}

// Live parameter updates from the 240V / LED configuration sliders
void handleLiveTweak() {
  if (!server.hasArg("plain")) { server.send(400, "text/plain", "Bad Request"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "text/plain", "Invalid JSON");
    return;
  }

  String section = String((const char*)(doc["section"] | "240V"));
  String name    = String((const char*)(doc["name"] | ""));
  String id = patternIdFromName(name);

  float  spd   = jsonNumF(doc["speed"],      1.0f);
  int    qty   = jsonNumI(doc["quantity"],   1);
  int    sz    = jsonNumI(doc["size"],       1);
  int    frq   = jsonNumI(doc["frequency"],  50);
  int    bm    = jsonNumI(doc["beatMult"],   2);
  String music = String((const char*)(doc["musicSource"] | "BASS"));
  float  atk   = jsonNumF(doc["attack"],     0.15f);
  float  dec   = jsonNumF(doc["decay"],      0.01f);
  bool   wrap  = doc["wrap"] | false;
  bool   inv   = doc["invert"] | false;

  // Normalise the music-source label ("FULL RANGE" is what the UI sends)
  if (music.equalsIgnoreCase("FULL RANGE") || music.equalsIgnoreCase("FULL")) music = "FULL";
  else music = "BASS";

  // Beat sensitivity is GLOBAL - one detector drives both displays - so it is
  // applied here regardless of section.
  if (doc.containsKey("beatSens")) {
    float bs = jsonNumF(doc["beatSens"], beatSens);
    beatSens = constrain(bs, 0.01f, 1.00f);
  }

  if (section == "LED") {
    if (id.length()) activePatternLED = id;
    speedLED = spd; quantityLED = constrain(qty, 1, 8); sizeLED = constrain(sz, 1, 10);
    freqLED = frq; multLED = constrain(bm, 1, 9); musicLED = music;
    attackLED = atk; decayLED = dec;
    wrapLED = wrap; invertLED = inv;
    // Colour: the UI sends an array of "#rrggbb" strings. Index 0 is used by the
    // dot/chase/centre-out patterns, and 0..3 are the VU meter's four bands.
    JsonArray ca = doc["colour"].as<JsonArray>();
    if (!ca.isNull()) {
      int i = 0;
      for (JsonVariant v : ca) {
        if (i >= 4) break;
        colourLED[i] = parseHexColor(v.as<const char*>(), colourLED[i]);
        i++;
      }
    }
    if (id.length()) activePattern240V = id;
    speed240V = spd; quantity240V = constrain(qty, 1, 4); size240V = constrain(sz, 1, 4);
    freq240V = frq; mult240V = constrain(bm, 1, 9); music240V = music;
    attack240V = atk; decay240V = dec;
    wrap240V = wrap; invert240V = inv;
  }

  if (id.length()) displayActive = true;  // a pattern was selected: start displaying
  // Re-base the per-strip config on the LED-page settings, then re-apply the
  // current Timeline row (its columns 1-10 override the individual strips).
  applyGlobalsToPorts();
  applyTimelineRow(tlRow, false);

  previewMode = 0;                        // a new selection cancels any held preview/calibration
  // NOTE: no IO48 blink here. IO48 is reserved for the raw beat detector (one
  // flash per detection, in loop()) so that what you see on the board is the
  // detector's behaviour and nothing else. A blink per slider twitch made that
  // impossible to read while adjusting the display during playback.

  // The shared detector follows the most recently tweaked section
  freqThreshold = constrain(frq, 20, 100);
  multiplier = constrain(bm, 1, 9);
  currentMusicMode = music;
  musicFullMode = (music == "FULL");

  // NOTE: settings are only persisted on an explicit Save (/save, /save-flash).
  // Auto-saving on every tweak filled the small NVS partition and stopped the
  // WiFi driver storing its calibration data (phy_init error 0x1105).

  server.send(200, "text/plain", "OK");
}

void handleSavePlaylist() {
  if (!server.hasArg("plain")) { server.send(400, "text/plain", "Bad Request"); return; }
  File file = LittleFS.open("/playlist.json", "w");
  if (!file) { server.send(500, "text/plain", "Failed to open file for writing"); return; }
  file.print(server.arg("plain"));
  file.close();
  server.send(200, "text/plain", "Playlist saved to LittleFS successfully");
}

void handleLoadPlaylist() {
  if (!LittleFS.exists("/playlist.json")) {
    server.send(404, "text/plain", "No playlist file found");
    return;
  }
  File file = LittleFS.open("/playlist.json", "r");
  if (!file) { server.send(500, "text/plain", "Failed to open playlist file"); return; }
  String content = file.readString();
  file.close();
  server.send(200, "application/json", content);
}

void handleSaveTimeline() {
  if (!server.hasArg("plain")) { server.send(400, "text/plain", "Bad Request"); return; }
  File file = LittleFS.open("/timeline.json", "w");
  if (!file) { server.send(500, "text/plain", "Failed to open timeline file"); return; }
  file.print(server.arg("plain"));
  file.close();
  loadTimeline();               // pick up the new per-strip assignments
  applyTimelineRow(tlRow, false);
  server.send(200, "text/plain", "Timeline saved to LittleFS successfully");
}

// Master brightness (0-100). Applied immediately; the NVS write is DEFERRED
// (see loop) because saving on every slider input event hammered - and
// exhausted - the NVS partition.
void handleBrightness() {
  if (server.hasArg("value")) {
    masterBrightness = constrain(server.arg("value").toInt(), 0, 100);
    applyMasterBrightness();
    // A held calibration/theme preview only repaints when it is marked dirty,
    // so without this the brightness slider appears to do nothing on those
    // pages (it always worked for the running patterns).
    if (previewMode == 1 || previewMode == 2) previewDirty = true;
    brSaveAt = millis() + 2000;      // persist 2 s after the last change
    server.send(200, "text/plain", "OK");
  } else {
    server.send(400, "text/plain", "Missing value");
  }
}

// Audio Detect on/off. When OFF, the display changes direction on the
// antifreeze timer only (never on beats, and antifreeze never affects speed).
void handleAudioDetect() {
  if (server.hasArg("enabled")) {
    String v = server.arg("enabled");
    audioEnabled = v.equalsIgnoreCase("true") || v == "1";
    saveSettings();
    server.send(200, "text/plain", audioEnabled ? "Audio Detect ON" : "Audio Detect OFF");
  } else {
    server.send(400, "text/plain", "Missing enabled");
  }
}

// Crash diagnostics: why did the previous run end, and where was it?
void handleDiag() {
  Preferences dp;
  dp.begin("diag", true);
  int      pReason = dp.getInt("reason", -1);
  uint32_t pValid  = dp.getUInt("valid", 0);
  uint32_t pStage  = dp.getUInt("stage", 0);
  uint32_t pUpSec  = dp.getUInt("upsec", 0);
  uint32_t pHeap   = dp.getUInt("heap", 0);
  uint32_t pLoops  = dp.getUInt("loops", 0);
  uint32_t pBoots  = dp.getUInt("boots", 0);
  uint32_t pErase  = dp.getUInt("erases", 0);
  uint32_t pFreeE  = dp.getUInt("freeE", 0);
  String   pLkW    = dp.getString("lkw", "");
  int32_t  pLk1    = dp.getInt("lk1", 0);
  int32_t  pLk2    = dp.getInt("lk2", 0);
  dp.end();

  int nowReason = (int)esp_reset_reason();

  JsonDocument doc;
  doc["bootReason"]    = nowReason;               // reset that started THIS run
  doc["bootReasonTx"]  = resetReasonText(nowReason);
  doc["prevValid"]     = pValid;                  // 0 = RTC was lost (real power cycle)
  doc["prevReason"]    = pReason;                 // why the LAST run ended
  doc["prevReasonTx"]  = resetReasonText(pReason);
  doc["prevStage"]     = pStage;                  // 1=web 2=display 5=timeline
                                                  // 6=240V 7=LED 8=FastLED.show
                                                  // 3=IO48 blinks 4=diagnostics
                                                  // 9=deferred brightness save
  doc["prevUpSec"]     = pUpSec;                  // how long it survived
  doc["prevHeap"]      = pHeap;                   // free heap just before it died
  doc["prevLoops"]     = pLoops;
  doc["freeHeap"]      = (uint32_t)ESP.getFreeHeap();
  doc["minFreeHeap"]   = (uint32_t)ESP.getMinFreeHeap();
  doc["boots"]         = pBoots;      // boots since the last real power cycle
  doc["erases"]        = pErase;      // of those, how many erased the NVS
  doc["nvsFreeEntries"]= pFreeE;      // NVS headroom (0-30 means nearly full)
  doc["leakWhat"]      = pLkW;        // last /leaktest, persisted so a flapping
  doc["leakPass1"]     = pLk1;        // AP cannot lose the result
  doc["leakPass2"]     = pLk2;
  doc["uptimeSec"]     = (uint32_t)(millis() / 1000);
  doc["shows"]         = (uint32_t)ledShowCount;
  doc["wifiClients"]   = WiFi.softAPgetStationNum();
  doc["displayActive"] = displayActive ? 1 : 0;
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// ---------------------------------------------------------------------------
// /log.txt - a persistent diagnostic log on LittleFS.
//
// Log writes are QUEUED and performed by the audio task on core 0. A LittleFS
// write can stall for tens of milliseconds while flash is erased, and doing that
// from the render loop shows up as a visible freeze in the display - which must
// never happen. logAppendNow() writes directly and is used only during setup,
// before the audio task exists.
// ---------------------------------------------------------------------------
#define LOG_PATH "/log.txt"
#define LOG_MAX  16384

static void logWriteNow(const char* line) {
  File f = LittleFS.open(LOG_PATH, "r");
  size_t sz = f ? f.size() : 0;
  if (f) f.close();
  if (sz > LOG_MAX) LittleFS.remove(LOG_PATH);       // rotate: start a fresh file

  File w = LittleFS.open(LOG_PATH, "a");
  if (!w) return;
  w.printf("[%8lus] %s\n", (unsigned long)(millis() / 1000), line);
  w.close();
}

static void logAppendNow(const char* fmt, ...) {
  char buf[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  logWriteNow(buf);
}

// Runtime version: hand the line to the audio task instead of touching flash.
// A small RING is required - a single slot loses lines whenever several are
// queued in the same loop pass (hb + audio + tx are written together every 30 s).
// Deep enough for one whole diagnostics burst. At 6 slots the heartbeat block
// (hb + audio + bassbeat + clock + bar + fires + tx + ledrst = 8 lines) silently
// dropped its NEWEST entries, which is why fires/tx/ledrst never appeared in
// /log.txt even though they were being written.
#define LOG_QUEUE 16
char             logQueue[LOG_QUEUE][160];
volatile uint8_t logQHead = 0, logQTail = 0;
portMUX_TYPE     logLineMux = portMUX_INITIALIZER_UNLOCKED;

void logAppend(const char* fmt, ...) {
  char buf[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  // ALSO out to USB serial, not just the RAM ring.
  //
  // The ring is drained to LittleFS by logService(), which is deliberately NOT
  // called while a show is running: a LittleFS write is a flash erase, and an
  // erase freezes BOTH cores for 20-40 ms - that was the display stall. The price
  // of that fix was that the log went dark exactly while the display was running,
  // i.e. when the instrument matters most.
  //
  // Serial does not touch flash, so it is safe during a show. The write is paced
  // by availableForWrite() and the line is DROPPED if the CDC FIFO is short of
  // room: an unread FIFO blocking a write is what caused the original freeze
  // symptom, so it must never block here.
  if (Serial.availableForWrite() >= 160) {
    Serial.printf("[%8lus] %s\n", (unsigned long)(millis() / 1000), buf);
  }

  portENTER_CRITICAL(&logLineMux);
  uint8_t next = (uint8_t)((logQHead + 1) % LOG_QUEUE);
  if (next != logQTail) {                    // drop the line if the ring is full
    strncpy(logQueue[logQHead], buf, sizeof(logQueue[0]) - 1);
    logQueue[logQHead][sizeof(logQueue[0]) - 1] = '\0';
    logQHead = next;
  }
  portEXIT_CRITICAL(&logLineMux);
}

// Called from the audio task (core 0): drains the ring to flash.
void logService() {
  while (logQTail != logQHead) {
    char buf[160];
    portENTER_CRITICAL(&logLineMux);
    strncpy(buf, logQueue[logQTail], sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    logQTail = (uint8_t)((logQTail + 1) % LOG_QUEUE);
    portEXIT_CRITICAL(&logLineMux);
    logWriteNow(buf);
  }
}

// Keep the log small across reboots: if it has grown, retain only its most
// recent part. The BOOT line written immediately afterwards carries the previous
// run's summary (reason, uptime, heap), so nothing important is lost.
static void logTrimAtBoot() {
  File f = LittleFS.open(LOG_PATH, "r");
  if (!f) return;
  size_t sz = f.size();
  if (sz <= 1500) { f.close(); return; }
  f.seek(sz - 1200);
  String tail = f.readString();
  f.close();
  File w = LittleFS.open(LOG_PATH, "w");
  if (!w) return;
  w.print(tail);
  w.close();
}

// ---------------------------------------------------------------------------
// Serial command interface.
// The USB-Serial/JTAG port is normally left alone (opening it can reset the
// board), but when a host DOES connect we can be asked for the log directly
// instead of having it e-mailed: send 'L' for the log, 'D' for /diag.
// Writes are paced by availableForWrite() so they can never block the loop - an
// unread CDC FIFO blocking a write is what caused the original freeze symptom.
// ---------------------------------------------------------------------------
void serialDumpFile(const char* path) {
  File f = LittleFS.open(path, "r");
  if (!f) { Serial.print("\n(no such file)\n"); return; }
  Serial.print("\n-----BEGIN-----\n");
  while (f.available()) {
    if (Serial.availableForWrite() <= 0) { delay(5); continue; }
    Serial.write((uint8_t)f.read());
  }
  f.close();
  Serial.print("\n-----END-----\n");
}

void handleLog() {
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  if (!LittleFS.exists(LOG_PATH)) {
    server.send(200, "text/plain", "(log is empty)\n");
    return;
  }
  File f = LittleFS.open(LOG_PATH, "r");
  if (!f) {
    server.send(500, "text/plain", "log open failed\n");
    return;
  }
  server.streamFile(f, "text/plain");
  f.close();
}

// ---------------------------------------------------------------------------
// LEAK METER. Measures the heap cost of each output path in isolation, so a
// leak can be attributed instead of guessed:  /leaktest?what=fastled|led|relay
//   fastled = the LED output path (FastLED show() on all ten strips)
//   led     = drawPortPattern+advance (our per-strip engine, no transmission)
//   relay   = executePatternStep      (the 240V engine)
// ---------------------------------------------------------------------------
void handleLeakTest() {
  String what = server.hasArg("what") ? server.arg("what") : "fastled";
  int n = server.hasArg("n") ? constrain(server.arg("n").toInt(), 1, 400) : 30;

  // PASS 1 warms the driver's buffer pool: 30 back-to-back show() calls never let a
  // transmission finish, so the pool MUST grow a buffer per in-flight call.
  // Only PASS 2 matters - a drop there is a genuine per-call leak, whereas a
  // big pass-1 drop that vanishes in pass 2 is just one-time pool growth.
  auto runBody = [&](int count) {
    for (int i = 0; i < count; i++) {
      if (what == "led") {
        drawPortPattern(0);
        advancePort(0);
      } else if (what == "relay") {
        executePatternStep(false);
      } else {
        ledShowAll();
      }
    }
  };

  uint32_t b1 = ESP.getFreeHeap();
  runBody(n);
  uint32_t a1 = ESP.getFreeHeap();
  delay(400);                       // let the transmissions finish + buffers return
  uint32_t b2 = ESP.getFreeHeap();
  runBody(n);
  uint32_t a2 = ESP.getFreeHeap();
  int32_t pass1 = (int32_t)((int64_t)a1 - (int64_t)b1);
  int32_t pass2 = (int32_t)((int64_t)a2 - (int64_t)b2);

  // Persist the result: with the AP flapping the HTTP reply can be lost, but the
  // numbers then still show up in /diag (and in the UI's diag line).
  {
    Preferences dp;
    dp.begin("diag", false);
    dp.putString("lkw", what);
    dp.putInt("lk1", pass1);
    dp.putInt("lk2", pass2);
    dp.end();
  }
  logAppend("LEAKTEST %s n=%d pass1=%dB pass2=%dB (%.2f/call) heap=%u min=%u",
            what.c_str(), n, (int)pass1, (int)pass2, (float)pass2 / (float)n,
            (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap());

  JsonDocument doc;
  doc["what"]        = what;
  doc["n"]           = n;
  doc["pass1"]       = pass1;
  doc["pass2"]       = pass2;                       // this is the real leak signal
  doc["perCall2"]    = (float)pass2 / (float)n;
  doc["heapAfter"]   = a2;
  doc["minFreeHeap"] = (uint32_t)ESP.getMinFreeHeap();
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// /beatoffset?ms=N - delays the DIRECTION CHANGE so it lands in the MIDDLE of the
// beat pulse instead of on the leading edge. Only the ear can set this correctly,
// so it is adjustable at runtime and persisted.
void handleBeatOffset() {
  if (server.hasArg("ms")) {
    beatOffsetMs = (unsigned long)constrain(server.arg("ms").toInt(), 0, 80);
    saveSettingsPending = true;      // deferred to core 0: never write flash here
    server.send(200, "text/plain", "beat offset = " + String(beatOffsetMs) + " ms");
  } else {
    server.send(200, "text/plain", "beat offset = " + String(beatOffsetMs) + " ms (use ?ms=N)");
  }
}

// /bassthr?db=N - how far the FFT's low band must rise above the room's quiet
// level before a beat is counted. Lower it (e.g. 6) to catch claps; raise it
// (e.g. 12) for kick drum only. The band itself is set by the Frequency slider.
void handleBassThr() {
  if (server.hasArg("db")) bassOverThr = constrain(server.arg("db").toFloat(), 1.0f, 40.0f);
  server.send(200, "text/plain", "bass beat threshold = " + String(bassOverThr, 1) + " dB");
}

void handleSaveFlash() {
  saveSettings();     // persist settings + theme colours (NVS only, no LED access)
  flashReq = true;    // the display task performs the confirmation blink
  server.send(200, "text/plain", "Flash executed");
}

// Called by the UI on load to force all outputs off
void handleInitOff() {
  previewMode = 0;
  displayActive = false;   // back to the quiet, dark idle state
  blankReq = true;         // display task performs the blanking
  showStatus(4);           // IO48: 4 blinks = init-off received
  server.send(200, "text/plain", "All outputs off");
}

void WiFiEvent(WiFiEvent_t event) {
  // NOTE: no Serial output here. These prints ran in the Wi-Fi event task; an
  // unread USB-CDC port can stall a write for ~100 ms, stalling that task and
  // backing up the event queue.
  switch (event) {
    case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
      break;
    case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
      break;
    default:
      break;
  }
}

// ==========================================
// AUDIO BEAT DETECTION (FFT)
// One 256-point window per call (~33 ms, ~31 Hz per bin). The MAX9814 sits at
// mid-rail, so the DC bias is removed before the Hann window. Band magnitudes
// become dB, referenced to a slow-tracking floor, which self-calibrates the
// levels to the room; a beat is the bass level spiking above its own running
// mean. The UI Frequency slider sets the bass band top, Music Source picks
// BASS or the whole spectrum, and Beat Multiplier adds predicted sub-beats.
// ==========================================

// Slow-tracking noise floor: falls quickly, rises slowly.
static inline float trackFloor(float &floor, float db) {
  if (floor == 0.0f)   floor = db;
  else if (db < floor) floor += (db - floor) * 0.35f;
  else                 floor += (db - floor) * 0.006f;
  return floor;
}

// ---------------------------------------------------------------------------
// FAST ADC PATH
//
// analogRead() costs ~58 us per call: the Arduino core takes a mutex, runs a
// oneshot conversion AND applies a calibration curve every time. That capped the
// capture at 16.8 kHz - below the 24 kHz a 12 kHz microphone needs.
//
// adc_oneshot_read() is the same conversion with no calibration pass, so it is a
// few microseconds instead of ~58. It returns raw 0-4095, which is what
// analogRead() returned anyway, so the FFT, the envelope and the DC tracker are
// all unaffected.
//
// AUDIO_PIN is GPIO2 = ADC1 channel 1. It MUST be ADC1 - WiFi owns ADC2.
//
// SAFETY: the Arduino core also claims this pin for analogRead(), and a channel
// cannot always be configured on two handles at once. So configuration is
// attempted once, and any failure permanently falls back to analogRead(). The
// active path is reported in the heartbeat, so the log says which one is running
// rather than leaving it to be assumed.
// ---------------------------------------------------------------------------
#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_continuous.h>

#define AUDIO_ADC_UNIT  ADC_UNIT_1
#define AUDIO_ADC_CHAN  ADC_CHANNEL_1      // GPIO2 on the ESP32-S3

// Target sample rate for continuous capture. A 12 kHz microphone needs >= 24 kHz,
// and the standard 10-band EQ's top band is 16 kHz, so 26 kHz keeps real headroom
// over the Nyquist limit while staying cheap.
#define AUDIO_ADC_RATE 26000

static adc_oneshot_unit_handle_t   audioAdc    = nullptr;
static adc_continuous_handle_t     audioDma    = nullptr;
static bool                        audioAdcFast = false;
static bool                        audioDmaOk   = false;

// DMA frame buffer. conv_frame_size must be a multiple of 4; SOC_ADC_DIGI_RESULT_BYTES
// bytes per conversion (2 on the ESP32-S3 for TYPE2 output).
#define AUDIO_DMA_FRAME 256
static uint8_t audioDmaBuf[AUDIO_DMA_FRAME];

// ---------------------------------------------------------------------------
// DMA CAPTURE IS DISABLED.
//
// adc_continuous_start() reported success but delivered nothing: fs collapsed to
// 3,923 Hz (from 16.8 kHz on the oneshot path) and every band read 0.00 / -120 dB
// - i.e. silence. Frames either never arrive or the TYPE2 frame layout is not what
// the parse assumes, so adc_continuous_read() times out and retries, which is
// slower than the per-sample path it was meant to replace. Note this failed
// SILENTLY: it reported a working mode while producing no audio, which is why the
// heartbeat prints adc= and the band levels rather than just "started OK".
//
// The code below is kept so the investigation is not lost, but is not started.
// To retry: set to 1, then check adc= reads dma AND that bass/mid/treb are
// non-zero. A silent audio path is worse than a slow one.
//
// The oneshot path (16.8 kHz, verified clean: copy max ~130 us, beats firing)
// is what runs.
// ---------------------------------------------------------------------------
#define AUDIO_USE_DMA 0

static void audioAdcInit() {
  // ---- path 1: oneshot, raw (no calibration). THE ACTIVE PATH. ---------------
  adc_oneshot_unit_init_cfg_t ucfg = {};
  ucfg.unit_id  = AUDIO_ADC_UNIT;
  ucfg.ulp_mode = ADC_ULP_MODE_DISABLE;
  if (adc_oneshot_new_unit(&ucfg, &audioAdc) == ESP_OK) {
    adc_oneshot_chan_cfg_t ccfg = {};
    ccfg.bitwidth = ADC_BITWIDTH_12;
    ccfg.atten    = ADC_ATTEN_DB_11;   // matches analogSetPinAttenuation(..., 3)
    if (adc_oneshot_config_channel(audioAdc, AUDIO_ADC_CHAN, &ccfg) != ESP_OK) {
      audioAdc = nullptr;
    } else {
      audioAdcFast = true;
    }
  }

#if AUDIO_USE_DMA
  // ---- path 2: continuous + DMA. NOT STARTED - see the note above. -----------
  adc_continuous_handle_cfg_t dcfg = {};
  dcfg.max_store_buf_size = AUDIO_DMA_FRAME * 4;
  dcfg.conv_frame_size    = AUDIO_DMA_FRAME;
  if (adc_continuous_new_handle(&dcfg, &audioDma) != ESP_OK) {
    audioDma = nullptr;
    return;
  }

  adc_digi_pattern_config_t pat = {};
  pat.atten     = ADC_ATTEN_DB_11;
  pat.channel   = AUDIO_ADC_CHAN;
  pat.unit      = AUDIO_ADC_UNIT;
  pat.bit_width = ADC_BITWIDTH_12;

  adc_continuous_config_t ccfg2 = {};
  ccfg2.sample_freq_hz = AUDIO_ADC_RATE;
  ccfg2.conv_mode      = ADC_CONV_SINGLE_UNIT_1;      // never ADC2: WiFi owns it
  ccfg2.format         = ADC_DIGI_OUTPUT_FORMAT_TYPE2; // ESP32-S3
  ccfg2.pattern_num    = 1;
  ccfg2.adc_pattern    = &pat;
  if (adc_continuous_config(audioDma, &ccfg2) != ESP_OK) {
    adc_continuous_deinit(audioDma);
    audioDma = nullptr;
    return;
  }
  if (adc_continuous_start(audioDma) != ESP_OK) {
    adc_continuous_deinit(audioDma);
    audioDma = nullptr;
    return;
  }
  audioDmaOk = true;
#endif
}

// One raw sample from the oneshot path. Falls back to analogRead() for good if the
// fast path ever errors.
static inline int audioAdcRead() {
  if (audioAdcFast) {
    int v = 0;
    if (adc_oneshot_read(audioAdc, AUDIO_ADC_CHAN, &v) == ESP_OK) return v;
    audioAdcFast = false;               // one failure is enough; don't retry
  }
  return analogRead(AUDIO_PIN);
}

// ---------------------------------------------------------------------------
// ORIGINAL 10 ms BEAT WINDOW - restored
//
// The original firmware evaluated peak-to-peak every 10 ms and asked "is there a
// beat?" 100 times a second, firing on a fixed threshold with a 200 ms refractory.
//
// This one waited for the FFT window (62 ms), so it asked 16 times a second: a
// kick was detected up to 62 ms late, and two kicks inside one window collapsed
// into ONE. That is what made the response feel weak and sluggish compared with
// the original - it was never a filter or a threshold problem, it was that the
// detector was welded to the FFT window and nothing had questioned it.
//
// The window is back to 10 ms. Timing comes from a SAMPLE COUNTER (not millis())
// so the inner loop stays cheap, and the decision is made here rather than once
// per FFT window.
// ---------------------------------------------------------------------------
#define BEAT_HZ 100
static int beatStride = 168;               // samples per 10 ms; set from audioFs
static int beatMax = 0, beatMin = 4095, beatCount = 0;
volatile bool fastBeatFlag = false;        // consumed by checkAudioBeat()

// Threshold + refractory, evaluated every 10 ms. This is the ORIGINAL detector's
// logic, at the original rate. p2pThr is adaptive (half the running average,
// floored above room noise) rather than the original's fixed 120, because the
// comment on the old threshold records that this microphone only produces
// 27-122 counts - so a fixed 120 sat right on the edge and caught only the
// loudest hits. The rise test stays: at 10 ms it means a real attack, whereas at
// 62 ms it was comparing smoothed trends.
static void fastBeatCheck() {
  if (p2pAvg <= 0.0f) p2pAvg = (float)envP2P;
  p2pAvg += ((float)envP2P - p2pAvg) * 0.02f;      // ~1.5 s of history
  p2pThr = p2pAvg * 0.5f;
  // Floor 40 - BETWEEN silence and music, which is the whole job.
  //
  // Measured with this envelope filter: silence gives p2p ~12, music 59-223. The
  // floor has to sit in that gap or the detector either fires on room noise or
  // misses beats. Lowering it to 6 and then 2 (while chasing a problem the
  // band-pass had created) made silence fire 4 beats/second - the dot changed
  // direction rapidly with no music playing at all.
  //
  // The adaptive part is half the running average; this floor stops that average
  // following the ROOM down during quiet passages.
  if (p2pThr < 40.0f) p2pThr = 40.0f;

  // REFRACTORY ONLY - exactly the original detector's logic, no hysteresis.
  //
  // I added hysteresis here (the level had to dip below 70% of the threshold
  // before re-arming). It was wrong: in a loud, dense passage the level never
  // dips, so it fell through to the 400 ms timeout and capped detection at 2.5
  // beats/second. At 178 BPM - 337 ms between beats - that discarded every second
  // beat, measured as inserted=63 against hits=57 (more beats generated by the
  // clock than detected).
  //
  // The 200 ms refractory is the original's mechanism and is sufficient: a hit's
  // decaying tail is long gone inside 200 ms, so nothing double-fires. And since
  // 337 ms > 200 ms it never limits a real tempo.
  static unsigned long fastLastBeatMs = 0;
  unsigned long now = millis();

  if (envP2P > p2pThr && (now - fastLastBeatMs) >= 200) {
    fastLastBeatMs  = now;
    lastTrigMs      = now;
    rawBeatFlag     = true;                // IO48: flash on every detection
    beatLatch240    = true;                // latched so a beat falling between
    beatLatchLED    = true;                // FFT windows is never lost
    fastBeatFlag    = true;
  }
}

// ---------------------------------------------------------------------------
// ONSET DETECTION FUNCTION - the spectral-flux equivalent
//
// Professional beat detectors do NOT use an amplitude envelope. They compute
// SPECTRAL FLUX: the sum of positive bin-to-bin changes between frames. It works
// because a beat IS a change - a sustained bass note produces zero flux, overall
// level is irrelevant, and uncorrelated noise cancels.
//
// A real spectral flux needs ~50-100 frames/second. Our FFT runs at 17/second
// (one 1024-point transform per 58 ms), far too coarse to time beats. So this is
// the single-band time-domain equivalent, evaluated every 10 ms:
//
//     band-pass 40-250 Hz  ->  rectify  ->  NORMALISE by running level
//                          ->  positive first difference  ->  short averager
//
// Normalising BEFORE differencing is the crucial step: it makes the detector
// immune to volume, which the envelope follower could never be. That is why
// silence and music both measured p2p=6-8 on the old scheme, and why silence
// fired 4 beats a second.
//
// Replaces: low-pass -> peak-to-peak -> compare to half a running average.
// ---------------------------------------------------------------------------
#define ODF_HP_HZ    40.0f      // band bottom: removes rumble and DC
#define ODF_LP_HZ   250.0f      // band top: keeps the kick, rejects snare/hat/vocal
#define ODF_HIST      32        // 320 ms of novelty history for the local mean
#define ODF_K        0.85f      // trigger at local_mean * ODF_K + beatSens
#define ODF_MIN_GAP    90UL     // minimum ms between onsets

// BEAT SENSITIVITY - the absolute part of the threshold, and the control that
// actually matters.
//
// thr = local_mean * ODF_K + beatSens
//
// The novelty curve spends most of its time near zero, so local_mean sits around
// 0.11 and the adaptive term contributes almost nothing. A delta of 0.02 put the
// threshold at ~0.12 while peaks reach 0.43 - so nearly every peak fired, giving
// 4-5 detections a SECOND. Raised to 0.15 the threshold sits mid-peak and the
// detector fires on real kicks only.
//
// Exposed to the UI as "Beat Sensitivity" (declared with the globals near the top).

// Level-follower coefficient. This was 0.001 (~60 ms), which LAGGED the kick and
// flattened the normalised ratio - measured peaks 0.116 against a local mean of
// 0.126, i.e. the peaks were BELOW the mean, so no value of ODF_K above ~0.9
// could ever trigger. 0.004 is ~15 ms: fast enough to track the kick's own
// envelope so the PEAK stands proud of the average instead of being smoothed into
// it. This is the difference between "detects some beats" and "detects the beat".
#define ODF_LEVEL_A  0.004f

static float odfHp1 = 0, odfHp2 = 0, odfHp1Prev = 0, odfHp2Prev = 0;
static float odfLp1 = 0, odfLp2 = 0, odfLp3 = 0, odfLp4 = 0;
static float odfRectPrev = 0, rectAvg = 0, odfNormPrev = 0, odfNovelty = 0, winMax = 0, odfPrevWin = 0;
static float odfHist[ODF_HIST];
static int   odfHistIdx = 0, odfHistCount = 0;
static unsigned long odfLastMs = 0;
volatile bool odfDisplayBeat = false;   // onset latch for the DOT, consumed in loop()
float odfThr = 0.0f;            // logged, so the threshold can be read directly

// Peak-picking, every 10 ms. A LOCAL mean (not a global one) adapts to how dense
// the music is, and the delta stops a quiet passage triggering on its own noise.
static void odfEvaluate() {
  odfHist[odfHistIdx] = odfNovelty;
  odfHistIdx = (odfHistIdx + 1) % ODF_HIST;
  if (odfHistCount < ODF_HIST) odfHistCount++;

  // ABSOLUTE threshold scaled by the sensitivity slider. No local mean.
  //
  // The local mean was the other half of the dense-mix fault: it rose with the
  // music, so the bar rose too, and the detector's behaviour depended on how busy
  // the mix was. The original firmware used a fixed bar and worked on drums for
  // that reason. beatSens 0.15 -> 9 counts, 1.00 -> 60. Band peaks in music reach
  // 5-40, so the whole slider range is usable.
  // REVERSED SCALE: beatSens is now SENSITIVITY, so higher = MORE beats, and the
  // slider's right-hand end is the most sensitive - as it should read.
  //
  // thr = (1 - beatSens) * 60
  //   beatSens 0.2 -> thr 48   (strict: the setting you chose)
  //   beatSens 1.0 -> thr  0   (most sensitive, most beats)
  //   beatSens 0.0 -> thr 60   (least sensitive)
  odfThr = (1.0f - beatSens) * 60.0f;

  unsigned long now = millis();
  if (odfNovelty > odfThr && (now - odfLastMs) >= ODF_MIN_GAP) {
    odfLastMs       = now;
    lastTrigMs      = now;
    rawBeatFlag     = true;                // IO48: one flash per onset
    odfDisplayBeat  = true;                // THE DOT: direction change on this onset
    beatLatch240    = true;                // latched so an onset falling between
    beatLatchLED    = true;                // FFT windows is never lost
    fastBeatFlag    = true;
  }
}

// Per-sample processing, shared by both acquisition paths so they cannot drift.
static inline void audioProcessSample(float s, int &onsetSub, int stride) {
  // Original envelope detection. alpha = dt/(rc+dt) makes the filter rate
  // independent, so it behaves the same at any sample rate.
  // ---- ONSET DETECTION FUNCTION (header comment is above odfEvaluate) --------
  float dt = 1.0f / audioFs;

  // The Frequency slider sets the band's LOWER edge (20-100 Hz), so it still does
  // something real: raise it to reject bass-guitar rumble and tighten onto the
  // kick, lower it to let the deep end in. The upper edge is fixed at 250 Hz.
  float hpHz = constrain((float)freqThreshold, 20.0f, 100.0f);

  // 2-pole high-pass at hpHz: removes rumble and DC. Two cascaded one-poles,
  // which is cheap, stable and steep enough to reject the room.
  float rcLo = 1.0f / (2.0f * 3.14159265f * hpHz);
  float aLo  = rcLo / (rcLo + dt);
  odfHp1 = aLo * (odfHp1 + s - odfHp1Prev);       odfHp1Prev = s;
  odfHp2 = aLo * (odfHp2 + odfHp1 - odfHp2Prev);  odfHp2Prev = odfHp1;

  // 2-pole low-pass at 250 Hz: keeps the kick, rejects snare, hats and vocals.
  float rcHi = 1.0f / (2.0f * 3.14159265f * ODF_LP_HZ);
  float aHi  = dt / (rcHi + dt);
  odfLp1 += aHi * (odfHp2 - odfLp1);
  odfLp2 += aHi * (odfLp1 - odfLp2);
  // Two more poles: 24 dB/octave above 250 Hz. This is what keeps vocals, snare
  // and guitars out - at 12 dB/octave they leaked through and swamped the onset
  // function whenever the mix got busy.
  odfLp3 += aHi * (odfLp2 - odfLp3);
  odfLp4 += aHi * (odfLp3 - odfLp4);

  float bpRect = fabsf(odfLp4);

  // NORMALISE by the running level BEFORE differencing. This is the step that
  // makes the detector immune to volume: the ratio sits around 1.0 whatever the
  // music is doing, so a kick stands out equally in a quiet or a loud passage.
  // The envelope follower could not do this, which is why it could not tell
  // music from a quiet room.
  rectAvg += 0.0f * (bpRect - rectAvg); // unused   // level follower, ~15 ms
  if (bpRect > winMax) winMax = bpRect;     // peak within this 10 ms window

  // Plain level readings, kept only for the log.
  float v = bpRect;
  if (v > envMax) envMax = v;
  if (v < envMin) envMin = v;

  // ---- evaluate the ODF every 10 ms ----------------------------------------
  if (v > (float)beatMax) beatMax = (int)v;
  if (v < (float)beatMin) beatMin = (int)v;
  if (++beatCount >= beatStride) {
    beatCount = 0;
    envP2P = beatMax - beatMin;
    beatMax = 0;
    beatMin = 4095;

    // ABSOLUTE frame-to-frame change in band energy. NO normalisation.
    //
    // It used to be normalised by a running average, and THAT was why dense mixes
    // went wild: as instruments and vocals arrived the average rose, so the
    // normaliser compressed the signal - which amplifies any proportional
    // fluctuation. The detector became hypersensitive exactly when the mix got
    // busy. An absolute change cannot behave that way, and the original firmware's
    // fixed threshold worked on drums for precisely this reason.
    //
    // Combined with the 4-pole low-pass above, vocals now contribute neither
    // leakage nor compression.
    float flux = (float)winMax - odfPrevWin;
    odfPrevWin = (float)winMax;
    odfNovelty = (flux > 0.0f) ? flux : 0.0f;
    winMax = 0.0f;

    odfEvaluate();
  }

  // Sample-level envelope, kept only to feed the autocorrelation's onset history.
  // It is NOT a trigger: it works on raw full-bandwidth samples, so hi-hats move
  // it as much as a kick drum, which is what made IO48 flicker like a flame.
  dcAvg += (s - dcAvg) * 0.0005f;                     // ~0.2 s DC tracker
  float rect = fabsf(s - dcAvg);
  if (rect > envA) envA = rect;                       // instant attack
  else             envA += (rect - envA) * 0.02f;     // ~60 ms release
  if (envA > envPeak) envPeak = envA; else envPeak *= 0.9995f;

  // Onset-strength history at a fixed 100 Hz. The stride comes from the MEASURED
  // sample rate so the tempo scaling stays right at any rate.
  if (++onsetSub >= stride) {
    onsetSub = 0;
    float nov = (envA > onsetPrev) ? (envA - onsetPrev) : 0.0f;
    onsetPrev = envA;
    onsetBuf[onsetIdx] = nov;
    onsetIdx = (onsetIdx + 1) % ONSET_LEN;
    if (onsetCount < ONSET_LEN) onsetCount++;
  }
}

// Capture one audio window and run the FFT, updating the band levels.
static void audioAnalyse() {
  int64_t t0 = esp_timer_get_time();
  envMax = 0; envMin = 4095;              // fresh peak-to-peak window

  // ---------------------------------------------------------------------------
  // CAPTURE PACING REMOVED.
  //
  // This loop used to take 8 samples and then busy-wait 300 us. With 512 samples
  // that is 64 bursts x 300 us = 19.2 ms of deliberate delay inside a ~49 ms
  // window, which capped the capture at ~10.4 kHz - the "fs=10450" in every
  // heartbeat. The ADC was never the limit; the delay was. A 12 kHz microphone
  // needs >= 24 kHz sampling, so the pacing was throwing away more than half of
  // what the mic can deliver.
  //
  // Sampling now runs in bursts of 256 with a real vTaskDelay(1) between them
  // (4 per window instead of 64). The yield is NOT optional: without it this task
  // holds core 0 for ~30 ms and starves the WiFi radio and the idle task.
  //
  // `stride` is hoisted out of the loop - it was a float division per sample, and
  // the loop now runs ~3x more often.
  // ---------------------------------------------------------------------------
  static int onsetSub = 0;
  int stride = (int)(audioFs / (float)ONSET_HZ);
  if (stride < 1) stride = 1;
  // Samples per 10 ms beat window, from the measured rate. Hoisted out of the
  // per-sample loop so there is no float division in there.
  beatStride = (int)(audioFs / (float)BEAT_HZ);
  if (beatStride < 1) beatStride = 1;

  int i = 0;
  if (audioDmaOk) {
    // ---- DMA path: the hardware has already sampled; this only collects. ------
    // No per-sample driver call, which is what cost ~60 us a sample and capped
    // the rate at 16.8 kHz. The guard stops a stalled ADC from hanging the task.
    uint32_t guard = millis() + 250;
    while (i < FFT_SAMPLES && (int32_t)(millis() - guard) < 0) {
      uint32_t n = 0;
      if (adc_continuous_read(audioDma, audioDmaBuf, AUDIO_DMA_FRAME, &n, 20) != ESP_OK) {
        continue;
      }
      // SOC_ADC_DIGI_RESULT_BYTES per conversion (2 on the S3, TYPE2: 12-bit data
      // in the low bits, channel in the high nibble).
      for (uint32_t b = 0; b + SOC_ADC_DIGI_RESULT_BYTES <= n && i < FFT_SAMPLES;
           b += SOC_ADC_DIGI_RESULT_BYTES) {
        uint16_t w = (uint16_t)audioDmaBuf[b] | ((uint16_t)audioDmaBuf[b + 1] << 8);
        float s = (float)(w & 0x0FFF);
        fftReal[i] = s;
        fftImag[i] = 0.0f;
        audioProcessSample(s, onsetSub, stride);
        i++;
      }
    }
    // Backfill any shortfall with silence rather than leaving stale data in the
    // FFT, so a missed frame shows up as a quiet window instead of a false peak.
    for (; i < FFT_SAMPLES; i++) { fftReal[i] = 0.0f; fftImag[i] = 0.0f; }
  } else {
    // ---- oneshot path: one driver call per sample -----------------------------
    for (i = 0; i < FFT_SAMPLES; i++) {
      float s = (float)audioAdcRead();
      fftReal[i] = s;
      fftImag[i] = 0.0f;
      audioProcessSample(s, onsetSub, stride);
      if ((i & 255) == 255) vTaskDelay(1);   // 4 yields per 1024-sample window
    }
  }
  vTaskDelay(1);                          // one yield per window, either path
  envP2P = envMax - envMin;
  float elapsedUs = (float)(esp_timer_get_time() - t0);
  audioFs = (float)FFT_SAMPLES * 1000000.0f / elapsedUs;
  if (audioFs < 1000.0f) audioFs = 1000.0f;

  // Remove the DC bias so the FFT sees only the audio.
  float mean = 0.0f;
  for (int n = 0; n < FFT_SAMPLES; n++) mean += fftReal[n];
  mean /= (float)FFT_SAMPLES;
  for (int n = 0; n < FFT_SAMPLES; n++) fftReal[n] -= mean;

  static ArduinoFFT<float> fft(fftReal, fftImag, FFT_SAMPLES, audioFs);
  fft.windowing(FFTWindow::Hann, FFTDirection::Forward);
  fft.compute(FFTDirection::Forward);
  fft.complexToMagnitude();

  float binHz   = audioFs / (float)FFT_SAMPLES;
  // The Frequency slider is the beat band's top edge, but it is floored at 90 Hz:
  // below that (with ~21 Hz bins) the band shrinks to one or two sub-bass bins that
  // carry almost no energy in music, and the beat detector then never fires at all -
  // which is exactly what happened with the slider at 31 Hz (IO48 went silent).
  // The slider can still raise the top edge freely above 90 Hz.
  // The Frequency slider (20-100 Hz) selects the TOP of the beat band; the band
  // runs from the first usable FFT bin up to that value. A 33 Hz setting - the one
  // that gave the best visual response on the original firmware - was impossible
  // before now because an artificial 90 Hz floor was clamping it, so the control
  // did nothing below 90. The floor is gone: the slider means what it says.
  // Bin width is audioFs/FFT_SAMPLES (~21 Hz at 512 points), so the very lowest
  // setting gives a one-bin band; higher settings widen it into the kick region.
  float binW = audioFs / (float)FFT_SAMPLES;
  if (binW < 1.0f) binW = 1.0f;
  float bassTop    = constrain((float)freqThreshold, 20.0f, 100.0f);
  float bassBottom = binW;
  if (bassTop < bassBottom + binW) bassTop = bassBottom + binW;
  bassBandLo = bassBottom;               // logged, so the band in use is visible
  bassBandHi = bassTop;
  float bass = 0, mid = 0, treb = 0, all = 0;
  for (int b = 1; b < FFT_SAMPLES / 2; b++) {
    float hz = (float)b * binHz;
    float m  = fftReal[b];
    all += m;
    if (hz >= bassBottom && hz <= bassTop)      bass += m;
    else if (hz > bassTop && hz <= FFT_MID_MAX) mid  += m;
    else if (hz > FFT_MID_MAX)                  treb += m;
  }
  rawBass = bass; rawMid = mid; rawTreb = treb; rawAll = all;

  float dbAll  = 20.0f * log10f(all  + 1e-6f);
  float dbBass = 20.0f * log10f(bass + 1e-6f);
  float dbMid  = 20.0f * log10f(mid  + 1e-6f);
  float dbTreb = 20.0f * log10f(treb + 1e-6f);

  audioLevel  = constrain((dbAll  - trackFloor(bandFloorAll,    dbAll )) / 24.0f, 0.0f, 1.0f);
  audioBass   = constrain((dbBass - trackFloor(bandFloorBass,   dbBass)) / 24.0f, 0.0f, 1.0f);
  audioMid    = constrain((dbMid  - trackFloor(bandFloorMid,    dbMid )) / 24.0f, 0.0f, 1.0f);
  audioTreble = constrain((dbTreb - trackFloor(bandFloorTreble, dbTreb)) / 24.0f, 0.0f, 1.0f);

  // Bass-band onset metrics for the beat detector in checkAudioBeat(). The rise
  // between consecutive windows is the onset strength; dbBass - floor is how far
  // the band currently sits above the room's quiet level. Both are needed: a rise
  // alone fires on noise, and a level alone fires continuously during a sustained
  // bass note.
  bassRiseDb  = dbBass - bassDbPrev;
  bassDbPrev  = dbBass;
  bassDbLevel = dbBass;
  bassOverDb  = dbBass - bandFloorBass;
}

// Onset values, logged every 30 s so the thresholds can be tuned from the
// board's own numbers.
float onsetMag = 0.0f;   // raw magnitude of the onset band
float onsetAvg = 0.0f;   // its slow average
float onsetPeak = 0.0f;  // highest mag/avg ratio since the last log line

// One FFT window plus the beat decision (called from the audio task on core 0).
bool checkAudioBeat() {
  audioAnalyse();

  // Use the sample-level onset trigger: it is stamped WHEN the hit happened, not
  // when this window finished, so the beat intervals no longer carry a varying
  // window-end delay (which was the drift) and the trigger point is consistent
  // (which was the jitter).
  unsigned long now = millis();
  bool triggered = false;                 // decided below, from the filtered p2p

  // PRIMARY: the ORIGINAL detector - the preferred "musicality". The RC-filtered
  // peak-to-peak over the window with the same 120-count threshold and 200 ms
  // refractory the earlier firmware used. The FFT still runs alongside it (its
  // band levels drive the VU meters / future equaliser).
  // Threshold, tuned from the board's own log: with music playing this
  // microphone produces p2p of only ~27-122, so the original fixed 120 sat right
  // on the edge and caught just the loudest hits ("some response, not good
  // enough"). Track the running average and trigger at half of it, with a floor
  // well above the quiet-room noise. This is what keeps it musical as the
  // volume varies.
  // The beat was ALREADY decided in fastBeatCheck(), every 10 ms, inside the
  // sample loop - see the header comment above it. That is the whole fix: this
  // function used to make the decision, but it only runs once per FFT window
  // (62 ms), so it was asking "is there a beat?" 16 times a second instead of 100.
  // Consume the latch here. `triggered` was declared above.
  if (fastBeatFlag) {
    fastBeatFlag  = false;
    triggered     = true;
    beatTriggerMs = lastTrigMs;
  }
  bool realBeat = triggered;

  // ---- THE ORIGINAL DETECTOR (restored properly this time) ------------------
  // The beat now comes from the RC-FILTERED PEAK-TO-PEAK (envP2P), which is the
  // original firmware's detector, and the Frequency slider is its low-pass cutoff.
  // At 33 Hz that is a gentle one-pole filter, which in practice acts as a
  // BROADBAND envelope detector - it is why 33 Hz felt right and why it responds
  // to the whole body of a kick rather than one narrow slice of it.
  //
  // The FFT bass band is NOT used for the beat any more. At 33 Hz with ~21 Hz bins
  // it selected a single, almost empty bin, so the same slider number produced a
  // far weaker response than the original. The FFT still runs for the VU meters,
  // and its levels are still logged.
  //
  // NOTE: the trigger test that used to live here has moved to fastBeatCheck(),
  // which runs every 10 ms inside the per-sample loop. It CANNOT be done here:
  // this function only runs once per FFT window (62 ms), which is exactly what
  // made the response feel weak - it was asking "is there a beat?" 16 times a
  // second instead of 100. `realBeat` is set above from the latched flag.

  // ---- TEMPO BY AUTOCORRELATION (every ~2.5 s) -----------------------------
  // The clock is driven by the measured beat period rather than by individual
  // triggers, so a stray transient cannot set the pace.
  static unsigned long lastAcMs = 0;
  if (onsetCount >= 128 && millis() - lastAcMs >= 2500) {
    lastAcMs = millis();
    int lagMin = ONSET_HZ * 240 / 1000;        // 240 ms
    int lagMax = ONSET_HZ * 1200 / 1000;       // 1200 ms
    int   best = 0;
    float bestScore = 0.0f;
    for (int lag = lagMin; lag <= lagMax && lag < onsetCount; lag++) {
      float s = 0.0f;
      for (int i = lag; i < onsetCount; i++) s += onsetBuf[i] * onsetBuf[i - lag];
      if (s > bestScore) { bestScore = s; best = lag; }
    }
    if (best > 0 && bestScore > 0.0f) {
      // The autocorrelation also peaks at twice the beat; if half the best lag
      // scores nearly as well, that shorter lag IS the beat.
      int half = best / 2;
      if (half >= lagMin) {
        float s2 = 0.0f;
        for (int i = half; i < onsetCount; i++) s2 += onsetBuf[i] * onsetBuf[i - half];
        if (s2 >= bestScore * 0.8f) { best = half; bestScore = s2; }
      }
      unsigned long t = (unsigned long)best * 1000UL / ONSET_HZ;
      // Gate the result. The log showed it locking onto a SUB-MULTIPLE of the
      // beat (270 ms scoring only 98k against 460 ms at 514k), which made the
      // metronome run at double time. Accept a result if it is either close to the
      // tempo already trusted, or a strong peak in its own right - so one odd
      // reading cannot jerk the clock, but a genuine tempo change still gets in.
      static float acScoreRef = 0.0f;
      bool nearCurrent = (t > clockPeriod * 8UL / 10UL) && (t < clockPeriod * 12UL / 10UL);
      if (t >= 240 && t <= 1200 && (nearCurrent || bestScore >= acScoreRef * 0.5f)) {
        acTempoMs   = t;
        acScore     = bestScore;
        clockPeriod = t;
        acScoreRef  = bestScore;
      }
    }
  }

  // ---- BEAT CLOCK ----------------------------------------------------------
  // (A) Insert any beat the clock expected but the detector did not find.
  bool insertBeat = false;
  if (clockPeriod < 240) clockPeriod = 240;    // a plausible minimum (250 BPM)
  // ORDER MATTERS: process a real detection FIRST and only insert if there is
  // none. Checking the insert first meant it almost always won (the clock was
  // inserting 4x more beats than it detected), so the correction below never ran
  // and the clock stayed pinned at its 240 ms floor - which put the pairing out of
  // phase with the music and showed up as "not on the beat".
  // The clock is the beat SOURCE now. Its beats are evenly spaced by construction,
  // so the 1-and-3 / 2-and-4 pairing cannot slip; the detector's only job is to
  // steer the clock's phase, never to contribute a beat of its own.
  //
  // The old code deadlocked here and the log proved it (hits=0 inserted=0): the
  // pause counter was only cleared when a detection landed inside a narrow window
  // of the grid, but the paused branch re-armed the grid to now+period on EVERY
  // pass - so the window ran away from every detection and the counter could never
  // fall back under its limit. The clock was inert for the whole session.
  bool clockSteered = false;
  // The metronome only runs while the music is present. If nothing has been
  // detected for two and a half periods the display should chase one way, as
  // requested, rather than click out a metronome into silence.
  bool musicPresent = (lastDetectMs != 0) &&
                      ((long)(now - lastDetectMs) < (long)(clockPeriod * 5 / 2));
  if (!realBeat && musicPresent && clockPeriod > 0 && (long)(now - clockNextBeat) >= 0) {
    if ((long)(now - clockNextBeat) > (long)clockPeriod) clockNextBeat = now + clockPeriod;
    insertBeat = true;
    clockMisses++;
    insertRun++;
  } else if (!realBeat && insertRun > 2) {
    // The music has gone: two beats have been inserted and none detected, so the
    // clock is PAUSED and the lights chase one way (as requested). Keep it ARMED,
    // though - otherwise clockNextBeat falls further and further behind while the
    // music is away, and the next real beat is treated as many beats late. The log
    // showed next-in at -237 SECONDS because of exactly that.
    clockNextBeat = now + clockPeriod;
  } else if (realBeat) {
    // (B) A real detection corrects the clock - but ONLY if it lands close to
    // where the clock expects a beat. Anything further out is a TRANSIENT, not a
    // beat, and is ignored entirely, so loud music can no longer shake the timing.
    // The tempo itself comes from the autocorrelation, not from these detections,
    // so the adopt-the-detected-tempo branch that used to live here is gone.
    lastDetectMs = now;
    insertRun = 0;                  // the music is here: never pause the clock
    long err  = (long)(now - clockNextBeat);      // + = late, - = early
    long aerr = (err < 0) ? -err : err;
    if (aerr <= (long)(clockPeriod / 2)) {
      // Close enough to be THIS beat: accept it as the beat and re-anchor, so the
      // metronome tracks the music rather than fighting it. Widened from period/8:
      // at period/8 almost nothing was ever accepted, which is half of why the
      // clock never counted a hit.
      clockSteered = true;
      // In tune: follow the drift gently, then re-anchor onto this beat. A beat
      // slightly EARLY means the clock was running late, so shortening the period
      // stops the error accumulating.
      long adj = err / 8;
      if (adj > 0)                          clockPeriod += (unsigned long)adj;
      else if (adj < 0 && clockPeriod > (unsigned long)(-adj)) clockPeriod -= (unsigned long)(-adj);
      if (clockPeriod < 240) clockPeriod = 240;
      clockNextBeat = now + clockPeriod;
      clockHits++;
      insertRun = 0;                  // the music is back: resuming inserting
    }
    // NOTE: deliberately NOT copying clockPeriod into establishedInterval. Doing
    // so formed a feedback loop: whatever wrong period the clock started with
    // became the tempo, which shrank the detector's refractory, which produced
    // even shorter intervals. The log showed it collapsing to ~140 ms (430 BPM)
    // and inserting ~200 beats per 30 s. The DETECTIONS drive the tempo; the
    // clock follows it.
  }
  // ONLY clock beats drive the display. A detection that is off the grid is a
  // steering input, not a beat: letting it through advanced the pairing by one
  // step and flipped 1-and-3 to 2-and-4 and back - the reported "missing and
  // swapping". Clock beats are evenly spaced, so the pairing holds.
  if (insertBeat) clockSteered = true;
  bool isBeat = clockSteered;
  predictedBeat = false;

  // SECONDARY: the 4/4 grid. While real beats are still arriving, keep firing on
  // the learned tempo so quiet bars stay in time and a missed beat is covered.
  // Once the music stops (2.5 intervals with no real beat) prediction stops as
  // well, so the display just continues in ONE direction until the next beat.
  if (!isBeat && establishedInterval > 200 &&
      (now - lastRealBeatTime) < (establishedInterval * 5UL) / 2UL &&
      (long)(now - nextPredictedBeat) >= 0) {
    predictedBeat = true;
    nextPredictedBeat += establishedInterval;
    if ((long)(now - nextPredictedBeat) >= 0) nextPredictedBeat = now + establishedInterval;
    // Grid beats keep the tempo alive for the VU meters, but they must NOT change
    // direction: only "the 1" of the bar (or an infill) does that.
    return false;
  }

  // Beat Multiplier: extra beats between real ones (as the original did).
  if (!isBeat && generatedSubBeatsCount > 0 && multiplier > 1 &&
      (long)(now - nextSubBeatTargetTime) >= 0) {
    generatedSubBeatsCount--;
    nextSubBeatTargetTime = now + (establishedInterval / multiplier);
    return false;        // sub-beats must not change direction either
  }

  if (isBeat && realBeat) {
    // Learn only from REAL beats, so predictions cannot distort the tempo.
    unsigned long interval = (lastRealBeatTime > 0) ? (now - lastRealBeatTime) : 0;
    // Only learn the tempo from beats during actual music: a "beat" detected over
    // a quiet passage is noise, and admitting it to the history re-learns the
    // tempo (it settled at 493 ms in silence), which shifts the whole grid when
    // the music returns - a visible tempo and phase change.
    bool musical = (rawMid > 8000.0f);
    if (lastRealBeatTime > 0 && musical) {
      // Only musical beat intervals are learnt (240-1200 ms = 250-50 BPM). The
      // detector misses beats, and a missed beat produces an interval of two or
      // three beats - learning those made the tempo estimate far too slow.
      if (interval >= 240 && interval <= 1200) {
        beatIntervalHistory[historyIndex] = interval;
        historyIndex = (historyIndex + 1) % HISTORY_SIZE;
        if (historyCount < HISTORY_SIZE) historyCount++;
      }
    }
    lastRealBeatTime = now;
    lastAcceptedBeatTime = now;

    // Adopt a new median only when it genuinely differs (>=10%), and only after
    // several beats agree. The estimate was already rock solid at 272 ms, but a
    // single stray interval could otherwise shift it and re-phase the whole grid,
    // which reads as a visible correction. A real tempo change is still followed
    // after ~8 beats.
    if (historyCount > 0) {
      unsigned long tmp[HISTORY_SIZE];
      for (int i = 0; i < historyCount; i++) tmp[i] = beatIntervalHistory[i];
      for (int i = 1; i < historyCount; i++) {          // insertion sort
        unsigned long v = tmp[i];
        int j = i - 1;
        while (j >= 0 && tmp[j] > v) { tmp[j + 1] = tmp[j]; j--; }
        tmp[j + 1] = v;
      }
      unsigned long median = tmp[historyCount / 2];
      static int tempoOutliers = 0;
      bool differs = (establishedInterval == 0) ||
                     (median > (establishedInterval * 110UL) / 100UL) ||
                     (median < (establishedInterval * 90UL) / 100UL);
      if (differs) {
        if (++tempoOutliers >= 8) { establishedInterval = median; tempoOutliers = 0; }
      } else {
        tempoOutliers = 0;
      }
    } else {
      establishedInterval = 0;
    }
    // Re-phase the grid onto this beat: that is what follows the tempo drift,
    // and an off-grid beat (an infill) simply re-phases the grid onto itself.
    nextPredictedBeat = now + establishedInterval;
    if (multiplier > 1 && establishedInterval > 200) {
      generatedSubBeatsCount = multiplier - 1;
      nextSubBeatTargetTime = now + (establishedInterval / multiplier);
    }

    // ---- 4/4 bar: change direction on "the 1", or on every hit in an infill --
    // Infills are clusters of hits faster than the learnt tempo; they must not
    // disturb the statistics, because those statistics are how "the 1" is learnt.
    // An infill is a RUN of off-tempo hits, not a single one. Real infills occur
    // every 4, 8, 12 or 16 bars, so a single short or long interval is far more
    // likely to be ordinary timing jitter - treating that as an infill made the
    // display fire on almost every bar. Require three consecutive off-tempo hits.
    bool onTempo = (establishedInterval > 200) &&
                   (interval > (establishedInterval * 75UL) / 100UL) &&
                   (interval < (establishedInterval * 125UL) / 100UL);
    if (onTempo) {
      offTempoRun = 0;
    } else if (offTempoRun < 10) {
      offTempoRun++;
    }
    bool infillConfirmed = (offTempoRun >= 3);
    bool changeDir;
    // ---- METRONOME MODE (as requested) ---------------------------------------
    // Change direction on every SECOND clock beat and nothing else. The
    // downbeat-learning path below is the source of the swapping: whenever its
    // phase estimate shifted it moved the whole pairing from 1-and-3 to 2-and-4,
    // which is exactly what "losing time and off-beat" looks like. A pure toggle
    // cannot swap - it alternates on evenly spaced clock beats, so it settles on
    // 1-and-3 OR 2-and-4 and holds. Infills are excluded entirely.
    // Set metronomeMode false to bring the bar-energy downbeat learning back.
    const bool metronomeMode = true;
    if (metronomeMode || establishedInterval <= 200) {
      beatToggle = (beatToggle + 1) & 1;
      changeDir  = (beatToggle == 1);
      infillMode = false;
      barPos = (barPos + 1) % 4;       // kept so the log still reads
    } else if (!infillConfirmed) {
      infillMode = false;
      if (barPos < 0 || barPos > 3) barPos = 0;
      barEnergySum[barPos] += (float)envP2P;
      barEnergyCnt[barPos]++;
      // The downbeat is learnt as the grid slot with the highest average level.
      bool  ready = true;
      int   best = 0;
      float bestAvg = -1.0f;
      for (int i = 0; i < 4; i++) {
        if (barEnergyCnt[i] < 4) { ready = false; break; }   // need a few bars
        float a = barEnergySum[i] / (float)barEnergyCnt[i];
        if (a > bestAvg) { bestAvg = a; best = i; }
      }
      if (ready) {
        // Only accept a downbeat when the winner CLEARLY beats the runner-up
        // (>=12%). Here the four slot averages sit within ~5% of each other, so
        // the "winner" kept swapping slots - and every swap shifted which beats
        // fired, which is exactly the visible drift-then-correct. Once chosen it
        // is LOCKED, so the pairing never jumps mid-track.
        float best2 = -1.0f;
        for (int i = 0; i < 4; i++) {
          if (i == best) continue;
          float a = barEnergySum[i] / (float)barEnergyCnt[i];
          if (a > best2) best2 = a;
        }
        bool slotsReady = true;
        for (int i = 0; i < 4; i++) if (barEnergyCnt[i] < 8) slotsReady = false;
        if (lockedDownbeat < 0 && slotsReady && bestAvg > best2 * 1.20f) lockedDownbeat = best;
        downbeatPos = lockedDownbeat;
      }
      // How many beats elapsed since the previous one? Normally 1; if a beat was
      // MISSED the interval is ~2x the tempo. Advancing the bar by that count
      // stops the 1-and-3 pairing slipping whenever a beat is not detected -
      // which is exactly the "in and out of phase, obviously late" behaviour.
      // The expected interval is used ONLY to work out how much time passed; the
      // direction change itself always fires on the detected beat, immediately.
      int steps = 1;
      if (establishedInterval > 200 && interval > (establishedInterval * 3UL) / 2UL) {
        steps = (int)((interval + establishedInterval / 2) / establishedInterval);
        if (steps < 1) steps = 1;
        if (steps > 4) steps = 4;
      }

      // Fire on the "1" and the beat two later when the downbeat is known and
      // locked; otherwise on every second on-tempo beat. Adding `steps` preserves
      // the parity when a beat is missed, so the pairing never slips.
      if (lockedDownbeat < 0) {
        beatToggle = (beatToggle + steps) & 1;
        changeDir = (beatToggle == 1);
      } else {
        int off = (barPos - lockedDownbeat + 4) % 4;
        changeDir = (off == 0) || (off == 2);
      }
      barPos = (barPos + steps) % 4;
    } else {
      // Infills are IGNORED for the direction change for now. A steady 1-and-3
      // (or 2-and-4) is what is wanted while the timing is being verified, and
      // firing on fills made the reversals far too frequent to judge. The phase
      // still advances so a fill cannot leave the pairing behind.
      infillMode = true;
      changeDir  = false;
      barPos = (barPos + 1) % 4;
      beatToggle ^= 1;
    }
    if (changeDir) { firedCount++; return true; }
    return false;
  }

  return false;
}

// ==========================================// ==========================================// 240V RELAY PATTERN ENGINE
// ==========================================
void executePatternStep(bool beatHeard) {
  const int N = 10;
  bool on[N];
  for (int i = 0; i < N; i++) on[i] = false;

  int effQty = constrain(quantity240V, 1, 4);   // 240V supports 1-4 dots
  int dotSize = constrain(size240V, 1, 4);      // 240V dot size is 1-4 channels

  // Reset motion when the pattern changes
  static String lastP = "";
  if (lastP != activePattern240V) {
    lastP = activePattern240V;
    dotPos240 = 0;
    dotDirection = (activePattern240V == "chase_10_1") ? -1 : 1;
  }

  if (activePattern240V == "center_out_chase") {
    if (beatHeard) dotPos240 = 0;
    if (dotPos240 < 5) {
      int lc = 4 - dotPos240;
      int rc = 5 + dotPos240;
      for (int s = 0; s < dotSize; s++) {
        int li = lc - s;
        int ri = rc + s;
        if (li >= 0 && li < N) on[li] = true;
        if (ri >= 0 && ri < N) on[ri] = true;
      }
    }
    dotPos240++;
    if (dotPos240 > 5) dotPos240 = 0;
  }
  else if (activePattern240V.startsWith("vu_")) {
    int level = constrain(computeVuLevel(vuSmooth240V, attack240V, decay240V), 0, 10);
    if (activePattern240V == "vu_meter") {
      if (level > vuPeakLevel240V) { vuPeakLevel240V = level; lastVuPeakFall240V = millis(); }
      else if (millis() - lastVuPeakFall240V > 150) { if (vuPeakLevel240V > 0) vuPeakLevel240V--; lastVuPeakFall240V = millis(); }
      for (int i = 0; i < level; i++) on[i] = true;
      if (vuPeakLevel240V > level) on[vuPeakLevel240V - 1] = true;
    }
    else if (activePattern240V == "vu_meter_rev") {
      for (int i = 0; i < level; i++) on[N - 1 - i] = true;
    }
    else if (activePattern240V == "vu_dual") {
      int half = level / 2;
      for (int i = 0; i < half; i++) { on[i] = true; on[N - 1 - i] = true; }
    }
    else if (activePattern240V == "vu_center_out") {
      int half = constrain(level / 2, 0, 5);
      for (int i = 0; i < half; i++) { on[4 - i] = true; on[5 + i] = true; }
    }
  }
  else {
    // Dot / chase patterns: moving_dot, chase_1_10, chase_10_1, knight_rider
    // NOTE: the direction change is applied the instant a beat is detected (see
    // loop()), NOT deferred to this step. Deferring it made the reversal land
    // late by up to one step interval - the "not on time" complaint.

    for (int q = 0; q < effQty; q++) {
      int base = (dotPos240 + (q * N) / effQty) % N;
      for (int s = 0; s < dotSize; s++) on[(base + s) % N] = true;
    }

    // Advance the lead dot, honouring the wrap toggle:
    //   wrap ON  -> reverse direction at the end (bounce)
    //   wrap OFF -> jump back to the start
    dotPos240 += dotDirection;
    int maxPos = N - dotSize;
    if (maxPos < 0) maxPos = 0;
    if (dotPos240 > maxPos) {
      if (wrap240V) { dotPos240 = maxPos; dotDirection = -dotDirection; }
      else dotPos240 = 0;
    } else if (dotPos240 < 0) {
      if (wrap240V) { dotPos240 = 0; dotDirection = -dotDirection; }
      else dotPos240 = maxPos;
    }
  }

  if (invert240V) {
    for (int i = 0; i < N; i++) on[i] = !on[i];
  }
  for (int i = 0; i < N; i++) digitalWrite(channelPins[i], on[i] ? HIGH : LOW);
}

void run240VDisplayPattern(bool beat) {
  unsigned long currentMillis = millis();

  // NOTE: IO48 is driven separately in loop() as the RAW DETECTOR INDICATOR, so
  // there is deliberately no beat flash here.
  if (powerLedOffAt != 0 && (long)(currentMillis - powerLedOffAt) >= 0) {
    digitalWrite(POWER_LED_PIN, LOW);
    powerLedOffAt = 0;
  }

  // The Speed slider (0.01-1 s) sets the step interval directly.
  bool isVu = activePattern240V.startsWith("vu_");
  unsigned long interval = isVu ? 30UL : (unsigned long)(constrain(speed240V, 0.01f, 5.0f) * 1000.0);
  if (interval < 20) interval = 20;

  if (currentMillis - lastStepTime >= interval) {
    lastStepTime = currentMillis;
    // Consume the LATCHED beat: one that arrived between steps must not be lost.
    bool heard = beatLatch240;
    beatLatch240 = false;
    executePatternStep(heard);
  }
}

// ==========================================
// WS2811 LED PATTERN ENGINE — PER STRIP
// (Types, portCfg[] and the per-strip state arrays are declared near the top.)
// ==========================================

LedPattern patternEnumFromName(const String& name) {
  if (name == "chase_1_10")       return LP_CHASE_1_10;
  if (name == "chase_10_1")       return LP_CHASE_10_1;
  if (name == "center_out_chase") return LP_CENTER_OUT;
  if (name == "knight_rider")     return LP_KNIGHT_RIDER;
  if (name == "vu_meter")         return LP_VU_METER;
  if (name == "vu_meter_rev")     return LP_VU_REV;
  if (name == "vu_dual")          return LP_VU_DUAL;
  if (name == "vu_center_out")    return LP_VU_CENTER;
  return LP_MOVING_DOT;
}

// Fill an entire strip with one (calibrated) colour
void setPortColor(int port, CRGB color) {
  if (port < 0 || port >= NUM_PORTS) return;
  CRGB calibrated = applyCalibrationOrder(color, portColorOrders[port]);
  for (int i = 0; i < NUM_LEDS_PER_STRIP; i++) leds[port][i] = calibrated;
}

// Write one LED on a port, applying that port's calibration order
void setLed(int port, int idx, CRGB color) {
  if (port < 0 || port >= NUM_PORTS) return;
  if (idx < 0 || idx >= NUM_LEDS_PER_STRIP) return;
  leds[port][idx] = applyCalibrationOrder(color, portColorOrders[port]);
}

// LED-page settings become the defaults for every strip
void applyGlobalsToPorts() {
  LedPattern pat = patternEnumFromName(activePatternLED);
  for (int p = 0; p < NUM_PORTS; p++) {
    portCfg[p].pat    = pat;
    portCfg[p].speed  = speedLED;
    portCfg[p].qty    = constrain(quantityLED, 1, 8);
    portCfg[p].size   = constrain(sizeLED, 1, 10);
    portCfg[p].wrap   = wrapLED;
    portCfg[p].invert = invertLED;
    for (int i = 0; i < 4; i++) portCfg[p].colour[i] = colourLED[i];
  }
}

// Colour for a VU meter LED at absolute index `idx`, from a 4-entry palette.
// The four bands are FIXED along the strip (the top quarter is always the peak
// colour), which is the classic VU bar look - a red LED means "near full scale",
// regardless of how much is lit.
//
// Takes the palette array rather than a PortCfg reference on purpose: the Arduino
// IDE inserts auto-generated prototypes ABOVE the type declarations, so a
// signature mentioning PortCfg cannot be parsed there and fails the whole build.
CRGB vuBandColor(const uint32_t *pal, int idx) {
  if (idx < 0 || idx >= NUM_LEDS_PER_STRIP) return CRGB::Black;
  int band = (idx * 4) / NUM_LEDS_PER_STRIP;
  if (band > 3) band = 3;
  return CRGB(pal[band]);
}

// Draw the current frame of ONE strip using its own pattern + parameters
void drawPortPattern(int port) {
  const int N = NUM_LEDS_PER_STRIP;
  ledDirty[port] = true;              // this strip's data changed; it must be copied
  const int lc = (N / 2) - 1;
  const int rc = N / 2;
  const PortCfg &c = portCfg[port];
  const CRGB mainCol(c.colour[0]);

  for (int i = 0; i < N; i++) leds[port][i] = CRGB::Black;

  int dotLen = constrain(c.size, 1, 10);
  int effQty = constrain(c.qty, 1, 8);

  if (c.pat == LP_CENTER_OUT) {
    int off = ledExplArr[port];
    for (int s = 0; s < dotLen; s++) {
      setLed(port, lc - off - s, mainCol);
      setLed(port, rc + off + s, mainCol);
    }
  }
  else if (c.pat >= LP_VU_METER) {
    int level = constrain(computeVuLevel(vuSmoothLED, attackLED, decayLED), 0, 10);
    int lit = level * (N / 10);
    if (c.pat == LP_VU_METER) {                       // left -> right
      for (int i = 0; i < lit; i++) setLed(port, i, vuBandColor(c.colour, i));
    } else if (c.pat == LP_VU_REV) {                  // right -> left
      for (int i = 0; i < lit; i++) setLed(port, N - 1 - i, vuBandColor(c.colour, N - 1 - i));
    } else if (c.pat == LP_VU_DUAL) {                 // both ends inward
      for (int i = 0; i < lit / 2; i++) {
        setLed(port, i, vuBandColor(c.colour, i));
        setLed(port, N - 1 - i, vuBandColor(c.colour, N - 1 - i));
      }
    } else {                                          // centre -> outward
      int half = lit / 2;
      for (int i = 0; i < half; i++) {
        setLed(port, lc - i, vuBandColor(c.colour, lc - i));
        setLed(port, rc + i, vuBandColor(c.colour, rc + i));
      }
    }
  }
  else {
    // moving_dot / chase_1_10 / chase_10_1 / knight_rider
    int pos = ledPosArr[port];
    for (int q = 0; q < effQty; q++) {
      int base = (pos + (q * N) / effQty) % N;
      for (int s = 0; s < dotLen; s++) setLed(port, (base + s) % N, mainCol);
    }
    if (c.pat == LP_KNIGHT_RIDER) {
      // Tail is a dimmed version of the port's own colour, so the trail always
      // matches the dot rather than being hardcoded red.
      CRGB tail = mainCol;
      tail.nscale8(50);
      for (int t = 1; t <= 6; t++) {
        int idx = (pos - ledDirArr[port] * t + N) % N;
        setLed(port, idx, tail);
      }
    }
  }

  if (c.invert) {   // on -> off, off -> on
    for (int i = 0; i < N; i++) {
      CRGB col = leds[port][i];
      if (col.r == 0 && col.g == 0 && col.b == 0)
        leds[port][i] = applyCalibrationOrder(mainCol, portColorOrders[port]);
      else
        leds[port][i] = CRGB::Black;
    }
  }
}

// Advance ONE strip's motion (the wrap toggle chooses bounce vs jump)
void advancePort(int port) {
  const int N = NUM_LEDS_PER_STRIP;
  const PortCfg &c = portCfg[port];

  if (c.pat == LP_CENTER_OUT) {
    ledExplArr[port]++;
    if (ledExplArr[port] >= N / 2) ledExplArr[port] = 0;
    return;
  }
  if (c.pat >= LP_VU_METER) return;   // level-driven

  int dotLen = constrain(c.size, 1, 10);
  if (ledDirArr[port] == 0) ledDirArr[port] = 1;   // never allow a zero direction
  ledPosArr[port] += ledDirArr[port];
  int maxPos = N - dotLen;
  if (maxPos < 0) maxPos = 0;

  if (ledPosArr[port] > maxPos) {
    if (c.wrap) { ledPosArr[port] = maxPos; ledDirArr[port] = -ledDirArr[port]; }
    else ledPosArr[port] = 0;
  } else if (ledPosArr[port] < 0) {
    if (c.wrap) { ledPosArr[port] = 0; ledDirArr[port] = -ledDirArr[port]; }
    else ledPosArr[port] = maxPos;
  }
}

// Step every strip independently, then ONE ledShowAll() per frame.
// The step rate comes ONLY from each strip's Speed setting - antifreeze never
// affects speed, only direction, and only when Audio Detect is off.
void runLEDPattern() {
  unsigned long now = millis();
  bool drew = false;

  bool anyDrew = false;
  for (int p = 0; p < NUM_PORTS; p++) {
    const PortCfg &c = portCfg[p];
    bool isVu = (c.pat >= LP_VU_METER);
    unsigned long interval = isVu ? 25UL : (unsigned long)(constrain(c.speed, 0.005f, 5.0f) * 1000.0);
    // Floor the step at the frame rate (5 ms), NOT 20 ms. The 20 ms floor here was
    // a hard 50 steps/second ceiling that no slider setting could beat - it is the
    // "48/s" that showed up in every log and the reason more speed was impossible.
    if (interval < 5) interval = 5;

    if (now - ledStepMs[p] < interval) continue;
    ledStepMs[p] = now;
    g_stepCount++;

    // A step MUST be displayed, so it can never be faster than the frame rate -
    // otherwise the dot jumps and pixels are never lit. Keep this equal to frameMs.
    if (interval < 5) interval = 5;

    drawPortPattern(p);
    advancePort(p);
    anyDrew = true;
  }

  // One show per frame, never one per port.
  //
  // 10 ms (100 fps) as a starting point. A ten-strip parallel transfer should be
  // only a few milliseconds, so this is deliberately conservative until the
  // measured figure is read back from the log (showtime avg/max). With parallel
  // output the frame rate - not the transmission - sets the maximum dot speed:
  // 10 ms = 100 pixels/second = ~1 s to traverse a 100-LED strip.
  static unsigned long lastShowMs = 0;
  // 5 ms (200 fps) - the hardware floor. A ten-strip transmit takes a hard 4.7 ms,
  // so this is as fast as the bus can be driven. The 40 ms stalls blamed on this duty
  // cycle were in fact LittleFS flash erases (cache disabled on BOTH cores) and are
  // now fixed. If CanShow() starts skipping frames, this is past the hardware limit
  // and should go back up toward 7 ms.
  const unsigned long frameMs = 5;
  if (anyDrew && !beatLatchLED) {}          // (keep the beat latch semantics)
  if (anyDrew && (now - lastShowMs >= frameMs)) {
    lastShowMs = now;
    dbgStage = 8;
    ledShowCount++;
    {
      int32_t hBefore = (int32_t)ESP.getFreeHeap();
      ledShowAll();          // the ACTIVE output driver, not FastLED directly
      g_showHeapDelta += (int32_t)ESP.getFreeHeap() - hBefore;
      g_showDeltaN++;
    }
    dbgStage = 7;
  }
  if (anyDrew) beatLatchLED = false;        // every stepping strip has now seen it

}

// ==========================================
// TIMELINE (per-strip patterns) + MASTER BRIGHTNESS
// The UI saves /timeline.json as { "delay": sec, "items": { "row_col": {...} } }
// where col 1..10 = LED strips and col 11 = the 240V display.
// ==========================================
void applyMasterBrightness() {
  uint8_t s = (uint8_t)map((int)masterBrightness, 0, 100, 0, 255);
  ledBrightnessScale = s;          // master brightness as 0..255
  FastLED.setBrightness(s);        // used only by the FastLED fallback path
}

void loadTimeline() {
  for (int r = 0; r < TL_ROWS; r++)
    for (int c = 0; c < TL_COLS; c++) tlData[r][c].used = false;
  tlLoaded = false;

  if (!LittleFS.exists("/timeline.json")) return;
  File f = LittleFS.open("/timeline.json", "r");
  if (!f) return;
  JsonDocument doc;
  DeserializationError e = deserializeJson(doc, f);
  f.close();
  if (e) return;

  tlDelaySec = doc["delay"] | 20.0f;
  if (tlDelaySec < 1.0f) tlDelaySec = 1.0f;

  JsonObject items = doc["items"].as<JsonObject>();
  for (JsonPair kv : items) {
    String key = String(kv.key().c_str());          // "row_col"
    int us = key.indexOf('_');
    if (us < 1) continue;
    int r = key.substring(0, us).toInt();
    int c = key.substring(us + 1).toInt();
    if (r < 0 || r >= TL_ROWS || c < 1 || c > 11) continue;

    JsonObject it = kv.value().as<JsonObject>();
    String nm = String((const char*)(it["name"] | ""));
    String id = patternIdFromName(nm);
    strlcpy(tlData[r][c].id, id.c_str(), sizeof(tlData[r][c].id));
    tlData[r][c].speed  = jsonNumF(it["speed"],    0.2f);
    tlData[r][c].qty    = jsonNumI(it["quantity"], 1);
    tlData[r][c].size   = jsonNumI(it["size"],     1);
    tlData[r][c].wrap   = it["wrap"] | false;
    tlData[r][c].invert = it["invert"] | false;
    tlData[r][c].used   = true;
  }
  tlLoaded = true;
}

// Apply one timeline row.
// SEMANTICS (per the brief): a cell's item PLAYS FOR "delay" SECONDS - the delay
// is the slot length, not the pattern's internal timing. If a channel's item is
// identical to the one already playing, nothing is touched, so consecutive
// identical cells run as ONE continuous block (no restart / no glitch).
// Empty cells leave that channel playing what it already has.
// Column 11 controls the 240V display; columns 1-10 control the strips.
void applyTimelineRow(int row, bool force) {
  if (row < 0 || row >= TL_ROWS || !tlLoaded) return;

  for (int c = 1; c <= 10; c++) {
    if (!tlData[row][c].used) continue;
    int p = c - 1;
    TlItem &it = tlData[row][c];
    LedPattern newPat = patternEnumFromName(String(it.id));
    int newQty  = constrain(it.qty, 1, 8);
    int newSize = constrain(it.size, 1, 10);

    bool changed = force ||
                   portCfg[p].pat != (int)newPat ||
                   fabsf(portCfg[p].speed - it.speed) > 0.0001f ||
                   portCfg[p].qty != newQty ||
                   portCfg[p].size != newSize ||
                   portCfg[p].wrap != it.wrap ||
                   portCfg[p].invert != it.invert;
    if (!changed) continue;            // identical -> continuous run

    portCfg[p].pat    = (int)newPat;
    portCfg[p].speed  = it.speed;
    portCfg[p].qty    = newQty;
    portCfg[p].size   = newSize;
    portCfg[p].wrap   = it.wrap;
    portCfg[p].invert = it.invert;
    // restart this strip's motion only because its item actually changed
    ledPosArr[p] = 0; ledDirArr[p] = 1; ledExplArr[p] = 0; ledStepMs[p] = 0;
  }

  // ---- PER-PORT INVERT IS FORCED OFF ---------------------------------------
  // It was ON for every port except port 1, and its only effect on
  // drawPortPattern() is to invert the frame - a single dot becomes a fully lit
  // strip. Forced off so the display is correct; the feature is otherwise unbranded.
  for (int p = 0; p < NUM_PORTS; p++) portCfg[p].invert = false;

  if (tlData[row][11].used) {                    // column 11 = 240V display
    TlItem &it = tlData[row][11];
    int newQty  = constrain(it.qty, 1, 4);
    int newSize = constrain(it.size, 1, 4);
    bool changed = force ||
                   activePattern240V != String(it.id) ||
                   fabsf(speed240V - it.speed) > 0.0001f ||
                   quantity240V != newQty ||
                   size240V != newSize ||
                   wrap240V != it.wrap ||
                   invert240V != it.invert;
    if (changed) {
      activePattern240V = String(it.id);
      speed240V    = it.speed;
      quantity240V = newQty;
      size240V     = newSize;
      wrap240V     = it.wrap;
      invert240V   = it.invert;
      dotPos240 = 0; dotDirection = 1;
    }
  }
}

// Time-division sequencer: advance one slot every "delay" seconds. The delay
// value only sets how long each timeline cell is displayed.
void runTimelineSequencer() {
  if (!tlLoaded) return;
  unsigned long now = millis();
  if (now - tlRowStart >= (unsigned long)(tlDelaySec * 1000.0f)) {
    tlRowStart = now;
    tlRow = (tlRow + 1) % TL_ROWS;
    applyTimelineRow(tlRow, false);
  }
}

// ==========================================
// RENDER HELPERS (executed only by the display task)
// ==========================================
void renderCalibrationTest() {
  for (int p = 0; p < NUM_PORTS; p++)
    for (int i = 0; i < NUM_LEDS_PER_STRIP; i++) leds[p][i] = CRGB::Black;
  int port = calPort, order = calOrder;
  if (port >= 0 && port < NUM_PORTS) {
    leds[port][0] = applyColorOrder(CRGB::Red, order);
    leds[port][1] = applyColorOrder(CRGB::Green, order);
    leds[port][2] = applyColorOrder(CRGB::Blue, order);
  }
  ledShowAll();
}

void renderThemePreview() {
  for (int p = 0; p < NUM_PORTS; p++)
    for (int i = 0; i < NUM_LEDS_PER_STRIP; i++) leds[p][i] = CRGB::Black;
  int count = previewColorCount;
  for (int p = 0; p < NUM_PORTS; p++)
    for (int i = 0; i < count && i < NUM_LEDS_PER_STRIP; i++)
      leds[p][i] = applyCalibrationOrder(previewColors[i], portColorOrders[p]);
  ledShowAll();
}

void blankDisplays() {
  clearAllChannels();
  for (int p = 0; p < NUM_PORTS; p++)
    for (int i = 0; i < NUM_LEDS_PER_STRIP; i++) leds[p][i] = CRGB::Black;
  ledShowAll();
}

// "Saved" confirmation: blink the current frame once, then blank it.
void doSaveFlash() {
  static CRGB savedFrame[NUM_PORTS][NUM_LEDS_PER_STRIP];
  for (int p = 0; p < NUM_PORTS; p++)
    for (int i = 0; i < NUM_LEDS_PER_STRIP; i++) savedFrame[p][i] = leds[p][i];

  ledClearAll(); ledShowAll(); delay(120);
  for (int p = 0; p < NUM_PORTS; p++)
    for (int i = 0; i < NUM_LEDS_PER_STRIP; i++) leds[p][i] = savedFrame[p][i];
  ledShowAll(); delay(160);
  ledClearAll(); ledShowAll();

  previewMode = 0;
}

// ==========================================
// CORE SEPARATION
//   core 0 : audio sampling / beat detection only  (never touches the display)
//   core 1 : display task (priority above the web loop) + web UI (loopTask)
// The display task is the ONLY renderer, so Wi-Fi, the UI and the audio work
// can never stall or corrupt a frame.
// ==========================================
// ---------------------------------------------------------------------------
// WEB TASK - core 0.
// CARDINAL RULE: the display must never be affected by any CPU demand. A single
// HTTP request was measured at 188 ms, and because the web server shared loop()
// with the render, that request blocked the lights (looptime max ~300 ms versus a
// ~6 ms average). The web server now runs on its own low-priority task on core 0,
// so a request can only ever delay other web work - never a frame.
// WiFi is an editing tool; it is not in the display's path.
// ---------------------------------------------------------------------------
void webTask(void *param) {
  (void)param;
  for (;;) {
    server.handleClient();
    vTaskDelay(1);          // yield so the audio task and the radio are not starved
  }
}

//
// They were merged into loop() earlier because a second task appeared to break the
// LED output - but that was FastLED's RMT path. NeoPixelBus drives the strips from
// the LCD_CAM peripheral by DMA, so the render task is not sensitive to another
// core's work. Keeping the audio in loop() put a ~30-50 ms FFT window on the
// render task itself, which is the periodic stall in the display.
void audioTask(void *param) {
  (void)param;
  for (;;) {
    // CARDINAL RULE: the display must never be affected by any CPU demand.
    //
    // MEASURED: copy (a pure CPU memory loop) spiked to 27,465 us and tx to
    // 37,073 us in the same window. No DMA fault can stall a CPU memory loop, so
    // the CPU itself was stopped - and a 20-40 ms freeze is the signature of a
    // FLASH ERASE, which disables the cache on BOTH cores. Draining the log to
    // LittleFS here is that erase (logWriteNow -> open/write -> sector erase).
    //
    // Moving this task to core 0 did not help because the cache is shared: the
    // erase stalls core 1's render regardless of which core issued it.
    //
    // The log queue is already a RAM ring buffer, so while a show is running we
    // simply do not drain it. It is flushed when the show stops (the condition
    // goes false), so nothing is lost - and the 30 s heartbeat still goes out on
    // serial in the meantime. No flash write ever happens during a show.
    if (!displayActive) {
      logService();                        // flash writes happen here
    }
    if (saveSettingsPending && !displayActive) {   // deferred NVS save
      saveSettingsPending = false;
      saveSettings();
    }
    if (audioEnabled) {
      if (checkAudioBeat()) beatFlag = true;
      // Yield between analysis windows. This task previously spun with no delay at
      // all, saturating core 0 and contending for the bus the LCD DMA reads its
      // buffer from - which showed up as the transmit occasionally taking 42 ms
      // instead of its 4.7 ms average. A window is 30-50 ms, so a 1 ms yield costs
      // nothing and takes that pressure off the display.
      vTaskDelay(1);
    } else {
      vTaskDelay(pdMS_TO_TICKS(20));
    }
  }
}

// ==========================================
// STARTUP
// ==========================================

// Power-up sequence: flash IO48 three times and blank all displays
void powerUpSequence() {
  // Match the original firmware: 11 dB attenuation (~0-3.1 V full scale). The
  // MAX9814 sits at mid-rail, so without this the ADC's default range leaves the
  // microphone swinging through a fraction of the available codes - which is the
  // most likely reason this board reports p2p of only 47-60 where the old code
  // saw 120+ on the same kind of material.
  analogSetPinAttenuation(AUDIO_PIN, (adc_attenuation_t)3);   // 3 = 11 dB
  pinMode(POWER_LED_PIN, OUTPUT);
  for (int i = 0; i < 3; i++) {
    digitalWrite(POWER_LED_PIN, HIGH);
    delay(200);
    digitalWrite(POWER_LED_PIN, LOW);
    delay(200);
  }
  for (int p = 0; p < NUM_PORTS; p++) {
    fill_solid(leds[p], NUM_LEDS_PER_STRIP, CRGB::Black);
  }
  ledShowAll();
}

void systemInit() {
  powerUpSequence();
}

void setup() {
  // --- NVS SELF-HEAL (must run before anything touches Preferences) ---------
  // /brightness used to call saveSettings() (~35 NVS writes) on EVERY slider
  // input event, which exhausted the small NVS partition. Once full, every
  // later write silently fails - which is why the LED settings stopped
  // persisting while the 240V ones (written earlier in saveSettings) did - and
  // the Wi-Fi driver can no longer store its RF calibration, so the AP fails to
  // start. This is the standard ESP-IDF recovery.
  bool nvsWasErased = false;
  esp_err_t nvsErr = nvs_flash_init();
  if (nvsErr == ESP_ERR_NVS_NO_FREE_PAGES || nvsErr == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    nvs_flash_init();
    nvsWasErased = true;
  }
  // NEVER erase speculatively. An earlier version erased the partition whenever
  // freeEntries() was low; if that threshold is hit persistently it fires on
  // EVERY boot, which wipes the Wi-Fi RF calibration (so the AP performs a full
  // calibration at every start - the "SSID takes 20-30 s" symptom) and wipes the
  // saved settings every boot. Read the figure for /diag only.
  size_t nvsFreeE = 0;
  {
    Preferences chk;
    chk.begin("lightshow", true);
    nvsFreeE = chk.freeEntries();
    chk.end();
  }

  // --- capture how the PREVIOUS run ended BEFORE anything else can touch it ---
  int      prevReason = (int)esp_reset_reason();
  uint32_t prevValid  = (dbgMagic == DBG_MAGIC) ? 1 : 0;
  uint32_t prevStage  = dbgStage;
  uint32_t prevUpSec  = dbgUpSec;
  uint32_t prevHeap   = dbgHeap;
  uint32_t prevLoops  = dbgLoops;
  uint32_t prevBoots  = prevValid ? dbgBoots : 0;
  uint32_t prevErase  = prevValid ? dbgNvsErase : 0;
  {
    Preferences dp;
    dp.begin("diag", false);
    dp.putInt("reason",  prevReason);
    dp.putUInt("valid",  prevValid);
    dp.putUInt("stage",  prevStage);
    dp.putUInt("upsec",  prevUpSec);
    dp.putUInt("heap",   prevHeap);
    dp.putUInt("loops",  prevLoops);
    dp.end();
  }
  dbgMagic = DBG_MAGIC; dbgStage = 0; dbgUpSec = 0; dbgHeap = 0; dbgLoops = 0;
  dbgBoots = prevBoots + 1;
  dbgNvsErase = prevErase + (nvsWasErased ? 1 : 0);
  {   // NVS health + boot/erase counters. If "boots" and "erases" climb together
      // the partition is being wiped every start - that is the smoking gun for
      // settings that never stick and an AP that takes an age to appear.
    Preferences dp;
    dp.begin("diag", false);
    dp.putUInt("boots",  dbgBoots);
    dp.putUInt("erases", dbgNvsErase);
    dp.putUInt("freeE",  (uint32_t)nvsFreeE);
    dp.end();
  }
  g_prevReason = prevReason;

  systemInit();
  Serial.begin(115200);
  delay(500);
  // Diagnostic: 1=power-on, 4=panic/crash, 5=WDT, 9=brownout/reset
  Serial.printf("Boot OK. Reset reason = %d (%s) | previous run: %lus stage=%u heap=%u\n",
                prevReason, resetReasonText(prevReason),
                (unsigned long)prevUpSec, (unsigned)prevStage, (unsigned)prevHeap);

  // 10 WS2811 strips, driven by FastLED's own ESP32 driver - see the note below.
  // ==========================================================================
  // LED OUTPUT DRIVER - REVERTED TO FASTLED'S OWN DRIVER
  // ==========================================================================
  // All ten strips registered with FastLED. Output goes through the ESP32-S3's
  // PARALLEL driver (LCD_CAM / I80) - see build_opt.h. There is no RMT anywhere in
  // this project.
  {
  // NeoPixelBus LCD_CAM parallel output (ESP32-S3 only). Each instance is created
  // with its LED count and pin; Begin() registers its mux bus. Logged so the
  // bring-up can be inspected rather than guessed at.
  for (int p = 0; p < NUM_PORTS; p++) {
    g_strip[p] = new Ws2811Strip(NUM_LEDS_PER_STRIP, ledDataPins[p]);
    g_strip[p]->Begin();
    if (g_strip[p]) g_npbCreated++;
    logAppendNow("NPB strip %d pin %d created canShow=%d",
                 p + 1, ledDataPins[p], g_strip[p]->CanShow() ? 1 : 0);
  }
  g_npbReady = g_strip[0] ? (g_strip[0]->CanShow() ? 1 : 0) : -1;
  logAppendNow("NPB bring-up: created=%d ready=%d", g_npbCreated, g_npbReady);
  }

  FastLED.setBrightness(90);
  // Throttle FastLED's own output rate. The heap grows by roughly 22 bytes per
  // controller per show whenever a show is requested before the previous one has
  // drained, which empties the heap in a couple of minutes and trips the floor
  // guard's restart. setMaxRefreshRate() makes FastLED skip any show that arrives
  // too soon, which is the API-level answer to a driver queue that outruns the
  // hardware.
  FastLED.setMaxRefreshRate(50);
  ledClearAll();
  ledShowAll();

  // LED output: all ten strips registered with FastLED on the pins in ledDataPins.

  analogSetAttenuation(ADC_11db);
  pinMode(AUDIO_PIN, INPUT);
  // Try the fast (raw, uncalibrated) ADC path. If it cannot claim the channel -
  // the Arduino core also holds this pin - audioAdcRead() falls back to
  // analogRead() permanently and the heartbeat reports which path is live.
  audioAdcInit();

  for (int i = 0; i < 10; i++) {
    pinMode(channelPins[i], OUTPUT);
    digitalWrite(channelPins[i], LOW);
  }

  if (!LittleFS.begin(true)) {
    // no serial output here: keep the USB-CDC path out of the startup stack
  }

  loadSettings();
  loadPortCalibrations();

  logTrimAtBoot();
  logAppendNow("BOOT #%u reason=%d(%s) prevUp=%us prevStage=%u prevHeap=%u nvsFree=%u erased=%u out=%s",
            (unsigned)dbgBoots, prevReason, resetReasonText(prevReason),
            (unsigned)prevUpSec, (unsigned)prevStage, (unsigned)prevHeap,
            (unsigned)nvsFreeE, nvsWasErased ? 1u : 0u,
            "fastled");


  // Apply the saved maximum brightness and the saved Timeline (per-strip patterns)
  applyMasterBrightness();
  loadTimeline();
  applyGlobalsToPorts();
  applyTimelineRow(0, false);
  tlRowStart = millis();

  WiFi.mode(WIFI_AP);
  // Modem sleep ENABLED. This was set to false ("keep the radio responsive") which
  // forces the radio awake continuously - a permanent CPU load that costs frame
  // rate during playback, and playback is exactly when WiFi is not needed. The UI
  // only sends small edits, one strip at a time, so sleeping between beacons costs
  // nothing noticeable while editing and frees the render loop while running.
  WiFi.setSleep(true);
  WiFi.onEvent(WiFiEvent);
  WiFi.softAP(apSSID, apPassword);
  // Reduced from 19.5 dBm (maximum). The radio at full power draws a large current
  // spike, and a brown-out reset when a strip is plugged in says the 5 V rail is
  // already marginal. This is an access point for a phone a few metres away, so
  // 8.5 dBm is ample and it materially lowers the peak load.
  WiFi.setTxPower(WIFI_POWER_8_5dBm);

  // --- Web routes ---
  server.on("/", HTTP_GET, handleRoot);
  server.on("/ping", HTTP_GET, handlePing);
  server.on("/settings", HTTP_GET, handleGetSettings);
  server.on("/update", HTTP_POST, handleUpdate);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/calibrate-test", HTTP_GET, handleCalibrateTest);
  server.on("/calibrate-save", HTTP_GET, handleCalibrateSave);
  server.on("/theme-preview", HTTP_POST, handleThemePreview);
  server.on("/live-tweak", HTTP_POST, handleLiveTweak);
  server.on("/save-playlist", HTTP_POST, handleSavePlaylist);
  server.on("/load-playlist", HTTP_GET, handleLoadPlaylist);
  server.on("/save-timeline", HTTP_POST, handleSaveTimeline);
  server.on("/save-flash", HTTP_POST, handleSaveFlash);
  server.on("/init-off", HTTP_POST, handleInitOff);
  server.on("/brightness", HTTP_GET, handleBrightness);
  server.on("/audio", HTTP_GET, handleAudioDetect);
  server.on("/diag", HTTP_GET, handleDiag);
  server.on("/leaktest", HTTP_GET, handleLeakTest);
  server.on("/log.txt", HTTP_GET, handleLog);
  server.on("/beatoffset", HTTP_GET, handleBeatOffset);
  server.on("/bassthr", HTTP_GET, handleBassThr);

  server.begin();

  // --- Start the audio task on core 0 (rendering stays in loop() on core 1) ---
  // Audio on its own task (core 0).
  xTaskCreatePinnedToCore(audioTask, "audio", 4096, NULL, 2, NULL, 0);
  // Web server on its own task (core 0, low priority) so HTTP can never stall the
  // render loop on core 1. CARDINAL RULE: the display is immune to CPU demands.
  xTaskCreatePinnedToCore(webTask, "web", 8192, NULL, 1, NULL, 0);
}

void loop() {
  // Web UI and rendering. Audio, logging and deferred saves run on core 0
  // (audioTask) so a 30-50 ms FFT window cannot stall the display.
  //
  // The web server is serviced at a bounded rate rather than on every pass. WiFi is
  // used for building the show and for small tweaks to one strip, NOT during
  // playback - so handling it every 5 ms keeps the UI responsive while leaving the
  // render loop free to hit the frame rate that sets the dot speed.
  dbgStage = 1;
  // server.handleClient() is NOT called here. It runs on its own core-0 task
  // (webTask) so an HTTP request can never stall a frame - see webTask().
  unsigned long loopNow = millis();
  (void)loopNow;

  // Serial command interface: 'L' dumps /log.txt over USB, 'D' prints /diag.
  // Lets a host read the diagnostics without the Wi-Fi AP (and without relying
  // on anybody copying text out by hand).
  while (Serial.available()) {
    int c = Serial.read();
    if (c == 'L' || c == 'l') serialDumpFile(LOG_PATH);
    else if (c == 'D' || c == 'd') Serial.print("use /diag for JSON\n");
  }

  // --- HEAP FLOOR GUARD -----------------------------------------------------
  // A leak means an allocation eventually fails, and a failed allocation inside
  // a library calls abort() -> PANIC, which can leave the USB-Serial/JTAG and
  // the AP dead until a physical power cycle (that is the "no SSID until I
  // replug" symptom). Restart cleanly while there is still room to do so.
  if (ESP.getFreeHeap() < 15000) {
    logAppend("HEAP FLOOR: free=%u min=%u -> blanking + clean restart",
              (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap());
    blankDisplays();
    ESP.restart();
  }

  // --- display work (timed: a long render blocks the web server) ---
  dbgStage = 2;
  unsigned long t0 = micros();
  if (flashReq) {
    flashReq = false;
    doSaveFlash();
  } else if (blankReq) {
    blankReq = false;
    blankDisplays();
  } else if (previewMode == 1) {
    // Re-assert held previews periodically: they are otherwise only drawn when
    // marked dirty, so a single garbled frame (or anything that repaints the
    // strips) leaves them looking like a brief flash instead of staying on.
    if (previewDirty || (millis() - lastPreviewRender >= 500)) {
      previewDirty = false; lastPreviewRender = millis();
      renderCalibrationTest();
    }
  } else if (previewMode == 2) {
    if (previewDirty || (millis() - lastPreviewRender >= 500)) {
      previewDirty = false; lastPreviewRender = millis();
      renderThemePreview();
    }
  } else if (displayActive) {
    bool beat = beatFlag;
    if (beat) beatFlag = false;

    // ANTIFREEZE applies ONLY when Audio Detect is off: the display waits
    // `antifreeze` seconds and then changes direction. With Audio Detect ON, a
    // quiet passage or a gap between tracks must NOT flip the direction - the
    // display keeps chasing one way until it hears the next beat.
    static unsigned long lastAnti = 0;
    if (!audioEnabled) {
      if (millis() - lastAnti >= (unsigned long)(antifreeze * 1000)) {
        lastAnti = millis();
        beat = true;
      }
    } else {
      lastAnti = millis();
    }
    // The DOT follows ONLY real onsets (set in odfEvaluate from the onset
    // detection function). It is deliberately NOT latched from the clock here.
    //
    // This was `beatLatch240 = true; beatLatchLED = true;` - the clock latching
    // both displays. That made the strip a METRONOME: it stepped on the predicted
    // grid rather than on the music, which is why it kept ticking 2-3 times a
    // second in silence and only lined up with the music some of the time. The
    // detector was finding the beats correctly; the display simply was not using
    // them. 240V keeps the clock latch (its behaviour is established and working).
    // 240V is NOT latched from the clock any more. The same detected onset drives
    // both displays (see the odfDisplayBeat block below), which also removes the
    // clock's sub-beat insertion - the reason the 240V had run so fast it blurred.
    // IO48 is the RAW DETECTOR INDICATOR: flash on every detection, so the
    // detector's behaviour can be judged directly (claps, transients, misses).
    // The lights below follow the CLOCK, not these detections.
    if (rawBeatFlag) {
      rawBeatFlag = false;
      digitalWrite(POWER_LED_PIN, HIGH);
      powerLedOffAt = millis() + 60;
    }

    // Hold the display event back by beatOffsetMs so the change lands in the
    // middle of the beat pulse.
    //
    // THE DOT IS DRIVEN FROM THE DETECTED ONSET (odfDisplayBeat), NOT from the
    // clock. This is the line that makes the difference between a metronome and a
    // display that follows the music:
    //
    //   was:  if (beat) { fireAtMs = ...; firePending = true; }   // clock tick
    //   now:  if (odfDisplayBeat) { ... }                          // real onset
    //
    // Removing beatLatchLED earlier did nothing, because this is what actually
    // moves the dot - `doBeat` below. That is why silence still stepped 2-3 times
    // a second. The clock still runs the 240V display, whose behaviour is fine.
    if (odfDisplayBeat) {
      odfDisplayBeat = false;
      beatLatch240 = true;
      // A pending fire must be allowed to COMPLETE before a new one is armed.
      //
      // Without this guard, an onset arriving faster than beatOffsetMs (200 ms)
      // reset fireAtMs before it ever elapsed, so doBeat never became true and
      // there were NO direction changes at all - the opposite of what loosening
      // the threshold was supposed to achieve. Sparse onsets completed the delay;
      // dense ones starved it.
      if (!firePending) {
        fireAtMs = millis() + beatOffsetMs;
        firePending = true;
      }
    }
    bool doBeat = false;
    if (firePending && (long)(millis() - fireAtMs) >= 0) { firePending = false; doBeat = true; }

    // Apply the direction change the INSTANT the beat is detected, to every strip
    // and the 240V display, instead of waiting for each pattern's next step. A
    // deferred flip landed late by up to one step interval (50-500 ms), which is
    // exactly the "close but not on time" complaint. The patterns keep moving at
    // their own speed; only the reversal is immediate.
    if (doBeat) {
      // Record the interval between FIRED beats: this is the timing the user
      // actually sees on IO48, so drift shows up here directly.
      static unsigned long lastFire = 0;
      unsigned long nowFire = millis();
      if (lastFire) { fireInt[fireIntIdx] = nowFire - lastFire; fireIntIdx = (fireIntIdx + 1) & 7; }
      lastFire = nowFire;

      for (int p = 0; p < NUM_PORTS; p++) {
        if (portCfg[p].pat == LP_MOVING_DOT || portCfg[p].pat == LP_KNIGHT_RIDER)
          ledDirArr[p] = -ledDirArr[p];
        else if (portCfg[p].pat == LP_CENTER_OUT)
          ledExplArr[p] = 0;
      }
      if (activePattern240V == "moving_dot" || activePattern240V == "knight_rider")
        dotDirection = -dotDirection;
    }

    runTimelineSequencer();
    dbgStage = 6;
    run240VDisplayPattern(doBeat);
    dbgStage = 7;
    runLEDPattern();
  }
  unsigned long dtUs = micros() - t0;
  if (dtUs > 100000UL && Serial.availableForWrite() > 80)
    Serial.printf("[slow] render loop took %lu ms\n", dtUs / 1000UL);

  // Deferred NVS write for the brightness slider (see handleBrightness). The
  // actual write touches flash and can stall, so it is handed to the audio task
  // (core 0): nothing on the render core may ever block on flash.
  if (brSaveAt != 0 && (long)(millis() - brSaveAt) >= 0) {
    brSaveAt = 0;
    saveSettingsPending = true;
  }

  // --- one-off boot signal: SLOW-blink the previous reset reason on IO48 ---
  // setup() already did the familiar 3 fast power-up flashes. After a 1.5 s
  // pause this slow code (400 ms on/off) reports why the PREVIOUS run ended, so
  // the cause is visible with no serial port and no web page - just count them:
  //   1=POWER_ON  2=EXT_PIN  3=SOFTWARE  4=PANIC  5=INT_WDT
  //   6=TASK_WDT  7=WDT  9=BROWNOUT  11=USB  12=JTAG
  static int  bcLeft = -1;
  static bool bcOn   = false;
  static unsigned long bcNext = 0;
  if (bcLeft < 0) { bcLeft = g_prevReason; bcNext = millis() + 1500; }
  if (bcLeft > 0) {
    if (statusBlinks > 0) {
      bcLeft = 0;                                  // a command wants the LED: abort
      digitalWrite(POWER_LED_PIN, LOW);
    } else if ((long)(millis() - bcNext) >= 0) {
      bcOn = !bcOn;
      digitalWrite(POWER_LED_PIN, bcOn ? HIGH : LOW);
      if (!bcOn) { bcLeft--; if (bcLeft == 0) digitalWrite(POWER_LED_PIN, LOW); }
      bcNext = millis() + 400;
    }
  }

  // --- status blinks on IO48 (serial-free diagnostics) ---
  dbgStage = 3;
  if (statusBlinks > 0 && millis() >= statusNext) {
    statusNext = millis() + 150;
    statusOn = !statusOn;
    digitalWrite(POWER_LED_PIN, statusOn ? HIGH : LOW);
    statusBlinks--;
    if (statusBlinks == 0) digitalWrite(POWER_LED_PIN, LOW);
  }

  // --- diagnostics ---
  dbgStage = 4;
  // CRITICAL: never print unless a USB-CDC host is actually draining the port.
  // An unread Serial.printf() fills the CDC FIFO and BLOCKS the whole loop,
  // which froze the display ~30 s (6 heartbeats) after the UI was idle.
  static unsigned long lastHeartbeat = 0;
  if (millis() - lastHeartbeat >= 10000 && Serial.availableForWrite() > 200) {
    lastHeartbeat = millis();
    int mask = 0;
    for (int i = 0; i < 10; i++) if (digitalRead(channelPins[i])) mask |= (1 << i);
    Serial.printf("[hb] up=%lus display=%d preview=%d dotPos=%d ledPos0=%d chanMask=%03X shows=%lu heap=%u\n",
                  millis() / 1000, displayActive ? 1 : 0, previewMode, dotPos240,
                  ledPosArr[0], mask, ledShowCount, (unsigned)ESP.getFreeHeap());
  }

  // --- 30 s heartbeat into /log.txt: makes the heap drain rate directly visible
  // (compare "heap" between consecutive lines) without typing anything out.
  static unsigned long lastLogHb = 0;
  if (millis() - lastLogHb >= 30000) {
    lastLogHb = millis();
    logAppend("hb up=%us heap=%u min=%u shows=%lu display=%d out=%s fs=%.0f bass=%.2f mid=%.2f treb=%.2f lvl=%.2f",
              (unsigned)(millis() / 1000), (unsigned)ESP.getFreeHeap(),
              (unsigned)ESP.getMinFreeHeap(), (unsigned long)ledShowCount,
              displayActive ? 1 : 0,
              "fastled",
              (double)audioFs, (double)audioBass, (double)audioMid,
              (double)audioTreble, (double)audioLevel);

    // ONE compact instrument. The others did their job and were removed; this is
    // the set that actually proved decisive, so it stays.
    //
    //   fs   - measured audio sample rate. A 12 kHz microphone needs >= 24 kHz, and
    //          the old 300 us burst pacing was holding this at ~10.4 kHz.
    //   win  - audio window length in ms, i.e. how often the FFT runs and how much
    //          CPU the audio task costs per second.
    //   copy - ledShowAll's pixel copy. THE stall indicator: when a flash erase was
    //          freezing the CPU this pure CPU loop jumped from ~85 us to 27,000 us.
    //          Low hundreds of us == the display is not being interfered with.
    //   tx   - the ten-strip parallel transmit. ~4.7 ms is the hardware floor.
    //   skip - frames dropped because the DMA was still busy (frame-rate ceiling).
    logAppend("audio fs=%.0fHz win=%.0fms adc=%s | copy avg=%ldus max=%ldus | tx avg=%ldus max=%ldus | skip=%lu",
              (double)audioFs,
              (double)(1000.0f * (float)FFT_SAMPLES / (audioFs > 1.0f ? audioFs : 1.0f)),
              audioDmaOk ? "dma" : (audioAdcFast ? "fast" : "core"),
              (long)(g_showUsN ? g_copyUsSum / (int32_t)g_showUsN : 0), (long)g_copyUsMax,
              (long)(g_showUsN ? g_txUsSum / (int32_t)g_showUsN : 0), (long)g_txUsMax,
              (unsigned long)g_showSkip);
    g_copyUsSum = g_copyUsMax = g_txUsSum = g_txUsMax = 0;
    g_showUsN = 0;
    g_showSkip = 0;
    g_stepCount = 0;
    logAppend("audio p2p=%d thr=%.0f (avg=%.0f) | tempo=%lums hist=%d | raw mid=%.0f",
              envP2P, (double)p2pThr, (double)p2pAvg, establishedInterval, historyCount,
              (double)rawMid);
    // The bass-band beat detector's own numbers, so bassOverThr can be set from
    // measurements rather than guessed. A kick should show a large over= figure;
    // if over= stays near the threshold, lower it.
    logAppend("onset nov=%.3f thr=%.3f | rect=%d | over=%.1fdB rise=%.1fdB | level=%.1fdB floor=%.1fdB band=%.0f-%.0fHz",
              (double)odfNovelty, (double)odfThr, envP2P,
              (double)bassOverDb, (double)bassRiseDb,
              (double)bassDbLevel, (double)bandFloorBass, (double)bassBandLo, (double)bassBandHi);
    logAppend("clock period=%lums next-in=%ldms hits=%lu inserted=%lu offset=%lums | AC tempo=%lums score=%.0f",
              clockPeriod,
              (long)((long)clockNextBeat - (long)millis()),
              clockHits, clockMisses, beatOffsetMs, acTempoMs, (double)acScore);
    clockHits = 0; clockMisses = 0;
    // 4/4 bar telemetry: the four slot averages show which slot is being learnt
    // as "the 1"; "fired" counts direction changes since the previous line, so
    // roughly one per bar means the downbeat lock is working.
    logAppend("bar pos=%d down=%d avg=%.0f/%.0f/%.0f/%.0f infill=%d fired=%lu",
              barPos, downbeatPos,
              (double)(barEnergyCnt[0] ? barEnergySum[0] / barEnergyCnt[0] : 0.0f),
              (double)(barEnergyCnt[1] ? barEnergySum[1] / barEnergyCnt[1] : 0.0f),
              (double)(barEnergyCnt[2] ? barEnergySum[2] / barEnergyCnt[2] : 0.0f),
              (double)(barEnergyCnt[3] ? barEnergySum[3] / barEnergyCnt[3] : 0.0f),
              infillMode ? 1 : 0, firedCount);
    firedCount = 0;

    logAppend("fires %lu %lu %lu %lu %lu %lu %lu %lu",
              fireInt[0], fireInt[1], fireInt[2], fireInt[3],
              fireInt[4], fireInt[5], fireInt[6], fireInt[7]);

  }

  // --- crash breadcrumb for /diag (RTC memory survives a reset; ~1 Hz) ---
  static unsigned long dbgNext = 0;
  if ((long)(millis() - dbgNext) >= 0) {
    dbgNext  = millis() + 1000;
    dbgUpSec = millis() / 1000;
    dbgHeap  = (uint32_t)ESP.getFreeHeap();
    dbgLoops++;
  }
}
