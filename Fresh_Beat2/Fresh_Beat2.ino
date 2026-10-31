// ============================================================================
//  Fresh_Beat2.ino  -  a NEW approach: MEASURE the beat, do not track it
// ----------------------------------------------------------------------------
//  Written from scratch.  Nothing is reused from Fresh_Beat.
//
//  WHY THIS IS DIFFERENT.  Everything tried before was a SEQUENTIAL TRACKER: a
//  threshold decided "that was an onset", and a phase-locked loop then CARRIED the
//  tempo and phase forward and corrected them with gains.  A tracker can only
//  follow what its detector hands it, so every detector error became a tempo error
//  (MEASURED: ~1.3 onsets per beat pulled a 134 BPM track to 178 BPM), and once the
//  phase was wrong the corrections were made blind.  The tempo and phase were never
//  MEASURED - they were inferred step by step.
//
//  THIS SKETCH MEASURES THEM DIRECTLY:
//    1. one clean onset curve, 200 frames per second, 6 seconds deep;
//    2. a COMB SEARCH over the whole 80-180 BPM range and every phase, scoring how
//       much onset energy lands on each candidate grid.  The best grid IS the beat.
//       It is re-run every 100 ms, so the beat is re-measured ten times a second.
//  Nothing is carried forward, so nothing can drift: the phase is SEEN in the
//  newest window every time.  There is no PLL, no capture state, no metrical test,
//  no interval histogram, no gain to tune.
//
//  REQUIREMENT (Audio.docx) - all of it:
//    * IO48 flashes 100 ms (50 ms here, as agreed) on the beat, 80-180 BPM
//    * silence -> IO48 off
//    * flashes numbered 1..4 internally, repeating
//    * counts 1 and 3 also flash Channel 10 for 50 ms
//    * BEAT / BEAT_STEP true at the MIDDLE of the beat, 25 ms window about it
//    * no internal timer drives the LEDs - the grid is re-measured from the music
//    * predictive timing one beat ahead, from the beat-to-beat period
//    * a track change is followed as quickly as possible (one search window)
//    Unacceptable: missed beats, drift, incorrect timing.
//
//  HARDWARE (HARDWARE_CONNECTION.md):
//    AUDIO = GPIO 2   MAX9814, ANALOGUE, ADC1 channel 1 (idles ~2048)
//    IO48  = GPIO 48  onboard LED
//    CH10  = GPIO 42  240V relay channel 10, HIGH = relay on
//
//  BUILD: esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=huge_app,
//         FlashMode=qio,USBMode=hwcdc,CDCOnBoot=cdc
//  PORT : COM5
// ============================================================================

#include <Arduino.h>
#include <math.h>
#include <string.h>

// ---------------------------------------------------------------------------
//  PINS
// ---------------------------------------------------------------------------
static const uint8_t PIN_AUDIO = 2;
static const uint8_t PIN_IO48  = 48;
static const uint8_t PIN_CH10  = 42;

// ---------------------------------------------------------------------------
//  OUTPUT TIMING
// ---------------------------------------------------------------------------
static const uint16_t IO48_MS     = 50;
static const uint16_t CH10_MS     = 50;
static const uint16_t BEAT_WIN_MS = 25;
static const int32_t  BEAT_WIN_HALF_US = (BEAT_WIN_MS * 1000L) / 2;
// The flash is timed for the MIDDLE of the beat, not the leading edge (Audio.docx).
// MEASURED: 70 ms read as "slightly behind" on EVERY track, on top of the detector's
// own lag (~15 ms of envelope smoothing + 5 ms phase quantisation).  30 ms is the
// correction; it is the one number to tune by ear.
static const uint32_t FLASH_OFFSET_US = 30000;

// ---------------------------------------------------------------------------
//  BAND  (kick / bass fundamentals)
// ---------------------------------------------------------------------------
static const float BP_HP_HZ = 40.0f;
static const float BP_LP_HZ = 150.0f;
static const float BP_Q     = 0.7071f;     // Butterworth

// ---------------------------------------------------------------------------
//  FRONT END
// ---------------------------------------------------------------------------
static const uint32_t SAMPLE_BATCH = 16;      // ADC reads per loop pass
static const uint32_t FRAME_US     = 5000;    // 5 ms -> 200 onset frames per second
static const uint32_t RING_N       = 1200;    // 6 seconds of history
static const float    ENV_TAU_S    = 0.015f;  // envelope smoothing ~15 ms
static const float    DC_TAU_S     = 0.30f;   // DC tracker
static const float    RAW_TAU_S    = 1.0f;    // raw peak hold, for the silence gate
static const uint32_t SEARCH_EVERY_FRAMES = 20;   // 100 ms

// ---------------------------------------------------------------------------
//  THE COMB SEARCH
//  Periods are counted in FRAMES: 80 BPM = 150 frames, 180 BPM = 67 frames.
// ---------------------------------------------------------------------------
static const uint8_t P_MIN_FR = 67;    // 180 BPM
static const uint8_t P_MAX_FR = 150;   // 80 BPM
// A fit is ACCEPTED only if it stands clear of the best DIFFERENT tempo, and the
// window carries real energy.  The margin is published - it is the honest measure
// of whether the fit is a real beat or a coin toss.
static const uint32_t ONSET_ENERGY_MIN = 200;
// TEMPO CONSTANCY - the user's rule: a record's BPM NEVER jumps, it holds to about
// +/-1 BPM.  So a tempo is LOCKED once it has been seen a few times in a row, and a
// different answer is only accepted if it PERSISTS.  One flickering search can no
// longer move the lights - MEASURED: the fit flipped 116.5 -> 173.9 -> 116.5 BPM in
// two seconds, which is impossible for real music and produced double flashing.
static const uint8_t  LOCK_NEEDED_SEARCHES   = 3;    // 300 ms to lock a tempo
static const uint8_t  CHANGE_NEEDED_SEARCHES = 15;   // 1.5 s to accept a new tempo
// A DECISIVE challenger is a new track, not a guess, and the user's rule is to lock onto
// it as soon as possible: 8 searches (0.8 s) instead of 15.
// MEASURED (Take on Me): `mar` runs 15-46 % on a real track and 0-7 % when the comb is
// guessing, so 20 % separates "clearly reliable" from "coin toss" with room to spare.
static const uint16_t DECISIVE_MAR_PCT  = 20;
static const uint8_t  DECISIVE_SEARCHES = 8;
// THE CONFIDENCE GATE AND THE HOLD (user's rule, 31 Oct 2026).
// The grid is only MOVED when the measurement is trustworthy.  While it is not, the grid
// free-runs on the last good tempo and keeps firing; after 2 bars of that the lights go
// dark but the TIMING KEEPS RUNNING, because that is what an instrumental or vocal passage
// looks like and the beat does come back.
// MEASURED on track 3: with the music LOUD (we 5300-6500, rp 375-653, i.e. 200-400x the
// silence gate) the comb's margin collapsed to 0-7 per cent for 22 of 207 seconds - a
// dead tie, fit=333 sec=332 - while everything else ran at 15-46 per cent.  The lights
// were being dragged off the beat by a measurement that had no idea where the beat was.
// Hysteresis: 10 % to hold, 15 % to resume following, so it cannot chatter on the edge.
static const uint16_t TRUST_PCT_HOLD       = 10;
static const uint16_t TRUST_PCT_FOLLOW     = 15;
static const uint8_t  HOLD_DARK_BEATS      = 8;      // 2 bars of 4/4, in beats
// A HOLD MUST BE A DAMPER, NOT A FREEZE.  MEASURED on Take on Me (fb2i.log): freezing the
// grid during a hold produced 181 dark beats out of 347 and a flash that sat at whatever
// offset the last untrustworthy measurement had produced until the margin recovered -
// heard as "drifting out of time again".  A hold therefore applies ONE THIRD of the
// correction: a noisy measurement can no longer drag the grid, and the grid is never left
// where it was either, so a stale offset cannot persist.
static const uint8_t  HOLD_SNAP_DIV        = 3;
// A BIG CORRECTION IS NOT A CORRECTION, IT IS A DIFFERENT ANSWER.  MEASURED on Take on Me
// (fb2j.log, the first build with `dmax`): 47 of 241 locked seconds had the grid sitting
// 150-176 ms away from the measured beat - HALF a period (177.5 ms at 355 ms) - i.e. ON
// THE OFF-BEAT, and that is exactly what the user hears as "the lights drift into the
// off-beat and then lock on again", both ahead and behind.  In 43 of those 47 seconds the
// comb's own PHASE margin was below 20 %: it was telling us it was unsure, and the full
// snap overruled it.  The cause is the music: Take on Me's bass runs in eighths, so the
// onset train has a strong peak every HALF beat and the comb cannot always tell the beat
// from the off-beat.  A move larger than a quarter period is therefore only made when the
// phase is clear AND the same answer persists - a real phase jump is sure and steady, a
// mis-identified off-beat is neither.
static const uint16_t BIG_MOVE_PM_PCT   = 25;   // the phase must be this clear ...
static const uint8_t  BIG_MOVE_SEARCHES = 8;    // ... and persist this long (0.8 s)
// The silence gate: measured on this board, silence is rp 27-42 and music 375-631.
static const float SILENCE_RAW_MIN = 120.0f;
// If there is NO SIGNAL AT ALL for this long, the lights go dark.  This is a SIGNAL
// timer, not a fit timer: an ambiguous fit must HOLD the lights, never darken them.
static const uint32_t SIGNAL_LOST_MS = 1500;
// Beat instants are only used once at least this much history exists.
static const uint16_t MIN_FRAMES_FOR_SEARCH = 400;   // 2 s

// ---------------------------------------------------------------------------
//  BIQUAD  (RBJ cookbook), floating point - the S3 has an FPU
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
    const float w0 = 2.0f * PI * f0 / fs, cw = cosf(w0), sw = sinf(w0);
    const float alpha = sw / (2.0f * Q), a0 = 1.0f + alpha;
    b0 = ((1.0f - cw) * 0.5f) / a0; b1 = (1.0f - cw) / a0; b2 = b0;
    a1 = (-2.0f * cw) / a0;         a2 = (1.0f - alpha) / a0;
    z1 = z2 = 0.0f;
  }

  void designHighPass(float f0, float Q, float fs) {
    const float w0 = 2.0f * PI * f0 / fs, cw = cosf(w0), sw = sinf(w0);
    const float alpha = sw / (2.0f * Q), a0 = 1.0f + alpha;
    b0 = ((1.0f + cw) * 0.5f) / a0; b1 = (-(1.0f + cw)) / a0; b2 = b0;
    a1 = (-2.0f * cw) / a0;         a2 = (1.0f - alpha) / a0;
    z1 = z2 = 0.0f;
  }
};

// ---------------------------------------------------------------------------
//  FRONT END STATE
// ---------------------------------------------------------------------------
static Biquad hpf, lpf;
static float  fsMeasured  = 24000.0f;
static float  usPerSample = 41.67f;

static float  dcMean = 2048.0f, dcCoef = 0.0f;
static float  rawPeak = 0.0f,  rawDecay = 0.0f;
static float  envSm  = 0.0f,   envCoef = 0.0f;
// The envelope is sampled once per frame into a short history, and the ONSET is the
// RISE OVER THE LAST 20 ms - not the sum of per-sample rises.  MEASURED: summing
// per-sample spreads a gradual attack over 3-4 frames at a quarter each, leaving no
// sharp peak - track 3 gave 1.8 onset frames per second against a 2.8/s beat rate,
// and its tempo margin collapsed to 21 against 46 on track 2.  One lagged difference
// puts a gradual attack's whole rise into ONE frame.
static float  envHist[8];               // newest at [0]; [4] is 20 ms ago
static uint32_t frameStartUs = 0;
static uint16_t framesMade = 0;

// ---------------------------------------------------------------------------
//  THE ONSET CURVE: ring[0] is the NEWEST frame, ring[n] is n frames ago.
//  One unsigned value per 5 ms frame: the total RISE of the envelope in that
//  frame.  A kick's attack is a large rise; a decay or steady bass is zero.
// ---------------------------------------------------------------------------
static uint16_t ring[RING_N];
static uint32_t windowEnergy = 0;
static const uint16_t ONSET_FRAME_MIN = 25;   // a frame this strong carries an onset
static volatile uint32_t onsetFrames = 0;     // onsets seen since the last publish
static int32_t pScore[P_MAX_FR + 1];          // best-phase score per candidate period

// ---------------------------------------------------------------------------
//  THE FIT (what the comb search found)
// ---------------------------------------------------------------------------
static int32_t  fitScore = 0, fitSecond = 0;
static uint16_t fitMarginPct = 0;

// ---------------------------------------------------------------------------
//  TEMPO LOCK - the period actually being followed (never a single search's answer)
// ---------------------------------------------------------------------------
static uint8_t  lockedP = 0;                 // 0 = no tempo locked yet
static uint8_t  candP = 0, candCount = 0;    // the candidate and its run length
static uint8_t  phaseUsed = 0;               // beat phase AT THE LOCKED PERIOD
static int32_t  scoreAtLocked = 0;           // excess energy at the locked period
static volatile uint32_t periodChanges = 0;  // tempo changes accepted (should be ~0)
static volatile uint8_t  pchCount = 0;       // changes since the last publish

// ---------------------------------------------------------------------------
//  OUTPUT STATE
// ---------------------------------------------------------------------------
// TWO SEPARATE THINGS, and this split is the whole point of the 31 Oct 2026 changes:
//   gridLocked - the TIMING exists and keeps running (beats, the bar count, the phase)
//   lightsOn   - whether those beats actually reach IO48 and CH10
// With one flag, going dark threw the tempo away and the board had to find the beat again
// from scratch - measured at the end of Take on Me, where it came back locked on 455
// (131.9 BPM) instead of the 355 (169.0 BPM) it had been holding seconds earlier.
static bool     gridLocked  = false;         // the timing exists and is running
static bool     lightsOn    = false;         // the beats reach the outputs
static uint32_t nextBeatUs  = 0;             // the next beat instant
static uint32_t periodUs    = 0;             // the fitted period
static uint32_t lastSignalMs = 0;            // last search that saw real signal
static uint8_t  toneIndex   = 0;             // 0 = not started, else 1..4

// THE HOLD.  The grid is moved only when the measurement is TRUSTWORTHY (see TRUST_PCT_*).
// While it is not, the grid free-runs on the last good tempo, keeps firing, and keeps
// listening for a new tempo; after 2 bars (HOLD_DARK_BEATS) the lights go dark while the
// timing runs on.
static bool     phaseHold   = false;         // true = holding: do NOT move the grid
static uint32_t holdStartMs = 0;             // when the current hold began
static uint8_t  darkReason  = 0;             // 0 = lit, 1 = no signal, 2 = held 2 bars
static uint16_t phaseMarginPct = 0;          // confidence of the PHASE at the locked period
static volatile uint32_t holdCount = 0;      // hold searches since the last publish
static volatile uint32_t dmaxUs    = 0;      // largest grid correction this second, BEFORE
                                             // damping = the true error of the held grid
static uint8_t  bigCount = 0;                // consecutive searches reporting a BIG move
static volatile uint8_t bigSeen = 0;         // highest bigCount seen this second
// FRESH EARS.  The ratio guard and the incumbent preference both rest on one physical
// fact - a record's tempo never jumps to a ratio of itself WHILE IT IS PLAYING.  That
// fact does not survive an interruption.  Retaining the tempo through a silence (above)
// means a NEW track at half or double the old tempo would otherwise be refused for ever,
// so while the music has just been interrupted the guards are lifted and the new music
// is allowed to pick its own tempo.  They re-arm as soon as the locked tempo is confirmed.
static bool     freshEars   = false;

static uint32_t flashCentreUs = 0;
static bool     flashPending  = false;
static bool     stepThisBeat  = false;
static uint32_t winStartUs = 0, winEndUs = 0;
static bool     io48Active = false, ch10Active = false;
static uint32_t io48OffUs = 0, ch10OffUs = 0;

static volatile bool BEAT = false, BEAT_STEP = false;
static volatile uint8_t beatCount = 0;

// ---------------------------------------------------------------------------
//  COUNTERS / TELEMETRY
// ---------------------------------------------------------------------------
static volatile uint32_t searches = 0;
static volatile uint32_t beatsFired = 0;
static volatile uint32_t io48Flashes = 0;
static volatile uint32_t ch10Flashes = 0;
static volatile uint32_t silences = 0;
static volatile uint32_t fitRejects = 0;
static uint32_t lastPublishMs = 0;

// ---------------------------------------------------------------------------
//  PROTOTYPES
// ---------------------------------------------------------------------------
static void     processSample(int raw, uint32_t t);
static void     closeFrame(uint32_t now);
static void     runSearch(uint32_t now);
static void     serviceOutputs(uint32_t now);
static void     publishIfDue();
static float    measureSampleRate();
static inline bool timeReached(uint32_t now, uint32_t t) {
  return (int32_t)(now - t) >= 0;
}

// ---------------------------------------------------------------------------
//  SAMPLE RATE - measured, never assumed
// ---------------------------------------------------------------------------
static float measureSampleRate() {
  const uint32_t windowUs = 200000;
  const uint32_t t0 = micros();
  uint32_t n = 0;
  while ((uint32_t)(micros() - t0) < windowUs) { analogRead(PIN_AUDIO); n++; }
  return (float)n * 1000000.0f / (float)windowUs;
}

// ---------------------------------------------------------------------------
//  ONE SAMPLE: DC out, band in, envelope, and the RISE goes into the frame
// ---------------------------------------------------------------------------
static void processSample(int raw, uint32_t t) {
  (void)t;
  // 1. DC removal - the mic idles near mid-rail
  dcMean += ((float)raw - dcMean) * dcCoef;
  float x = (float)raw - dcMean;

  // silence gate: hold the raw AC peak for ~1 s
  const float ar = fabsf(x);
  rawPeak *= rawDecay;
  if (ar > rawPeak) rawPeak = ar;

  // 2. the band the beat lives in
  x = hpf.process(x);
  x = lpf.process(x);
  const float rect = fabsf(x);

  // 3. envelope ~15 ms
  envSm += (rect - envSm) * envCoef;
  // 4. the ONSET is taken per FRAME (see closeFrame) as the rise over the last 20 ms.
}

// ---------------------------------------------------------------------------
//  CLOSE A FRAME: push it at index 0 (newest) and shift the history
// ---------------------------------------------------------------------------
static void closeFrame(uint32_t now) {
  (void)now;
  // sample the envelope into the history, then take the RISE over the last 20 ms
  memmove(&envHist[1], &envHist[0], 7 * sizeof(envHist[0]));
  envHist[0] = envSm;
  float d = envHist[0] - envHist[4];        // 4 frames = 20 ms
  if (d < 0.0f) d = 0.0f;                   // a decay is not a beat
  memmove(&ring[1], &ring[0], (RING_N - 1) * sizeof(ring[0]));
  if (d > 65535.0f) d = 65535.0f;
  ring[0] = (uint16_t)(d + 0.5f);
  if (ring[0] >= ONSET_FRAME_MIN) onsetFrames++;
  if (framesMade < 0xFFFF) framesMade++;
}

// ---------------------------------------------------------------------------
//  THE COMB SEARCH
//  For every period in 80-180 BPM and every phase, score how much onset energy
//  lands on that candidate grid.  The best score IS the beat.  Run every 100 ms.
// ---------------------------------------------------------------------------
// Best phase for ONE candidate period, scored as EXCESS OVER CHANCE:
//   score = (sum of the curve at the grid points) - (how much a random phase would
//           have collected from those same points)
// That is what makes the score fair between periods of different length.  It also
// encodes the user's 80-vs-160 rule by itself:
//   * 80 BPM track - the 150-frame grid lands on every beat (a big excess), while the
//     75-frame grid lands half on beats and half on the EMPTY midpoints (no excess),
//     so the LONGER period wins.
//   * 160 BPM track - every point of BOTH grids carries a beat, so the shorter grid
//     collects twice the excess and wins.
// The true tempo level therefore falls out of the arithmetic.
// The best phase for ONE period, AND how far clear it stands of the best DIFFERENT phase
// (at least 3 frames away, so the neighbouring frames of the same peak do not count).
// That second number is the confidence of the PHASE, and it is what decides whether the
// grid may be MOVED at all.  The tempo margin answers "which tempo"; this answers "do we
// actually know where the beat is".  MEASURED: when the tempo margin is 0 the phase
// measured from the same landscape is a coin toss, and the flash was being dragged with it.
static int32_t bestPhaseAt(int32_t mean, uint8_t P, uint8_t *phOut, int32_t *secondOut) {
  int32_t bestS = -2000000000; uint8_t bestPh = 0;
  for (uint8_t ph = 0; ph < P; ph++) {
    int32_t s = 0; uint16_t n = 0;
    for (uint16_t idx = ph; idx < RING_N; idx += P) { s += (int32_t)ring[idx]; n++; }
    const int32_t sc = s - (int32_t)((uint32_t)mean * (uint32_t)n);
    if (sc > bestS) { bestS = sc; bestPh = ph; }
  }
  *phOut = bestPh;
  if (secondOut) {
    int32_t secondS = -2000000000;
    for (uint8_t ph = 0; ph < P; ph++) {
      int16_t d = (int16_t)ph - (int16_t)bestPh;      // circular distance from the peak
      if (d < 0) d = (int16_t)(-d);
      if (d > (int16_t)(P / 2)) d = (int16_t)((int16_t)P - d);
      if (d < 3) continue;                            // the same peak, not a rival
      int32_t s = 0; uint16_t n = 0;
      for (uint16_t idx = ph; idx < RING_N; idx += P) { s += (int32_t)ring[idx]; n++; }
      const int32_t sc = s - (int32_t)((uint32_t)mean * (uint32_t)n);
      if (sc > secondS) secondS = sc;
    }
    *secondOut = secondS;
  }
  return bestS;
}

// Are two periods a simple musical RATIO of each other (1.5x, 2x, 3x, or their
// inverses 2/3, 1/2, 1/3)?  A record's tempo never jumps to a ratio of itself, so a
// candidate like that is the DETECTOR's ratio error, not a tempo change.
// Tolerance 3 frames: the period is quantised to 5 ms, so 1.5 x 355 = 532.5 lands on
// 535 - which is exactly the value the lock walked to on track 3.
static bool nearRatio(uint8_t a, uint8_t b) {
  const uint8_t big   = (a > b) ? a : b;
  const uint8_t small = (a > b) ? b : a;
  const int16_t want[3] = { (int16_t)((small * 3) / 2),
                            (int16_t)(small * 2),
                            (int16_t)(small * 3) };
  for (uint8_t i = 0; i < 3; i++) {
    int16_t d = (int16_t)big - want[i];
    if (d < 0) d = -d;
    if (d <= 3) return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
//  THE COMB SEARCH  (runs every 100 ms)
// ---------------------------------------------------------------------------
static void runSearch(uint32_t now) {
  if (framesMade < MIN_FRAMES_FOR_SEARCH) return;

  // THE SIGNAL TIMER, driven by the LEVEL alone (not by the fit).  "Literally no noise"
  // is the only thing that darkens the lights immediately; an ambiguous fit must hold
  // them instead.  Measured on this board: silence rp 27-42, music 375-717.
  if (rawPeak > SILENCE_RAW_MIN) lastSignalMs = millis();

  uint32_t energy = 0;
  for (uint16_t i = 0; i < RING_N; i++) energy += ring[i];
  windowEnergy = energy;
  const int32_t mean = (int32_t)(energy / RING_N);

  // 1. score EVERY period's best phase, keeping the results so they can be compared
  int32_t best = -2000000000; uint8_t bp = 0;
  for (uint8_t P = P_MIN_FR; P <= P_MAX_FR; P++) {
    uint8_t ph;
    pScore[P] = bestPhaseAt(mean, P, &ph, NULL);
    if (pScore[P] > best) { best = pScore[P]; bp = P; }
  }
  // 2. the best from a genuinely DIFFERENT tempo (+/-3 frames = +/-15 ms of period)
  int32_t second = -2000000000;
  for (uint8_t P = P_MIN_FR; P <= P_MAX_FR; P++) {
    if (P + 3 >= bp && P <= bp + 3) continue;
    if (pScore[P] > second) second = pScore[P];
  }

  // 3. SHORTER PERIOD ON A TIE - but ONLY for a true 2x or 3x relationship.
  //    MEASURED on the fast track: the lock jumped between 169 BPM and its HALF and
  //    TWO THIRDS (84 and 112 BPM), because a detector that catches only some beats
  //    leaves the half-tempo grid scoring almost the same - the two are then
  //    indistinguishable by score alone, and the beat is the SHORTER one.
  //    MEASURED failure of the first version: allowing ANY shorter period within 15%
  //    made track 2 START at 179 BPM (the top of the range) whenever the evidence was
  //    weak.  Restricting it to best/2 and best/3 is the user's 80-vs-160 rule and
  //    cannot pick an arbitrary fast period.
  uint8_t chosen = bp;
  if (best > 0) {
    const int32_t thr = best - (best / 7);
    for (uint8_t k = 2; k <= 3; k++) {
      const uint8_t cand = (uint8_t)(bp / k);
      if (cand >= P_MIN_FR && cand < bp && pScore[cand] >= thr) { chosen = cand; break; }
    }
  }

  // 4. THE INCUMBENT GETS THE BENEFIT OF THE DOUBT, and a ratio of the locked period is
  //    refused outright.  MEASURED on track 3: the lock flip-flopped between 355 frames
  //    (169.0 BPM) and 535 frames (112.1 BPM = exactly TWO THIRDS of 169) 17 times in one
  //    track, spending seconds at a time at the wrong tempo - heard as "periods where it
  //    goes ontime and periods where it looses it".  At the moment of each switch `mar`
  //    was 1 or 3 per cent: the comb genuinely cannot separate the two tempos there, so
  //    score alone will never settle it.  A 1.5x grid lands alternately ON the beat and
  //    halfway between beats, and this track carries energy in both places.
  if (lockedP != 0 && chosen != lockedP && !freshEars) {
    const int32_t inc = pScore[lockedP];
    if ((inc > 0 && pScore[chosen] < inc + inc / 8) ||   // not decisively better: hold the lock
        nearRatio(chosen, lockedP)) {                    // 1.5x / 2x / 3x: a ratio error, not a tempo
      chosen = lockedP;
    }
  }

  fitScore = best; fitSecond = second;
  fitMarginPct = (best > 0) ? (uint16_t)(((best - second) * 100) / best) : 0;
  searches++;

  const bool fitReal = (best > 0) &&
                       (energy >= ONSET_ENERGY_MIN) &&
                       (rawPeak > SILENCE_RAW_MIN);

  // 4. TEMPO LOCK - a record's BPM never jumps (user's rule).  A candidate within +/-
  //    2 frames of the lock is the SAME tempo and is followed silently; anything
  //    further must PERSIST before it is believed, so one flickering search can no
  //    longer move the lights.
  if (fitReal) {
    if (candP == 0 || (chosen <= candP + 1 && chosen + 1 >= candP)) {
      if (candCount < 255) candCount++;
    } else {
      candCount = 1;
    }
    candP = chosen;
    if (lockedP == 0) {
      if (candCount >= LOCK_NEEDED_SEARCHES) { lockedP = candP; periodChanges++; pchCount++; }
    } else if (candP > lockedP + 2 || candP + 2 < lockedP) {
      // A DIFFERENT tempo.  A record never jumps, so it must normally persist for
      // CHANGE_NEEDED_SEARCHES.  But if the challenger is DECISIVE - clear of the
      // second-best tempo AND not a ratio of the lock - that is a NEW TRACK, and the
      // user's rule is to lock onto it as soon as possible.
      const uint8_t need = ((fitMarginPct >= DECISIVE_MAR_PCT) &&
                            (freshEars || !nearRatio(candP, lockedP)))
                             ? DECISIVE_SEARCHES : CHANGE_NEEDED_SEARCHES;
      if (candCount >= need) {
        lockedP = candP; periodChanges++; pchCount++;
        toneIndex   = 0;                 // a new track: the bar restarts at 1
        holdStartMs = 0;                 // and the 2-bar hold timer restarts with it
      }
    } else if (!phaseHold) {
      lockedP = candP;                 // small drift: follow it, do not count a change
    }
    // else: HOLDING - do not move the PERIOD either.  MEASURED (fb2k.log, t=362 s): while
    // the grid was holding with the fit at mar=0, this silent +/-2-frame follow still walked
    // the lock 355 -> 350 -> 345 ms (169.0 -> 173.9 BPM, 2.8 per cent FAST) - which the user
    // heard as "it was at a faster tempo than the beat" through the quiet section.  A hold
    // that keeps the timing must keep the tempo too.
  } else {
    candCount = 0;
  }

  if (lockedP == 0) { fitRejects++; return; }

  // 5. THE PHASE at the locked period, AND ITS OWN CONFIDENCE.  The phase answers "where
  //    is the beat", and phaseMarginPct says whether that answer is worth acting on.
  uint8_t ph;
  int32_t secondPhase = 0;
  scoreAtLocked = bestPhaseAt(mean, lockedP, &ph, &secondPhase);
  phaseUsed = ph;
  const int32_t psec = (secondPhase < 0) ? 0 : secondPhase;
  phaseMarginPct = (scoreAtLocked > 0)
                     ? (uint16_t)(((scoreAtLocked - psec) * 100) / scoreAtLocked) : 0;

  // 6. TRUST IT OR HOLD IT.  A fit is only followed when the tempo margin is CLEAR.
  //    Otherwise the grid HOLDS: it free-runs on the last good tempo, keeps firing, and
  //    keeps listening for a new one - the user's rule, "maintain the BPM and catch it
  //    when it returns".  After 2 bars of holding, the lights go dark while the timing
  //    runs on, because that is what an instrumental or vocal passage looks like.
  //    MEASURED on track 3: 22 of 207 loud seconds (we 5300-6500, rp 343-653) had the
  //    margin at 0-7 per cent, so the grid was being moved to a coin toss ten times a
  //    second.  Those are exactly the seconds the user hears as "it gets tripped up".
  const bool confident = fitReal &&
                         (fitMarginPct >= (phaseHold ? TRUST_PCT_FOLLOW : TRUST_PCT_HOLD));
  phaseHold = !confident;
  periodUs  = (uint32_t)lockedP * FRAME_US;         // the 2-bar timer needs it
  if (confident && chosen == lockedP) freshEars = false;   // confirmed: guards re-arm

  if (!phaseHold) {
    holdStartMs = 0;
    if (darkReason == 2) { darkReason = 0; lightsOn = true; }   // the beat is back
  } else {
    holdCount++;
    if (holdStartMs == 0) {
      holdStartMs = millis();
    } else if (darkReason != 1 &&
               (uint32_t)(millis() - holdStartMs) >=
                   (uint32_t)HOLD_DARK_BEATS * (periodUs / 1000UL)) {
      darkReason = 2;                              // 2 bars held: dark, but keep time
      lightsOn   = false;
      freshEars  = true;                           // the music is not continuous any more,
                                                   // so let it put its own tempo forward
    }
  }

  if (!fitReal || scoreAtLocked <= 0) { fitRejects++; return; }  // no grid move: free-run

  // 7. PLACE THE GRID - and only ever from a measurement we trust.
  const uint32_t beatRef = now - (uint32_t)phaseUsed * FRAME_US;
  uint32_t ideal = beatRef + periodUs;
  while (timeReached(now, ideal)) ideal += periodUs;      // first beat in the future

  if (!gridLocked) {
    gridLocked = true;
    lightsOn   = true;
    toneIndex  = 0;                                       // count restarts at 1
    nextBeatUs = ideal;
  } else {
    // Move the beat timeline onto the measured grid, by at most half a period.  The grid
    // is ABSOLUTE - re-measured ten times a second - so nothing accumulates.
    int32_t d = (int32_t)(ideal - nextBeatUs);
    const int32_t half = (int32_t)(periodUs / 2);
    if (d > half) d -= (int32_t)periodUs;
    else if (d < -half) d += (int32_t)periodUs;
    const int32_t ad = (d < 0) ? -d : d;
    if ((uint32_t)ad > dmaxUs) dmaxUs = (uint32_t)ad;     // the grid's true error, pre-damp

    bool apply = true;
    if (ad > (int32_t)(periodUs / 4)) {
      // A move bigger than a QUARTER PERIOD would put the beat somewhere quite different
      // - a half-period version of it is the OFF-BEAT.  Only do it when the phase is
      // clear and the same answer keeps coming back.
      if (phaseMarginPct >= BIG_MOVE_PM_PCT) {
        if (bigCount < 255) bigCount++;
        if (bigCount > bigSeen) bigSeen = bigCount;
      } else {
        bigCount = 0;
      }
      if (bigCount < BIG_MOVE_SEARCHES) apply = false;    // not corroborated: stay put
    } else {
      bigCount = 0;
      if (phaseHold) d /= HOLD_SNAP_DIV;                  // damped, NEVER frozen
    }
    if (apply) {
      bigCount = 0;
      nextBeatUs = (uint32_t)((int32_t)nextBeatUs + d);
    }
  }
}

// ---------------------------------------------------------------------------
//  OUTPUTS - fire on the grid: measured when it can be trusted, held when it cannot
// ---------------------------------------------------------------------------
static uint16_t frameTick = 0;

static void serviceOutputs(uint32_t now) {
  // DARK.  The LIGHTS go out, the TIMING DOES NOT: the tempo, the phase and the bar count
  // are all kept, so the beat's return needs no re-lock at all.
  //   reason 1 = no signal at all  (measured: silence rp 27-42, music 375-717)
  //   reason 2 = held for 2 bars without a trustworthy measurement (set in runSearch)
  if (gridLocked) {
    const bool noSignal = (uint32_t)(millis() - lastSignalMs) > SIGNAL_LOST_MS;
    if (noSignal) {
      if (lightsOn) { lightsOn = false; darkReason = 1; silences++; }
      flashPending = false;
      digitalWrite(PIN_IO48, LOW); io48Active = false;
      digitalWrite(PIN_CH10, LOW); ch10Active = false;
      BEAT = false; BEAT_STEP = false;
    } else if (darkReason == 1) {
      // The signal is back: light up again on the held tempo and give the fit a fresh
      // 2 bars to prove itself, whether it is the same track drifted or a new one.
      darkReason = 0; holdStartMs = 0; lightsOn = true;
      freshEars  = true;   // the music was interrupted: allow it to pick a new tempo
    }
  }

  // THE BEAT - the grid ALWAYS runs, whether the beats reach the lights is separate
  if (gridLocked && timeReached(now, nextBeatUs)) {
    toneIndex = (uint8_t)((toneIndex % 4) + 1);
    beatCount = toneIndex;
    stepThisBeat = (toneIndex == 1 || toneIndex == 3);
    beatsFired++;
    if (lightsOn) {
      flashCentreUs = nextBeatUs + FLASH_OFFSET_US;    // the MIDDLE of the beat
      flashPending  = true;
      winStartUs = flashCentreUs - BEAT_WIN_HALF_US;
      winEndUs   = flashCentreUs + BEAT_WIN_HALF_US;
    }
    nextBeatUs += periodUs;                            // one beat ahead only
    // PER-BEAT FIT: the tempo actually used for THIS flash, and whether the lights were
    // on for it.  `beat` minus `io48` is exactly the beats fired while dark.
    if (Serial) Serial.printf("BB c=%u per=%lu bpm=%.1f lock=%u ph=%u d=%u\n",
                              (unsigned)toneIndex, (unsigned long)(periodUs / 1000u),
                              periodUs ? (60000000.0 / (double)periodUs) : 0.0,
                              (unsigned)lockedP, (unsigned)phaseUsed, (unsigned)darkReason);
  }

  // THE FLASH, at the beat's middle
  if (flashPending && timeReached(now, flashCentreUs)) {
    flashPending = false;
    digitalWrite(PIN_IO48, HIGH); io48Active = true;
    io48OffUs = flashCentreUs + (uint32_t)IO48_MS * 1000UL;
    io48Flashes++;
    if (stepThisBeat) {
      // CHANNEL 10 IS NO LONGER DRIVEN (user, 31 Oct 2026): from now on only IO48 flashes.
      // The 1&3 marker is still counted - BEAT_STEP is what the lightshow reads, and the
      // `ch10` column in the log is how those marks are checked.  PIN_CH10 is left LOW.
      ch10Flashes++;
    }
  }
  if (io48Active && timeReached(now, io48OffUs)) { digitalWrite(PIN_IO48, LOW); io48Active = false; }
  if (ch10Active && timeReached(now, ch10OffUs)) { digitalWrite(PIN_CH10, LOW); ch10Active = false; }

  const bool inWin = timeReached(now, winStartUs) && !timeReached(now, winEndUs);
  BEAT      = inWin;
  BEAT_STEP = inWin && stepThisBeat;
}

// ---------------------------------------------------------------------------
//  TELEMETRY  (USB serial only; nothing is printed with no host connected)
//  bpm/per  the fitted tempo          ph   beat age in frames (phase)
//  fit/sec  best and second-best score       mar  margin % (fit confidence)
//  we       window onset energy              rp   raw peak (silence gate)
//  cnt/beat/io48/ch10  the outputs           sil  times the fit was lost
//  sch/rej  searches and rejected fits       lk   1 while following a fit
// ---------------------------------------------------------------------------
static void publishIfDue() {
  if (!Serial) return;
  const uint32_t ms = millis();
  if (ms - lastPublishMs < 1000) return;
  lastPublishMs = ms;
  Serial.printf(
      "FT bpm=%.1f per=%lu lock=%u ph=%u fit=%ld sec=%ld mar=%u we=%lu rp=%.0f ons=%lu cnt=%u beat=%lu io48=%lu ch10=%lu sil=%lu sch=%lu rej=%lu chg=%lu pch=%u lk=%u pm=%u hold=%lu dark=%u dmax=%lu big=%u\n",
      periodUs ? (60000000.0 / (double)periodUs) : 0.0,
      (unsigned long)(periodUs / 1000u),
      (unsigned)lockedP, (unsigned)phaseUsed,
      (long)fitScore, (long)fitSecond, (unsigned)fitMarginPct,
      (unsigned long)windowEnergy, (double)rawPeak, (unsigned long)onsetFrames,
      (unsigned)beatCount, (unsigned long)beatsFired,
      (unsigned long)io48Flashes, (unsigned long)ch10Flashes,
      (unsigned long)silences, (unsigned long)searches, (unsigned long)fitRejects,
      (unsigned long)periodChanges, (unsigned)pchCount,
      gridLocked ? 1u : 0u,
      (unsigned)phaseMarginPct, (unsigned long)holdCount, (unsigned)darkReason,
      (unsigned long)dmaxUs, (unsigned)bigSeen);
  pchCount = 0;
  holdCount = 0;
  dmaxUs = 0;
  bigSeen = 0;
  onsetFrames = 0;
}

// ---------------------------------------------------------------------------
//  SETUP
// ---------------------------------------------------------------------------
void setup() {
  pinMode(PIN_IO48, OUTPUT); digitalWrite(PIN_IO48, LOW);
  pinMode(PIN_CH10, OUTPUT); digitalWrite(PIN_CH10, LOW);

  Serial.begin(115200);
  const uint32_t t0 = millis();
  while (!Serial && (millis() - t0) < 2000) { delay(10); }
  delay(100);
  // never block on USB serial: with no host connected a blocking CDC write stalls
  // the loop and stops everything (measured earlier on this board)
  Serial.setTxTimeoutMs(0);

  analogReadResolution(12);

  Serial.println();
  Serial.println("Fresh_Beat2 - the beat is MEASURED by comb search, not tracked");

  fsMeasured  = measureSampleRate();
  usPerSample = 1000000.0f / fsMeasured;

  hpf.designHighPass(BP_HP_HZ, BP_Q, fsMeasured);
  lpf.designLowPass(BP_LP_HZ, BP_Q, fsMeasured);

  dcCoef   = 1.0f - expf(-1.0f / (DC_TAU_S * fsMeasured));
  rawDecay = expf(-1.0f / (RAW_TAU_S * fsMeasured));
  envCoef  = 1.0f - expf(-1.0f / (ENV_TAU_S * fsMeasured));

  memset(ring, 0, sizeof(ring));
  frameStartUs = micros();

  Serial.printf("measured fs=%.0f Hz  band=%.0f-%.0f Hz  frames=%lu Hz  window=%lu ms\n",
                fsMeasured, BP_HP_HZ, BP_LP_HZ,
                (unsigned long)(1000000UL / FRAME_US),
                (unsigned long)(RING_N * FRAME_US / 1000UL));
}

// ---------------------------------------------------------------------------
//  LOOP
// ---------------------------------------------------------------------------
void loop() {
  const uint32_t t0 = micros();
  for (uint32_t i = 0; i < SAMPLE_BATCH; i++) {
    processSample(analogRead(PIN_AUDIO), t0 + (uint32_t)((float)i * usPerSample));
  }

  const uint32_t now = micros();

  uint8_t guard = 0;
  while ((uint32_t)(now - frameStartUs) >= FRAME_US && guard++ < 4) {
    closeFrame(now);
    frameStartUs += FRAME_US;
    frameTick++;
  }
  if (frameTick >= SEARCH_EVERY_FRAMES) {
    frameTick = 0;
    runSearch(now);
  }

  serviceOutputs(now);
  publishIfDue();
}

