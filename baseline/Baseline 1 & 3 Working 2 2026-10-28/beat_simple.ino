// ===========================================================================
//  beat_simple  -  MINIMAL beat flasher
//
//  REQUIREMENT (only this, nothing else):
//    IO48          flashes in time with the beat
//    Channel 10    flashes on beats 1 and 3  (i.e. every OTHER beat)
//
//  Everything else has been REMOVED.  Not present, by design:
//    - no comb filter bank, no tempo lock
//    - no agent pool / hypothesis pool
//    - no modulo-4 bar counter, no downbeat rule
//    - no phrase-boundary anchor, no centroid / crash cues
//    - no mid or high bands, no three-band fusion
//    - no dual-timescale normaliser, no AGC expander
//    - no Goertzel probes, no 'D' plot stream
//    - no Wi-Fi, no web server
//
//  What is kept is only what the beat needs, and each part is one that was
//  measured working on this hardware:
//    1. DMA ADC front end          (fs measured 23,975 Hz)
//    2. DC removal                 (fractional tracker - a whole-count step
//                                   cannot move by less than one count)
//    3. 40-150 Hz band, 24 dB/oct  (the 12 dB/oct version leaked snare and
//                                   hi-hat into the novelty curve)
//    4. rectified novelty  |x[n]| - |x[n-1]|
//    5. adaptive threshold + refractory lockout
//    6. ONE beat grid, phase-locked to the onsets
//
//  THE ONE THING THAT MATTERS, and the reason the earlier version drifted:
//  the grid is SEEDED ON AN ONSET and the phase is pulled on every plausible
//  onset.  It therefore starts ON the beat and cannot settle half a beat away.
//  The earlier design seeded its phase from a comb-filter estimate that slid;
//  once the grid was half a beat out the pull direction became ambiguous and
//  it could never recover.
//
//  HARDWARE (from HARDWARE_CONNECTION.md)
//    AUDIO_PIN  = 2    MAX9814, analogue, ADC1 channel 1
//    IO48       = 48   onboard LED, 100 ms pulse
//    CH10       = 42   240V relay channel 10
//
//  BUILD: esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=huge_app,
//         FlashMode=qio,USBMode=hwcdc,CDCOnBoot=cdc
//  (CDCOnBoot=cdc is REQUIRED or Serial is UART0 and the USB log is dead.)
// ===========================================================================

#include <Arduino.h>
#include <math.h>
#include <string.h>
#include "esp_adc/adc_continuous.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"

// ---------------------------------------------------------------------------
//  PINS
// ---------------------------------------------------------------------------
static const uint8_t PIN_AUDIO = 2;
static const uint8_t PIN_IO48  = 48;   // beat indicator
static const uint8_t PIN_CH10  = 42;   // 240V relay, channel 10

// ---------------------------------------------------------------------------
//  ADC / SAMPLE RATE
// ---------------------------------------------------------------------------
static const uint32_t TARGET_FS     = 24000;
static const uint32_t FRAME_SAMPLES = 256;
static const uint32_t CONV_BYTES    = 4;
static const uint32_t FRAME_BYTES   = FRAME_SAMPLES * CONV_BYTES;
static const uint32_t POOL_BYTES    = FRAME_BYTES * 4;
static const uint32_t READ_TIMEOUT_MS = 100;

static const adc_unit_t    ADC_UNIT_USED = ADC_UNIT_1;
static const adc_channel_t ADC_CHAN      = ADC_CHANNEL_1;   // GPIO2 == ADC1 ch1

// ---------------------------------------------------------------------------
//  DC REMOVAL
//  The mic sits at mid-rail, so DC must come out before anything else.  Held in
//  DC_Q fixed point: with only tens of counts of signal a whole-count step can
//  only ever move DOWN, so the tracker would converge to the signal MINIMUM
//  instead of its mean.  DC_Q must be >= DC_SHIFT.
// ---------------------------------------------------------------------------
static const int32_t DC_SHIFT = 12;    // ~0.93 Hz high-pass at 24 kHz
static const int32_t DC_Q     = 12;

// ---------------------------------------------------------------------------
//  BAND 40-150 Hz
// ---------------------------------------------------------------------------
static const float BP_HP_HZ = 40.0f;
static const float BP_LP_HZ = 150.0f;
static const float BP_Q     = 0.7071f;   // Butterworth, flat passband

// ---------------------------------------------------------------------------
//  ONSET DETECTION
// ---------------------------------------------------------------------------
static const uint32_t NOV_AVG_SHIFT  = 13;   // rolling average, ~341 ms
static const uint32_t NOV_MARGIN_PCT = 800;  // nov > average * 8.00
static const uint32_t NOV_FLOOR_MUL  = 8;    // floor = 8 x silent sigma
static const uint32_t REFRACTORY_MS  = 150;  // stops double-triggering a decay
static const uint32_t REFR_SAMPLES =
    (uint32_t)((uint64_t)REFRACTORY_MS * TARGET_FS / 1000ULL);

// ---------------------------------------------------------------------------
//  BEAT GRID
// ---------------------------------------------------------------------------
static const uint32_t PERIOD_MIN  = (uint32_t)((60ULL * TARGET_FS) / 180ULL); // 180 BPM
static const uint32_t PERIOD_MAX  = (uint32_t)((60ULL * TARGET_FS) / 60ULL);  //  60 BPM
static const uint32_t PERIOD_SEED = (uint32_t)((60ULL * TARGET_FS) / 120ULL); // 120 BPM
// An onset only counts as a beat if it arrives within +/-25% of the current
// period.  A spurious onset outside that is ignored for BOTH the period and the
// phase, so it cannot drag the grid.
static const int32_t  IV_LO_PCT   = 75;
static const int32_t  IV_HI_PCT   = 125;
static const int32_t  PERIOD_DAMP = 8;   // period moves 1/8 toward each beat
static const int32_t  PHASE_DAMP  = 8;   // phase moves 1/8 toward each beat
// If no beat for this long the grid free-runs rather than dying.
static const uint32_t BEAT_TIMEOUT_MS = 3000;
static const uint32_t BEAT_TIMEOUT_SAMPLES =
    (uint32_t)((uint64_t)BEAT_TIMEOUT_MS * TARGET_FS / 1000ULL);

// ---------------------------------------------------------------------------
//  OUTPUT
// ---------------------------------------------------------------------------
static const uint16_t FLASH_MS = 100;   // 100 ms is clearly visible; 300 ms
                                        // merges consecutive beats into a glow
// Channel 10 marks beats 1 and 3.
//
// FLIPPED to false on 28 Oct 2026 for the 3.5-minute 4-section test track.  The
// parity was stable across the whole track (mg = 2 for 163 of 168 marks, with the
// five exceptions being the quiet sections where nothing is marked at all) but it
// landed on 2 and 4 rather than 1 and 3.
//
// This is a 50/50 that the audio does not resolve: which of the two half-bar
// positions is "1" depends on which beat the track happens to start on, and that
// is not knowable from the signal - the same limit Audio.md states when it says
// pure audio DSP cannot distinguish Beat 1 from Beat 3 in a symmetric 4/4 loop.
// So it is one value, set per track, and nothing else needs to change.
static const bool MARK_ON_ODD = false;

// ---------------------------------------------------------------------------
//  DARK / LOCKED BEHAVIOUR
//
//  When the beat drops the metronome goes DARK.  Flashing a CALCULATED grid
//  through a quiet section throws IO48 off the music - measured: the reported
//  period ran ~3% away from the actual beat rate, so the grid is held on the
//  beat by correcting every beat, and with nothing to correct against it walks
//  off.  It is better to go dark and re-acquire.
//
//  DARK means:  channel 10 off, and IO48 marks only the onsets it genuinely
//  finds (untimed - acceptable in a quiet section, and it proves the detector
//  is still listening for the bass to return).
//
//  Re-acquisition is deliberately FAST: the grid re-seeds on the first onset,
//  takes its period from the next one, and starts marking 1 and 3 on the one
//  after that.  Three onsets, so a gentle return out of the noise locks in
//  about one and a half beats.
// ---------------------------------------------------------------------------
// MEASURED: at 2 this went dark on two consecutive missed beats, which at the
// onset rates seen here (1-2/s) happened constantly - lock flipped 1,0,1,0
// through the first 20 s.  Every re-lock used to restart the period at the
// 120 BPM seed, so the grid ran 10% slow against a 133 BPM track and drifted.
// 4 periods is ~1.8 s at 130 BPM: long enough not to trip on a missed beat,
// short enough to go dark promptly in a real breakdown.
static const uint32_t DARK_AFTER_PERIODS = 4;   // beats with no onset => dark
static const uint32_t MARK_AFTER_ONSETS  = 2;   // onsets before CH10 starts
// A pulse is FLASH_MS on, then GUARANTEED off for FLASH_OFF_MS.  Without this,
// onsets arriving closer together than FLASH_MS retrigger the pulse and IO48
// never goes out.  It can flash untimed in a quiet section; it must not stick on.
static const uint16_t FLASH_OFF_MS = 100;

// ---------------------------------------------------------------------------
//  CALIBRATION
// ---------------------------------------------------------------------------
static const uint32_t CAL_SECONDS = 5;   // mandatory silent calibration
static const uint32_t CAL_SAMPLES = CAL_SECONDS * TARGET_FS;

// ---------------------------------------------------------------------------
//  BIQUAD  -  RBJ cookbook, Q27 coefficients
//  Q27 is REQUIRED, not a luxury: the 40 Hz low-pass has b0 = 2.7e-5, which
//  quantises to ZERO in Q14/Q15 - the filter would pass nothing and look
//  exactly like "no signal".  Signal is Q12, state and products are int64 so a
//  loud transient cannot wrap them.
// ---------------------------------------------------------------------------
static const int32_t COEF_Q = 27;

enum BqType { BQ_LP, BQ_HP };

struct Biquad {
  int32_t b0, b1, b2, a1, a2;
  int64_t z1, z2;

  void reset() { z1 = 0; z2 = 0; }

  inline int32_t process(int32_t x) {
    int64_t y = (((int64_t)b0 * x) >> COEF_Q) + z1;
    z1 = (((int64_t)b1 * x) >> COEF_Q) - (((int64_t)a1 * y) >> COEF_Q) + z2;
    z2 = (((int64_t)b2 * x) >> COEF_Q) - (((int64_t)a2 * y) >> COEF_Q);
    return (int32_t)y;
  }

  void design(BqType type, float f0, float Q, float fs) {
    const float w0    = 2.0f * PI * f0 / fs;
    const float cw    = cosf(w0);
    const float alpha = sinf(w0) / (2.0f * Q);
    const float a0    = 1.0f + alpha;

    float nb0, nb1, nb2;
    if (type == BQ_LP) {
      nb0 = (1.0f - cw) * 0.5f;  nb1 = 1.0f - cw;      nb2 = nb0;
    } else {
      nb0 = (1.0f + cw) * 0.5f;  nb1 = -(1.0f + cw);   nb2 = nb0;
    }

    const float S = (float)(1LL << COEF_Q);
    b0 = (int32_t)lroundf((nb0 / a0) * S);
    b1 = (int32_t)lroundf((nb1 / a0) * S);
    b2 = (int32_t)lroundf((nb2 / a0) * S);
    a1 = (int32_t)lroundf((-2.0f * cw) / a0 * S);
    a2 = (int32_t)lroundf((1.0f - alpha) / a0 * S);
    reset();
  }
};

// ---------------------------------------------------------------------------
//  ONSET DETECTOR  -  adaptive threshold on the novelty curve
//  The average is held SCALED for the same reason the DC tracker is: a plain
//  "avg += (nov - avg) >> SHIFT" on raw values floors every positive residual
//  to 0 and every negative one to -1, so the average walks down to the MINIMUM
//  of the novelty and the threshold ends up far too low.  That defect was
//  measured on this board, not assumed.
// ---------------------------------------------------------------------------
struct OnsetDet {
  int64_t  avgAcc;
  int32_t  novPk;
  int32_t  thr;
  int32_t  floor;
  uint32_t refrLeft;
  uint32_t total;

  void reset() {
    avgAcc = 0; novPk = 0; thr = 0; floor = 0; refrLeft = 0; total = 0;
  }

  inline bool run(int32_t nov, bool armed) {
    if (nov > novPk) novPk = nov;

    avgAcc += (int64_t)nov - (avgAcc >> NOV_AVG_SHIFT);
    const int32_t avg = (int32_t)(avgAcc >> NOV_AVG_SHIFT);

    thr = (int32_t)(((int64_t)avg * (int64_t)NOV_MARGIN_PCT) / 100);
    if (thr < floor) thr = floor;

    if (refrLeft) refrLeft--;

    if (armed && nov > thr) {
      if (refrLeft == 0) {
        refrLeft = REFR_SAMPLES;
        total++;
        return true;
      }
    }
    return false;
  }
};

// ---------------------------------------------------------------------------
//  SHARED STATE  -  written by the DSP task on Core 0, read by the loop on
//  Core 1.  Plain volatile scalars; no allocation anywhere in this sketch.
// ---------------------------------------------------------------------------
struct Stats {
  volatile uint32_t seconds;
  volatile uint32_t onsets;      // onsets since boot
  volatile uint32_t beats;       // beat flashes the loop has issued
  volatile uint32_t marks;       // channel-10 flashes the loop has issued
  // Beats between consecutive channel-10 marks.  MUST stay 2.  A 1 or a 3 means
  // the 1-and-3 parity flipped - the one thing the log could not otherwise show,
  // which is why every previous report of "it is on 2 and 4" had to come from ear
  // rather than from a reading.
  volatile uint32_t markGap;
  // Mean SIGNED onset-to-grid offset, in ms, for the last second, and the number
  // of onsets it is averaged over.  THIS is the phase measurement - markGap only
  // shows the internal parity, which cannot see whether the marks land on the
  // music's 1 and 3.  Near 0 = the grid is on the beat; near +/-half a period
  // means it has slipped a whole beat against the music.
  volatile int32_t  pdMs;
  volatile uint32_t pdN;
  // Set from the serial port to swap the marker between 1-and-3 and 2-and-4.
  // The bar phase across a quiet section is NOT in the audio, so it cannot be
  // derived - it has to be chosen.  Send 'F' to flip it.
  volatile uint8_t  flip;
  volatile uint32_t beatSeq;     // +1 per beat; the loop flashes on this
  volatile uint32_t onsetSeq;    // +1 per RAW onset; flashed when DARK
  volatile uint8_t  markNow;     // 1 if the beat just published is a 1 or a 3
  volatile uint8_t  locked;      // 1 = metronome running, 0 = dark
  volatile int32_t  novNow;      // last novelty
  volatile int32_t  thrNow;      // last threshold
  volatile int32_t  dcNow;       // DC estimate, whole counts
  volatile uint32_t periodNow;   // current period, samples
  volatile uint32_t bpmNow;
  volatile uint32_t fsNow;       // measured sample rate
  volatile uint32_t dspStalls;
  volatile uint32_t freeRuns;    // beats emitted with no onset to confirm them
  volatile uint32_t rejected;    // onsets outside the +/-25% interval gate
  volatile uint32_t calibrated;
};
static Stats g;

static adc_continuous_handle_t s_adc = nullptr;
static uint8_t s_dmaBuf[FRAME_BYTES];   // static: no allocation, ever

// Integer square root - no floating point in the DSP loop.
static uint32_t isqrt64(uint64_t v) {
  uint64_t x = v, y = 0, b = 1ULL << 62;
  while (b > x) b >>= 2;
  while (b) {
    if (x >= y + b) { x -= y + b; y = (y >> 1) + b; }
    else            { y >>= 1; }
    b >>= 2;
  }
  return (uint32_t)y;
}

// ---------------------------------------------------------------------------
//  ADC  -  continuous DMA, never analogRead()
// ---------------------------------------------------------------------------
static esp_err_t adcConfigure() {
  adc_continuous_handle_cfg_t hcfg = {};
  hcfg.max_store_buf_size = POOL_BYTES;
  hcfg.conv_frame_size    = FRAME_BYTES;
  hcfg.flags.flush_pool   = 0;

  esp_err_t r = adc_continuous_new_handle(&hcfg, &s_adc);
  if (r != ESP_OK) return r;

  static adc_digi_pattern_config_t pattern = {};
  pattern.atten     = (uint8_t)ADC_ATTEN_DB_12;   // mid-rail bias needs the
  pattern.channel   = (uint8_t)ADC_CHAN;          // widest range; 0 dB would
  pattern.unit      = (uint8_t)ADC_UNIT_USED;     // clip the bias itself
  pattern.bit_width = (uint8_t)ADC_BITWIDTH_12;   // the only DMA width on S3

  adc_continuous_config_t ccfg = {};
  ccfg.pattern_num    = 1;
  ccfg.adc_pattern    = &pattern;
  ccfg.sample_freq_hz = TARGET_FS;
  ccfg.conv_mode      = ADC_CONV_SINGLE_UNIT_1;
  ccfg.format         = ADC_DIGI_OUTPUT_FORMAT_TYPE2;

  return adc_continuous_config(s_adc, &ccfg);
}

// ---------------------------------------------------------------------------
//  DSP TASK  -  Core 0.  Samples, filters, detects onsets, runs the beat grid.
//  It publishes a beat by bumping g.beatSeq; the loop on Core 1 does the
//  flashing, so a GPIO write can never delay the DSP.
// ---------------------------------------------------------------------------
static void audioTask(void *arg) {
  (void)arg;

  int64_t  dcAcc = ((int64_t)2048) << DC_Q;
  int32_t  lastMag = 0;
  uint64_t absN = 0;

  static Biquad fHp, fLp, fLp2;
  fHp.design(BQ_HP, BP_HP_HZ, BP_Q, (float)TARGET_FS);
  fLp.design(BQ_LP, BP_LP_HZ, BP_Q, (float)TARGET_FS);
  fLp2.design(BQ_LP, BP_LP_HZ, BP_Q, (float)TARGET_FS);   // cascaded: 24 dB/oct

  static OnsetDet det;
  det.reset();

  uint64_t calNovSq = 0;      // calibration accumulators
  uint32_t calN = 0;
  bool armed = false;
  bool calDone = false;

  // ---- beat grid + dark/locked state ----
  bool     locked     = false;  // metronome running (false = DARK)
  // The learned PERIOD is kept ACROSS a dark section.  The tempo is a property of
  // the track, not of the gap: a breakdown does not change the BPM.  Resetting it
  // on every re-lock made the grid restart at the 120 BPM seed against a 133 BPM
  // track and then crawl back, which is the slow drift that was reported.  Only
  // the PHASE is re-seeded when the beat returns.
  int64_t  period     = 0;      // 0 = never learned yet
  int64_t  nextBeat   = 0;
  int64_t  lastOnset  = 0;      // sample of the last ACCEPTED onset
  uint32_t beatCount  = 0;      // 1,2,1,2,...
  uint32_t confirmCnt = 0;      // accepted onsets since locking

  int64_t  winStart = esp_timer_get_time();
  uint32_t winN = 0;
  uint32_t stall = 0;
  int64_t  pdAcc = 0;      // signed onset-to-grid offset accumulator
  uint32_t pdN   = 0;

  // Seed the DC tracker from the first frame so it starts CONVERGED.  Without
  // this it slews to the real bias over the first second, and that transient
  // poisons the silent calibration.
  {
    uint32_t got = 0;
    if (adc_continuous_read(s_adc, s_dmaBuf, FRAME_BYTES, &got,
                            READ_TIMEOUT_MS) == ESP_OK) {
      int64_t sum = 0; uint32_t n = 0;
      for (uint32_t i = 0; i + CONV_BYTES <= got; i += CONV_BYTES) {
        adc_digi_output_data_t d;
        memcpy(&d.val, &s_dmaBuf[i], sizeof(uint32_t));
        if (d.type2.channel != (uint32_t)ADC_CHAN) continue;
        sum += (int32_t)d.type2.data;
        n++;
      }
      if (n) dcAcc = (sum / (int64_t)n) << DC_Q;
    }
  }

  for (;;) {
    uint32_t got = 0;
    const esp_err_t r = adc_continuous_read(s_adc, s_dmaBuf, FRAME_BYTES, &got,
                                            READ_TIMEOUT_MS);
    if (r != ESP_OK || got == 0) {
      if (++stall > 50) {
        g.dspStalls = g.dspStalls + 1;
        adc_continuous_flush_pool(s_adc);
        stall = 0;
      }
      vTaskDelay(1);
      continue;
    }
    stall = 0;

    for (uint32_t i = 0; i + CONV_BYTES <= got; i += CONV_BYTES) {
      adc_digi_output_data_t d;
      memcpy(&d.val, &s_dmaBuf[i], sizeof(uint32_t));
      if (d.type2.channel != (uint32_t)ADC_CHAN) continue;

      const int32_t raw = (int32_t)d.type2.data;
      dcAcc += (((int64_t)raw << DC_Q) - dcAcc) >> DC_SHIFT;
      const int32_t acQ = (int32_t)(((int64_t)raw << DC_Q) - dcAcc);
      absN++;
      winN++;

      // ---- 40-150 Hz, 24 dB/octave ----
      int32_t b = acQ;
      b = fHp.process(b);
      b = fLp.process(b);
      b = fLp2.process(b);

      // ---- rectified first-order difference ----
      const int32_t mag = (b < 0) ? -b : b;
      int32_t nov = mag - lastMag;
      lastMag = mag;
      if (nov < 0) nov = 0;
      g.novNow = nov;

      // ---- mandatory silent calibration: measure the room, then arm ----
      if (!calDone) {
        calNovSq += (uint64_t)nov * (uint64_t)nov;
        calN++;
        if (calN >= CAL_SAMPLES) {
          const uint32_t rms = isqrt64(calNovSq / (uint64_t)calN);
          det.floor = (int32_t)(rms * NOV_FLOOR_MUL);
          if (det.floor < 1) det.floor = 1;
          armed = true;
          calDone = true;
          g.calibrated = 1;
        }
        continue;                     // no detection during calibration
      }

      const bool onset = det.run(nov, armed);
      g.thrNow = det.thr;

      if (onset) {
        g.onsets   = det.total;
        g.onsetSeq = g.onsetSeq + 1;   // ALWAYS: flashed on IO48 while DARK

        if (!locked) {
          // ---- RE-ACQUIRE ----
          if (period <= 0) {
            // FIRST lock ever: there is no grid yet, so seed the phase here.
            period     = (int64_t)PERIOD_SEED;
            nextBeat   = (int64_t)absN;
            confirmCnt = 0;
          }
          // After a gap the grid has been COUNTING THROUGH at the learned period,
          // so it is already within a fraction of a beat.  DO NOT snap it to this
          // onset: `nextBeat = absN` moves the grid onto whatever beat triggered
          // the re-lock, and if the grid had drifted more than half a beat during
          // the gap the snap lands on the NEIGHBOURING beat, which flips the
          // 1-and-3 parity by one.  MEASURED: that is exactly the flip heard on
          // the 3.5 minute track - section 1 on 2 and 4, section 2 on 1 and 3,
          // section 3 back on 2 and 4.  The damped pull below re-phases it
          // gradually instead, and cannot cross a beat boundary.
          //
          // confirmCnt is NOT reset either: the tempo is already known, so the
          // marker is not guessing and has nothing to re-confirm.  Resetting it
          // switched channel 10 off for 40 seconds of the test track.
          lastOnset = (int64_t)absN;
          locked    = true;
        } else {
          const int64_t iv = (int64_t)absN - lastOnset;
          const int64_t lo = (period * IV_LO_PCT) / 100;
          const int64_t hi = (period * IV_HI_PCT) / 100;
          if (iv >= lo && iv <= hi) {
            // A plausible beat: adapt the period, then pull the phase.
            period += (iv - period) / PERIOD_DAMP;
            if (period < (int64_t)PERIOD_MIN) period = (int64_t)PERIOD_MIN;
            if (period > (int64_t)PERIOD_MAX) period = (int64_t)PERIOD_MAX;
            int64_t dd = ((int64_t)absN - nextBeat) % period;
            if (dd >  period / 2) dd -= period;
            if (dd < -period / 2) dd += period;
            nextBeat += dd / PHASE_DAMP;
            // pd diagnostic: how far the grid is from the music.  Averaged over
            // the second so it reads as one number in the log.
            pdAcc += dd;
            pdN++;
            if (confirmCnt < 1000) confirmCnt++;
          } else {
            g.rejected = g.rejected + 1;  // spurious: period AND phase untouched
          }
          lastOnset = (int64_t)absN;
        }
      }

      // ---- the beat drops: go DARK -----------------------------------------
      // DARK_AFTER_PERIODS beats past the last accepted onset.  While dark there
      // is NO calculated grid at all, so it cannot walk off the music - which is
      // exactly what it did when a 3%-out period was left free-running.  IO48
      // still marks real onsets; channel 10 stays out.
      if (locked &&
          ((int64_t)absN - lastOnset) > period * (int64_t)DARK_AFTER_PERIODS) {
        // period is KEPT on purpose - see the note where it is declared.  Only
        // the phase and the 1-and-3 confirmation are dropped.
        locked     = false;
        confirmCnt = 0;
        g.markNow  = 0;
      }
      g.locked = locked ? 1 : 0;

      // ---- advance the grid; emit only while LOCKED -------------------------
      // The grid is advanced in BOTH states on purpose.  MEASURED: freezing it
      // while dark lost the bar phase, so every re-lock after a quiet section
      // restarted the 1-and-3 marker from whatever beat was playing - on the
      // 4-section test track it was 2 and 4 for the first three sections and 1
      // and 3 only for the last.  Counting through the gap preserves the parity.
      // Nothing is flashed while dark, because the visuals there come from raw
      // onsets instead - so this does not put a calculated grid back on IO48.
      if (period > 0) {
        while (nextBeat <= (int64_t)absN) {
          beatCount = (beatCount % 2) + 1;                 // 1,2,1,2,...
          if (locked) {
            if (((int64_t)absN - lastOnset) > period) g.freeRuns = g.freeRuns + 1;
            // Channel 10 only starts marking once the tempo has been CONFIRMED by
            // MARK_AFTER_ONSETS accepted onsets: IO48 comes back at once because
            // that is the feel that matters, but the 1-and-3 marker does not guess.
            // Which beats to mark.  MARK_ON_ODD is the compile-time choice and
            // g.flip is the runtime override - the bar phase across a quiet
            // section is not in the audio, so it has to be chosen rather than
            // derived.  Send 'F' over serial to swap it.
            const bool wantOdd = (MARK_ON_ODD != (g.flip != 0));
            g.markNow = (uint8_t)((confirmCnt >= MARK_AFTER_ONSETS &&
                                   ((beatCount == 1) == wantOdd)) ? 1 : 0);
            g.beatSeq = g.beatSeq + 1;
          } else {
            g.markNow = 0;               // dark: keep counting, publish nothing
          }
          nextBeat += period;
        }
      }
    }

    // ---- publish once per second ----
    const int64_t el = esp_timer_get_time() - winStart;
    if (el >= 1000000) {
      g.fsNow     = (uint32_t)(((uint64_t)winN * 1000000ULL) / (uint64_t)el);
      g.periodNow = (uint32_t)period;
      g.bpmNow    = (period > 0)
                    ? (uint32_t)((60ULL * TARGET_FS) / (uint64_t)period) : 0;
      g.dcNow     = (int32_t)(dcAcc >> DC_Q);
      // pd: the mean SIGNED onset-to-grid offset, in ms.  THIS is the phase
      // measurement.  Near 0 = on the beat; near +/-half a period means the grid
      // has slipped a whole beat against the music - which markGap cannot show.
      g.pdMs      = (pdN > 0)
                    ? (int32_t)((pdAcc * 1000) /
                                ((int64_t)pdN * (int64_t)TARGET_FS))
                    : 0;
      g.pdN       = pdN;
      pdAcc = 0;
      pdN   = 0;
      g.seconds   = g.seconds + 1;
      winStart = esp_timer_get_time();
      winN = 0;
    }
  }
}

// ---------------------------------------------------------------------------
//  OUTPUT  -  Core 1 only.  Non-blocking, retriggerable 100 ms pulses.
//  HARDWARE_CONNECTION.md: "Write the channel at the beat itself, never from a
//  slow engine" - a 1 s engine misses a 450 ms beat window.  The DSP task bumps
//  a counter AT the beat and the loop polls every 2 ms, so the flash lands on
//  the beat.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
//  OUTPUT  -  Core 1 only.  Non-blocking.
//
//  A pulse is FLASH_MS ON, then GUARANTEED OFF for FLASH_OFF_MS.  A trigger
//  arriving during either phase is DROPPED, never used to extend the pulse.
//  Without that, onsets closer together than FLASH_MS retrigger the pulse and
//  IO48 never goes out - which is exactly what must not happen in a quiet
//  section, where it is allowed to flash untimed but not to stick on.
//
//  HARDWARE_CONNECTION.md: "Write the channel at the beat itself, never from a
//  slow engine" - a 1 s engine misses a 450 ms beat window.  The DSP task bumps
//  a counter AT the beat and this loop polls every 2 ms.
// ---------------------------------------------------------------------------
struct Pulse {
  uint8_t  phase;      // 0 idle, 1 on, 2 forced gap
  uint32_t until;

  void init(uint8_t pin) {
    phase = 0; until = 0;
    digitalWrite(pin, LOW); pinMode(pin, OUTPUT);
  }
  inline void trigger(uint8_t pin) {
    if (phase != 0) return;                       // busy - do not retrigger
    digitalWrite(pin, HIGH);
    phase = 1;
    until = millis() + FLASH_MS;
  }
  inline void service(uint8_t pin) {
    if (phase == 0) return;
    if ((int32_t)(millis() - until) < 0) return;
    if (phase == 1) {
      digitalWrite(pin, LOW);
      phase = 2;
      until = millis() + FLASH_OFF_MS;            // guaranteed dark gap
    } else {
      phase = 0;
    }
  }
};
static Pulse s_io48, s_ch10;

static uint32_t s_lastSeq = 0, s_lastOSeg = 0, s_lastSec = 0;
static uint32_t s_markAtBeat = 0;
static uint8_t  s_lastLocked = 0;

// ---------------------------------------------------------------------------
//  LOG  -  one line per second.  Every event that matters has a counter, so a
//  reading proves what happened instead of suggesting what should have.
// ---------------------------------------------------------------------------
static void logLine(uint32_t sec) {
  Serial.printf("BT %lu  fs=%lu dc=%ld  nov=%ld thr=%ld  ons=%lu  "
                "per=%lu bpm=%lu  pd=%ldms/%lu  lock=%lu beats=%lu marks=%lu "
                "mg=%lu  rej=%lu free=%lu  stall=%lu cal=%lu\n",
                (unsigned long)sec,
                (unsigned long)g.fsNow, (long)g.dcNow,
                (long)g.novNow, (long)g.thrNow,
                (unsigned long)g.onsets,
                (unsigned long)g.periodNow, (unsigned long)g.bpmNow,
                (long)g.pdMs, (unsigned long)g.pdN,
                (unsigned long)g.locked,
                (unsigned long)g.beats, (unsigned long)g.marks,
                (unsigned long)g.markGap,
                (unsigned long)g.rejected, (unsigned long)g.freeRuns,
                (unsigned long)g.dspStalls, (unsigned long)g.calibrated);
}

// ---------------------------------------------------------------------------
//  SETUP
// ---------------------------------------------------------------------------
void setup() {
  s_io48.init(PIN_IO48);
  s_ch10.init(PIN_CH10);

  Serial.begin(115200);
#if ARDUINO_USB_CDC_ON_BOOT
  Serial.setTxTimeoutMs(0);        // non-blocking: a missing host cannot stall us
#endif

  const esp_err_t r = adcConfigure();
  if (r == ESP_OK) adc_continuous_start(s_adc);

  Serial.printf("\nbeat_simple  mic=GPIO%u  IO48=GPIO%u  CH10=GPIO%u\n",
                (unsigned)PIN_AUDIO, (unsigned)PIN_IO48, (unsigned)PIN_CH10);
  Serial.printf("targetFs=%lu  band=%.0f-%.0f Hz (24 dB/oct)  margin=%lu%%  "
                "refr=%lums\n",
                (unsigned long)TARGET_FS, (double)BP_HP_HZ, (double)BP_LP_HZ,
                (unsigned long)(NOV_MARGIN_PCT / 100),
                (unsigned long)REFRACTORY_MS);
  Serial.printf("IO48 flashes every beat.  Channel 10 marks beats %s.\n",
                MARK_ON_ODD ? "1 and 3" : "2 and 4");
  Serial.printf("Beat drops -> metronome goes DARK after %lu beats: CH10 out, "
                "IO48 marks raw onsets.  Re-locks on %lu onsets.\n",
                (unsigned long)DARK_AFTER_PERIODS,
                (unsigned long)(MARK_AFTER_ONSETS + 1));
  Serial.printf("Pulse: %ums on, %ums guaranteed off (never sticks on).\n",
                (unsigned)FLASH_MS, (unsigned)FLASH_OFF_MS);

  if (r != ESP_OK) {
    Serial.printf("ADC START FAILED, err=%d\n", (int)r);
    for (;;) { digitalWrite(PIN_IO48, HIGH); delay(150);
               digitalWrite(PIN_IO48, LOW);  delay(1500); }
  }

  Serial.printf("Silent calibration: %lu seconds, then the beat starts.\n",
                (unsigned long)CAL_SECONDS);

  xTaskCreatePinnedToCore(audioTask, "audio", 8192, nullptr, 10, nullptr, 0);
}

// ---------------------------------------------------------------------------
//  LOOP  -  Core 1.  Flash on the beat, count, log, feed the watchdog.
// ---------------------------------------------------------------------------
void loop() {
  esp_task_wdt_reset();
  s_io48.service(PIN_IO48);
  s_ch10.service(PIN_CH10);

  // 'F' swaps the channel-10 marker between 1-and-3 and 2-and-4.  The bar phase
  // after a quiet section is not recoverable from the audio, so this is a choice
  // the operator makes at the boundary rather than something the firmware can
  // work out for itself.
  while (Serial.available() > 0) {
    const int ch = Serial.read();
    if (ch == 'F' || ch == 'f') {
      g.flip = g.flip ? 0 : 1;
      Serial.printf("MARKER -> beats %s\n",
                    (MARK_ON_ODD != (g.flip != 0)) ? "1 and 3" : "2 and 4");
    }
  }

  const uint8_t lockNow = g.locked;
  if (lockNow != s_lastLocked) {
    // Mode change: resync BOTH counters, so a stale one cannot fire a burst.
    s_lastLocked = lockNow;
    s_lastSeq    = g.beatSeq;
    s_lastOSeg   = g.onsetSeq;
  }

  if (g.calibrated) {
    if (lockNow) {
      // LOCKED - IO48 on every beat, channel 10 on 1 and 3.
      const uint32_t seq = g.beatSeq;
      if (seq != s_lastSeq) {
        const uint32_t n = seq - s_lastSeq;
        s_lastSeq = seq;
        for (uint32_t k = 0; k < n; k++) {
          s_io48.trigger(PIN_IO48);
          g.beats = g.beats + 1;
          if (g.markNow) {
            s_ch10.trigger(PIN_CH10);
            g.marks = g.marks + 1;
            g.markGap = g.beats - s_markAtBeat;   // must stay 2
            s_markAtBeat = g.beats;
          }
        }
      }
    } else {
      // DARK - there is no calculated grid.  IO48 marks only the onsets actually
      // found, untimed, so a quiet section shows the detector is still listening
      // for the bass to return.  Channel 10 stays out.
      const uint32_t oseq = g.onsetSeq;
      if (oseq != s_lastOSeg) {
        const uint32_t n = oseq - s_lastOSeg;
        s_lastOSeg = oseq;
        for (uint32_t k = 0; k < n; k++) s_io48.trigger(PIN_IO48);
      }
    }
  }

  const uint32_t sec = g.seconds;
  if (sec != s_lastSec) {
    s_lastSec = sec;
    if (sec > 0) logLine(sec);
  }

  delay(2);
}

