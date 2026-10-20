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


// (ledBrightnessScale was removed: it was assigned here and never read - the
// brightness actually applied is the one FastLED.setBrightness() receives below.)

// NOTE: ledShowAll()/ledClearAll() are defined further DOWN, after the pattern
// type declarations. The Arduino builder inserts its automatic prototypes just
// before the FIRST function definition in the sketch, so defining a function up
// NOTE: the reasoning behind the longer comments in this sketch lives in CODE_NOTES.md -
// the audio paths, the Chase's two modes, the traps that have cost real time, and what is
// deliberately deferred. Flash is at ~92%, so explanations that need a paragraph go there
// and the code keeps the one-line "why" with a pointer.
//
// here would push every prototype above "enum LedPattern" and break the build.

const int channelPins[10] = {11, 12, 13, 14, 21, 38, 39, 40, 41, 42};

Preferences preferences;
WebServer server(80);

// ---- Global runtime settings ----
bool audioEnabled = true;
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
int   freqLED  = 50;    int multLED     = 1;   // 1 = one reversal per beat (DEFAULT)
float attackLED = 0.15; float decayLED = 0.01;
bool  wrapLED = false;  bool invertLED = false;
// LED VU meter DIRECTION. There is ONE "VU Meter" pattern now: this picks which of
// the four existing render branches (left, right, centre-out, both-ends) it uses.
// The row lives in the Special block.
String vuDirLED = "LEFT";    // LEFT | RIGHT | CENTRE | ENDS
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
int multiplier = 1;   // Beat Multiplier: 1 = one reversal per beat, 2 = two
// musicFullMode is the 240V VU METER's level source (BASS / FULL RANGE). It is
// set ONLY from the 240V section: the LED meter is bass-only by design and its
// source control has been removed, so an LED tweak must not change this.
volatile bool musicFullMode = false;   // "FULL RANGE" selected (audio task reads this)

// ---- 240V relay engine state ----
unsigned long lastStepTime = 0;
unsigned long lastBeatTime = 0;
unsigned long powerLedOffAt = 0;   // IO48 flash-off timestamp (0 = already off)
int dotPos240 = 0;
int dotDirection = 1;
// A beat must be LATCHED: the 240V display steps at its own rate, so a beat
// delivered on a single loop pass was usually gone before the next step ran -
// which is why the direction so rarely changed. It clears its own latch when it
// acts on it.
//
// The STRIPS no longer need a latch: their reversal happens in loop() the instant
// the onset is detected (doBeat), so beatLatchLED was set, never read, and is gone.
volatile bool beatLatch240 = false;

// ---- WS2811 LED engine state ----
// (The per-port arrays live further down, next to PortCfg. ledStep, ledDirection
// and ledExplosionStep were superseded by them, were never read, and are gone.)

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
// BASS LEVEL: the band from the Frequency setting (61 Hz) up to here. A kick drum's body lives
// between roughly 60 and 250 Hz; 500 Hz catches that with headroom. A CONSTANT, so it can be
// retuned without touching any pattern.
#define FFT_BASSLEVEL_MAX 500.0f
float fftReal[FFT_SAMPLES];
float fftImag[FFT_SAMPLES];

volatile float audioBass   = 0.0f;   // 0..1 band levels, read by the render loop
volatile float audioMid    = 0.0f;
volatile float audioTreble = 0.0f;
volatile float audioLevel  = 0.0f;   // whole-spectrum level
// BASS LEVEL - the band a meter or a kick SHOULD be reading: from the Frequency setting
// (61 Hz) up to FFT_BASSLEVEL_MAX. The FFT's "bass" band above is 17-61 Hz, BELOW what the
// MAX9814 can hear, which is why the VU meters folded in the mid band as a workaround. The
// Chase's football and the VU meters now read this instead (user, 19 Oct).
volatile float audioBassLvl = 0.0f;

// ---- Graphic EQ: ten band levels --------------------------------------------
// Filled by audioAnalyse() from the SAME FFT the VU meters use, and normalised the
// same way (dB above a tracked floor, 24 dB range), so a quiet room gives nothing
// and music gives movement without a second calibration.
#define EQ_BANDS 10
#define EQ_MAX   10                     // bar scale, the same 0..10 as the VU meters
volatile float audioEqBand[EQ_BANDS];
float          eqFloor[EQ_BANDS];       // per-band noise floor trackers
float          eqSmooth[EQ_BANDS];      // ballistics state (advanced once per frame)
int            eqBar[EQ_BANDS];         // what the renderer draws, 0..EQ_MAX
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
// The EXTRA (half-way) beats are recorded SEPARATELY from the real ones, so the
// multiplier's doubling can be measured in the log instead of judged by eye.
unsigned long fireIntX[8] = {0, 0, 0, 0, 0, 0, 0, 0};
int           fireIntIdxX = 0;

// Beat tagging counters, reset on every 30 s heartbeat. The audio layer marks BOTH
// the real beat and the extra half-way beat; each display then chooses which to act
// on, so these two counts plus the per-display flip counts show what actually
// happened rather than what was intended.
volatile uint32_t g_beatReal = 0, g_beatExtra = 0;   // beats delivered, by tag
volatile uint32_t g_onsetReal = 0;   // DISTINCT onsets acted on (one per render pass)
volatile uint32_t g_flipLED  = 0, g_flip240   = 0;   // reversals applied, by display
volatile uint32_t g_kickLED  = 0;   // CHASE kicks applied (Wrap OFF, level-driven)

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
unsigned long beatOffsetMs = 20;
// (fireAtMs/firePending are gone: the pending fire now lives in each BeatListener,
// because a beat is per PATTERN - see listeningStep().)

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
static float bandFloorBassLvl = 0;   // tracked floor for the 61-500 Hz BASS LEVEL band

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
// Theme colours are stored PER THEME: three themes x eight colours.
//
// There were FOUR: "Animations" existed to hold the EFFECT colours, and the effects are
// now defined per pattern (the Chase's tail, and whatever each other pattern gets) rather
// than by a theme of their own - so it is gone, along with its stored colours.
#define THEME_COUNT 3
CRGB previewColors[8];                 // the CURRENT theme, used by the renderer
CRGB themeStore[THEME_COUNT][8];       // all three themes, persisted
const char* THEME_NAMES[THEME_COUNT] = { "Disco", "Christmas", "Halloween" };


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
// (beatFlag is gone. The clock's beat value no longer reaches the display: the
// pattern engine is driven by doBeat, which comes from the real onset, plus
// antifreeze when Audio Detect is off.)
volatile bool previewDirty = false;  // a new preview frame is ready to draw
volatile bool blankReq     = false;  // request: blank all outputs
volatile bool flashReq     = false;  // request: do the "saved" confirmation blink
volatile int  calPort      = 0;      // RGB calibration test: port
volatile int  calOrder     = 2;      // RGB calibration test: colour order
// SAVE CONFIRMATION FLASH: off -> on -> off.
//
// Requested several times and never built. The test pixels flash to acknowledge a
// successful save, so there is no doubt the button did something.
#define CAL_FLASH_OFF1  180UL   // dark
#define CAL_FLASH_ON    540UL   // lit
#define CAL_FLASH_END   900UL   // dark again
unsigned long calFlashMs = 0;   // 0 = no flash running

// ---------------------------------------------------------------------------
// Per-strip pattern engine + Timeline + master brightness.
// Declarations live here so every handler can see them; implementations are
// further down with the LED engine.
// ---------------------------------------------------------------------------
// ===========================================================================
// PER-PATTERN PARAMETERS - ONE RECORD PER LED PATTERN
//
// THE RULE (user, 17 Oct 2026): EVERY pattern is unique. Nothing - no value, no colour, no
// band, no threshold - is shared between patterns. Whatever is changed in Moving Dot must
// have NO effect on Chase or any other pattern. (The 240V patterns are a separate table,
// still to be defined; the two sections are independent of each other.)
//
// These records are the STORAGE. The speedLED/colourLED/... globals further up are kept as a
// MIRROR of the SELECTED pattern's record, so every existing reader keeps working unchanged.
// ===========================================================================
#define LED_PAT_COUNT 6
#define LED_PAT_VER   1        // bump when a field is added: an older blob is then ignored

struct LedPatParams {
  uint16_t ver;
  uint32_t colour[4];          // references: T0..T7, RB, or #rrggbb
  float    speed;              // seconds per step - EXCEPT the Chase, which is LEDs/second
  float    beatSens;           // this pattern's own detection threshold
  float    attack, decay;      // its own VU/EQ ballistics
  float    peakDelay, peakSpeed;
  float    momentum;           // effect: weight of the shed debris (0 = off)
  int16_t  qty, size, mult, freqHz, peakDir;
  uint8_t  wrap, invert, vuDir, effect, distance;
  // ORIENTATION (user, 19 Oct): run the pattern from the OTHER END of the strip. APPENDED at the
  // very end on purpose, and forced to 0 after a load unless it is 0/1, so a record written before
  // this existed (whose bytes stop short of it) can never come back as a random direction.
  uint8_t  orient;
};

const char* const LED_PAT_IDS[LED_PAT_COUNT] = {
  "moving_dot", "chase_1_10", "center_out_chase", "knight_rider", "vu_meter", "eq"
};
LedPatParams ledPats[LED_PAT_COUNT];

enum LedPattern : int {
  LP_MOVING_DOT = 0, LP_CHASE_1_10, LP_CHASE_10_1, LP_CENTER_OUT,
  LP_KNIGHT_RIDER, LP_VU_METER, LP_VU_REV, LP_VU_DUAL, LP_VU_CENTER,
  LP_EQ
};
// ============================================================================================
// THE THREE AUDIO SOURCES, AND WHICH PATTERN USES WHICH  -  SET IN STONE (user, 19 Oct)
//
// There are ALWAYS THREE measurements of the music, produced once and shared by every pattern
// that wants them. A pattern SELECTS one of the three, IN CODE, via the table below. The bands of
// the two bass sources may be retuned, but the three themselves never change, and nothing may be
// added to this list without this table being updated in the same commit.
//
//   AUD_BASS_BEAT    the onset detector - WHEN a beat lands.
//                    Band: Frequency row .. 250 Hz (ODF_LP_HZ).
//   AUD_BASS_LEVEL   the FFT band level - HOW LOUD the bass is.
//                    Band: Frequency row .. 500 Hz (FFT_BASSLEVEL_MAX).
//   AUD_FULL         the whole-spectrum level - every frequency.
//
//   Moving Dot       -> AUD_BASS_BEAT    a beat reverses the dot, and fires the shed/erode effect
//   Knight Rider     -> AUD_BASS_BEAT
//   Explosion        -> AUD_BASS_BEAT
//   Chase, Wrap ON   -> AUD_BASS_BEAT    the metronome is TIMED by beats
//   Chase, Wrap OFF  -> AUD_BASS_LEVEL   the football: the LEVEL kicks it. The Bass Beat onset is
//                                        used as its TRIGGER and the level's own rise as a
//                                        fallback, but the DISTANCE and the strength come from
//                                        the level.
//   VU meters        -> AUD_BASS_LEVEL
//   EQ               -> AUD_FULL         (not built yet)
// ============================================================================================
enum AudioSource : int { AUD_BASS_BEAT = 0, AUD_BASS_LEVEL, AUD_FULL };

// THE TABLE. This is the ONE place a pattern's audio source is decided - no pattern decides for
// itself anywhere else.
static inline AudioSource audioSourceForPattern(int pat, bool wrap) {
  switch ((LedPattern)pat) {
    case LP_CHASE_1_10: return wrap ? AUD_BASS_BEAT : AUD_BASS_LEVEL;
    case LP_VU_METER:
    case LP_VU_REV:
    case LP_VU_DUAL:
    case LP_VU_CENTER:  return AUD_BASS_LEVEL;
    case LP_EQ:         return AUD_FULL;
    default:            return AUD_BASS_BEAT;   // Moving Dot, Knight Rider, Explosion
  }
}

// Does this pattern consume ONSETS? True for a pattern whose source IS the Bass Beat, and also
// for the Chase with Wrap OFF, which takes onsets as its kick TRIGGER (its source is the level).
static inline bool patternListensToBeats(int pat, bool wrap) {
  return (audioSourceForPattern(pat, wrap) == AUD_BASS_BEAT) || isChasePat(pat);
}



// The four VU render branches are now ONE pattern: "VU Meter", with the direction
// row in the Special block choosing which branch is used. LP_EQ therefore sits
// AFTER them, so "is this a VU pattern?" must use this range test and never the
// old `>= LP_VU_METER` (which would swallow the EQ).
static inline bool isVuPat(int p) { return p >= (int)LP_VU_METER && p <= (int)LP_VU_CENTER; }
static inline bool isLevelPat(int p) { return isVuPat(p) || p == (int)LP_EQ; }

// --- per-pattern records: the functions (the TYPES are declared above the enum, because the
// Arduino builder inserts every prototype before the sketch's FIRST function definition) ---

// Where a pattern that has never been set starts. The Chase's Speed is LEDs per second and
// the dot family's is seconds per step, so the default differs per pattern - which is only
// possible because every pattern now owns its own value.
LedPatParams ledPatDefaultFor(int i) {
  LedPatParams d = {};
  d.ver = LED_PAT_VER;
  d.colour[0] = 0xFFFFFF; d.colour[1] = 0xFFD000; d.colour[2] = 0xFF7000; d.colour[3] = 0xFF0000;
  d.speed = 0.02f; d.qty = 1; d.size = 1; d.mult = 1; d.freqHz = 50;
  d.wrap = 0; d.invert = 0; d.beatSens = 0.2f; d.attack = 0.15f; d.decay = 0.01f;
  d.peakDelay = 0.5f; d.peakSpeed = 1.0f; d.peakDir = 0; d.vuDir = 0;
  d.effect = 0; d.momentum = 0.0f; d.distance = 20;
  if (i == 1) d.speed = 20.0f;              // the Chase: LEDs per second
  return d;
}

// Which record a pattern ID belongs to. IDs no longer listed resolve to the pattern they
// became (chase_10_1 and the old VU branches), so saved data still plays.
int ledPatIndex(const String& id) {
  String s = id; s.trim(); s.toLowerCase();
  if (s == "chase_10_1") return 1;
  if (s == "vu_meter_rev" || s == "vu_dual" || s == "vu_center_out") return 4;
  for (int i = 0; i < LED_PAT_COUNT; i++) if (s == LED_PAT_IDS[i]) return i;
  return 0;                                 // unknown -> Moving Dot, exactly as before
}
LedPatParams& ledPat(int i) { return ledPats[(i < 0 || i >= LED_PAT_COUNT) ? 0 : i]; }
LedPatParams& ledSel()      { return ledPat(ledPatIndex(activePatternLED)); }

// The selected pattern's record -> the globals every existing reader uses.
void mirrorLedPat() {
  LedPatParams& r = ledSel();
  speedLED = r.speed; quantityLED = r.qty;  sizeLED = r.size;
  wrapLED = r.wrap;   invertLED = r.invert; multLED = r.mult;
  freqLED = r.freqHz; attackLED = r.attack; decayLED = r.decay;
  beatSens = r.beatSens; freqThreshold = r.freqHz;
  for (int i = 0; i < 4; i++) colourLED[i] = r.colour[i];
  vuDirLED = (r.vuDir == 1) ? "RIGHT" : (r.vuDir == 2) ? "CENTRE" : (r.vuDir == 3) ? "ENDS" : "LEFT";
}

// The record as it was before this build: the single LED settings become the SELECTED
// pattern's own values. Every other pattern starts from its defaults - nothing else could
// legitimately have values, because until now nothing was stored per pattern.
LedPatParams ledFromGlobals() {
  LedPatParams r = ledPatDefaultFor(ledPatIndex(activePatternLED));
  r.speed = speedLED; r.qty = quantityLED; r.size = sizeLED;
  r.wrap = wrapLED;   r.invert = invertLED; r.mult = multLED;
  r.freqHz = freqLED; r.beatSens = beatSens; r.attack = attackLED; r.decay = decayLED;
  for (int i = 0; i < 4; i++) r.colour[i] = colourLED[i];
  r.vuDir = (vuDirLED == "RIGHT") ? 1 : (vuDirLED == "CENTRE") ? 2 : (vuDirLED == "ENDS") ? 3 : 0;
  r.ver = LED_PAT_VER;
  return r;
}

// Read every pattern's own record. A missing or older-layout record is replaced by THAT
// pattern's defaults, except the selected one, which is seeded from the single LED settings
// this build replaces - so nothing already set is silently lost. Returns true when that
// migration happened, and the boot line reports it.
bool loadLedPatRecords() {
  bool migrated = false;
  int sel = ledPatIndex(activePatternLED);
  Preferences p; p.begin("lightshow", true);
  for (int i = 0; i < LED_PAT_COUNT; i++) {
    // ZEROED FIRST, and the size test is `<=`: a record saved BEFORE a new field was appended is
    // shorter than the struct is now, and reading it into a zeroed struct leaves the new field at
    // 0 - so appending ORIENTATION does not throw away every saved pattern. A blob of the wrong
    // version is still rejected.
    LedPatParams tmp{};
    size_t n = p.getBytes((String("lp") + String(i)).c_str(), &tmp, sizeof(tmp));
    if (n > 0 && n <= sizeof(tmp) && tmp.ver == LED_PAT_VER) { ledPats[i] = tmp; }
    else {
      ledPats[i] = (i == sel) ? ledFromGlobals() : ledPatDefaultFor(i);
      if (i == sel) migrated = true;
    }
    ledPatNormalise(i);
  }
  p.end();
  return migrated;
}

void saveLedPatRecords() {
  Preferences p; p.begin("lightshow", false);
  for (int i = 0; i < LED_PAT_COUNT; i++) {
    ledPats[i].ver = LED_PAT_VER;
    p.putBytes((String("lp") + String(i)).c_str(), &ledPats[i], sizeof(LedPatParams));
  }
  p.end();
}

// MOVING DOT HAS NO WRAP (user, 17 Oct): the row is greyed out on the page, and the value is
// forced OFF here as well - so a Wrap ticked before per-pattern records existed (or set by an
// old timeline cell) can never make the dot bounce at the ends of the strip.
void ledPatNormalise(int i) {
  if (i == 0) { ledPats[0].wrap = 0; ledPats[0].orient = 0; }  // Moving Dot: no wrap, no orientation
  if (ledPats[i].orient > 1) ledPats[i].orient = 0;            // a pre-append record's padding bytes
  if (ledPats[i].wrap) ledPats[i].orient = 0;                  // the control is greyed when Wrap is ON
}

struct PortCfg {
  int   pat    = LP_MOVING_DOT;
  float speed  = 0.2f;
  int   qty    = 1;
  int   size   = 1;
  bool  wrap   = false;
  bool  invert = false;
  // ORIENTATION: run this port's pattern from the OTHER END of the strip. Live for the Chase with
  // Wrap OFF; greyed for Moving Dot and for the Chase with Wrap ON (user, 19 Oct).
  bool  orient = false;
  // THE EFFECT, PER PORT. A timeline cell can give one strip its own Effect / Momentum /
  // Distance while the next strip keeps the pattern's own values: the effect is part of the
  // strip's configuration, exactly like speed and size.
  int   effect   = 0;
  float momentum = 0.0f;
  int   distance = 20;
  // LISTENING - this pattern's own view of the audio, on the same scale and with the
  // same formula as the pattern page, so a pattern left alone behaves exactly as the
  // 15/10 baseline did. A timeline cell overrides them for its port, which is what
  // lets two ports listen to different bands and different thresholds at once.
  float beatSens = 0.2f;     // its own sensitivity: thr = (1 - beatSens) * 60
  int   freqHz   = 50;       // its own band lower edge (20-100 Hz)
  int   mult     = 1;        // 1 = real beats only, 2 = real + half-way beats
  // Per-port colour, 0xRRGGBB.
  //   colour[0]          - the colour used by every non-VU pattern (dot, chase,
  //                        centre-out, knight rider) and by Invert.
  //   colour[0]..[3]     - the four bands of the VU meters, low level first.
  // Defaults preserve the previous look exactly: white for the dot patterns,
  // and the classic green -> yellow -> orange -> red VU gradient.
  uint32_t colour[4] = { 0xFFFFFF, 0xFFD000, 0xFF7000, 0xFF0000 };
  // The REFERENCE each slot was selected as. colour[] above is the RESOLVED
  // value the renderer draws; colourRef[] is what was STORED, and what a saved
  // playlist item keeps. See the model below.
  uint32_t colourRef[4] = { 0xFFFFFF, 0xFFD000, 0xFF7000, 0xFF0000 };
  // DARK: this port is blanked for the slot being played, because its timeline cell is
  // EMPTY (or holds the END OF DISPLAY marker). Set only while the timeline owns the port.
  bool  dark = false;
};

PortCfg portCfg[NUM_PORTS];

// ---------------------------------------------------------------------------
// COLOUR REFERENCE MODEL
//
// A colour slot stores a REFERENCE, not a resolved value. That is what makes a
// theme change flow through to everything that stored a colour: a playlist item
// keeps the reference, so re-theming repaints it instead of leaving a resolved
// value frozen behind.
//
//   0xFE000000 | n  (n = 0..7)  theme palette index   "T0".."T7"   LIVE (updates)
//   0xFEFFFFFF                  rainbow               "RB"        fixed
//   0x00RRGGBB                  custom colour        "#RRGGBB"    fixed
//
// 0xFE is a TAG byte, not part of a colour: an RGB value only ever occupies the
// low three bytes. Every value stored BEFORE this model existed therefore has a
// top byte of 0x00, which IS a valid custom colour - so the existing NVS keys
// (colLED0..3) and every stored uint32 carry over with no conversion at all,
// and "nothing stored" (0) still behaves exactly as it did.
//
// A theme reference is resolved AT DRAW TIME against the live palette, never
// cached into a stored slot, so a theme edited in the Colours page is picked up
// on the next frame. A rainbow cannot be resolved to one value at all: it is
// computed per LED from that LED's position along the strip, so adjacent
// rainbow zones join into ONE continuous rainbow instead of each restarting.
// ---------------------------------------------------------------------------
static const uint32_t REF_TAG            = 0xFE000000UL;
static const uint32_t COLOUR_REF_RAINBOW = 0xFEFFFFFFUL;

static inline bool     refIsTheme(uint32_t v)    { return (v & 0xFF000000UL) == REF_TAG && (v & 0x00FFFFFFUL) < 8UL; }
static inline bool     refIsRainbow(uint32_t v)  { return v == COLOUR_REF_RAINBOW; }
static inline int      refThemeIndex(uint32_t v) { return (int)(v & 0x00000007UL); }
static inline uint32_t refMakeTheme(int i)       { return REF_TAG | (uint32_t)(i & 7); }
static inline uint32_t refMakeCustom(uint32_t rgb) { return rgb & 0x00FFFFFFUL; }

static int themeIndexOf(const String& name) {
  for (int i = 0; i < THEME_COUNT; i++)
    if (name.equalsIgnoreCase(THEME_NAMES[i])) return i;
  return 0;                                   // unknown -> Disco
}

// Copy a stored theme into previewColors, which is what the renderer and the
// colour resolver read.
static void themeLoadToPreview(int idx) {
  if (idx < 0 || idx >= THEME_COUNT) idx = 0;
  for (int i = 0; i < 8; i++) previewColors[i] = themeStore[idx][i];
}


// "#rrggbb" (or "rrggbb") -> 0xRRGGBB. Returns `dflt` if unparseable.
//
// NOTE: these deliberately sit BELOW the type declarations. The Arduino IDE
// inserts auto-generated function prototypes just before the sketch's FIRST
// function, so defining any function above LedPattern/PortCfg moves that block
// above them and the build fails with "<type> does not name a type".
//
// (parseHexColor() used to live here. It had no callers left once colours became
// references, so it has been removed.)

// --- the reference encoders/decoders ---------------------------------------
// Reference -> the string form the UI and a playlist item carry: "T0".."T7",
// "RB", "#rrggbb". Reported by /settings so a reload restores the SELECTION,
// not a resolved colour that a theme change could no longer move.
static void refToString(uint32_t v, char* out, size_t n) {
  if (refIsRainbow(v))    snprintf(out, n, "RB");
  else if (refIsTheme(v)) snprintf(out, n, "T%d", refThemeIndex(v));
  else                    snprintf(out, n, "#%06lx", (unsigned long)(v & 0x00FFFFFFUL));
}

// String -> reference. Accepts "T0".."T7", "RB" and "#rrggbb"/"rrggbb".
// ANYTHING ELSE returns `dflt`, so a malformed or missing field can never blank
// a slot - the same protection the old parseHexColor() gave.
static uint32_t parseColourRef(const char* s, uint32_t dflt) {
  if (!s) return dflt;
  while (*s == ' ') s++;
  if ((*s == 'T' || *s == 't') && s[1] >= '0' && s[1] <= '7' && s[2] == 0)
    return refMakeTheme(s[1] - '0');
  if ((*s == 'R' || *s == 'r') && (s[1] == 'B' || s[1] == 'b') && s[2] == 0)
    return COLOUR_REF_RAINBOW;
  const char* h = (*s == '#') ? s + 1 : s;
  for (int i = 0; i < 6; i++) {                 // exactly six hex digits
    char c = h[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
      return dflt;
  }
  if (h[6] != 0) return dflt;
  return refMakeCustom((uint32_t)strtoul(h, nullptr, 16));
}

// Reference -> the colour to draw for a slot that holds a single colour. A theme
// reference reads the LIVE palette, so a theme edit repaints without touching a
// single stored slot. A rainbow has no single value: the renderer asks
// refIsRainbow() and uses rainbowAt() per LED instead - this White is only ever
// a last-resort fallback.
static CRGB resolveColourRef(uint32_t ref) {
  if (refIsTheme(ref))   return previewColors[refThemeIndex(ref)];
  if (refIsRainbow(ref)) return CRGB(0xFFFFFF);
  return CRGB(refMakeCustom(ref));
}

// The rainbow for ONE LED, from its POSITION ALONG THE STRIP plus a DRIFTING PHASE. Position -
// not the zone it belongs to - is what makes the four VU body sections join into one continuous
// rainbow, and what makes a rainbow dot take its colour from where it currently is.
//
// THE PHASE MOVES (user, 17 Oct): a fixed position->hue map makes a rainbow completely static,
// which reads as a still picture instead of a background. One full colour cycle across the
// strip every ~8 s - slow enough to be a gentle wash behind the pattern, never a flicker.
static CRGB rainbowAt(int idx, int n) {
  if (n <= 0) n = NUM_LEDS_PER_STRIP;
  if (idx < 0) idx = 0;
  uint8_t phase = (uint8_t)(millis() / 31);          // 256 hue units per ~8 s
  uint8_t hue = (uint8_t)(((uint32_t)idx * 256UL) / (uint32_t)n + phase);
  return CRGB(CHSV(hue, 255, 255));
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

// ---- MOVING DOT EFFECT: SHED DEBRIS (per port; RAM only, a live effect is never stored) ----
// THE BEHAVIOUR (user, 17 Oct 2026): every time the dot REVERSES it sheds a little of itself.
//   1..Distance/5 one-pixel pieces (max 4), RANDOMISED, leave from the dot's position in the
//   direction it WAS travelling - an eruption from the point of reversal - then drift and dim
//   on their own. Momentum is the WEIGHT of those pieces and is TIED to brightness: heavy
//   slows fast AND dims fast, light keeps its speed and fades away. Debris dies at the
//   Distance extent - the number of pixels the effect may use.
//   EROSION: every TENTH reversal the dot loses one WHOLE pixel, all its dots together, so a
//   3-pixel dot is gone after 30 reversals; a dot that is gone STAYS dark until the pattern is
//   applied afresh (a timeline row, PLAY, or selecting the pattern again).
// Effects = 0 or Distance = 0 means NONE of this - the pattern behaves exactly as before.
#define MD_FRAGS 64                   // pieces per strip: the DEATH BLAST plus the ORDINARY
                                      // debris of EVERY dot at once (Quantity x Effects can be
                                      // dozens per reversal - see mdShed()).
#define MD_BLAST 24                   // pieces the death blast puts out in total, split across
                                      // the port's dots so each erupts from its own position.
// THE DOT COMES BACK 3 SECONDS AFTER IT EXPLODES (user, 19 Oct). It restarts ITSELF, so the
// timeline's "the same item in the next row just continues" rule is never broken by a dead
// strip and no row has to re-arm it. ONE NUMBER to change if 3 s feels wrong.
#define MD_RESTART_MS 3000
struct MdFrag {
  bool     alive;
  bool     blast;      // part of the death blast: ignores Distance, and decays on its own terms
  float    posf;       // CONTINUOUS position along the strip. A FLOAT deliberately: with a
                       // whole-pixel integer, a slow piece rounds its frame step to zero and
                       // never moves at all - it just fades where it spawned.
  int8_t   dir;        // the direction the DOT was travelling when this broke off
  float    speed;      // pixels per second, decaying with the weight
  float    bright;     // 1.0 -> 0, decaying with the same weight
  float    moved;      // pixels travelled so far: it dies at the Distance extent
};
static MdFrag   mdFrags[NUM_PORTS][MD_FRAGS];
static uint8_t  mdFlips[NUM_PORTS];   // reversals since the last whole pixel was lost
static uint8_t  mdLost[NUM_PORTS];    // whole pixels the dot has lost
static uint32_t mdRng[NUM_PORTS];     // per-port randomiser state
static unsigned long mdExpiredAt[NUM_PORTS];   // when this port's dot exploded (0 = alive)
static unsigned long mdLastMs = 0;    // for the per-frame advance
static float    mdLastDotPxS = 0;     // the dot's own speed at the last reversal (px/s)
static float    mdLastMomFrac = 0;    // the Momentum fraction used at the last reversal (0..1)

// The record a PORT's pattern belongs to. A port's pattern can differ from the page's
// selection when a timeline cell sets it, so this is never the selected record by assumption.
int ledPatIndexOfEnum(int pat) {
  switch (pat) {
    case (int)LP_MOVING_DOT:   return 0;
    case (int)LP_CHASE_1_10:
    case (int)LP_CHASE_10_1:   return 1;
    case (int)LP_CENTER_OUT:   return 2;
    case (int)LP_KNIGHT_RIDER: return 3;
    case (int)LP_EQ:           return 5;
    default:                   return 4;      // the four VU branches are ONE pattern
  }
}

static inline uint32_t mdRand(int p) {        // xorshift: rand() has no place in a frame path
  uint32_t x = mdRng[p] ? mdRng[p] : 0x9E3779B9u;
  x ^= x << 13; x ^= x >> 17; x ^= x << 5;
  mdRng[p] = x;
  return x;
}

// Is the effect live on this port? Moving Dot only, Effects > 0 and Distance > 0. The values
// are the PORT's own, because a timeline cell may have set them for this strip alone.
static inline bool mdEffectOn(int p) {
  if (portCfg[p].pat != LP_MOVING_DOT) return false;
  return (portCfg[p].effect > 0 && portCfg[p].distance > 0);
}

// The dot's length WITH the effect's erosion. It only ever SHORTENS, and only the dot's own
// pattern can do it: every other pattern, and Effects = 0, return Size exactly.
static inline int mdDotLen(int p) {
  int len = constrain(portCfg[p].size, 1, 10);
  if (!mdEffectOn(p)) return len;
  int left = len - (int)mdLost[p];
  return (left < 0) ? 0 : left;
}

// Everything back to full: a fresh dot and no debris. Called when the pattern is applied
// afresh - a timeline row, PLAY, or selecting the pattern again.
void mdResetAll() {
  for (int p = 0; p < NUM_PORTS; p++) {
    mdFlips[p] = 0; mdLost[p] = 0;
    mdExpiredAt[p] = 0;                   // a fresh cell cancels any pending restart
    mdRng[p] = 0x1234567u + (uint32_t)p * 2654435761u;
    for (int k = 0; k < MD_FRAGS; k++) mdFrags[p][k].alive = false;
  }
}

// THE DEATH BLAST. When the erosion takes the dot's LAST pixel the strip explodes from the
// position where it died: a spray of one-pixel pieces radiating BOTH ways that fade to nothing.
// It is the END of the display, not a repeatable effect, so it ignores the Distance extent (it
// spans the whole strip) and its decay ignores Momentum - a heavy setting must not be able to
// turn the ending into a fizzle. It clears the old debris as it goes: once the dot is gone
// there is nothing left moving, so the cloud dies with it.
static void mdExplode(int p, int at) {
  for (int k = 0; k < MD_FRAGS; k++) mdFrags[p][k].alive = false;
  // EVERY DOT ERUPTS FROM ITS OWN POSITION (user, 19 Oct: "the effects ... should happen on as
  // many dots as were selected"). MD_BLAST pieces are split across the port's dots, so the whole
  // strip is covered and a dot is never left out of its own ending.
  int effQty = constrain(portCfg[p].qty, 1, 8);
  int per = MD_BLAST / effQty;
  if (per < 4) per = 4;
  int slot = 0;
  for (int q = 0; q < effQty && slot < MD_FRAGS; q++) {
    int base = (at + (q * NUM_LEDS_PER_STRIP) / effQty) % NUM_LEDS_PER_STRIP;
    for (int k = 0; k < per && slot < MD_FRAGS; k++, slot++) {
      MdFrag& f = mdFrags[p][slot];
      f.alive  = true;
      f.blast  = true;
      f.posf   = (float)constrain(base, 0, NUM_LEDS_PER_STRIP - 1);
      f.dir    = (k & 1) ? -1 : 1;                  // outwards, both ways, from THIS dot
      f.speed  = 50.0f + (float)(mdRand(p) % 260);  // a spread, so they arrive at different
      f.bright = 1.0f;                              // times and reach the far ends
      f.moved  = 0.0f;
    }
  }
}

// A REVERSAL: erode first, then shed.
static void mdShed(int p) {
  if (!mdEffectOn(p)) return;
  // A DOT THAT NO LONGER EXISTS SHEDS NOTHING - and must not explode a second time either.
  if (constrain(portCfg[p].size, 1, 10) - (int)mdLost[p] <= 0) return;
  // EROSION: a WHOLE pixel every (10 x Effects) reversals (user, 17 Oct). With Effects 4 that
  // is 40 reversals per pixel, so a 3-pixel dot survives 120 of them.
  int flipsPerPixel = 10 * constrain(portCfg[p].effect, 1, 10);
  if (++mdFlips[p] >= flipsPerPixel) {
    mdFlips[p] = 0;
    if (mdLost[p] < 200) mdLost[p]++;
    if (constrain(portCfg[p].size, 1, 10) - (int)mdLost[p] <= 0) {
      // ONE LINE PER DEATH, so "did this strip die by itself or because of another one?" is
      // answered by the log instead of inferred. Each port counts its own reversals.
      logAppend("effect EXPIRED port=%d at=%d size=%d", p + 1, (int)ledPosArr[p],
                constrain(portCfg[p].size, 1, 10));
      mdExpiredAt[p] = millis();      // ...and the automatic restart is timed from HERE
      mdExplode(p, (int)ledPosArr[p]);
      return;
    }
  }
  // THE COUNT IS THE EFFECT SLIDER, EXACTLY: 1..10 pieces per direction change, and NOT
  // randomised. (It used to be Distance/5 capped at 4 and randomised.)
  int n = constrain(portCfg[p].effect, 1, 10);
  // MOMENTUM IS THE PIECES' SPEED AWAY FROM THE DOT (user, 17 Oct, second look): at MAXIMUM
  // they leave at the dot's OWN speed, at MINIMUM they "hit the wall" and stay exactly where
  // the dot turned. (The first build had this the wrong way round - heavy meant slow.)
  // MOMENTUM IS 0-0.5 HERE (Moving Dot). The Chase is the ONLY pattern with the short 0-0.1 range,
    // and the page applies that for the Chase alone (user, 20 Oct) - so this normalises by 0.5
    // exactly as it always did.
    float frac = constrain(portCfg[p].momentum, 0.0f, 0.5f) / 0.5f;  // 0 = wall, 1 = dot's speed
  float dotPxS = 1.0f / ((portCfg[p].speed > 0.0005f) ? portCfg[p].speed : 0.0005f);
  if (dotPxS > 600.0f) dotPxS = 600.0f;
  if (dotPxS < 1.0f)   dotPxS = 1.0f;
  float piecePxS = dotPxS * frac;
  mdLastDotPxS  = dotPxS;
  mdLastMomFrac = frac;
  // EVERY DOT SHEDS FROM ITS OWN POSITION (user, 19 Oct: "the effects only happen on 1 of the
  // dots, it should happen on as many dots as were selected"). The dots are drawn at EVEN
  // OFFSETS from ledPosArr - the same arithmetic as drawPortPattern() - so each one breaks up
  // exactly where it is, and each sheds the full Effects count of pieces.
  int effQty = constrain(portCfg[p].qty, 1, 8);
  for (int q = 0; q < effQty; q++) {
    int base = (ledPosArr[p] + (q * NUM_LEDS_PER_STRIP) / effQty) % NUM_LEDS_PER_STRIP;
    for (int k = 0; k < n; k++) {
      int slot = -1;
      for (int j = 0; j < MD_FRAGS; j++) if (!mdFrags[p][j].alive) { slot = j; break; }
      if (slot < 0) slot = (int)(mdRand(p) % MD_FRAGS);   // all busy: the oldest is recycled
      MdFrag& f = mdFrags[p][slot];
      f.alive  = true;
      f.blast  = false;                                  // ordinary debris, not the death blast
      f.posf   = (float)constrain(base + (int)(mdRand(p) % 3) - 1, 0, NUM_LEDS_PER_STRIP - 1);
      f.dir    = (ledDirArr[p] > 0) ? -1 : 1;            // the direction the dots WERE travelling
      f.speed  = piecePxS * (0.55f + (float)(mdRand(p) % 90) / 100.0f);   // they break up
      f.bright = 1.0f;
      f.moved  = 0.0f;
    }
  }
}

// ADVANCE EVERY FRAME. The debris is a cloud, not a dot: it must not move on the dot's step
// timer, or a fast dot would leave a stutter of separate pixels instead of a stream.
void mdAdvanceAll(unsigned long now) {
  if (mdLastMs == 0) { mdLastMs = now; return; }
  float dtMs = (float)(now - mdLastMs);
  mdLastMs = now;
  if (dtMs <= 0.0f) return;
  if (dtMs > 100.0f) dtMs = 100.0f;
  float dtS = dtMs / 1000.0f;

  for (int p = 0; p < NUM_PORTS; p++) {
    // THE DOT RESTARTS ITSELF (user, 19 Oct): 3 s after it exploded the pattern comes back at
    // full size, WHERE IT ALREADY IS and still travelling - so a dead strip never disturbs the
    // timeline (two rows holding the same item still just continue) and no row has to re-arm it.
    if (mdExpiredAt[p] != 0 && (unsigned long)(now - mdExpiredAt[p]) >= MD_RESTART_MS) {
      mdExpiredAt[p] = 0;
      mdFlips[p] = 0; mdLost[p] = 0;
      for (int k = 0; k < MD_FRAGS; k++) mdFrags[p][k].alive = false;
      logAppend("effect RESTART port=%d", p + 1);
    }
    // MOMENTUM IS THE PIECES' SPEED AWAY FROM THE DOT (user, 17 Oct, second look) and
    // brightness is tied to it the SAME way round: a fast piece lasts, a stopped one dims out.
    // The values are the PORT's own: a timeline cell may set them for this strip alone.
    // MOMENTUM IS 0-0.5 HERE (Moving Dot) - the Chase's short 0-0.1 range is the page's business.
    float frac = constrain(portCfg[p].momentum, 0.0f, 0.5f) / 0.5f;   // 0 = wall, 1 = dot speed
    // A piece still SLOWS as it goes, but the endpoints must read exactly as specified: at
    // maximum it streaks away keeping (nearly) the dot's speed, at minimum it never moved.
    float kSpeed  = expf(-dtMs / (600.0f + 2400.0f * frac));
    // BRIGHTNESS IS TIED TO MOMENTUM the same way round: one that "hit the wall" dims out at
    // once, one travelling at speed lingers.
    float kBright = expf(-dtMs / (90.0f + 900.0f * frac));
    int   extent  = constrain(portCfg[p].distance, 0, 100);
    // The DEATH BLAST decays on its OWN terms: it must always span the strip and always fade
    // out, whatever Momentum and Distance are set to.
    float kBlastS = expf(-dtMs / 1500.0f);
    float kBlastB = expf(-dtMs /  900.0f);
    for (int k = 0; k < MD_FRAGS; k++) {
      MdFrag& f = mdFrags[p][k];
      if (!f.alive) continue;
      // CONTINUOUS movement, in floats. Rounding to whole pixels per frame is what made the
      // debris look stationary: at ~57 px/s and a 5 ms frame the step is 0.28 px, which rounds
      // to ZERO every frame - so the piece faded exactly where it spawned.
      float adv = f.speed * dtS;
      f.posf  += (float)f.dir * adv;
      f.moved += adv;
      float kS  = f.blast ? kBlastS : kSpeed;
      float kB  = f.blast ? kBlastB : kBright;
      int   ext = f.blast ? (NUM_LEDS_PER_STRIP - 1) : extent;
      if (f.moved >= (float)ext || f.posf < 0.0f ||
          f.posf > (float)(NUM_LEDS_PER_STRIP - 1)) f.alive = false;
      f.speed  *= kS;
      f.bright *= kB;
      if (f.bright < 0.02f) f.alive = false;
    }
  }
}

// ---- THE CHASE KICK: Wrap OFF is a FOOTBALL driven by the BASS LEVEL (user, 19 Oct) --------
// CHASE ONLY. It is not a Moving Dot behaviour and it is NOT beat-triggered - the level drives
// it, which is what the user asked for first and then confirmed they prefer. Chase Wrap ON (the
// metronome) is untouched by any of this, and so are all the other patterns.
//
// The dot ROLLS at Speed so the display never stalls, and the BASS LEVEL kicks it along. Every
// kick has a CREDIT of extra pixels it may travel, and the four controls divide the work:
//
//   DISTANCE   the credit's MAXIMUM - the furthest one kick can carry the dot (1-50 px)
//   BASS LEVEL how much of that credit is on offer right now ("how much of the distance is
//              covered"), and how hard the kick is applied
//   EFFECT     the SPEED of the kicked movement: 1 = twice the rolling speed, 10 = KICK_CEIL
//   MOMENTUM   the DECELERATION: 0 = a dead stop the moment the credit is gone, 0.5 = a long
//              coast that eases back to the roll (and so travels FURTHER than Distance)
//
// The fast phase spends the credit and stops at DISTANCE; MOMENTUM's coast is EXTRA travel on top
// of it. That was not the original plan - the decay was going to shape the same pixels - but a
// decay confined inside a 10-pixel kick lasts 40 ms and cannot be seen, so it has to continue past
// the Distance for the deceleration to be visible at all (user, 19 Oct).
//
// The ceiling is the user's own definition of the fastest a dot may move without the display
// stepping: the Moving Dot display's minimum Speed of 0.002 s per step, i.e. 500 pixels/second.
static const float KICK_CEIL   = 500.0f;   // px/s - the Speed 0.002 equivalent
// (There was a KICK_REFILL trickle here. It is GONE: it kept the credit above zero, so the code
// never left the kick branch and momentum's coast was cancelled a frame after it began.)
// The smallest gap between the gate and the full-kick reference, so n can never divide by zero in
// a very quiet passage. The reference itself is the MUSIC'S OWN PEAK now - see stepChase.
static const float KICK_MIN_SPAN = 0.06f;

static float    chaseCredit[NUM_PORTS];    // extra pixels this kick may still travel
static float    chaseCoast[NUM_PORTS];     // excess speed (px/s) coasting back down to the roll
static float    chaseCoastTau[NUM_PORTS];  // the glide's decay time, derived from Momentum
// THE HYBRID TRIGGER, TIMED TO THE BEAT'S CENTRE (user, 19 Oct).
//
// The onset detector fires at the LEADING EDGE of the beat, but the Bass Level envelope takes
// ~80 ms to rise - so granting the credit then took the level from BEFORE the beat. That is why
// kicks were small and plenty of beats appeared to do nothing at all. A beat now arms a grant
// CHASE_BEAT_CENTRE_MS in the future, and the credit is taken from the level as it stands then:
// the peak of the beat, not its leading edge. The LENGTH of the beat has no bearing on the
// distance - only Distance and the level do.
//
// The level's own rise still triggers a kick as a FALLBACK, rate-limited so one beat cannot fire
// twice (once from the onset, once from the rise that follows it).
// THE ONSET HIT IS SHARED, like the fallback below it. A beat is a property of the MUSIC, so it is
// detected once and every Chase port takes it in the SAME frame. Per-port pending grants were the
// last per-port asymmetry in the chain: a grant pending on one port and not the other got wiped by
// a row change, so one port lost a kick the other kept - measured as identical parameters, a
// 12-pixel difference over 8367, and accumulators 0.72 against 0.83.
volatile unsigned long g_beatSeenAt    = 0;   // when listeningStep last saw a real onset
volatile unsigned long g_beatDoneAt    = 0;   // the onset already turned into a hit
volatile bool          g_chaseOnsetHit = false;
static const unsigned long CHASE_BEAT_CENTRE_MS = 100;   // onset -> centre of the beat
static const unsigned long CHASE_FALLBACK_GAP_MS = 200;  // min gap between fallback kicks
// Set by the per-port adoption loop when ANY port's motion changed, and applied ONCE after the
// loop so EVERY port restarts together and in phase. A per-port restart left the restarted port
// at 0 while the others carried on - a permanent gap between display 1 and display 2 (user,
// 19 Oct: "why is there a definite gap between displays on Port 1 and 2?"). With a timeline row
// loaded the cells differ per port, so SOME ports restarted and others did not.
static bool     g_restartMotion = false;
static float    chaseLvlLast[NUM_PORTS];   // the RAW envelope last frame, so a RISE can be spotted
static float    chaseNlast[NUM_PORTS];     // the gated, referenced level last frame (the log's n=)

// THE BASS LEVEL ENVELOPE, shared by every pattern that needs a LEVEL rather than a beat: one
// smoothed copy of the 61-500 Hz band. It is updated in the AUDIO path, not the render loop, so
// it is meaningful even when the display is idle - which is what makes the gate tunable from the
// log. The audio window is ~59 ms, so the attack cannot be faster than a frame: 80 ms attack,
// 250 ms release.
volatile float   g_bassLvlEnv = 0.0f;
volatile float   g_bassPeak   = 0.0f;   // peak of the ENVELOPE since the last /diag read
// A SLOW peak of the envelope: rises at once, falls over ~2.5 s. This is what the Chase's gate is
// derived from, so the gate is always a fraction of what the music is ACTUALLY doing instead of
// a number the user has to guess. A hand-set gate is unworkable: the microphone's absolute scale
// is nothing like 0-1, so any fixed number is either inside the room noise or above the music.
volatile float   g_bassSlow   = 0.0f;
// ---- FRAME HITCH INSTRUMENT ----------------------------------------------------------------
// The cardinal rule: the display must never be disturbed. A row change happens inside
// runTimelineSequencer(), a transmit inside runLEDPattern(), and a FLASH ERASE (which freezes
// BOTH cores) shows up in the whole-pass figure - so all three are timed and /diag reports the
// worst of each since the last read. The log cannot serve here: it stops flushing while a show
// runs, and Serial cannot be watched without resetting the board.
volatile uint32_t g_maxLoopUs = 0;   // worst complete pass
volatile uint32_t g_maxSeqUs  = 0;   // worst runTimelineSequencer() - ROW CHANGES live here
volatile uint32_t g_maxLedUs  = 0;   // worst runLEDPattern()
volatile uint32_t g_hitchCount = 0;  // passes over 15 ms (a frame budget is about 5 ms)
// PER-PORT STALENESS. A CPU hitch and a dot that simply STOPS look identical on the strips, and
// these separate them: g_maxLoopUs catches the former, this catches the latter - and it names the
// port. Worst case since the last read, in milliseconds.
volatile uint32_t g_staleMaxMs = 0;
// ---- WHY A KICK DID NOT LAND ----------------------------------------------------------------
// A Chase kick needs an armed onset grant AND a level that clears the gate. The counters agree
// with what the strips show (68 beats delivered, 4 kicks landed), so this counts the two halves
// separately: armed but blocked by the level, versus never armed at all.
volatile uint32_t g_kickArmed = 0;   // onsets that armed a grant
volatile uint32_t g_kickZero  = 0;   // grants consumed while the level gave n == 0
volatile uint32_t g_kickMiss  = 0;   // grants still pending when a row change wiped them
// ---- PORT SYNC SNAPSHOT ---------------------------------------------------------------------
// Two ports running the SAME playlist entry must stay in step. This exposes their raw motion state
// (position, direction, chase accumulator and step age) for the first four ports, so "not in sync"
// can be seen rather than guessed at.
volatile int32_t  g_syncPos[NUM_PORTS] = {0};
volatile int32_t  g_syncDir[NUM_PORTS] = {0};
volatile int32_t  g_syncAcc[NUM_PORTS] = {0};   // the chase's fractional pixel accumulator
volatile uint32_t g_syncAge[NUM_PORTS] = {0};   // ms since this port last STEPPED
// THE SHARED FALLBACK HIT. The level's rise is a property of the MUSIC, so the decision to fire a
// fallback kick is made ONCE per frame and taken by every Chase port in that frame. Deciding it per
// port (each with its own 200 ms gate) let one port take a hit the other was still gated out of -
// which is exactly "the chase on port 1 is a full dot length different to port 2".
volatile bool     g_chaseFallback = false;
static const float CHASE_FALLBACK_RISE = 0.04f;   // the SHARED rise needed for a fallback hit
// PAIR DIVERGENCE: how far port 0 and port 1 have ever been apart in PIXELS (worst since boot),
// with the two positions and the row it happened in. A whole dot length apart is a missed or extra
// kick, not a sub-pixel drift, and this is what distinguishes them.
volatile int32_t  g_pairDiff = 0, g_pairA = 0, g_pairB = 0, g_pairRow = -1;
// STEP COUNTS, per port. Two ports fed identical parameters MUST take the same number of steps; if
// they do not, the asymmetry is in the code somewhere between the cell and the step. If the counts
// match but the positions differ, the difference came in through a parameter - which is data, not a
// code fault. This is how "sync as a byproduct" gets verified instead of special-cased.
volatile uint32_t g_steps[NUM_PORTS] = {0};
// MOTION RESTARTS, per port (user, 20 Oct). Whenever the adoption loop decides a port's motion
// changed, EVERY port is re-pinned to its start position and its step timer is zeroed. That is by
// design - but a port re-pinned over and over cannot advance, and the symptom is a dot that looks
// STOPPED even though the code is running normally. This counter makes that visible: a number
// climbing while the dot stands still IS the fault. It is reported as the "fre=" field.
volatile uint32_t g_framesRestart[NUM_PORTS] = {0};
// WHEN EACH PORT'S POSITION LAST CHANGED (user, 20 Oct). The first /diag reading of the intermittent
// Moving Dot stall showed something that cannot both be true: the step counter had climbed to ~56,000
// (a step ~72 times a second) while the board's own staleness detector reported port 1's position
// unchanged for 9.6 seconds - the whole row. So the POSITION is now published too, along with how
// long it has been unchanged, its direction, and whether the strip is dark. Those four numbers say
// whether the dot is moving-but-not-advancing, advancing-but-not-drawn, or genuinely pinned.
volatile unsigned long g_lastMoveMs[NUM_PORTS] = {0};
// FRAMES VISIBLE, per port. If two ports have identical parameters, their steps-per-visible-frame
// must match; a difference in the raw step count can then only come from one port being DARK for
// part of the time - which is DATA (an empty cell), not a code asymmetry. st0/st1 alone cannot tell
// those apart; this pair can.
volatile uint32_t g_framesVis[NUM_PORTS] = {0};
volatile int32_t  g_stalePort  = -1;
volatile int32_t  g_staleDark  = -1;   // was that port DARK at the time? (-1 = nothing recorded yet)
volatile int32_t  g_staleRow   = -1;   // which timeline row it happened in (1-based, -1 = none)
// These four are deliberately NOT cleared by reading /diag: a stall during a row TRANSITION is over
// in a second and cannot be caught by a poll that has to be timed by hand. They persist until the
// board is reset, so one reading after a test always shows whether it happened and to which port.

static inline bool isChaseKick(int p) {
  return portCfg[p].pat == LP_CHASE_1_10 && !portCfg[p].wrap;
}

// EFFECT sets the SPEED of the kick, as an ABSOLUTE ramp: KICK_MIN_PX_S at 1, KICK_CEIL at 10.
// It used to be a multiple of the rolling speed ("1 = twice the current speed"), which made sense
// when the roll was 1-50 px/s. The roll can now be 0.1 px/s, and twice that is invisible - so the
// ramp is absolute and a kick is always a kick. Effect 0 means no kick at all, just the roll.
static const float KICK_MIN_PX_S = 40.0f;   // px/s at Effect 1
static float chaseKickSpeed(int p) {
  float roll = chaseSpeed(p);
  int   eff  = constrain(portCfg[p].effect, 0, 10);
  if (eff <= 0) return roll;
  float v = KICK_MIN_PX_S * powf(KICK_CEIL / KICK_MIN_PX_S, (float)(eff - 1) / 9.0f);
  if (!(v >= roll && v <= KICK_CEIL)) v = KICK_CEIL;      // NaN guard
  return v;
}

// MOMENTUM is now measured in DISTANCE, not time: it sets how far the glide after a kick runs -
// Distance x 6 x momentum, so 0 is no glide at all and the maximum is 3 x Distance. The glide
// starts at the kick speed, so the dot decelerates smoothly. (It was a 0-6 s settling time here;
// that made the glide's length depend on the kick speed and let it run for hundreds of pixels.)

// ---- CHASE STATE (per port) ---------------------------------------------------------
// The Chase is NOT on the step timer. With wrap OFF its movement follows the LEVEL of the
// audio, capped by Speed; with wrap ON it interpolates ONE traverse between beats, so the
// light lands at the far end just as the next beat arrives - the metronome.
//
// Nothing is drawn while a chase is still, so a silent strip does not re-transmit 500 times
// a second. The step-timer patterns cannot avoid that, because they always step.
// A beat that lands while the dot is still moving is DEFERRED, not dropped (user, 20 Oct): one
// pending move per port, started the moment the current one finishes. A FLAG deliberately, not a
// count - a run of beats must never stack into a longer move.
static bool          chasePending[NUM_PORTS]  = {false};
static float         chaseAcc[NUM_PORTS]      = {0};   // fractional LEDs (wrap OFF)
static unsigned long chaseTrigAt[NUM_PORTS]   = {0};   // when the last trigger landed
static unsigned long chaseInterval[NUM_PORTS] = {0};   // measured gap between triggers
static int           chaseFromPos[NUM_PORTS]  = {0};   // where this traverse started

// THE CHASE is one pattern now: "Chase". chase_10_1 and knight_rider are still rendered if
// an old playlist or timeline cell names them, but they are no longer listed and chase_10_1
// now RESOLVES to this one, so old data plays the new behaviour.
static inline bool isChasePat(int p) { return p == (int)LP_CHASE_1_10; }

// A beat sensitivity at its LOWEST is the user saying "do not let sound drive this
// pattern": the music has no effect at all and the timer takes over. The slider's floor is
// 0.01, so that is the test - for EVERY beat-driven pattern, not just the Chase.
static inline bool sensAtMin(float s) { return s <= 0.011f; }

static inline int chaseDotLen(int p) { return constrain(portCfg[p].size, 1, 10); }
// CHASE SPEED IS SECONDS PER PIXEL, 1-10 (user, 19 Oct: "1 is still too fast. I'd like to change
// this from 1 to 10S, where it takes 1 to 10 seconds to move 1 pixel"). This function returns
// PIXELS per second, which is what every caller works in: 1 s/px -> 1 px/s (fastest), 10 s/px ->
// 0.1 px/s (a slow roll). It now matches the dot patterns, whose Speed is seconds per step. An
// item saved under the OLD meaning (pixels per second, 1-50) simply clamps.
static inline float chaseSpeed(int p) {
  float s = constrain(portCfg[p].speed, 1.0f, 10.0f);
  if (!(s >= 1.0f)) s = 1.0f;              // NaN guard
  return 1.0f / s;
}
static inline int chaseMaxPos(int p) {
  int m = NUM_LEDS_PER_STRIP - chaseDotLen(p);
  return m < 0 ? 0 : m;
}

// May this listener act on a beat? Minimum sensitivity means no - the pattern then runs on
// its own timer instead (Moving Dot keeps stepping, it just stops being flipped).
static inline bool beatActionAllowed(int i) {
  return i < NUM_PORTS ? !sensAtMin(portCfg[i].beatSens) : !sensAtMin(beatSens);
}

#include <NeoPixelBus.h>

// --- LED OUTPUT -------------------------------------------------------------
// NeoPixelBus, LCD_CAM parallel method (ESP32-S3 only). Up to 16 channels are
// clocked out in a single DMA transfer, so there is no RMT and no grouping. This
// is the only complete parallel clockless driver available for this chip: FastLED
// 3.10.7 ships one but it is disabled in the library (all files .disabled and
// absent from lcd_cam/_build.cpp.hpp).
// NeoGrbFeature is used because leds[][] is ALREADY in each port's wire order.
// X16, NOT X8. The X8 type sets BusMaxCount = sizeof(uint8_t) * 8 = 8 mux channels,
// and RegisterNewMuxBus() returns InvalidMuxId once those are used. Each strip
// registers its own mux, so with TEN strips the 9th and 10th were never encoded
// into the DMA - the render ran, the pixels were set, the flags were right, and
// nothing was transmitted for ports 9/10. The X16 type uses uint16_t flags, giving
// 16 mux channels, which is what ten outputs actually need.
typedef NeoPixelBus<NeoGrbFeature, NeoEsp32LcdX16Ws2811Method> Ws2811Strip;
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
// g_showUsN is the frame counter: the heartbeat divides the copy/tx totals by it.
// (g_showUsSum and g_showUsMax were accumulated but never read - the heartbeat
// reports the copy/tx split instead - so they are gone.)
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

// (The g_loop*/g_client*/g_render* stage-timing counters were removed. This
// display is stall-free, so nobody read them: they were declared and never once
// written to or displayed.)

// Per-port "this strip's buffer changed since the last show". Copying all 1000
// pixels every frame was ~5 ms of the frame time even when nine strips were
// unchanged; only the strips that actually stepped need copying now, so the
// per-frame cost drops roughly tenfold and the frame rate (which sets the maximum
// dot speed) is no longer dragged down by redundant work.
volatile bool ledDirty[NUM_PORTS] = {false};

void ledShowAll() {
  // NO FRAME SKIPPING. CanShow()/skip was added when a ten-strip transmit took
  // 4,700 us and waiting for it produced 40 ms stalls. The X16 driver now moves the
  // same frame in ~1,330 us, so waiting is cheap and blocking GUARANTEES every
  // position is transmitted.
  //
  // Skipping a frame does not just drop a frame: the step has already advanced
  // (ledStepMs/advancePort run before the show), so the position is simply never
  // displayed - which is exactly the "dot skips a pixel instead of using them all"
  // seen at maximum speed.

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
  // TRANSMISSION LOG (rate-limited). Moved to AFTER the Show loop.
  //
  // It was placed BETWEEN t1 and the Show() calls, so it measured the time to reach
  // the log line - ~7us - and NOT the transmit at all, while also delaying the show.
  // That made the X8->X16 question unanswerable: the X16 driver writes uint16_t DMA
  // words (2 bytes per bit period) against the X8's uint8_t (1 byte), so the
  // transfer volume may have DOUBLED, which would halve the maximum frame rate and
  // therefore the dot speed. This now reports the real figure.
  static unsigned long lastTxLog = 0;
  if (millis() - lastTxLog >= 1000) {
    lastTxLog = millis();
    int present = 0;
    for (int p = 0; p < NUM_PORTS; p++) if (g_strip[p]) present++;
    
  }
  {
    int32_t cu = (int32_t)(t1 - t0), xu = (int32_t)(t2 - t1);
    g_copyUsSum += cu; if (cu > g_copyUsMax) g_copyUsMax = cu;
    g_txUsSum   += xu; if (xu > g_txUsMax)   g_txUsMax   = xu;
  }
  g_npbShows++;
  g_showUsN++;
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
  char  id[32] = "";            // the playlist ITEM NAME - a reference, not a copy (Rules: the Build Process)
  float speed  = 0.2f;
  int   qty    = 1;
  int   size   = 1;
  bool  wrap   = false;
  bool  invert = false;
  bool  used   = false;
  // ORIENTATION, as saved with the item (-1 = the cell carries none, so it inherits the pattern).
  int   orient = -1;
  // The colour REFERENCES the item was saved with on the pattern parameters page
  // ("T0".."T7" / "RB" / "#rrggbb"), ADOPTED when this cell plays.
  //
  // The timeline has NO colour options of its own: a colour is set on the pattern
  // page, stored with the pattern in the playlist, and arrives here with the cell.
  // hasColour is false for a cell saved before this existed, and the strip then
  // simply keeps the colour it already has.
  uint32_t colour[4] = { 0xFFFFFF, 0xFFD000, 0xFF7000, 0xFF0000 };
  bool     hasColour = false;
  // The Beat Multiplier the item was saved with (1 or 2). 0 means the cell has none,
  // and then it is NOT adopted. It is a PER-DISPLAY parameter: it is set on the
  // pattern page, stored with the pattern in the playlist, and adopted here when the
  // cell plays - exactly like speed, size and colour.
  int      mult = 0;
  // The pattern's own LISTENING settings, so a cell can be strict while the next one
  // on the same port is sensitive. -1 / 0 mean "the cell carries none".
  float    beatSens = -1.0f;
  int      freqHz   = 0;
  // THE MOVING DOT EFFECT travels with the pattern like everything else. -1 means "the cell
  // carries none", so an older cell leaves the pattern's own values in force.
  int      effect   = -1;      // -1 none | 0 = off | 1..10 pieces per reversal
  float    momentum = -1.0f;   // 0..0.5 s: the pieces' speed away from the dot
  int      distance = -1;      // 0..100 pixels the effect may use
};

// ============================================================================
// THE PLAYLIST ITEM - THE MASTER PARAMETER STORE (Rules: the Build Process, 20 Oct)
// ----------------------------------------------------------------------------
// The Build Process says the pattern page creates the playlist item "with all the parameter settings
// stored at that time", the user names it (no duplicate names), adds it to the timeline, and then
// "the timeline when playing comes across a cell with an entry, and finds the associated playlist
// entry and obtains all the parameter data needed to display that pattern for that period".
//
// So the ITEM is the master and a CELL is only a NAME. While the timeline plays, NOTHING may fall
// back to the pattern page or to a pattern's stored record: the page is volatile, and reaching into
// it could change a running show. An empty cell, or a name with no item behind it, means that port
// is DARK for that slot - it does not mean "carry on with something else".
//
// Old timeline files keep working: a cell carries the item's name among the fields it copied, and
// only the name is read now.
// ============================================================================
#define PL_MAX_ITEMS 48
struct PlItem {
  char         name[32] = "";                 // the playlist name the cell references
  LedPattern   pat      = LP_MOVING_DOT;      // resolved from the NAME (renderBranchFor)
  LedPatParams p;                             // EVERY parameter the pattern page saved
  bool         used     = false;
};
static PlItem plItems[PL_MAX_ITEMS];
static int    plCount = 0;

static void          loadPlaylist();          // parse the saved playlist into plItems[]
static const PlItem* plFind(const char* nm);  // the item a cell names, or nullptr (-> DARK)

TlItem tlData[TL_ROWS][TL_COLS];
float tlDelaySec = 20.0f;
int   tlRow = 0;

// ---- END OF DISPLAY --------------------------------------------------------
// A cell whose id is this marker is a BOUNDARY, not a pattern: it lights nothing. A row
// containing one is the end of the show - reaching it restarts from row 1 and loops until
// STOP. The marker is placed by the page as a whole row, but each cell can then be moved
// or deleted on its own like any other item, so the test is per CELL, not per row.
static inline bool isEndOfDisplayId(const char *id) {
  // ACCEPT BOTH SPELLINGS (20 Oct): a cell now stores the playlist item's NAME, and the marker's is
  // "END OF DISPLAY" with a SPACE. patternIdFromName() used to store "END_OF_DISPLAY" with underscores
  // for the same cell, so only the underscored form matched - which made the marker row invisible, so
  // the show ran straight past it and the timeline never restarted. Both forms are accepted now, so
  // old cells and new cells both restart the show.
  return id && (strcmp(id, "END_OF_DISPLAY") == 0 || strcmp(id, "END OF DISPLAY") == 0);
}
static inline bool isEndOfDisplayRow(int r) {
  if (r < 0 || r >= TL_ROWS) return false;
  for (int c = 1; c < TL_COLS; c++)
    if (tlData[r][c].used && isEndOfDisplayId(tlData[r][c].id)) return true;
  return false;
}
// Which ports are currently OWNED by a timeline cell. Cleared when the timeline is
// reloaded, so a deleted cell reverts its port to the page settings.
bool  portCellOwned[NUM_PORTS] = {false};
// The sequence only ADVANCES while this is true. It starts false, so a saved timeline
// no longer runs itself at power-on: it is played deliberately with the Play button,
// which also restarts from row 1.
volatile bool tlPlaying = false;
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
// These are FAST (150 ms) and happen AT ONCE. They used to be confusable with the boot
// report, which is SLOW (500 ms, plus a 1 s marker flash 1.5 s in) - "4 flashes on IO48"
// was read as a reset when it was really /init-off. The report now owns IO48 until it
// finishes; a command blink waits for it (see the boot-report block in loop()).
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
//   dbgStage 1 = top of loop()  2 = display work  3 = IO48 blinks  4 = diagnostics
//            6 = 240V engine  7 = LED render  8 = LED transmit. ONLY loop() writes it
//            (webTask() never does), so a reported "1" is NOT the web server - that
//            misreading cost a session. See CODE_NOTES.md §3.
//   dbgReq   the HTTP request IN FLIGHT and dbgSub a checkpoint inside it: the web task's
//            own breadcrumbs, so a crash in a handler names itself and its line.
// ---------------------------------------------------------------------------
#define DBG_MAGIC 0x4C533951UL
RTC_NOINIT_ATTR uint32_t dbgMagic;
RTC_NOINIT_ATTR uint32_t dbgStage;
RTC_NOINIT_ATTR uint32_t dbgUpSec;
RTC_NOINIT_ATTR uint32_t dbgHeap;
RTC_NOINIT_ATTR uint32_t dbgLoops;
RTC_NOINIT_ATTR uint32_t dbgBoots;      // boots since the last real power cycle
RTC_NOINIT_ATTR uint32_t dbgNvsErase;   // how many of those erased the NVS
RTC_NOINIT_ATTR uint32_t dbgReq;        // request in flight (0 = none)
RTC_NOINIT_ATTR uint32_t dbgSub;        // checkpoint inside it (0 = none)
RTC_NOINIT_ATTR uint32_t dbgReqN;       // requests served this run

// REQUEST IDS. The list order IS the id, so the two can never drift apart.
enum { REQ_NONE = 0, REQ_SETTINGS, REQ_LIVE_TWEAK, REQ_CAL_STOP, REQ_CAL_SHOW,
       REQ_CAL_TEST, REQ_INIT_OFF, REQ_THEME_PREVIEW, REQ_TL_PLAY, REQ_TL_STOP,
       REQ_SAVE_FLASH, REQ_BRIGHT, REQ_DIAG, REQ_LOG, REQ_AUDIO, REQ_PAGEERR, REQ_TAIL };
static const char* const REQ_NAME[] = { "none", "/settings", "/live-tweak",
  "/calibrate-stop", "/calibrate-show", "/calibrate-test", "/init-off",
  "/theme-preview", "/timeline-play", "/timeline-stop", "/save-flash", "/brightness",
  "/diag", "/log.txt", "/audio", "/pagerr", "/tail" };
static inline const char* reqNameTxt(uint32_t r) {
  return (r < sizeof(REQ_NAME) / sizeof(REQ_NAME[0])) ? REQ_NAME[r] : "?";
}

// The last request, kept in NVS because RTC does NOT survive a power cut - and a power cut
// is exactly what a supply dip leaves behind (a share of the crashes are POWER_ON, where
// every RTC breadcrumb is garbage). Rate-limited: a slider drag sends ~10 tweaks a second
// and must not become flash wear or a stall.
static void reqPersist(uint32_t r) {
  static uint32_t lastR = 0; static unsigned long lastMs = 0;
  if (r == lastR && (millis() - lastMs) < 1000) return;
  lastR = r; lastMs = millis();
  Preferences p; p.begin("diag", false);
  p.putUInt("lastReq", r); p.putUInt("lastUp", (uint32_t)(millis() / 1000));
  p.putUInt("lastHeap", (uint32_t)ESP.getFreeHeap());
  p.end();
}

// EVERY request, by name, so the order at the moment of a fault is readable: which came
// first, /init-off or /calibrate-stop, and whether a tweak came at all. /diag is polled
// every 3 s so it is logged at most once every 10 s - the log must not become a flood.
static void reqTrace(uint32_t r) {
  static unsigned long lastDiagMs = 0;
  if (r == REQ_DIAG && (millis() - lastDiagMs) < 10000) return;
  if (r == REQ_DIAG) lastDiagMs = millis();
  logAppend("req %s heap=%u", reqNameTxt(r), (unsigned)ESP.getFreeHeap());
}

// One line at the top of a handler. Cleared by the destructor on EVERY return path, so it
// can never get stuck set.
struct ReqMark {
  explicit ReqMark(uint32_t r) { dbgReq = r; dbgReqN++; reqPersist(r); reqTrace(r); }
  ~ReqMark() { dbgReq = REQ_NONE; dbgSub = 0; }
};

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

// Reorder a colour according to a calibration index.
// THE AUTHORITATIVE TABLE - this is what the firmware actually does:
//   0:RGB  1:GRB  2:BRG  3:RBG  4:GBR  5:BGR
// NOTE: index 1 is GRB and index 3 is RBG. A previous log label in this file had
// those two swapped, which sent the calibration investigation down the wrong path:
// a stored value of 3 was reported as "GRB" when the code was applying RBG.
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
  if (n.equalsIgnoreCase("Chase")           || n.equalsIgnoreCase("chase"))            return "chase_1_10";
  if (n.equalsIgnoreCase("Chase 1 - 10")    || n.equalsIgnoreCase("chase_1_10"))       return "chase_1_10";
  // Chase 10 - 1 IS GONE as a pattern of its own; an old item that names it resolves to the
  // one Chase, so it still plays - as the new behaviour.
  if (n.equalsIgnoreCase("Chase 10 - 1")    || n.equalsIgnoreCase("chase_10_1"))       return "chase_1_10";
  if (n.equalsIgnoreCase("Chase 10 - 1")    || n.equalsIgnoreCase("chase_10_1"))       return "chase_10_1";
  if (n.equalsIgnoreCase("Explosion")       || n.equalsIgnoreCase("center_out_chase")) return "center_out_chase";
  if (n.equalsIgnoreCase("Knight Rider")    || n.equalsIgnoreCase("knight_rider"))     return "knight_rider";
  if (n.equalsIgnoreCase("VU Meter Left")   || n.equalsIgnoreCase("vu_meter"))         return "vu_meter";
  // ONE VU pattern now: the name in the list is plain "VU Meter". Its DIRECTION is a
  // row in the Special block, not a separate pattern.
  //
  // Nothing used to map "VU Meter" at all (only "VU Meter Left" was mapped), so it
  // fell through to the id "vu meter" - with a SPACE - matched nothing, and every VU
  // selection rendered as a MOVING DOT. That is fixed here AND in
  // patternEnumFromName(), which now normalises the string, so a name can no longer
  // silently degrade to Moving Dot.
  if (n.equalsIgnoreCase("VU Meter")        || n.equalsIgnoreCase("vu meter"))         return "vu_meter";
  // The old Right/Dual/Centre names keep their OWN ids, so a playlist saved before the
  // change still renders the way it did - with its own direction. (They are no longer
  // reachable from the pattern list, which is now a single VU Meter entry.)
  if (n.equalsIgnoreCase("VU Meter Right")  || n.equalsIgnoreCase("vu_meter_rev"))     return "vu_meter_rev";
  if (n.equalsIgnoreCase("VU Meter Dual")   || n.equalsIgnoreCase("vu_dual"))          return "vu_dual";
  if (n.equalsIgnoreCase("VU Meter Centre") || n.equalsIgnoreCase("vu_center_out"))    return "vu_center_out";
  if (n.equalsIgnoreCase("EQ")              || n.equalsIgnoreCase("eq"))               return "eq";
  // END OF DISPLAY: a whole-row BOUNDARY marker, not a pattern. The board maps names to ids
  // like this for every item, so the marker gets its own id - and can therefore never
  // silently degrade into Moving Dot, which is what an unmapped name does.
  if (n.equalsIgnoreCase("END OF DISPLAY")   || n.equalsIgnoreCase("END_OF_DISPLAY"))  return "END_OF_DISPLAY";
  // NAMES WITH EXTRA WORDS (user, 19 Oct). Items are named freely on the page - "Chase Green x2",
  // "Moving Dot Red Fast" - and the exact matches above can never resolve those. An unmapped name
  // fell through to its own lowercased text, which matches NO pattern, and renderBranchFor() then
  // degrades it to MOVING DOT. That is what a saved "Chase Green x2" did: the timeline played for
  // two minutes with bKicks 0 while bOns was 198 - no port was ever a Chase, so stepChase() never
  // ran and there was nothing to kick.
  //
  // A CONTAINS pass, checked after every exact match and before the fallback, resolves any name
  // that MENTIONS a pattern. An exact name still wins, so nothing already working changes.
  String lc = n;
  lc.toLowerCase();
  if (lc.indexOf("chase")     >= 0) return "chase_1_10";
  if (lc.indexOf("knight")    >= 0) return "knight_rider";
  if (lc.indexOf("explosion") >= 0) return "center_out_chase";
  if (lc.indexOf("moving")    >= 0) return "moving_dot";
  if (lc.indexOf("vu")        >= 0) return "vu_meter";
  if (lc.indexOf("eq")        >= 0) return "eq";
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
//
// The SOURCE is chosen by the CALLER now, because that is the agreed model:
//   LED VU meter -> BASS LEVEL  (fullRange = false)
//   EQ           -> FULL        (fullRange = true)
//   the 240V VU  -> its own Music Source control, passed in by its caller
// The MAX9814 barely responds below ~150 Hz, so a silent low band folds in the
// mid band rather than leaving the meters dead.
int computeVuLevel(float &smooth, float attack, float decay, bool fullRange) {
  float src;
  if (fullRange) {
    src = audioLevel;                 // EQ: the whole spectrum
  } else {
    // BASS LEVEL (61-500 Hz). This used to be max(bass, mid), a workaround for the microphone
    // barely responding below ~150 Hz. Bass Level is that band done properly, so the workaround
    // is gone: a meter should be reading bass level (user, 19 Oct).
    src = audioBassLvl;
  }
  float target = constrain(src, 0.0f, 1.0f) * 10.0f;
  float a = constrain(attack, 0.01f, 1.0f);
  float d = constrain(decay, 0.01f, 1.0f);
  if (target > smooth) smooth += a * (target - smooth);
  else                 smooth += d * (target - smooth);
  if (smooth < 0.3f) smooth = 0;
  return (int)(smooth + 0.5f);
}

// Advance the EQ bars ONCE PER FRAME. Doing it per port would run the ballistics
// ten times a frame, making the EQ react ten times faster than the VU meter does.
void advanceEqBars(float attack, float decay) {
  float a = constrain(attack, 0.01f, 1.0f);
  float d = constrain(decay, 0.01f, 1.0f);
  for (int e = 0; e < EQ_BANDS; e++) {
    float target = constrain(audioEqBand[e], 0.0f, 1.0f) * (float)EQ_MAX;
    if (target > eqSmooth[e]) eqSmooth[e] += a * (target - eqSmooth[e]);
    else                      eqSmooth[e] += d * (target - eqSmooth[e]);
    if (eqSmooth[e] < 0.3f) eqSmooth[e] = 0;
    eqBar[e] = (int)(eqSmooth[e] + 0.5f);
  }
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
  preferences.putInt("multLED2", multLED);   // NEW KEY - see loadSettings()
  // (musicLED is gone: the LED VU meter is BASS LEVEL only, the EQ is FULL.)
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
  // PER THEME: four themes x eight colours. Keys are short (th0c0..th3c7) to stay
  // inside the NVS 15-character key limit.
  for (int t = 0; t < THEME_COUNT; t++) {
    for (int i = 0; i < 8; i++) {
      uint32_t c = ((uint32_t)themeStore[t][i].r << 16) |
                   ((uint32_t)themeStore[t][i].g << 8) | themeStore[t][i].b;
      preferences.putUInt(("th" + String(t) + "c" + String(i)).c_str(), c);
    }
  }
  preferences.end();
  // EVERY pattern's own values, one blob per pattern, written AFTER the end() above because
  // Preferences cannot have the same namespace open twice at once.
  saveLedPatRecords();
}

// Restore runtime settings from NVS
void loadSettings() {
  preferences.begin("lightshow", true);
  audioEnabled = preferences.getBool("audioEnabled", true);
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
  // NEW KEY, and the default is 1, deliberately.
  //
  // The old key held 2 on every board (it was the initialiser's value) because the
  // multiplier used to have NO visible effect - it only moved clock counters that
  // nothing displayed. Now that it means "2 = an extra reversal half way between
  // beats", reading the old value would DOUBLE the reversal rate on every board
  // and the display would look over-sensitive. The old key is simply abandoned,
  // exactly as beatSens2 and beatOff2 were when their meaning changed.
  multLED = preferences.getInt("multLED2", 1);
  if (multLED > 1) multLED = 2; else multLED = 1;
  // (musicLED is gone: the LED VU meter is BASS LEVEL only, the EQ is FULL.)
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
  previewColorCount = 8;   // ALWAYS 8 - the count concept is gone entirely.
  // PER THEME restore. A theme with nothing stored keeps what it has, so the
  // presets the UI sends on first use are not wiped by a reboot.
  for (int t = 0; t < THEME_COUNT; t++) {
    for (int i = 0; i < 8; i++) {
      uint32_t c = preferences.getUInt(("th" + String(t) + "c" + String(i)).c_str(), 0);
      if (c != 0 || i == 0) {   // 0 means "nothing stored" (pure black is left alone)
        themeStore[t][i] = CRGB((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
      }
    }
  }
  preferences.end();

  // PER-PATTERN RECORDS. The single LED values just read become the SELECTED pattern's own
  // record if it has none yet (the one-off migration); every other pattern starts from its
  // own defaults. mirrorLedPat() then makes the globals agree with the selected record, so
  // every reader downstream is unchanged.
  bool ledMigrated = loadLedPatRecords();
  mirrorLedPat();
  logAppend("LED records %s selected=%s", ledMigrated ? "MIGRATED: selected seeded from pre-record settings"
                                                      : "loaded per pattern",
            activePatternLED.c_str());

  freqThreshold = constrain(freqLED, 20, 100);
  multiplier = (multLED > 1) ? 2 : 1;   // the DISPLAY multiplier (LED page checkbox)
  // (currentMusicMode is gone - it was written and never read.)
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

// THE PAGE'S OWN CRASHES, reported by the page (window.onerror / unhandledrejection).
// A JS exception while switching tabs is invisible to this firmware - the board stays
// perfectly healthy while the screen looks dead - so the page sends it here and it lands in
// /log.txt next to the requests, with the line and the stack.
void handlePageErr() {
  ReqMark _req(REQ_PAGEERR);
  logAppend("PAGE ERROR %s @%s stack=%s", server.arg("m").c_str(),
            server.arg("l").c_str(), server.arg("s").c_str());
  server.send(200, "text/plain", "logged");
}

void handleGetSettings() {
  ReqMark _req(REQ_SETTINGS);
  JsonDocument doc;
  doc["audioEnabled"] = audioEnabled;
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
  doc["music240"] = music240V;    // (musicLED removed: the LED source is fixed)
  doc["beatSens"] = beatSens;      // global: one detector drives both displays
  doc["vuDir"] = vuDirLED;         // the single VU pattern's direction row
  // Per-port colour calibration order (0=RGB 1=GRB 2=BRG 3=RBG 4=GBR 5=BGR).
  // Exposed so the UI can SHOW the stored order for the selected port instead of
  // always displaying RGB and disagreeing with what the strip is doing.
  {
    JsonArray ca = doc["portCal"].to<JsonArray>();
    for (int i = 0; i < NUM_PORTS; i++) ca.add(portColorOrders[i]);
  }
  // LED colour: BOTH the reference that was stored AND the value it currently
  // resolves to.
  //   colourRefLED - the authoritative model. The UI restores its SELECTION from
  //                  this, so a reload still shows "T2" selected rather than a
  //                  resolved colour that a theme change could no longer move.
  //                  A playlist item stores these same strings.
  //   colourLED    - the resolved value (a theme reference read against the live
  //                  palette), kept because it is the quickest read-out of what
  //                  the strip is actually about to draw. For "RB" it is a
  //                  placeholder: a rainbow has no single value.
  {
    JsonArray refs = doc["colourRefLED"].to<JsonArray>();
    JsonArray vals = doc["colourLED"].to<JsonArray>();
    char buf[8];
    for (int i = 0; i < 4; i++) {
      refToString(colourLED[i], buf, sizeof(buf));
      refs.add(buf);
      CRGB v = resolveColourRef(colourLED[i]);
      snprintf(buf, sizeof(buf), "#%06lx",
               (unsigned long)(((uint32_t)v.r << 16) | ((uint32_t)v.g << 8) | v.b));
      vals.add(buf);
    }
  }
  // ALL FOUR THEMES x 8 COLOURS.
  //
  // This used to report only the CURRENT theme's eight, so the UI could only make
  // one theme master - the other three kept whatever the browser already had, and
  // the corrupted values then got recirculated on the next save. The device holds
  // all 32 in themeStore[][] already; this reports them so the UI can adopt the
  // device as master for every theme, which is the agreed model:
  //   startup        -> stored values are master
  //   edit, no save  -> your changes, for this power cycle only
  //   edit and save  -> stored values updated, become the new master
  {
    JsonArray all = doc["themeColorsAll"].to<JsonArray>();
    for (int t = 0; t < THEME_COUNT; t++) {
      JsonArray row = all.add<JsonArray>();
      for (int i = 0; i < 8; i++) {
        char buf[8];
        snprintf(buf, sizeof(buf), "#%02X%02X%02X",
                 themeStore[t][i].r, themeStore[t][i].g, themeStore[t][i].b);
        row.add(buf);
      }
    }
  }
  doc["brightness"] = masterBrightness;
  doc["theme"] = themeName;
  doc["themeCount"] = previewColorCount;
  JsonArray cols = doc["themeColors"].to<JsonArray>();
  for (int i = 0; i < 8; i++) {
    char buf[8];
    snprintf(buf, sizeof(buf), "#%02X%02X%02X", previewColors[i].r, previewColors[i].g, previewColors[i].b);
    cols.add(buf);
  }
  // ALL SIX LED PATTERNS, each with its OWN values. Without this a page reload could only
  // learn the SELECTED pattern's numbers, and the first tweak after switching would then
  // overwrite another pattern's saved values with defaults. The page seeds its per-pattern
  // store from this, exactly as it adopts themeColorsAll.
  {
    JsonObject all = doc["ledPatAll"].to<JsonObject>();
    for (int i = 0; i < LED_PAT_COUNT; i++) {
      JsonObject o = all[LED_PAT_IDS[i]].to<JsonObject>();
      o["speed"] = ledPats[i].speed;      o["qty"] = ledPats[i].qty;
      o["size"] = ledPats[i].size;        o["wrap"] = ledPats[i].wrap;
      o["invert"] = ledPats[i].invert;    o["orient"] = ledPats[i].orient;
    o["mult"] = ledPats[i].mult;
      o["freq"] = ledPats[i].freqHz;      o["beatSens"] = ledPats[i].beatSens;
      o["atk"] = ledPats[i].attack;       o["dec"] = ledPats[i].decay;
      o["peakDelay"] = ledPats[i].peakDelay; o["peakSpeed"] = ledPats[i].peakSpeed;
      o["peakDir"] = ledPats[i].peakDir;  o["vuDir"] = (int)ledPats[i].vuDir;
      o["effect"] = (int)ledPats[i].effect; o["momentum"] = ledPats[i].momentum;
      o["distance"] = (int)ledPats[i].distance;
      JsonArray cr = o["colourRef"].to<JsonArray>();
      char cb[8];
      for (int k = 0; k < 4; k++) { refToString(ledPats[i].colour[k], cb, sizeof(cb)); cr.add(cb); }
    }
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

// Stop the calibration test. previewMode = 1 is otherwise LATCHED: once a test
// runs the strip stays lit showing it, forever, including when the Colours page is
// reopened - which is the "strip lights up when I open the Colours page" fault.
// Nothing ever exited the test until now.
// Show the STORED order for a port. The DEVICE owns the truth here.
//
// TEST used to call /calibrate-test with the page's own orderIndex, and the page
// advanced that index on every press - so TEST always illuminated the value AFTER
// the stored one, and pressing it to CONFIRM a save showed a different colour
// instead. The device now reports what it has, so the button confirms rather than
// contradicts.
void handleCalibrateShow() {
  ReqMark _req(REQ_CAL_SHOW);
  int port = server.arg("port").toInt();
  if (port < 0 || port >= NUM_PORTS) port = 0;
  calPort  = port;
  calOrder = portColorOrders[port];      // the STORED value, not a page guess
  previewMode = 1;
  previewDirty = true;
  
  server.send(200, "text/plain", "Showing stored order " + String(calOrder) +
                                  " for port " + String(port + 1));
}

void handleCalibrateStop() {
  ReqMark _req(REQ_CAL_STOP);
  bool wasPreview = (previewMode != 0);
  previewMode = 0;
  previewDirty = true;
  // BLANK ONLY WHEN THERE IS SOMETHING TO CLEAR. This is sent by EVERY tab switch, so
  // blanking unconditionally made navigation darken a RUNNING show (the log shows the blank
  // arriving while displayActive was 1). A stale TEST FRAME must still be cleared when
  // nothing is driving the strips - with no pattern running, previewMode = 0 on its own
  // leaves the previous test's pixels lit, which is why the second half is there.
  if (wasPreview || !displayActive) blankReq = true;
  server.send(200, "text/plain", "Calibration test stopped");
}

void handleCalibrateTest() {
  ReqMark _req(REQ_CAL_TEST);
  if (server.hasArg("port") && server.hasArg("order")) {
    calPort = server.arg("port").toInt();
    calOrder = server.arg("order").toInt();
    // Log what the PAGE sent, so the sequence is visible:
    //   CALTEST is what is being tested
    //   CALSAVE is what was stored
    //   CALIB (at boot) is what was loaded back
    // A mismatch between those three IS the fault.
    
    previewMode = 1;      // stays lit until SAVE, STOP, or leaving the tab
    previewDirty = true;
    showStatus(2);        // IO48: 2 blinks = calibrate-test received
    server.send(200, "text/plain", "Calibration Test Active");
  } else {
    server.send(400, "text/plain", "Missing arguments");
  }
}

// Save whatever the device is CURRENTLY SHOWING for a port.
//
// This exists because the page's SAVE sent its own orderIndex, which is page state
// and can be stale. Pressing TEST shows the STORED value (correct), then SAVE sent
// the page's unrelated index and OVERWROTE the good value - the strip flashed the
// wrong colours and the port tested wrong afterwards.
//
// The device knows exactly what it is illuminating (calOrder), so it saves that.
void handleCalibrateSaveCurrent() {
  int port = server.arg("port").toInt();
  if (port < 0 || port >= NUM_PORTS) { server.send(400, "text/plain", "Bad port"); return; }
  int order = calOrder;                       // what is on the strip right now
  if (order < 0 || order > 5) order = portColorOrders[port];
  portColorOrders[port] = order;
  preferences.begin("lightshow", false);
  preferences.putInt(("port_ord_" + String(port)).c_str(), order);
  preferences.end();
  
  // Acknowledge on the strip: off -> on -> off.
  calPort = port;
  calOrder = order;
  calFlashMs = millis();
  previewDirty = true;
  server.send(200, "text/plain", "Port " + String(port + 1) + " Calibration Saved!");
}

void handleCalibrateSave() {
  // Logged BEFORE the argument check, so we can tell "the request never arrived"
  // apart from "the arguments were rejected". Without this the absence of a
  // CALSAVE line is ambiguous - and it was, for a whole round.
  
  if (server.hasArg("port") && server.hasArg("order")) {
    int port = server.arg("port").toInt();
    int order = server.arg("order").toInt();

    if (port >= 0 && port < NUM_PORTS) {
      portColorOrders[port] = order;
      preferences.begin("lightshow", false);
      preferences.putInt(("port_ord_" + String(port)).c_str(), order);
      preferences.end();
      // Log what was ACCEPTED and stored, so it can be compared with the CALTEST
      // on the same press and with the CALIB line after a power cycle.
      
      // Acknowledge on the strip: off -> on -> off.
      calPort = port;
      calOrder = order;
      calFlashMs = millis();
      previewDirty = true;
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
  ReqMark _req(REQ_THEME_PREVIEW);
  if (!server.hasArg("plain")) { server.send(400, "text/plain", "Bad Request"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "text/plain", "Invalid JSON");
    return;
  }

  int count = doc["count"] | 0;
  count = constrain(count, 0, 8);
  previewColorCount = 8;   // always 8; the incoming count is ignored

  String tn = String((const char*)(doc["theme"] | "Disco"));
  if (tn.length()) themeName = tn;

  // Write into THIS theme's slot, then copy it to previewColors for rendering.
  int ti = themeIndexOf(themeName);
  for (int i = 0; i < 8; i++) themeStore[ti][i] = CRGB::Black;
  JsonArray colors = doc["colors"].as<JsonArray>();
  int idx = 0;
  for (JsonVariant v : colors) {
    if (idx >= 8) break;
    themeStore[ti][idx] = hexToCRGB(v.as<String>());
    idx++;
  }

  themeLoadToPreview(ti);
  // A theme edit must RE-RESOLVE every stored reference: a slot holding "T2" has
  // to move to the new T2, which is the entire point of storing a reference.
  // Colour only - rebuilding the ports here would also reset pattern/speed/size on
  // every strip and undo a timeline row that is currently playing.
  applyColourRefsToPorts();
  previewMode = 2;      // ONE-SHOT: rendered once and released at once (see the loop)
  previewDirty = true;
  showStatus(3);        // IO48: 3 blinks = theme-preview received

  // SAVE THE THEME COLOURS IMMEDIATELY.
  //
  // They used to ride on the deferred saveSettingsPending path, which is
  // deliberately NOT run while displayActive - a flash write during a show is the
  // erase that froze the display. But a theme colour change is a DELIBERATE user
  // action, and the strip is normally lit while editing, so the save was held
  // forever: nothing was persisted and every theme came back BLACK on reboot.
  //
  // Writing here costs one flash erase when the user changes a colour. That is
  // acceptable for an explicit save; the stall problem was CONTINUOUS logging
  // writes, not this. saveSettingsPending is cleared so the same data is not
  // written twice.
  saveSettingsPending = false;
  saveSettings();
  // Persisted only on an explicit Save (see /save-flash).

  server.send(200, "text/plain", "Theme Preview Active");
}

// Live parameter updates from the 240V / LED configuration sliders
void handleLiveTweak() {
  ReqMark _req(REQ_LIVE_TWEAK);
  if (!server.hasArg("plain")) { server.send(400, "text/plain", "Bad Request"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "text/plain", "Invalid JSON");
    return;
  }

  String section = String((const char*)(doc["section"] | "240V"));
  String name    = String((const char*)(doc["name"] | ""));
  String id = patternIdFromName(name);
  dbgSub = 1;          // past the JSON parse: a crash before this line is the body/parse
  // WHAT ARRIVED. Change-only: a slider drag sends ~10 tweaks a second, and a log line
  // for each would be flash wear. This is what proves whether opening a page sends a
  // tweak AT ALL - the question that decides which request the fault lives in.
  {
    JsonArray ca = doc["colour"].as<JsonArray>();
    static String lastSig;
    String sig = section + "|" + id + "|" + String(ca.isNull() ? 0 : (unsigned)ca.size());
    if (sig != lastSig) {
      lastSig = sig;
      // The SPEED the page sent is the whole question for "it runs very slowly": Chase sends
      // LEDs/s (1-50) and the dot patterns send SECONDS per step, and the value is shared.
      // THE COLOUR (slot 0's reference) AND THE EFFECT VALUES ARE LOGGED WITH IT. "the colour
      // would not change" and "the effects would not go away" are then answerable from the log
      // instead of argued about - and this is what the page SENT, not what the strip showed.
      char cbuf[8];
      JsonArray cArr = doc["colour"].as<JsonArray>();
      uint32_t cref = parseColourRef((!cArr.isNull() && cArr.size() > 0)
                                       ? cArr[0].as<const char*>() : nullptr, 0xFFFFFFFFu);
      if (cref == 0xFFFFFFFFu) snprintf(cbuf, sizeof(cbuf), "(none)");
      else                     refToString(cref, cbuf, sizeof(cbuf));
      logAppend("tweak %s speed=%.3f col0=%s eff=%d mom=%.2f dist=%d bytes=%u heap=%u",
                sig.c_str(), (double)jsonNumF(doc["speed"], -1.0f), cbuf,
                jsonNumI(doc["effect"], -1), (double)jsonNumF(doc["momentum"], -1.0f),
                jsonNumI(doc["distance"], -1),
                (unsigned)server.arg("plain").length(), (unsigned)ESP.getFreeHeap());
    }
  }

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

  // Beat sensitivity: PER PATTERN for the LED section (written into the record above and
  // mirrored into the globals), so what remains here is the 240V section's own detector
  // value. A 240V tweak must not be able to move an LED pattern's threshold.
  if (section == "240V" && doc.containsKey("beatSens")) {
    float bs = jsonNumF(doc["beatSens"], beatSens);
    beatSens = constrain(bs, 0.01f, 1.00f);
  }

  dbgSub = 2;          // scalars read; the section branch below is the prime suspect
  if (section == "LED") {
    // A PATTERN CHANGE PERSISTS THE RECORDS. Without this, a reboot before a Save would
    // re-seed the newly selected pattern from the legacy single-value keys and hand it the
    // OTHER pattern's values - the contamination this whole change exists to remove.
    // Selecting a pattern is a deliberate act (the same case as a theme colour, which is
    // already allowed its one write); sliders still never touch flash.
    if (id.length() && id != activePatternLED) { saveLedPatRecords(); mdResetAll(); }
    if (id.length()) activePatternLED = id;
    // THIS PATTERN'S OWN RECORD - never another pattern's. The globals are refreshed from it
    // by mirrorLedPat() below, which is what keeps every existing reader correct.
    LedPatParams& r = ledSel();
    r.speed = spd; r.qty = constrain(qty, 1, 8); r.size = constrain(sz, 1, 10);
    r.freqHz = frq; r.mult = (bm > 1) ? 2 : 1;   // checkbox: 1 or 2
    r.attack = atk; r.decay = dec;
    r.wrap = wrap; r.invert = inv;
    // ORIENTATION arrives as a JSON BOOLEAN, and jsonNumI() only understands strings and numbers -
    // a bool falls through to its default, so this read back as 0 every time and the toggle did
    // nothing (user, 19 Oct). as<bool>() converts either form correctly.
    if (doc.containsKey("orient")) r.orient = doc["orient"].as<bool>() ? 1 : 0;
    // The per-pattern EFFECT rows. Stored now so the values travel with the pattern; nothing
    // renders them until the Moving Dot effect build.
    if (doc.containsKey("effect"))   r.effect   = (uint8_t)constrain(jsonNumI(doc["effect"], 0), 0, 10);
    // 0-0.5 is the GLOBAL maximum; the Chase's 0-0.1 range is a PAGE range for that pattern only.
    if (doc.containsKey("momentum")) r.momentum = constrain(jsonNumF(doc["momentum"], 0.0f), 0.0f, 0.5f);
    if (doc.containsKey("distance")) r.distance = (uint8_t)constrain(jsonNumI(doc["distance"], 0), 0, 100);
    // The LED section's sensitivity is THIS pattern's own. (The 240V section still writes the
    // shared detector value until its own parameter list is defined.)
    if (doc.containsKey("beatSens"))
      r.beatSens = constrain(jsonNumF(doc["beatSens"], r.beatSens), 0.01f, 1.00f);
    // Colour: the UI sends an array of REFERENCES - "T0".."T7" (theme palette
    // index, resolved live), "RB" (rainbow) or "#rrggbb" (custom). Index 0 is
    // used by the dot/chase/centre-out patterns, and 0..3 are the VU meter's
    // four bands. An unparseable entry leaves its slot alone.
    JsonArray ca = doc["colour"].as<JsonArray>();
    if (!ca.isNull()) {
      int i = 0;
      for (JsonVariant v : ca) {
        if (i >= 4) break;
        r.colour[i] = parseColourRef(v.as<const char*>(), r.colour[i]);
        i++;
      }
    }
    // VU meter DIRECTION, from the Special block's row. The single VU pattern then
    // renders using whichever of the four branches this selects.
    if (doc.containsKey("vuDir")) {
      String d = String((const char*)(doc["vuDir"] | "LEFT"));
      d.toUpperCase();
      if (d == "LEFT")        r.vuDir = 0;
      else if (d == "RIGHT")  r.vuDir = 1;
      else if (d == "CENTRE") r.vuDir = 2;
      else if (d == "ENDS")   r.vuDir = 3;
    }
    mirrorLedPat();          // the globals now agree with THIS pattern's record
    ledPatNormalise(ledPatIndex(activePatternLED));   // Moving Dot's Wrap is forced off
  }
  // THE 240V SECTION'S OWN VALUES. This branch did not exist at all: the LED branch above
  // wrote the 240V globals too, which is exactly the cross-section contamination the
  // isolation rule forbids - and the reason the 240V page's own sliders did nothing. The
  // 240V patterns still share one set until their own parameter list is defined, but they no
  // longer receive LED values.
  else if (section == "240V") {
    if (id.length()) activePattern240V = id;
    speed240V = spd; quantity240V = constrain(qty, 1, 4); size240V = constrain(sz, 1, 4);
    freq240V = frq; mult240V = constrain(bm, 1, 9); music240V = music;
    attack240V = atk; decay240V = dec;
    wrap240V = wrap; invert240V = inv;
  }

  dbgSub = 3;          // the section branch is done
  if (id.length()) displayActive = true;  // a pattern was selected: start displaying
  // Re-base the per-strip config on the LED-page settings, then re-apply the
  // current Timeline row (its columns 1-10 override the individual strips).
  // ONE place builds the port config: each port is derived from its own source of
  // truth - its cell in the CURRENT row, or the page as the default. There is no
  // "stamp the page onto every port" step any more, because that is exactly what used
  // to overwrite timeline-controlled ports whenever the page was touched.
  applyTimelineRow(tlRow, false);
  dbgSub = 4;          // the per-port config has been rebuilt

  previewMode = 0;                        // a new selection cancels any held preview/calibration
  // NOTE: no IO48 blink here. IO48 is reserved for the raw beat detector (one
  // flash per detection, in loop()) so that what you see on the board is the
  // detector's behaviour and nothing else. A blink per slider twitch made that
  // impossible to read while adjusting the display during playback.

  // The shared detector follows the most recently tweaked section
  freqThreshold = constrain(frq, 20, 100);
  // The DISPLAY multiplier belongs to the LED page's checkbox. The 240V slider no
  // longer writes to it: its value used to move only clock counters that nothing
  // read, and it must not be able to double the LED display's reversal rate.
  if (section == "LED") multiplier = multLED;
  // musicFullMode belongs to the 240V meter ALONE. The LED meter is bass-only by
  // design and its source control has been removed, so an LED tweak must not be
  // able to change what the 240V meter shows.
  if (section == "240V") musicFullMode = (music == "FULL");

  // NOTE: settings are only persisted on an explicit Save (/save, /save-flash).
  // Auto-saving on every tweak filled the small NVS partition and stopped the
  // WiFi driver storing its calibration data (phy_init error 0x1105).

  dbgSub = 5;          // everything applied; only the reply is left
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

// READ THE TIMELINE BACK.
//
// The UI could only ever WRITE the timeline, so after a page reload its grid looked
// EMPTY while the board was still running /timeline.json - which is exactly the "it is
// playing old data that is not shown on the timeline" report. This lets the page show
// what the board actually holds.
void handleLoadTimeline() {
  if (!LittleFS.exists("/timeline.json")) {
    server.send(404, "text/plain", "No timeline file found");
    return;
  }
  File file = LittleFS.open("/timeline.json", "r");
  if (!file) { server.send(500, "text/plain", "Failed to open timeline file"); return; }
  String content = file.readString();
  file.close();
  server.send(200, "application/json", content);
}

// PLAY: always start from the BEGINNING. Row 0 is applied with force, so every strip's
// motion restarts from a known state instead of continuing from wherever it happened
// to be.
void handleTimelinePlay() {
  ReqMark _req(REQ_TL_PLAY);
  if (!tlLoaded) loadTimeline();
  if (!tlLoaded) { server.send(404, "text/plain", "No timeline saved to play"); return; }
  tlRow      = 0;
  tlRowStart = millis();
  tlPlaying  = true;
  applyTimelineRow(0, true);
  displayActive = true;
  showStatus(1);
  server.send(200, "text/plain", "Timeline playing from the first slot");
}

// STOP: the display must go DARK. displayActive = false stops the render loop drawing,
// and blankReq makes the display task clear and transmit black once - the same path the
// "All Off" button uses.
void handleTimelineStop() {
  ReqMark _req(REQ_TL_STOP);
  tlPlaying     = false;
  previewMode   = 0;
  displayActive = false;
  blankReq      = true;
  // SYNCHRONISE THE MOTION of every port.
  //
  // Each port keeps its OWN position and direction, and a beat flips each port's own
  // direction. Two strips with identical settings could therefore travel in OPPOSITE
  // directions while both reversing on the same beats - so they stayed mirrored for
  // ever and it was impossible to tell which port was being programmed. Programming now
  // always starts from the same state in every port, which is what makes it
  // port-agnostic: identical settings render identically.
  for (int p = 0; p < NUM_PORTS; p++) {
    ledPosArr[p] = 0; ledDirArr[p] = 1; ledExplArr[p] = 0; ledStepMs[p] = 0;
    // RELEASE the ports the timeline blanked. A blank cell in the playing slot sets
    // portCfg[].dark, and the render loop SKIPS a dark port - so without this a stopped show
    // left those strips unable to be lit by the page at all. The strips themselves are
    // already blank (blankReq above), which is what STOP asks for; this only frees them.
    portCfg[p].dark = false;
  }
  showStatus(1);
  server.send(200, "text/plain", "Timeline stopped - outputs dark, motion synchronised");
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
  ReqMark _req(REQ_BRIGHT);
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
  ReqMark _req(REQ_AUDIO);
  if (server.hasArg("enabled")) {
    String v = server.arg("enabled");
    audioEnabled = v.equalsIgnoreCase("true") || v == "1";
    saveSettings();
    server.send(200, "text/plain", audioEnabled ? "Audio Detect ON" : "Audio Detect OFF");
  } else {
    server.send(400, "text/plain", "Missing enabled");
  }
}

// The beat instruments are added to /diag by a function defined DOWN WITH THE BEAT STATE - the
// globals it reads (beatListen, odfChainUse, odfNovelty) are declared after this handler, and this
// prototype is what lets /diag call it from here.
static void diagBeatFields(JsonDocument &doc);


// Crash diagnostics: why did the previous run end, and where was it?
void handleDiag() {
  ReqMark _req(REQ_DIAG);
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
  uint32_t pReq     = dp.getUInt("req", 0);
  uint32_t pSub     = dp.getUInt("sub", 0);
  uint32_t pLastReq = dp.getUInt("lastReq", 0);
  uint32_t pLastUp  = dp.getUInt("lastUp", 0);
  uint32_t pLastHp  = dp.getUInt("lastHeap", 0);
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
  // LEGEND CORRECTED: 5 and 9 are never written anywhere, and 1 is the TOP OF loop().
  // The web task does not write dbgStage at all - prevReq/prevSub are its breadcrumbs.
  doc["prevStage"]     = pStage;                  // 1=top of loop 2=display work
                                                  // 3=IO48 blinks 4=diagnostics
                                                  // 6=240V engine 7=LED render 8=transmit
  doc["prevReq"]       = pReq;                    // the request in flight when it died
  doc["prevReqTx"]     = reqNameTxt(pReq);
  doc["prevSub"]       = pSub;                    // ...and how far into it
  // THE POWER-CUT-PROOF PAIR. RTC is wiped by a real supply dip, so after a POWER_ON
  // reset prevReq/prevStage are garbage - these two are in NVS and always mean something.
  doc["lastReq"]       = pLastReq;
  doc["lastReqTx"]     = reqNameTxt(pLastReq);
  doc["lastReqUpSec"]  = pLastUp;                 // uptime when that request arrived
  doc["lastReqHeap"]   = pLastHp;
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
  // TIMELINE state, so the page can SHOW which slot is live. Without this the fact
  // that "my cells are in a later slot" is completely invisible - which is exactly
  // what made "two yellow strips" so puzzling.
  doc["tlLoaded"]  = tlLoaded  ? 1 : 0;
  doc["tlPlaying"] = tlPlaying ? 1 : 0;
  doc["tlRow"]     = tlRow + 1;              // 1-based, as the grid shows it
  // THE PLAYHEAD. The page draws a line across the timeline for "where we are inside this
  // slot", so it needs the time already spent in the row AND the row's length. Both come
  // from here, so the line lands in the right place even if the page is opened halfway
  // through a show: the board is the master, the page only interpolates between polls.
  doc["tlElapsedMs"] = tlPlaying ? (uint32_t)(millis() - tlRowStart) : 0;
  doc["tlDelayMs"]   = (uint32_t)(tlDelaySec * 1000.0f);
  // AUDIO LEVELS. The log stops flushing while a show runs and /tail is a DELTA endpoint, so
  // neither can be polled for this - and tuning the Chase's gate needs these numbers.
  //   bass 17-61 Hz (the FFT's "bass") | blvl 61-500 Hz BASS LEVEL | env its smoothed envelope
  //   mid 61-2000 Hz | lvl whole spectrum | peak = highest envelope since the last read
  doc["aBass"] = (double)audioBass;
  doc["aBlvl"] = (double)audioBassLvl;
  doc["aEnv"]  = (double)g_bassLvlEnv;
  doc["aPeak"] = (double)g_bassPeak;
  doc["aSlow"] = (double)g_bassSlow;   // the slow peak the Chase's gate is derived from
  doc["aMid"]  = (double)audioMid;
  doc["aLvl"]  = (double)audioLevel;
  diagBeatFields(doc);     // beat instruments: defined with the beat state, far below
  g_bassPeak = g_bassLvlEnv;                 // start a fresh peak for the next read
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

// THE LIVE TAIL (served at /tail). logService() deliberately does NOT write flash while a
// show runs - a LittleFS write is an erase that freezes both cores - so /log.txt goes DEAF
// exactly when the instrument matters most: the requests arriving at the moment of a fault.
// A run whose log stopped at 50 s while the board was at 264 s is what this fixes. The last
// lines are kept in RAM and served on the fly, show or no show. 40 x 160 B = 6.4 KB.
#define LOG_RECENT 40
char             logRecent[LOG_RECENT][160];
volatile uint8_t logRecentN = 0;         // total lines kept (wraps at LOG_RECENT)

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
  uint8_t rs = (uint8_t)(logRecentN % LOG_RECENT);   // the live tail: never dropped
  strncpy(logRecent[rs], buf, sizeof(logRecent[0]) - 1);
  logRecent[rs][sizeof(logRecent[0]) - 1] = '\0';
  logRecentN++;
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
  ReqMark _req(REQ_LOG);
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

// THE LIVE LOG, readable while a show is running. /log.txt only ever holds what logService()
// has flushed, and it does not flush during a show - so this answers "what happened at the
// moment I pressed that button?" from RAM, whatever the display is doing.
void handleTail() {
  ReqMark _req(REQ_TAIL);
  String out;
  out.reserve(LOG_RECENT * 48);
  portENTER_CRITICAL(&logLineMux);
  uint8_t n = logRecentN < LOG_RECENT ? logRecentN : LOG_RECENT;
  for (uint8_t i = 0; i < n; i++) {
    uint8_t slot = (uint8_t)((logRecentN - n + i) % LOG_RECENT);
    out += logRecent[slot];
    out += '\n';
  }
  portEXIT_CRITICAL(&logLineMux);
  server.send(200, "text/plain", out);
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
  ReqMark _req(REQ_SAVE_FLASH);
  saveSettings();     // persist settings + theme colours (NVS only, no LED access)
  flashReq = true;    // the display task performs the confirmation blink
  server.send(200, "text/plain", "Flash executed");
}

// Called by the UI on load to force all outputs off
void handleInitOff() {
  ReqMark _req(REQ_INIT_OFF);
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
    beatLatch240    = true;                // latched so an onset falling between
                                           // 240V steps is never lost
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

// ---------------------------------------------------------------------------
// LISTENING CHAINS - the audio engine MEASURES, it does not decide.
//
// One chain per distinct band in use. CHAIN 0 IS ALWAYS THE BASELINE BAND (the
// Frequency setting), so the 15/10 response is always available and is produced by
// exactly the arithmetic that produced it: the same six one-poles, in the same
// order, with the same coefficients and the same 10 ms frame boundary. Extra chains
// exist only because some pattern asked for a different band.
//
// Each chain PUBLISHES, once per 10 ms frame: its novelty (the frame-to-frame change
// in band energy) and a sequence number. Who acts on that is decided by the PATTERN,
// in the render loop, against its own threshold - see the listening step there.
// ---------------------------------------------------------------------------
#define ODF_CHAINS 4
struct OdfChain {
  bool  used = false;
  float hpHz = ODF_HP_HZ;      // this chain's band lower edge
  float aLo  = 0.0f;           // its high-pass coefficient (derived from hpHz)
  // Filter state: the same six one-poles as the baseline, per chain.
  float hp1 = 0, hp1Prev = 0, hp2 = 0, hp2Prev = 0;
  float lp1 = 0, lp2 = 0, lp3 = 0, lp4 = 0;
  // 10 ms frame accumulation
  float winMax = 0, prevWin = 0;
  // Published once per frame.
  volatile float    novelty = 0.0f;
  volatile uint32_t seq     = 0;
};
OdfChain odfChain[ODF_CHAINS];
uint16_t odfChainUse = 0;                 // bitmask of chains currently in use
float    odfAHi = 0.0f;                   // shared low-pass coefficient (250 Hz)

// Chain 0's numbers keep their original names: the heartbeat's "onset nov=… thr=…"
// line must go on meaning exactly what it always meant.
// (odfDisplayBeat is gone: a beat is per PATTERN now, so the pending state lives in
// each BeatListener - see listeningStep().)
float odfThr = 0.0f;            // logged, so the threshold can be read directly
float odfNovelty = 0.0f;        // chain 0's last novelty (the logged value)

// The high-pass coefficient for a band edge. THE SAME FORMULA as the baseline used
// per sample; it is simply hoisted to the 10 ms frame, because audioFs (and therefore
// the coefficient) only changes once per FFT window anyway.
float odfBandCoeff(float hpHz) {
  float dt = 1.0f / (audioFs > 1.0f ? audioFs : 1.0f);
  float rc = 1.0f / (2.0f * 3.14159265f * hpHz);
  return rc / (rc + dt);
}

// ---------------------------------------------------------------------------
// LISTENERS - the PATTERN decides what is a beat for it.
//
// One listener per port, plus one for the 240V controller. Each holds its own band
// (which chain it reads), its own threshold, and whether it takes the half-way
// beats. VU and EQ ports are NOT listeners: they are level-driven, so they cost
// nothing here - which is why the number of chains stays small.
//
// This is the step that is "recalculated for each playlist item in real time": when
// a timeline cell is adopted, rebuildListening() gives its port that cell's band,
// threshold and multiplier.
// ---------------------------------------------------------------------------
struct BeatListener {
  bool          active   = false;
  int           chain    = 0;
  float         thr      = 48.0f;
  int           mult     = 1;
  uint32_t      lastSeq  = 0;
  unsigned long lastReal = 0, prevReal = 0;
  bool          realPend = false;  unsigned long realAt = 0;
  bool          subPend  = false;  unsigned long subAt  = 0;
  unsigned long firePrev = 0, firePrevX = 0;   // interval logging
  uint32_t      taken    = 0;                  // instrument: beats acted on
};
BeatListener beatListen[NUM_PORTS + 1];        // [NUM_PORTS] = the 240V controller
uint16_t g_chainOverflow = 0;                  // instrument: did the 4-chain cap bind?
bool     beatFired240 = false;                 // the 240V controller acted this pass

// Find or allocate a chain for a band. Chain 0 IS the baseline band, so a request
// that matches it SHARES it - the normal case, and why the usual count is one chain.
int chainForBand(float hz) {
  hz = constrain(hz, 20.0f, 100.0f);
  if (fabsf(hz - odfChain[0].hpHz) < 0.5f) return 0;
  for (int c = 1; c < ODF_CHAINS; c++)
    if (odfChain[c].used && fabsf(hz - odfChain[c].hpHz) < 0.5f) return c;
  for (int c = 1; c < ODF_CHAINS; c++) {
    if (!odfChain[c].used) {
      // Coefficient and state FIRST, `used` LAST: the audio core must never see a
      // chain marked live before its coefficient is valid.
      odfChain[c].hpHz = hz;
      odfChain[c].aLo  = odfBandCoeff(hz);
      odfChain[c].hp1 = odfChain[c].hp1Prev = odfChain[c].hp2 = odfChain[c].hp2Prev = 0;
      odfChain[c].lp1 = odfChain[c].lp2 = odfChain[c].lp3 = odfChain[c].lp4 = 0;
      odfChain[c].winMax = odfChain[c].prevWin = 0;
      odfChain[c].used = true;
      return c;
    }
  }
  g_chainOverflow++;      // the cap bound - reported in the heartbeat if it ever does
  return 0;               // fall back to the baseline band rather than fail
}

// (Re)configure the chain set from the bands actually in use. CHAIN 0 IS ALWAYS the
// baseline band - the Frequency setting - which is what the 240V and any unchanged
// pattern listens to, so the 15/10 response is never unavailable.
void odfConfigureChains() {
  odfChain[0].used = true;
  odfChain[0].hpHz = constrain((float)freqThreshold, 20.0f, 100.0f);
  odfChain[0].aLo  = odfBandCoeff(odfChain[0].hpHz);
  for (int c = 1; c < ODF_CHAINS; c++) {
    if (odfChain[c].used) continue;        // an in-use extra chain keeps its band
    odfChain[c].hpHz = 0.0f;
    odfChain[c].aLo  = 0.0f;
  }
  odfChainUse = 1;                         // chain 0 is always live
  for (int c = 1; c < ODF_CHAINS; c++) if (odfChain[c].used) odfChainUse |= (1u << c);
  odfAHi = 0.0f;                           // recomputed at the next frame boundary
}

// Give every listener its band, threshold and multiplier FROM ITS OWN PATTERN.
void rebuildListening() {
  for (int c = 1; c < ODF_CHAINS; c++) odfChain[c].used = false;
  odfConfigureChains();

  for (int p = 0; p < NUM_PORTS; p++) {
    BeatListener &L = beatListen[p];
    LedPattern pat = (LedPattern)portCfg[p].pat;
    // Only patterns that CONSUME ONSETS are listeners at all - decided by the table above, never
    // here. The CHASE is a listener even with wrap OFF (its source is the level, but it takes
    // onsets as the kick TRIGGER).
    L.active = patternListensToBeats(portCfg[p].pat, portCfg[p].wrap);
    if (!L.active) continue;
    // THIS PATTERN'S OWN BAND. The three audio SOURCES are shared measurements, but the BAND of
    // the Bass Beat source is a per-pattern setting (the Frequency row), so a listener asks for the
    // chain matching its own band and shares it with any pattern using the same one.
    // (This was briefly `L.chain = 0`, from misreading "the music output must remain constant".
    // What must remain constant is the THREE SOURCES - see the table at the LedPattern enum - not
    // the per-pattern band. The Frequency row is a real setting and must not be ignored.)
    L.chain = chainForBand((float)portCfg[p].freqHz);
    // THE THRESHOLD HAS A FLOOR (user, 19 Oct: "wrap on is still a mess of flashing lights in
    // silence. it looks like its listening to noise rather than the bass beat"). At Bass
    // Sensitivity maximum the formula gives ZERO, and a zero threshold fires on any novelty above
    // zero - which in silence is the noise floor, so the metronome beat on nothing. Real band peaks
    // measure 5-40 counts, so a floor of 5 still passes every musical beat while ignoring the room.
    L.thr   = constrain((1.0f - constrain(portCfg[p].beatSens, 0.01f, 1.0f)) * 60.0f, 5.0f, 60.0f);
    L.mult  = (portCfg[p].mult > 1) ? 2 : 1;
    L.lastSeq = odfChain[L.chain].seq;      // never act on a STALE frame
  }

  // The 240V controller: ALWAYS a listener, on the baseline band, with the global
  // sensitivity - so it behaves exactly as it does today. Its own sensitivity and
  // frequency arrive when we repeat this work on the 240V page.
  BeatListener &R = beatListen[NUM_PORTS];
  R.active = true;
  R.chain  = 0;
  R.thr    = (1.0f - constrain(beatSens, 0.01f, 1.0f)) * 60.0f;
  R.mult   = (mult240V > 1) ? 2 : 1;
  R.lastSeq = odfChain[0].seq;

  // AND NOW PUBLISH WHAT THE LISTENERS ACTUALLY ASKED FOR.
  //
  // odfConfigureChains() at the TOP of this function runs BEFORE the listeners choose their chains,
  // so the chain mask it computes still says "chain 0 only". The audio core SKIPS any chain that is
  // not in odfChainUse - so a listener allocated chain 1 or higher read a chain that was never
  // processed, its novelty stayed 0 for ever, and that port heard NOTHING.
  //
  // On the page every port shares chain 0, so this could not show up there. It showed up in the
  // TIMELINE, where the cells' Frequency differs from the page's: "they play correctly in patterns,
  // but do not respond to music at all in the timeline".
  //
  // The call is safe to repeat: it leaves an in-use chain's band alone and just recomputes the
  // mask, so running it again here is exactly what was missing.
  odfConfigureChains();
}

// THE BEAT INSTRUMENTS FOR /diag. Defined here, with the state it reads, and called from the /diag
// handler above through a prototype - because a SHOW CANNOT BE LOGGED: logService() does not flush
// while a show runs and /tail is a DELTA endpoint, so during a timeline the only live view is this.
//   bOns/bActs/bFlips/bKicks  the counters since they were last reset by the log's beats line
//   bNov/bThr                 chain 0's novelty (ADC counts) and its threshold
//   p0thr                     port 0's own threshold - the per-PATTERN part
static void diagBeatFields(JsonDocument &doc) {
  doc["bOns"]    = (uint32_t)g_onsetReal;
  doc["bActs"]   = (uint32_t)g_beatReal;
  doc["bFlips"]  = (uint32_t)g_flipLED;
  doc["bKicks"]  = (uint32_t)g_kickLED;
  doc["bUse"]    = (uint32_t)odfChainUse;      // which chains are configured (1 = chain 0 only)
  doc["bNov"]    = (double)odfNovelty;         // chain 0's last novelty, in ADC counts
  doc["bThr"]    = (double)odfThr;             // chain 0's threshold
  doc["p0pat"]   = portCfg[0].pat;
  doc["p0sens"]  = (double)portCfg[0].beatSens;
  doc["p0act"]   = beatListen[0].active ? 1 : 0;
  doc["p0thr"]   = (double)beatListen[0].thr;
  doc["p0taken"] = (uint32_t)beatListen[0].taken;
  doc["p1thr"]   = (double)beatListen[1].thr;
  // WHAT EACH PORT IS ACTUALLY RUNNING, plus what the current row's cells are CALLED. These two
  // proved that a "Chase Green x2" item was playing as MOVING DOT: an unmapped item name resolves
  // to no pattern at all and renderBranchFor() degrades it, so no port was ever a Chase and
  // stepChase() never ran - bKicks stayed 0 while bOns climbed.
  doc["p1pat"]   = portCfg[1].pat;
  doc["p2pat"]   = portCfg[2].pat;
  doc["p3pat"]   = portCfg[3].pat;
  doc["c0id"]    = tlData[tlRow][1].id;
  doc["c1id"]    = tlData[tlRow][2].id;
  doc["c2id"]    = tlData[tlRow][3].id;
  // FRAME HITCH FIGURES, in microseconds, worst case since the last read. Read these right after a
  // timeline row change: fSeqUs names a row-change stall, fLedUs a transmit stall, and fLoopUs a
  // stall that hit the whole pass - which is what a FLASH ERASE looks like (it freezes both cores).
  doc["fLoopUs"] = g_maxLoopUs;
  doc["fSeqUs"]  = g_maxSeqUs;
  doc["fLedUs"]  = g_maxLedUs;
  doc["fHitch"]  = g_hitchCount;
  g_maxLoopUs = 0; g_maxSeqUs = 0; g_maxLedUs = 0; g_hitchCount = 0;
  // THE STALENESS PAIR. fStaleMs is the worst case in ms and fStaleP names the port. A large
  // fStaleMs with a small fLoopUs means the dot STOPPED rather than the CPU stalling - and if it
  // is the Moving Dot that stops, the erode/death/restart path is the place to look, not the CPU.
  doc["fStaleMs"] = g_staleMaxMs;
  doc["fStaleP"]  = (int32_t)g_stalePort;
  doc["fStaleDk"] = (int32_t)g_staleDark;   // 1 = that port was DARK: not a CPU stall at all
  doc["fStaleRw"] = (int32_t)g_staleRow;    // which row the worst stall happened in (1-based)
  // WHY A KICK DID NOT LAND: armed but blocked by the level, versus never armed.
  doc["kArmed"] = g_kickArmed;
  doc["kZero"]  = g_kickZero;
  doc["kMiss"]  = g_kickMiss;
  // PORT SYNC SNAPSHOT for ports 0-3: position, direction, chase accumulator (hundredths of a
  // pixel) and ms since that port last stepped. Two ports on the SAME entry should agree on all
  // four; comparing sA0 with sA1 and sG0 with sG1 shows a drift in the tenths of a pixel.
  doc["sP0"] = g_syncPos[0]; doc["sP1"] = g_syncPos[1]; doc["sP2"] = g_syncPos[2]; doc["sP3"] = g_syncPos[3];
  doc["sD0"] = g_syncDir[0]; doc["sD1"] = g_syncDir[1]; doc["sD2"] = g_syncDir[2]; doc["sD3"] = g_syncDir[3];
  doc["sA0"] = g_syncAcc[0]; doc["sA1"] = g_syncAcc[1]; doc["sA2"] = g_syncAcc[2]; doc["sA3"] = g_syncAcc[3];
  doc["sG0"] = g_syncAge[0]; doc["sG1"] = g_syncAge[1]; doc["sG2"] = g_syncAge[2]; doc["sG3"] = g_syncAge[3];
  // PAIR DIVERGENCE (port 0 vs port 1), worst since boot. A value of roughly the dot's SIZE means
  // one port took a kick the other did not; small values are just the fractional phase.
  doc["pairDiff"] = g_pairDiff;
  doc["pairA"]    = g_pairA;
  doc["pairB"]    = g_pairB;
  doc["pairRow"]  = g_pairRow;
  // THE PROOF PAIR. st0/st1 are the pixels each port has advanced: two ports fed identical
  // parameters MUST agree. v0xx/v1xx are the parameters that were actually APPLIED to each port, so
  // "the cells are identical" can be checked rather than assumed - the difference between two
  // identical cells should be nothing at all, and if a parameter differs the cause is the DATA, not
  // a code exception.
  doc["st0"] = g_steps[0]; doc["st1"] = g_steps[1];
  // FRAMES VISIBLE per port: st/fv is the real comparison. If the RATIOS match, the two ports are
  // behaving identically and any difference in the raw step count is time spent DARK (an empty
  // cell), which is data. If the ratios differ, the asymmetry is in the code.
  doc["fv0"] = g_framesVis[0]; doc["fv1"] = g_framesVis[1];
  doc["v0sp"] = (double)portCfg[0].speed;      doc["v1sp"] = (double)portCfg[1].speed;
  doc["v0sz"] = portCfg[0].size;               doc["v1sz"] = portCfg[1].size;
  doc["v0qt"] = portCfg[0].qty;                doc["v1qt"] = portCfg[1].qty;
  doc["v0wr"] = portCfg[0].wrap ? 1 : 0;       doc["v1wr"] = portCfg[1].wrap ? 1 : 0;
  doc["v0ef"] = portCfg[0].effect;             doc["v1ef"] = portCfg[1].effect;
  doc["v0ds"] = portCfg[0].distance;           doc["v1ds"] = portCfg[1].distance;
  doc["v0mo"] = (double)portCfg[0].momentum;   doc["v1mo"] = (double)portCfg[1].momentum;
  doc["v0bs"] = (double)portCfg[0].beatSens;   doc["v1bs"] = (double)portCfg[1].beatSens;
  doc["v0fq"] = portCfg[0].freqHz;             doc["v1fq"] = portCfg[1].freqHz;
  // ---- MOVING DOT TELEMETRY, EVERY PORT (user, 20 Oct) ---------------------------------------
  // The Moving Dot fault is INTERMITTENT: "the moving dots did not stall - I don't understand why
  // the fault was there before but not now". A hand-timed poll cannot catch it, so every port
  // reports on demand the numbers that decide it:
  //   pat   the pattern that port is REALLY running (a port running the Chase stops between kicks)
  //   sp    the live speed - the step interval is sp x 1000 ms, so 1.0 means one pixel a second
  //   sz/ef/ds/mo   the values the port is actually holding
  //   lost/dot      the EROSION STATE. mdLost is never cleared by the timeline, and dot is the
  //                 length actually DRAWN - dot=0 is an invisible dot, which looks exactly like a
  //                 stopped one, and the watchdog below flags it.
  //   dead  mdExpiredAt: 1 = it exploded and is waiting for its 3 s restart
  //   cell  the id of the cell that owns the port ("-" = the page owns it)
  //   gs    the pixels that port has ADVANCED - two readings apart answer "is it moving?"
  //   fre   how many times the motion restart has re-pinned this port
  for (int p = 0; p < NUM_PORTS; p++) {
    const TlItem &cc = tlData[tlRow][p + 1];
    char k[8];  snprintf(k, sizeof(k), "m%d", p + 1);
    char b[200];
    snprintf(b, sizeof(b),
             "pat=%d sp=%.4f sz=%d ef=%d ds=%d mo=%.2f lost=%d dot=%d dead=%d fre=%lu cell=%s gs=%lu pos=%d dir=%d dark=%d age=%lu",
             portCfg[p].pat, (double)portCfg[p].speed, portCfg[p].size, portCfg[p].effect,
             portCfg[p].distance, (double)portCfg[p].momentum,
             (int)mdLost[p], mdDotLen(p), (mdExpiredAt[p] != 0) ? 1 : 0,
             (unsigned long)g_framesRestart[p], cc.used ? cc.id : "-",
             (unsigned long)g_steps[p],
             ledPosArr[p], ledDirArr[p], portCfg[p].dark ? 1 : 0,
             (unsigned long)(millis() - g_lastMoveMs[p]));
    doc[k] = b;
  }
  doc["selPat"] = activePatternLED;
  doc["tlRowCell0"] = tlData[tlRow][11].used ? tlData[tlRow][11].id : "-";
  // Deliberately NOT reset: see the note where they are declared. A transition stall is over in a
  // second and a hand-timed poll cannot catch it, so they persist until the board is reset.
}

// The per-pattern ACTION - the only place a beat moves anything.
static void actForListener(int i) {
  if (i < NUM_PORTS) {
    // MOVING DOT AND KNIGHT RIDER: a beat REVERSES the dot, as it always has. The Chase is not
    // here at all - its beats go through actOnBeat, and its FOOTBALL is level-driven, not
    // beat-driven. No pattern shares another's effect.
    if (portCfg[i].pat == LP_MOVING_DOT || portCfg[i].pat == LP_KNIGHT_RIDER)
      ledDirArr[i] = -ledDirArr[i];
    // THE MOVING DOT EFFECT. The beat is the trigger: every one sheds debris, and every TENTH
    // also costs the dot a whole pixel. Knight Rider has no effect yet.
    if (portCfg[i].pat == LP_MOVING_DOT) mdShed(i);
    else if (portCfg[i].pat == LP_CENTER_OUT)
      ledExplArr[i] = 0;
    g_flipLED++;
  } else {
    if (activePattern240V == "moving_dot" || activePattern240V == "knight_rider")
      dotDirection = -dotDirection;
    beatFired240 = true;
    g_flip240++;
  }
}

// ONE PLACE decides what a beat DOES, because two rules cut across the patterns: the Chase
// is timed rather than flipped, and a minimum sensitivity means the music has no effect.
static void actOnBeat(int i, unsigned long now, bool fromAntifreeze, bool isSub) {
  if (i < NUM_PORTS && isChasePat(portCfg[i].pat)) {
    // A REAL beat times the metronome. A SUB-beat (the half-way one) is NOT a beat for this
    // purpose - it would reverse the traverse twice per beat - so it is ignored here.
    //
    // With wrap OFF there is NO beat action at all: the football is LEVEL-driven (the roll from
    // Speed plus the bass kicks), so the antifreeze no longer nudges it either - the roll already
    // keeps the display alive. The antifreeze still drives the wrap-ON metronome, where it stands
    // in for a beat that never came.
    if (portCfg[i].wrap && !isSub)              chaseBeat(i, now);
    else if (portCfg[i].wrap && fromAntifreeze) chaseBeat(i, now);
    // THE HYBRID, wrap OFF only: the hit is SHARED now - see g_chaseOnsetHit in the render loop.
    // Nothing is armed here any more, so no per-port state can be left behind or wiped unevenly.
    return;
  }
  if (!beatActionAllowed(i)) return;     // minimum sensitivity: the timer takes over
  actForListener(i);
}

// THE LISTENING STEP, once per render pass. Each listener compares its OWN chain's
// published novelty against its OWN threshold, with its own minimum gap, and then
// acts on its own pattern after the display offset.
static void listeningStep() {
  unsigned long now = millis();
  beatFired240 = false;
  bool anyReal = false;        // did ANY listener take a real beat this pass?

  // Antifreeze applies ONLY when Audio Detect is off: every `antifreeze` seconds the
  // pattern changes direction as if a beat had been heard. With Audio Detect ON it
  // must have no effect at all.
  static unsigned long lastAnti = 0;
  bool antiFire = false;
  if (!audioEnabled) {
    if (now - lastAnti >= (unsigned long)(antifreeze * 1000)) { lastAnti = now; antiFire = true; }
  } else {
    lastAnti = now;
  }

  for (int i = 0; i <= NUM_PORTS; i++) {
    BeatListener &L = beatListen[i];
    if (!L.active) continue;

    OdfChain &ch = odfChain[L.chain];
    if (ch.seq != L.lastSeq) {                 // one evaluation per 10 ms frame
      L.lastSeq = ch.seq;
      if (ch.novelty > L.thr && (now - L.lastReal) >= ODF_MIN_GAP) {
        anyReal = true;
        g_beatSeenAt = now;                     // the SHARED onset stamp (see g_chaseOnsetHit)
        L.prevReal = L.lastReal;
        L.lastReal = now;
        g_beatReal++;
        rawBeatFlag = true;                    // IO48: one flash per detection
        lastTrigMs  = now;
        // The 240V's own pattern uses the beat AT THE INSTANT it is heard, exactly as
        // it always has; every direction change still waits for the display offset.
        if (i == NUM_PORTS) beatLatch240 = true;
        if (!L.realPend) { L.realAt = now + beatOffsetMs; L.realPend = true; }
        // The half-way beat is armed only when THIS pattern takes it.
        L.subPend = false;
        if (L.mult > 1 && L.prevReal > 0) {
          unsigned long gap = now - L.prevReal;
          if (gap >= 240 && gap <= 1200) { L.subAt = now + beatOffsetMs + (gap / 2); L.subPend = true; }
        }
      }
    }

    bool real = false, sub = false;
    if (L.realPend && (long)(now - L.realAt) >= 0) { L.realPend = false; real = true; }
    if (L.subPend  && (long)(now - L.subAt)  >= 0) { L.subPend  = false; sub  = true; }

    if (real) {
      if (L.firePrev) { fireInt[fireIntIdx] = now - L.firePrev; fireIntIdx = (fireIntIdx + 1) & 7; }
      L.firePrev = now;
      actOnBeat(i, now, false, false);
      L.taken++;
    } else if (sub) {
      g_beatExtra++;
      if (L.firePrevX) { fireIntX[fireIntIdxX] = now - L.firePrevX; fireIntIdxX = (fireIntIdxX + 1) & 7; }
      L.firePrevX = now;
      actOnBeat(i, now, false, true);
      L.taken++;
    } else if (antiFire) {
      actOnBeat(i, now, true, false);
      L.taken++;
    }
  }

  if (anyReal) g_onsetReal++;
}

// Peak-picking, every 10 ms. A LOCAL mean (not a global one) adapts to how dense
// the music is, and the delta stops a quiet passage triggering on its own noise.
// Publication, once per 10 ms frame, plus - TEMPORARILY - the old decision so the
// display keeps working while the refactor is proved in stages. The decision moves
// out to the pattern in the next step.
static void odfEvaluate() {
  // Coefficients refreshed once per frame: audioFs only changes once per FFT window,
  // so this is the same value the per-sample form would have produced.
  odfAHi          = (1.0f / (audioFs > 1.0f ? audioFs : 1.0f)) /
                    ((1.0f / (2.0f * 3.14159265f * ODF_LP_HZ)) + (1.0f / (audioFs > 1.0f ? audioFs : 1.0f)));
  odfChain[0].hpHz = constrain((float)freqThreshold, 20.0f, 100.0f);
  odfChain[0].aLo  = odfBandCoeff(odfChain[0].hpHz);

  // Every chain publishes its novelty and bumps its sequence number. Chain 0's value
  // keeps the original global name so the log is directly comparable with the
  // baseline's.
  envP2P = beatMax - beatMin;
  beatMax = 0;
  beatMin = 4095;
  for (int c = 0; c < ODF_CHAINS; c++) {
    if (!(odfChainUse & (1u << c))) continue;
    OdfChain &ch = odfChain[c];
    float flux = ch.winMax - ch.prevWin;
    ch.prevWin = ch.winMax;
    ch.winMax  = 0.0f;
    ch.novelty = (flux > 0.0f) ? flux : 0.0f;
    ch.seq++;
    if (c == 0) odfNovelty = ch.novelty;
  }

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

  // NOTHING IS DECIDED HERE ANY MORE. This function MEASURES and PUBLISHES; the
  // PATTERN decides, in listeningStep() on the render core. That is the whole point
  // of the change: one measurement, ten independent listeners.
}

// Per-sample processing, shared by both acquisition paths so they cannot drift.
static inline void audioProcessSample(float s, int &onsetSub, int stride) {
  // ---- ONE PASS PER LISTENING CHAIN ----------------------------------------
  // Chain 0 is the baseline band and runs the IDENTICAL arithmetic the single chain
  // used to run: the same six one-poles, in the same order, with the same
  // coefficients. An extra chain differs only in its band edge (its own aLo) and in
  // having its own filter state - which is what lets two patterns listen to
  // different bands at the same time without either one moving.
  for (int c = 0; c < ODF_CHAINS; c++) {
    if (!(odfChainUse & (1u << c))) continue;
    OdfChain &ch = odfChain[c];

    // 2-pole high-pass at this chain's band edge: removes rumble and DC. Two
    // cascaded one-poles - cheap, stable, and steep enough to reject the room.
    ch.hp1 = ch.aLo * (ch.hp1 + s - ch.hp1Prev);       ch.hp1Prev = s;
    ch.hp2 = ch.aLo * (ch.hp2 + ch.hp1 - ch.hp2Prev);  ch.hp2Prev = ch.hp1;

    // 4-pole low-pass at 250 Hz: keeps the kick, rejects snare, hats and vocals.
    // Two poles were not enough - at 12 dB/octave they leaked through and swamped
    // the onset function whenever the mix got busy.
    ch.lp1 += odfAHi * (ch.hp2 - ch.lp1);
    ch.lp2 += odfAHi * (ch.lp1 - ch.lp2);
    ch.lp3 += odfAHi * (ch.lp2 - ch.lp3);
    ch.lp4 += odfAHi * (ch.lp3 - ch.lp4);

    float bpRect = fabsf(ch.lp4);
    if (bpRect > ch.winMax) ch.winMax = bpRect;   // peak within this 10 ms window

    // Chain 0 ALSO feeds the shared level readings, so the heartbeat's numbers stay
    // directly comparable with the baseline's.
    if (c == 0) {
      float v = bpRect;
      if (v > envMax) envMax = v;
      if (v < envMin) envMin = v;
      if (v > (float)beatMax) beatMax = (int)v;
      if (v < (float)beatMin) beatMin = (int)v;
    }
  }

  // ---- frame boundary, every 10 ms, exactly where the baseline had it --------
  if (++beatCount >= beatStride) {
    beatCount = 0;
    odfEvaluate();     // publishes each chain's novelty + sequence number
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
  float bass = 0, mid = 0, treb = 0, all = 0, blvl = 0;
  // Ten LOG-spaced EQ bands. Equal widths in Hz would put nine bars in the bass and
  // one across everything else, which is not what a spectrum looks like. This table
  // is each band's UPPER edge in Hz (40 Hz to 5 kHz).
  static const float eqEdge[EQ_BANDS] = { 60, 90, 140, 210, 320, 480, 750, 1200, 2200, 5000 };
  float eqSum[EQ_BANDS] = {0};
  for (int b = 1; b < FFT_SAMPLES / 2; b++) {
    float hz = (float)b * binHz;
    float m  = fftReal[b];
    all += m;
    if (hz >= bassBottom && hz <= bassTop)      bass += m;
    else if (hz > bassTop && hz <= FFT_MID_MAX) mid  += m;
    else if (hz > FFT_MID_MAX)                  treb += m;
    // BASS LEVEL (61-500 Hz). A SEPARATE accumulator, deliberately not part of the chain
    // above, so the bass/mid/treble bands and everything that reads them are untouched.
    if (hz > bassTop && hz <= FFT_BASSLEVEL_MAX) blvl += m;
    for (int e = 0; e < EQ_BANDS; e++) {
      if (hz <= eqEdge[e]) { eqSum[e] += m; break; }
    }
  }
  rawBass = bass; rawMid = mid; rawTreb = treb; rawAll = all;

  float dbAll  = 20.0f * log10f(all  + 1e-6f);
  float dbBass = 20.0f * log10f(bass + 1e-6f);
  float dbMid  = 20.0f * log10f(mid  + 1e-6f);
  float dbTreb = 20.0f * log10f(treb + 1e-6f);
  float dbBassLvl = 20.0f * log10f(blvl + 1e-6f);

  audioLevel  = constrain((dbAll  - trackFloor(bandFloorAll,    dbAll )) / 24.0f, 0.0f, 1.0f);
  audioBass   = constrain((dbBass - trackFloor(bandFloorBass,   dbBass)) / 24.0f, 0.0f, 1.0f);
  audioMid    = constrain((dbMid  - trackFloor(bandFloorMid,    dbMid )) / 24.0f, 0.0f, 1.0f);
  audioTreble = constrain((dbTreb - trackFloor(bandFloorTreble, dbTreb)) / 24.0f, 0.0f, 1.0f);
  // BASS LEVEL: the same dB + tracked-floor normalisation as every other level, so a quiet
  // room reads 0 and music reads 0-1 without a separate calibration.
  audioBassLvl = constrain((dbBassLvl - trackFloor(bandFloorBassLvl, dbBassLvl)) / 24.0f, 0.0f, 1.0f);

  // THE BASS LEVEL ENVELOPE, smoothed HERE so it exists whether or not the display is running.
  // One FFT window is ~59 ms, so the attack cannot be finer than that: 80 ms attack, 250 ms
  // release. Every level-driven pattern (the Chase's football) reads this, never the raw figure.
  {
    float winSec = (float)FFT_SAMPLES / (audioFs > 1.0f ? audioFs : 8000.0f);
    if (!(winSec > 0.001f) || winSec > 0.2f) winSec = 0.059f;
    float src = audioBassLvl;
    if (!(src >= 0.0f && src <= 1.0f)) src = 0.0f;                 // NaN guard
    // 150 ms release: long enough to be stable, short enough that two fast bass notes are still
    // two hits. At 250 ms drum-and-bass passages merged into one and beats were missed.
    float tau = (src > g_bassLvlEnv) ? 0.080f : 0.150f;
    g_bassLvlEnv += (src - g_bassLvlEnv) * (1.0f - expf(-winSec / tau));
    if (!(g_bassLvlEnv >= 0.0f && g_bassLvlEnv <= 1.0f)) g_bassLvlEnv = 0.0f;
    if (g_bassLvlEnv > g_bassPeak) g_bassPeak = g_bassLvlEnv;   // read and reset by /diag
    // THE SLOW PEAK the Chase's gate is derived from: follow a rise immediately, fall over ~2.5 s.
    if (g_bassLvlEnv > g_bassSlow) g_bassSlow = g_bassLvlEnv;
    else g_bassSlow += (g_bassLvlEnv - g_bassSlow) * (1.0f - expf(-winSec / 2.5f));
    if (!(g_bassSlow >= 0.0f && g_bassSlow <= 1.0f)) g_bassSlow = 0.0f;
  }

  // The ten EQ bands: same dB + tracked-floor normalisation as the bands above.
  for (int e = 0; e < EQ_BANDS; e++) {
    float dbEq = 20.0f * log10f(eqSum[e] + 1e-6f);
    audioEqBand[e] = constrain((dbEq - trackFloor(eqFloor[e], dbEq)) / 24.0f, 0.0f, 1.0f);
  }

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
    // 240V VU: its own Music Source control (BASS / FULL RANGE) still selects the
    // source, so this behaviour is deliberately unchanged.
    int level = constrain(computeVuLevel(vuSmooth240V, attack240V, decay240V, musicFullMode), 0, 10);
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

LedPattern patternEnumFromName(const String& nameIn) {
  // NORMALISED, so a display name can never silently degrade to Moving Dot. A space
  // where an underscore is expected was all it took: "VU Meter" resolved to nothing
  // and every VU selection rendered as a moving dot. Trim, lowercase and treat
  // spaces as underscores, then match.
  String name = nameIn;
  name.trim();
  name.toLowerCase();
  name.replace(' ', '_');

  // "Chase" is the ONE chase now. chase_10_1 is matched only so an old saved item still
  // resolves - and it resolves to the SAME pattern, because the reverse chase no longer
  // exists as a thing of its own.
  if (name == "chase" || name == "chase_1_10" || name == "chase_10_1") return LP_CHASE_1_10;
  if (name == "center_out_chase" || name == "explosion") return LP_CENTER_OUT;
  if (name == "knight_rider")     return LP_KNIGHT_RIDER;
  if (name == "vu_meter" || name == "vu_left")           return LP_VU_METER;
  if (name == "vu_meter_rev" || name == "vu_right")      return LP_VU_REV;
  if (name == "vu_dual" || name == "vu_ends")            return LP_VU_DUAL;
  if (name == "vu_center_out" || name == "vu_centre")    return LP_VU_CENTER;
  if (name == "eq" || name == "graphic_eq")              return LP_EQ;
  return LP_MOVING_DOT;
}

// Pattern id + the VU direction row -> the render branch actually used.
//
// BOTH the pattern page and the timeline go through this, so a timeline cell that
// holds a VU item behaves the same as selecting it on the page.
LedPattern renderBranchFor(String id) {
  LedPattern pat = patternEnumFromName(id);
  if (pat == LP_VU_METER) {
    if (vuDirLED == "RIGHT")       return LP_VU_REV;
    if (vuDirLED == "CENTRE")      return LP_VU_CENTER;
    if (vuDirLED == "ENDS")        return LP_VU_DUAL;
  }
  return pat;
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

// Resolve each port's OWN stored colour REFERENCES against the LIVE palette, keeping
// the reference alongside the value.
//
// Per PORT, deliberately: a timeline cell can hold its own references, so a theme
// edit must repaint those too - and must NOT reset them to the page's colours.
void applyColourRefsToPorts() {
  for (int p = 0; p < NUM_PORTS; p++) {
    for (int i = 0; i < 4; i++) {
      CRGB v = resolveColourRef(portCfg[p].colourRef[i]);
      portCfg[p].colour[i] = ((uint32_t)v.r << 16) | ((uint32_t)v.g << 8) | v.b;
    }
  }
}

// (applyGlobalsToPorts() USED TO LIVE HERE, and stamped the LED page's settings onto
// all ten ports. That was the fault behind "changing a parameter disrupts the
// timeline": it overwrote EVERY port, and only the ports with a cell in the CURRENT
// row were put back afterwards. Every port is now derived from its own source of
// truth by applyTimelineRow(), which is the single place a port's config is built.)

// The colour for ONE LED in one colour slot: the slot's fixed colour, or - if the
// slot holds a rainbow - the rainbow at THAT LED's position along the strip.
//
// Taking the POSITION rather than the zone is what makes four rainbow body
// sections render as ONE continuous rainbow instead of four restarts, and what
// gives a rainbow dot its colour from where it currently is.
//
// Takes plain arrays rather than a PortCfg reference on purpose: the Arduino IDE
// inserts auto-generated prototypes ABOVE the type declarations, so a signature
// mentioning PortCfg cannot be parsed there and fails the whole build.
CRGB slotColourAt(const uint32_t *ref, const uint32_t *val, int slot, int idx) {
  if (slot < 0) slot = 0;
  if (slot > 3) slot = 3;
  if (refIsRainbow(ref[slot])) return rainbowAt(idx, NUM_LEDS_PER_STRIP);
  if (refIsTheme(ref[slot]))   return previewColors[refThemeIndex(ref[slot])];  // live
  return CRGB(val[slot] & 0x00FFFFFFUL);
}

// Colour for a VU meter LED at absolute index `idx`, from a 4-entry palette of
// REFERENCES. The four bands are FIXED along the strip (the top quarter is
// always the peak colour), which is the classic VU bar look - a red LED means
// "near full scale", regardless of how much is lit.
CRGB vuBandColor(const uint32_t *ref, const uint32_t *val, int idx) {
  if (idx < 0 || idx >= NUM_LEDS_PER_STRIP) return CRGB::Black;
  int band = (idx * 4) / NUM_LEDS_PER_STRIP;
  if (band > 3) band = 3;
  return slotColourAt(ref, val, band, idx);
}

// Draw the current frame of ONE strip using its own pattern + parameters
void drawPortPattern(int port) {
  const int N = NUM_LEDS_PER_STRIP;
  ledDirty[port] = true;              // this strip's data changed; it must be copied
  const int lc = (N / 2) - 1;
  const int rc = N / 2;
  const PortCfg &c = portCfg[port];
  // NOTE: there is deliberately no single `mainCol` here any more. Slot 0 is
  // usually one fixed colour, but it can also be a rainbow, which has no single
  // value - so every pixel asks slotColourAt() with its OWN index instead.

  for (int i = 0; i < N; i++) leds[port][i] = CRGB::Black;

  int dotLen = constrain(c.size, 1, 10);
  int effQty = constrain(c.qty, 1, 8);

  if (c.pat == LP_CENTER_OUT) {
    int off = ledExplArr[port];
    for (int s = 0; s < dotLen; s++) {
      int l = lc - off - s, r = rc + off + s;
      setLed(port, l, slotColourAt(c.colourRef, c.colour, 0, l));
      setLed(port, r, slotColourAt(c.colourRef, c.colour, 0, r));
    }
  }
  else if (isVuPat(c.pat)) {
    int level = constrain(computeVuLevel(vuSmoothLED, attackLED, decayLED, false), 0, 10);
    int lit = level * (N / 10);
    if (c.pat == LP_VU_METER) {                       // left -> right
      for (int i = 0; i < lit; i++) setLed(port, i, vuBandColor(c.colourRef, c.colour, i));
    } else if (c.pat == LP_VU_REV) {                  // right -> left
      for (int i = 0; i < lit; i++) setLed(port, N - 1 - i, vuBandColor(c.colourRef, c.colour, N - 1 - i));
    } else if (c.pat == LP_VU_DUAL) {                 // both ends inward
      for (int i = 0; i < lit / 2; i++) {
        setLed(port, i, vuBandColor(c.colourRef, c.colour, i));
        setLed(port, N - 1 - i, vuBandColor(c.colourRef, c.colour, N - 1 - i));
      }
    } else {                                          // centre -> outward
      int half = lit / 2;
      for (int i = 0; i < half; i++) {
        setLed(port, lc - i, vuBandColor(c.colourRef, c.colour, lc - i));
        setLed(port, rc + i, vuBandColor(c.colourRef, c.colour, rc + i));
      }
    }
  }
  else if (c.pat == LP_EQ) {
    // Graphic EQ: TEN bands, ONE PER STRIP. In the STAR layout the ten strips are
    // ten equal rays, so each ray carries one band and the ten together are the
    // spectrum; the bar simply grows along the strip.
    //
    // FIRST CUT, deliberately: the bar is drawn in the pattern's colour. Colour by
    // height, the smooth<->stepped blend and the 100-band horizontal variant are
    // still to be designed, as is the Max Peak marker (its rows are present and
    // stored, but nothing reads them yet).
    int band = port;                       // strip index = band index
    if (band >= EQ_BANDS) band = EQ_BANDS - 1;
    int lit = eqBar[band] * (N / EQ_MAX);
    if (lit > N) lit = N;
    for (int i = 0; i < lit; i++)
      setLed(port, i, slotColourAt(c.colourRef, c.colour, 0, i));
  }
  else {
    // moving_dot / chase_1_10 / chase_10_1 / knight_rider
    int pos = ledPosArr[port];
    // THE EFFECT'S DEBRIS, drawn FIRST so the dot always wins where they overlap: one pixel of
    // light each, in the dot's OWN colour sampled at that pixel, only dimmer.
    for (int k = 0; k < MD_FRAGS; k++) {
      MdFrag& f = mdFrags[port][k];
      if (!f.alive) continue;
      int ipos = (int)(f.posf + 0.5f);
      if (ipos < 0 || ipos >= N) continue;            // just off the end
      CRGB dc = slotColourAt(c.colourRef, c.colour, 0, ipos);
      dc.nscale8((uint8_t)(constrain(f.bright, 0.0f, 1.0f) * 255.0f));
      setLed(port, ipos, dc);
    }
    // The dot's length WITH the effect's erosion. 0 means fully erased: it stays dark, and
    // Size is only ever its STARTING length.
    int dlen = mdDotLen(port);
    for (int q = 0; q < effQty; q++) {
      int base = (pos + (q * N) / effQty) % N;
      for (int s = 0; s < dlen; s++) {
        int idx = (base + s) % N;
        setLed(port, idx, slotColourAt(c.colourRef, c.colour, 0, idx));
      }
    }
    if (c.pat == LP_KNIGHT_RIDER) {
      // Tail is a dimmed version of the port's own colour, so the trail always
      // matches the dot rather than being hardcoded red - and a rainbow dot now
      // leaves a rainbow trail, dimmed in place.
      for (int t = 1; t <= 6; t++) {
        int idx = (pos - ledDirArr[port] * t + N) % N;
        CRGB tail = slotColourAt(c.colourRef, c.colour, 0, idx);
        tail.nscale8(50);
        setLed(port, idx, tail);
      }
    }
  }

  if (c.invert) {   // on -> off, off -> on
    for (int i = 0; i < N; i++) {
      CRGB col = leds[port][i];
      if (col.r == 0 && col.g == 0 && col.b == 0)
        leds[port][i] = applyCalibrationOrder(slotColourAt(c.colourRef, c.colour, 0, i), portColorOrders[port]);
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
  if (isLevelPat(c.pat)) return;   // level-driven (the VU meters and the EQ)

  int dotLen = constrain(c.size, 1, 10);
  if (ledDirArr[port] == 0) ledDirArr[port] = 1;   // never allow a zero direction
  ledPosArr[port] += ledDirArr[port];
  int maxPos = N - dotLen;
  if (maxPos < 0) maxPos = 0;

  if (ledPosArr[port] > maxPos) {
    if (c.wrap) { ledPosArr[port] = maxPos; ledDirArr[port] = -ledDirArr[port]; }
    // ORIENTATION swaps which end the dot restarts at, so a dot running toward pixel 0 reappears
    // at the far end rather than at 0.
    else ledPosArr[port] = c.orient ? maxPos : 0;
  } else if (ledPosArr[port] < 0) {
    if (c.wrap) { ledPosArr[port] = 0; ledDirArr[port] = -ledDirArr[port]; }
    else ledPosArr[port] = c.orient ? 0 : maxPos;
  }
}

// A CHASE TRIGGER - a real beat, a half-way beat, or an antifreeze pulse. This is the only
// place a chase moves because of an EVENT; the level-driven movement is in stepChase().
static void chaseBeat(int p, unsigned long now) {
  const int maxPos = chaseMaxPos(p);
  if (portCfg[p].wrap) {
    // METRONOME. One reversal per beat, and the dot travels to the far end over the gap
    // measured to the previous beat, so it lands as the next one arrives.
    unsigned long gap = (chaseTrigAt[p] == 0) ? 0 : (now - chaseTrigAt[p]);
    // DEBOUNCE. An extra onset part-way through a traverse used to reverse it, so the dot
    // went back to the end it had come from - the "many times it moves back the same way"
    // report. A trigger arriving before 60% of the last measured beat is treated as noise
    // and ignored, and an ignored trigger does NOT touch the timing.
    unsigned long minGap = chaseInterval[p] ? (chaseInterval[p] * 6 / 10) : 150;
    if (chaseTrigAt[p] != 0 && gap < minGap) return;
    if (gap >= 150 && gap <= 4000) chaseInterval[p] = gap;
    if (chaseInterval[p] == 0)     chaseInterval[p] = 500;
    chaseTrigAt[p]  = now;
    chaseFromPos[p] = ledPosArr[p];
    // AIM FOR THE FAR END FROM WHEREVER THE DOT IS. Blindly alternating would send it back
    // the way it came if an extra beat slipped through; aiming at the opposite end cannot.
    ledDirArr[p] = (ledPosArr[p] > (maxPos / 2)) ? -1 : 1;
  } else {
    // NOT REACHED any more for a wrap-OFF chase: the football in stepChase() is level-driven, so
    // no beat or antifreeze trigger calls this branch on a wrap-OFF dot. It is left in place
    // because chaseBeat() is also the entry point for the wrap-ON metronome.
    ledPosArr[p] += (int)chaseSpeed(p);
    if (ledPosArr[p] > maxPos) ledPosArr[p] = 0;   // wrap OFF: restart at the start
  }
}

// The per-frame chase update. Returns TRUE only when the dot MOVED, so a still chase draws
// and transmits nothing.
static bool stepChase(int p, unsigned long now, unsigned long dtMs) {
  const PortCfg &c = portCfg[p];
  int maxPos = chaseMaxPos(p);
  int was = ledPosArr[p];

  if (c.wrap) {
    // ONE TRAVERSE PER BEAT. With no beat for `antifreeze` seconds the traverse is re-armed
    // anyway - which is what keeps a silent show bouncing, exactly like a beat would.
    if (chaseTrigAt[p] == 0 ||
        (now - chaseTrigAt[p]) > (unsigned long)(antifreeze * 1000)) chaseBeat(p, now);
    unsigned long iv = chaseInterval[p] ? chaseInterval[p] : 500;
    unsigned long el = now - chaseTrigAt[p];
    if (el > iv) el = iv;
    int from = chaseFromPos[p];
    int to   = (ledDirArr[p] > 0) ? maxPos : 0;
    int np   = from + (int)(((long)(to - from) * (long)el) / (long)iv);
    ledPosArr[p] = (np < 0 || np > maxPos) ? 0 : np;      // clamped: see the note below
  } else {
    // BASS BEAT DRIVES THE DOT (user, 20 Oct): every detected BASS BEAT advances the dot by DISTANCE
    // pixels, covered in the KICK SPEED's time, and then the dot STOPS and waits for the next beat.
    // One move per beat, always the same size, however loud or quiet that beat was.
    //
    // THE BASS LEVEL NO LONGER DECIDES ANYTHING ABOUT THE MOTION. It used to set both how far a
    // kick went (Distance x the level's strength) and how fast it went (a speed blended from the
    // level) - so a quiet beat barely moved the dot and a loud one flung it. That is the behaviour
    // the user identified as the problem. The level is still measured and still logged, so a level
    // that never rises remains diagnosable; it is simply not a motion input any more.
    //
    // Minimum beat sensitivity (or Audio Detect OFF) means the music has NO effect: the dot does not
    // move at all - the same rule every other pattern follows. Sensitivity also sets this port's
    // ONSET THRESHOLD (see rebuildListening), so it still decides HOW EASILY a beat counts.
    float dtSec = (float)dtMs / 1000.0f;
    // THE QUIET SPEED (user, 20 Oct: "The speed should not be greyed in chase wrap off. It should be
    // active and set the quiet speed so the dot still moves at that rate when there is no music").
    // Speed is the ROLL - a slow creep, 1-10 seconds per pixel - and it is what the dot does when
    // nothing is being detected: in silence the strip keeps drifting instead of looking dead, and
    // between beats it adds only a fraction of a pixel, so a kick is still Distance pixels.
    float roll  = chaseSpeed(p);

    // BASS LEVEL (61-500 Hz), smoothed by the envelope follower in the render loop, then GATED.
    // DIAGNOSTIC ONLY since 20 Oct - it grants and scales no travel. The gate, n and chaseNlast
    // exist for the /diag log and for nothing else.
    float gate = 0.0f;
    float n    = 0.0f;                 // the level AFTER the gate and the reference, 0..1
    float env  = g_bassLvlEnv;         // the smoothed Bass Level, before anything else
    if (!(env >= 0.0f && env <= 1.0f)) env = 0.0f;                 // NaN guard
    // The sensitivity is needed OUTSIDE the gate block: it also sets what counts as a HIT.
    float sens = constrain(c.beatSens, 0.01f, 1.0f);
    if (!sensAtMin(c.beatSens) && !sensAtMin(beatSens) && audioEnabled) {
      // THE GATE IS AUTOMATIC, DERIVED FROM THE MUSIC (user, 19 Oct: "Bass Level set to max - all
      // I see is a slow moving dot"). A HAND-SET gate cannot work: its maximum asked for 0.46
      // while this music peaks at 0.38, so nothing could kick; at the other end it sat inside the
      // room noise. The absolute scale of the band is simply not something a slider can express.
      //
      //   g_bassSlow  a slow peak of the envelope (rises at once, falls over ~2.5 s)
      //   gate        a fraction of that peak, so it always sits INSIDE the music
      //   ref         the peak itself, so a hit at full height is a FULL kick
      //
      // The slider is now a SENSITIVITY, which is what the user called it: higher = a lower gate
      // = responds to quieter hits. It cannot be set to block everything, because the gate is
      // always a fraction of what the music is doing.
      float peak = g_bassSlow;
      if (!(peak >= 0.0f && peak <= 1.0f)) peak = 0.0f;            // NaN guard
      if (peak < 0.16f) peak = 0.16f;       // the room's own idle level measures ~0.15
      gate = peak * (1.0f - 0.75f * sens);  // sens 1 -> 25% of peak; sens 0.2 -> 85%
      if (gate < 0.16f) gate = 0.16f;
      float ref = peak;
      if (ref < gate + KICK_MIN_SPAN) ref = gate + KICK_MIN_SPAN;
      // LINEAR above the gate (user's choice). No sqrt: that curve lifted the noise floor too.
      if (env > gate) {
        n = (env - gate) / (ref - gate);
        if (!(n >= 0.0f && n <= 1.0f)) n = 0.0f;
      }
    }

    // DISTANCE IS THE PIXELS, KICK SPEED IS THE TIME (user, 20 Oct, correcting the change made just
    // before): "when the size is set to 10 and the distance is set to 10 the dot moves swiftly at
    // the kick speed 0.02 to 0.1 to the next position defined by the distance and waits there until
    // the next beat occurs." So:
    //   DISTANCE    how many pixels the dot advances on a beat (1-50).
    //   KICK SPEED  how long that move takes, as the Effects row relabelled "Kick Speed" on the
    //               Chase. 1-10 INVERTED: 1 is the SLOWEST at 1.0s and 10 the fastest at 0.1s (user,
    //               20 Oct: "changed from 0.02 - 0.1 to 0.1 to 1.0 (yes that is inverted)").
    //               0 means NO KICK AT ALL - the Effect row's own rule, minimum = the effect is off,
    //               which is why the page greys Distance and Momentum at 0.
    //   then it STOPS and waits for the next beat. There is no roll any more.
    float dm      = (float)constrain(portCfg[p].distance, 1, 50);
    int   eff     = constrain(portCfg[p].effect, 0, 10);
    float kickSec  = (eff <= 0) ? 0.0f : ((11.0f - (float)eff) / 10.0f);
    float kickRate = (kickSec > 0.0f) ? (dm / kickSec) : 0.0f;      // px/s: the traverse
    // (Momentum is applied where the glide is set up, from the kick's pixels - see below.)

    // THE LEVEL'S OWN RISE IS NO LONGER A KICK (user, 20 Oct). It used to be a fallback trigger, so
    // a rising level could move the dot without any detected beat - the level driving the motion by
    // the back door. It is measured for the log and nothing else now: the BASS BEAT alone triggers
    // travel. Whether a beat is detected at all is set by this port's ONSET THRESHOLD, which Bass
    // Sensitivity drives (see rebuildListening), so the slider still decides how easily it counts.
    chaseLvlLast[p] = env;
    chaseNlast[p]   = n;

    // A HIT IS THE BASS BEAT - the SHARED onset, decided once per frame and never per port, so two
    // identical cells always take exactly the same beats. One beat = one kick, and it is the same
    // detector the wrap-ON metronome uses, so the two can never disagree about when music beats.
    bool hit = g_chaseOnsetHit;
    if (!(chaseCredit[p] >= 0.0f && chaseCredit[p] <= 1000.0f)) chaseCredit[p] = 0.0f;
    if (!(chaseCoast[p] >= 0.0f && chaseCoast[p] <= 1000.0f)) chaseCoast[p] = 0.0f;
    // ONE MOVE AT A TIME, AND ALWAYS EXACTLY DISTANCE PIXELS (user, 20 Oct: "all I get is a few
    // moves that look like they are responding to the beat and then it moves a large distance in one
    // go. That is incorrect but probably triggered by the music"). The credit used to ACCUMULATE, so
    // several onsets landing inside one move stacked up to four times Distance and the dot shot off
    // - exactly the jump the user saw, and exactly what the music could trigger with a bass run.
    // A beat that lands while the dot is still moving is now IGNORED: one beat, one move of
    // Distance, never more. Beats arriving faster than the move simply move the dot less often,
    // which is the honest reading of "a set distance for each beat".
    // A BEAT IS NEVER DROPPED, ONLY DEFERRED (user, 20 Oct: "I still get missed beats and then a
    // large movement all at once ... I cannot get the dot to get kicked down the strip by the same
    // amount each beat"). The previous rule IGNORED a beat that landed mid-move - that was the
    // missed beats. It is REMEMBERED now (one pending move, never a count) and starts the moment the
    // dot is still, so every beat still delivers exactly Distance - and no run of beats can stack
    // into a longer move, which was the large one.
    if (hit && kickRate > 0.0f) {
      if (chaseCredit[p] <= 0.0f) { chaseCredit[p] = dm; chaseCoast[p] = 0.0f; g_kickLED++; }
      else                        { chasePending[p] = true;  g_kickZero++; }   // remembered
    }
    // THE DEFERRED MOVE STARTS THE MOMENT THE DOT STOPS.
    if (chasePending[p] && chaseCredit[p] <= 0.0f && kickRate > 0.0f) {
      chaseCredit[p] = dm; chasePending[p] = false; chaseCoast[p] = 0.0f; g_kickLED++;
    }
    if (chaseCredit[p] > dm) chaseCredit[p] = dm;   // a stale credit can never cause a jump

    float rate = roll;
    if (chaseCredit[p] > 0.0f && kickRate > 0.0f) {
      // THE KICK ITSELF, at the KICK SPEED's rate: the credit is Distance pixels and the rate is
      // Distance divided by the Kick Speed's time, so the dot covers exactly Distance pixels in
      // that time and then stops. Neither number involves the level.
      rate = kickRate;
      float spend = (rate - roll) * dtSec;                         // pixels the kick pays for
      if (spend > chaseCredit[p]) {                                // the credit runs out mid-frame
        float frac = (spend > 0.0f) ? (chaseCredit[p] / spend) : 0.0f;
        rate  = roll + (rate - roll) * frac;
        spend = chaseCredit[p];
      }
      chaseCredit[p] -= spend;
      // THE GLIDE. MOMENTUM SETS HOW FAR IT RUNS PAST THE KICK, measured in the kick's own pixels:
      // 0 = no glide, so the dot stops dead at exactly Distance, and the maximum carries it
      // 3 x Distance beyond. It starts at the KICK SPEED's rate, so the dot eases down instead of
      // braking. This is the ONLY travel beyond Distance, and it is what Momentum is for.
      float coastV   = rate - roll;
      // MOMENTUM IS 0-0.1 ON THE CHASE (Wrap OFF) AND 0-0.5 EVERYWHERE ELSE (user, 20 Oct). The PAGE
      // sets that short range for this pattern alone, so the multiplier stays 6 and the formula is
      // untouched: 0.1 gives the same 0.6 x Distance it always did. The clamp guards against a value
      // saved under a different pattern; the Chase's own slider cannot exceed 0.1.
      float glideCap = dm * 6.0f * constrain(portCfg[p].momentum, 0.0f, 0.1f);
      chaseCoast[p]    = coastV;
      chaseCoastTau[p] = (coastV > 0.5f && glideCap > 0.0f) ? (glideCap / coastV) : 0.0f;
    } else if (chaseCoastTau[p] > 0.0f && chaseCoast[p] > 0.5f) {
      // THE COAST - THIS IS MOMENTUM, and it is why it was invisible before: the speed used to
      // decay WHILE the credit was being spent, but the credit is gone in tens of milliseconds at
      // these speeds, so the decay never got going and the dot snapped straight back to the roll.
      // Now the kick's own distance is covered at full kick speed and THEN the excess decays back
      // to the roll, so the whole dot visibly carries on and slows down.
      //   Momentum 0    -> tau 0    -> no coast at all: a dead stop exactly at Distance.
      //   Momentum 0.5  -> tau 6s   -> a long glide that eases past Distance over seconds.
      chaseCoast[p] *= expf(-dtSec / chaseCoastTau[p]);
      rate = roll + chaseCoast[p];
      if (chaseCoast[p] <= 0.5f) chaseCoast[p] = 0.0f;   // the glide is spent
    } else {
      chaseCoast[p] = 0.0f;
    }

    // SANITISE THE RATE. This is the crash: sqrtf() of a corrupt level is NaN, constrain()
    // passes NaN straight through because BOTH of its comparisons are false for NaN, and
    // (int)NaN is INT_MIN - so the position was thrown far negative, the renderer's `% N`
    // gave a negative index, and setLed() wrote outside the strip buffer. Anything not sane
    // falls back to `roll`, which is ZERO for the Chase now - so a corrupt rate means "do not
    // move" rather than a crawl.
    // THE CEILING IS 20,000 px/s, NOT 1,000 (20 Oct): the fastest legal kick is Distance 50
    // covered in the Kick Speed's minimum 0.02s = 2,500 px/s, and the old 1,000 ceiling rejected
    // that as insane and froze the dot - one drag of Distance to the top silently stopped it.
    if (!(rate > 0.0f && rate <= 20000.0f)) rate = roll;
    chaseAcc[p] += rate * dtSec;
    if (!(chaseAcc[p] >= 0.0f && chaseAcc[p] <= 1000.0f)) chaseAcc[p] = 0.0f;
    if (chaseAcc[p] >= 1.0f) {
      int add = (int)chaseAcc[p];
      chaseAcc[p] -= (float)add;
      g_steps[p] += (uint32_t)add;      // the pixels this port has ADVANCED (see g_steps)
      // ORIENTATION decides which way a kick carries the dot: toward the far end as before, or
      // toward pixel 0 when it is ticked. The end test mirrors with it, so the dot restarts at the
      // FAR end when it reaches 0 instead of at 0 when it reaches the far end.
      int np = portCfg[p].orient ? (ledPosArr[p] - add) : (ledPosArr[p] + add);
      if (np > maxPos)      ledPosArr[p] = portCfg[p].orient ? maxPos : 0;
      else if (np < 0)      ledPosArr[p] = portCfg[p].orient ? maxPos : 0;
      else                  ledPosArr[p] = np;
    }
  }
  return ledPosArr[p] != was;
}

// Step every strip independently, then ONE ledShowAll() per frame.
// The step rate comes ONLY from each strip's Speed setting - antifreeze never
// affects speed, only direction, and only when Audio Detect is off.
void runLEDPattern() {
  unsigned long now = millis();
  bool drew = false;

  // THE MOVING DOT EFFECT'S DEBRIS moves EVERY FRAME: it is a cloud, not a dot, so it must not
  // be tied to the dot's step timer.
  mdAdvanceAll(now);

  // (The BASS LEVEL ENVELOPE is updated in the AUDIO path now - see audioAnalyse - so it exists
  // whether or not the display is running.)

  // THE FALLBACK HIT, DECIDED ONCE PER FRAME. The level's rise is a property of the MUSIC, so every
  // Chase port must take a fallback kick in the same frame - a per-port decision (each with its own
  // 200 ms gate) let one port take a hit the other was gated out of, putting it a whole kick ahead.
  {
    static float         envLast = 0.0f;
    static unsigned long fbAt    = 0;
    float envNow    = g_bassLvlEnv;
    bool  risingNow = (envNow > envLast + CHASE_FALLBACK_RISE);
    envLast = envNow;
    g_chaseFallback = (risingNow && (now - fbAt) > CHASE_FALLBACK_GAP_MS);
    if (g_chaseFallback) fbAt = now;

    // THE SHARED ONSET HIT. The beat was stamped by listeningStep; CHASE_BEAT_CENTRE_MS later it
    // becomes a hit for EVERY Chase port in that single frame - decided once, so two identical
    // cells cannot take different hits. The level still sets the strength, per port, from n.
    g_chaseOnsetHit = false;
    if (g_beatSeenAt != 0 && g_beatSeenAt != g_beatDoneAt &&
        (long)(now - g_beatSeenAt) >= (long)CHASE_BEAT_CENTRE_MS) {
      g_chaseOnsetHit = true;
      g_beatDoneAt    = g_beatSeenAt;
      g_kickArmed++;
    }
  }

  // The EQ bars advance ONCE per frame, never once per port.
  for (int p = 0; p < NUM_PORTS; p++) {
    if (portCfg[p].pat == LP_EQ) { advanceEqBars(attackLED, decayLED); break; }
  }

  bool anyDrew = false;
  // Frame delta for the chase. CAPPED, so a stall (a flash erase, a wifi burst) cannot
  // fling the dot halfway down the strip in one frame.
  static unsigned long lastChaseMs = 0;
  unsigned long chaseDt = (lastChaseMs == 0) ? 2 : (now - lastChaseMs);
  if (chaseDt > 100) chaseDt = 100;
  lastChaseMs = now;
  for (int p = 0; p < NUM_PORTS; p++) {
    const PortCfg &c = portCfg[p];
    // COUNT THE FRAMES THIS PORT WAS ACTUALLY VISIBLE, so the step counts can be compared fairly.
    g_framesVis[p]++;

    // A DARK port stays blank: skipping it here is what keeps its black buffer black. The
    // blank itself was written and transmitted once, at the moment the port went dark.
    if (c.dark) continue;
    // PORT SYNC SNAPSHOT, so two ports on the same entry can be compared directly.
    g_syncPos[p] = ledPosArr[p];
    g_syncDir[p] = ledDirArr[p];
    g_syncAcc[p] = (int32_t)(chaseAcc[p] * 100.0f);      // hundredths of a pixel
    g_syncAge[p] = (uint32_t)(now - ledStepMs[p]);        // ms since this port last stepped
    // PAIR DIVERGENCE, port 0 against port 1 - the two the user runs on the same entry.
    if (p == 1) {
      int32_t d = ledPosArr[0] - ledPosArr[1];
      if (d < 0) d = -d;
      if (d > g_pairDiff) { g_pairDiff = d; g_pairA = ledPosArr[0]; g_pairB = ledPosArr[1]; g_pairRow = tlRow + 1; }
    }
    // PER-PORT STALENESS, three compares per port per frame: how long has this port's POSITION been
    // unchanged? A pause that shows on the strips is either a CPU hitch (see g_maxLoopUs) or a dot
    // that has stopped - and this is the only instrument that can tell them apart.
    {
      static unsigned long posAtMs[NUM_PORTS] = {0};
      static int           posPrev[NUM_PORTS] = {0};
      if (posAtMs[p] == 0) posAtMs[p] = now;
      if (ledPosArr[p] != posPrev[p]) { posPrev[p] = ledPosArr[p]; posAtMs[p] = now; }
      else {
        uint32_t st = (uint32_t)(now - posAtMs[p]);
        if (st > g_staleMaxMs) {
          g_staleMaxMs = st;
          g_stalePort  = p;
          g_staleDark  = c.dark ? 1 : 0;    // a DARK port looks exactly like a stopped dot
          g_staleRow   = tlRow + 1;         // 1-based, the same as the page shows
        }
      }
      g_lastMoveMs[p] = posAtMs[p];   // published in /diag: for a dot the POSITION is the motion
    }
    // THE CHASE is not on the step timer: it is updated EVERY FRAME, because with wrap OFF
    // its movement follows the audio level and with wrap ON it interpolates a traverse
    // between beats. It reports whether it moved, so a still chase transmits nothing.
    //
    // Its DRAW, however, is capped like every other pattern. Asking for a show every frame
    // saturated the bus (a ten-strip transmit is a hard ~4.7 ms) and starved the audio and
    // web tasks - which is what an incoming UI request on top of it was tipping over. The
    // position is still updated every frame; only the transmit is rate-limited.
    if (isChasePat(c.pat)) {
      bool moved = stepChase(p, now, chaseDt);
      if (moved && (now - ledStepMs[p]) >= 5) {
        ledStepMs[p] = now;
        drawPortPattern(p);
        anyDrew = true;
      }
      continue;
    }
    bool isVu = isLevelPat(c.pat);   // VU meters and the EQ are all level-driven
    unsigned long interval = isVu ? 25UL : (unsigned long)(constrain(c.speed, 0.002f, 5.0f) * 1000.0);
    // Floor the step at the frame rate (5 ms), NOT 20 ms. The 20 ms floor here was
    // a hard 50 steps/second ceiling that no slider setting could beat - it is the
    // "48/s" that showed up in every log and the reason more speed was impossible.
    if (interval < 2) interval = 2;

    if (now - ledStepMs[p] < interval) continue;
    ledStepMs[p] = now;
    g_stepCount++;

    // A step MUST be displayed, so it can never be faster than the frame rate -
    // otherwise the dot jumps and pixels are never lit. Keep this equal to frameMs.
    if (interval < 2) interval = 2;

    drawPortPattern(p);
    g_steps[p] += 1;                    // the pixels this port has ADVANCED (see g_steps)
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
  const unsigned long frameMs = 2;
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
  // ---- STALL WATCHDOG (user, 20 Oct) ----------------------------------------------------------
  // The Moving Dot stall is INTERMITTENT - "I don't understand why the fault was there before but
  // not now" - and a fault like that cannot be caught by hand. So the board now watches itself:
  // once a second, every port that is NOT dark and NOT level-driven must have advanced its step
  // counter. Four still seconds writes ONE log line carrying every number that decides why:
  // pattern, speed, size, effect, distance, the EROSION state (lost/dot - dot=0 is an INVISIBLE dot,
  // which looks exactly like a stopped one), whether it has exploded, the owning cell, the pixels
  // advanced and the motion-restart count. The log persists, so an unattended stall is diagnosable
  // afterwards instead of being guessed at. Rate-limited to one line per port per 10 s.
  {
    static uint32_t      lastSt[NUM_PORTS]       = {0};
    static unsigned long stillSince[NUM_PORTS]   = {0};
    static unsigned long lastComplain[NUM_PORTS] = {0};
    static unsigned long lastCheck = 0;
    if ((unsigned long)(now - lastCheck) >= 1000) {
      lastCheck = now;
      for (int p = 0; p < NUM_PORTS; p++) {
        uint32_t st = g_steps[p];
        if (st != lastSt[p]) { lastSt[p] = st; stillSince[p] = now; continue; }
        if (portCfg[p].dark || isLevelPat(portCfg[p].pat)) continue;
        if ((unsigned long)(now - stillSince[p]) < 4000) continue;
        if ((unsigned long)(now - lastComplain[p]) < 10000) continue;
        lastComplain[p] = now;
        const TlItem &wc = tlData[tlRow][p + 1];
        logAppend("STALL p=%d pat=%d sp=%.4f sz=%d ef=%d ds=%d mo=%.2f or=%d lost=%d dot=%d dead=%d fre=%lu cell=%s pos=%d dir=%d dark=%d age=%lu gs=%lu",
                  p + 1, portCfg[p].pat, (double)portCfg[p].speed, portCfg[p].size,
                  portCfg[p].effect, portCfg[p].distance, (double)portCfg[p].momentum,
                  portCfg[p].orient ? 1 : 0,
                  (int)mdLost[p], mdDotLen(p), (mdExpiredAt[p] != 0) ? 1 : 0,
                  (unsigned long)g_framesRestart[p], wc.used ? wc.id : "-",
                  ledPosArr[p], ledDirArr[p], portCfg[p].dark ? 1 : 0,
                  (unsigned long)(now - g_lastMoveMs[p]), (unsigned long)g_steps[p]);
      }
    }
  }
}

// ---- THE SAME PICTURE, OVER USB (user, 20 Oct) ----------------------------------------------
// The PC cannot stay joined to an access point with no internet - it has other work to do - so the
// Wi-Fi route to /diag is not usable for watching an intermittent fault. This formats ONE port's
// state exactly as the STALL watchdog does, so the USB serial port carries the same evidence with no
// network involved. The 'D' serial command prints it for all ten ports.
// NOTE: it deliberately does NOT call handleDiag(), which sends over HTTP from the web task; a
// serial command runs in loop(), and mixing the two contexts is not safe.
static void portStateLine(int p, char* b, size_t n) {
  const TlItem &cc = tlData[tlRow][p + 1];
  snprintf(b, n,
           "p=%d pat=%d sp=%.4f sz=%d ef=%d ds=%d mo=%.2f or=%d lost=%d dot=%d dead=%d fre=%lu cell=%s pos=%d dir=%d dark=%d age=%lu gs=%lu",
           p + 1, portCfg[p].pat, (double)portCfg[p].speed, portCfg[p].size, portCfg[p].effect,
           portCfg[p].distance, (double)portCfg[p].momentum, portCfg[p].orient ? 1 : 0,
           (int)mdLost[p], mdDotLen(p), (mdExpiredAt[p] != 0) ? 1 : 0,
           (unsigned long)g_framesRestart[p], cc.used ? cc.id : "-",
           ledPosArr[p], ledDirArr[p], portCfg[p].dark ? 1 : 0,
           (unsigned long)(millis() - g_lastMoveMs[p]), (unsigned long)g_steps[p]);
}

// ==========================================
// TIMELINE (per-strip patterns) + MASTER BRIGHTNESS
// The UI saves /timeline.json as { "delay": sec, "items": { "row_col": {...} } }
// where col 1..10 = LED strips and col 11 = the 240V display.
// ==========================================
void applyMasterBrightness() {
  uint8_t s = (uint8_t)map((int)masterBrightness, 0, 100, 0, 255);
  FastLED.setBrightness(s);        // used only by the FastLED fallback path
}

// ---- THE PLAYLIST: THE MASTER PARAMETER STORE (Rules: the Build Process) ---------------------
// Every field the pattern page saved is read here, so a timeline cell needs nothing but the item's
// name. This is the ONLY place a playing port gets its parameters from.
static void loadPlaylist() {
  for (int i = 0; i < PL_MAX_ITEMS; i++) { plItems[i].used = false; plItems[i].name[0] = 0; }
  plCount = 0;
  File pf = LittleFS.open("/playlist.json", "r");
  if (!pf) { logAppend("playlist: /playlist.json not found - every timeline cell will be DARK"); return; }
  JsonDocument pdoc;
  DeserializationError pe = deserializeJson(pdoc, pf);
  pf.close();
  if (pe) { logAppend("playlist: parse failed (%s) - every timeline cell will be DARK", pe.c_str()); return; }
  JsonArray arr = pdoc["items"].as<JsonArray>();
  if (arr.isNull()) { logAppend("playlist: no items array - every timeline cell will be DARK"); return; }

  for (JsonVariant v : arr) {
    if (plCount >= PL_MAX_ITEMS) break;
    String nm = String((const char*)(v["name"] | ""));
    if (nm.isEmpty()) continue;
    PlItem &it = plItems[plCount];
    strlcpy(it.name, nm.c_str(), sizeof(it.name));
    // NAME -> PATTERN ID -> PATTERN (20 Oct). renderBranchFor() takes a PATTERN ID, not a playlist name:
    // given a name it matches nothing and DEGRADES TO MOVING DOT. That is why the Moving Dot rows
    // worked (the fallback happened to be correct) while "Chase yel"/"Chase yep r" ran as MOVING DOT
    // and never responded to the audio. patternIdFromName() is the same two-step the old cell parse
    // used, so the item's name reaches the pattern it names.
    it.pat      = renderBranchFor(patternIdFromName(nm));
    it.p.speed     = jsonNumF(v["speed"],     0.02f);
    it.p.qty       = constrain(jsonNumI(v["quantity"], 1), 1, 8);
    it.p.size      = constrain(jsonNumI(v["size"],     1), 1, 10);
    it.p.wrap      = v["wrap"]   | false;
    it.p.invert    = v["invert"] | false;
    it.p.orient    = v["orient"].is<bool>() ? (v["orient"].as<bool>() ? 1 : 0) : jsonNumI(v["orient"], 0);
    it.p.freqHz    = jsonNumI(v["frequency"], 50);
    it.p.mult      = (jsonNumI(v["beatMult"], 1) > 1) ? 2 : 1;
    it.p.attack    = jsonNumF(v["attack"],    0.15f);
    it.p.decay     = jsonNumF(v["decay"],     0.01f);
    it.p.peakDelay = jsonNumF(v["peakDelay"], 0.5f);
    it.p.peakSpeed = jsonNumF(v["peakSpeed"], 1.0f);
    it.p.peakDir   = jsonNumI(v["peakDir"],   0);
    it.p.beatSens  = jsonNumF(v["beatSens"],  0.2f);
    it.p.effect    = constrain(jsonNumI(v["effect"], 0), 0, 10);
    it.p.momentum  = constrain(jsonNumF(v["momentum"], 0.0f), 0.0f, 0.5f);
    it.p.distance  = constrain(jsonNumI(v["distance"], 20), 0, 100);
    String vd = String((const char*)(v["vuDir"] | "LEFT"));
    vd.toUpperCase();
    it.p.vuDir = (vd == "RIGHT") ? 1 : (vd == "CENTRE") ? 2 : (vd == "ENDS") ? 3 : 0;
    JsonArray ca = v["colour"].as<JsonArray>();
    if (!ca.isNull()) {
      int ci = 0;
      for (JsonVariant cv : ca) {
        if (ci >= 4) break;
        it.p.colour[ci] = parseColourRef(cv.as<const char*>(), it.p.colour[ci]);
        ci++;
      }
    }
    it.used = true;
    plCount++;
  }
  logAppend("playlist: %d items loaded - timeline cells resolve against these", plCount);
}

// THE ITEM A CELL NAMES - case-insensitive, and nullptr when there is none. A nullptr means the port
// goes DARK: never to the pattern page, never to a pattern's stored record.
static const PlItem* plFind(const char* nm) {
  if (!nm || !nm[0]) return nullptr;
  for (int i = 0; i < PL_MAX_ITEMS; i++) {
    if (!plItems[i].used) continue;
    if (String(plItems[i].name).equalsIgnoreCase(nm)) return &plItems[i];
  }
  return nullptr;
}

void loadTimeline() {
  // THE PLAYLIST IS THE MASTER (Rules: the Build Process): every cell names one of its items, so it
  // must be loaded before any cell can resolve. Loading it here means it refreshes whenever the
  // timeline is loaded - at boot, and after any save - with no separate hook to remember.
  loadPlaylist();
  for (int r = 0; r < TL_ROWS; r++)
    for (int c = 0; c < TL_COLS; c++) tlData[r][c].used = false;
  tlLoaded = false;
  // Fresh load = fresh ownership: a port whose cell was deleted must go back to the
  // page settings rather than keep the deleted item playing for ever.
  for (int p = 0; p < NUM_PORTS; p++) portCellOwned[p] = false;

  if (!LittleFS.exists("/timeline.json")) return;
  File f = LittleFS.open("/timeline.json", "r");
  if (!f) return;
  JsonDocument doc;
  DeserializationError e = deserializeJson(doc, f);
  f.close();
  if (e) return;

  // jsonNumF, NOT `doc["delay"] | 20.0f`. The page sends the delay as a STRING, and the |
  // operator silently returns its DEFAULT for a string - so the delay slider had no effect
  // at all and the show always ran at 20s. The row really was 20s while the grid said 10s,
  // which is why the moving bar looked like it was at the wrong speed: the bar was right.
  tlDelaySec = jsonNumF(doc["delay"], 20.0f);
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
    // THE CELL KEEPS THE ITEM NAME - the reference the Build Process specifies (20 Oct). This used to
    // store patternIdFromName(nm) - the PATTERN id, e.g. "moving_dot" - which can never match a
    // playlist ITEM named "Moving Dot Red Fast". Every cell therefore resolved to nothing and every
    // port went dark. The pattern now comes from the item (pit->pat), so the pattern id is not needed
    // here at all - and "END OF DISPLAY" still matches, because that is a name too.
    strlcpy(tlData[r][c].id, nm.c_str(), sizeof(tlData[r][c].id));
    tlData[r][c].speed  = jsonNumF(it["speed"],    0.2f);
    tlData[r][c].qty    = jsonNumI(it["quantity"], 1);
    tlData[r][c].size   = jsonNumI(it["size"],     1);
    tlData[r][c].wrap   = it["wrap"] | false;
    // ORIENTATION arrives from the page as a JSON BOOLEAN (the slider's .checked), and jsonNumI()
    // only understands strings and numbers - so this used to read back as -1, "the cell carries
    // none", and EVERY cell fell back to the PATTERN's orientation. That is why two Chase entries
    // with different Orientation ticks both ran the same way (user, 20 Oct: "Regardless of the
    // orientation checkmark, the patterns all went in the same direction"). SAME TRAP as the pattern
    // record above, same fix: a real boolean first, the numeric form for anything old.
    if (it["orient"].is<bool>()) tlData[r][c].orient = it["orient"].as<bool>() ? 1 : 0;
    else                         tlData[r][c].orient = jsonNumI(it["orient"], -1);
    tlData[r][c].invert = it["invert"] | false;
    // Colour references travel WITH the pattern, so the timeline ADOPTS them rather
    // than offering colour controls of its own. A cell that has none (saved before
    // this existed) leaves whatever colour the strip already has.
    JsonArray ca = it["colour"].as<JsonArray>();
    if (!ca.isNull()) {
      int ci = 0;
      for (JsonVariant cv : ca) {
        if (ci >= 4) break;
        tlData[r][c].colour[ci] = parseColourRef(cv.as<const char*>(), tlData[r][c].colour[ci]);
        ci++;
      }
      tlData[r][c].hasColour = true;
    }
    // Beat Multiplier: it travels with the pattern like everything else. 1 or 2; 0
    // means the cell carries none, and then the page's value stays in force.
    int bm = jsonNumI(it["beatMult"], 0);
    tlData[r][c].mult = (bm > 1) ? 2 : ((bm == 1) ? 1 : 0);
    // Its LISTENING settings travel with it too.
    tlData[r][c].freqHz   = jsonNumI(it["frequency"], 0);
    tlData[r][c].beatSens = jsonNumF(it["beatSens"], -1.0f);
    // The EFFECT travels with the pattern too, so a cell can give its strip its own shed
    // debris while the next cell - or the page - uses another setting.
    tlData[r][c].effect   = jsonNumI(it["effect"],   -1);
    tlData[r][c].momentum = jsonNumF(it["momentum"], -1.0f);
    tlData[r][c].distance = jsonNumI(it["distance"], -1);
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
  if (row < 0 || row >= TL_ROWS) row = 0;
  // THE EFFECT RE-ARMS HERE: a new timeline ROW (a fresh cell is "seen again") or PLAY. NOT on
  // a slider move - a tweak passes through this function too, and re-arming there would
  // resurrect a dead dot every time any control moved.
  {
    static int mdRow = -1;
    if (force || row != mdRow) {
      mdRow = row; mdResetAll();
      // WHAT EACH PORT WILL USE IN THIS ROW: "eff/dist/mom" per column, '-' when the cell
      // carries none (so that port's pattern record is in force instead). Logged ONLY at a row
      // change or PLAY - a tweak passes through here too, and logging there would flood.
      char ef[NUM_PORTS * 16 + 1];
      int en = 0;
      for (int p = 0; p < NUM_PORTS && en < (int)sizeof(ef); p++) {
        TlItem *cell = tlData[row][p + 1].used ? &tlData[row][p + 1] : nullptr;
        if (cell && cell->effect >= 0)
          en += snprintf(ef + en, sizeof(ef) - en, "%d/%d/%.2f ", cell->effect,
                         constrain(cell->distance, 0, 100), (double)cell->momentum);
        else
          en += snprintf(ef + en, sizeof(ef) - en, "- ");
      }
      logAppend("row %d cells eff/dist/mom %s", row + 1, ef);
    }
  }
  // NOTE: there is deliberately NO `!tlLoaded` early return any more. With no timeline
  // every port simply takes the page's settings - which is exactly what the
  // derivation below does when it finds no cell for that port.

  // ===========================================================================
  // EVERY port is derived from its SOURCE OF TRUTH:
  //   the CURRENT row has a cell for this port -> THAT CELL OWNS THE PORT
  //   no cell                                   -> the LED page is its default
  //
  // This replaced "stamp the page onto all ten ports, then put back the cells that
  // happen to be in this row". THAT pair was the fault: a page tweak knocked EVERY
  // port without a cell in the CURRENT row onto the page settings, so touching a
  // colour or a slider wrecked a running timeline until the row came round again
  // (up to 8 x 20 s) - and a port with no cell could never hold anything else.
  // ===========================================================================
  LedPattern pagePat = renderBranchFor(activePatternLED);

  // A timeline cell only OWNS its port WHILE THE TIMELINE IS PLAYING. Stopped, the LED
  // page owns every port, so a pattern can be built and WATCHED on the strips without
  // deleting cells or fighting the timeline - which was impossible after pressing Stop:
  // touching the page re-derived the ports from the timeline's slot (so it looked like
  // the show had restarted) and a colour change could not reach the strips at all.
  bool cellsOwn = tlPlaying;
  if (!cellsOwn) for (int p = 0; p < NUM_PORTS; p++) portCellOwned[p] = false;

  for (int p = 0; p < NUM_PORTS; p++) {
    TlItem *cell = (cellsOwn && tlData[row][p + 1].used) ? &tlData[row][p + 1] : nullptr;
    if (cell) portCellOwned[p] = true;

    // EMPTY CELL = DARK, for the length of that cell's slot. This used to KEEP whatever the
    // port was already doing, so a blank cell was invisible - the strip simply carried on -
    // and a blank cell must now mean "this strip is off for this period". The END OF
    // DISPLAY marker lights nothing either, so it counts as blank here.
    //
    // Ownership is cleared when the timeline is (re)loaded, so deleting a cell and saving
    // reverts the port to the page.
    // THE ITEM THIS CELL NAMES - the ONLY source of a playing port's parameters (Rules: the Build
    // Process). plFind() is case-insensitive and returns nullptr when the name has no item behind it -
    // and then the port is DARK. It never falls back to the page, and never to a pattern's stored
    // record: the page is volatile and editing it must never be able to change a running show.
    const PlItem *pit = cell ? plFind(cell->id) : nullptr;
    // A cell naming nothing resolvable is DARK, exactly like an empty cell.
    bool wantDark = cellsOwn && (!cell || !pit || isEndOfDisplayId(cell->id));
    if (wantDark != portCfg[p].dark) {
      portCfg[p].dark = wantDark;
      if (wantDark) {
        for (int i = 0; i < NUM_LEDS_PER_STRIP; i++) leds[p][i] = CRGB::Black;
        ledDirty[p] = true;            // the blank has to be TRANSMITTED, not just held
      }
      g_restartMotion = true;          // applied after the loop, so ALL ports restart in phase
    }
    if (wantDark) continue;

    // ---- what this port SHOULD be ----
    // THE PATTERN COMES FROM THE ITEM: the name decided it when the playlist was parsed, so while the
    // timeline plays nothing is resolved from the page at all.
    LedPattern wantPat = pit ? pit->pat : pagePat;
    float wantSpeed    = cell ? cell->speed : speedLED;
    int   wantQty      = cell ? constrain(cell->qty, 1, 8)   : constrain(quantityLED, 1, 8);
    int   wantSize     = cell ? constrain(cell->size, 1, 10) : constrain(sizeLED, 1, 10);
    bool  wantWrap     = cell ? cell->wrap   : wrapLED;
    // ORIENTATION: THE CELL'S OWN, OR NONE (user, 20 Oct). This fell back to ledSel() - the pattern
    // SELECTED ON THE PAGE - so a cell carrying no orientation inherited whatever the page's pattern
    // was set to. With the Chase selected and its Orientation ticked, that pushed orient=1 onto the
    // Moving Dot's ports, and orient=1 with Wrap OFF is what parked the dot at the end of its strip
    // ("the moving dot is behaving like the chase with wrap on"). Rule 2: the timeline applies what
    // the PLAYLIST stored, never what another pattern's page happens to be showing. A cell that
    // carries no orientation of its own now means NO orientation, not "another pattern's".
    bool  wantOrient   = cell ? (cell->orient > 0) : (ledSel().orient != 0);
    bool  wantInv      = cell ? cell->invert : invertLED;
    int   wantMult     = (cell && cell->mult > 0) ? ((cell->mult > 1) ? 2 : 1)
                                                  : ((multLED > 1) ? 2 : 1);
    int   wantFreq     = (cell && cell->freqHz > 0) ? constrain(cell->freqHz, 20, 100) : freqLED;
    // THE CELL'S OWN SENSITIVITY, OR THE PATTERN'S WHEN THERE IS NO CELL (user, 19 Oct: "A timeline
    // cell should not affect the sensitivity in any way shape or form. That is all handled in the
    // patterns page and stored in the playlist. The timeline is a playback facility only").
    //
    // A cell IS the saved playlist item: it carries the parameters saved with it, and the timeline
    // only decides WHEN it plays. So the cell's value is authoritative while it owns the port.
    // (A "sensitivity floor" was added and removed on the same day - it made a cell's own value
    // lose to the pattern's, which is not how the playlist is supposed to work.) If a cell seems
    // to ignore a sensitivity, the ITEM needs re-saving: edit the pattern on the patterns page,
    // then save it into the playlist.
    float wantSens = (cell && cell->beatSens >= 0.0f)
                       ? constrain(cell->beatSens, 0.01f, 1.0f) : pit->p.beatSens;      // NEVER the page's selected pattern (20 Oct): the ITEM is the master
    const uint32_t *wantRef = (cell && cell->hasColour) ? cell->colour : colourLED;
    // THE EFFECT'S VALUES ARE THE CELL'S OWN (user, 19 Oct). A cell that carries NONE means the
    // effect is OFF - NOT "inherit whatever the pattern happens to be set to". An item saved
    // before the effect existed must not quietly adopt it: that is exactly the cross-pattern
    // contamination the isolation rule forbids ("the playlist items adopted the effects of the
    // pattern I was testing").
    const LedPatParams &ownPat = ledPat(ledPatIndexOfEnum((int)wantPat));
    int   wantEffect = cell ? constrain((cell->effect   < 0)    ? 0    : cell->effect, 0, 10)
                            : (int)ownPat.effect;
    float wantMom    = cell ? constrain((cell->momentum < 0.0f) ? 0.0f : cell->momentum, 0.0f, 0.5f)
                            : ownPat.momentum;
    int   wantDist   = cell ? constrain((cell->distance < 0)    ? 20   : cell->distance, 0, 100)
                            : (int)ownPat.distance;

    // MOTION is compared SEPARATELY from the rest: changing a colour, a sensitivity or
    // the multiplier must NOT restart the pattern, or the dot visibly jumps every time
    // you touch the page.
    bool motionDiff = portCfg[p].pat != (int)wantPat ||
                      fabsf(portCfg[p].speed - wantSpeed) > 0.0001f ||
                      portCfg[p].qty  != wantQty ||
                      portCfg[p].size != wantSize ||
                      portCfg[p].wrap != wantWrap ||
                      portCfg[p].orient != wantOrient;

    bool valueDiff = motionDiff ||
                     portCfg[p].invert   != wantInv ||
                     portCfg[p].mult     != wantMult ||
                     portCfg[p].freqHz   != wantFreq ||
                     fabsf(portCfg[p].beatSens - wantSens) > 0.001f ||
                     portCfg[p].effect   != wantEffect ||
                     fabsf(portCfg[p].momentum - wantMom) > 0.001f ||
                     portCfg[p].distance != wantDist ||
                     portCfg[p].colourRef[0] != wantRef[0] ||
                     portCfg[p].colourRef[1] != wantRef[1] ||
                     portCfg[p].colourRef[2] != wantRef[2] ||
                     portCfg[p].colourRef[3] != wantRef[3];
    if (!valueDiff) continue;                 // identical -> continuous run

    portCfg[p].pat      = (int)wantPat;
    portCfg[p].speed    = wantSpeed;
    portCfg[p].qty      = wantQty;
    portCfg[p].size     = wantSize;
    portCfg[p].wrap     = wantWrap;
    portCfg[p].orient   = wantOrient;
    portCfg[p].invert   = wantInv;
    portCfg[p].mult     = wantMult;
    portCfg[p].freqHz   = wantFreq;
    portCfg[p].beatSens = wantSens;
    portCfg[p].effect   = wantEffect;
    portCfg[p].momentum = wantMom;
    portCfg[p].distance = wantDist;
    for (int i = 0; i < 4; i++) {
      portCfg[p].colourRef[i] = wantRef[i];
      CRGB v = resolveColourRef(wantRef[i]);      // a theme reference resolves LIVE
      portCfg[p].colour[i] = ((uint32_t)v.r << 16) | ((uint32_t)v.g << 8) | v.b;
    }
    if (force || motionDiff) {                    // the MOTION changed: restart (see below)
      g_restartMotion = true;                     // applied after the loop, so ALL ports restart
    }
  }

  // EVERY PORT RESTARTS TOGETHER, or none does. Restarting only the ports whose motion changed
  // left them at different phases - the permanent gap between display 1 and display 2. The
  // chase's own motion state is reset here too, so a restart is a real restart.
  if (g_restartMotion) {
    for (int p = 0; p < NUM_PORTS; p++) {
      // ORIENTATION: ticked, the pattern STARTS at the far end and travels toward pixel 0 (user,
      // 19 Oct: "starts at the far end and travels toward pixel 0"). The far end is the CHASE's own
      // limit (the strip less the dot), and for every other pattern the strip less its size.
      int startAt = 0;
      if (portCfg[p].orient) {
        int m = (portCfg[p].pat == LP_CHASE_1_10)
                  ? chaseMaxPos(p)
                  : (NUM_LEDS_PER_STRIP - constrain(portCfg[p].size, 1, 10));
        startAt = (m < 0) ? 0 : m;
      }
      ledPosArr[p] = startAt;
      ledDirArr[p] = portCfg[p].orient ? -1 : 1;
      ledExplArr[p] = 0; ledStepMs[p] = 0;
      chaseAcc[p] = 0.0f; chaseCredit[p] = 0.0f; chaseCoast[p] = 0.0f;
      chaseLvlLast[p] = 0.0f; chaseNlast[p] = 0.0f;
      chaseCoastTau[p] = 0.0f;
      g_framesRestart[p]++;      // the dot was re-pinned; a climbing count with a still dot IS the fault
    }
    g_restartMotion = false;
  }

  // ---- PER-PORT INVERT IS NO LONGER FORCED OFF ------------------------------
  // It used to be cleared here for every port. Invert is an ordinary per-item
  // setting: it is set on the pattern page, stored with the pattern in the
  // playlist item and adopted by the timeline cell, and it must not affect any
  // other pattern or timeline entity. Tested on Moving Dot and working.

  if (tlData[row][11].used && !isEndOfDisplayId(tlData[row][11].id)) {   // column 11 = 240V display
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

  // The LISTENED bands, sensitivities and multipliers follow the cells that were just
  // adopted - so each playlist item on the timeline is listened to in its own terms,
  // recalculated as the timeline advances.
  rebuildListening();
}

// Time-division sequencer: advance one slot every "delay" seconds. The delay
// value only sets how long each timeline cell is displayed.
void runTimelineSequencer() {
  if (!tlLoaded) return;
  // The sequence only runs while PLAYING. A saved timeline used to run itself for ever
  // from power-on, which is why the strips could appear to be "playing old data" with
  // nothing in the UI.
  if (!tlPlaying) return;
  unsigned long now = millis();
  if (now - tlRowStart >= (unsigned long)(tlDelaySec * 1000.0f)) {
    tlRowStart = now;
    int next = (tlRow + 1) % TL_ROWS;
    // END OF DISPLAY. The marker row is never PLAYED - it is a boundary - so reaching it
    // restarts the show at row 1, and that loops until STOP. A dark pause between loops is
    // made by leaving a row EMPTY, because an empty cell now blanks its strip.
    if (isEndOfDisplayRow(next)) {
      logAppend("timeline END OF DISPLAY at row %d -> restart at row 1", next + 1);
      next = 0;
      tlRowStart = now;
    }
    tlRow = next;
    applyTimelineRow(tlRow, false);
  }
}

// ==========================================
// RENDER HELPERS (executed only by the display task)
// ==========================================
void renderCalibrationTest() {
  for (int p = 0; p < NUM_PORTS; p++)
    for (int i = 0; i < NUM_LEDS_PER_STRIP; i++) leds[p][i] = CRGB::Black;
  // MARK EVERY PORT DIRTY.
  //
  // This is why the test appeared to do nothing on ports 9 and 10: ledShowAll()
  // only copies strips whose ledDirty[] is set, and falls back to copying ALL only
  // when NONE are dirty. Cleared strips were never marked, so with any other port
  // left dirty from an earlier test, the newly cleared/lit ports were never
  // transmitted. The moving dot hid this completely because it repaints every
  // frame. Cleared pixels are a change and must be marked as one.
  for (int p = 0; p < NUM_PORTS; p++) ledDirty[p] = true;
  int port = calPort, order = calOrder;
  // (The CALRENDER instrument that logged every test render was removed here: it
  // fired twice a second and had served its purpose proving this function runs.
  // Removal confirmed that ports 9/10 lighting was fixed by the X16 driver change,
  // not by anything about the render.)
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
      // The return value is deliberately UNUSED: it reports CLOCK beats, and the
      // display follows real onsets (doBeat) plus antifreeze instead. The call is
      // kept because it runs the tempo tracking and the diagnostics.
      checkAudioBeat();
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
  uint32_t prevReq    = dbgReq;      // which request the web task was inside, if any
  uint32_t prevSub    = dbgSub;      // and how far into it
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
    dp.putUInt("req",    prevReq);
    dp.putUInt("sub",    prevSub);
    dp.putUInt("upsec",  prevUpSec);
    dp.putUInt("heap",   prevHeap);
    dp.putUInt("loops",  prevLoops);
    dp.end();
  }
  dbgMagic = DBG_MAGIC; dbgStage = 0; dbgUpSec = 0; dbgHeap = 0; dbgLoops = 0;
  dbgReq = REQ_NONE; dbgSub = 0; dbgReqN = 0;
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
  // SILENCE THE WiFi LOG FLOOD (user, 20 Oct). The iPhone connects with a PRIVATE, ROTATING MAC
  // address, and this access point then logs "CCMP replay detected" endlessly - hundreds of error
  // lines that bury everything worth reading (the STALL watchdog, the tweaks, the display
  // transitions) and make the serial Monitor look as though it is ignoring commands. The display is
  // untouched by this; only the noise is suppressed. Restore by removing this line, or use
  // ESP_LOG_ERROR to keep genuine errors and drop the rest.
  esp_log_level_set("wifi", ESP_LOG_NONE);
  // Diagnostic: 1=power-on, 4=panic/crash, 5=WDT, 9=brownout/reset
  Serial.printf("Boot OK. Reset reason = %d (%s) | previous run: %lus stage=%u heap=%u"
                " req=%s sub=%u\n",
                prevReason, resetReasonText(prevReason),
                (unsigned long)prevUpSec, (unsigned)prevStage, (unsigned)prevHeap,
                reqNameTxt(prevReq), (unsigned)prevSub);

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
  // Configure the listening chains BEFORE the audio task starts: chain 0 is the
  // baseline band and must be live, or nothing would be measured at all.
  odfConfigureChains();

  for (int i = 0; i < 10; i++) {
    pinMode(channelPins[i], OUTPUT);
    digitalWrite(channelPins[i], LOW);
  }

  if (!LittleFS.begin(true)) {
    // no serial output here: keep the USB-CDC path out of the startup stack
  }

  loadSettings();

  // ONE-OFF CLEANUP: the dead "clockDelay" key - the Fallback Clock Speed box that was
  // removed from the Master page - is still sitting in NVS from earlier builds. Nothing
  // reads it any more. Erasing a key that is NOT there is a no-op that writes nothing, so
  // this is harmless to leave in place: it reclaims the entry once and does nothing at all
  // afterwards. The heartbeat's nvsFree count gains one entry when it takes effect.
  preferences.begin("lightshow", false);
  preferences.remove("clockDelay");
  // The "Animations" theme is gone, so its eight stored colours (th3c0..th3c7) are swept
  // once. The other three themes are untouched. Erasing keys that are not there writes
  // nothing, so this is harmless to leave in.
  for (int ti = 0; ti < 8; ti++) preferences.remove((String("th3c") + String(ti)).c_str());
  preferences.end();

  loadPortCalibrations();

  // Populate previewColors from the restored theme. WITHOUT THIS the themes come
  // back BLACK on every power cycle: themeLoadToPreview() was only ever called
  // from /theme-preview, so previewColors kept its constructed (black) value no
  // matter what was stored - and even a correct save looked like a failed one.
  themeLoadToPreview(themeIndexOf(themeName));

  // Report the loaded calibration. It is loaded BEFORE the display is configured
  // (applyGlobalsToPorts / applyTimelineRow below), so this line proves what the
  // strip will actually use on the very first frame - rather than leaving the
  // "powers up showing the wrong order" report to be reasoned about.
  // ALL TEN PORTS. It only reported 1-4, so ports 5-8 could not be verified from
  // the log - and that is exactly what needed checking.
  

  logTrimAtBoot();
  // prevReq/prevSub name the request the web task was inside if the run ended in a panic.
  // They are GARBAGE after a POWER_ON reset (RTC is lost with the supply) - in that case
  // read lastReq from /diag, which is kept in NVS and does survive the cut.
  logAppendNow("BOOT #%u reason=%d(%s) prevUp=%us prevStage=%u prevHeap=%u prevReq=%s prevSub=%u nvsFree=%u erased=%u out=%s",
            (unsigned)dbgBoots, prevReason, resetReasonText(prevReason),
            (unsigned)prevUpSec, (unsigned)prevStage, (unsigned)prevHeap,
            reqNameTxt(prevReq), (unsigned)prevSub,
            (unsigned)nvsFreeE, nvsWasErased ? 1u : 0u,
            "fastled");


  // Apply the saved maximum brightness and the saved Timeline (per-strip patterns)
  applyMasterBrightness();
  loadTimeline();
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
  server.on("/pagerr", HTTP_GET, handlePageErr);
  server.on("/tail", HTTP_GET, handleTail);
  server.on("/settings", HTTP_GET, handleGetSettings);
  server.on("/update", HTTP_POST, handleUpdate);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/calibrate-test", HTTP_GET, handleCalibrateTest);
  server.on("/calibrate-show", HTTP_GET, handleCalibrateShow);
  server.on("/calibrate-stop", HTTP_GET, handleCalibrateStop);
  server.on("/calibrate-save", HTTP_GET, handleCalibrateSave);
  server.on("/calibrate-savecurrent", HTTP_GET, handleCalibrateSaveCurrent);
  server.on("/theme-preview", HTTP_POST, handleThemePreview);
  server.on("/live-tweak", HTTP_POST, handleLiveTweak);
  server.on("/save-playlist", HTTP_POST, handleSavePlaylist);
  server.on("/load-playlist", HTTP_GET, handleLoadPlaylist);
  server.on("/save-timeline", HTTP_POST, handleSaveTimeline);
  server.on("/load-timeline", HTTP_GET,  handleLoadTimeline);
  server.on("/timeline-play", HTTP_GET,  handleTimelinePlay);
  server.on("/timeline-stop", HTTP_GET,  handleTimelineStop);
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

  // THE PLAYLIST MUST BE LOADED BEFORE ANY CELL CAN RESOLVE (Rules: the Build Process). loadTimeline()
  // calls loadPlaylist() as well, but THIS retries until it succeeds, because LittleFS may not be
  // mounted on the first pass - and while the store is empty every timeline cell resolves to nothing
  // and its port goes DARK. It stops the moment items are loaded, so there is no ongoing flash access.
  {
    static unsigned long plRetryAt = 0;
    if (plCount == 0 && (long)(millis() - plRetryAt) >= 0) { plRetryAt = millis() + 2000; loadPlaylist(); }
  }

  // Serial command interface: 'L' dumps /log.txt over USB, 'D' prints /diag.
  // Lets a host read the diagnostics without the Wi-Fi AP (and without relying
  // on anybody copying text out by hand).
  while (Serial.available()) {
    int c = Serial.read();
    if (c == 'L' || c == 'l') serialDumpFile(LOG_PATH);
    else if (c == 'D' || c == 'd') {
      // EVERY PORT'S STATE, OVER USB (user, 20 Oct): the same fields the /diag JSON and the STALL
      // watchdog report. The PC cannot stay joined to an access point that has no internet - it has
      // other work to do - so this is now the primary way to read the board during a fault: no
      // network, no reset (the host de-asserts DTR/RTS before opening the port).
      Serial.print("\n-----BEGIN-----\n");
      Serial.printf("tlPlaying=%d tlRow=%d tlElapsedMs=%lu selPat=%s\n",
                    tlPlaying ? 1 : 0, tlRow + 1,
                    (unsigned long)(tlPlaying ? (millis() - tlRowStart) : 0),
                    activePatternLED.c_str());
      char b[260];
      for (int i = 0; i < NUM_PORTS; i++) { portStateLine(i, b, sizeof(b)); Serial.println(b); }
      Serial.print("\n-----END-----\n");
    }
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
  // Display ON/OFF transitions, ONE line each. This is what separates "the page asked for a
  // blank" (/init-off, /calibrate-stop) from "the firmware stopped rendering". The request
  // that caused it is the `req` line immediately above.
  {
    static bool wasActive = false;
    if (displayActive != wasActive) {
      wasActive = displayActive;
      logAppend("display %s pLED=%s p240=%s heap=%u", displayActive ? "ON" : "OFF",
                activePatternLED.c_str(), activePattern240V.c_str(),
                (unsigned)ESP.getFreeHeap());
    }
  }
  if (flashReq) {
    flashReq = false;
    doSaveFlash();
  } else if (calFlashMs) {
    // SAVE CONFIRMATION: off -> on -> off. Runs before the normal preview branches
    // so it cannot be overridden by them.
    unsigned long e = millis() - calFlashMs;
    if (e < CAL_FLASH_OFF1) {
      previewMode = 0; blankReq = true;                 // dark
    } else if (e < CAL_FLASH_ON) {
      previewMode = 1;                                  // lit
      if (previewDirty || (millis() - lastPreviewRender >= 500)) {
        previewDirty = false; lastPreviewRender = millis();
        renderCalibrationTest();
      }
    } else if (e < CAL_FLASH_END) {
      previewMode = 0; blankReq = true;                 // dark again
    } else {
      calFlashMs = 0; previewMode = 0; previewDirty = true;
    }
  } else if (blankReq) {
    blankReq = false;
    logAppend("blank displayActive=%d preview=%d", displayActive ? 1 : 0, previewMode);
    blankDisplays();
  } else if (previewMode == 1) {
    // THE CALIBRATION TEST STAYS LIT until it is ended deliberately: SAVE (whose flash finishes
    // by returning to the normal render), the STOP / leave-the-tab endpoint, or another
    // preview taking over. There is NO time limit - choosing a colour match takes as long as
    // it takes.
    //
    // It used to auto-expire after 3 s. That was a workaround for the strip lighting up merely
    // by OPENING the Colours page, and the PAGE has since been fixed for it: the tab-entry call
    // only reads the stored order, so nothing arms a test except the TEST button. With the
    // cause gone, the workaround is gone - and re-adding a timer here would take the test away
    // from the user mid-decision.
    //
    // Re-assert held previews periodically: they are otherwise only drawn when
    // marked dirty, so a single garbled frame (or anything that repaints the
    // strips) leaves them looking like a brief flash instead of staying on.
    if (previewDirty || (millis() - lastPreviewRender >= 500)) {
      previewDirty = false; lastPreviewRender = millis();
      renderCalibrationTest();
    }
  } else if (previewMode == 2) {
    // THE THEME PREVIEW WAS ALSO LATCHED FOREVER. The Colours page sends a theme
    // preview as soon as it is opened, so this - not the calibration test - is what
    // lit every strip in the theme colours when the page was selected.
    // ONE-SHOT: render the palette then RELEASE immediately. It used to be held,/
    // and because the Colours page re-sends the preview the hold timer was reset
    // every time, so it never expired - which is the "strips light up when I
    // select the Colours page and stay lit" fault.
    if (previewDirty || (millis() - lastPreviewRender >= 500)) {
      previewDirty = false; lastPreviewRender = millis();
      renderThemePreview();
      previewMode = 0;                    // one-shot: release at once
    }
  } else if (displayActive) {
    // ANTIFREEZE is handled inside listeningStep() now: it fires every active
    // listener when Audio Detect is OFF, and does nothing at all when it is ON.
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
    // ---- THE LISTENING STEP ------------------------------------------------
    // The audio core has published each chain's novelty; HERE is where a beat
    // becomes a beat, and it becomes one per PATTERN: each listener compares its own
    // chain against its own threshold, with its own minimum gap, and acts on its own
    // port after the display offset. A VU or EQ port is not a listener at all.
    listeningStep();
    bool doBeat240 = beatFired240;

    unsigned long tA = micros();
    runTimelineSequencer();
    unsigned long tB = micros();
    dbgStage = 6;
    run240VDisplayPattern(doBeat240);
    dbgStage = 7;
    unsigned long tC = micros();
    runLEDPattern();
    unsigned long tD = micros();

    // PER-STAGE WORST CASE. A row change happens INSIDE runTimelineSequencer, a transmit inside
    // runLEDPattern, and a flash erase freezes both cores so it shows in the whole-pass figure.
    // /diag reports the worst of each since the last read - the log cannot be read during a show.
    uint32_t usSeq = (uint32_t)(tB - tA);
    uint32_t usLed = (uint32_t)(tD - tC);
    if (usSeq > g_maxSeqUs) g_maxSeqUs = usSeq;
    if (usLed > g_maxLedUs) g_maxLedUs = usLed;
  }
  unsigned long dtUs = micros() - t0;
  if (dtUs > g_maxLoopUs) g_maxLoopUs = (uint32_t)dtUs;
  if (dtUs > 15000UL) g_hitchCount++;
  if (dtUs > 15000UL && Serial.availableForWrite() > 80)
    Serial.printf("[slow] render loop took %lu ms\n", dtUs / 1000UL);

  // Deferred NVS write for the brightness slider (see handleBrightness). The
  // actual write touches flash and can stall, so it is handed to the audio task
  // (core 0): nothing on the render core may ever block on flash.
  if (brSaveAt != 0 && (long)(millis() - brSaveAt) >= 0) {
    brSaveAt = 0;
    saveSettingsPending = true;
  }

  // --- one-off BOOT REPORT on IO48: the previous reset reason, readable with no PC ---
  // setup() has just done the 3 fast power-up flashes. Then, 1.5 s in:
  //   ONE LONG 1 s flash  = "this is the boot report"
  //   1 s pause, then the reason as 500 ms slow blinks:
  //   1=POWER_ON 2=EXT_PIN 3=SOFTWARE 4=PANIC 5=INT_WDT 6=TASK_WDT 7=WDT 9=BROWNOUT
  //   11=USB 12=JTAG.  (Reason 0 = unknown: the marker flash appears alone.)
  //
  // IT IS IMMUNE. It used to be CANCELLED by any command blink, so "4 flashes" could be
  // /init-off (fast) or a PANIC (slow) - and a whole test round was lost to exactly that
  // reading. It now takes IO48 until it finishes; the command blinker waits its turn.
  static uint8_t bcPhase = 0;      // 0 waiting 1 marker ON 2 gap 3 counting 4 finished
  static int     bcLeft  = 0;
  static bool    bcOn    = false;
  static unsigned long bcNext = 0;
  unsigned long bcNow = millis();
  if (bcPhase == 0) {
    if (bcNext == 0) bcNext = bcNow + 1500;
    else if ((long)(bcNow - bcNext) >= 0) {
      statusBlinks = 0;                                   // the report owns IO48 now
      bcPhase = 1; bcOn = true; bcNext = bcNow + 1000;    // the marker
      digitalWrite(POWER_LED_PIN, HIGH);
    }
  } else if ((long)(bcNow - bcNext) >= 0) {
    if (bcPhase == 1) {                                   // marker ends
      bcOn = false; digitalWrite(POWER_LED_PIN, LOW);
      bcLeft = g_prevReason; bcPhase = 2; bcNext = bcNow + 500;
    } else if (bcPhase == 2) {                            // gap ends
      if (bcLeft <= 0) bcPhase = 4;
      else { bcOn = true; bcPhase = 3; bcNext = bcNow + 500;
             digitalWrite(POWER_LED_PIN, HIGH); }
    } else {                                              // a reasons blink ends
      bcOn = false; digitalWrite(POWER_LED_PIN, LOW);
      if (--bcLeft <= 0) bcPhase = 4;
      else { bcPhase = 2; bcNext = bcNow + 500; }
    }
  }

  // --- status blinks on IO48 (serial-free diagnostics) ---
  dbgStage = 3;
  // Yields to the boot report (bcPhase 1-3) - the two must never drive IO48 at once.
  if (statusBlinks > 0 && (bcPhase == 0 || bcPhase >= 4) && millis() >= statusNext) {
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
    logAppend("hb up=%us heap=%u min=%u shows=%lu display=%d out=%s fs=%.0f bass=%.2f blvl=%.2f env=%.2f mid=%.2f treb=%.2f lvl=%.2f",
              (unsigned)(millis() / 1000), (unsigned)ESP.getFreeHeap(),
              (unsigned)ESP.getMinFreeHeap(), (unsigned long)ledShowCount,
              displayActive ? 1 : 0,
              "fastled",
              (double)audioFs, (double)audioBass, (double)audioBassLvl, (double)g_bassLvlEnv,
              (double)audioMid, (double)audioTreble, (double)audioLevel);

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

    // The EXTRA-beat intervals, so the doubling is visible in the intervals too and
    // not only in the counts below.
    logAppend("firesx %lu %lu %lu %lu %lu %lu %lu %lu",
              fireIntX[0], fireIntX[1], fireIntX[2], fireIntX[3],
              fireIntX[4], fireIntX[5], fireIntX[6], fireIntX[7]);

    // BEAT TAGGING + PATTERN REPORT. One line, because it settles two questions at
    // once:
    //   real/extra - the tagged beats the audio layer delivered this period.
    //   flips      - reversals actually APPLIED, per display. LED multiplier OFF
    //                should track real; ON should be about real+extra. That is the
    //                doubling, measured rather than felt.
    //   pat/tl     - which pattern each section was ASKED for, which render branch
    //                each port ACTUALLY has (one digit per port, 0 = moving dot, so
    //                a column of 4s = explosion), and whether a TIMELINE is
    //                overriding them. This is the "every pattern looks like a moving
    //                dot" question, answered from the board itself.
    char pats[NUM_PORTS + 1];
    for (int p = 0; p < NUM_PORTS; p++) pats[p] = (char)('0' + (int)portCfg[p].pat);
    pats[NUM_PORTS] = 0;
    logAppend("beats onsets=%lu | acts real=%lu extra=%lu | flips led=%lu 240=%lu | kicks=%lu | pat led=%s 240=%s tl=%d ports=%s",
              (unsigned long)g_onsetReal, (unsigned long)g_beatReal, (unsigned long)g_beatExtra,
              (unsigned long)g_flipLED, (unsigned long)g_flip240, (unsigned long)g_kickLED,
              activePatternLED.c_str(), activePattern240V.c_str(),
              tlLoaded ? 1 : 0, pats);
    g_beatReal = g_beatExtra = g_flipLED = g_flip240 = g_kickLED = g_onsetReal = 0;

    // LISTENERS. 'portchain' is the CHAIN each port listens on - 9 means "not a
    // listener" (a VU or EQ port, or a pattern that does not act on beats) - and
    // 'taken' is how many beats each port actually acted on. Two ports listening to
    // different bands or with different sensitivities show up here as different
    // chains and different counts, from ONE log read.
    char pch[NUM_PORTS + 1];
    for (int p = 0; p < NUM_PORTS; p++)
      pch[p] = (char)('0' + (beatListen[p].active ? beatListen[p].chain : 9));
    pch[NUM_PORTS] = 0;
    logAppend("listen chains=%d cap=%u | portchain=%s | taken %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu",
              (int)__builtin_popcount((unsigned)odfChainUse), (unsigned)g_chainOverflow, pch,
              (unsigned long)beatListen[0].taken, (unsigned long)beatListen[1].taken,
              (unsigned long)beatListen[2].taken, (unsigned long)beatListen[3].taken,
              (unsigned long)beatListen[4].taken, (unsigned long)beatListen[5].taken,
              (unsigned long)beatListen[6].taken, (unsigned long)beatListen[7].taken,
              (unsigned long)beatListen[8].taken, (unsigned long)beatListen[9].taken);
    for (int p = 0; p <= NUM_PORTS; p++) beatListen[p].taken = 0;
    g_chainOverflow = 0;

    // PER-PORT COLOUR AND WHERE IT CAME FROM. Two strips showing the same colour is
    // otherwise unexplainable from the log:
    //   'C' = a timeline cell owns this port and supplied its colour
    //   'P' = the port fell back to the LED page
    // So: both 'P' means the CELLS carry no colour at all; both 'C' with the same value
    // means the same item was placed in both cells; different 'C' values with matching
    // strips would point at the render/strip side instead.
    char cols[NUM_PORTS * 9 + 1];
    int  cn = 0;
    for (int p = 0; p < NUM_PORTS && cn < (int)sizeof(cols); p++)
      cn += snprintf(cols + cn, sizeof(cols) - cn, "%06lx%c ",
                     (unsigned long)(portCfg[p].colour[0] & 0xFFFFFFUL),
                     portCellOwned[p] ? 'C' : 'P');
    logAppend("colours 1st=%s", cols);

    // And what the CELLS themselves hold for the CURRENT row, so "this cell has no
    // colour stored" is distinguishable from "this cell holds the same as its neighbour".
    // 'y' = the cell carries a colour, 'n' = it carries none, '--------' = empty cell.
    char cels[NUM_PORTS * 9 + 1];
    int  en = 0;
    for (int c = 1; c <= NUM_PORTS && en < (int)sizeof(cels); c++) {
      if (tlData[tlRow][c].used)
        en += snprintf(cels + en, sizeof(cels) - en, "%06lx%c ",
                       (unsigned long)(tlData[tlRow][c].colour[0] & 0xFFFFFFUL),
                       tlData[tlRow][c].hasColour ? 'y' : 'n');
      else
        en += snprintf(cels + en, sizeof(cels) - en, "-------- ");
    }
    // The row's elapsed time rides on the same line, so the playback line the page draws
    // can be checked from the serial log alone, without a phone: t/len in milliseconds.
    logAppend("cells row%d=%s t=%lums/%lums", tlRow + 1, cels,
              tlPlaying ? (unsigned long)(millis() - tlRowStart) : 0UL,
              (unsigned long)(tlDelaySec * 1000.0f));

    // MOTION: each port's position and direction. Two ports with identical settings
    // must read the SAME here - that is what makes programming port-agnostic, and it is
    // how "the two strips are in mirror image" is proved rather than guessed at.
    char mot[NUM_PORTS * 7 + 1];
    int  mn = 0;
    // A DARK port prints "D" in place of its position. That makes "which strips are blanked
    // in this slot" readable straight off the log, which is the only way to check the
    // empty-cell rule without a phone in front of the strips.
    for (int p = 0; p < NUM_PORTS && mn < (int)sizeof(mot); p++) {
      if (portCfg[p].dark) mn += snprintf(mot + mn, sizeof(mot) - mn, "D  ");
      else mn += snprintf(mot + mn, sizeof(mot) - mn, "%d%c ",
                          ledPosArr[p], ledDirArr[p] > 0 ? '>' : '<');
    }
    logAppend("motion play=%d pos=%s", tlPlaying ? 1 : 0, mot);

    // THE CHASE KICK, so a kick can be verified from the log alone: the level driving it, the
    // rolling speed, the kick speed EFFECT asks for, and how much of the DISTANCE credit is left.
    // Printed for the FIRST wrap-OFF chase port - the one a single-strip test would use.
    for (int p = 0; p < NUM_PORTS; p++) {
      if (!isChaseKick(p)) continue;
      logAppend("chase kick p=%d env=%.2f n=%.2f bs=%.2f roll=%.0f spd=%.0f eff=%d cr=%.1f/%d mom=%.2f coast=%.0f",
                p, (double)g_bassLvlEnv, (double)chaseNlast[p], (double)portCfg[p].beatSens,
                (double)chaseSpeed(p), (double)chaseKickSpeed(p),
                (int)portCfg[p].effect, (double)chaseCredit[p],
                (int)constrain(portCfg[p].distance, 1, 50), (double)portCfg[p].momentum,
                (double)chaseCoast[p]);
      break;
    }

    // THE MOVING DOT EFFECT, per port, in the same compact style: flips since the last whole
    // pixel / whole pixels lost / live debris pieces. An ERASED dot (lost == its Size) is
    // otherwise invisible in the log - it simply draws nothing.
    {
      char ef[NUM_PORTS * 12 + 1];
      int en = 0;
      for (int p = 0; p < NUM_PORTS && en < (int)sizeof(ef); p++) {
        int live = 0;
        for (int k = 0; k < MD_FRAGS; k++) if (mdFrags[p][k].alive) live++;
        en += snprintf(ef + en, sizeof(ef) - en, "%u/%u/%d ",
                       (unsigned)mdFlips[p], (unsigned)mdLost[p], live);
      }
      logAppend("effect flips/lost/frags %s", ef);
      // The SPEEDS and a live piece, so "do they move?" is answered by the log rather than
      // guessed: dot = the dot's own px/s, cap = the visibility limit applied, and p/v/b = the
      // first live piece's position, speed and brightness of port 1.
      {
        int pp = -1; float pv = 0.0f, pb = 0.0f, pt = 0.0f;
        for (int k = 0; k < MD_FRAGS; k++) {
          if (mdFrags[0][k].alive) {
            pp = (int)(mdFrags[0][k].posf + 0.5f); pv = mdFrags[0][k].speed;
            pb = mdFrags[0][k].bright; pt = mdFrags[0][k].moved;
            break;
          }
        }
        logAppend("effect speeds dot=%.0f mom=%.2f | piece1 p=%d v=%.0f b=%.2f t=%.0f",
                  (double)mdLastDotPxS, (double)mdLastMomFrac, pp, (double)pv, (double)pb,
                  (double)pt);
      }
    }

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
