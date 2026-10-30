// ============================================================================
//  Fresh_Beat.ino  -  standalone beat follower for the ESP32-S3 light show
// ----------------------------------------------------------------------------
//  Written FRESH for the requirement in Audio.docx.  NOTHING is carried over
//  from any earlier sketch in this project: the project has been plagued by old
//  code reintroducing variables that then have to be found and removed.
//
//  REQUIREMENT (Audio.docx):
//    * Flash IO48 for 50 ms on the beat of the disco music, 80-180 BPM.
//    * Silence must leave IO48 off.
//    * Number the IO48 flashes internally 1..4 and repeat.
//    * On counts 1 and 3, flash Channel 10 for 50 ms.
//    * BEAT      : true only when the beat hits, timed to the MIDDLE of the
//                  beat (not the leading edge), 25 ms window about that centre.
//    * BEAT_STEP : as BEAT but only on counts 1 and 3.
//    * No internal timer drives the LEDs - a timer drifts off the music.
//    * Predictive timing, ONE beat only, from the previous beat's timing.
//    * On a track change, flash at the new track's BPM as quickly as possible.
//    Unacceptable: missed beats, timing drift, incorrect timing.
//
//  AGREED WITH THE USER:
//    * Channel 10 marks counts 1 and 3 ONLY.  Audio.docx governs the hardware
//      document on this point.
//    * Count 1 is the first beat heard.
//    * Silence = TWO consecutive missed beats -> IO48 off and counter reset.
//      (A correct system should never miss even the first beat.)
//    * Low-band detection to start; broadband is the fallback if bass-only
//      proves insufficient.
//    * Spec name "BEAT-STEP"; the code spelling is BEAT_STEP, because a hyphen
//      is the subtraction operator and is not legal in a C identifier.
//    * Counters are read over USB serial.  Wi-Fi is NOT used.
//
//  HOW A BEAT IS TIMED (the one design point that matters here):
//    A beat is the rise and fall of the low-band envelope through a sensitivity
//    line.  The LEADING EDGE is where it rises through; the beat's MIDDLE is a
//    fixed point just AFTER that leading edge.  The flash, and the BEAT /
//    BEAT_STEP windows, are timed to that MIDDLE - never to the leading edge.
//    The offset is taken from the beat-to-beat PERIOD (Audio.docx: predictive
//    timing "based on previous beat to beat timing"), so it is IDENTICAL every
//    beat while the tempo is steady and re-places itself within one beat on a
//    track change.  The grid is never a free clock: it is re-derived from the
//    music, and the beat-to-beat PERIOD also drives the missed-beat rule.
//
//  HARDWARE (HARDWARE_CONNECTION.md):
//    AUDIO = GPIO 2   MAX9814 microphone, ANALOGUE, ADC1 channel 1
//    IO48  = GPIO 48  onboard LED, 100 ms pulse is clearly visible
//    CH10  = GPIO 42  240V relay channel 10, HIGH = relay on
//    The mic idles near mid-rail (~2048 on a 12-bit ADC).  Silence is only
//    1-3 counts and a bass beat 5-30 counts, so the WHOLE usable signal is a
//    few tens of counts.  Every threshold here is RELATIVE to the signal's own
//    measured range - never a fixed count.
//
//  BUILD: esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=huge_app,
//         FlashMode=qio,USBMode=hwcdc,CDCOnBoot=cdc
//         (CDCOnBoot=cdc is REQUIRED or Serial goes to UART0 and the USB log
//          is silent - only the ROM banner would be seen.)
//  PORT : COM5  (close every other holder of COM5 before uploading)
// ============================================================================

#include <Arduino.h>
#include <math.h>

// ---------------------------------------------------------------------------
//  PINS
// ---------------------------------------------------------------------------
static const uint8_t PIN_AUDIO = 2;
static const uint8_t PIN_IO48  = 48;
static const uint8_t PIN_CH10  = 42;

// ---------------------------------------------------------------------------
//  OUTPUT TIMING
// ---------------------------------------------------------------------------
static const uint16_t IO48_MS     = 50;    // IO48 pulse length, per beat
static const uint16_t CH10_MS     = 50;    // Channel 10 pulse length
static const uint16_t BEAT_WIN_MS = 25;    // window about the beat centre
static const int32_t  BEAT_WIN_HALF_US = (BEAT_WIN_MS * 1000L) / 2;

// ---------------------------------------------------------------------------
//  TEMPO LIMITS (80-180 BPM)
// ---------------------------------------------------------------------------
static const uint32_t BPM_MIN = 80;
static const uint32_t BPM_MAX = 180;
static const uint32_t PERIOD_MIN_US = 60000000UL / BPM_MAX;  // ~333333 us
static const uint32_t PERIOD_MAX_US = 60000000UL / BPM_MIN;  // ~750000 us

// THE GRID, AND HOW A TRACK CHANGE IS HANDLED (the user's three steps):
//   1. MAINTAIN the existing beat.
//   2. LOOK for the transition beat, but keep maintaining the existing beat.
//   3. New beat found -> transition to it AS FAST AS POSSIBLE.
// NORMAL (locked): the grid fires the beats and a detection only steers it, and
// only from INSIDE a tight window.  MEASURED: the old +/-1/3 window (~135-150 ms)
// let off-beat onsets ~100-150 ms away steer the grid, and the phase sat 10-46 ms
// off the beat - the "a little slow" and "never on time" sections.
// CAPTURE (transition): entered when the music stops matching the grid; the grid
// KEEPS FIRING on the established beat (steps 1 and 2) while a fresh interval
// histogram is collected from the detections.  The moment it is consistent the
// grid jumps to the new tempo, one beat ahead (step 3).  This is automatic - no
// gain is set by hand - and it also removes the old 13-second tempo walk.
static const float   PHASE_K            = 0.35f;  // NORMAL phase correction per detection
static const float   PERIOD_K           = 0.05f;  // NORMAL period correction per detection
static const int32_t PHASE_TOL_DIV      = 6;      // NORMAL window = period / 6 (~85 ms)
static const uint8_t CAPTURE_ENTER_REJ  = 6;      // consecutive SAME-SIGN rejects -> CAPTURE
static const uint8_t CAPTURE_LOCK_N     = 4;      // intervals needed to adopt a new beat
static const uint32_t CAPTURE_TIMEOUT_MS = 6000;  // give up looking, adopt what we have
// A DROPOUT (an audio glitch - a momentary pause) is a PHASE DISCONTINUITY, not
// silence.  It must be treated like a new track: follow the beat as it now is.
// MEASURED fault: a Wi-Fi glitch paused the music for a split second; the grid kept
// firing on the old phase and never re-locked, because a phase jump near half a
// beat is AMBIGUOUS and so the same-sign reject test never tripped.  A missed beat
// is therefore its own trigger, and it is well inside the silence timeout.
static const float GLITCH_MISS_FACTOR = 1.5f;     // no detection for 1.5 periods = dropout
static const float SILENCE_FACTOR     = 2.6f;     // no detection for 2.6 periods = silence
static const float   LOCK_SPREAD        = 1.15f;  // intervals within this spread = consistent

// THE FLASH OFFSET.  Audio.docx says the timing comes from "previous beat to beat
// timing", so the offset is taken from the PERIOD - not from a measured beat
// length.  MEASURED: the envelope half-decay is CONTENT DEPENDENT (115-272 ms
// within one track), which made the flash wander 56-130 ms and jump up to 61 ms
// between beats while the tempo was rock steady.  That was the drift.  A period
// based offset is IDENTICAL every beat while the tempo is steady, and re-places
// itself within one beat when the track changes - which the document requires.
//   MODE B (try first): a FIXED absolute time - the middle of the kick
//   MODE A            : a fixed FRACTION of the period - scales with the tempo
static const uint8_t  OFFSET_MODE_B     = 0;
static const uint8_t  OFFSET_MODE_A     = 1;
static const uint8_t  FLASH_OFFSET_MODE = OFFSET_MODE_B;
static const uint32_t FLASH_OFFSET_US   = 70000;   // mode B: 70 ms
static const float    FLASH_OFFSET_K    = 0.14f;   // mode A: 14 % of the period
static const uint8_t  LEN_N             = 5;       // beat-length history, TELEMETRY ONLY

// ---------------------------------------------------------------------------
//  DETECTOR TUNABLES
// ---------------------------------------------------------------------------
static const bool  USE_LOW_BAND = true;    // false = broadband (the fallback)
static const float BP_HP_HZ = 40.0f;       // kick / bass fundamentals
static const float BP_LP_HZ = 150.0f;
static const float BP_Q     = 0.7071f;     // Butterworth

static const uint32_t SAMPLE_BATCH = 16;   // samples per loop pass

// The refractory ALSO enforces the BPM band: an edge closer than 300 ms to the
// previous accepted beat is faster than 180 BPM and is refused outright, so a
// decay tail or a ghost cannot become a beat.
static const uint32_t REFRACTORY_US = 300000;
static const uint32_t LEN_MIN_US    = 15000;   // plausible beat length
static const uint32_t LEN_MAX_US    = 500000;
static const uint8_t  MISS_COUNT_FOR_SILENCE = 2;
// The MAX9814 produces a power-on surge: measured rp = 329 -> 177 -> 65 over the
// first ~2 s before settling near 30.  Beats are ignored during this window so
// the surge cannot become one phantom flash at switch-on.
static const uint32_t STARTUP_SETTLE_MS = 2500;

static const float DC_TAU_S          = 0.30f;   // DC tracker time constant
static const float ENV_RELEASE_TAU_S = 0.150f;  // envelope release ~150 ms
static const float PEAK_TAU_S        = 0.35f;   // slow peak tracker
static const float TROUGH_TAU_S      = 0.60f;   // slow trough tracker (telemetry only)

// --- NOVELTY (rise) DETECTION -------------------------------------------------
// A LEVEL-CROSSING detector cannot do this job: on Billy Jean the SUSTAINED bass
// held the envelope above the line, so the detector stayed "above" and missed the
// next kick (MEASURED: det rate fell, the period doubled, the tempo lurched to
// 83 BPM).  A kick is a RISE - the envelope jumping above its own slow baseline -
// and sustained bass is not.  So the onset is the novelty (rise) crossing its own
// adaptive level.
static const float SLOW_TAU_S      = 0.20f;   // envelope baseline lag
static const float NOV_AVG_TAU_S   = 1.00f;   // adaptive novelty level
static const float NOV_MARGIN      = 1.80f;   // beat when novelty > avg * margin
static const float NOV_FLOOR       = 3.0f;    // absolute minimum novelty
static const float BEAT_HALF_DECAY = 0.50f;   // beat ends at half its envelope peak
// SILENCE GATE.  MEASURED on this board with and without music playing:
//   silence (no music)      rp = 27-42   raw counts, envelope pk = 6.6-8.7
//   Billy Jean playing      rp = 375-631 raw counts, envelope pk = 134-367
// So the room noise alone sits near 30 counts - the earlier gate of 4 was far
// below it and the board flashed in silence.  120 sits between the two regimes.
static const float SILENCE_RAW_MIN   = 120.0f;
static const float RAW_TAU_S         = 1.0f;    // raw peak hold decay, ~1 s

static const uint8_t N_INTERVALS = 7;           // period histogram depth

// ---------------------------------------------------------------------------
//  BIQUAD  (RBJ cookbook), floating point.  The S3 has an FPU, so the filters
//  are written plainly rather than in fixed point.
// ---------------------------------------------------------------------------
struct Biquad {
  float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f, z1 = 0.0f, z2 = 0.0f;

  inline float process(float x) {
    const float y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
  }

  void designLowPass(float f0, float Q, float fs) {
    const float w0 = 2.0f * PI * f0 / fs;
    const float cw = cosf(w0), sw = sinf(w0);
    const float alpha = sw / (2.0f * Q);
    const float a0 = 1.0f + alpha;
    b0 = ((1.0f - cw) * 0.5f) / a0;
    b1 = (1.0f - cw) / a0;
    b2 = b0;
    a1 = (-2.0f * cw) / a0;
    a2 = (1.0f - alpha) / a0;
    z1 = z2 = 0.0f;
  }

  void designHighPass(float f0, float Q, float fs) {
    const float w0 = 2.0f * PI * f0 / fs;
    const float cw = cosf(w0), sw = sinf(w0);
    const float alpha = sw / (2.0f * Q);
    const float a0 = 1.0f + alpha;
    b0 = ((1.0f + cw) * 0.5f) / a0;
    b1 = (-(1.0f + cw)) / a0;
    b2 = b0;
    a1 = (-2.0f * cw) / a0;
    a2 = (1.0f - alpha) / a0;
    z1 = z2 = 0.0f;
  }
};

// ---------------------------------------------------------------------------
//  FILTERS AND SAMPLE RATE
// ---------------------------------------------------------------------------
static Biquad hpf, lpf;
static float  fsMeasured  = 24000.0f;   // measured in setup, never assumed
static float  usPerSample = 41.67f;

// ---------------------------------------------------------------------------
//  DC REMOVAL AND ENVELOPE
// ---------------------------------------------------------------------------
static float dcMean    = 2048.0f, dcCoef     = 0.0f;
static float env       = 0.0f,    envRelease = 0.0f;
static float peakEnv   = 0.0f,    peakCoef   = 0.0f;
static float troughEnv = 0.0f,    troughCoef = 0.0f;
static float rawPeak   = 0.0f,    rawDecay   = 0.0f;   // silence gate
static bool  wasAbove  = false;

// novelty detection state
static float envSlow   = 0.0f, slowCoef   = 0.0f;   // envelope baseline
static float novAvg    = 0.0f, novAvgCoef = 0.0f;   // adaptive novelty level
static float novelty   = 0.0f, novThresh  = 0.0f;
static bool  beatOpen  = false;                     // beat length being measured
static float peakSince = 0.0f;                      // envelope peak since onset

// ---------------------------------------------------------------------------
//  BEAT STATE
// ---------------------------------------------------------------------------
static uint8_t           toneIndex = 0;   // 0 = not started; 1..4 while running
static uint32_t          lastLeadUs = 0;
static uint32_t          avgLenUs   = 0;  // running average beat length
static uint32_t          periodUs   = 0;  // median beat-to-beat period
static volatile uint32_t bpmX10     = 0;

// ---------------------------------------------------------------------------
//  PERIOD HISTOGRAM  (median of the recent valid intervals)
// ---------------------------------------------------------------------------
static uint32_t intervalRing[N_INTERVALS];
static uint8_t  intervalCount = 0;
static uint8_t  intervalHead  = 0;

// --- THE GRID (phase-locked to the detections) ------------------------------
static bool     gridLocked  = false;
static uint32_t gridPeriod  = 0;   // us
static uint32_t nextOnsetUs = 0;   // predicted leading edge of the next beat
static uint32_t lastDetUs   = 0;   // last detection (drives the silence rule)
static volatile uint32_t detections = 0;

// transition handling - the user's three steps (maintain, look, jump)
static bool     gridCapturing  = false;
static uint8_t  rejRun         = 0;   // consecutive rejected detections
static int8_t   rejSign        = 0;   // sign of that run (+1 / -1)
static uint32_t captureStartMs = 0;
static bool     captureFirstDet = false;   // first detection after entering CAPTURE

// --- STABLE FLASH OFFSET: history of measured beat lengths -------------------
static uint32_t lenRing[LEN_N];
static uint8_t  lenCount = 0, lenHead = 0;

// --- per-beat instrument ----------------------------------------------------
static volatile int32_t lastPhaseErrUs = 0;   // signed, detection - grid
static volatile uint8_t lastPhaseState = 0;   // 0 none, 1 accepted, 2 rejected

// ---------------------------------------------------------------------------
//  PENDING CENTRE FLASH AND BEAT WINDOW
// ---------------------------------------------------------------------------
static uint32_t centreFireUs   = 0;
static bool     centrePending  = false;
static bool     beatStepIsThis = false;
static uint32_t beatWinStartUs = 0;
static uint32_t beatWinEndUs   = 0;

// ---------------------------------------------------------------------------
//  OUTPUT PULSES
// ---------------------------------------------------------------------------
static bool     io48Active = false, ch10Active = false;
static uint32_t io48OffUs  = 0,     ch10OffUs  = 0;

// ---------------------------------------------------------------------------
//  MISS / SILENCE
// ---------------------------------------------------------------------------
static uint32_t expectedLeadUs = 0;
static bool     awaitingBeat   = false;
static uint8_t  missCount      = 0;
static bool     silence        = true;

// ---------------------------------------------------------------------------
//  PUBLISHED VARIABLES AND COUNTERS
// ---------------------------------------------------------------------------
static volatile bool     BEAT      = false;   // spec variable
static volatile bool     BEAT_STEP = false;   // spec variable (spec name BEAT-STEP)
static volatile uint8_t  beatCount = 0;
static volatile uint32_t beatsDetected = 0;
static volatile uint32_t io48Flashes   = 0;
static volatile uint32_t ch10Flashes   = 0;
static volatile uint32_t beatsRefused  = 0;
static volatile uint32_t missEvents    = 0;
static volatile uint32_t silenceEvents = 0;
static uint32_t lastPublishMs = 0;

// ---------------------------------------------------------------------------
//  PROTOTYPES  (kept explicit so nothing depends on IDE-injected prototypes)
// ---------------------------------------------------------------------------
static void     processSample(int raw, uint32_t t);
static void     onLeadingEdge(uint32_t t);
static void     onTrailingEdge(uint32_t t);
static void     scheduleCentre(uint32_t centreUs);
static void     enterSilence();
static void     pushInterval(uint32_t iv);
static uint32_t medianPeriod();
static float    periodSpread();
static uint32_t medianLen();
static uint32_t flashOffsetUs();
static float    measureSampleRate();

// ---------------------------------------------------------------------------
//  TIME HELPERS  (micros() wraps about every 71 minutes)
// ---------------------------------------------------------------------------
static inline bool timeReached(uint32_t now, uint32_t t) {
  return (int32_t)(now - t) >= 0;
}

// ---------------------------------------------------------------------------
//  SAMPLE-RATE MEASUREMENT  -  measured, never assumed.  A 12 kHz microphone
//  needs at least 24 kHz, so the real figure matters and is reported.
// ---------------------------------------------------------------------------
static float measureSampleRate() {
  const uint32_t windowUs = 200000;   // 200 ms of straight ADC reads
  const uint32_t t0 = micros();
  uint32_t n = 0;
  while ((uint32_t)(micros() - t0) < windowUs) {
    analogRead(PIN_AUDIO);
    n++;
  }
  return (float)n * 1000000.0f / (float)windowUs;
}

// ---------------------------------------------------------------------------
//  PERIOD HISTOGRAM  -  the median of the recent valid intervals, so a single
//  odd interval (a missed or doubled beat) cannot move the period.
// ---------------------------------------------------------------------------
static void pushInterval(uint32_t iv) {
  intervalRing[intervalHead] = iv;
  intervalHead = (uint8_t)((intervalHead + 1) % N_INTERVALS);
  if (intervalCount < N_INTERVALS) intervalCount++;
}

static uint32_t medianPeriod() {
  if (intervalCount == 0) return 0;
  uint32_t tmp[N_INTERVALS];
  for (uint8_t i = 0; i < intervalCount; i++) tmp[i] = intervalRing[i];
  for (uint8_t i = 1; i < intervalCount; i++) {   // insertion sort, tiny array
    const uint32_t v = tmp[i];
    int8_t j = (int8_t)i - 1;
    while (j >= 0 && tmp[j] > v) { tmp[j + 1] = tmp[j]; j--; }
    tmp[j + 1] = v;
  }
  return tmp[intervalCount / 2];
}

// The spread (max/min) of the collected intervals.  Near 1.0 means the tempo is
// steady and the grid may lock.
static float periodSpread() {
  if (intervalCount == 0) return 99.0f;
  uint32_t mn = 0xFFFFFFFFu, mx = 0;
  for (uint8_t i = 0; i < intervalCount; i++) {
    const uint32_t v = intervalRing[i];
    if (v < mn) mn = v;
    if (v > mx) mx = v;
  }
  if (mn == 0) return 99.0f;
  return (float)mx / (float)mn;
}

// The MEDIAN of the recent measured beat lengths - resists the odd long beat, so
// the flash offset stops jumping beat to beat.
static uint32_t medianLen() {
  if (lenCount == 0) return 0;
  uint32_t tmp[LEN_N];
  for (uint8_t i = 0; i < lenCount; i++) tmp[i] = lenRing[i];
  for (uint8_t i = 1; i < lenCount; i++) {
    const uint32_t v = tmp[i];
    int8_t j = (int8_t)i - 1;
    while (j >= 0 && tmp[j] > v) { tmp[j + 1] = tmp[j]; j--; }
    tmp[j + 1] = v;
  }
  return tmp[lenCount / 2];
}

// The flash offset - the point inside the beat at which the flash is timed - taken
// from the PERIOD so it cannot drift (see the note at the constants).
static uint32_t flashOffsetUs() {
  if (FLASH_OFFSET_MODE == OFFSET_MODE_A && periodUs) {
    return (uint32_t)((float)periodUs * FLASH_OFFSET_K);
  }
  return FLASH_OFFSET_US;   // mode B, or mode A before the period is known
}

// ---------------------------------------------------------------------------
//  SCHEDULE A FLASH AT A BEAT'S MIDDLE, and open its BEAT / BEAT_STEP window
// ---------------------------------------------------------------------------
static void scheduleCentre(uint32_t centreUs) {
  centreFireUs   = centreUs;
  centrePending  = true;
  beatWinStartUs = centreUs - BEAT_WIN_HALF_US;
  beatWinEndUs   = centreUs + BEAT_WIN_HALF_US;
  beatStepIsThis = (toneIndex == 1 || toneIndex == 3);
}

// ---------------------------------------------------------------------------
//  SILENCE  -  IO48 off, counter reset, period cleared so a new track is
//  measured from scratch the moment it starts.
// ---------------------------------------------------------------------------
static void enterSilence() {
  silence       = true;
  missCount     = 0;
  awaitingBeat  = false;
  lastLeadUs    = 0;
  toneIndex     = 0;
  beatCount     = 0;
  periodUs      = 0;
  bpmX10        = 0;
  avgLenUs      = 0;
  lenCount      = 0;
  lenHead       = 0;
  intervalCount = 0;
  intervalHead  = 0;
  beatOpen      = false;
  gridLocked    = false;
  gridPeriod    = 0;
  nextOnsetUs   = 0;
  lastDetUs     = 0;
  gridCapturing = false;
  captureFirstDet = false;
  rejRun        = 0;
  rejSign       = 0;
  centrePending = false;
  beatWinEndUs  = 0;
  BEAT          = false;
  BEAT_STEP     = false;
  digitalWrite(PIN_IO48, LOW); io48Active = false;
  digitalWrite(PIN_CH10, LOW); ch10Active = false;
  silenceEvents++;
}

// ---------------------------------------------------------------------------
//  LEADING EDGE  -  the envelope has risen through the sensitivity line: a
//  beat has begun.  This is NOT where the flash is fired; the flash waits for
//  the MIDDLE of the beat.
// ---------------------------------------------------------------------------
static void onLeadingEdge(uint32_t t) {
  // ignore the microphone power-on surge (see STARTUP_SETTLE_MS)
  if ((uint32_t)millis() < STARTUP_SETTLE_MS) { beatsRefused++; return; }

  const uint32_t prevDet = lastDetUs;
  // refractory: an edge arriving impossibly close to the last one is a double
  // trigger on a decay tail, not a beat.
  if (prevDet != 0 && (uint32_t)(t - prevDet) < REFRACTORY_US) { beatsRefused++; return; }

  detections++;
  lastDetUs    = t;
  silence      = false;
  missCount    = 0;
  awaitingBeat = false;

  if (!gridLocked) {
    // ---- ACQUIRE: collect intervals until they are consistent, then lock ----
    if (prevDet != 0) {
      const uint32_t iv = t - prevDet;
      if (iv >= PERIOD_MIN_US && iv <= PERIOD_MAX_US) {
        pushInterval(iv);
        periodUs = medianPeriod();
        bpmX10   = periodUs ? (600000000UL / periodUs) : 0;
      }
    }
    if (intervalCount >= N_INTERVALS && periodSpread() <= LOCK_SPREAD) {
      gridPeriod  = periodUs;
      nextOnsetUs = t + gridPeriod;
      gridLocked  = true;
    }
    // while acquiring, flash per detection so the show starts immediately
    toneIndex = (uint8_t)((toneIndex % 4) + 1);
    beatCount = toneIndex;
    beatsDetected++;
    // the beat-length measurement starts here (during acquisition, every detection)
    lastLeadUs = t;
    beatOpen   = true;
    peakSince  = env;
    const uint32_t offset = flashOffsetUs();
    scheduleCentre(t + offset);
    if (periodUs) { expectedLeadUs = t + periodUs; awaitingBeat = true; }
    return;
  }

  // ---- LOCKED: STEER the grid.  The GRID fires the beats, so a stray or a
  //      missing detection can neither move a flash nor shift the 1..4 count. --
  if (gridCapturing) {
    // STEPS 1 and 2: the grid KEEPS FIRING on the established beat while we look
    // for the new one - and it FOLLOWS the beat as it now is, by re-anchoring the
    // phase on each plausible detection.  MEASURED: steering alone left the grid
    // parked ~180 ms off for six seconds and then a single-detection re-anchor threw
    // it the other way; and after an audio glitch the phase can land near HALF a
    // beat, where steering is ambiguous.  Following the beat removes both faults.
    lastPhaseState = 3;                                   // 3 = in transition
    lastPhaseErrUs = 0;                                   // (not meaningful in CAPTURE)
    if (prevDet != 0) {
      const uint32_t iv = t - prevDet;
      // "plausible" = this detection follows the previous by about one beat
      const bool plausible = (iv * 10u >= gridPeriod * 8u) && (iv * 10u <= gridPeriod * 13u);
      if (plausible || captureFirstDet) {
        nextOnsetUs     = t + gridPeriod;                 // follow the beat
        captureFirstDet = false;
      }
      if (iv >= PERIOD_MIN_US && iv <= PERIOD_MAX_US) pushInterval(iv);
    }
    lastLeadUs = t; beatOpen = true; peakSince = env;     // length = telemetry only
    const bool consistent = (intervalCount >= CAPTURE_LOCK_N) && (periodSpread() <= LOCK_SPREAD);
    const bool timedOut   = (uint32_t)(millis() - captureStartMs) > CAPTURE_TIMEOUT_MS;
    if (consistent || (timedOut && intervalCount >= 2)) {
      // STEP 3: adopt the new tempo AS FAST AS POSSIBLE, still following the beat.
      const uint32_t np = medianPeriod();
      if (np >= PERIOD_MIN_US && np <= PERIOD_MAX_US) {
        gridPeriod  = np;
        periodUs    = np;
        bpmX10      = 600000000UL / np;
        nextOnsetUs = t + np;                             // one beat ahead, new tempo
      }
      gridCapturing   = false;
      captureFirstDet = false;
      rejRun          = 0;
      rejSign         = 0;
      intervalCount   = 0;
      intervalHead    = 0;
      lastPhaseState  = 4;          // 4 = new tempo adopted
    }
    return;
  }

  int32_t e = (int32_t)(t - nextOnsetUs);          // signed phase error, us
  const int32_t halfP = (int32_t)(gridPeriod / 2);
  if (e >  halfP) e -= (int32_t)gridPeriod;
  else if (e < -halfP) e += (int32_t)gridPeriod;
  const int32_t tol = (int32_t)(gridPeriod / PHASE_TOL_DIV);
  if (e > -tol && e < tol) {
    rejRun = 0;
    // a plausible detection: nudge the phase and the period a little
    lastPhaseErrUs = e;
    lastPhaseState = 1;                       // accepted
    // ONLY an accepted detection may start the beat-length measurement.  A
    // rejected (spurious) onset used to start it as well, which inflated the
    // measured length and swung the flash offset - MEASURED: the offset reached
    // the 25% cap and moved 53 ms between beats while the tempo was steady.
    lastLeadUs = t;
    beatOpen   = true;
    peakSince  = env;
    nextOnsetUs = (uint32_t)((int32_t)nextOnsetUs + (int32_t)((float)e * PHASE_K));
    int32_t np = (int32_t)gridPeriod + (int32_t)((float)e * PERIOD_K);
    if (np < (int32_t)PERIOD_MIN_US) np = (int32_t)PERIOD_MIN_US;
    if (np > (int32_t)PERIOD_MAX_US) np = (int32_t)PERIOD_MAX_US;
    gridPeriod = (uint32_t)np;
    periodUs   = gridPeriod;
    bpmX10     = periodUs ? (600000000UL / periodUs) : 0;
  } else {
    lastPhaseErrUs = e;
    lastPhaseState = 2;                       // rejected: outside the tight window
    beatsRefused++;
    // A GENUINE change shows as a RUN of rejects with the SAME SIGN; random stray
    // onsets flip sign.  MEASURED: entering on 4 rejects with no sign check let the
    // controller trip on stray onsets and oscillate - the drift heard at the end of
    // a track.  Six SAME-SIGN rejects in a row are now required.
    const int8_t sgn = (e >= 0) ? 1 : -1;
    if (sgn == rejSign) { if (rejRun < 255) rejRun++; } else { rejSign = sgn; rejRun = 1; }
    if (rejRun >= CAPTURE_ENTER_REJ) {
      // STEP 1 and 2: the music no longer matches the grid.  MAINTAIN the
      // existing beat - the grid keeps firing - and start looking for the new one.
      gridCapturing   = true;
      captureFirstDet = true;
      captureStartMs  = millis();
      intervalCount   = 0;
      intervalHead    = 0;
      rejRun          = 0;
      rejSign         = 0;
    }
  }
}

// ---------------------------------------------------------------------------
//  TRAILING EDGE  -  the envelope has fallen back through the line: the beat
//  has ended.  Measure its length so the next beat's MIDDLE can be predicted.
// ---------------------------------------------------------------------------
static void onTrailingEdge(uint32_t t) {
  if (lastLeadUs == 0 || silence) return;
  const uint32_t len = t - lastLeadUs;
  if (len < LEN_MIN_US || len > LEN_MAX_US) return;
  // push into the history and take the MEDIAN, so one odd beat cannot move the
  // flash offset (the raw length is far noisier than the tempo)
  lenRing[lenHead] = len;
  lenHead = (uint8_t)((lenHead + 1) % LEN_N);
  if (lenCount < LEN_N) lenCount++;
  avgLenUs = medianLen();
}

// ---------------------------------------------------------------------------
//  ONE SAMPLE THROUGH THE DETECTOR
// ---------------------------------------------------------------------------
static void processSample(int raw, uint32_t t) {
  // 1. DC removal - the mic sits near mid-rail, so the DC must come out first
  dcMean += ((float)raw - dcMean) * dcCoef;
  float x = (float)raw - dcMean;

  // 1b. SILENCE GATE.  Track the raw AC peak with a ~1 s hold.  Below 4 counts
  //     the room is silent (measured: silence is 1-3 counts), so no beats.
  const float ar = fabsf(x);
  rawPeak *= rawDecay;
  if (ar > rawPeak) rawPeak = ar;

  // 2. low band (kick / bass), or plain broadband if the fallback is selected
  if (USE_LOW_BAND) { x = hpf.process(x); x = lpf.process(x); }
  const float rect = fabsf(x);

  // 3. envelope - fast attack, ~150 ms release
  if (rect > env) env = rect; else env *= envRelease;

  // 4. slow peak and trough - kept for the pk / tr telemetry only
  peakEnv += (env - peakEnv) * peakCoef;
  if (env > peakEnv) peakEnv = env;
  troughEnv += (env - troughEnv) * troughCoef;
  if (env < troughEnv) troughEnv = env;

  // 5. NOVELTY.  The rise of the envelope above its own slow baseline.  A kick's
  //    fast attack makes this large; a sustained bass line keeps env ~ envSlow so
  //    the novelty stays ~0 and no beat is invented.
  envSlow += (env - envSlow) * slowCoef;
  novelty = env - envSlow;
  if (novelty < 0.0f) novelty = 0.0f;

  novAvg += (novelty - novAvg) * novAvgCoef;
  novThresh = novAvg * NOV_MARGIN;
  if (novThresh < NOV_FLOOR) novThresh = NOV_FLOOR;

  // 6. BEAT LENGTH: from the onset until the envelope falls to half its peak.
  //    The MIDDLE of the beat is half of this, so the flash is never the leading
  //    edge.
  if (beatOpen) {
    if (env > peakSince) peakSince = env;
    if (env < peakSince * BEAT_HALF_DECAY) { onTrailingEdge(t); beatOpen = false; }
  }

  // 7. ONSET: the novelty rising through its adaptive level.  In silence the
  //    phrase gate is closed and no onset is declared.
  const bool music = (rawPeak > SILENCE_RAW_MIN);
  bool above = wasAbove;
  if (!music) {
    above = false;
  } else if (!above && novelty > novThresh) {
    above = true;
    onLeadingEdge(t);              // a beat has begun
  } else if (above && novelty < novThresh * 0.5f) {
    above = false;                 // re-arm for the next kick
  }
  wasAbove = above;
}

// ---------------------------------------------------------------------------
//  OUTPUT SERVICE  -  fires each beat at its MIDDLE.  Nothing here is driven by
//  a free-running clock: the instant is the beat's own measured middle.
// ---------------------------------------------------------------------------
static void serviceOutputs(uint32_t now) {
  // THE GRID fires the beats.  This is not a free-running clock: the grid is
  // phase-locked to the music and steered by every detection.
  if (gridLocked && timeReached(now, nextOnsetUs)) {
    toneIndex = (uint8_t)((toneIndex % 4) + 1);
    beatCount = toneIndex;
    beatsDetected++;
    const uint32_t offset = flashOffsetUs();
    scheduleCentre(nextOnsetUs + offset);
    nextOnsetUs += gridPeriod;
    // PER-BEAT INSTRUMENT: the grid onset, the flash offset, the last detection's
    // phase error and whether it was accepted or rejected.  This is what makes a
    // 1-2 beat flash drift visible (the 1 Hz summary cannot see it).
    if (Serial) Serial.printf("PERF c=%u t=%lu off=%lu pe=%ld st=%u md=%u tol=%ld\n",
                              (unsigned)toneIndex, (unsigned long)millis(),
                              (unsigned long)offset, (long)lastPhaseErrUs,
                              (unsigned)lastPhaseState, (unsigned)gridCapturing,
                              (long)(gridPeriod / PHASE_TOL_DIV));
  }

  if (centrePending && timeReached(now, centreFireUs)) {
    centrePending = false;

    // IO48: one 100 ms flash per beat
    digitalWrite(PIN_IO48, HIGH);
    io48Active = true;
    io48OffUs  = centreFireUs + (uint32_t)IO48_MS * 1000UL;
    io48Flashes++;

    // Channel 10: 100 ms, on counts 1 and 3 ONLY
    if (beatStepIsThis) {
      digitalWrite(PIN_CH10, HIGH);
      ch10Active = true;
      ch10OffUs  = centreFireUs + (uint32_t)CH10_MS * 1000UL;
      ch10Flashes++;
    }
  }

  if (io48Active && timeReached(now, io48OffUs)) { digitalWrite(PIN_IO48, LOW); io48Active = false; }
  if (ch10Active && timeReached(now, ch10OffUs)) { digitalWrite(PIN_CH10, LOW); ch10Active = false; }

  // BEAT / BEAT_STEP: the 25 ms window about the beat's middle
  const bool inWin = timeReached(now, beatWinStartUs) && !timeReached(now, beatWinEndUs);
  BEAT      = inWin;
  BEAT_STEP = inWin && beatStepIsThis;
}

// ---------------------------------------------------------------------------
//  MISS / SILENCE SERVICE
//  A beat was predicted from the measured period.  If it does not arrive, that
//  is a missed beat.  TWO consecutive missed beats mean silence: IO48 off and
//  the counter reset.
// ---------------------------------------------------------------------------
static void serviceDetector(uint32_t now) {
  if (lastDetUs == 0) return;
  const uint32_t ref = periodUs ? periodUs : (gridPeriod ? gridPeriod : 500000u);
  const uint32_t sinceDet = (uint32_t)(now - lastDetUs);

  // SILENCE - the music has stopped: IO48 off and the counter reset.
  if (sinceDet > (uint32_t)((float)ref * SILENCE_FACTOR)) { enterSilence(); return; }

  // DROPOUT - a glitch, NOT silence.  Treat it as a new track: follow the beat as
  // it comes back, while the show keeps running on the established beat.  This is
  // the fault where a Wi-Fi glitch paused the audio for a split second and the
  // grid never re-locked.
  if (gridLocked && sinceDet > (uint32_t)((float)ref * GLITCH_MISS_FACTOR)) {
    gridCapturing   = true;
    captureFirstDet = true;
    captureStartMs  = millis();
    intervalCount   = 0;
    intervalHead    = 0;
    rejRun          = 0;
    rejSign         = 0;
    return;
  }

  if (!gridLocked && awaitingBeat && periodUs != 0) {
    const uint32_t tol = periodUs / 3;
    if (timeReached(now, expectedLeadUs + tol)) {
      awaitingBeat = false;
      missCount++;
      missEvents++;
      if (missCount >= MISS_COUNT_FOR_SILENCE) {
        enterSilence();
      } else {
        expectedLeadUs += periodUs;   // wait for the one after
        awaitingBeat = true;
      }
    }
  }
}

// ---------------------------------------------------------------------------
//  PUBLISH THE COUNTERS  (USB serial only - no Wi-Fi)
//  fs   measured sample rate            bpm  from the median period
//  cnt  current 1..4 count              det  beats detected
//  io48 flashes issued                  ch10 flashes issued
//  ref  beats refused (refractory / out of range interval)
//  miss missed beats                    sil  silence events
//  per  measured period (us)            len  average beat length (us)
// ---------------------------------------------------------------------------
static void publishIfDue() {
  // Nothing is printed unless a host is actually connected.  Without this the
  // CDC write can block when the cable is attached but no terminal is open,
  // which stalls the loop and stops the LEDs.
  if (!Serial) return;
  const uint32_t ms = millis();
  if (ms - lastPublishMs < 1000) return;
  lastPublishMs = ms;
  Serial.printf(
      "fs=%.0f bpm=%.1f cnt=%u det=%lu dtn=%lu io48=%lu ch10=%lu ref=%lu miss=%lu sil=%lu per=%lu len=%lu rp=%.1f pk=%.1f tr=%.1f nv=%.1f na=%.1f BEAT=%d STEP=%d\n",
      fsMeasured,
      (double)bpmX10 / 10.0,
      (unsigned)beatCount,
      (unsigned long)beatsDetected,
      (unsigned long)detections,
      (unsigned long)io48Flashes,
      (unsigned long)ch10Flashes,
      (unsigned long)beatsRefused,
      (unsigned long)missEvents,
      (unsigned long)silenceEvents,
      (unsigned long)periodUs,
      (unsigned long)avgLenUs,
      (double)rawPeak,
      (double)peakEnv,
      (double)troughEnv,
      (double)novelty,
      (double)novAvg,
      BEAT ? 1 : 0,
      BEAT_STEP ? 1 : 0);
}

// ---------------------------------------------------------------------------
//  SETUP
// ---------------------------------------------------------------------------
void setup() {
  pinMode(PIN_IO48, OUTPUT); digitalWrite(PIN_IO48, LOW);
  pinMode(PIN_CH10, OUTPUT); digitalWrite(PIN_CH10, LOW);

  Serial.begin(115200);
  const uint32_t t0 = millis();
  while (!Serial && (millis() - t0) < 2000) { delay(10); }  // USB CDC enumerate
  delay(100);

  // CRITICAL: never block on the USB serial.  With no host connected the CDC
  // write can block, which stalls the main loop and stops ALL flashing - the
  // board looks dead while the code is fine.  MEASURED this session: the board
  // worked while a serial capture held the port open and went dark the moment
  // the port was closed.  A zero TX timeout makes writes non-blocking.
  Serial.setTxTimeoutMs(0);

  analogReadResolution(12);

  Serial.println();
  Serial.println("Fresh_Beat - standalone beat follower");
  Serial.println("Audio.docx: IO48 50ms per beat, Channel 10 on counts 1 and 3.");

  // the sample rate is MEASURED, then the filters and time constants follow it
  fsMeasured  = measureSampleRate();
  usPerSample = 1000000.0f / fsMeasured;

  hpf.designHighPass(BP_HP_HZ, BP_Q, fsMeasured);
  lpf.designLowPass(BP_LP_HZ, BP_Q, fsMeasured);

  dcCoef     = 1.0f - expf(-1.0f / (DC_TAU_S * fsMeasured));
  envRelease = expf(-1.0f / (ENV_RELEASE_TAU_S * fsMeasured));
  peakCoef   = 1.0f - expf(-1.0f / (PEAK_TAU_S * fsMeasured));
  troughCoef = 1.0f - expf(-1.0f / (TROUGH_TAU_S * fsMeasured));
  rawDecay   = expf(-1.0f / (RAW_TAU_S * fsMeasured));
  slowCoef   = 1.0f - expf(-1.0f / (SLOW_TAU_S * fsMeasured));
  novAvgCoef = 1.0f - expf(-1.0f / (NOV_AVG_TAU_S * fsMeasured));

  Serial.printf("measured fs=%.0f Hz  usPerSample=%.3f  band=%s %.0f-%.0f Hz\n",
                fsMeasured, usPerSample, USE_LOW_BAND ? "low" : "broadband",
                BP_HP_HZ, BP_LP_HZ);
}

// ---------------------------------------------------------------------------
//  LOOP
//  A short batch of samples is taken, each timestamped so an edge is timed
//  accurately; then the outputs and the miss/silence rule are serviced.  There
//  is no blocking anywhere, and no Wi-Fi.
// ---------------------------------------------------------------------------
void loop() {
  const uint32_t t0 = micros();

  for (uint32_t i = 0; i < SAMPLE_BATCH; i++) {
    const int raw = analogRead(PIN_AUDIO);
    processSample(raw, t0 + (uint32_t)((float)i * usPerSample));
  }

  const uint32_t now = micros();
  serviceOutputs(now);
  serviceDetector(now);
  publishIfDue();
}

