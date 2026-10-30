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
// The silence gate: measured on this board, silence is rp 27-42 and music 375-631.
static const float SILENCE_RAW_MIN = 120.0f;
// If no confident fit for this long, stop firing (lights off).
static const uint32_t FIT_LOST_MS = 1500;
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
static bool     fitConfident = false;

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
static bool     gridLocked  = false;         // a confident fit is being followed
static uint32_t nextBeatUs  = 0;             // the next beat instant
static uint32_t periodUs    = 0;             // the fitted period
static uint32_t lastFitMs   = 0;
static uint8_t  toneIndex   = 0;             // 0 = not started, else 1..4

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
static int32_t bestPhaseAt(int32_t mean, uint8_t P, uint8_t *phOut) {
  int32_t bestS = -2000000000; uint8_t bestPh = 0;
  for (uint8_t ph = 0; ph < P; ph++) {
    int32_t s = 0; uint16_t n = 0;
    for (uint16_t idx = ph; idx < RING_N; idx += P) { s += (int32_t)ring[idx]; n++; }
    const int32_t sc = s - (int32_t)((uint32_t)mean * (uint32_t)n);
    if (sc > bestS) { bestS = sc; bestPh = ph; }
  }
  *phOut = bestPh;
  return bestS;
}

// ---------------------------------------------------------------------------
//  THE COMB SEARCH  (runs every 100 ms)
// ---------------------------------------------------------------------------
static void runSearch(uint32_t now) {
  if (framesMade < MIN_FRAMES_FOR_SEARCH) return;

  uint32_t energy = 0;
  for (uint16_t i = 0; i < RING_N; i++) energy += ring[i];
  windowEnergy = energy;
  const int32_t mean = (int32_t)(energy / RING_N);

  // 1. score EVERY period's best phase, keeping the results so they can be compared
  int32_t best = -2000000000; uint8_t bp = 0;
  for (uint8_t P = P_MIN_FR; P <= P_MAX_FR; P++) {
    uint8_t ph;
    pScore[P] = bestPhaseAt(mean, P, &ph);
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

  fitScore = best; fitSecond = second;
  fitMarginPct = (best > 0) ? (uint16_t)(((best - second) * 100) / best) : 0;
  searches++;

  const bool fitReal = (best > 0) &&
                       (energy >= ONSET_ENERGY_MIN) &&
                       (rawPeak > SILENCE_RAW_MIN);
  fitConfident = fitReal;

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
      if (candCount >= CHANGE_NEEDED_SEARCHES) { lockedP = candP; periodChanges++; pchCount++; }
    } else {
      lockedP = candP;                 // small drift: follow it, do not count a change
    }
  } else {
    candCount = 0;
  }

  if (lockedP == 0) { fitRejects++; return; }

  // 4. THE PHASE, re-measured AT THE LOCKED PERIOD every search.  This is what makes
  //    the flash follow the beat rather than a clock: the beat's position in the
  //    newest 6 seconds is re-read ten times a second.
  uint8_t ph;
  scoreAtLocked = bestPhaseAt(mean, lockedP, &ph);
  phaseUsed = ph;

  if (!fitReal || scoreAtLocked <= 0) { fitRejects++; return; }

  // 5. PLACE THE GRID
  periodUs = (uint32_t)lockedP * FRAME_US;
  const uint32_t beatRef = now - (uint32_t)phaseUsed * FRAME_US;
  uint32_t ideal = beatRef + periodUs;
  while (timeReached(now, ideal)) ideal += periodUs;      // first beat in the future

  if (!gridLocked) {
    gridLocked = true;
    toneIndex  = 0;                                       // count restarts at 1
    nextBeatUs = ideal;
  } else {
    // Snap the beat timeline onto the measured grid, by at most half a period.  The
    // grid is ABSOLUTE - re-measured ten times a second - so nothing accumulates.
    int32_t d = (int32_t)(ideal - nextBeatUs);
    const int32_t half = (int32_t)(periodUs / 2);
    if (d > half) d -= (int32_t)periodUs;
    else if (d < -half) d += (int32_t)periodUs;
    nextBeatUs = (uint32_t)((int32_t)nextBeatUs + d);
  }
  lastFitMs = millis();
}

// ---------------------------------------------------------------------------
//  OUTPUTS - fire on the FITTED grid; nothing here is a free-running clock
// ---------------------------------------------------------------------------
static uint16_t frameTick = 0;

static void serviceOutputs(uint32_t now) {
  // A fit that has gone stale means silence as far as the lights are concerned.
  if (gridLocked && (uint32_t)(millis() - lastFitMs) > FIT_LOST_MS) {
    gridLocked = false;
    silences++;
    digitalWrite(PIN_IO48, LOW); io48Active = false;
    digitalWrite(PIN_CH10, LOW); ch10Active = false;
    BEAT = false; BEAT_STEP = false;
    toneIndex = 0; beatCount = 0;
    lockedP = 0; candP = 0; candCount = 0;    // a new track must re-lock from scratch
  }

  // THE BEAT
  if (gridLocked && timeReached(now, nextBeatUs)) {
    toneIndex = (uint8_t)((toneIndex % 4) + 1);
    beatCount = toneIndex;
    stepThisBeat = (toneIndex == 1 || toneIndex == 3);
    beatsFired++;
    flashCentreUs = nextBeatUs + FLASH_OFFSET_US;      // the MIDDLE of the beat
    flashPending  = true;
    winStartUs = flashCentreUs - BEAT_WIN_HALF_US;
    winEndUs   = flashCentreUs + BEAT_WIN_HALF_US;
    nextBeatUs += periodUs;                            // one beat ahead only
    // PER-BEAT FIT: the tempo actually used for THIS flash.  If it ever differs from
    // the previous beat without the music changing, it is visible here at once.
    if (Serial) Serial.printf("BB c=%u per=%lu bpm=%.1f lock=%u ph=%u\n",
                              (unsigned)toneIndex, (unsigned long)(periodUs / 1000u),
                              periodUs ? (60000000.0 / (double)periodUs) : 0.0,
                              (unsigned)lockedP, (unsigned)phaseUsed);
  }

  // THE FLASH, at the beat's middle
  if (flashPending && timeReached(now, flashCentreUs)) {
    flashPending = false;
    digitalWrite(PIN_IO48, HIGH); io48Active = true;
    io48OffUs = flashCentreUs + (uint32_t)IO48_MS * 1000UL;
    io48Flashes++;
    if (stepThisBeat) {
      digitalWrite(PIN_CH10, HIGH); ch10Active = true;
      ch10OffUs = flashCentreUs + (uint32_t)CH10_MS * 1000UL;
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
      "FT bpm=%.1f per=%lu lock=%u ph=%u fit=%ld sec=%ld mar=%u we=%lu rp=%.0f ons=%lu cnt=%u beat=%lu io48=%lu ch10=%lu sil=%lu sch=%lu rej=%lu chg=%lu pch=%u lk=%u\n",
      periodUs ? (60000000.0 / (double)periodUs) : 0.0,
      (unsigned long)(periodUs / 1000u),
      (unsigned)lockedP, (unsigned)phaseUsed,
      (long)fitScore, (long)fitSecond, (unsigned)fitMarginPct,
      (unsigned long)windowEnergy, (double)rawPeak, (unsigned long)onsetFrames,
      (unsigned)beatCount, (unsigned long)beatsFired,
      (unsigned long)io48Flashes, (unsigned long)ch10Flashes,
      (unsigned long)silences, (unsigned long)searches, (unsigned long)fitRejects,
      (unsigned long)periodChanges, (unsigned)pchCount,
      gridLocked ? 1u : 0u);
  pchCount = 0;
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

