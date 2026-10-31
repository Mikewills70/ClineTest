// ============================================================================
//  beat_track.ino  --  STAGE B : KICK-BAND ISOLATION AND ONSET DETECTION
// ----------------------------------------------------------------------------
//  Clean-slate rhythm tracker for the ESP32-S3 light-show controller.
//  Nothing from any previous sketch is included or referenced.
//
//  STAGE A (complete, retained): the DMA front end
//    - Continuous DMA ADC capture of the MAX9814 on GPIO2 (ADC1 channel 1).
//      No analogRead anywhere: the brief forbids it and it is right to.
//    - Requested 24 kHz; the ACTUAL rate is measured from the DMA stream and
//      published.  It is never assumed.
//    - DC removal by a FRACTIONAL tracker, verified every second against the
//      exact window mean (dc vs dcE) and by the symmetry counter (neg%).
//    - Per-second broadband measurement: min, max, peak-to-peak, RMS, all-time
//      peak-to-peak, activity percentage.
//    - Mains-ripple probe at 50/90/100/120 Hz.  90 Hz is a control frequency
//      that mains cannot produce, so a 100 Hz reading can be judged against it.
//    - DMA stall detection with in-task recovery (stop/start), counted.
//
//  STAGE B (this build): Audio.md Stage 1 and Stage 2, and NOTHING ELSE
//    - Stage 1 sub-band isolation 40-150 Hz as a fixed-point biquad pair:
//        HPF 40 Hz -> LPF 150 Hz
//      ("a digital low-pass filter (or fixed-point biquad filter) to isolate
//       the low-frequency sub-band (40 Hz to 150 Hz)")
//    - Stage 1 novelty: full-wave rectification, then the first-order
//      difference dx[n] = |x[n]| - |x[n-1]|.  The negative half is discarded,
//      because a decay is not an onset.
//    - Stage 2: onset when the peak exceeds the rolling local average by a
//      mathematically tuned adaptive threshold margin, floored by the baseline
//      measured during the 5-second silent calibration, with a 150 ms
//      refractory lockout.
//    - DIAGNOSTICS ONLY, no effect on the algorithm: onsets fired, samples
//      above threshold inside the lockout, novelty peak, average, effective
//      threshold, calibrated floor, sigma, in-band RMS, and the inter-onset
//      intervals in ms.  Audio.md requires a telemetry stream carrying the
//      novelty curve, the threshold curve and the onset pulses; the intervals
//      are added because a fire COUNT cannot distinguish "kick plus offbeat
//      bass" from "one kick double-triggering", and the margin tuning decision
//      rests on exactly that distinction.
//
//  TUNING STATUS: the margin (NOV_MARGIN_PCT) is the ONE value Audio.md leaves
//  open - it says "mathematically tuned" and gives no number.  MEASURED at 2.5x
//  on a 130 BPM track (2.17 beats/s): the novelty sat above the threshold about
//  15% of the time and the detector free-ran at the refractory rate, 6.31
//  onsets/s.  Each tuning step is reported rather than picked blind.
//
//  NOT BUILT YET, and deliberately NOT pre-empted here: Audio.md's three-band
//  fusion (40-150 / 500-2000 / 5000-10000), its dual-timescale self-normaliser
//  (50 ms + 5-10 s), its 3-5 agent tempo pool, Stage 3 (tempo lock) and Stage 4
//  (modulo-4 bar counter and downbeat).
//
//  IO48: 5 flashes at 1 Hz during the 5-second silent calibration, then ONE
//  100 ms flash per detected onset - so the onset detector can be judged by
//  ear against the music.  Non-blocking and retriggerable.
//
//  CORE SPLIT (answer C6): the DSP task is pinned to Core 0; logic and visual
//  output stay on Core 1 in the Arduino loop.  Wi-Fi is not used in this test,
//  so Core 0 belongs to the DSP alone.
//
//  HARDWARE (from HARDWARE_CONNECTION.md)
//    MIC      GPIO2              MAX9814 analogue out, ADC1 channel 1
//    IO48     GPIO48             status / beat indicator, 100 ms pulse
//    CH1..4   GPIO11,12,13,14    opto-coupled channels, HIGH = on
//
//  DESIGN TARGET: the measured signal is tiny.  Silence spans 1-3 counts and
//  a bass beat only 5-30 counts, so every constant here is single-digit
//  counts.  ADC attenuation MUST be 12 dB: the microphone idles at mid-rail
//  (~1.65 V) and 0 dB would clip the bias itself.
// ============================================================================

#include "esp_adc/adc_continuous.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// ---------------------------------------------------------------------------
//  CONFIGURATION
// ---------------------------------------------------------------------------
static const uint8_t PIN_MIC    = 2;                    // MAX9814 analogue out
static const uint8_t PIN_IO48   = 48;                   // status / beat LED
static const uint8_t CH_PINS[4] = { 11, 12, 13, 14 };   // 240V channels 1-4

static const adc_unit_t    ADC_UNIT_USED = ADC_UNIT_1;
static const adc_channel_t ADC_CHAN      = ADC_CHANNEL_1;   // GPIO2 == ADC1 ch1

static const uint32_t TARGET_FS      = 24000;   // requested; actual is measured
static const uint32_t FRAME_SAMPLES  = 256;     // one DMA conversion frame
static const uint32_t CONV_BYTES     = 4;       // SOC_ADC_DIGI_DATA_BYTES_PER_CONV
static const uint32_t FRAME_BYTES    = FRAME_SAMPLES * CONV_BYTES;
static const uint32_t POOL_BYTES     = FRAME_BYTES * 4;
static const uint32_t WINDOW_SAMPLES = TARGET_FS;   // one published window

static const uint16_t FLASH_MS        = 100;   // visible, does not merge
static const uint16_t CAL_SECONDS     = 5;     // silent calibration length
static const uint32_t READ_TIMEOUT_MS = 100;
static const uint32_t STALL_LIMIT     = 50;    // consecutive timeouts -> recover

static const int32_t DC_SHIFT  = 12;   // tracker averaging: ~0.93 Hz at 24 kHz
static const int32_t DC_Q      = 12;   // fractional bits in the DC tracker.
                                       // MUST be >= DC_SHIFT.  The step is a
                                       // floor of residual>>DC_SHIFT, so with
                                       // fewer fractional bits the DOWN step
                                       // stays at 1 unit while the UP step
                                       // needs 2^(DC_SHIFT-DC_Q) counts, and
                                       // the tracker parks on the signal
                                       // MINIMUM.  Measured on this board:
                                       // DC_Q=6 sat 33 counts low, DC_Q=12 is
                                       // exact (mean=0, neg=50%).
static const int32_t ACT_COUNT = 8;    // "activity" threshold, in counts

// --- Stage B: kick-band isolation and onset detection ----------------------
// STRICTLY Audio.md Stage 1 and Stage 2.  Every mechanism below is named in the
// document.  Everything I previously added has been REMOVED: an envelope
// follower, a decaying-peak threshold term, mains notches, a 1-5 kHz click band
// with a fusion gate, and a parallel threshold sweep.  Those were either not in
// the document at all, or belong to stages not yet built - and pre-empting a
// later stage is what makes it fail.
static const float    BP_HP_HZ    = 40.0f;    // kick band lower edge
static const float    BP_LP_HZ    = 150.0f;   // kick band upper edge
static const float    BP_Q        = 0.7071f;  // Butterworth, flat passband

// Stage 2 requires the peak to exceed "the local average by a MATHEMATICALLY
// TUNED adaptive threshold margin".  The document gives no number, so this is
// the ONE value that must be tuned by measurement.  Tuning history against a
// 130 BPM track (2.17 beats/s):
//   margin 2.5x  -> novelty above threshold ~15% of the time, detector
//                   free-running at the refractory rate (6.31 onsets/s)
//   harder gate  -> above-threshold time ~1%, rate 3.75 onsets/s, so the
//                   detector DOES become event-driven as the margin rises.
// Each tuning step is reported rather than picked blind.
static const uint32_t NOV_AVG_SHIFT  = 13;    // rolling average window ~341 ms

// Stage 2 requires the peak to exceed "the local average by a MATHEMATICALLY
// TUNED adaptive threshold margin".  TUNING HISTORY, all measured on a 130 BPM
// track (2.17 beats/s):
//   2.5x -> threshold = 23% of the peak novelty; the novelty sat above it ~15%
//           of the time, so onsets were spaced by the REFRACTORY, not the
//           music: 6.08 candidates/s, 3.67 validated/s.  Stage 3 was then fed
//           that train and its comb filter found NO periodicity at all - the
//           best of 121 candidate periods, at its best phase, captured only
//           1-4 of the ~15 onsets in the window, confidence 3%, and it never
//           locked on 130 BPM.  Stage 3 was reporting, correctly, that its
//           input had no beat in it.
//   8.0x -> threshold ~75% of the peak novelty, which is the point of the
//           change: thin the train until the onsets are events again.
static const uint32_t NOV_MARGIN_PCT = 800;   // low band: nov > average * 8.00
// The fusion accent bands keep their own margin so the two can be tuned
// independently and their effects attributed separately.
static const uint32_t ACCENT_MARGIN_PCT = 250;
static const uint32_t NOV_FLOOR_MUL  = 8;     // baseline floor = 8 x the silent
                                              // noise sigma measured at boot.
                                              // TUNING HISTORY (measured):
                                              //  3x sigma = 102 mc, but silent
                                              //  novelty peaks reach 145-168 mc,
                                              //  so the detector fired 6/s in
                                              //  silence.  The novelty is SPIKY
                                              //  (mostly ~0 with occasional
                                              //  spikes), so its sigma is small
                                              //  against its peaks - a 5-sigma
                                              //  spike needs more than 3x sigma.
                                              //  8x sigma sits above the measured
                                              //  silent peak range.
static const uint32_t REFRACTORY_MS  = 150;   // lockout after an onset
static const uint32_t REFR_SAMPLES   = (uint32_t)((uint64_t)REFRACTORY_MS * TARGET_FS / 1000ULL);

// Diagnostic only - it does not affect the algorithm.  Audio.md requires a
// telemetry stream carrying the novelty curve, the threshold curve and the
// onset pulses; this adds the inter-onset intervals, because a fire COUNT
// cannot tell "kick plus offbeat bass" apart from "one kick double-triggering",
// and that distinction is what the margin tuning decision rests on.
static const int MAX_IOI = 12;

// --- Multi-band spectral flux fusion (Audio.md supplemental section 1) ------
// "compute parallel spectral flux or energy differentials across three separate
//  bands using lightweight Goertzel algorithms or fixed-point FFT bins:
//    Low Band (40-150 Hz): Kick drum tracking.
//    Mid Band (500-2000 Hz): Snare/clap transient tracking.
//    High Band (5000-10000 Hz): Hi-hat / continuous rhythm grid reinforcement.
//  Fusion Logic: A beat is only validated when a low-band transient coincides
//  with or closely precedes a mid-band (snare) or high-band accent, rejecting
//  false single-frequency energy bloat."
//
// The LOW band is the Stage 1 + Stage 2 path already built (biquad, rectify,
// first-order difference).  MID and HIGH are computed here with lightweight
// Goertzel bins over short frames, as the document names, and each gets its own
// Stage 2 adaptive threshold.
static const uint32_t FUS_FRAME_SAMPLES  = 240;   // 10 ms at 24 kHz -> 100 Hz bins
static const uint32_t FUS_FRAMES_PER_SEC = TARGET_FS / FUS_FRAME_SAMPLES;

static const uint8_t  MID_NBINS  = 6;
static const uint32_t MID_HZ[MID_NBINS]  = { 500, 800, 1100, 1400, 1700, 2000 };
static const uint8_t  HIGH_NBINS = 6;
static const uint32_t HIGH_HZ[HIGH_NBINS] = { 5000, 6000, 7000, 8000, 9000, 10000 };

// Goertzel |X| for a bin of amplitude A over N samples is A*N/2; with A in Q12
// counts that is large, so each bin magnitude is scaled down before summing.
static const int32_t  BIN_SHIFT = 10;

// "coincides with or closely precedes": a low-band transient is valid if a
// mid/high accent lands within this window of it, either side.
static const uint32_t COINC_MS      = 40;
static const uint32_t COINC_SAMPLES = (uint32_t)((uint64_t)COINC_MS * TARGET_FS / 1000ULL);
static const uint8_t  MAX_PENDING   = 8;      // low-band candidates held for fusion
static const uint32_t FUS_REFR_FRAMES = (uint32_t)((uint64_t)REFRACTORY_MS * FUS_FRAMES_PER_SEC / 1000ULL);

// --- Stage 3: tempo tracking via periodicity analysis ----------------------
// Audio.md: "Pass the stream of detected onsets through an autocorrelation
// array, a COMB FILTER BANK, or an adaptive phase-locked loop (PLL) ... to
// evaluate temporal spacing" and "Continuously calculate and lock onto the
// dominant periodicity (BPM range: 60 to 180 BPM).  Maintain a confidence
// score; if the rhythm drops or changes (breakdown), utilize predictive
// filtering to maintain phase continuity until a new stable tempo emerges."
//
// Implemented as a comb filter bank over 60-180 BPM, feeding a 4-agent pool,
// which the document's supplemental section 2 requires: "Maintain 3 to 5
// simultaneous competing tempo/phase hypotheses ... Continuously score each
// agent based on how accurately its predicted grid matches incoming onsets.
// Dynamically promote the highest-scoring agent while keeping others running
// in the background to prevent catastrophic reset jumps."
static const uint32_t T_BIN_SAMPLES = 120;      // onset-train resolution: 5 ms
static const uint32_t T_BINS        = 800;      // 4 s of onset history
static const uint32_t BPM_MIN       = 60;
static const uint32_t BPM_MAX       = 180;
static const uint8_t  NAGENT        = 4;        // "3 to 5"
// MEASURED: the validated onsets jitter by 20-45 ms around the beat (ad, the mean
// absolute onset-to-grid distance, sat at 20-45 ms once the grid was on the music).
// A +/-20 ms lock window is therefore tighter than the material's own jitter, so a
// CORRECT-tempo agent collected misses instead of hits and its score stayed at 0 -
// after which any wrong-tempo agent with a single lucky hit (a1=65/8, a3=86/8) was
// promoted and the metronome dropped to half rate.  That is the reported "lost a
// few beats" and the tempo changing.  10 bins = +/-50 ms: wide enough to accept the
// real jitter, still only 11% of a 461 ms beat, so it cannot lock onto a
// neighbouring beat.
static const uint32_t PLL_TOL_BINS  = 10;       // +/-50 ms counts as a hit.
// PLL CAPTURE RANGE - deliberately wider than the lock window.
// Audio.md section 2: "a continuous smoothing buffer and ERROR-CORRECTION WINDOW
// must be implemented".  MEASURED DEFECT this fixes: the lock window was ALSO the
// only window in which the phase could be corrected, so a grid more than 20 ms off
// the onsets could never be pulled back.  Observed: agent 0 at the correct 131 BPM
// sat 33, 39, 54, 38, 56 ms off the onsets - earning no hits, therefore no
// correction, therefore drifting to 125, 163, 212, 218 ms and dying, at which point
// the pool promoted a wrong-tempo agent (65 / 180 / 87 BPM) and the metronome tempo
// changed.  A PLL needs a capture range wider than its lock range; that is the whole
// of this constant.  24 bins = 120 ms, safely inside half a beat (46 bins at
// 130 BPM) so it can never lock onto the neighbouring beat.
static const uint32_t PLL_PULL_BINS = 24;       // +/-120 ms can be corrected
                                                // +/-10 ms was too tight: a 2%
                                                // period error drifts out of it
                                                // within two beats
static const int32_t  AGENT_GAIN    = 8;        // score for a hit
// MEASURED DEFECT this fixes: the promotion kept handing the metronome to the
// DOUBLE-tempo agent (a2=180 while the comb said 129-131), and the tempo changed
// with it.  With GAIN 8 against LOSS 1 a denser grid was strictly favoured - it
// collects more hits because it has more chances to land within 20 ms of an onset,
// while each of its extra EMPTY points cost almost nothing.  MEASURED in one run:
// bpm = 130, 180, 86, 130, 129, 180, 129, 64.  Setting the miss cost equal to the
// hit reward makes the score 8 x (hits - misses), so a grid can only score
// positively if it explains the onsets WITHOUT leaving grid points unexplained -
// which is exactly how the octave ambiguity is meant to be resolved.
static const int32_t  AGENT_LOSS    = 8;        // score decay per grid point missed
static const int32_t  AGENT_MAX     = 4000;     // score clamp

// The BPM range is mandated: "BPM range: 60 to 180 BPM".  Agent periods are
// clamped into it, because seeding the half/double hypotheses without clamping
// produced agents at 30 and 40 BPM - outside the range the document allows.
static const uint32_t AGENT_PERIOD_MIN = (uint32_t)((60ULL * TARGET_FS) / BPM_MAX);  // 180 BPM
static const uint32_t AGENT_PERIOD_MAX = (uint32_t)((60ULL * TARGET_FS) / BPM_MIN);  // 60 BPM

// --- Stage 4: phase alignment and the modulo-4 bar counter ------------------
// Audio.md: "Phase Synchronization: Align the internal metronome clock phase
// precisely with the strongest transient peaks, designating the highest-energy
// transient following structural shifts or heavy low-frequency hits as Beat 1
// (the downbeat)" and "Modulo 4 Bar Counter: Run an internal sample-accurate or
// clock-accurate modulo counter synchronized to the tracked BPM clock ...
// Update global state variables: current_beat = 1, 2, 3, 4, is_downbeat".
//
// Supplemental section 2: "the internal tracking clock must use a DAMPED PLL to
// prevent abrupt micro-steps that instantly throw off the 1-2-3-4 visual
// counter".  Supplemental section 3: "Track long-term energy changes (e.g. ...
// structural drop-outs ...).  Use these macro-structural markers to force-align
// the modulo-4 counter so that Bar 1 lines up with the true musical phrase
// boundary rather than an arbitrary starting point."
// DRIFT CORRECTED.  Audio.md section 2: "the internal tracking clock must use a
// DAMPED PLL to prevent abrupt micro-steps that instantly throw off the 1-2-3-4
// visual counter".  At 2 the grid moved HALF the way to every onset, which is not
// a damped PLL - and with ~49% of validated onsets spurious (measured IOI
// histogram: 20 of 55 at 423-482 ms, 27 of 55 spurious) a single bad onset yanked
// the grid off the music.  MEASURED: ad, agent 0's mean absolute distance from
// every onset, swung between 5 ms and 155 ms second by second, and agent 0's score
// fell from 139 back to 0 with it.  8 = 12.5% per onset: still fast enough to
// track a micro-tempo shift, slow enough that one spurious onset cannot move the
// grid a whole beat.
static const int32_t  PLL_DAMP        = 8;     // damped phase pull: error / 8
// MEASURED: with PERIOD_DAMP=16 the lock decayed before the period could
// converge (agent a0 at 130 BPM fell 34 -> 14 -> 0), because the period error
// kept the phase drifting out of tolerance faster than it was corrected.
static const int32_t  PERIOD_DAMP     = 8;     // faster period adaptation
// Three timescales, because two are not enough.  MEASURED with a 43 ms "short"
// term the short/long ratio swung 187-313% on EVERY BEAT, so the structural
// detector fired 23 times in 15 seconds and would have re-anchored Beat 1
// continuously - destroying the 1-2-3-4 count it exists to protect.
static const uint32_t FAST_AVG_SHIFT  = 10;    // ~43 ms   envelope
static const uint32_t SHORT_AVG_SHIFT = 14;    // ~0.7 s   recent level
static const uint32_t LONG_AVG_SHIFT  = 18;    // ~11 s    long-term level
// The long-term average is held SCALED (longAcc = longEnv * 2^LONG_Q).  MEASURED
// defect, the same class as the DC tracker: "longEnv += (fastEnv-longEnv) >> 18"
// with fastEnv ~250,000 gives an increment of ZERO, so the long-term average
// never moved, short/long read ~250% permanently, the anchor re-anchored Beat 1
// ~0.5 times a second, and the 1-2-3-4 count was destroyed.
static const uint32_t LONG_Q          = 10;
static const uint32_t STRUCT_DROP_PCT = 50;    // short < 50% of long = drop-out
static const uint32_t STRUCT_RISE_PCT = 180;   // short > 180% of long = a rise
static const uint32_t STRUCT_HOLD_MS  = 1500;  // re-arm time for the anchor
static const uint32_t STRUCT_HOLD_SAMPLES =
    (uint32_t)((uint64_t)STRUCT_HOLD_MS * TARGET_FS / 1000ULL);

// Audio.md Stage 4: the structural markers are the phrase change markers
// "occurring every 16 or 32 beats".  That periodicity is the ONLY thing that
// separates the real downbeat from an arbitrary quarter note, so it is what
// qualifies an event as a phrase boundary.  MEASURED on the 130 BPM test track:
// the per-beat low-band energies were near-equal and their maximum alternated
// between Beat 1 and Beat 3 (e.g. 19936/15332/16647/16203, then
// 19936/15924/20270/16203) - a TWO-beat pattern, which by definition cannot
// choose between two beats a half-bar apart.  Audio.md states this limit itself:
// "Pure audio DSP cannot mathematically distinguish Beat 1 ... from Beat 3 in a
// standard 4/4 symmetrical loop using audio frequency data alone."
static const uint32_t PHRASE_TOL_BEATS = 2;    // +/- 2 beats on a 16/32 grid

static inline bool isPhraseGap(uint32_t g) {
  const uint32_t t = PHRASE_TOL_BEATS;
  return (g >= 16 - t && g <= 16 + t) || (g >= 32 - t && g <= 32 + t) ||
         (g >= 48 - t && g <= 48 + t);
}

// Stage 4: "designating the highest-energy transient following structural shifts
// or heavy low-frequency hits as Beat 1 (the downbeat)."
// The low-band novelty peak is captured for each of the four beats of a bar; when
// the bar completes, if one beat clearly dominates the runner-up the counter is
// HARD-RESET so that beat is Beat 1.  If no beat dominates, the phase is left
// alone, because re-anchoring on near-equal energies would thrash.
static const uint32_t DOWNBEAT_MARGIN_PCT = 120;   // winner >= 120% of runner-up
// The rule must not judge a single bar.  MEASURED over a 40-bar music window: the
// single-bar winner landed on Beat 1 fourteen times, Beat 2 three times, Beat 3
// eight times and Beat 4 twelve times - i.e. on nothing - and it cleared the 120%
// margin on 16 of those 40 bars purely by chance.  Each crossing moved the phase,
// which is the jumping that was reported.  The four beats' energies differ by only
// ~14% on average (mean winner/runner-up 113.8%), so one bar cannot resolve them;
// the MEANS over many bars can, because they average that noise down by sqrt(N).
// 8 bars is 32 beats, the document's other phrase unit (~15 s at 130 BPM).  The
// metronome flashes from the very first beat regardless - only "which beat is 1"
// waits for this much evidence.
// The window over which the rule judges, counted from the most recent STRUCTURAL
// marker - not from the start of the track.  DRIFT CORRECTED: this was 8 bars over
// an accumulator that never reset, so the estimate averaged the whole track and the
// start-of-track transient dominated it and then decayed out of it.  MEASURED: Er
// fell 745% -> 248% while db climbed 0 -> 11, i.e. the phase wandered purely because
// the average was sliding.  Audio.md section 4 asks for the highest-energy transient
// "following structural shifts or heavy low-frequency hits", so the accumulator is
// now reset at each structural marker and judged after only 2 bars - the transient
// that follows the shift, which is what the document describes.
static const uint32_t DOWNBEAT_MIN_BARS = 2;

// --- Dual-timescale self-normalisation (Audio.md supplemental section 4) ----
// "Maintain a short-term moving average (~50ms) for immediate transient
//  comparison.  Maintain a long-term moving average (~5 to 10 seconds) to gauge
//  overall track volume.  Normalize incoming energy relative to the long-term
//  average so the algorithm behaves identically during a quiet intro and a
//  wall-of-sound drop without manual volume adjustment."
// The two averages already exist for the downbeat anchor: FAST_AVG (~43 ms, the
// document's ~50 ms short term) and LONG_AVG (~11 s, the document's 5-10 s).
static const int32_t NORM_REF_E    = (int32_t)40 << DC_Q;   // reference level, Q12
static const int32_t NORM_MIN_E    = (int32_t)8  << DC_Q;   // normalise above this
static const int32_t NORM_GAIN_MIN = 1024;    // 0.25x, Q12
static const int32_t NORM_GAIN_MAX = 16384;   // 4.00x, Q12

// --- MAX9814 AGC compensation (Audio.md section 1) -------------------------
// "the DSP pipeline must incorporate an inverse envelope tracker or dynamic
//  expansion filter to prevent the AGC from flattening the transient novelty
//  curve required for onset detection."
// Implemented as a dynamic EXPANDER: the gain rises when the fast (~43 ms)
// envelope exceeds the ~0.7 s reference, which re-expands the transients the AGC
// compressed.  It works on a FAST timescale while the normaliser works on the
// LONG one, so the two are complementary rather than cancelling each other.
static const int32_t AGC_K        = 2;        // expansion strength
static const int32_t AGC_GAIN_MAX = 24576;    // 6.00x, Q12

// Predictive filtering must hold phase through a breakdown, but it must not run
// for ever.  MEASURED: without a timeout, a spurious seed during boot left the
// board emitting 93 BPM beats - and flashing IO48 and all four channels - into
// total silence.  With no input at all there is no tempo to hold.
static const uint32_t BEAT_TIMEOUT_MS = 3000;
static const uint32_t BEAT_TIMEOUT_SAMPLES =
    (uint32_t)((uint64_t)BEAT_TIMEOUT_MS * TARGET_FS / 1000ULL);
// A seed needs real evidence, not chance: 2 onsets on a grid out of 4 available
// is meaningless, and that is exactly what produced the false lock above.
// The seed test must be scale-free and must count the SAME window the comb sees.
// MEASURED bug: SEED_MIN_ONSETS was compared against a ONE-SECOND count (~2
// onsets) while the comb looks at FOUR seconds (~8), so the seed never fired and
// the board never flashed a single LED.  A capture FRACTION plus a minimum
// sample size fixes that and the original chance seed (2 of 4) together.
static const uint32_t SEED_MIN_ONSETS  = 6;    // onsets in the comb's 4 s window
static const uint32_t SEED_CAPTURE_PCT = 30;   // ... and the % it must capture
// A re-seed must clear a higher bar than a first seed, or the pool churns: with
// one bar, the tempo jumped 63 -> 60 -> 126 -> 60 -> 130 -> 180 -> 65 -> 162 ->
// 81 as each 3-second re-seed took a different noisy comb peak.
static const uint32_t RESEED_CAPTURE_PCT = 55; // re-seed needs real evidence
static const uint32_t RESEED_HOLD_SEC    = 6;  // ... and not more often than this

// --- Stage 3 state ----------------------------------------------------------
// One tempo/phase hypothesis.  The comb bank seeds these; the PLL tracks them.
struct TempoAgent {
  uint32_t period;      // samples between predicted beats
  int64_t  nextBeat;    // sample index of the next predicted beat (predictive
                        // filtering: it keeps predicting when no onset arrives)
  int32_t  score;       // decaying hit score
  uint32_t hits;        // onsets that landed on this agent's grid
  uint32_t beats;       // predicted beats emitted since boot
  int64_t  lastHitAt;   // sample index of the previous hit, for period adaption
  uint16_t bpm;         // period expressed as BPM, for the log

  void setPeriod(uint32_t p) {
    if (p < AGENT_PERIOD_MIN) p = AGENT_PERIOD_MIN;   // clamp into 60-180 BPM
    if (p > AGENT_PERIOD_MAX) p = AGENT_PERIOD_MAX;
    period = p;
    uint64_t b = (60ULL * TARGET_FS) / (uint64_t)p;
    bpm = (uint16_t)((b > 65535ULL) ? 65535ULL : b);
  }
};

// REMOVED at your instruction, and recorded here so the history is not lost:
// my envelope follower, my decaying-peak threshold term, my mains notches, my
// 1-5 kHz click band with its fusion gate, and my parallel threshold sweep.
// Audio.md's mandated three-band fusion (40-150 / 500-2000 / 5000-10000), its
// dual-timescale self-normaliser (50 ms + 5-10 s) and its 3-5 agent tempo pool
// are NOT built yet, and are deliberately NOT pre-empted here.

static const BaseType_t CORE_DSP  = 0;      // DSP owns Core 0
static const uint32_t   DSP_PRIO  = 20;     // above the loop task (prio 1)
static const uint32_t   DSP_STACK = 4096;

// ---------------------------------------------------------------------------
//  SHARED STATE  -  one writer (DSP task), one reader (loop on Core 1).
//  Every field is a 32-bit aligned scalar, so reads and writes are atomic on
//  the ESP32-S3 without a lock.  "seconds" is written LAST, so the reader
//  always sees a complete set of measurements for the window it announces.
// ---------------------------------------------------------------------------
struct AudioStats {
  volatile uint32_t seconds;      // completed measurement windows
  volatile uint32_t rateHz;       // MEASURED sample rate
  volatile uint32_t totalSamples; // samples captured since boot
  volatile uint32_t timeouts;     // adc_continuous_read timed out
  volatile uint32_t errors;       // any other driver error
  volatile uint32_t recoveries;   // driver restarts after a stall
  volatile int32_t  dc;           // real-time DC estimate, counts
  volatile int32_t  dcExact;      // exact window mean of raw: the cross-check
                                  // that proves the real-time tracker is right
  volatile int32_t  acMin;        // most negative AC sample, counts
  volatile int32_t  acMax;        // most positive AC sample, counts
  volatile int32_t  acPp;         // peak-to-peak, counts
  volatile int32_t  acRms;        // RMS of the AC signal, counts
  volatile int32_t  acMean;       // mean of AC, counts: ~0 proves DC removal
  volatile uint32_t negPct;       // % of samples below DC: ~50 proves it too
  volatile int32_t  acMaxPp;      // largest peak-to-peak since boot
  volatile uint32_t actPct;       // % of samples with |AC| >= ACT_COUNT
  volatile int32_t  noiseRms;     // calibration result: silent RMS
  volatile int32_t  noisePp;      // calibration result: silent peak-to-peak
  volatile int32_t  pr50;         // ripple probe amplitudes, in counts
  volatile int32_t  pr90;         //   90 Hz is the control frequency
  volatile int32_t  pr100;
  volatile int32_t  pr120;
  volatile uint32_t onsets;       // onsets fired since boot
  volatile uint32_t onsetSeq;     // +1 AT the onset; the loop flashes on this
  volatile int32_t  bandRmsMc;    // in-band (40-150 Hz) RMS, milli-counts
  // Stage 2 detector readout, all in MILLI-COUNTS (1000 mc = 1 ADC count)
  // because the in-band signal and the novelty are both sub-count.
  volatile uint32_t fireWin;      // onsets fired in the last window
  volatile uint32_t refrWin;      // samples above threshold but in the lockout
  volatile int32_t  novPkMc;      // peak novelty in the window
  volatile int32_t  avgMc;        // the rolling local average
  volatile int32_t  thrMc;        // effective threshold = max(avg*margin, floor)
  volatile int32_t  floorMc;      // calibrated baseline floor
  volatile int32_t  sigmaMc;      // sigma of the silent novelty, measured during
                                  // the boot calibration (see the note there)
  // Multi-band fusion readout (Audio.md supplemental section 1).  lowCand is
  // the low-band Stage 2 candidate count, lowValid the ones a mid/high accent
  // validated, lowRej the ones it rejected.  Only validated onsets drive IO48.
  volatile uint32_t lowCand;      // low-band candidates this window
  volatile uint32_t lowValid;     // validated by a mid/high accent
  volatile uint32_t lowRej;       // rejected by the fusion
  volatile uint32_t midFire;      // mid-band accents this window
  volatile uint32_t highFire;     // high-band accents this window
  volatile int32_t  midFluxPk;    // peak mid-band flux in the window
  volatile int32_t  midThrMc;     // mid-band effective threshold
  volatile int32_t  highFluxPk;   // peak high-band flux in the window
  volatile int32_t  highThrMc;    // high-band effective threshold
  // Stage 3: tempo tracking (periodicity analysis) and the agent pool.
  volatile uint16_t bpmLocked;    // the promoted agent's BPM
  volatile uint32_t bpmConf;      // confidence, per cent of onsets on its grid
  volatile uint32_t combScore;    // best comb-filter score this second
  volatile uint32_t combBpm;      // the comb bank's own dominant BPM
  volatile uint16_t agentBpm[NAGENT];
  volatile int32_t  agentScore[NAGENT];
  volatile uint32_t agentHits[NAGENT];
  volatile uint32_t beatsPred;    // predicted beats emitted (all agents)
  // Stage 4: phase alignment and the modulo-4 bar counter.
  volatile uint32_t beatSeq;      // +1 per predicted beat; the loop flashes on it
  volatile uint8_t  currentBeat;  // 1..4
  volatile uint8_t  isDownbeat;   // 1 on beat 1
  volatile uint32_t beatsTotal;   // predicted beats since boot
  volatile uint32_t anchors;      // downbeat re-anchors from structural shifts
  volatile uint32_t structShifts; // structural shifts detected
  volatile int32_t  shortE;       // short-term band energy, for the anchor
  volatile int32_t  longE;        // long-term band energy
  volatile int32_t  normGain;     // dual-timescale normalisation gain, Q12
  volatile int32_t  agcGain;      // AGC expansion gain, Q12
  volatile uint32_t bestPeriod;   // promoted agent's period, in SAMPLES
  volatile uint32_t bestBeats;    // promoted agent's own beat count
  // Diagnostic plot stream (the document's 'D' mode).  Written per sample by the
  // DSP task, read by the loop at ~100 Hz.
  volatile int32_t  plotNov;      // instantaneous novelty, Q12
  volatile int32_t  plotThr;      // instantaneous threshold, Q12
  volatile uint8_t  plotPulse;    // 1 on the sample an onset fired, else 0
  volatile uint32_t dspStalls;    // times the DSP window counter went quiet
  // Downbeat heuristic (Stage 4): the low-band novelty peak at each of the four
  // beats of the last completed bar, so the cue can be VERIFIED not assumed.
  volatile int32_t  beatE[4];     // per-beat energies, milli-counts
  volatile int32_t  meanE[4];     // MEAN energy per bar position - what the rule
                                  // now compares; flat means = no cue exists
  volatile uint32_t dbAnchors;    // hard-resets from the highest-energy rule
  // What the highest-energy rule ACTUALLY decided on the last completed bar, so
  // the cue can be judged from data instead of from the output lights.
  volatile uint8_t  barWinner;    // which beat won that bar, 1..4
  volatile uint32_t barRatio;     // winner as a percentage of the runner-up
  // Stage 4's phrase-boundary anchor.  Audio.md requires the structural markers
  // to recur "every 16 or 32 beats"; a change that does NOT land on that grid is
  // an ordinary musical event and must not reset Bar 1.
  volatile uint32_t phraseAnchors;// re-anchors from qualified phrase boundaries
  volatile uint32_t phraseGap;    // beats between the last two phrase boundaries
  // The signed offset between each onset and the grid point it landed on, in ms,
  // for the promoted agent.  This is what decides whether the per-beat energy
  // attribution is sound: a beat's energy is the novelty peak in the interval
  // BEFORE it, so a consistently POSITIVE value (grid leads the onsets) means the
  // beat was emitted before its kick and the peak is credited to the NEXT beat.
  volatile int32_t  pllErrMs;
  // Stage 3 seed diagnostics: the comb's own inputs, so a failure to seed is
  // diagnosable from the log rather than inferred from silence.
  volatile uint32_t combOnsets;   // onsets in the comb's 4 s window
  volatile uint32_t combCapture;  // the best period's capture of them, percent
  volatile uint32_t combPh;       // the comb's chosen phase, in 5 ms bins
  // The promoted agent's HIT COUNT.  pllErrMs returns 0 both when the offset is
  // 0 ms and when there were no hits at all, which made "pd=0ms" unreadable.
  volatile uint32_t pllErrN;
  // Agent 0's mean ABSOLUTE distance from every onset, hit or miss, in ms.  If
  // this is near 0 the grid is on the onsets and something else is wrong; if it
  // is near half a beat period the grid is systematically between beats.
  volatile int32_t  a0ErrMs;
  // Where inside the beat interval the novelty peak falls, as a percentage of the
  // period.  ~0% = the peak sits just after the previous beat, so it belongs to
  // the NEXT beat and the energy attribution is one beat late; ~100% = the peak
  // sits just before this beat and the attribution is correct as written.
  volatile uint32_t peakPosPct;
  volatile int32_t  beatEPeak;    // novelty peak since the last beat, Q12
  volatile uint32_t flashes;      // beat flashes the LOOP has issued, so the
                                  // output path can be proved end to end
  // Inter-onset intervals of the onsets, in ms.  ioiN is written LAST so a
  // reader never sees a stale count alongside fresh data.
  volatile uint16_t ioiMs[MAX_IOI];
  volatile uint8_t  ioiN;
  volatile uint32_t calibrated;   // 1 once calibration has completed
};

static AudioStats g_stats;                     // zero-initialised by default

static adc_continuous_handle_t s_adc = nullptr;
static uint8_t s_dmaBuf[FRAME_BYTES];          // static: no allocation, ever

// ---------------------------------------------------------------------------
//  Integer square root (Newton).  Called once per window, so the cost is
//  irrelevant, but it keeps the pipeline free of floating point as required.
// ---------------------------------------------------------------------------
static uint32_t isqrt64(uint64_t v) {
  if (v == 0) return 0;
  uint64_t x = v;
  uint64_t y = (x + 1) >> 1;
  while (y < x) {
    x = y;
    y = (x + v / x) >> 1;
  }
  return (uint32_t)x;
}

// ---------------------------------------------------------------------------
//  DC_Q fixed point -> whole counts, rounded to nearest.  A plain shift would
//  FLOOR, which would bias every negative measurement by a whole count - and
//  with a signal only tens of counts wide that bias is not negligible.
// ---------------------------------------------------------------------------
static inline int32_t dcQToCounts(int32_t v) {
  const int32_t half = 1 << (DC_Q - 1);
  return (v >= 0) ? ((v + half) >> DC_Q) : -(((-v) + half) >> DC_Q);
}

// ---------------------------------------------------------------------------
//  MAINS-RIPPLE PROBE  (Stage A addendum, answers conflict C7 with data)
//
//  The silent floor measures ~6 counts RMS / ~66 counts peak-to-peak and is
//  IDENTICAL with a room fan running and stopped, so it is not acoustic.  The
//  question is whether it is mains ripple - and the answer decides whether a
//  different power supply is worth fitting at all.
//
//  Measured at 50 Hz (UK mains), 100 Hz (full-wave rectified) and 120 Hz,
//  against 90 Hz as a CONTROL that mains cannot produce.
//    - If 100 Hz is much larger than 90 Hz  -> it is the supply.  Swap it.
//    - If all four are similar              -> it is the microphone/AGC/ADC
//      noise floor, and no power supply will fix it.
// ---------------------------------------------------------------------------
static const uint8_t  NPROBE = 4;
static const uint32_t PROBE_HZ[NPROBE] = { 50, 90, 100, 120 };

struct Goertzel {
  int32_t coeff;        // Q15: 2*cos(2*pi*f/Fs)
  int64_t s1, s2;

  void init(uint32_t f, uint32_t fs) {
    coeff = (int32_t)(2.0f * cosf(2.0f * PI * (float)f / (float)fs) * 32768.0f);
    s1 = 0;
    s2 = 0;
  }
  void reset() { s1 = 0; s2 = 0; }

  // x is in WHOLE COUNTS, so the returned amplitude is in whole counts too.
  inline void push(int32_t x) {
    int64_t s = (int64_t)x + ((coeff * s1) >> 15) - s2;
    s2 = s1;
    s1 = s;
  }
  // Raw |X| for the frame just accumulated, unscaled.  The caller scales it.
  // Used by the multi-band fusion to sum several bins into one band energy.
  int64_t magnitude() const {
    int64_t p = s1 * s1 + s2 * s2 - ((coeff * s1 * s2) >> 15);
    return (p > 0) ? (int64_t)isqrt64((uint64_t)p) : 0;
  }

  // Amplitude in MILLI-COUNTS.  Whole counts are useless here: with a ~7-count
  // RMS noise floor over 24000 samples, a single-frequency component is well
  // under 1 count, and whole-count output measured 0/0/0/0 - which cannot be
  // told apart from a dead probe.  At this scaling a WORKING probe must read
  // ~90 mc on this noise floor, so exactly 0 means the probe itself is broken.
  int32_t amplitudeMilli(uint32_t n) const {
    int64_t p = s1 * s1 + s2 * s2 - ((coeff * s1 * s2) >> 15);
    if (p <= 0) return 0;
    return (int32_t)((2000 * (int64_t)isqrt64((uint64_t)p)) / (int64_t)n);
  }
};

// ---------------------------------------------------------------------------
//  Q12 (1 count = 4096) -> milli-counts.  Stage B lives on sub-count values:
//  the in-band noise floor is a fraction of one count, so whole counts read 0
//  and cannot be told apart from a dead filter.
// ---------------------------------------------------------------------------
static inline int32_t q12ToMilli(int64_t v) {
  return (int32_t)((v * 1000) / 4096);
}

// ---------------------------------------------------------------------------
//  STAGE B : SUB-BAND ISOLATION  (brief Stage 1)
//
//  Chain:  HPF 40 Hz -> LPF 150 Hz
//
//  My 50 Hz and 100 Hz notches were REMOVED here at the user's instruction:
//  they are not in Audio.md.
//
//  Coefficients are Q27 and that is REQUIRED, not a luxury.  The 40 Hz
//  low-pass has b0 = 2.7e-5, which quantises to ZERO in Q14 or Q15 - the
//  filter would silently pass nothing and look like "no signal".  At Q27 the
//  same coefficient is 3651, with room to spare.
//
//  Signal stays in Q12; state and products are int64 so a loud transient
//  cannot wrap them.
// ---------------------------------------------------------------------------
static const int32_t COEF_Q = 27;

enum BqType { BQ_LP, BQ_HP, BQ_NOTCH };

struct Biquad {
  int32_t b0, b1, b2, a1, a2;   // Q27, RBJ convention (a1 and a2 subtracted)
  int64_t z1, z2;               // transposed direct form II state, Q12

  void reset() { z1 = 0; z2 = 0; }

  inline int32_t process(int32_t x) {
    int64_t y = (((int64_t)b0 * x) >> COEF_Q) + z1;
    z1 = (((int64_t)b1 * x) >> COEF_Q) - (((int64_t)a1 * y) >> COEF_Q) + z2;
    z2 = (((int64_t)b2 * x) >> COEF_Q) - (((int64_t)a2 * y) >> COEF_Q);
    return (int32_t)y;
  }

  // RBJ cookbook design, quantised to Q27.  This is a MEMBER function on
  // purpose: the Arduino preprocessor inserts its generated prototypes ABOVE
  // the struct definitions, so a free function taking Biquad& or BqType
  // cannot compile - it reports "'Biquad' was not declared in this scope".
  void design(BqType type, float f0, float Q, float fs) {
    const float w0    = 2.0f * PI * f0 / fs;
    const float cw    = cosf(w0);
    const float alpha = sinf(w0) / (2.0f * Q);
    const float a0    = 1.0f + alpha;

    float nb0, nb1, nb2;
    if (type == BQ_LP) {
      nb0 = (1.0f - cw) * 0.5f;  nb1 = 1.0f - cw;      nb2 = nb0;
    } else if (type == BQ_HP) {
      nb0 = (1.0f + cw) * 0.5f;  nb1 = -(1.0f + cw);   nb2 = nb0;
    } else {
      nb0 = 1.0f;                nb1 = -2.0f * cw;     nb2 = 1.0f;
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
//  One onset detector: adaptive threshold on ONE novelty variant.
//  run() is a MEMBER for the same reason as Biquad::design - the Arduino
//  preprocessor would otherwise place a generated prototype above this struct.
// ---------------------------------------------------------------------------
struct OnsetDet {
  int64_t  avgAcc;        // scaled moving average (avg * 2^avgShift)
  int32_t  novPk;         // peak novelty this window
  int32_t  thr;           // effective threshold at the last tick
  int32_t  floor;         // calibrated baseline floor, Q12 (0 = not yet set)
  uint16_t avgShift;      // averaging: avg = avgAcc >> avgShift
  uint16_t marginPct;     // the tuned margin, per cent of the local average
  uint32_t refrTicks;     // refractory lockout, in TICKS (samples or frames)
  uint32_t refrLeft;      // ticks left in the lockout
  uint32_t fireWin;       // onsets fired this window
  uint32_t refrWin;       // ticks above threshold but inside the lockout
  uint32_t total;         // onsets since boot

  void reset() {
    avgAcc = 0; novPk = 0; thr = 0; floor = 0;
    refrLeft = 0; fireWin = 0; refrWin = 0; total = 0;
  }

  // Stage 2, exactly as written: the peak must exceed "the local average by a
  // mathematically tuned adaptive threshold margin", floored by the baseline
  // measured during the boot calibration.
  //
  // The average is held SCALED, for the same reason the DC tracker is: a plain
  // "avg += (nov - avg) >> SHIFT" on raw values floors every positive residual
  // to 0 and every negative one to -1, so the average walks down to the
  // MINIMUM of the novelty and the threshold ends up far too low.  That defect
  // was measured on this board, not assumed.
  //
  // The same struct serves the low band (ticks = samples) and the mid/high
  // fusion bands (ticks = 10 ms frames), hence avgShift and refrTicks are
  // per-instance.
  //
  // Returns true when THIS tick is an onset.
  inline bool run(int32_t nov, bool armed) {
    if (nov > novPk) novPk = nov;

    avgAcc += (int64_t)nov - (avgAcc >> avgShift);
    int32_t avg = (int32_t)(avgAcc >> avgShift);

    thr = (int32_t)(((int64_t)avg * (int64_t)marginPct) / 100);
    if (thr < floor) thr = floor;

    if (refrLeft) refrLeft--;

    // Not armed until the silent calibration has set the floor, otherwise the
    // warm-up transient fires immediately.
    if (armed && nov > thr) {
      if (refrLeft == 0) {
        refrLeft = refrTicks;      // refractory lockout
        fireWin++;
        total++;
        return true;
      }
      refrWin++;                   // above threshold but inside the lockout
    }
    return false;
  }
};

// ---------------------------------------------------------------------------
//  ADC SETUP  -  continuous DMA, one channel, TYPE2 output.
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
//  DSP TASK  -  Core 0.  Captures one window, measures it, publishes it,
//  repeats.  Never allocates, never touches the loop task's outputs.
// ---------------------------------------------------------------------------
static void audioTask(void *arg) {
  (void)arg;

  uint64_t calSumSq = 0;      // accumulators for the silent calibration
  uint32_t calN     = 0;
  int32_t  calPp    = 0;

  int64_t  dcAcc = ((int64_t)2048) << DC_Q;   // DC estimate, DC_Q fixed point
  uint32_t stall = 0;

  // Seed the DC tracker from the first frame so it starts CONVERGED.  Without
  // this it slews from its seed to the real bias over the first second, making
  // a ~600-count transient that poisoned the silent calibration (measured:
  // noisePp=650, noiseRms inflated 10x).  The calibration must measure the
  // room, not this tracker's own settling.
  {
    uint32_t got = 0;
    if (adc_continuous_read(s_adc, s_dmaBuf, FRAME_BYTES, &got,
                            READ_TIMEOUT_MS) == ESP_OK) {
      int64_t  sum = 0;
      uint32_t n   = 0;
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

  static Goertzel probe[NPROBE];
  for (uint8_t k = 0; k < NPROBE; k++) probe[k].init(PROBE_HZ[k], TARGET_FS);

  // --- Stage B state --------------------------------------------------------
  // Stage 1: "a digital low-pass filter (or fixed-point biquad filter) to
  // isolate the low-frequency sub-band (40 Hz to 150 Hz)".  A high-pass at 40
  // and a low-pass at 150 isolate exactly that sub-band.  The filters run
  // CONTINUOUSLY across windows; resetting them per window would inject a
  // transient every second and fake an onset.
  static Biquad fHp, fLp;
  fHp.design(BQ_HP, BP_HP_HZ, BP_Q, (float)TARGET_FS);
  fLp.design(BQ_LP, BP_LP_HZ, BP_Q, (float)TARGET_FS);

  static OnsetDet det;
  det.reset();
  det.avgShift  = NOV_AVG_SHIFT;
  det.refrTicks = REFR_SAMPLES;
  det.marginPct = NOV_MARGIN_PCT;

  int32_t  lastMag     = 0;   // |x[n-1]|, for the first-order difference
  int64_t  calNovSumSq = 0;   // sum of novelty squares, for the sigma below
  uint32_t calNovN     = 0;

  // Diagnostic only - it does not affect the algorithm.
  uint64_t absN        = 0;            // samples since boot
  uint64_t prevOnsetAt = 0;
  bool     havePrev    = false;
  uint16_t ioiLocal[MAX_IOI];
  uint8_t  ioiLocalN   = 0;

  // --- Multi-band fusion state ----------------------------------------------
  static Goertzel gMid[MID_NBINS], gHigh[HIGH_NBINS];
  for (uint8_t k = 0; k < MID_NBINS;  k++) gMid[k].init(MID_HZ[k], TARGET_FS);
  for (uint8_t k = 0; k < HIGH_NBINS; k++) gHigh[k].init(HIGH_HZ[k], TARGET_FS);

  // The fusion bands run at FRAME rate, so their refractory is in frames and
  // their averaging shift is 5 (32 frames ~ 320 ms, close to the low band's
  // ~341 ms so the two are comparable).
  static OnsetDet detMid, detHigh;
  detMid.reset();
  detMid.avgShift = 5;  detMid.refrTicks = FUS_REFR_FRAMES;
  detMid.marginPct = ACCENT_MARGIN_PCT;
  detHigh.reset();
  detHigh.avgShift = 5; detHigh.refrTicks = FUS_REFR_FRAMES;
  detHigh.marginPct = ACCENT_MARGIN_PCT;

  uint32_t frameN         = 0;   // samples into the current frame
  int64_t  midEnergyPrev  = 0;   // previous frame's band energies
  int64_t  highEnergyPrev = 0;
  int32_t  midFluxPk      = 0;   // this window's peak flux per band
  int32_t  highFluxPk     = 0;
  uint32_t lastAccentAt   = 0;   // sample index of the last mid/high accent
  uint32_t lowCandWin = 0, lowValidWin = 0, lowRejWin = 0;
  int32_t  midCalFluxPk = 0, highCalFluxPk = 0;   // boot calibration
  int64_t  midCalSumSq = 0, highCalSumSq = 0;
  uint32_t midCalN = 0, highCalN = 0;
  // Low-band candidates held for fusion.  A low transient is validated when a
  // mid/high accent lands within COINC_SAMPLES of it, EITHER side: "coincides
  // with or closely precedes".
  uint32_t pendAt[MAX_PENDING];
  uint8_t  pendN = 0;

  // --- Stage 3 state --------------------------------------------------------
  static uint16_t tBin[T_BINS];        // onset train, 5 ms bins, 4 s of history
  uint32_t tBinIdx = 0;                // newest bin
  uint32_t tBinCnt = 0;                // samples into the current bin
  static TempoAgent ag[NAGENT];
  bool     agSeeded = false;
  uint8_t  bestAg   = 0;
  uint32_t onsetWin = 0;               // onsets since the last comb update
  uint32_t combTick = 0;               // samples since the last comb update
  uint32_t lastSeedSec = 0;            // last agent-pool re-seed, in seconds

  // --- Stage 4 state --------------------------------------------------------
  // The beat index runs forever; the LABEL is derived from it and a phase
  // offset, so the bar boundary can be shifted by any number of beats without
  // the counter ever stalling.  (An earlier version set the counter itself,
  // which could re-enter the bar-complete block and lock it on beat 4 for ever.)
  uint32_t beatIdx      = 0;     // running beat index, never reset
  uint32_t phaseOff     = 0;     // label of beat i = ((i - phaseOff) & 3) + 1
  int32_t  fastEnv      = 0;     // ~43 ms  envelope
  int64_t  shortEnv     = 0;     // ~0.7 s  recent level
  int64_t  longEnv      = 0;     // ~11 s   long-term level
  int64_t  longAcc      = 0;     // the long-term average, scaled by 2^LONG_Q
  int32_t  gNorm        = 4096;  // dual-timescale normalisation gain, Q12
  int32_t  gAgc         = 4096;  // AGC expansion gain, Q12
  bool     anchorArmed  = false; // force the next beat to be Beat 1
  uint32_t lastStructAt = 0;     // sample index of the last structural shift
  uint32_t anchorCnt    = 0;
  uint32_t structCnt    = 0;
  uint32_t lastOnsetAt  = 0;     // sample index of the last validated onset
  int32_t  novPeakSinceBeat = 0; // low-band novelty peak since the last beat
  int32_t  beatE[4]     = { 0, 0, 0, 0 };  // per-beat energies, current bar
  uint32_t dbAnchorCnt  = 0;     // hard-resets from the highest-energy rule
  // Phrase-boundary anchor state (Audio.md: markers "occurring every 16 or 32
  // beats").  lastStructBeat holds the last QUALIFIED boundary, not the last
  // event, so an ordinary event between two phrase boundaries cannot destroy the
  // phrase grid.
  bool     structSeen   = false; // first structural event just sets the origin
  uint32_t lastStructBeat = 0;   // beat index of the last qualified boundary
  uint32_t phraseCnt    = 0;
  uint32_t phraseGap    = 0;     // beats between the last two qualified boundaries
  // The last COMPLETED bar's energies in label order, plus what the rule decided
  // about it.  Logging the in-progress bar instead made the values look frozen.
  int32_t  lastBarE[4]  = { 0, 0, 0, 0 };
  uint8_t  lastBarWin   = 0;
  uint32_t lastBarRatio = 0;
  // Per-bar-POSITION energy accumulated over every bar so far.  Indexed by
  // (beatIdx & 3), which is phase-independent, so a phase change does not
  // corrupt the accumulator and its argmax directly names the downbeat position.
  int64_t  eSum[4]      = { 0, 0, 0, 0 };
  uint32_t eBars        = 0;                       // bars accumulated
  int32_t  lastMeanE[4] = { 0, 0, 0, 0 };          // means per label, for the log
  // pp= diagnostic: where inside each beat interval the novelty peak falls, as a
  // fraction of the period (0 = at the interval start, 255 = at the beat).  A
  // beat's energy is the peak in the interval BEFORE it, so a peak near the START
  // means the beat was emitted before its own kick and the peak belongs to the
  // NEXT beat - a systematic one-beat offset.  Near the END means the attribution
  // is correct as written.  Measured, not assumed.
  uint32_t peakAtN      = 0;     // sample index of the current novelty peak
  uint32_t lastBeatAtN  = 0;     // sample index of the last beat emission
  uint64_t peakPosAcc   = 0;     // sum of peak positions, 0..255 fractions
  uint32_t peakPosN     = 0;

  for (;;) {
    int64_t  winStart = esp_timer_get_time();
    uint32_t winN     = 0;
    int32_t  mnQ      = 0;    // AC extremes, DC_Q fixed point, zero-centred
    int32_t  mxQ      = 0;
    int64_t  sumQ     = 0;
    int64_t  sumRaw   = 0;
    int64_t  sumSq    = 0;
    uint32_t over     = 0;
    uint32_t neg      = 0;

    int64_t  bandSumSq = 0;

    // Per-agent onset-to-grid offsets for this window, for the pd= diagnostic.
    int64_t  agErrSum[NAGENT] = { 0, 0, 0, 0 };
    uint32_t agErrN[NAGENT]   = { 0, 0, 0, 0 };
    // Agent 0's mean ABSOLUTE distance from every onset, hit or miss, for ad=.
    int64_t  a0AbsErrAcc = 0;
    uint32_t a0AbsErrN   = 0;

    ioiLocalN = 0;

    // Audio.md requires a MANDATORY 5-second silent calibration at boot, and
    // requires it to establish a noise baseline rather than a hardcoded
    // threshold.  calPhase is true for those five windows.
    const bool calPhase = (g_stats.calibrated == 0);

    for (uint8_t k = 0; k < NPROBE; k++) probe[k].reset();

    while (winN < WINDOW_SAMPLES) {
      uint32_t  got = 0;
      esp_err_t r = adc_continuous_read(s_adc, s_dmaBuf, FRAME_BYTES, &got,
                                        READ_TIMEOUT_MS);

      if (r == ESP_ERR_TIMEOUT) {          // driver gone quiet
        g_stats.timeouts = g_stats.timeouts + 1;
        if (++stall >= STALL_LIMIT) {      // restart it from this task: safe,
          adc_continuous_stop(s_adc);      // no cross-task driver call
          adc_continuous_start(s_adc);
          g_stats.recoveries = g_stats.recoveries + 1;
          stall = 0;
        }
        continue;
      }
      if (r != ESP_OK) {                   // driver outran us: drop and go on
        g_stats.errors = g_stats.errors + 1;
        adc_continuous_flush_pool(s_adc);
        vTaskDelay(1);
        continue;
      }
      stall = 0;

      for (uint32_t i = 0; i + CONV_BYTES <= got; i += CONV_BYTES) {
        adc_digi_output_data_t d;
        memcpy(&d.val, &s_dmaBuf[i], sizeof(uint32_t));
        if (d.type2.channel != (uint32_t)ADC_CHAN) continue;   // not our pin

        int32_t raw = (int32_t)d.type2.data;
        // Fractional single-pole DC tracker (~0.93 Hz high-pass).  The step
        // MUST be able to move by less than one count: with only tens of
        // counts of signal, a whole-count step can only ever move DOWN, so it
        // converges to the signal MINIMUM instead of its mean.  Measured on
        // this board, not assumed.
        dcAcc += (((int64_t)raw << DC_Q) - dcAcc) >> DC_SHIFT;
        int32_t acQ = (int32_t)(((int64_t)raw << DC_Q) - dcAcc);   // DC_Q units

        if (acQ < mnQ) mnQ = acQ;
        if (acQ > mxQ) mxQ = acQ;
        if (acQ < 0) neg++;
        sumQ   += acQ;
        sumRaw += raw;
        sumSq  += (int64_t)acQ * (int64_t)acQ;

        int32_t acC = acQ >> DC_Q;            // whole counts, for the probe
        for (uint8_t k = 0; k < NPROBE; k++) probe[k].push(acC);

        // ---- Stage B: isolate the kick band, then find its attacks ---------
        int32_t b = acQ;
        b = fHp.process(b);                  // 40 Hz high-pass
        b = fLp.process(b);                  // 150 Hz low-pass
        bandSumSq += (int64_t)b * (int64_t)b;

        int32_t magRaw = (b < 0) ? -b : b;   // full-wave rectification

        // The Stage 4 downbeat anchor AND the dual-timescale normaliser share
        // these envelopes.  They track the UN-NORMALISED level deliberately: if
        // they tracked the normalised signal the gain would be self-referential.
        //   fastEnv  ~43 ms = the document's "short-term (~50ms)"
        //   shortEnv ~0.7 s = the AGC expansion reference
        //   longEnv  ~11 s  = the document's "long-term (5 to 10 seconds)"
        fastEnv  += (magRaw - fastEnv)  >> FAST_AVG_SHIFT;
        shortEnv += (fastEnv - shortEnv) >> SHORT_AVG_SHIFT;
        longAcc  += (((int64_t)fastEnv << LONG_Q) - longAcc) >> LONG_AVG_SHIFT;
        longEnv   = longAcc >> LONG_Q;

        // The document's structural markers are "structural drop-outs, vocal
        // entries, ... sudden spectral centroid shifts" - a drop-out or a rise
        // relative to the long-term level is what is detectable from energy
        // alone, and it is what re-aligns Bar 1 to the phrase boundary.
        if (longEnv > NORM_MIN_E) {                    // only when there is level
          const int32_t sPct = (int32_t)(((int64_t)shortEnv * 100) / longEnv);
          if ((sPct < (int32_t)STRUCT_DROP_PCT || sPct > (int32_t)STRUCT_RISE_PCT) &&
              (((uint32_t)absN - lastStructAt) > STRUCT_HOLD_SAMPLES)) {
            lastStructAt = (uint32_t)absN;
            structCnt++;
            // Only a change landing on the document's 16/32-beat phrase grid is a
            // phrase boundary.  MEASURED: the bare threshold fired 22 times in one
            // track - roughly every 4 beats - which is ordinary musical activity,
            // not a phrase change, and re-anchoring on those resets Bar 1
            // constantly.  A non-qualifying event is recorded but does NOT move
            // the phase, so the anchor fails SAFE: if this track has no detectable
            // phrase period, Beat 1 stops being re-anchored rather than being
            // re-anchored wrongly.
            if (!structSeen) {
              structSeen     = true;        // first event sets the phrase origin
              lastStructBeat = beatIdx;
            } else {
              const uint32_t gap = beatIdx - lastStructBeat;
              if (isPhraseGap(gap)) {
                lastStructBeat = beatIdx;
                phraseGap      = gap;
                phraseCnt++;
                anchorArmed    = true;      // the next beat becomes Beat 1
              }
            }
          }
        }

        // ---- MAX9814 AGC compensation: dynamic expansion -------------------
        // Gain rises when the fast envelope exceeds the ~0.7 s reference, which
        // re-expands the transients the AGC compressed.  FAST timescale.
        gAgc = 4096;
        if (shortEnv > NORM_MIN_E) {
          const int32_t ex = (int32_t)(fastEnv - shortEnv);
          if (ex > 0) {
            gAgc = 4096 + (int32_t)((((int64_t)ex * AGC_K) << 12) / shortEnv);
            if (gAgc > AGC_GAIN_MAX) gAgc = AGC_GAIN_MAX;
          }
        }

        // ---- Dual-timescale self-normalisation ----------------------------
        // gNorm is recomputed once per window from the LONG-term level, so it
        // equalises the track's overall level and cannot chase a transient.
        // Both gains are applied BEFORE rectification, which is what makes the
        // novelty level-independent - the document's stated goal: "the algorithm
        // behaves identically during a quiet intro and a wall-of-sound drop".
        int32_t bDet = (int32_t)(((int64_t)b * gAgc) >> 12);
        bDet = (int32_t)(((int64_t)bDet * gNorm) >> 12);
        int32_t mag = (bDet < 0) ? -bDet : bDet;

        // Stage 1 low band, exactly as written: "full-wave rectification and a
        // first-order difference operation (dx[n] = |x[n]| - |x[n-1]|) to
        // generate a novelty curve that highlights sharp transient attacks".
        // The negative half is discarded, because a decay is not an onset.
        int32_t nov = mag - lastMag;
        lastMag = mag;
        if (nov < 0) nov = 0;

        // Peak novelty since the last beat: this is the "energy" of the beat we
        // are about to reach, and it is what the Stage 4 downbeat rule compares.
        if (nov > novPeakSinceBeat) {
          novPeakSinceBeat = nov;
          peakAtN = (uint32_t)absN;    // WHERE in the interval the peak sits
        }

        if (calPhase) {                      // boot calibration (low band)
          calNovSumSq += (int64_t)nov * (int64_t)nov;
          calNovN++;
        }

        // ---- Multi-band fusion: accumulate the Goertzel bins ----------------
        for (uint8_t k = 0; k < MID_NBINS;  k++) gMid[k].push(acQ);
        for (uint8_t k = 0; k < HIGH_NBINS; k++) gHigh[k].push(acQ);

        const bool armed = (g_stats.calibrated != 0);

        // Low-band Stage 2 candidate.  It is NOT an onset yet - the fusion
        // decides whether it is valid.
        const bool cand = det.run(nov, armed);
        if (cand) lowCandWin++;

        // Diagnostic plot stream (the document's 'D' mode): the novelty, the
        // adaptive threshold curve and the onset pulse, published every sample.
        g_stats.plotNov   = nov;
        g_stats.plotThr   = det.thr;
        g_stats.plotPulse = cand ? 1 : 0;

        bool onsetNow = false;
        if (++frameN >= FUS_FRAME_SAMPLES) {
          frameN = 0;

          // Band energies: the sum of that band's bin magnitudes.
          int64_t midE = 0, highE = 0;
          for (uint8_t k = 0; k < MID_NBINS; k++) {
            midE += gMid[k].magnitude() >> BIN_SHIFT;
            gMid[k].reset();
          }
          for (uint8_t k = 0; k < HIGH_NBINS; k++) {
            highE += gHigh[k].magnitude() >> BIN_SHIFT;
            gHigh[k].reset();
          }

          // Spectral flux: the POSITIVE frame-to-frame energy difference.
          int32_t midFlux  = (int32_t)(midE  - midEnergyPrev);
          int32_t highFlux = (int32_t)(highE - highEnergyPrev);
          midEnergyPrev  = midE;
          highEnergyPrev = highE;
          if (midFlux  < 0) midFlux  = 0;
          if (highFlux < 0) highFlux = 0;
          if (midFlux  > midFluxPk)  midFluxPk  = midFlux;
          if (highFlux > highFluxPk) highFluxPk = highFlux;

          if (calPhase) {                  // boot calibration for the floors
            midCalSumSq  += (int64_t)midFlux  * midFlux;   midCalN++;
            highCalSumSq += (int64_t)highFlux * highFlux;  highCalN++;
          }

          const bool midAcc  = detMid.run(midFlux,  armed);
          const bool highAcc = detHigh.run(highFlux, armed);

          // A held candidate no accent validated in time is rejected.
          uint8_t k = 0;
          while (k < pendN) {
            if (((uint32_t)absN - pendAt[k]) > COINC_SAMPLES) {
              lowRejWin++;
              pendAt[k] = pendAt[--pendN];
            } else {
              k++;
            }
          }

          // An accent validates every held candidate: by construction they are
          // all within COINC_SAMPLES of now.
          if (midAcc || highAcc) {
            lastAccentAt = (uint32_t)absN;
            for (uint8_t j = 0; j < pendN; j++) {
              lowValidWin++;
              onsetNow = true;
              g_stats.onsetSeq = g_stats.onsetSeq + 1;  // the loop flashes here
              if (havePrev && ioiLocalN < MAX_IOI) {
                ioiLocal[ioiLocalN++] =
                    (uint16_t)((absN - prevOnsetAt) * 1000ULL / TARGET_FS);
              }
              prevOnsetAt = absN;
              havePrev    = true;
            }
            pendN = 0;
          }
        }

        // A candidate that coincides with (or follows) a recent accent is valid
        // at once; otherwise it is held, awaiting an accent within COINC.
        if (cand) {
          if (((uint32_t)absN - lastAccentAt) <= COINC_SAMPLES) {
            lowValidWin++;
            onsetNow = true;
            g_stats.onsetSeq = g_stats.onsetSeq + 1;    // the loop flashes here
            if (havePrev && ioiLocalN < MAX_IOI) {
              ioiLocal[ioiLocalN++] =
                  (uint16_t)((absN - prevOnsetAt) * 1000ULL / TARGET_FS);
            }
            prevOnsetAt = absN;
            havePrev    = true;
          } else if (pendN < MAX_PENDING) {
            pendAt[pendN++] = (uint32_t)absN;
          } else {
            lowRejWin++;                                // queue full
          }
        }

        // ---- Stage 3: feed the onset train and drive the agent PLLs ---------
        if (onsetNow) {
          tBin[tBinIdx]++;
          onsetWin++;
          lastOnsetAt = (uint32_t)absN;
          for (uint8_t a = 0; a < NAGENT; a++) {
            if (!ag[a].period) continue;
            const int64_t p = (int64_t)ag[a].period;
            int64_t d = ((int64_t)absN - ag[a].nextBeat) % p;   // bounded wrap
            if (d >  p / 2) d -= p;
            if (d < -p / 2) d += p;
            const int64_t tol  = (int64_t)PLL_TOL_BINS  * (int64_t)T_BIN_SAMPLES;
            const int64_t pull = (int64_t)PLL_PULL_BINS * (int64_t)T_BIN_SAMPLES;
            // Diagnostic for agent 0 - the hypothesis whose tempo the comb keeps
            // choosing.  Record how far its grid is from EVERY onset, hit or not.
            // The hit count alone cannot distinguish "aligned" from "half a beat
            // away": both can read 0 hits in a given second.  This is the number
            // that says whether a0's grid is on the onsets at all.
            if (a == 0) {
              a0AbsErrAcc += (d < 0) ? -d : d;
              a0AbsErrN++;
            }
            if (d <= tol && d >= -tol) {
              // On this agent's grid: score the hit and pull the phase.  The
              // pull is DAMPED (error/PLL_DAMP, not a full re-phase), because
              // the document requires it: "the internal tracking clock must use
              // a damped PLL to prevent abrupt micro-steps that instantly throw
              // off the 1-2-3-4 visual counter".
              ag[a].hits++;
              ag[a].score += AGENT_GAIN;
              if (ag[a].score > AGENT_MAX) ag[a].score = AGENT_MAX;
              // pd= diagnostic: how far this onset sits from the grid point it
              // landed on.  See g_stats.pllErrMs for why this matters.
              agErrSum[a] += d;
              agErrN[a]++;
              // DAMPED PHASE CORRECTION ONLY - it must NOT advance the grid.
              // The emission loop below is the ONLY place the grid advances.
              // MEASURED BUG: advancing it here as well meant every onset that
              // landed on a grid point cancelled that beat's emission, so the
              // metronome ran at HALF rate - "per=11040" (correct for 130 BPM)
              // with "pbeats" climbing at 1.09/s instead of 24000/11040 = 2.17/s.
              ag[a].nextBeat += d / PLL_DAMP;
              // Micro-tempo shift: adapt the period slowly, and only from hits
              // that were one beat apart, so a missed beat cannot double it.
              if (ag[a].lastHitAt) {
                const int64_t iv = (int64_t)absN - ag[a].lastHitAt;
                if (iv > p / 2 && iv < (p * 3) / 2) {
                  int32_t np = (int32_t)ag[a].period +
                               (int32_t)((iv - (int64_t)ag[a].period) / PERIOD_DAMP);
                  if (np < (int32_t)AGENT_PERIOD_MIN) np = (int32_t)AGENT_PERIOD_MIN;
                  if (np > (int32_t)AGENT_PERIOD_MAX) np = (int32_t)AGENT_PERIOD_MAX;
                  ag[a].setPeriod((uint32_t)np);
                }
              }
              ag[a].lastHitAt = (int64_t)absN;
            } else if (d <= pull && d >= -pull) {
              // Inside the CAPTURE RANGE but outside the lock window: pull the phase
              // (damped) but award NO score and leave lastHitAt alone, so the period
              // only ever adapts from genuine hits.  Without this a grid 40-200 ms
              // off the onsets can never be recovered - it earns no hits, so it is
              // never corrected, so it drifts further and dies.
              ag[a].nextBeat += d / PLL_DAMP;
            } else {
              ag[a].score -= AGENT_LOSS;
              if (ag[a].score < 0) ag[a].score = 0;
            }
          }
        }

        // Predictive filtering: every agent keeps emitting its grid whether or
        // not onsets arrive.  That is what "maintain phase continuity until a
        // new stable tempo emerges" requires through a breakdown.
        for (uint8_t a = 0; a < NAGENT; a++) {
          if (!ag[a].period) continue;
          if (ag[a].nextBeat <= (int64_t)absN) {
            const int64_t behind = (int64_t)absN - ag[a].nextBeat;
            ag[a].nextBeat += (int64_t)ag[a].period *
                              (behind / (int64_t)ag[a].period + 1);
            ag[a].beats++;
            // Penalise a grid point that had NO onset on it.  This term is what
            // makes the scores comparable between tempos: a denser grid has more
            // points, so it collects more penalties when the onsets are absent.
            // WITHOUT it the fastest hypothesis always wins, because +/-20 ms
            // covers 12% of a 180 BPM period but only 8.7% of a 130 BPM one.
            const int64_t gTol = (int64_t)PLL_TOL_BINS * (int64_t)T_BIN_SAMPLES;
            if (((int64_t)absN - (int64_t)lastOnsetAt) > gTol) {
              ag[a].score -= AGENT_LOSS;
              if (ag[a].score < 0) ag[a].score = 0;
            }
            if (a == bestAg) {
              // The metronome IS the promoted agent.  Stage 4's modulo-4 bar
              // counter runs from here, at sample resolution, synchronised to the
              // tracked BPM clock - and a structural shift forces the next beat
              // to be Beat 1, which is the phrase-boundary downbeat anchor.
              if (anchorArmed) {
                // Force the NEXT beat to be Beat 1 by placing the phase origin
                // on it.  The label is derived, so this cannot stall the counter.
                phaseOff    = beatIdx;
                anchorArmed = false;
                anchorCnt++;
              }
              // Record this beat's energy, keyed by its POSITION in the bar
              // (beatIdx & 3, which is phase-independent), and accumulate it.
              // The four positions ARE the evidence for the downbeat rule, so
              // they are published rather than trusted.
              const int32_t eThis = novPeakSinceBeat;
              beatE[beatIdx & 3] = eThis;
              novPeakSinceBeat = 0;
              // pp= diagnostic (see the state comment above): the fraction of the
              // beat period at which this beat's peak occurred.  Accumulated per
              // real beat, so the published mean covers beats only.
              {
                const uint32_t per = ag[bestAg].period;
                if (per > 0 && lastBeatAtN > 0 && peakAtN >= lastBeatAtN) {
                  uint32_t pos = peakAtN - lastBeatAtN;
                  if (pos > per) pos = per;
                  peakPosAcc += ((uint64_t)pos * 255ULL) / per;
                  peakPosN++;
                }
                lastBeatAtN = (uint32_t)absN;
                peakAtN     = (uint32_t)absN;
              }

              // The bar is complete once four beats have been recorded.  Stage 4:
              // "designating the highest-energy transient ... as Beat 1".
              //
              // MEASURED DEFECT this replaces: judging ONE bar at a time.  Over a
              // 40-bar music window the single-bar winner landed on Beat 1 14
              // times, Beat 2 three times, Beat 3 eight times and Beat 4 twelve
              // times - i.e. on nothing - and it cleared the 120% margin on 16 of
              // those 40 bars by chance.  Each crossing moved the phase, which is
              // the "jumping around" that was reported.  Comparing MEANS over many
              // bars averages that noise down by sqrt(N), so the same 120% margin
              // becomes genuinely strict.
              //
              // It also fails SAFE: if the track has no four-beat energy cue, no
              // position ever dominates, the rule never fires, and the phase stops
              // moving - the behaviour that was asked for.
              if ((beatIdx & 3) == 3) {
                // Accumulate each bar NORMALISED by its own peak, so a loud bar
                // cannot dominate the average.  MEASURED: the start-of-track
                // transient is ~26x a normal bar (527914 against ~20000), and a raw
                // sum let one bar dominate and then decay out of it - Er fell
                // 745% -> 248% while db climbed 0 -> 11.  Normalising removes that
                // without any reset.  (An earlier attempt that DID reset the
                // accumulator at each structural marker left the means full of
                // zeros - mE=0/0/0/36720, mE=21065/22318/0/0 - because those
                // markers fire far more often than every 2 bars.)
                int32_t barMax = 0;
                for (uint8_t j = 0; j < 4; j++) {
                  if (beatE[j] > barMax) barMax = beatE[j];
                }
                if (barMax > 0) {
                  for (uint8_t j = 0; j < 4; j++) {
                    eSum[j] += ((int64_t)beatE[j] * 1000) / barMax;
                  }
                }
                eBars++;
                int64_t mean[4];
                for (uint8_t j = 0; j < 4; j++) {
                  mean[j] = eSum[j] / (int64_t)eBars;
                }
                uint8_t k = 0;
                for (uint8_t j = 1; j < 4; j++) {
                  if (mean[j] > mean[k]) k = j;
                }
                int64_t second = 0;
                for (uint8_t j = 0; j < 4; j++) {
                  if (j != k && mean[j] > second) second = mean[j];
                }
                // Publish the last completed bar AND the accumulated means, both
                // in LABEL order, so the log reads the way the rule does.
                for (uint8_t j = 0; j < 4; j++) {
                  lastBarE[j]  = beatE[(phaseOff + j) & 3];
                  lastMeanE[j] = (int32_t)mean[(phaseOff + j) & 3];
                }
                lastBarWin   = (uint8_t)(((beatIdx - 3 + k - phaseOff) & 3) + 1);
                lastBarRatio = (second > 0)
                                   ? (uint32_t)(((int64_t)mean[k] * 100) / second)
                                   : 0;
                // Act only once there is enough evidence, and only when the MEANS
                // show a genuinely dominant beat.
                if (eBars >= DOWNBEAT_MIN_BARS && second > 0 &&
                    (int64_t)mean[k] * 100 >=
                        (int64_t)DOWNBEAT_MARGIN_PCT * second) {
                  // The beat at running index (beatIdx - 3 + k) becomes Beat 1.
                  const uint32_t newOff = beatIdx - 3 + k;
                  if (newOff != phaseOff) dbAnchorCnt++;   // count real MOVES only
                  phaseOff = newOff;
                }
              }

              // The label comes from the index and the phase, so a shift takes
              // effect immediately and the counter cannot stall.
              const uint8_t label = (uint8_t)(((beatIdx - phaseOff) & 3) + 1);
              g_stats.currentBeat = label;
              g_stats.isDownbeat  = (label == 1) ? 1 : 0;
              g_stats.beatSeq     = g_stats.beatSeq + 1;
              g_stats.beatsTotal  = g_stats.beatsTotal + 1;
              beatIdx++;
            }
          }
        }

        // Advance the onset-train bins.
        if (++tBinCnt >= T_BIN_SAMPLES) {
          tBinCnt = 0;
          if (++tBinIdx >= T_BINS) tBinIdx = 0;
          tBin[tBinIdx] = 0;               // clear the slot about to be filled
        }
        absN++;
        if (acQ >= (ACT_COUNT << DC_Q) || acQ <= -(ACT_COUNT << DC_Q)) over++;

        if (++winN >= WINDOW_SAMPLES) break;
      }
    }

    // Predictive filtering holds the phase through a breakdown, but the input
    // cannot be absent for ever.  With no onset for BEAT_TIMEOUT the pool is
    // cleared, so the metronome stops instead of flashing an empty room.
    if (agSeeded && ((uint32_t)absN - lastOnsetAt) > BEAT_TIMEOUT_SAMPLES) {
      for (uint8_t a = 0; a < NAGENT; a++) {
        ag[a].period = 0; ag[a].bpm = 0; ag[a].score = 0; ag[a].lastHitAt = 0;
      }
      agSeeded          = false;
      // No grid, so no bar phase; the next beat to be emitted starts a new bar.
      phaseOff          = beatIdx;
      // The metronome has stopped, so the running beat index no longer maps to
      // consecutive musical beats.  The per-position energy accumulator is only
      // meaningful over a CONTINUOUS beat sequence, so it is discarded here.
      // (It is deliberately NOT cleared on a re-seed: there the index is still
      // continuous, and the accumulator's argmax is what re-derives the phase.)
      for (uint8_t j = 0; j < 4; j++) eSum[j] = 0;
      eBars             = 0;
      g_stats.bpmLocked = 0;
      g_stats.bpmConf   = 0;
    }

    // ---- Stage 3: comb filter bank over 60-180 BPM -------------------------
    // Audio.md: "an autocorrelation array, a comb filter bank, or an adaptive
    // phase-locked loop (PLL) ... to evaluate temporal spacing."  For every
    // candidate period in the required BPM range, find the phase that captures
    // the most onsets; the best candidate is the dominant periodicity.
    //
    // MEASURED DEFECT this replaces: the comb summed ONE 5 ms bin per grid point,
    // while a validated onset may sit up to PLL_TOL_BINS (4 bins = 20 ms) from the
    // beat it belongs to.  Almost every onset therefore fell BETWEEN grid points.
    // The best score was 2-3 out of the ~9 onsets in the window and the chosen
    // period changed every second - "comb=2@94", "comb=3@129", "comb=2@65" in
    // consecutive seconds.  The IOI stream proves the onset train itself was
    // sound (intervals clustered at 450-496 ms = 130 BPM), so the tempo was lost
    // HERE, not in the detector.  Seeded from a random period the pool locked at
    // 65 BPM against 130 BPM music - HALF rate - and a half-tempo grid makes a
    // 4-beat bar 8 musical beats, so the bar phase could never be right.  This is
    // upstream of every downbeat symptom that has been reported.
    //
    // The fix applies the tolerance the rest of the pipeline already uses: smooth
    // the onset train by +/-PLL_TOL_BINS once, then comb the smoothed array.  That
    // also makes the choice principled - a grid denser than the onsets cannot score
    // above the number of onsets and a sparser one cannot reach it, so the LONGEST
    // period that captures every onset wins, which is the beat period.  (Windows
    // cannot overlap: the shortest period in range, 180 BPM, is 66 bins against a
    // 9-bin window.)
    static uint16_t tSm[T_BINS];
    for (uint32_t i = 0; i < T_BINS; i++) {
      uint32_t v = 0;
      for (int32_t o = -(int32_t)PLL_TOL_BINS; o <= (int32_t)PLL_TOL_BINS; o++) {
        int32_t j = (int32_t)i + o;
        if (j < 0) j += (int32_t)T_BINS;
        else if (j >= (int32_t)T_BINS) j -= (int32_t)T_BINS;
        v += tBin[j];
      }
      tSm[i] = (uint16_t)v;
    }

    uint32_t bestScore = 0, bestPer = 0, bestPh = 0;
    for (uint32_t bpm = BPM_MIN; bpm <= BPM_MAX; bpm++) {
      const uint32_t P = (uint32_t)((60ULL * TARGET_FS) /
                                    ((uint64_t)bpm * T_BIN_SAMPLES));
      if (P < 3 || P >= T_BINS) continue;
      for (uint32_t ph = 0; ph < P; ph++) {
        uint32_t s = 0;
        for (uint32_t age = ph; age < T_BINS; age += P) {
          uint32_t idx = tBinIdx + T_BINS - age;
          if (idx >= T_BINS) idx -= T_BINS;
          s += tSm[idx];
        }
        if (s > bestScore) { bestScore = s; bestPer = P; bestPh = ph; }
      }
    }

    // Seed the agent pool from the comb result once, then let the PLLs track.
    // The document's example pool is a tempo plus its half and double - exactly
    // the octave ambiguity that "instantly drift or double/halve its BPM"
    // describes - so those are the four hypotheses here.  bestPh sets agent 0's
    // phase to the comb's own best grid.
    // Re-seeding is allowed when every agent has decayed to zero.  The document
    // says the pool exists so the highest-scoring agent can be promoted with the
    // others kept "to prevent catastrophic reset jumps" - but a ONE-SHOT seed
    // cannot recover from a bad first guess, and that is exactly what was
    // measured: the pool took the very first comb result (a spurious score of 1)
    // and stayed on it for the whole run, reporting 60/120 BPM at 3% confidence.
    bool allZero = true;
    for (uint8_t a = 0; a < NAGENT; a++) if (ag[a].score > 0) allZero = false;
    (void)allZero;   // retained as a named quantity; no longer drives re-seeding
    // Evidence: count the onsets in the SAME 4 s window the comb looked at, and
    // require a CAPTURE FRACTION rather than a raw score, so the test neither
    // fails at the real rate nor passes on chance.
    uint32_t tBinTotal = 0;
    for (uint32_t i = 0; i < T_BINS; i++) tBinTotal += tBin[i];
    // The FIRST acquisition may be quick, because there is nothing to protect.
    // Every seed AFTER that must be slow and strict - including the one taken when
    // all agents have decayed to zero.
    // MEASURED: keying this on "no agent currently scores" gave the allZero case
    // the LOOSE path (3 s hold, 30% capture).  Because in that run every agent
    // scored 0 (a0=131/0, a2=180/0 ...), the pool re-seeded every 3 seconds, and
    // the period changed on each re-seed: per = 10920, 11160, 22080, 10920, 11160,
    // 16740 in a single run.  That is the erratic, off-time metronome reported.
    // A three-second reset loop is precisely the "catastrophic reset jump" that
    // Audio.md says the pool exists to prevent.
    // The correct key is whether the pool was EXPLICITLY cleared - never seeded, or
    // emptied by the input-gap timeout - and not whether it currently scores, so a
    // pool that is merely failing to track cannot reset itself repeatedly.
    const bool firstSeed    = !agSeeded;
    const uint32_t needPct  = firstSeed ? SEED_CAPTURE_PCT : RESEED_CAPTURE_PCT;
    const uint32_t needHold = firstSeed ? 3                : RESEED_HOLD_SEC;
    // bestScore IS a count of captured onsets: the smoothed windows around
    // successive grid points are disjoint (the shortest period in range, 180 BPM,
    // is 66 bins against a 9-bin window), so each captured onset is counted once.
    // It is therefore compared with the RAW onset count, which is what makes this
    // a capture FRACTION.  Comparing it with the smoothed total was wrong by a
    // factor of 9 - tSmTotal counts every onset once per window position - and
    // demanded captured >= 2.7 x total, which is unsatisfiable, so the pool never
    // seeded and NO beats were emitted at all.
    const bool seedEvidence =
        (tBinTotal >= SEED_MIN_ONSETS) &&
        ((uint64_t)bestScore * 100ULL >= (uint64_t)needPct * tBinTotal);
    // Published so a seed failure is diagnosable from the log instead of guessed:
    // ons = onsets in the comb's window, cap = the best period's capture of them.
    g_stats.combOnsets  = tBinTotal;
    g_stats.combCapture = (tBinTotal > 0)
        ? (uint32_t)(((uint64_t)bestScore * 100ULL) / (uint64_t)tBinTotal) : 0;
    // The comb's chosen phase, so the seeded grid can be compared against where
    // the onsets actually are if an agent fails to score.
    g_stats.combPh = bestPh;
    // Stage 3: "continuously calculate and lock onto the dominant periodicity".
    // MEASURED DEFECT: the pool is seeded ONCE, and if that first seed is taken
    // from an early, unreliable comb result it stays there for ever.  In the last
    // run the pool was seeded at comb=4@109 (TEM 227) and the comb then settled at
    // 126-133 for the whole rest of the track - but agent 0 remained at 109
    // (a0=109/0), so the metronome ran at the WRONG tempo and sat half a beat off
    // the onsets (ad = 230-257 ms against a 550 ms beat, which is p/2).  That is
    // "not on time, not the right tempo" exactly.
    // Adopting a materially better comb result is Stage 3's stated job, NOT the
    // "catastrophic reset jump" the pool exists to prevent: a reset jump is a reset
    // with no evidence behind it.  This one is guarded three ways so it cannot
    // thrash - the comb must capture >= 80% of the onsets, must disagree with the
    // promoted agent by >= 12%, and the same 6 s hold applies.
    const uint32_t poolBpm = ag[bestAg].bpm;
    const bool combBetter =
        (g_stats.combCapture >= 80) && (g_stats.combBpm > 0) && (poolBpm > 0) &&
        ((g_stats.combBpm > poolBpm * 112 / 100) ||
         (g_stats.combBpm < poolBpm * 88 / 100));
    // DRIFT CORRECTED.  The condition was "(!agSeeded || allZero)" - it re-seeded
    // whenever no agent happened to be scoring.  Audio.md section 2 states the pool
    // exists "to prevent catastrophic reset jumps during music transitions or
    // drops", so a pool that is merely failing to track must NOT reset itself.
    // MEASURED: with every agent scoring 0 the pool re-seeded every 3 seconds and
    // the metronome period changed each time (per = 10920, 11160, 22080, 10920,
    // 11160, 16740 in one run).
    if ((!agSeeded || combBetter) && seedEvidence &&
        (g_stats.seconds - lastSeedSec) >= needHold) {
      agSeeded    = true;
      lastSeedSec = g_stats.seconds;
      const uint32_t p0 = bestPer * T_BIN_SAMPLES;      // period in samples
      ag[0].setPeriod(p0);                              // clamped to 60-180 BPM
      ag[1].setPeriod(p0 * 2);                          // half-tempo hypothesis
      ag[2].setPeriod((p0 > 1) ? (p0 / 2) : 1);         // double-tempo
      ag[3].setPeriod((p0 * 3) / 2);
      for (uint8_t a = 0; a < NAGENT; a++) {
        ag[a].score = 0; ag[a].hits = 0; ag[a].beats = 0;
        ag[a].lastHitAt = 0;
        ag[a].nextBeat = (int64_t)absN + (int64_t)ag[a].period;
      }
      // A fresh pool has no bar phase.  Setting the phase origin to the next
      // beat makes that beat Beat 1, so the downbeat anchor is NOT armed here -
      // it is reserved for the structural markers the document names
      // ("structural drop-outs, vocal entries ... phrase boundary").
      // Arming it on a re-seed was my addition and is removed.
      phaseOff    = beatIdx;
      // The comb's most recent grid point is bestPh bins ago, so its next one
      // is (P - bestPh) bins ahead.
      ag[0].nextBeat = (int64_t)absN +
                       (int64_t)(bestPer - bestPh) * (int64_t)T_BIN_SAMPLES;
    }

    // "Dynamically promote the HIGHEST-SCORING agent" - exactly as written, with
    // NO hysteresis.  That is only safe because the SCORING was corrected: it now
    // penalises grid points with no onset, which removes the bias toward the
    // fastest hypothesis.  Before that fix plain argmax picked a 180 BPM agent
    // over a correct 130 BPM one; the fault was in the score, not in the argmax.
    bestAg = 0;
    for (uint8_t a = 1; a < NAGENT; a++) {
      if (ag[a].score > ag[bestAg].score) bestAg = a;
    }
    g_stats.bpmLocked = ag[bestAg].bpm;
    g_stats.bestPeriod = ag[bestAg].period;      // in samples, so the emitted
    g_stats.bestBeats  = ag[bestAg].beats;       // rate can be checked directly
    g_stats.bpmConf   = (onsetWin > 0)
                        ? (uint32_t)((uint64_t)ag[bestAg].hits * 100ULL / onsetWin)
                        : 0;
    g_stats.combScore = bestScore;
    g_stats.combBpm   = (bestPer > 0)
                        ? (uint32_t)((60ULL * TARGET_FS) /
                                     ((uint64_t)bestPer * T_BIN_SAMPLES))
                        : 0;
    g_stats.beatsPred = 0;
    for (uint8_t a = 0; a < NAGENT; a++) {
      g_stats.agentBpm[a]   = ag[a].bpm;
      g_stats.agentScore[a] = ag[a].score;
      g_stats.agentHits[a]  = ag[a].hits;
      g_stats.beatsPred    += ag[a].beats;
    }
    onsetWin = 0;                                  // per-second hit rate
    for (uint8_t a = 0; a < NAGENT; a++) ag[a].hits = 0;

    // Dual-timescale normalisation gain, from the LONG-term average, recomputed
    // once per window so it equalises the track's level without chasing a
    // transient.
    gNorm = 4096;
    if (longEnv > NORM_MIN_E) {
      gNorm = (int32_t)(((int64_t)NORM_REF_E << 12) / longEnv);
      if (gNorm < NORM_GAIN_MIN) gNorm = NORM_GAIN_MIN;
      if (gNorm > NORM_GAIN_MAX) gNorm = NORM_GAIN_MAX;
    }
    g_stats.normGain = gNorm;
    g_stats.agcGain  = gAgc;

    // Stage 4 readout: the structural anchor's own inputs, so the downbeat
    // heuristic can be judged rather than trusted.
    g_stats.shortE       = (int32_t)(shortEnv >> DC_Q);
    g_stats.longE        = (int32_t)(longEnv  >> DC_Q);
    g_stats.structShifts = structCnt;
    g_stats.anchors      = anchorCnt;
    // Publish the last COMPLETED bar in LABEL order (E[0] = Beat 1 ... Beat 4),
    // so the log reads the same way the downbeat rule does and shows the ratios
    // the rule actually compared.
    for (uint8_t j = 0; j < 4; j++) {
      g_stats.beatE[j] = q12ToMilli(lastBarE[j]);
      // meanE is a NORMALISED fraction, not a raw energy: each bar contributes
      // 1000 x (its own energy / that bar's peak), so the loudest beat of a bar is
      // 1000 and a quiet track and a loud one are directly comparable.
      g_stats.meanE[j] = lastMeanE[j];
    }
    g_stats.barWinner     = lastBarWin;
    g_stats.barRatio      = lastBarRatio;
    g_stats.phraseAnchors = phraseCnt;
    g_stats.phraseGap     = phraseGap;
    // Signed onset-to-grid offset of the promoted agent, in ms.
    g_stats.pllErrMs = (agErrN[bestAg] > 0)
        ? (int32_t)((agErrSum[bestAg] * 1000) /
                    ((int64_t)agErrN[bestAg] * (int64_t)TARGET_FS))
        : 0;
    g_stats.pllErrN = agErrN[bestAg];
    g_stats.a0ErrMs = (a0AbsErrN > 0)
        ? (int32_t)((a0AbsErrAcc * 1000) /
                    ((int64_t)a0AbsErrN * (int64_t)TARGET_FS))
        : 0;
    // pp= : mean position of the novelty peak inside the beat interval, percent.
    g_stats.peakPosPct = (peakPosN > 0)
        ? (uint32_t)((peakPosAcc * 100ULL) / ((uint64_t)peakPosN * 255ULL))
        : 0;
    g_stats.dbAnchors    = dbAnchorCnt;
    g_stats.beatEPeak    = q12ToMilli(novPeakSinceBeat);

    int64_t elapsedUs = esp_timer_get_time() - winStart;
    int32_t pp  = dcQToCounts(mxQ - mnQ);
    int32_t rms = dcQToCounts((int32_t)isqrt64((uint64_t)(sumSq / (int64_t)WINDOW_SAMPLES)));

    g_stats.dc      = (int32_t)(dcAcc >> DC_Q);
    g_stats.dcExact = (int32_t)(sumRaw / (int64_t)WINDOW_SAMPLES);
    g_stats.acMin   = dcQToCounts(mnQ);
    g_stats.acMax   = dcQToCounts(mxQ);
    g_stats.acPp    = pp;
    g_stats.acRms   = rms;
    g_stats.acMean  = dcQToCounts((int32_t)(sumQ / (int64_t)WINDOW_SAMPLES));
    g_stats.negPct = (uint32_t)((uint64_t)neg * 100ULL / WINDOW_SAMPLES);

    g_stats.pr50  = probe[0].amplitudeMilli(WINDOW_SAMPLES);
    g_stats.pr90  = probe[1].amplitudeMilli(WINDOW_SAMPLES);
    g_stats.pr100 = probe[2].amplitudeMilli(WINDOW_SAMPLES);
    g_stats.pr120 = probe[3].amplitudeMilli(WINDOW_SAMPLES);

    g_stats.bandRmsMc = q12ToMilli((int64_t)isqrt64((uint64_t)(bandSumSq / (int64_t)WINDOW_SAMPLES)));

    g_stats.novPkMc = q12ToMilli(det.novPk);
    g_stats.avgMc   = q12ToMilli(det.avgAcc >> det.avgShift);
    g_stats.thrMc   = q12ToMilli(det.thr);
    g_stats.floorMc = q12ToMilli(det.floor);
    g_stats.fireWin = det.fireWin;
    g_stats.refrWin = det.refrWin;
    g_stats.onsets  = g_stats.onsets + lowValidWin;   // VALIDATED onsets only

    // Multi-band fusion readout (Audio.md supplemental section 1).
    g_stats.lowCand    = lowCandWin;
    g_stats.lowValid   = lowValidWin;
    g_stats.lowRej     = lowRejWin;
    g_stats.midFire    = detMid.fireWin;
    g_stats.highFire   = detHigh.fireWin;
    g_stats.midFluxPk  = midFluxPk;
    g_stats.midThrMc   = detMid.thr;
    g_stats.highFluxPk = highFluxPk;
    g_stats.highThrMc  = detHigh.thr;

    // Inter-onset intervals, published LAST so the reader never sees a stale
    // count alongside fresh data.
    for (uint8_t k = 0; k < ioiLocalN; k++) g_stats.ioiMs[k] = ioiLocal[k];
    g_stats.ioiN = ioiLocalN;

    g_stats.actPct = (uint32_t)((uint64_t)over * 100ULL / WINDOW_SAMPLES);
    g_stats.rateHz = (uint32_t)(((uint64_t)WINDOW_SAMPLES * 1000000ULL) /
                                (uint64_t)(elapsedUs > 0 ? elapsedUs : 1));
    g_stats.totalSamples += WINDOW_SAMPLES;
    if (pp > g_stats.acMaxPp) g_stats.acMaxPp = pp;

    if (g_stats.seconds < CAL_SECONDS) {          // silent baseline at boot
      calSumSq += (uint64_t)sumSq;
      calN     += WINDOW_SAMPLES;
      if (pp > calPp) calPp = pp;
      if (g_stats.seconds + 1 == CAL_SECONDS) {   // last calibration window
        // Audio.md: "measure the background ADC RMS energy and dynamic range
        // before music starts" and "dynamically calculate baseline noise
        // offsets (sigma_noise) and feed them directly into the Stage 2
        // adaptive threshold peak picker as a baseline floor".
        //
        // DEVIATION, discussed with the user BEFORE it was written: the ADC RMS
        // and dynamic range ARE measured and published exactly as written
        // (noiseRms, noisePp).  But the FLOOR is derived from sigma measured on
        // the NOVELTY, because the Stage 2 picker thresholds the novelty curve,
        // not the ADC signal.  A sigma taken on the ADC is in counts and cannot
        // be fed "directly" into a threshold on a filtered, differentiated
        // quantity: the difference gain is frequency dependent, so any
        // conversion factor would itself be a derived constant needing its own
        // justification.  Measuring sigma on the novelty is the same intent - a
        // boot-time noise baseline, nothing hardcoded - in the units the
        // threshold actually uses.
        g_stats.noiseRms = dcQToCounts((int32_t)isqrt64(calN ? (calSumSq / (int64_t)calN) : 0));
        g_stats.noisePp  = calPp;
        g_stats.sigmaMc  = q12ToMilli((int64_t)isqrt64(calNovN ? (calNovSumSq / (int64_t)calNovN) : 0));
        // sigmaMc is in milli-counts; the floor must be in the same DC_Q fixed
        // point as the novelty, so convert: Q12 units = mc * 4096 / 1000.
        const int32_t sigmaQ = (int32_t)(((int64_t)g_stats.sigmaMc * 4096) / 1000);
        det.floor            = sigmaQ * NOV_FLOOR_MUL;
        // Mid and high band floors: the same sigma-times-multiplier rule,
        // measured on each band's OWN silent flux.  These fluxes are already in
        // energy units, so no unit conversion is needed.
        const int32_t midSigma  = (int32_t)isqrt64(midCalN  ? (midCalSumSq  / (int64_t)midCalN)  : 0);
        const int32_t highSigma = (int32_t)isqrt64(highCalN ? (highCalSumSq / (int64_t)highCalN) : 0);
        detMid.floor  = midSigma  * NOV_FLOOR_MUL;
        detHigh.floor = highSigma * NOV_FLOOR_MUL;
        g_stats.calibrated   = 1;
      }
    }

    // Clear the per-window counters AFTER the calibration block has read them.
    // They MUST be cleared or they accumulate for ever - but clearing them
    // before that block silently zeroed the calibration and left the floor at
    // 0, so the detector fired on everything.  That was measured on this board.
    det.fireWin = 0; det.refrWin = 0; det.novPk = 0;
    detMid.fireWin = 0; detMid.refrWin = 0; detMid.novPk = 0;
    detHigh.fireWin = 0; detHigh.refrWin = 0; detHigh.novPk = 0;
    lowCandWin = 0; lowValidWin = 0; lowRejWin = 0;
    midFluxPk = 0; highFluxPk = 0;

    g_stats.seconds = g_stats.seconds + 1;   // published LAST: the reader
                                             // keys off this field
  }
}

// ---------------------------------------------------------------------------
//  OUTPUT HELPERS  -  Core 1 only (the Arduino loop task).
// ---------------------------------------------------------------------------
static void pulse(uint8_t pin, uint16_t ms) {
  digitalWrite(pin, HIGH);
  delay(ms);
  digitalWrite(pin, LOW);
}

// ---------------------------------------------------------------------------
//  Non-blocking IO48 marker.  ONE writer (this task), so there is no GPIO race
//  with the DSP task, and it never blocks the publish path - a blocking flash
//  would stall the log for 100 ms on every onset.  Retriggerable: an onset
//  arriving during a flash restarts the full 100 ms.
// ---------------------------------------------------------------------------
static bool     s_io48On    = false;
static uint32_t s_io48OffAt = 0;

static void io48Mark(uint16_t ms) {
  digitalWrite(PIN_IO48, HIGH);
  s_io48On    = true;
  s_io48OffAt = millis() + ms;
}

static void io48Service() {
  if (s_io48On && (int32_t)(millis() - s_io48OffAt) >= 0) {
    digitalWrite(PIN_IO48, LOW);
    s_io48On = false;
  }
}

// ---------------------------------------------------------------------------
//  Non-blocking CHANNEL marker, one writer (this task).
//  Audio.md's hardware tie-in: "All beats will be flashed on IO48 100ms flash.
//  Beat 1 is on channel 1 100ms flash.  Beat 2 is on channel 2 ... Beat 4 is on
//  channel 4 100ms flash."
// ---------------------------------------------------------------------------
static bool     s_chOn[4]    = { false, false, false, false };
static uint32_t s_chOffAt[4] = { 0, 0, 0, 0 };

// ---------------------------------------------------------------------------
//  Audio.md: "Implement a toggleable binary or high-baud-rate diagnostic mode
//  (e.g., via serial command 'D') that streams comma-separated values or binary
//  packets compatible with the Serial Plotter tool ... The stream must
//  simultaneously output:
//    - Raw audio envelope / novelty function.
//    - Adaptive threshold curve.
//    - Detected onset trigger pulses.
//    - Internal modulo-4 bar count state (1,2,3,4)."
//  Press 'D' to toggle.  The per-second text lines are suppressed while it runs
//  so the plotter sees nothing but the CSV.
// ---------------------------------------------------------------------------
static bool s_plotMode = false;

static void chMark(uint8_t i, uint16_t ms) {
  if (i > 3) return;
  digitalWrite(CH_PINS[i], HIGH);
  s_chOn[i]    = true;
  s_chOffAt[i] = millis() + ms;
}

static void chService() {
  for (uint8_t i = 0; i < 4; i++) {
    if (s_chOn[i] && (int32_t)(millis() - s_chOffAt[i]) >= 0) {
      digitalWrite(CH_PINS[i], LOW);
      s_chOn[i] = false;
    }
  }
}

static void publishLine(uint32_t sec) {
  if (s_plotMode) return;            // 'D' mode: the CSV stream replaces this
  if (sec <= CAL_SECONDS) {
    Serial.printf("CAL %lu/%u  fs=%lu  dc=%ld  dcE=%ld  ac=%ld..%ld  pp=%ld  "
                  "rms=%ld  mean=%ld  neg=%lu%%  act=%lu%%  "
                  "ripMC=%ld/%ld/%ld/%ld\n",
                  (unsigned long)sec, (unsigned)CAL_SECONDS,
                  (unsigned long)g_stats.rateHz, (long)g_stats.dc,
                  (long)g_stats.dcExact,
                  (long)g_stats.acMin, (long)g_stats.acMax, (long)g_stats.acPp,
                  (long)g_stats.acRms, (long)g_stats.acMean,
                  (unsigned long)g_stats.negPct, (unsigned long)g_stats.actPct,
                  (long)g_stats.pr50, (long)g_stats.pr90,
                  (long)g_stats.pr100, (long)g_stats.pr120);
  } else {
    Serial.printf("RUN %lu  fs=%lu  dc=%ld  dcE=%ld  ac=%ld..%ld  pp=%ld  "
                  "rms=%ld  mean=%ld  neg=%lu%%  maxPP=%ld  act=%lu%%  "
                  "ripMC=%ld/%ld/%ld/%ld  to=%lu err=%lu rec=%lu\n",
                  (unsigned long)sec,
                  (unsigned long)g_stats.rateHz, (long)g_stats.dc,
                  (long)g_stats.dcExact,
                  (long)g_stats.acMin, (long)g_stats.acMax, (long)g_stats.acPp,
                  (long)g_stats.acRms, (long)g_stats.acMean,
                  (unsigned long)g_stats.negPct, (long)g_stats.acMaxPp,
                  (unsigned long)g_stats.actPct,
                  (long)g_stats.pr50, (long)g_stats.pr90,
                  (long)g_stats.pr100, (long)g_stats.pr120,
                  (unsigned long)g_stats.timeouts, (unsigned long)g_stats.errors,
                  (unsigned long)g_stats.recoveries);
  }

  // Stage 2 readout.  Every value is in MILLI-COUNTS (1000 mc = 1 ADC count)
  // because the in-band signal and the novelty are both sub-count.
  // Audio.md's telemetry requirement: the novelty curve, the threshold curve
  // and the onset pulses.  The inter-onset intervals are added for tuning.
  // Low band (Stage 1 + Stage 2).  "cand" is the CANDIDATE count; the fusion
  // decides how many of those become onsets, reported on the FUS line.
  Serial.printf("OND %lu  band=%ld  novPk=%ld  avg=%ld  thr=%ld  flr=%ld  "
                "sig=%ld  cand=%lu refr=%lu\n",
                (unsigned long)sec, (long)g_stats.bandRmsMc,
                (long)g_stats.novPkMc, (long)g_stats.avgMc,
                (long)g_stats.thrMc, (long)g_stats.floorMc,
                (long)g_stats.sigmaMc,
                (unsigned long)g_stats.fireWin, (unsigned long)g_stats.refrWin);

  // Fusion: a low-band candidate becomes an ONSET only if a mid-band (snare) or
  // high-band accent coincides with it or closely precedes it.
  Serial.printf("FUS %lu  cand=%lu valid=%lu rej=%lu  midAcc=%lu midFluxPk=%ld "
                "midThr=%ld  highAcc=%lu highFluxPk=%ld highThr=%ld  tot=%lu\n",
                (unsigned long)sec,
                (unsigned long)g_stats.lowCand, (unsigned long)g_stats.lowValid,
                (unsigned long)g_stats.lowRej,
                (unsigned long)g_stats.midFire, (long)g_stats.midFluxPk,
                (long)g_stats.midThrMc,
                (unsigned long)g_stats.highFire, (long)g_stats.highFluxPk,
                (long)g_stats.highThrMc,
                (unsigned long)g_stats.onsets);

  // Stage 3: the tempo lock, the comb bank's own answer, and the agent pool.
  // "bpm" is the promoted agent; "conf" is the share of this second's onsets
  // that landed on its grid; each aN is that agent's BPM and score.  "ons" and
  // "cap" are the comb's own inputs - onsets in its 4 s window and the best
  // period's capture of them - so a seed failure is diagnosable rather than guessed.
  Serial.printf("TEM %lu  bpm=%u conf=%lu%%  comb=%lu@%u ph=%lu  ons=%lu "
                "cap=%lu%%  ad=%ldms  beats=%lu |",
                (unsigned long)sec, (unsigned)g_stats.bpmLocked,
                (unsigned long)g_stats.bpmConf,
                (unsigned long)g_stats.combScore, (unsigned)g_stats.combBpm,
                (unsigned long)g_stats.combPh,
                (unsigned long)g_stats.combOnsets,
                (unsigned long)g_stats.combCapture,
                (long)g_stats.a0ErrMs,
                (unsigned long)g_stats.beatsPred);
  for (uint8_t a = 0; a < NAGENT; a++) {
    Serial.printf(" a%u=%u/%ld", (unsigned)a, (unsigned)g_stats.agentBpm[a],
                  (long)g_stats.agentScore[a]);
  }
  Serial.printf("\n");

  // Stage 4: phase alignment and the modulo-4 bar counter.  current_beat and
  // is_downbeat are the document's global state variables; anchors counts the
  // structural re-alignments, struct the shifts that triggered them.
  Serial.printf("BAR %lu  beat=%u down=%u  total=%lu  seq=%lu flash=%lu  "
                "anchors=%lu  struct=%lu  short=%ld long=%ld  "
                "gN=%ld gA=%ld  per=%lu pbeats=%lu  db=%lu  "
                "ph=%lu pg=%lu  pd=%ldms/%lu pp=%lu%%  Ew=%u Er=%lu%%  "
                "mE=%ld/%ld/%ld/%ld  "
                "E=%ld/%ld/%ld/%ld\n",
                (unsigned long)sec, (unsigned)g_stats.currentBeat,
                (unsigned)g_stats.isDownbeat, (unsigned long)g_stats.beatsTotal,
                (unsigned long)g_stats.beatSeq, (unsigned long)g_stats.flashes,
                (unsigned long)g_stats.anchors,
                (unsigned long)g_stats.structShifts,
                (long)g_stats.shortE, (long)g_stats.longE,
                (long)g_stats.normGain, (long)g_stats.agcGain,
                (unsigned long)g_stats.bestPeriod,
                (unsigned long)g_stats.bestBeats,
                (unsigned long)g_stats.dbAnchors,
                (unsigned long)g_stats.phraseAnchors,
                (unsigned long)g_stats.phraseGap,
                (long)g_stats.pllErrMs,
                (unsigned long)g_stats.pllErrN,
                (unsigned long)g_stats.peakPosPct,
                (unsigned)g_stats.barWinner, (unsigned long)g_stats.barRatio,
                (long)g_stats.meanE[0], (long)g_stats.meanE[1],
                (long)g_stats.meanE[2], (long)g_stats.meanE[3],
                (long)g_stats.beatE[0], (long)g_stats.beatE[1],
                (long)g_stats.beatE[2], (long)g_stats.beatE[3]);

  // Inter-onset intervals, in ms.  EVEN spacing near 461 ms means the detector
  // is finding beats (130 BPM); near 230 ms means half-beats; clustering means
  // one kick is double-triggering on its own decay.
  Serial.printf("IOI %lu  n=%u  ", (unsigned long)sec, (unsigned)g_stats.ioiN);
  for (uint8_t k = 0; k < g_stats.ioiN && k < MAX_IOI; k++) {
    Serial.printf("%u ", (unsigned)g_stats.ioiMs[k]);
  }
  Serial.printf("\n");
}

// ---------------------------------------------------------------------------
//  SETUP
// ---------------------------------------------------------------------------
void setup() {
  // Outputs first, all LOW, so nothing glitches on at power-up.
  digitalWrite(PIN_IO48, LOW);
  pinMode(PIN_IO48, OUTPUT);
  for (uint8_t i = 0; i < 4; i++) {
    digitalWrite(CH_PINS[i], LOW);
    pinMode(CH_PINS[i], OUTPUT);
  }

  Serial.begin(115200);
#if ARDUINO_USB_CDC_ON_BOOT
  // With CDCOnBoot=cdc, Serial IS the USB-Serial/JTAG peripheral (COM5).
  // Timeout 0 makes writes non-blocking, so a missing host can never stall us.
  Serial.setTxTimeoutMs(0);
#endif

  esp_err_t r = adcConfigure();
  if (r == ESP_OK) r = adc_continuous_start(s_adc);

  Serial.printf("\nSTAGE A  mic=GPIO%u (ADC1 ch%d)  targetFs=%lu Hz  "
                "frame=%lu samples  pool=%lu bytes\n",
                (unsigned)PIN_MIC, (int)ADC_CHAN, (unsigned long)TARGET_FS,
                (unsigned long)FRAME_SAMPLES, (unsigned long)POOL_BYTES);

  if (r != ESP_OK) {                 // unmistakable: 3 fast blinks, repeat
    Serial.printf("ADC START FAILED, err=%d\n", (int)r);
    for (;;) {
      pulse(PIN_IO48, 150); delay(150);
      pulse(PIN_IO48, 150); delay(150);
      pulse(PIN_IO48, 150); delay(1500);
    }
  }

  Serial.printf("5-second SILENT CALIBRATION starting now "
                "(IO48 flashes once per second).\n");

  // Audio.md: "Explicitly configure and feed the Task Watchdog Timer (TWDT)
  // inside the main non-DSP loops.  If the DSP core hangs or drops below a
  // minimum execution threshold, implement a non-blocking watchdog recovery
  // routine that safely resets the audio processing task without requiring a
  // hard power cycle."
  // The TWDT covers THIS (loop) task.  The DSP task is covered by its own driver
  // stall recovery plus the loop's stall counter, because restarting a task that
  // may be blocked inside a driver read from another core is not safe.
  {
    esp_task_wdt_config_t wcfg = {};
    wcfg.timeout_ms     = 5000;
    wcfg.idle_core_mask = 0;
    wcfg.trigger_panic  = false;
    if (esp_task_wdt_init(&wcfg) == ESP_ERR_INVALID_STATE) {
      esp_task_wdt_reconfigure(&wcfg);   // the Arduino core already started it
    }
    esp_task_wdt_add(NULL);              // this (loop) task
  }

  xTaskCreatePinnedToCore(audioTask, "audio", DSP_STACK, nullptr, DSP_PRIO,
                          nullptr, CORE_DSP);
}

// ---------------------------------------------------------------------------
//  LOOP  -  Core 1.  Watches the DSP task's window counter and turns each
//  completed window into one IO48 flash (calibration) or one tick
//  (heartbeat).  No measurement maths happens here.
// ---------------------------------------------------------------------------
void loop() {
  static bool     started = false;
  static uint32_t t0      = 0;
  static uint32_t tick    = 0;              // whole seconds since start
  static uint32_t lastSec = 0xFFFFFFFFu;
  static uint32_t lastSeq = 0;
// Separate counter for the onset indicator, so an onset flash and a beat flash
// can never swallow one another.
static uint32_t lastOnsetSeq = 0;

  if (!started) {
    started = true;
    t0   = millis();
    tick = 0;
  }

  io48Service();                            // turn IO48 off when its time is up
  chService();                              // and the beat channels
  esp_task_wdt_reset();                     // feed the TWDT (this task)

  // Audio.md: 'D' toggles the diagnostic plot stream.
  while (Serial.available() > 0) {
    const int ch = Serial.read();
    if (ch == 'D' || ch == 'd') {
      s_plotMode = !s_plotMode;
      Serial.printf("\n# plot mode %s\n", s_plotMode ? "ON" : "OFF");
    }
  }

  // DSP liveness.  If the window counter stops advancing, the DSP task has hung.
  // The RECOVERY lives in the DSP task itself (it restarts the ADC driver from
  // its own context, which is the only safe place to do it, since it may be
  // blocked inside a driver read); this detects and counts the condition.
  {
    static uint32_t lastDspSec = 0;
    static uint32_t lastDspMs  = 0;
    const uint32_t dspSec = g_stats.seconds;
    if (dspSec != lastDspSec) {
      lastDspSec = dspSec;
      lastDspMs  = millis();
    } else if ((uint32_t)(millis() - lastDspMs) > 3000u) {
      lastDspMs = millis();
      g_stats.dspStalls = g_stats.dspStalls + 1;
      Serial.printf("# DSP STALL: no window for 3 s (stalls=%lu)\n",
                    (unsigned long)g_stats.dspStalls);
    }
  }

  // --- 5 calibration flashes, at t = 1, 2, 3, 4, 5 s ------------------------
  // Driven by THIS task's own clock, never slaved to the DSP window counter:
  // if the DSP is late or misses a window, a slaved marker silently skips a
  // calibration flash.  It has to be exactly five, one per second.
  uint32_t due = (millis() - t0) / 1000u;
  if (due > tick) {
    tick = due;
    if (tick <= CAL_SECONDS) io48Mark(FLASH_MS);
  }

  // --- the onset indicator: IO48 pulses per VALIDATED ONSET until Stage 3 has
  //     locked a tempo ------------------------------------------------------
  // This is the behaviour this file's own header documents: "ONE 100 ms flash per
  // detected onset - so the onset detector can be judged by ear against the
  // music."  It was lost when the metronome output was added - onsetSeq was
  // written by the DSP and never read - and THAT is the 6-8 s of darkness at the
  // start of a track.  It is not the calibration: the first BEAT cannot occur
  // until Stage 3 has seeded the tempo pool, and the comb filter needs
  // T_BINS x T_BIN_SAMPLES = 800 x 120 = 96,000 samples = 4.0 s of onset history
  // before it can seed anything, plus the pool hold.  An onset flash needs none
  // of that, so feedback starts within a beat or two of the music.
  // Once the grid IS locked (bestPeriod != 0) IO48 carries the beats only,
  // exactly as Audio.md requires: "All beats will be flashed on IO48 100ms flash."
  uint32_t oseq = g_stats.onsetSeq;
  if (oseq != lastOnsetSeq) {
    lastOnsetSeq = oseq;
    // One-shot acquisition indicator: onsets until the FIRST beat ever exists,
    // then never again.  Gating on bestPeriod instead would flicker - the
    // promoted agent's period passes through 0 whenever the pool re-seeds - and
    // IO48 would alternate between onset flashes and beat flashes mid-track,
    // which reads as erratic timing.
    if (tick > CAL_SECONDS && g_stats.beatsTotal == 0) io48Mark(FLASH_MS);
  }

  // --- the metronome output: IO48 plus the channel for the beat number ------
  // Driven by beatSeq, which the DSP task bumps AT the predicted beat, so the
  // flash lands on the beat (the loop polls every 2 ms) rather than at the end
  // of a publish window.  Audio.md: "All beats will be flashed on IO48 100ms
  // flash.  Beat 1 is on channel 1 ... Beat 4 is on channel 4."
  uint32_t bseq = g_stats.beatSeq;
  if (bseq != lastSeq) {
    lastSeq = bseq;
    if (tick > CAL_SECONDS) {
      io48Mark(FLASH_MS);
      const uint8_t b = g_stats.currentBeat;
      if (b >= 1 && b <= 4) chMark((uint8_t)(b - 1), FLASH_MS);
      g_stats.flashes = g_stats.flashes + 1;   // proves the output path ran
    }
  }

  // --- publish each completed DSP window, exactly once ----------------------
  uint32_t sec = g_stats.seconds;
  if (sec != lastSec && sec > 0) {
    lastSec = sec;
    publishLine(sec);
    if (sec == CAL_SECONDS) {
      Serial.printf("CAL_DONE  noiseRms=%ld counts  noisePp=%ld counts  "
                    "onsetFloor=%ld mc\n",
                    (long)g_stats.noiseRms, (long)g_stats.noisePp,
                    (long)g_stats.floorMc);
    }
  }

  delay(2);

  // Audio.md 'D' mode: "comma-separated values ... compatible with the Serial
  // Plotter tool": novelty, threshold, onset pulse, modulo-4 beat count.
  if (s_plotMode) {
    static uint32_t nextPlot = 0;
    const uint32_t now = millis();
    if ((int32_t)(now - nextPlot) >= 0) {
      nextPlot = now + 10;                    // ~100 Hz
      Serial.printf("%ld,%ld,%u,%u\n",
                    (long)(((int64_t)g_stats.plotNov * 1000) / 4096),
                    (long)(((int64_t)g_stats.plotThr * 1000) / 4096),
                    (unsigned)g_stats.plotPulse,
                    (unsigned)g_stats.currentBeat);
    }
  }
}
