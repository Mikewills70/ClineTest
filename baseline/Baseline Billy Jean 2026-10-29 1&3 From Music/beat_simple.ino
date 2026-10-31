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

// ---- THE TIGHT GATE, ONCE THE PHASE IS SETTLED (added 29 Oct 2026) ------------
// MEASURED on the Billy Jean run: every "IO48 jumped ahead and was dragged back"
// event coincided with 3.0 onsets/s against a steady 2.0, and the period dropped
// ~8% (12300 -> 11300 samples) for a few seconds before recovering.  The cause is
// an EARLY spurious onset - a flam or ghost note just before the beat, 10-20%
// ahead.  At the wide +/-25% it passes the gate, is accepted as a BEAT, shortens
// the bar span, and that period error is multiplied by PHASE_DAMP into visible
// phase - the grid jumps AHEAD of the music and the pull then walks it back.
//
// While ACQUIRING the wide gate must stay: it has to lock on from a cold seed.
// Once the phase is SETTLED - i.e. we are demonstrably ON the beat - +/-12% is
// enough for any genuine tempo drift and rejects the early flam outright, so it
// touches neither the period nor the phase.  12% is not a tight tolerance: it is
// ~61 ms at 117 BPM, far more than any real beat-to-beat variation.
static const int32_t  IV_TIGHT_PCT      = 12;
// Consecutive tight rejections before the gate WIDENS itself again.  If the tempo
// really did move, rejecting forever would lock the grid out of the music - so it
// heals: several in a row is treated as "the tempo moved", not "all spurious".
static const uint8_t  IV_TIGHT_REJ_RUN  = 6;
static const int32_t  PERIOD_DAMP = 8;   // period moves 1/8 toward each beat
static const int32_t  PHASE_DAMP  = 8;   // phase moves 1/8 toward each beat

// The period is taken as the MEAN OF THE LAST FEW BEAT INTERVALS, not from a
// leaky single-interval adaptation.
//
// MEASURED: the leaky version wandered between 10,453 and 10,790 samples - about
// 3% - because one interval is a noisy estimate of the tempo.  The grid carries
// that period through a quiet section while it free-runs, and the error
// accumulates: 3% over 20 beats is 0.6 of a beat, which is enough to cross the
// half-beat ambiguity and flip the 1-and-3 parity when the beat returns.  That is
// why section 3 of the 4-section test track came back on 2 and 4.
//
// Averaging the last few intervals is a much better estimate of the real beat
// time, so the grid drifts far less while it free-runs through a gap.  The +/-25%
// interval gate already excludes spurious intervals, so the mean cannot be dragged
// by an outlier.
// NOTE: a mean-of-last-N period (PERIOD_HIST) was tried and REMOVED - see the
// period-adaptation note in the grid loop.  The baseline's leaky 1/8 adaptation is
// what kept time.
// If no beat for this long the grid free-runs rather than dying.
static const uint32_t BEAT_TIMEOUT_MS = 3000;
static const uint32_t BEAT_TIMEOUT_SAMPLES =
    (uint32_t)((uint64_t)BEAT_TIMEOUT_MS * TARGET_FS / 1000ULL);

// ---------------------------------------------------------------------------
//  ODD/EVEN DISCRIMINATION
//  RESTORED from baseline\Baseline Professional 2026-10-28\beat_track.ino, with
//  the band and the sense CHANGED.  Read this whole note before touching it.
//
//  THE DELETED CODE.  beat_track.ino had a modulo-4 bar counter (beatIdx +
//  phaseOff), a per-beat energy capture, per-bar-position running means, and a
//  rule that moved the phase when one position dominated.  All of it was removed
//  from beat_simple - see the "no mid or high bands, no three-band fusion" line
//  in this file's own header - and THAT is why the marker became a 50/50 coin
//  flip: nothing was left that looked at odd versus even at all.  beatCount is a
//  plain alternator, so the parity was whatever beat happened to be counted
//  first.  The user's diagnosis was exactly right.
//
//  WHY IT FAILED IN THE PROFESSIONAL BASELINE - measured, from that README:
//      Er (winner / runner-up) : 1587% at the start transient -> 109% filled
//      mE (normalised means)   : 716 / 755 / 583 / 824   (within 41%, unstable)
//  "the four beats' low-band energies converge to within ~10% of each other...
//   There is no four-beat energy cue in the low band on this material."
//  The rule was fed the LOW band.  The kick is on ALL FOUR beats, so there is
//  nothing there to discriminate, Er collapsed to 109% (no dominance), and the
//  rule correctly never fired.  The rule was not wrong - its INPUT was.
//
//  THE CUE IS IN THE MID BAND.  The snare/clap sits on 2 and 4, so the mid band
//  (500-2000 Hz) carries exactly the odd/even information.  Measured 22 Oct on
//  board data + the user's taps (al_out.txt):
//      lowest snare reading in the bar        80.0%
//      snare contrast voted over +/-8 bars    82.7%
//      (the then-current model               66.4%;  2-way chance is 50%)
//
//  SO THIS IS TWO CHANGES, NOT A REVERT:
//    1. Feed the rule the MID band, not the low band.
//    2. INVERT the sense - the LOWEST mid energy is beat 1, because the snare is
//       on 2 and 4, so the professional rule's "highest energy = beat 1" would
//       land on a snare beat (2 or 4) and give the wrong parity.
//  That is al_out.txt candidate A, which is the professional baseline's own rule
//  with the band corrected.
//
//  PHASE A IS DIAGNOSTIC ONLY.  The rule computes and is logged; the marker is
//  still driven by beatCount.  Nothing about the marker changes until the Er
//  numbers prove the cue is really there on this material.
// ---------------------------------------------------------------------------

// The mid band, exactly as the professional baseline has it: six Goertzel bins
// over 10 ms frames, the sum of the bin magnitudes being the band energy.
static const uint32_t FUS_FRAME_SAMPLES = 240;   // 10 ms at 24 kHz -> 100 Hz bins

static const uint8_t  MID_NBINS = 6;
// MEASURED DEFECT, on a track with a CLEAR snare, and it is the reason the group has
// always been a coin flip.  Audio.md specifies the mid band as 500-2000 Hz for
// "Snare/clap transient tracking", and that is exactly what was implemented.  But on
// this material that band is excited by EVERY beat:
//
//     ap 327  mid=XXXXXXXXXXXXXXXX (16)   2grp=100%/93%
//
// Sixteen accents in sixteen beats, and a two-group split of 7% - i.e. none.  The
// KICK's click and body live in 500-2000 Hz, so the "snare band" was in practice a
// SECOND KICK DETECTOR and carried no odd/even information whatsoever.  Every cue
// built on it - the accent rate, the argmin rule, the parity steer - was therefore
// reading noise, which is why the group never had a reliable reference.
//
// The snare/clap SNAP sits higher, roughly 1.5-4 kHz, where the kick has far less
// energy.  Moving the bins there is the ONE change that can turn this band into a
// real group cue.  Same code, one variable - and `2grp` will say immediately whether
// the split appears.
static const uint32_t MID_HZ[MID_NBINS] = { 1500, 2000, 2500, 3000, 3500, 4000 };
static const uint8_t  HIGH_NBINS = 6;
static const uint32_t HIGH_HZ[HIGH_NBINS] = { 5000, 6000, 7000, 8000, 9000, 10000 };

// THE ACCENT DETECTORS - the two-group instrument.
// Each fusion band gets its OWN OnsetDet, so "did a snare/clap accent land on
// this beat?" is a PER-BEAT BINARY measurement with its own adaptive threshold.
// It self-calibrates to the music and does NOT depend on comparing energies
// across bar positions, so it says something about the two groups of beats even
// when the bar position is in doubt.
//
// The user's point, 28 Oct, and it is the right one: the task is to separate the
// two GROUPS of beats, not to reason about parity.  The accent flag does exactly
// that - the beats that fire it and the beats that do not.
//
// ACCENT_MARGIN_PCT 250 and avgShift 5 are the professional baseline's measured
// values (its lines 150 and 895-899), restored unchanged.
static const uint32_t ACCENT_MARGIN_PCT = 250;
static const uint8_t  ACCENT_AVG_SHIFT  = 5;     // ~0.32 s over 10 ms frames
static const uint32_t FUS_REFR_FRAMES =
    (uint32_t)((uint64_t)REFRACTORY_MS *
               (TARGET_FS / FUS_FRAME_SAMPLES) / 1000ULL);

// How many beats of accent pattern to publish.  The PATTERN is the answer to
// "are the two groups distinguishable": with a clap on 2 and 4 it reads
// X.X.X.X. (or .X.X.X.X); on every beat XXXXXXXXXXXXXXXX; on none, all dots.
// A pattern that alternates in PAIRS is a real cue.  A pattern that does not is
// not, and no amount of parity reasoning will conjure one out of it.
static const uint8_t ACC_PAT_N = 16;

// ---- ACCENT-STRENGTH CODE (added 29 Oct 2026) -----------------------------
// One character per beat, base 36, in 50% steps of that band's own local average:
//   '0' = under 50%      '1' = 50-99%      '2' = 100-149% ... '9' = 450-499%
//   'A' = 500-549%       'B' = 550-599%    ... 'Z' = 1750% and above
// 50% steps are fine enough to separate an accented beat group from an unaccented
// one and coarse enough that a 16-character string stays readable in the log.
// This exists because the X/. accent flag SATURATES on this material - it fires on
// every beat, so it cannot show a difference between the two groups at all.
static inline char strenCode(int32_t ratPct) {
  int32_t s = ratPct / 50;
  if (s < 0)  s = 0;
  if (s > 35) s = 35;                       // 'Z' is the top code
  return (char)((s < 10) ? ('0' + s) : ('A' + (s - 10)));
}

// A MARKED beat is shown in LOWERCASE, so ONE string carries both the accent
// strength AND which parity CH10 actually flashed on.  That single piece of case is
// what ties the strength split to the GROUP: without it the string can only prove a
// split exists, not which side of it is 1&3.  Digits have no case, so a marked beat
// whose code is a digit stays a digit - the LOW band, which is the band that has
// shown a split, codes entirely in letters, so nothing is lost there.
static inline char caseCode(char c, bool marked) {
  return (marked && c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

// Append a signed long as decimal text, optionally preceded by a comma, and keep
// the result NUL-terminated.  Returns the new write position.  Hand-rolled rather
// than snprintf because the destination is a volatile publish buffer and casting
// volatile away to call a library function is worse than fifteen lines of this.
static char *appendInt(char *p, char *end, long v, bool comma) {
  char     tmp[12];
  int      n   = 0;
  const bool neg = (v < 0);
  unsigned long u = neg ? (unsigned long)(-v) : (unsigned long)v;
  do { tmp[n++] = (char)('0' + (u % 10u)); u /= 10u; } while (u != 0 && n < 11);
  if (comma && p < end) *p++ = ',';
  if (neg   && p < end) *p++ = '-';
  while (n > 0 && p < end) *p++ = tmp[--n];
  if (p < end) *p = 0;
  return p;
}

// ===========================================================================
//  THE FULL ANALYSIS STACK, RESTORED
//  Everything below is from baseline\Baseline Professional 2026-10-28\beat_track.ino
//  (and, for the two spectral phrase cues, from beat_track\beat_track.ino where
//  they were completed after that baseline).  These are the sections that were
//  stripped out when beat_simple was cut down, and they are restored because the
//  1-and-3 / 2-and-4 question needs every cue the audio can offer.
//
//  The BEAT PATH is still beat_simple's own (onset-seeded grid with a bounded
//  phase pull) - that is the part measured working and it is not replaced.  What
//  is restored here is the ANALYSIS around it.
// ===========================================================================

// --- three timescales -------------------------------------------------------
// The Stage 4 anchor, the structural detector and the normaliser all share these
// envelopes.  MEASURED in the professional baseline: with a 43 ms "short" term the
// short/long ratio swung 187-313% on EVERY BEAT, so the structural detector fired
// 23 times in 15 seconds - ordinary musical activity, not phrase changes.
static const uint32_t FAST_AVG_SHIFT  = 10;    // ~43 ms   envelope
static const uint32_t SHORT_AVG_SHIFT = 14;    // ~0.7 s   recent level
static const uint32_t LONG_AVG_SHIFT  = 18;    // ~11 s    long-term level
// The long-term average is held SCALED.  MEASURED defect, the same class as the
// DC tracker: "longEnv += (fastEnv-longEnv) >> 18" with fastEnv ~250,000 gives an
// increment of ZERO, so the long-term average never moved and the anchor
// re-anchored Beat 1 half a second at a time.
static const uint32_t LONG_Q          = 10;

// --- the structural detector (Audio.md section 3) ---------------------------
static const uint32_t STRUCT_DROP_PCT = 50;    // short < 50% of long = drop-out
static const uint32_t STRUCT_RISE_PCT = 180;   // short > 180% of long = a rise
static const uint32_t STRUCT_HOLD_MS  = 1500;  // re-arm time
static const uint32_t STRUCT_HOLD_SAMPLES =
    (uint32_t)((uint64_t)STRUCT_HOLD_MS * TARGET_FS / 1000ULL);

// --- the phrase-boundary anchor (Audio.md Stage 4) --------------------------
// The markers are the phrase changes "occurring every 16 or 32 beats".  That
// PERIODICITY is the only thing that separates the true phrase boundary - and so
// Beat 1 - from an arbitrary quarter note.  MEASURED: the bare structural
// threshold fired 22 times in one track, roughly every 4 beats, so without this
// qualification the anchor resets Bar 1 constantly and destroys the count.
static const uint32_t PHRASE_TOL_BEATS = 2;    // +/- 2 beats on a 16/32 grid

static inline bool isPhraseGap(uint32_t g) {
  const uint32_t t = PHRASE_TOL_BEATS;
  return (g >= 16 - t && g <= 16 + t) || (g >= 32 - t && g <= 32 + t) ||
         (g >= 48 - t && g <= 48 + t);
}

// --- dual-timescale self-normalisation (Audio.md supplemental section 4) ----
// "Normalize incoming energy relative to the long-term average so the algorithm
//  behaves identically during a quiet intro and a wall-of-sound drop."
static const int32_t NORM_REF_E    = (int32_t)40 << DC_Q;   // reference level, Q12
static const int32_t NORM_MIN_E    = (int32_t)8  << DC_Q;   // normalise above this
static const int32_t NORM_GAIN_MIN = 1024;    // 0.25x, Q12
static const int32_t NORM_GAIN_MAX = 16384;   // 4.00x, Q12

// --- MAX9814 AGC compensation (Audio.md section 1) --------------------------
// "the DSP pipeline must incorporate an inverse envelope tracker or dynamic
//  expansion filter to prevent the AGC from flattening the transient novelty
//  curve required for onset detection."
static const int32_t AGC_K        = 2;        // expansion strength
static const int32_t AGC_GAIN_MAX = 24576;    // 6.00x, Q12

// --- Audio.md section 3: the two spectral phrase cues ----------------------
// "structural drop-outs, vocal entries, cymbal crashes on downbeats, or sudden
//  spectral centroid shifts that traditionally occur every 16 or 32 bars".
// The short/long ENERGY ratio above covers the drop-outs; these two cover the
// other three.  NEITHER depends on the onset train, so neither can disturb the
// beat grid.  Both baselines are held SCALED, like longAcc, because a shift-based
// integrator whose increment floors to zero never moves.
static const uint32_t CENT_Q           = 10;   // centroid fixed-point scale
static const uint32_t CENT_AVG_SHIFT   = 10;   // ~10 s baseline at 100 frames/s
static const uint32_t CENT_SHIFT_PCT   = 25;   // >= 25% from its own baseline
static const uint32_t CENT_HOLD_FRAMES = 30;   // ... and held 300 ms
// A crash differs from a hi-hat by DECAY, not by level: a hat is gone in ~30 ms,
// a crash rings for hundreds.  So a crash is high-band energy HELD above baseline.
static const uint32_t HIGH_AVG_SHIFT    = 10;  // ~10 s baseline for the high band
static const uint32_t CRASH_RISE_PCT    = 60;  // >= 60% above that baseline
static const uint32_t CRASH_HOLD_FRAMES = 40;  // ... and held 400 ms

// --- three-band fusion (Audio.md supplemental section 1) --------------------
// "a beat is only validated when a low-band transient coincides with or closely
//  precedes a mid-band (snare) or high-band accent".  "Coincides with or closely
//  precedes" = within COINC either side.
static const uint32_t COINC_MS      = 40;
static const uint32_t COINC_SAMPLES =
    (uint32_t)((uint64_t)COINC_MS * TARGET_FS / 1000ULL);
static const uint8_t  MAX_PENDING   = 8;
// Goertzel |X| for a bin of amplitude A over N samples is A*N/2.  The sample fed
// in is acQ (Q12 counts), so it is shifted down first: at full Q12 a 240-sample
// frame puts coeff*s1*s2 near 7e18 against an int64 limit of 9.2e18, which is too
// close for comfort.  Q8 keeps every product below ~3e16 with no loss that
// matters here, because only RELATIVE band energies are ever compared.
static const int32_t  MID_PUSH_SHIFT = 4;
static const int32_t  BIN_SHIFT      = 10;

// The rule must not judge a single bar.  MEASURED in the professional baseline:
// over 40 bars the single-bar winner landed on beat 1 fourteen times, beat 2
// three times, beat 3 eight times and beat 4 twelve times - i.e. on nothing - and
// it cleared the 120% margin on 16 of those 40 bars purely by chance.  The means
// over many bars average that noise down by sqrt(N).
// LOWERED from 120 to 108.  MEASURED: on this material the rule's contrast settles
// at about 115% with the argmin STABLE for a whole track (`win` never changed across
// ~100 bars), so the 120% margin blocked the rule from ever acting - it sat at
// cnf=0 while holding a perfectly consistent verdict, which is why nothing ever came
// of it.  The 120% figure came from the professional baseline, which judged over a
// TWO-bar window; this rule judges over a BAR_HIST window of 32 bars, where the
// per-bar noise is averaged down, so a modest contrast is far more significant than
// the same figure over 2 bars.  DB_CONFIRM_BARS = 8 consecutive agreeing bars must
// still hold, and that is the real evidence.
//
// This also matters for the measurement on the `mk` line: while the rule is blocked
// its LABELS are just the initial ones and mean nothing, so agreement against them
// would read ~50% and tell us nothing.  Letting it act makes its labels reflect the
// argmin, which is what the agreement has to be measured against.
static const uint32_t DOWNBEAT_MARGIN_PCT = 108;   // winner >= 108% of runner-up
// The professional value was 2.  Raised to 4 here: at 2 bars the per-bar error
// rate measured on this material leaves far too much chance in the verdict.
static const uint32_t DOWNBEAT_MIN_BARS   = 4;

// SLIDING WINDOW, not a cumulative average.  MEASURED 28 Oct - and this is the
// SECOND time this exact defect has been found in this project.
//
// With a cumulative mean the four positions converge to EQUALITY as the track
// runs.  The measured spread of the four mid-band means:
//      10 bars -> 211      50 bars -> 140      75 bars -> 35      100 bars -> 16
// and ErM fell 142% -> 100% over the same span, with mE[0] dragged 427 -> 649
// while the other three sat at ~600-650.  The cue was destroyed by the ESTIMATOR,
// not absent from the audio: every bar in a quiet section contributes four
// positions that are only noise, and those bars pull the running mean toward the
// population mean.
//
// baseline\Baseline Professional 2026-10-28\beat_track.ino hit this too, and its
// own comment records it: "this was 8 bars over an accumulator that never reset,
// so the estimate averaged the whole track".  Restoring the cumulative form
// reintroduced a defect that baseline had already identified.
//
// 16 bars is ~29 s at 133 BPM: long enough to average the per-bar noise down,
// short enough that a section boundary re-derives the parity quickly.
// RAISED to 32 (28 Oct): at 16 bars the argmin flipped bar to bar and the rule
// moved the phase 36 times in one track (anc=36, win only 61% on one label).
// A sliding window does NOT have the dilution problem of a cumulative mean - it
// tracks - so a longer window is safe and cuts the jitter by ~sqrt(2).
static const uint8_t BAR_HIST = 32;

// HYSTERESIS.  The phase moves only when the SAME position has been the argmin
// AND cleared the margin for this many CONSECUTIVE bar completions.  MEASURED
// without it: anc=36 over one track and the argmin changing every few bars, i.e.
// the rule was following noise rather than the music.  The comparison is on the
// POSITION index (beatIdx & 3), which is phase-independent, so a phase move does
// not reset the confirmation.
static const uint8_t DB_CONFIRM_BARS = 8;

// ===========================================================================
//  THE TWO-GROUP DECISION  -  the connection that was missing
// ===========================================================================
// The marker's CADENCE stays the baseline's exactly: alternate grid emissions, so
// markGap is 2 and the flashing rhythm is untouched.  What the analysis changes is
// WHICH GROUP that alternation lands on, and it is one latched bit.
//
// THE STATISTIC: the MID-band accent rate of the beats CH10 is CURRENTLY marking.
// The clap/snare sits on 2 and 4, so if the marked beats carry the accent the
// marker is on the wrong group and the bit inverts.  If the marked beats never
// carry it, the marker is already on 1 and 3.
//
// WHY THIS NEEDS NO BAR-PHASE KNOWLEDGE, which is the whole point: we never have
// to work out which beat is "1".  We only ask whether the beats we are marking are
// the accented group - a question the audio can answer, unlike the bar phase.
//
// THE MID BAND ONLY.  MEASURED: the HIGH band fires ~3/s on this material (470
// accents in 157 s against a 2.2/s beat rate) because it follows the hi-hats on the
// eighth notes, which is not a beat-group cue at all.  Using it would answer
// "accented" for every beat and the decision would be meaningless.
//
// FAIL-SAFE: between the two thresholds the decision HOLDS.  Ambiguity never moves
// the marker, so this can never be worse than the baseline's 50/50 - it can only
// improve on it.
static const uint8_t PARITY_WIN    = 32;   // marked beats per judgement
static const uint8_t PARITY_ACC_HI = 60;   // >=60% accented => marked group IS 2&4
static const uint8_t PARITY_ACC_LO = 40;   // <=40% accented => marked group IS 1&3

// How many CONSECUTIVE onsets a grid/onset mismatch must persist before it is
// treated as a real slip.  MEASURED: without this the resync fired every ~7 beats
// (rsy climbed 0 -> 61 in one track) and each firing flipped the group, so CH10
// flicked between 1&3 and 2&4 throughout - the opposite of the stability wanted.
// A genuine slip leaves the mismatch standing, so it still reaches this; an
// intermittent mismatch never does.  16 onsets is ~7 s at 133 BPM.
static const uint8_t RESYNC_CONFIRM = 16;

// ---- PHASE ACQUISITION -----------------------------------------------------
// A grid that seeds HALF A BEAT OUT walks back at period/32 per onset, because the
// pull is bounded to +/-period/4 and then damped by 8.  That is 16-32 onsets, 4 to
// 8 BARS, during which IO48 flashes BETWEEN the beats - the reported "off beat,
// between the beats for around 8 bars".  The seed is `nextBeat = absN` on the first
// onset found, so a single spurious mid-beat onset starts it half a beat out.
//
// MEASURED CONSEQUENCE, and it is the real damage: the marker's group was latched
// during that window, and while the grid is half a beat out the marked grid beats
// land on the WRONG musical group - which is how 2-and-4 got captured and then held.
// The latch was doing its job on corrupt input.
//
// Two changes: acquire at up to a quarter period per onset (still under the half
// period that would let it jump a whole beat), and latch the group ONLY once the
// phase has settled.  A settled phase means the last PHASE_SETTLE_ONSETS onsets have
// all landed within PHASE_SETTLE_MS of the grid - i.e. IO48 is ON the beat.
static const int64_t  PHASE_SETTLE_SAMPLES = (int64_t)TARGET_FS / 100;   // 10 ms
static const uint8_t  PHASE_SETTLE_ONSETS  = 8;

// ---- THE PHRASE PERIOD IS MEASURED, NOT ASSUMED ----------------------------
// Audio.md section 3 says the phrase markers are the events "occurring every 16 or
// 32 beats", and that it is THAT periodicity which separates a true phrase boundary
// - and so Beat 1 - from an arbitrary quarter note.
//
// MEASURED on this track, and it is why the anchor never armed: with the fixed
// 16/32 +/-2 test the counter `phr` stayed at 0 for the whole track while the
// structural detector fired 11 times (str=11).  The gaps came out around 20 beats,
// so 16+/-2 and 32+/-2 BOTH missed.  This material's phrases are not 16 or 32 beats
// long, so the assumption can never be satisfied however long we run.
//
// The periodicity itself is the evidence, and it can be measured: if the SAME gap
// keeps recurring between structural events, that gap IS the phrase period.  Three
// repeats is the bar.  This keeps the document's principle - only a PERIODIC event
// is a phrase boundary - while dropping the guessed period.
static const uint8_t PHRASE_REPEATS = 3;

// A phrase period must be PLAUSIBLE.  Audio.md names 16 or 32 beats; this material
// measures about 20, so 8 to 128 is the working range.
//
// MEASURED DEFECT, and it armed the anchor on nothing: with no range test the
// repeated-gap rule matched gap=0 and gap=1.  A gap of ZERO occurs whenever the
// structural detector fires repeatedly while the beat index is FROZEN - a quiet
// section with no onsets - and it repeated 6 times, so `phr` climbed to 8 and `amv`
// to 2 on pure silence.  A phrase cannot be one beat long, let alone none.
static const uint32_t PHRASE_MIN_BEATS = 8;
static const uint32_t PHRASE_MAX_BEATS = 128;

// ---------------------------------------------------------------------------
//  GOERTZEL  -  one bin, from the professional baseline unchanged.
//  x is fed in scaled counts; magnitude() returns the unscaled frame |X|.
//  isqrt64 lives further down with the other DSP helpers, so it needs a
//  declaration here - Goertzel::magnitude() is the only caller this early.
// ---------------------------------------------------------------------------
static uint32_t isqrt64(uint64_t v);

struct Goertzel {
  int32_t coeff;        // Q15: 2*cos(2*pi*f/Fs)
  int64_t s1, s2;

  void init(uint32_t f, uint32_t fs) {
    coeff = (int32_t)(2.0f * cosf(2.0f * PI * (float)f / (float)fs) * 32768.0f);
    s1 = 0;
    s2 = 0;
  }
  void reset() { s1 = 0; s2 = 0; }

  inline void push(int32_t x) {
    int64_t s = (int64_t)x + ((coeff * s1) >> 15) - s2;
    s2 = s1;
    s1 = s;
  }
  // Raw |X| for the frame just accumulated, unscaled.  The caller scales it.
  int64_t magnitude() const {
    int64_t p = s1 * s1 + s2 * s2 - ((coeff * s1 * s2) >> 15);
    return (p > 0) ? (int64_t)isqrt64((uint64_t)p) : 0;
  }
};

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
//
//  RESTORED from baseline\Baseline Professional 2026-10-28\beat_track.ino: the
//  three tuning values are now PER INSTANCE, because the same struct serves the
//  low band (ticks = SAMPLES) and the mid/high accent bands (ticks = 10 ms
//  FRAMES).  Without that, one detector cannot serve both.
// ---------------------------------------------------------------------------
struct OnsetDet {
  int64_t  avgAcc;
  int32_t  novPk;
  int32_t  thr;
  int32_t  floor;
  uint16_t avgShift;      // averaging: avg = avgAcc >> avgShift
  uint16_t marginPct;     // the margin, per cent of the local average
  uint32_t refrTicks;     // refractory lockout, in TICKS (samples or frames)
  uint32_t refrLeft;
  uint32_t total;

  // ---- ACCENT STRENGTH (added 29 Oct 2026) ---------------------------------
  // The binary "did an accent happen?" flag is SATURATED on this material.  At
  // ACCENT_MARGIN_PCT the detector fires on essentially every beat, so the X/./
  // pattern reads all-X on EVERY track measured, and a saturated flag physically
  // cannot show whether the two beat groups differ.  The missing quantity is how
  // STRONG the accent was relative to the band's own local average, which is what
  // these two members carry.
  //   avgNow  the local average from the most recent run()
  //   ratPct  the STRONGEST event since the caller last cleared it, in per cent of
  //           the local average (700 means seven times the local average)
  // The caller reads ratPct at each beat and clears it, so it is always the
  // strength of the strongest event inside THAT beat's interval.  Neither member
  // affects thr, the refractory or the return value - they are read-outs only.
  int32_t  avgNow;
  int32_t  ratPct;

  void reset() {
    avgAcc = 0; novPk = 0; thr = 0; floor = 0;
    refrLeft = 0; total = 0;
    avgNow = 0; ratPct = 0;
  }

  inline bool run(int32_t nov, bool armed) {
    if (nov > novPk) novPk = nov;

    avgAcc += (int64_t)nov - (avgAcc >> avgShift);
    const int32_t avg = (int32_t)(avgAcc >> avgShift);

    thr = (int32_t)(((int64_t)avg * (int64_t)marginPct) / 100);
    if (thr < floor) thr = floor;

    // ---- ACCENT STRENGTH: read-out only, does NOT change thr or the firing ----
    // One divide per call (per sample for the low band, per 10 ms frame for the
    // accent bands).  This is the ONLY new quantity that can separate the two
    // beat groups once the X/. flag has saturated - see the member note above.
    avgNow = avg;
    // GATED ON THE CALIBRATED FLOOR, which is 8x the noise sigma measured in
    // silence during calibration.  Without this gate the ratio is garbage in every
    // quiet moment: the local average collapses toward the noise and a meaningless
    // few-hundred-x ratio appears.  MEASURED on the first run of this instrument -
    // `mx=31600/4550/3550` with NO music playing at all.  Requiring the average to
    // clear the floor keeps the measurement to real signal, so a code means "this
    // beat's event stood this far above its own local average" and nothing else.
    if (avg >= floor && avg > 0) {
      const int32_t r = (int32_t)(((int64_t)nov * 100) / (int64_t)avg);
      if (r > ratPct) ratPct = r;
    }

    if (refrLeft) refrLeft--;

    if (armed && nov > thr) {
      if (refrLeft == 0) {
        refrLeft = refrTicks;
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

  // ---- THE PHASE MEASUREMENT, UNCLAMPED (added 29 Oct 2026) ----------------
  // pdMs above is the mean of dd AFTER the +/-period/4 bound is applied, so it
  // SATURATES at that bound (+/-128 ms at 120 BPM) and a larger error is
  // invisible in it.  MEASURED on the Billy Jean baseline: 14 of 115 one-second
  // samples read exactly at the bound, so the true error was being hidden.
  //   pdRawMs  the mean of dd BEFORE the bound - the honest phase error
  //   pdClamp  how many dd samples in the last second HIT the bound
  // If pdClamp is non-zero the pull is not keeping up; that is the servo telling
  // us so rather than us inferring it.
  volatile int32_t  pdRawMs;
  volatile uint32_t pdClamp;

  // ---- WHY THE GRID MOVES: the period span and the raw material -------------
  // The phase measurement above says the grid is out; these say WHY.
  //   per4Now / per8Now  the period over the last 4 and 8 accepted intervals.
  //     A single interval carries the DETECTOR's latency for that one beat, and
  //     that latency differs between a kick and a snare.  Over 4 beats (one bar)
  //     each group appears an equal number of times, so the latency cancels; over
  //     8 it cancels twice over and is steadier still but responds half as fast.
  //   ivsStr  the last 8 accepted intervals, samples - the alternation, if any
  //   ddsStr  the last 8 signed dd values, ms - alternation in the PHASE
  volatile uint32_t per4Now;
  volatile uint32_t per8Now;
  // The tight interval gate's state.  `gw` 1 = the gate has WIDENED itself back to
  // +/-25% because six onsets in a row fell outside +/-12% - which means the tempo
  // moved, not that the onsets were spurious.  `tr` is the running count of tight
  // rejections; a healthy steady track holds it at 0.
  volatile uint8_t  gateWide;
  volatile uint8_t  tightRej;

  // ---- THE GROUP STEER'S READING (29 Oct 2026) -----------------------------
  // `gsA`/`gsB` are the MID-band accent means of the two groups, in per cent of
  // each group's own local average - the split itself, as the steer measured it.
  // `gsT` is the beatCount value CH10 is marking, `gsV` 1 once a real split has
  // been measured.  A run with gsV=0 is one where the material showed NO split and
  // the old default/'F' is in force; gsV=1 with a clear gap is the music deciding.
  volatile int32_t  gsA;
  volatile int32_t  gsB;
  volatile uint8_t  gsT;
  volatile uint8_t  gsV;
  volatile char     ivsStr[64];
  volatile char     ddsStr[64];
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

  // ---- odd/even evidence (PHASE A: DIAGNOSTIC ONLY, nothing acts on these) ----
  // The four per-bar-position MEAN mid-band energies, in LABEL order, so the log
  // reads the way the rule does.  Beat 1 should be the LOWEST of the four if the
  // snare really is on 2 and 4.
  volatile int32_t  eMeanMid[4];
  // Contrast of the mid band: (second LOWEST mean x 100) / lowest mean.  This is
  // the inverted form of the professional baseline's Er (winner / runner-up),
  // because the sense is inverted - see the note at DOWNBEAT_MARGIN_PCT.  Above
  // ~120 means the lowest position is genuinely lower than the next one, i.e. a
  // real cue exists.  Near 100 means no cue, which is exactly what the low band
  // measured in the professional baseline (109%).
  volatile uint32_t eRatioMid;
  // The SAME measurement on the LOW band, as a control.  Expected near 100%: the
  // kick is on all four beats, so the low band has no odd/even cue.  Logging both
  // is the whole point of Phase A - if the low band also shows a contrast, or the
  // mid band shows none, the premise is wrong and must not be acted on.
  volatile uint32_t eRatioLow;
  volatile uint32_t eBars;       // bars accumulated into the means
  volatile uint32_t eWin;        // label holding the LOWEST mid mean (= should be 1)
  // The phase offset the rule WOULD adopt, and how many times it would have moved.
  // Published so the rule's verdict can be compared with what is heard, while the
  // marker itself is still driven by beatCount.
  volatile uint32_t ePhaseOff;
  volatile uint32_t eAnchors;
  volatile uint8_t  eActed;      // 1 once the rule has enough bars to speak
  // Consecutive bars the current argmin has held AND cleared the margin - the
  // hysteresis progress.  Reaching DB_CONFIRM_BARS is what permits a move, so
  // watching this number settle is how we tell "the rule has found the music"
  // from "the rule is following noise".
  volatile uint8_t  eCnf;
  // Liveness for the mid band.  The professional baseline hit a defect where a
  // DEAD probe and a SILENT ROOM both read 0 and could not be told apart.  This
  // is the peak mid flux over the last second: in silence it reads a non-zero
  // noise floor, which proves the whole Goertzel chain is alive.  Without it, a
  // dead band would look exactly like "no cue on this material" - the precise
  // mistake this project has already made once.
  volatile int32_t  eMidPk;
  // TWO-GROUP CONTRAST: the MID accent rate of each of the two alternating beat
  // groups over the last 64 beats, as percentages.  THE direct test for whether the
  // material carries the odd/even cue at all, and it needs no knowledge of which
  // group is which.  A clap on 2 and 4 => one near 100%, the other near 0%.  An
  // accent on every beat => both near 100%.  No cue => both near 50%.
  volatile uint8_t  accEvenPct;
  volatile uint8_t  accOddPct;
  // WHERE in the inter-onset interval the mid-band peak falls, as a 0..255
  // fraction of that interval.  The professional baseline carried exactly this
  // diagnostic (its `pp`) and its comment states the point: a beat's energy is the
  // peak in the interval BEFORE it, so a peak near the START means the peak
  // actually belongs to the PREVIOUS beat and the attribution carries a systematic
  // ONE-BEAT OFFSET.  A kick transient sits right after its beat, so a reading near
  // 0 would prove that offset - and an off-by-one would make the rule name the
  // wrong position, i.e. give the wrong parity however reliable the cue is.
  // Measured, not assumed.
  volatile uint32_t ePp;

  // ---- THE TWO-GROUP MEASUREMENT: the per-beat ACCENT PATTERN ----------------
  // One character per BEAT, newest last: 'X' = a mid- or high-band accent landed
  // on that beat, '.' = it did not.  THIS is the direct answer to "are the two
  // groups of beats distinguishable" - and it needs no parity reasoning at all.
  //
  //   X.X.X.X.X.X.X.X.   the accents alternate one-on one-off -> a REAL cue,
  //                      and the two groups are exactly the X beats and the . beats
  //   XXXXXXXXXXXXXXXX   an accent on every beat -> NO cue between groups
  //   ................   no accents at all -> NO cue
  //   X..X.X..X.X..X.X   irregular -> NO cue, and no amount of parity work helps
  //
  // A pattern that alternates in PAIRS is what we need.  If this line does not
  // show that, the cue is not there and I will say so.
  // SPLIT BY BAND, because combining them was MY error and it hid the answer.
  // MEASURED 28 Oct: the HIGH band fired 470 times in 157 s = 3/s, against a 2.2/s
  // beat rate - it is tracking the hi-hats on the eighth notes, which are NOT a
  // beat-group cue.  A combined mid-OR-high flag therefore read XXXXXXXXXXXXXXXX
  // on every beat and concealed everything.
  // The MID band fired 134 times in the same 157 s = 0.85/s = about 39% of beats.
  // THAT is a number that could mean something, and it was invisible.
  volatile char     accPatMid[ACC_PAT_N + 1];
  volatile char     accPatHigh[ACC_PAT_N + 1];
  volatile uint32_t accBeats;    // beats accumulated into the patterns
  volatile uint32_t accXMid;     // how many of the last ACC_PAT_N beats had a MID accent
  volatile uint32_t accXHigh;    // ... and a HIGH accent
  volatile uint32_t accMid;      // mid-band accents since boot
  volatile uint32_t accHigh;     // high-band accents since boot

  // ---- PER-BEAT ACCENT STRENGTH, one code per beat (added 29 Oct 2026) ------
  // The X/. pattern above is SATURATED on this material - it reads all-X on EVERY
  // track measured, because at ACCENT_MARGIN_PCT the detector fires on every beat.
  // A saturated flag cannot answer "is the accent stronger on one of the two beat
  // groups?", which is the only question the group decision needs answered.
  // These carry STRENGTH instead.  Each character is the strongest event in that
  // beat's interval as a multiple of that band's own local average, coded in 50%
  // steps, base 36:
  //   '0' <50%   '1' 50-99%   '2' 100-149% ... '9' 450-499%
  //   'A' 500-549%   'B' 550-599% ... 'M' 1100% ... 'Z' 1750% and above
  // READ IT AS A PATTERN, which is the whole point:
  //   C2C2C2C2C2C2C2C2   the two groups carry DIFFERENT strengths, and the
  //                      threshold to cut between them is visible (between 2 and C)
  //   9999999999999999   flat - that band has NO group cue at ANY threshold
  // This is the measurement that the binary flag could never make, and it needs no
  // knowledge of which group is 1&3 - so it separates "the cue is not in this
  // material" from "the cue is there and we are reading its sign wrongly".
  volatile char     margPatLow[ACC_PAT_N + 1];
  volatile char     margPatMid[ACC_PAT_N + 1];
  volatile char     margPatHigh[ACC_PAT_N + 1];
  // Strongest accent of the CURRENT second per band, in per cent of the local
  // average - the exact number behind the codes, for a fine reading.
  volatile uint32_t maxRatLow;
  volatile uint32_t maxRatMid;
  volatile uint32_t maxRatHigh;

  // ---- THE RESTORED ANALYSIS STACK: its behaviour readout -------------------
  // The structural/phrase anchor.  anchorMoves is how many times it moved Beat 1.
  // A handful per track is healthy; dozens means the qualification is not working
  // and the anchor is thrashing - the defect the professional baseline measured at
  // 22 firings per track, ~4 beats apart, before PHRASE_TOL_BEATS was applied.
  volatile uint32_t structEv;     // structural shifts seen
  volatile uint32_t phraseBd;     // QUALIFIED phrase boundaries
  // The MEASURED phrase period: the last gap between structural events in beats, and
  // how many times in a row it has repeated.  When `srun` reaches PHRASE_REPEATS the
  // gap IS the phrase period and the anchor arms.  This is what replaces the guessed
  // 16/32 that never once passed on this material.
  volatile uint32_t lastGap;
  volatile uint8_t  sameGapRun;
  volatile uint32_t anchorMoves;  // times the anchor moved Beat 1
  volatile uint32_t centMark;     // spectral-centroid phrase cues
  volatile uint32_t crashMark;    // cymbal-crash phrase cues
  // Three-band fusion.  lowEarly = candidates confirmed early by a mid/high
  // accent; lowValid = candidates accepted when their window expired.  A healthy
  // mix means the accents really do coincide with the low-band transients, which
  // is the document's premise.  lowRej = queue overruns.
  volatile uint32_t lowCand;
  volatile uint32_t lowEarly;
  volatile uint32_t lowValid;
  volatile uint32_t lowRej;
  // The two gains and the three envelopes.  Published so the normaliser's effect
  // is MEASURED rather than assumed - the whole stack looked present while gNorm
  // was a constant 1.0 and did nothing.
  volatile int32_t  gNorm;      // Q12, 4096 = 1.0x
  volatile int32_t  gAgc;       // Q12
  volatile int32_t  fastEnv;
  volatile int32_t  shortEnv;
  volatile int32_t  longEnv;

  // ---- THE TWO-GROUP DECISION READOUT ---------------------------------------
  // Which group CH10 marks is now decided from the audio instead of being a 50/50.
  // These fields show the reasoning, so the result can be checked rather than
  // taken on trust.
  volatile uint8_t  parInv;      // 1 = the decision has inverted the marker's group
  volatile uint32_t parRate;     // MID-accent rate of the MARKED beats, %
  volatile uint32_t parDec;      // 32-beat judgements made
  volatile int8_t   parLast;     // 1 = last judgement inverted, 0 = kept
  // The odd/even rule's verdict against the beats CH10 actually marks.  THE number
  // that settles the last design question - see the note in the DSP state block.
  volatile uint32_t ruleWin;     // marked beats counted
  volatile uint32_t ruleAgree;   // ... of which agreed with the rule's 1-and-3
  // The slip-proof marker group: latched at the first mark from the MUSIC's beat
  // parity, so a briefly fast or slow grid can no longer move CH10's half-bar.
  volatile uint8_t  grpLatched;  // 1 once the group has been latched
  volatile uint8_t  grpParity;   // the onset parity CH10 marks
  // Times the group was re-latched from a MEASURED PHRASE BOUNDARY rather than the
  // coin-flip first mark.  This is the number that shows Audio.md's Stage 4 absolute
  // reference actually taking control of the group.
  volatile uint32_t reLatch;
  // Times the alternation had to be re-aligned to the music.  Every one of these is
  // a grid slip that WOULD have moved CH10 half a bar and was put back at once - the
  // section-3 flip, corrected instead of suffered.
  volatile uint32_t resyncs;
  // Consecutive onsets the grid/onset mismatch has held for.  Climbing towards
  // RESYNC_CONFIRM means a REAL slip; resetting to 0 means the alternation is
  // aligned and the mismatches were intermittent.
  volatile uint8_t  resyncRun;
  // Phase acquisition: 1 once IO48 is ON the beat, and how many consecutive onsets
  // have landed within PHASE_SETTLE_SAMPLES.  `acq=0` with the run climbing means
  // the grid is still walking onto the beat - and the group is deliberately NOT
  // latched until it reads 1.
  volatile uint8_t  phaseSettled;
  volatile uint8_t  phaseRun;
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

  // ---- MID band (snare/clap), the odd/even evidence -------------------------
  // Purely ADDITIVE: it taps acQ, it does not sit in the low-band path, and it
  // cannot alter the beat grid.  That is deliberate - the low-band beat path is
  // the part that is measured working and must not be disturbed by this.
  static Goertzel gMid[MID_NBINS];
  for (uint8_t k = 0; k < MID_NBINS; k++) gMid[k].init(MID_HZ[k], TARGET_FS);
  // The HIGH band (hi-hat / rhythm reinforcement), restored with the mid band.
  static Goertzel gHigh[HIGH_NBINS];
  for (uint8_t k = 0; k < HIGH_NBINS; k++) gHigh[k].init(HIGH_HZ[k], TARGET_FS);
  uint32_t midFrameN   = 0;      // samples into the current 10 ms frame
  int64_t  midEnergyPrev = 0;    // previous frame's band energy, for the flux
  int64_t  highEnergyPrev = 0;
  int32_t  midPeakSinceBeat = 0; // peak mid flux since the last beat = this beat's
  int32_t  lowPeakSinceBeat = 0; // peak LOW novelty since the last beat (control)
  uint32_t midPeakAt = 0;        // sample index of that peak, for pp= (see Stats)
  uint64_t ppAcc = 0;            // running mean of the peak position, 0..255
  uint32_t ppN   = 0;

  // THE ACCENT DETECTORS.  One per fusion band, each with its OWN adaptive
  // threshold, so "did a snare/clap accent land on this beat?" is answered
  // per beat rather than inferred from energies across bar positions.
  static OnsetDet detMid, detHigh;
  detMid.reset();
  detMid.avgShift  = ACCENT_AVG_SHIFT;
  detMid.marginPct = ACCENT_MARGIN_PCT;
  detMid.refrTicks = FUS_REFR_FRAMES;
  detHigh.reset();
  detHigh.avgShift  = ACCENT_AVG_SHIFT;
  detHigh.marginPct = ACCENT_MARGIN_PCT;
  detHigh.refrTicks = FUS_REFR_FRAMES;

  // Per-beat accent counting and the published pattern.
  uint32_t accMidSinceBeat  = 0; // MID accents since the last accepted onset
  uint32_t accHighSinceBeat = 0; // HIGH accents since the last accepted onset
  uint8_t  accPatIdx = 0;
  uint8_t  accPatN   = 0;
  // One ring per band - they are NOT combined; see the note in Stats.
  // 64-beat history of MID accents, one bit per beat, newest in bit 0.  Used for the
  // two-group contrast - see the note where it is folded in.
  uint64_t accMask      = 0;
  char     accRingMid[ACC_PAT_N]  = {};
  char     accRingHigh[ACC_PAT_N] = {};
  // The STRENGTH rings, one code per beat, same indexing as the two above.  Written
  // from the peak accent strength of each band over that beat's interval - see
  // strenCode for the coding.  These are the measurement the X/. rings cannot make.
  char     margRingLow[ACC_PAT_N]  = {};
  char     margRingMid[ACC_PAT_N]  = {};
  char     margRingHigh[ACC_PAT_N] = {};

  // ---- THE TWO-GROUP DECISION STATE ----
  // NOTE the direction of `justMarked`.  The accent measured over an inter-onset
  // interval belongs to the beat at its START, not its end - MEASURED pp=105 of
  // 255, so the peak sits 41% into the interval.  So the window cannot be fed at
  // mark time (the accent for that beat has not arrived yet); it is fed one onset
  // later, and `justMarked` carries "the previous grid beat was marked" forward to
  // that onset.
  bool     justMarked   = false;  // was the previous GRID BEAT marked?
  uint8_t  parWin       = 0;      // MARKED beats judged in this window
  uint8_t  parAcc       = 0;      // ... of which carried the accent
  bool     parityInvert = false;  // LATCHED: flip which group CH10 marks
  uint32_t parRate      = 0;      // last judged rate, % - for the log
  uint32_t parDec       = 0;      // times the window has been judged
  int8_t   parLast      = -1;     // last verdict: 1 = inverted, 0 = kept

  // The odd/even rule's verdict in ONSET space, and how often the beats CH10 marks
  // agree with it.  THIS decides the remaining design question with no risk to the
  // marker: agree ~100% means wiring the rule keeps the group CH10 already holds,
  // ~0% means it would invert it (the rule's sense is flipped for this material),
  // ~50% means the two are not aligned at all.
  bool     onsetOddRule = false;
  uint32_t ruleWin      = 0;
  uint32_t ruleAgree    = 0;

  // ---- THE SLIP-PROOF MARKER GROUP ------------------------------------------
  // The marker's group is taken from the MUSIC's beat parity, not from the grid's
  // emission count, and then LATCHED.
  //
  // MEASURED - this is the fault behind the section-3 flip: the marker's parity came
  // from beatCount, which counts GRID EMISSIONS.  At sec 352 the reported period went
  // to 10566 (136 BPM) against a 133 BPM track, the grid emitted ONE EXTRA beat,
  // beatCount gained one, and mg went to 9 - ODD, i.e. the marker jumped to the other
  // half-bar.  A grid emission count is not the music's beat count whenever the
  // period is briefly wrong, and the low-band onset gate (+/-25% of period) does not
  // protect the emission count.
  //
  // beatIdx counts ACCEPTED ONSETS - real musical beats, each gated to +/-25% of
  // the period - so a grid that is briefly fast or slow cannot add or drop one.
  // Driving the group from the onset parity therefore cannot slip.
  //
  // `targetParity` is latched at the FIRST mark, which is exactly where the baseline
  // decided its group, so the group you already had is preserved - and from then on
  // it is immune to slips.  'F' still inverts it if the group itself is wrong.
  bool     phaseSettled  = false;  // IO48 is ON the beat (see PHASE_SETTLE_ONSETS)
  uint8_t  phaseRun      = 0;      // consecutive onsets within PHASE_SETTLE_SAMPLES
  bool     onsetParity   = false;  // parity of the most recently accepted onset
  bool     targetLatched = false;  // has the group been latched yet?
  bool     targetParity  = false;  // the onset parity CH10 marks
  uint32_t resyncs       = 0;      // times the alternation was re-aligned to the music
  uint8_t  resyncRun     = 0;      // consecutive onsets the mismatch has held for
  // Times the marker's group was re-latched from a MEASURED phrase boundary - the
  // absolute reference the group otherwise lacks.  See the anchor block.
  uint32_t reLatch       = 0;
  // NOTE: there is deliberately NO one-mark-per-onset gate here.  That was tried and
  // it REGRESSED: the mark is consumed per GRID EMISSION, so a gate keyed to onsets
  // SKIPS marks whenever the two are not exactly 1:1 - CH10 went irregular ("lost the
  // beat") and each skipped mark moved the next one to the other group ("swapped to
  // 2 and 4").  The cadence must stay the grid's; only the GROUP is anchored.

  // Calibration accumulators for the two accent bands' floors.
  uint64_t midCalSq = 0, highCalSq = 0;
  uint32_t calFrameN = 0;

  // ---- THE ANALYSIS STACK STATE (restored) ---------------------------------
  // The envelopes, shared by the Stage 4 anchor, the structural detector and the
  // two gains.  They track the UN-NORMALISED level deliberately: if they tracked
  // the normalised signal the gain would be self-referential.
  int32_t  fastEnv  = 0;     // ~43 ms
  int64_t  shortEnv = 0;     // ~0.7 s
  int64_t  longEnv  = 0;     // ~11 s
  int64_t  longAcc  = 0;     // the long-term average, scaled by 2^LONG_Q
  int32_t  gNorm    = 4096;  // dual-timescale normalisation gain, Q12
  int32_t  gAgc     = 4096;  // AGC expansion gain, Q12

  // The structural / phrase-boundary anchor (Audio.md section 3 and Stage 4).
  bool     structSeen     = false;  // the first event just sets the phrase origin
  bool     anchorArmed    = false;  // force the next beat to be Beat 1
  uint32_t lastStructAt   = 0;      // sample index of the last structural shift
  uint32_t lastStructBeat = 0;      // beat index of the last QUALIFIED boundary
  uint32_t structCnt      = 0;      // structural shifts seen
  uint32_t phraseCnt      = 0;      // QUALIFIED phrase boundaries
  uint32_t phraseGap      = 0;      // the MEASURED phrase period, in beats
  // The measured phrase period: the last gap between structural events, and how many
  // times in a row it has repeated.  See PHRASE_REPEATS.
  uint32_t lastGap        = 0;
  uint8_t  sameGapRun     = 0;
  uint32_t anchorCnt      = 0;      // times the anchor moved Beat 1

  // The two spectral phrase cues.
  int64_t  centAcc   = 0;     // centroid baseline, scaled by 2^CENT_Q
  int32_t  centLong  = 0;
  int32_t  centNow   = 0;
  uint32_t centRun   = 0;
  // NOTE the name: NOT "highAcc" - the accent detector already has a local
  // `const bool highAcc` and shadowing it made this assignment read-only.
  int64_t  highBaseAcc = 0;   // high-band energy baseline, scaled by 2^CENT_Q
  uint32_t crashRun  = 0;
  bool     phraseMarkerPending = false;  // a spectral cue fired, awaiting the hold
  uint32_t centMarks  = 0;
  uint32_t crashMarks = 0;

  // Three-band fusion: the low-band candidate queue and its counters.
  uint32_t pendAt[MAX_PENDING] = {};
  uint8_t  pendN        = 0;
  uint32_t lastAccentAt = 0;
  uint32_t prevOnsetAt  = 0;
  bool     havePrev     = false;
  uint32_t lowCandWin   = 0;   // low-band candidates this window
  uint32_t lowEarlyWin  = 0;   // confirmed early by a mid/high accent
  uint32_t lowValidWin  = 0;   // accepted on expiry (the accent does not veto)
  uint32_t lowRejWin    = 0;   // queue full

  static OnsetDet det;
  det.reset();
  // The LOW band's own tuning - the values this file has always used, now set as
  // members because the same struct also serves the accent bands.
  det.avgShift  = NOV_AVG_SHIFT;
  det.marginPct = NOV_MARGIN_PCT;
  det.refrTicks = REFR_SAMPLES;

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
  // The tight interval gate's own state - see IV_TIGHT_PCT.
  bool     gateWide    = false; // the tight gate widened itself (the tempo moved)
  uint8_t  tightRejRun = 0;     // consecutive tight-gate rejections

  // ==========================================================================
  //  THE GROUP STEER - the music's own two groups (29 Oct 2026)
  // ==========================================================================
  // THE FAULT THIS REPLACES, and it was a real one: the group came from
  // `targetParity`, latched from the ONSET-COUNT parity at the first mark, which
  // includes every spurious detection before the music starts.  Nothing tied it to
  // the track, so one run held 1&3 and the next landed on 2&4.  It was a latch.
  //
  // WHY THE OLD ACCENT STEER COULD NOT WORK: its input was fed at the ONSET FOLD
  // while the mark decision is made at the GRID EMISSION, and those two are not
  // 1:1 (they diverge whenever the grid free-runs, and by an odd amount during a
  // drop).  MEASURED on the Billy Jean capture: the marked positions were strictly
  // every-other in only 2 of 153 16-beat strings - so the statistic that was
  // supposed to steer the group was measuring the wrong beats.  That is very
  // likely why applying it before "turned a marker that started correctly on 1&3
  // into 2&4": it was not the sign that was wrong, the pairing was.
  //
  // THE NEW MECHANISM, which cannot drift: the accent is accumulated PER FRAME
  // into the group of the grid beat CURRENTLY SOUNDING - the group is the grid's
  // own alternation (`beatCount`), so there is no fold/mark pairing to get wrong.
  //   gsMax[g]  strongest MID accent since the last emission, for group g
  //   gsSum/gsN the window's sums, judged every GS_WINDOW beats per group
  //   gsTargetBC  the beatCount value CH10 should mark
  //
  // THE SIGN IS SET BY THE EAR, not assumed - see GS_MARGIN.  `F` does not just
  // flip the marker, it TEACHES the steer: the preferred side of the split is
  // chosen from g.flip, so one keystroke fixes it for the rest of the material and
  // the steer then holds it from the music.  No start-of-track assumption is made
  // anywhere: where the track began is irrelevant to which group is 1&3.
  int32_t  gsMax[2]   = { 0, 0 };   // strongest MID accent since the last emission
  int64_t  gsSum[2]   = { 0, 0 };   // per-group sums over the window
  uint32_t gsN[2]     = { 0, 0 };   // per-group beats in the window
  uint8_t  gsIdx      = 0;          // the group whose beat is sounding now
  uint8_t  gsTargetBC = 1;          // which beatCount value to MARK (1 or 2)
  bool     gsValid    = false;      // the window has filled and the split is real
  int32_t  gsMean[2]  = { 0, 0 };   // last judged means, for the log
  uint8_t  lastFlipSeen = 0;        // to notice 'F' and teach the steer at once
  // Beats PER GROUP before a judgement, and the margin in per cent of the local
  // average.  MEASURED on the Billy Jean tail, where the split is rock steady
  // (`L` byte-identical for 153 s, even-minus-odd gap exactly +2.00 code units,
  // i.e. 100% of the local average): the two groups differ by ~1-2 code units of
  // mid-band accent.  A 100% margin is one full code unit, so it demands a real
  // difference and holds when the arrangement makes the groups symmetrical - an
  // ambiguous reading must never move the marker.
  static const uint32_t GS_WINDOW = 16;
  static const int32_t  GS_MARGIN = 100;
  // NO interval-history ring here.  A mean-of-last-N period was tried and removed:
  // it has to FILL before it is accurate, so the first bars ran at the initial
  // guess - the reported "lagging 2 or 3 bars at the start".  The leaky 1/8
  // adaptation in the grid loop is in time from the first beat.  See the note there.

  // ---- the restored odd/even rule ------------------------------------------
  // PHASE A: this computes and is logged, but NOTHING here drives the marker yet.
  // Restored from baseline\Baseline Professional 2026-10-28\beat_track.ino:
  //   beatIdx / phaseOff  -> the modulo-4 bar counter  (label = ((i - phaseOff)&3)+1)
  //   beatE[4]            -> per-beat energy, keyed by (beatIdx & 3), which is
  //                          PHASE-INDEPENDENT, so a phase move cannot corrupt the
  //                          accumulator - its argmin directly names the position.
  //   eSum / eBars / mean -> per-position means over many bars, each bar NORMALISED
  //                          by its own peak so a loud bar cannot dominate.
  uint32_t beatIdx  = 0;     // running beat index, never reset
  uint32_t phaseOff = 0;     // label of beat i = ((i - phaseOff) & 3) + 1
  int32_t  beatEMid[4] = { 0, 0, 0, 0 };
  int32_t  beatELow[4] = { 0, 0, 0, 0 };
  // The SLIDING WINDOW of the last BAR_HIST bars, each normalised by its own peak.
  // See the note at BAR_HIST: a cumulative average was measured to destroy the cue
  // (spread 211 -> 16, ErM 142% -> 100%).
  int32_t  barHistMid[BAR_HIST][4] = {};
  int32_t  barHistLow[BAR_HIST][4] = {};
  uint8_t  barIdx = 0, barN = 0;
  // Hysteresis state - see DB_CONFIRM_BARS.  dbCand is the POSITION index
  // (phase-independent), dbCandN how many consecutive bars it has held.
  uint8_t  dbCand = 0, dbCandN = 0;

  int64_t  winStart = esp_timer_get_time();
  uint32_t winN = 0;
  uint32_t stall = 0;
  int64_t  pdAcc = 0;      // signed onset-to-grid offset accumulator
  uint32_t pdN   = 0;
  int32_t  midPkWin = 0;   // peak mid flux this second - the mid band's liveness

  // ---- WHY THE GRID MOVES (added 29 Oct 2026) -------------------------------
  // The last 8 ACCEPTED intervals and the last 8 signed dd values.  Together they
  // separate a period BIAS (steady, one sign) from a per-group detector LATENCY
  // (alternating), which is what decides where the fix belongs.
  int64_t  ivRing[8] = {};
  uint8_t  ivIdx     = 0;
  uint8_t  ivCnt     = 0;
  int32_t  ddRing[8] = {};
  uint8_t  ddIdx     = 0;
  uint8_t  ddCnt     = 0;
  // The UNCLAMPED phase error, and how often the bound was hit.  pdMs saturates
  // at the bound and hides anything larger; these do not.
  int64_t  pdRawAcc   = 0;
  uint32_t pdClampWin = 0;
  uint32_t per4Win    = 0;
  uint32_t per8Win    = 0;
  // Strongest accent this second, per band - the exact numbers behind the codes.
  int32_t  maxRatLowW = 0, maxRatMidW = 0, maxRatHighW = 0;

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

      // ---- THE ENVELOPES (restored) -----------------------------------------
      // Three timescales.  Shared by the Stage 4 anchor, the structural detector
      // and the two gains, because they all need the same three views of the level.
      const int32_t magRaw = (b < 0) ? -b : b;
      fastEnv  += (magRaw - fastEnv)  >> FAST_AVG_SHIFT;
      // shortEnv is held SCALED by 2^DC_Q, for exactly the reason the LONG_Q note
      // above gives.  MEASURED on this board: written unscaled as
      // "shortEnv += (fastEnv - shortEnv) >> 14" with fastEnv ~12,000, the
      // increment floors to ZERO (12,000 >> 14 == 0), so shortEnv sat at 0 for the
      // whole run while fastEnv read 12,115.  Every ratio built on it was
      // meaningless, and the structural detector's "short < 50% of long" test
      // would have been permanently TRUE had the level gate not been there.
      shortEnv += (((int64_t)fastEnv << DC_Q) - shortEnv) >> SHORT_AVG_SHIFT;
      longAcc  += (((int64_t)fastEnv << LONG_Q) - longAcc) >> LONG_AVG_SHIFT;
      longEnv   = longAcc >> LONG_Q;
      const int32_t shortNow = (int32_t)(shortEnv >> DC_Q);

      // ---- THE STRUCTURAL DETECTOR (Audio.md section 3, restored) ------------
      // "structural drop-outs, vocal entries, ... sudden spectral centroid
      // shifts" - a drop-out or a rise relative to the long-term level is what is
      // detectable from energy alone, and it is what re-aligns Bar 1 to the
      // phrase boundary.  Only an event on the document's 16/32-beat grid is
      // QUALIFIED as a phrase boundary; the rest are recorded but do NOT move the
      // phase, so the anchor fails SAFE rather than thrashing.
      if (longEnv > NORM_MIN_E) {                    // only when there is level
        const int32_t sPct = (int32_t)(((int64_t)shortNow * 100) / longEnv);
        if ((sPct < (int32_t)STRUCT_DROP_PCT || sPct > (int32_t)STRUCT_RISE_PCT) &&
            (((uint32_t)absN - lastStructAt) > STRUCT_HOLD_SAMPLES)) {
          lastStructAt = (uint32_t)absN;
          structCnt++;
          if (!structSeen) {
            structSeen     = true;      // the first event just sets the origin
            lastStructBeat = beatIdx;
          } else {
            const uint32_t gap = beatIdx - lastStructBeat;
            lastStructBeat = beatIdx;   // gaps are between CONSECUTIVE events
            // MEASURED PHRASE PERIOD - see PHRASE_REPEATS.  The same gap recurring
            // IS the period.  The fixed 16/32 test measured phr=0 for a whole track
            // with str=11, because this material's phrases are about 20 beats, so
            // 16+/-2 and 32+/-2 both missed however long it ran.
            if (gap >= PHRASE_MIN_BEATS && gap <= PHRASE_MAX_BEATS) {
              if (gap == lastGap) {
                if (sameGapRun < 255) sameGapRun++;
              } else {
                lastGap    = gap;
                sameGapRun = 1;
              }
              if (sameGapRun >= PHRASE_REPEATS) {
                phraseGap   = gap;
                phraseCnt++;
                anchorArmed = true;     // the NEXT beat becomes Beat 1
              }
            } else {
              // An implausible gap - a frozen beat index, or two events on one beat.
              // It is not a phrase period and must not accumulate towards one.
              lastGap    = 0;
              sameGapRun = 0;
            }
          }
        }
      }

      // ---- THE DUAL-TIMESCALE NORMALISER GAIN (Audio.md supplemental 4) ------
      // "Normalize incoming energy relative to the long-term average so the
      //  algorithm behaves identically during a quiet intro and a wall-of-sound
      //  drop without manual volume adjustment."
      // gNorm = NORM_REF_E / longEnv, clamped.  This was the ONE part of the
      // restored stack that had been left as a constant 1.0 (4096) - the state
      // variable existed and the multiply existed, but nothing ever computed it,
      // so the pipeline was NOT normalised despite looking as though it was.  The
      // clamp matters: MEASURED in beat_track, without it a silent passage drove
      // the gain to the ceiling and the next transient saturated.
      gNorm = 4096;
      if (longEnv > NORM_MIN_E) {
        int64_t gn = ((int64_t)NORM_REF_E << 12) / longEnv;
        if (gn < NORM_GAIN_MIN) gn = NORM_GAIN_MIN;
        if (gn > NORM_GAIN_MAX) gn = NORM_GAIN_MAX;
        gNorm = (int32_t)gn;
      }

      // ---- THE AGC EXPANDER (Audio.md section 1, restored) -------------------
      // Gain rises when the fast envelope exceeds the ~0.7 s reference, which
      // re-expands the transients the MAX9814's AGC compressed.  It works on a
      // FAST timescale while the normaliser works on the LONG one, so the two are
      // complementary rather than cancelling.
      gAgc = 4096;
      if (shortNow > NORM_MIN_E) {
        const int32_t ex = (int32_t)(fastEnv - shortNow);
        if (ex > 0) {
          gAgc = 4096 + (int32_t)((((int64_t)ex * AGC_K) << 12) / shortNow);
          if (gAgc > AGC_GAIN_MAX) gAgc = AGC_GAIN_MAX;
        }
      }

      // ---- THE DUAL-TIMESCALE NORMALISER and AGC: NOT in the beat path --------
      // REGRESSION, measured on the track: with the gains multiplied into the
      // detection signal the beat ran "way too fast", because a 1.0-4.0x gain
      // raises every transient above the onset threshold and the detector then
      // fires many times per beat.  The period follows those extra intervals and
      // collapses toward PERIOD_MIN - which is exactly "flashing nowhere near in
      // time with the music".
      //
      // The professional baseline states the rule in its own comment about the
      // additive bands: "Purely ADDITIVE ... it cannot alter the beat path in any
      // way."  The same rule applies to these two gains.  The novelty curve is
      // therefore taken from the raw band output, UNCHANGED from the build that
      // tracked 1 & 3 all the way through the track.  The gains are still computed
      // and still published on the `an` line, so their behaviour is measurable -
      // but they are applied to the ANALYSIS only, below, never to the signal that
      // decides when a beat happened.
      // ---- rectified first-order difference ----
      // RESTORED TO THE BASELINE VERBATIM.  This line is the beat path's first
      // stage and the build that tracked 1 & 3 all the way through the track used
      // exactly this.  Any gain inserted here changes WHICH transients clear the
      // onset threshold, and therefore when a beat happens - so it is not an
      // additive change and does not belong here.
      const int32_t mag = (b < 0) ? -b : b;
      int32_t nov = mag - lastMag;
      lastMag = mag;
      if (nov < 0) nov = 0;
      g.novNow = nov;

      // ---- MID and HIGH bands: accumulate the Goertzel bins -----------------
      // Taps acQ, which is BEFORE the low-band biquads, so this cannot alter the
      // beat path in any way.
      for (uint8_t k = 0; k < MID_NBINS; k++)  gMid[k].push(acQ >> MID_PUSH_SHIFT);
      for (uint8_t k = 0; k < HIGH_NBINS; k++) gHigh[k].push(acQ >> MID_PUSH_SHIFT);

      // Close a 10 ms frame: the band energy is the sum of its bin magnitudes, and
      // the flux is the POSITIVE frame-to-frame change, exactly as the
      // professional baseline computes it (Audio.md supplemental section 1).
      if (++midFrameN >= FUS_FRAME_SAMPLES) {
        midFrameN = 0;
        // Band energies: the sum of that band's bin magnitudes.  The spectral
        // centroid (restored) is accumulated in the SAME pass, because each bin is
        // read once and then reset - reading them again afterwards returns zero.
        int64_t midE = 0, highE = 0;
        int64_t cNum = 0, cDen = 0;
        for (uint8_t k = 0; k < MID_NBINS; k++) {
          const int64_t m = gMid[k].magnitude() >> BIN_SHIFT;
          midE += m;
          cNum += (int64_t)MID_HZ[k] * m;
          cDen += m;
          gMid[k].reset();
        }
        for (uint8_t k = 0; k < HIGH_NBINS; k++) {
          const int64_t m = gHigh[k].magnitude() >> BIN_SHIFT;
          highE += m;
          cNum += (int64_t)HIGH_HZ[k] * m;
          cDen += m;
          gHigh[k].reset();
        }
        // The magnitude-weighted mean frequency across the mid + high bands.
        const int32_t centroid = (cDen > 0) ? (int32_t)(cNum / cDen) : centLong;
        centNow = centroid;
        int32_t midFlux = (int32_t)(midE - midEnergyPrev);
        midEnergyPrev = midE;
        if (midFlux < 0) midFlux = 0;
        int32_t highFlux = (int32_t)(highE - highEnergyPrev);
        highEnergyPrev = highE;
        if (highFlux < 0) highFlux = 0;

        // The beat's mid-band energy is the PEAK flux in the interval BEFORE it -
        // the same attribution the low band uses, so the two are comparable.
        if (midFlux > midPeakSinceBeat) {
          midPeakSinceBeat = midFlux;
          midPeakAt = (uint32_t)absN;     // for pp=, see Stats
        }
        if (midFlux > midPkWin) midPkWin = midFlux;   // liveness, see Stats

        // ---- THE ACCENT DETECTORS: the two-group measurement -----------------
        // Each band answers independently, per 10 ms frame: did an accent happen?
        // The counts are accumulated per BEAT below and published as a pattern -
        // that pattern is what shows whether the two groups differ.
        const bool midAcc  = detMid.run(midFlux,  armed);
        const bool highAcc = detHigh.run(highFlux, armed);
        if (midAcc)  { accMidSinceBeat++;  g.accMid  = g.accMid  + 1; }
        if (highAcc) { accHighSinceBeat++; g.accHigh = g.accHigh + 1; }
        // ACCENT STRENGTH, the per-second maximum, for the `mx=` field.  The
        // per-BEAT strength is read at the beat fold below; this is the headline.
        //
        // GATED ON REAL SIGNAL - the same gate the phrase cues use - and the gate
        // is NOT optional here.  MEASURED on the first run of this instrument, with
        // NO music playing: mx=4700/3633.  The mid/high calibrated floors sit near
        // zero, so the floor test inside run() cannot catch this: an isolated noise
        // spike stands thousands of per cent above a near-zero local average, and
        // the number is meaningless.  The per-BEAT strings below are immune, because
        // a beat only ever folds while the music is playing.
        //
        // THE LOW BAND (det) is included here rather than in the sample loop: on
        // this material the kick is the cue that sits on a beat GROUP rather than on
        // every beat, so it is the most promising one, and it deserves the same gate.
        if (longEnv > NORM_MIN_E) {
          if (det.ratPct      > maxRatLowW)  maxRatLowW  = det.ratPct;
          if (detMid.ratPct   > maxRatMidW)  maxRatMidW  = detMid.ratPct;
          if (detHigh.ratPct  > maxRatHighW) maxRatHighW = detHigh.ratPct;

          // ---- THE GROUP ACCUMULATOR (29 Oct 2026) -------------------------
          // The accent is credited to the group of the grid beat CURRENTLY
          // SOUNDING, taken straight from the grid's own alternation.  Nothing
          // here depends on the fold/mark pairing, which is what made the old
          // statistic measure the wrong beats - see the note at the gs state.
          // The MID band is used because it carries the largest RELIABLE split
          // measured on this material (even-minus-odd gap +3.79 code units in the
          // steady tail, against +0.80 in the low band).
          const int32_t ga = detMid.avgNow;
          if (ga > 0 && ga >= detMid.floor) {
            const int32_t gr = (int32_t)(((int64_t)midFlux * 100) / (int64_t)ga);
            if (gr > gsMax[gsIdx]) gsMax[gsIdx] = gr;
          }
        }

        // ---- Audio.md section 3: the two spectral phrase cues (restored) -----
        // Gated on real signal: MEASURED in beat_track, without the gate the cues
        // fired 5 and 1 times during SILENCE, because the centroid of room noise
        // wanders as much as a musical phrase change does.  The baselines also
        // only adapt while there is level, so silence cannot drag them.
        if (longEnv > NORM_MIN_E) {
          centAcc += (((int64_t)centroid << CENT_Q) - centAcc) >> CENT_AVG_SHIFT;
          centLong = (int32_t)(centAcc >> CENT_Q);
          highBaseAcc += (((int64_t)highE << CENT_Q) - highBaseAcc) >> HIGH_AVG_SHIFT;
          const int64_t highLong = highBaseAcc >> CENT_Q;

          // 1. "sudden spectral centroid shifts" - a vocal entry or a crash moves
          //    the balance between the mid and high bands and HOLDS it there.
          const int32_t cShift = centroid - centLong;
          const int32_t cAbs   = (cShift < 0) ? -cShift : cShift;
          if (centLong > 0 &&
              (int64_t)cAbs * 100 >= (int64_t)CENT_SHIFT_PCT * (int64_t)centLong) {
            centRun++;
          } else {
            centRun = 0;
          }
          if (centRun == CENT_HOLD_FRAMES) {
            centMarks++;
            phraseMarkerPending = true;
          }

          // 2. "cymbal crashes on downbeats" - detected by DECAY, which is what
          //    separates a crash from a hi-hat, not by level.
          if (highLong > 0 &&
              (int64_t)highE * 100 >= (int64_t)CRASH_RISE_PCT * highLong) {
            crashRun++;
          } else {
            crashRun = 0;
          }
          if (crashRun == CRASH_HOLD_FRAMES) {
            crashMarks++;
            phraseMarkerPending = true;
          }
        }

        // ---- THREE-BAND FUSION (Audio.md supplemental 1, restored) -----------
        // "a beat is only validated when a low-band transient coincides with or
        // closely precedes a mid-band (snare) or high-band accent."
        //
        // RESTORED AS A MEASUREMENT, NOT AS A GATE ON THE BEAT PATH, and that is
        // deliberate: beat_track.ino measured that the accent must CONFIRM and not
        // VETO - a four-on-the-floor kick is on every beat while the snare is only
        // on 2 and 4, so vetoing discarded real kicks and halved the onset rate,
        // which made the tempo lock read half tempo.  A candidate whose accent
        // window expires is therefore ACCEPTED.  The net effect is confirmation,
        // and routing the beat path through a 0-40 ms queue would add that much
        // jitter to timing that is measured working.  The counters below give the
        // fusion's full behaviour readout, and the candidate queue is maintained
        // exactly as the document describes.
        uint8_t kp = 0;
        while (kp < pendN) {
          if (((uint32_t)absN - pendAt[kp]) > COINC_SAMPLES) {
            pendAt[kp] = pendAt[--pendN];
            lowValidWin++;                     // accepted on expiry
          } else {
            kp++;
          }
        }
        if (midAcc || highAcc) {
          lastAccentAt = (uint32_t)absN;
          lowEarlyWin += pendN;                // confirmed early by an accent
          pendN = 0;
        }

        // Floors for the two accent bands, measured over the same silent
        // calibration the low band uses.  The professional baseline did exactly
        // this (its lines 1802-1808).  Without a floor the warm-up transient
        // fires the detector immediately.
        if (!calDone) {
          midCalSq  += (uint64_t)midFlux  * (uint64_t)midFlux;
          highCalSq += (uint64_t)highFlux * (uint64_t)highFlux;
          calFrameN++;
        }
      }
      // The LOW band's per-beat peak, kept only as the CONTROL for the ratio log.
      if (nov > lowPeakSinceBeat) lowPeakSinceBeat = nov;

      // ---- mandatory silent calibration: measure the room, then arm ----
      if (!calDone) {
        calNovSq += (uint64_t)nov * (uint64_t)nov;
        calN++;
        if (calN >= CAL_SAMPLES) {
          const uint32_t rms = isqrt64(calNovSq / (uint64_t)calN);
          det.floor = (int32_t)(rms * NOV_FLOOR_MUL);
          if (det.floor < 1) det.floor = 1;
          // The accent bands get their own floors, measured over the same silent
          // window, so their detectors are calibrated to the room too.
          if (calFrameN > 0) {
            const uint32_t mSig = isqrt64(midCalSq / (uint64_t)calFrameN);
            const uint32_t hSig = isqrt64(highCalSq / (uint64_t)calFrameN);
            detMid.floor  = (int32_t)(mSig * NOV_FLOOR_MUL);
            if (detMid.floor  < 1) detMid.floor  = 1;
            detHigh.floor = (int32_t)(hSig * NOV_FLOOR_MUL);
            if (detHigh.floor < 1) detHigh.floor = 1;
          }
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

        // Three-band fusion: queue this low-band candidate so the frame loop can
        // match it against a mid/high accent.  The queue is drained and counted in
        // the frame loop; it deliberately does NOT gate the beat path - see the
        // note there for why the accent must confirm and not veto.
        if (pendN < MAX_PENDING) {
          pendAt[pendN++] = (uint32_t)absN;
          lowCandWin++;
        } else {
          lowRejWin++;                 // queue full
        }

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

          // ---- THE TWO-TIER INTERVAL GATE (29 Oct 2026) ----------------------
          // +/-25% while ACQUIRING, +/-12% once the phase is SETTLED.  See the long
          // note at IV_TIGHT_PCT: a flam or ghost note 10-20% early was being
          // accepted as a beat, which shortened the period and threw the grid
          // AHEAD of the music until the pull walked it back.  That was the
          // "IO48 jumped ahead for a moment" the user reported.
          const bool    tight = (phaseSettled && !gateWide);
          const int32_t loPct = tight ? (100 - IV_TIGHT_PCT) : IV_LO_PCT;
          const int32_t hiPct = tight ? (100 + IV_TIGHT_PCT) : IV_HI_PCT;
          const int64_t lo    = (period * loPct) / 100;
          const int64_t hi    = (period * hiPct) / 100;
          if (iv >= lo && iv <= hi) {
            tightRejRun = 0;      // this onset was a beat; the gate is calibrated
            gateWide    = false;
            // ---- PERIOD: the BASELINE's leaky adaptation, restored verbatim ----
            // This is the line the build that kept time used:
            //     period += (iv - period) / PERIOD_DAMP;
            // The mean-of-last-N version that briefly replaced it was MY idea to
            // chase a drift, and it was wrong to apply it here: an 8-slot mean has
            // to FILL before it is accurate, so the first bars run at whatever the
            // initial guess was - the "lagging 2 or 3 bars at the start, then
            // catching up" that was reported.  The baseline's version moves 1/8 of
            // the way toward each interval from the very first beat, so it is in
            // time immediately.  Keeping time was never the broken part.
            // ---- PERIOD: THE BAR SPAN, NOT A SINGLE INTERVAL (29 Oct 2026) -----
            // MEASURED on the Billy Jean baseline: `per` wandered 11618-12425, a
            // 6.57% spread, on a drum machine that cannot actually change tempo.
            // THAT wander is the entire fault, because the phase loop AMPLIFIES it.
            // In steady state the grid advances by period + dd/PHASE_DAMP per beat
            // and the music by T, so dd settles at PHASE_DAMP x (T - period).  With
            // both gains at 8, +/-16 ms of period wander became the +/-128 ms of
            // PHASE error seen in the log - exactly the bound - and the grid visibly
            // ran AHEAD of the beat, worst in the last third (mean pd +80 ms against
            // -2 ms in the first).  The phase loop CANNOT fix this: it is a SLOPE
            // error, and no amount of pull removes a wrong slope.
            //
            // WHY ONE INTERVAL IS NOISY HERE: an interval is measured between two
            // DETECTED onsets, and the detector's latency is NOT the same for every
            // beat - a kick attack and a snare attack cross the threshold at
            // different points.  On this material those groups alternate every beat,
            // so the intervals alternate short/long, and a 1/8 leaky average settles
            // wherever the alternation happens to be rather than on its middle.
            //
            // THE FIX: average over a whole BAR.  Across 4 beats each group appears
            // an equal number of times, so the per-group latency CANCELS EXACTLY.
            // The leaky adaptation still runs FIRST, so the first beats after a lock
            // use a sane period immediately - the earlier mean-of-N version regressed
            // precisely because it had to FILL and ran on the seed instead.
            ivRing[ivIdx] = iv;
            ivIdx = (uint8_t)((ivIdx + 1u) & 7u);
            if (ivCnt < 8) ivCnt++;

            period += (iv - period) / PERIOD_DAMP;     // seed / fill, and fallback

            int64_t s4 = 0, s8 = 0;
            if (ivCnt >= 4) {
              for (uint8_t i = 0; i < 4; i++)
                s4 += ivRing[(uint8_t)((ivIdx + 7u - i) & 7u)];
            }
            if (ivCnt >= 8) {
              for (uint8_t i = 0; i < 8; i++) s8 += ivRing[i];
            }
            // ---- THE SPAN IS NOW EIGHT BEATS, ON THE MEASUREMENT ----------------
            // The user's rule was "start on 4 and correct to 8 if necessary".  The
            // Billy Jean run made that call measurable - the period's standard
            // deviation was:
            //     single interval  8.27 ms
            //     4 beats (1 bar)  7.30 ms
            //     8 beats (2 bars) 5.20 ms   <== the steadiest
            // so 4 was a real gain but 8 is better again (-37% against the single
            // interval).  It also SURVIVES one bad interval outright, because a flam
            // corrupts two of the eight and the other six still carry the estimate.
            // FOUR is kept as the fallback while the ring fills, so the first bar is
            // never worse than the version that measured 4, and the leaky value
            // stands below that - so the run starts in time as it always did.
            if      (ivCnt >= 8) period = s8 / 8;
            else if (ivCnt >= 4) period = s4 / 4;

            if (period < (int64_t)PERIOD_MIN) period = (int64_t)PERIOD_MIN;
            if (period > (int64_t)PERIOD_MAX) period = (int64_t)PERIOD_MAX;

            // Published AFTER the clamp, so per4 is the period actually in use.
            per4Win = (ivCnt >= 4) ? (uint32_t)(s4 / 4) : 0u;
            per8Win = (ivCnt >= 8) ? (uint32_t)(s8 / 8) : 0u;

            // ---- THE STRUCTURAL ANCHOR: make THIS beat Beat 1 -----------------
            // A QUALIFIED phrase boundary fired (a 16/32-beat-aligned structural
            // shift - see the structural detector in the sample loop), so this
            // beat IS the downbeat.  Set the modulo-4 offset so this beat's label
            // reads 1.  This is the ONLY cue that can fix the parity across a
            // quiet section, because the bar phase is simply not present in the
            // audio during the gap - the phase has to be taken from the structure
            // when the music comes back.  It is applied here, on an ACCEPTED
            // onset, so it lands on a real beat and not on the grid's guess.
            if (anchorArmed) {
              anchorArmed = false;
              const uint32_t newOff = beatIdx;
              if (newOff != phaseOff) {
                phaseOff = newOff;
                anchorCnt++;
                g.anchorMoves = g.anchorMoves + 1;
              }
              // ---- THE ABSOLUTE REFERENCE FOR THE GROUP -----------------------
              // This beat is a measured PHRASE BOUNDARY, so by Audio.md Stage 4 it
              // IS Beat 1.  The label is ((beatIdx - phaseOff) & 3) + 1, so the beats
              // labelled 1 or 3 are exactly those whose parity equals (phaseOff & 1)
              // - and those are the beats CH10 must mark.
              //
              // WHY THIS MATTERS: until now the group had NO absolute reference.  The
              // latch just took the parity of the first marked beat, which is a coin
              // flip.  MEASURED: section 1 latched 2-and-4 and needed a slow resync to
              // correct it, while sections 2 and 3 happened to latch correctly.  The
              // phrase boundary is the document's answer to precisely this, and it is
              // the only cue that carries an ABSOLUTE bar phase rather than a relative
              // alternation.
              targetLatched = true;
              targetParity  = ((phaseOff & 1u) != 0u);
              reLatch++;
            }

            int64_t dd = ((int64_t)absN - nextBeat) % period;
            if (dd >  period / 2) dd -= period;
            if (dd < -period / 2) dd += period;
            // The UNCLAMPED error, kept BEFORE the bound below is applied.  pdMs is
            // the mean AFTER the bound, so it saturates at +/-period/4 and hides any
            // larger error - MEASURED: 14 of 115 seconds read exactly at the bound on
            // the Billy Jean baseline.  This is the honest phase error; see pdRawMs.
            const int64_t ddRaw = dd;
            // BOUND THE CORRECTION so the grid can never jump onto the
            // NEIGHBOURING beat.
            // MEASURED: dd is bounded to +/-half a period by the modulo above, so
            // a phase error larger than half a beat wraps and the pull then
            // corrects toward the NEAREST grid point - which is the neighbouring
            // beat.  That is a whole-beat slip, and it is INVISIBLE to both
            // diagnostics: the grid still lands on a beat (pd reads ~0) and the
            // marks are still 2 apart (mg reads 2).  It is exactly what flips the
            // 1-and-3 parity across a quiet section.
            // Limiting the correction to a quarter period means the pull can only
            // nudge the grid WITHIN a beat.  It still converges, but it can never
            // cross a beat boundary, so the parity cannot flip.
            const int64_t lim = period / 4;
            if (dd >  lim) dd =  lim;
            if (dd < -lim) dd = -lim;
            // Servo saturation, COUNTED rather than inferred: every sample that had
            // to be trimmed is one the pull could not absorb.  Non-zero here with the
            // period steady means the gain is too slow; non-zero with the period
            // wandering means the period is still wrong and the gain cannot save it.
            if (dd != ddRaw) pdClampWin++;
            pdRawAcc += ddRaw;
            // The last 8 raw errors in ms.  ALTERNATING values prove a per-group
            // detector latency; a steady one-sided string proves a period bias.
            ddRing[ddIdx] = (int32_t)((ddRaw * 1000) / (int64_t)TARGET_FS);
            ddIdx = (uint8_t)((ddIdx + 1u) & 7u);
            if (ddCnt < 8) ddCnt++;

            // ---- PHASE ACQUISITION: fast while acquiring, damped once settled ---
            // The quarter-period bound is kept in BOTH modes, so the grid can never
            // jump onto the neighbouring beat - the slip protection is untouched.
            // What changes is the damping: UNDAMPED while acquiring, so a half-beat
            // seed error is walked in two onsets instead of 16-32 (4-8 bars).
            if (phaseSettled) {
              nextBeat += dd / PHASE_DAMP;
            } else {
              nextBeat += dd;                 // full correction, still <= period/4
            }

            // Have the last PHASE_SETTLE_ONSETS onsets all landed ON the grid?
            // This is the "IO48 is actually on the beat" test, and the group latch
            // waits for it - see PHASE ACQUISITION.
            if (dd <= PHASE_SETTLE_SAMPLES && dd >= -PHASE_SETTLE_SAMPLES) {
              if (phaseRun < 255) phaseRun++;
            } else {
              phaseRun = 0;
            }
            if (phaseRun >= PHASE_SETTLE_ONSETS) phaseSettled = true;  // once, and stays

            // pd diagnostic: how far the grid is from the music.  Averaged over
            // the second so it reads as one number in the log.
            pdAcc += dd;
            pdN++;
            if (confirmCnt < 1000) confirmCnt++;

            // beatCount is deliberately NOT advanced here.  It was moved here on
            // 28 Oct to stop a fast grid sliding the marker's half-bar, and that
            // REGRESSED: markNow is still evaluated once per GRID EMISSION, so when
            // the grid emitted two beats inside one onset interval both read the
            // same beatCount and BOTH were marked - CH10 flashed on consecutive
            // beats.  MEASURED: marks/beats fell to 0.38-0.51 with mg=1,3,6,8,13.
            // The working baseline counted grid emissions and held mg=2 all track.
            // Reverted - see the note at the grid loop.  The slip it was meant to
            // cure is a separate, smaller problem than a marker that fires wrongly.

            // pp= diagnostic: WHERE in the interval since the PREVIOUS onset the
            // mid-band peak fell, as 0..255.  Near 0 means the peak is right after
            // the previous onset - i.e. it belongs to THAT beat, and attributing it
            // to this one is a systematic one-beat offset.  Near 255 means the
            // attribution below is correct as written.  See the note in Stats.
            {
              const int64_t span = (int64_t)absN - lastOnset;
              if (span > 0 && (int64_t)midPeakAt >= lastOnset) {
                int64_t pos = (int64_t)midPeakAt - lastOnset;
                if (pos > span) pos = span;
                ppAcc += (uint64_t)((pos * 255) / span);
                ppN++;
                g.ePp = (uint32_t)(ppAcc / (uint64_t)ppN);
              }
            }

            // ---- THE ODD/EVEN CAPTURE, COUNTED FROM THE MUSIC ---------------
            // This block used to sit in the grid-emission loop, advancing on GRID
            // beats.  THAT was the bug, and the user's ear found it before the log
            // did: "tempo was too fast at the beginning but was on 1 and 3
            // initially, then due to incorrect fast tempo caught up with 2 and 4
            // and settled there."
            //
            // MEASURED in that run: the period started at per=10512 (136 BPM)
            // against a 133 BPM track - 2.3% fast - and held for ~24 s.  At 2.2
            // beats/s that is ~53 beats, and 2.3% of 53 is ~1.2 beats.  So the grid
            // emitted ONE MORE beat than the music, beatIdx advanced one extra time,
            // and from then on (beatIdx & 3) - the BAR POSITION - was permanently
            // one out.  That is a parity flip no phase correction can undo, because
            // the PHASE is correct and only the COUNT is wrong.  It is invisible to
            // both diagnostics: pd reads ~0 (the grid is on a beat) and mg reads 2
            // (the marks are still two apart).
            //
            // Counting ACCEPTED ONSETS ties the bar position to the MUSIC instead of
            // to the grid's emission count.  An accepted onset is a real beat, gated
            // to +/-25% of the period, so the position advances exactly when the
            // music does.  A grid that is briefly fast or slow can no longer add or
            // drop a position.
            {
              beatEMid[beatIdx & 3] = midPeakSinceBeat;
              beatELow[beatIdx & 3] = lowPeakSinceBeat;
              midPeakSinceBeat = 0;
              lowPeakSinceBeat = 0;

              // A bar is complete once four beats have been recorded.
              if ((beatIdx & 3) == 3) {
                int32_t barMaxMid = 0, barMaxLow = 0;
                for (uint8_t j = 0; j < 4; j++) {
                  if (beatEMid[j] > barMaxMid) barMaxMid = beatEMid[j];
                  if (beatELow[j] > barMaxLow) barMaxLow = beatELow[j];
                }
                // Normalise each bar by its OWN peak, so one loud bar cannot
                // dominate the window.  The professional baseline measured the
                // start-of-track transient at ~26x a normal bar.
                for (uint8_t j = 0; j < 4; j++) {
                  barHistMid[barIdx][j] = (barMaxMid > 0)
                      ? (int32_t)(((int64_t)beatEMid[j] * 1000) / barMaxMid) : 0;
                  barHistLow[barIdx][j] = (barMaxLow > 0)
                      ? (int32_t)(((int64_t)beatELow[j] * 1000) / barMaxLow) : 0;
                }
                barIdx = (uint8_t)((barIdx + 1) % BAR_HIST);
                if (barN < BAR_HIST) barN++;
                // The means over the WINDOW ONLY.  MEASURED: a cumulative mean let
                // the four positions converge to equality (spread 211 -> 16) and the
                // cue decayed 142% -> 100% - see the note at BAR_HIST.
                int64_t meanMid[4] = { 0, 0, 0, 0 };
                int64_t meanLow[4] = { 0, 0, 0, 0 };
                for (uint8_t b = 0; b < barN; b++) {
                  for (uint8_t j = 0; j < 4; j++) {
                    meanMid[j] += barHistMid[b][j];
                    meanLow[j] += barHistLow[b][j];
                  }
                }
                for (uint8_t j = 0; j < 4; j++) {
                  meanMid[j] /= (int64_t)barN;
                  meanLow[j] /= (int64_t)barN;
                }

                // SENSE INVERTED from the professional baseline: the snare is on 2
                // and 4, so the position with the LOWEST mid energy is beat 1.  The
                // professional rule took the HIGHEST, which with the mid band would
                // land on a snare beat and give the wrong parity.
                uint8_t k = 0;
                for (uint8_t j = 1; j < 4; j++) if (meanMid[j] < meanMid[k]) k = j;
                int64_t second = (int64_t)1 << 62;
                for (uint8_t j = 0; j < 4; j++)
                  if (j != k && meanMid[j] < second) second = meanMid[j];

                // The LOW band's control, same inverted form.
                uint8_t kl = 0;
                for (uint8_t j = 1; j < 4; j++) if (meanLow[j] < meanLow[kl]) kl = j;
                int64_t secondL = (int64_t)1 << 62;
                for (uint8_t j = 0; j < 4; j++)
                  if (j != kl && meanLow[j] < secondL) secondL = meanLow[j];

                // Publish in LABEL order, using the CURRENT phase.
                for (uint8_t j = 0; j < 4; j++)
                  g.eMeanMid[j] = (int32_t)meanMid[(phaseOff + j) & 3];
                g.eRatioMid = (meanMid[k] > 0)
                              ? (uint32_t)((second * 100) / meanMid[k]) : 0;
                g.eRatioLow = (meanLow[kl] > 0)
                              ? (uint32_t)((secondL * 100) / meanLow[kl]) : 0;
                g.eBars = barN;               // the WINDOW count, not a track total
                g.eWin  = (uint32_t)(((beatIdx - 3 + k - phaseOff) & 3) + 1);
                g.eActed = (barN >= DOWNBEAT_MIN_BARS) ? 1 : 0;

                // The verdict the rule WOULD act on.  STILL DIAGNOSTIC: the marker
                // is driven by beatCount, not by this.  With no cue the margin never
                // clears and this never fires - the fail-safe the professional rule
                // had.
                const bool strong = (barN >= DOWNBEAT_MIN_BARS && meanMid[k] > 0 &&
                                     second * 100 >=
                                         (int64_t)DOWNBEAT_MARGIN_PCT * meanMid[k]);
                if (strong) {
                  // Hysteresis: the SAME position must hold, consecutively.
                  if (k == dbCand) {
                    if (dbCandN < 255) dbCandN++;
                  } else {
                    dbCand  = k;
                    dbCandN = 1;
                  }
                  if (dbCandN >= DB_CONFIRM_BARS) {
                    const uint32_t newOff = beatIdx - 3 + k;
                    if (newOff != phaseOff) {
                      phaseOff = newOff;
                      g.eAnchors = g.eAnchors + 1;
                    }
                    dbCandN = 0;  // re-arm: a further move needs fresh evidence
                  }
                } else {
                  dbCandN = 0;    // the evidence lapsed: start confirming again
                }
                g.eCnf = dbCandN;
                g.ePhaseOff = (uint32_t)(((beatIdx - phaseOff) & 3));
              }
              beatIdx++;
            }

            // ---- THE ODD/EVEN RULE'S VERDICT, IN ONSET SPACE -------------------
            // The label of the beat just counted, and therefore whether it is one of
            // the beats CH10 should mark.  This is evaluated where beatIdx and
            // phaseOff are both final for this beat, so it needs no mapping between
            // the grid's beat counter and the music's beat index - which is what
            // made the accent statistic fragile.
            //
            // MEASURED: on this material the rule's verdict is STABLE (`win` held one
            // value for a whole track, contrast ~115%) while the accent rate wandered
            // between 100% and 130%.  A stable verdict is the better input.
            //
            // It is MEASURED here, not yet applied to the marker.  The marker now
            // holds the baseline's parity, which started correct on 1 and 3, and the
            // agreement below tells us whether wiring this verdict would keep it or
            // invert it.  One run settles that, with no risk to the marker.
            {
              const uint32_t lbl = ((uint32_t)((beatIdx - 1u) - phaseOff) & 3u) + 1u;
              onsetOddRule = (lbl == 1u || lbl == 3u);
            }

            // ---- THE MARKER'S GROUP, FROM THE MUSIC'S PARITY --------------------
            // The beat just counted is a real musical beat, so its parity is a
            // slip-proof reference.  See THE SLIP-PROOF MARKER GROUP in the state
            // block.  One mark may be issued per accepted onset.
            // ---- RESYNC THE ALTERNATION TO THE MUSIC ---------------------------
            // The marker still ALTERNATES PER GRID EMISSION, so its cadence stays
            // regular and mg stays 2 - the flash rate is untouched.  What is
            // anchored to the music is the GROUP.
            //
            // Compare the previous grid beat's marked-ness with the previous musical
            // beat's.  If they disagree, the alternation has slipped by one beat and
            // a single shift of beatCount re-aligns it.  MEASURED: per=10566
            // (136 BPM) against a 133 BPM track made the grid emit ONE EXTRA beat,
            // beatCount gained one, mg went to 9 - ODD, i.e. CH10 moved half a bar -
            // and that was the section-3 flip.  Normally ZERO resyncs occur; one
            // happens per slip and the group is restored immediately.
            {
              const bool bcMarked = (beatCount ==
                                     ((MARK_ON_ODD != (g.flip != 0)) ? 1u : 2u));
              const bool onMarked = (!targetLatched) ||
                                    ((onsetParity == targetParity) != (g.flip != 0));
              // ---- THE RESYNC NEEDS A VERIFIED TARGET -------------------------
              // `targetParity` is only meaningful once a PHRASE BOUNDARY has set it
              // (rlt > 0).  Without that it is just the parity of whichever beat got
              // marked first - a coin flip - and re-aligning the grid to a coin flip
              // is what moved the group mid-track.  MEASURED: with rlt=0 the two
              // resyncs in the last run were the reported "corrected after a minute"
              // and "flipped right at the end", and neither had anything real to
              // correct toward.
              //
              // On this material the anchor never arms (gap never settles), so this
              // gate means NO mid-track movement at all: the group is set once and
              // then simply held.  That is the honest behaviour when the audio does
              // not contain an absolute reference.
              if (targetLatched && reLatch > 0 && (bcMarked != onMarked)) {
                // A mismatch must PERSIST before it counts as a slip - see
                // RESYNC_CONFIRM.  Without this the servo fired every ~7 beats and
                // each firing flipped the group, so CH10 flicked between 1&3 and
                // 2&4 throughout, which is the opposite of what is wanted.
                if (resyncRun < 255) resyncRun++;
                if (resyncRun >= RESYNC_CONFIRM) {
                  beatCount = (beatCount % 2u) + 1u;   // slip one beat and re-align
                  resyncs++;
                  resyncRun = 0;
                }
              } else {
                resyncRun = 0;   // agreement: the alternation is aligned
              }
            }

            onsetParity = (((beatIdx - 1u) & 1u) != 0u);

            // ---- THE ACCENT PATTERN: one character per BEAT -------------------
            // 'X' = at least one mid- or high-band accent landed since the previous
            // accepted onset; '.' = none.  THIS is the two-group measurement, and it
            // is INDEPENDENT of the bar position and of all parity reasoning - it
            // simply asks, beat by beat, whether an accent happened.
            //
            // A pattern that alternates in PAIRS (X.X.X.X. or .X.X.X.X) means the
            // two groups ARE acoustically distinct and the cue is real.  A pattern of
            // all X, or all dots, or an irregular one, means they are not.
            {
              // ---- FEED THE TWO-GROUP WINDOW, WITH THE ATTRIBUTION CORRECT ----
              // accMidSinceBeat covers [previous onset, this onset], and pp (the
              // fraction of that interval at which the mid peak falls) reads 105 of
              // 255 - the peak is in the FIRST HALF, so this accent belongs to the
              // beat at the START of the interval, i.e. the PREVIOUS beat.
              //
              // That is why the window is fed HERE rather than at mark time: the
              // accent of the beat being marked is only known once the next onset
              // arrives.  `justMarked` says whether the beat this accent belongs to
              // was the one CH10 flashed on - which is exactly the question.
              //
              // MEASURED, and the reason this fix exists: fed at mark time, the
              // statistic compared the marked beats against the OTHER group's
              // accents, so the first judgement INVERTED a marker that had started
              // CORRECTLY on 1 and 3 - the track began on 1 and 3, changed to 2 and
              // 4 inside section 1, and stayed there.  The diagnostic measured the
              // error and I had not used it.
              if (justMarked && parWin < 200) {
                parWin++;
                if (accMidSinceBeat > 0) parAcc++;
              }
              // Captured BEFORE it is cleared, for the strength code's CASE below.
              // The accent in this interval belongs to the beat at its START, which
              // is the beat justMarked refers to - the same attribution the parity
              // window uses, so the string and the marker stay in step.
              const bool beatWasMarked = justMarked;
              justMarked = false;
              // TWO-GROUP CONTRAST: fold this beat's accent into a 64-beat history.
              // Bit 0 is the most recent beat, so bits at EVEN positions are one
              // alternating group and bits at ODD positions the other - which is
              // exactly the 2-and-4 versus 1-and-3 split, measured without needing to
              // know which is which.  A clap on 2 and 4 gives one group near 100% and
              // the other near 0%; an accent on every beat gives both near 100%; a
              // cue that is not there gives both near 50%.  THIS is the test for
              // whether the material carries the cue at all - see the ap line.
              accMask = (accMask << 1) | ((accMidSinceBeat > 0) ? 1u : 0u);
              accRingMid[accPatIdx]  = (accMidSinceBeat  > 0) ? 'X' : '.';
              accRingHigh[accPatIdx] = (accHighSinceBeat > 0) ? 'X' : '.';
              // ---- THE STRENGTH CODES FOR THIS BEAT (29 Oct 2026) -------------
              // The X/. flags above are SATURATED on this material - at
              // ACCENT_MARGIN_PCT the accent detector fires on every beat, so they
              // read all-X on every track measured and cannot show whether one beat
              // group is accented more than the other.  THAT is the question the
              // group decision needs, and it has never been measurable before now.
              // These codes carry the strength instead; read them as a PATTERN.
              // LOWERCASE = CH10 flashed on this beat.  So the string answers TWO
              // questions at once: how strong the accent was (the code) and which
              // parity the marker is on (the case).  That is the link between the
              // split and the group - read `L=zKzOzVzHzZnZzJ` and the lowercase
              // positions ARE the group CH10 is marking.
              margRingLow [accPatIdx] = caseCode(strenCode(det.ratPct),     beatWasMarked);
              margRingMid [accPatIdx] = caseCode(strenCode(detMid.ratPct),  beatWasMarked);
              margRingHigh[accPatIdx] = caseCode(strenCode(detHigh.ratPct), beatWasMarked);
              // Cleared so the next beat's code covers ITS interval only.
              det.ratPct = detMid.ratPct = detHigh.ratPct = 0;
              accMidSinceBeat  = 0;
              accHighSinceBeat = 0;
              accPatIdx = (uint8_t)((accPatIdx + 1) % ACC_PAT_N);
              if (accPatN < ACC_PAT_N) accPatN++;
              uint32_t nxm = 0, nxh = 0;
              for (uint8_t i = 0; i < accPatN; i++) {
                const uint8_t j =
                    (uint8_t)((accPatIdx + ACC_PAT_N - accPatN + i) % ACC_PAT_N);
                g.accPatMid[i]  = accRingMid[j];
                g.accPatHigh[i] = accRingHigh[j];
                g.margPatLow[i]  = margRingLow[j];
                g.margPatMid[i]  = margRingMid[j];
                g.margPatHigh[i] = margRingHigh[j];
                if (accRingMid[j]  == 'X') nxm++;
                if (accRingHigh[j] == 'X') nxh++;
              }
              g.accPatMid[accPatN]  = 0;
              g.accPatHigh[accPatN] = 0;
              g.margPatLow[accPatN]  = 0;
              g.margPatMid[accPatN]  = 0;
              g.margPatHigh[accPatN] = 0;
              g.accBeats = accPatN;
              g.accXMid  = nxm;
              g.accXHigh = nxh;
            }
          } else {
            g.rejected = g.rejected + 1;  // spurious: period AND phase untouched
            // ---- THE TIGHT GATE HEALS (29 Oct 2026) --------------------------
            // Consecutive tight-gate rejections are counted.  A few in a row means
            // the onsets really are spurious (keep rejecting - that is the whole
            // point), but SIX in a row more likely means the TEMPO MOVED, and
            // rejecting forever would lock the grid out of the music.  So the gate
            // widens itself and lets the damped pull re-settle.
            if (phaseSettled && !gateWide) {
              if (tightRejRun < 255) tightRejRun++;
              if (tightRejRun >= IV_TIGHT_REJ_RUN) {
                gateWide    = true;
                tightRejRun = 0;
              }
            }
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
          // THE MARKER'S PARITY.  beatCount advances per GRID EMISSION, as it did
          // in the baseline that held mg=2 for a whole track.  It was briefly moved
          // to the accepted-onset path to stop a fast grid sliding the marker's
          // half-bar; that REGRESSED and is reverted - see the note in the onset
          // path.  beatIdx (the odd/even rule's bar position) DOES still count
          // accepted onsets, and that is correct: it feeds the measurement, not the
          // marker, so a stale value there costs nothing visible.
          // ---- GROUP STEER: close the beat that just ended -------------------
          // The accent window of a beat is [its emission, the next emission), so
          // what has accumulated for the CURRENT group is filed here, before the
          // group flips.  Both groups therefore always accumulate over exactly one
          // full beat each, so the comparison can never be lopsided.
          gsSum[gsIdx] += (int64_t)gsMax[gsIdx];
          if (gsN[gsIdx] < 0xFFFFFFu) gsN[gsIdx]++;
          gsMax[gsIdx] = 0;

          // ---- 'F' TEACHES THE STEER, AND DOES IT AT ONCE --------------------
          // A change of preference must take effect immediately, or the operator
          // presses the key and sees nothing for half a minute.  The window is
          // cleared so the next reading is taken fresh under the new preference.
          if (g.flip != lastFlipSeen) {
            lastFlipSeen = g.flip;
            gsTargetBC   = (gsTargetBC == 1u) ? 2u : 1u;
            gsSum[0] = gsSum[1] = 0;
            gsN[0]   = gsN[1]   = 0;
          }

          beatCount = (beatCount % 2) + 1;                 // 1,2,1,2,...
          gsIdx = (uint8_t)(beatCount - 1u);               // the group now sounding

          if (locked) {
            if (((int64_t)absN - lastOnset) > period) g.freeRuns = g.freeRuns + 1;
            // Channel 10 only starts marking once the tempo has been CONFIRMED by
            // MARK_AFTER_ONSETS accepted onsets: IO48 comes back at once because
            // that is the feel that matters, but the 1-and-3 marker does not guess.
            // Which beats to mark.  MARK_ON_ODD is the compile-time default and
            // g.flip is the serial override; both are now only a STARTING POINT.
            // The group is decided from the music by the accent rate just below, so
            // "the bar phase cannot be derived, it has to be chosen" no longer
            // applies where the material accents one group.  'F' stays as a manual
            // escape hatch only - it is not part of the normal path.
            // ---- THE TWO-GROUP DECISION --------------------------------------
            // Judge the window the last 32 MARKED beats filled, then latch.  This
            // runs BEFORE the mark for this beat, so a change lands on this beat and
            // a window is never re-judged part-filled.
            // A change also shifts the alternation by one beat, so `mg` shows ONE
            // 1-gap and ONE 3-gap at the moment of a change.  That pair is the
            // transition, not a fault - mg returns to 2 immediately after.
            //
            // Only MARKED beats are counted, which is the whole point: the question
            // is "are the beats CH10 flashes on the accented group?".  Counting
            // every beat would average both groups together and read ~50% for ever -
            // permanently ambiguous, and the marker would never move.
            if (parWin >= PARITY_WIN) {
              parRate = (uint32_t)(((uint32_t)parAcc * 100u) / parWin);
              parDec++;
              if (parRate >= PARITY_ACC_HI) {
                if (!parityInvert) parLast = 1;
                parityInvert = true;          // marked group IS the accented one
              } else if (parRate <= PARITY_ACC_LO) {
                if (parityInvert) parLast = 0;
                parityInvert = false;         // marked group is NOT accented => 1&3
              }
              // between the thresholds: HOLD - ambiguity never moves the marker
              parWin = 0;
              parAcc = 0;
            }

            // THE ACCENT VERDICT IS NO LONGER APPLIED.
            // MEASURED TWICE, at the SAME reproducible moment: applying it turned a
            // marker that had started CORRECTLY on 1 and 3 into 2 and 4 after the
            // first judgement.  With the attribution corrected, that means the MID
            // accent genuinely sits on the 1-and-3 group on this material - the
            // OPPOSITE of the clap-on-2-and-4 convention the statistic assumes.
            // The cue therefore proves the two groups are DISTINCT but not which is
            // which, so its sign cannot steer.  It is still measured and logged on
            // the `mk` line; it no longer decides.
            // ---- THE MARK DECISION, FROM THE MUSIC'S PARITY --------------------
            // NO beatCount HERE ANY MORE.  The group comes from the onset parity,
            // latched at the first mark, so a grid that is briefly fast or slow
            // cannot move it - which is precisely the section-3 flip.
            //
            // At most ONE mark per accepted onset, so a grid emitting two beats
            // inside one musical beat can never flash CH10 twice for the same beat.
            // That is the "CH10 on every beat" failure, and this bound prevents it
            // structurally rather than by hoping the period stays right.
            // ---- THE GROUP JUDGEMENT, FROM THE MUSIC (29 Oct 2026) ------------
            // Judged only once BOTH groups have GS_WINDOW beats, then the window is
            // reset.  The winner is the side the EAR says is right: `g.flip` picks
            // which side of the split is 1&3, so 'F' TEACHES the steer instead of
            // merely flipping the marker for the moment - one keystroke and the
            // music holds it from then on.  A margin is demanded, because a
            // symmetrical passage must never move the marker.
            if (gsN[0] >= GS_WINDOW && gsN[1] >= GS_WINDOW) {
              gsMean[0] = (int32_t)(gsSum[0] / (int64_t)gsN[0]);
              gsMean[1] = (int32_t)(gsSum[1] / (int64_t)gsN[1]);
              // `g0Quieter` = group 0 is measurably the QUIETER of the two.  (The
              // first version of this named them "stronger" and got the mapping
              // backwards - see below.)
              const bool g0Quieter = (gsMean[0] + GS_MARGIN < gsMean[1]);
              const bool g1Quieter = (gsMean[1] + GS_MARGIN < gsMean[0]);
              if (g0Quieter || g1Quieter) {
                // ---- WHICH SIDE IS 1&3: SETTLED BY THE EAR, 29 Oct 2026 --------
                // NOT assumed and NOT a convention taken on trust.  The first steer
                // run measured a 394% split (gsA=136, gsB=530), marked the STRONGER
                // group, and the user heard 2&4.  That is the backbeat CONFIRMED on
                // this material: the clap/snare accents 2 and 4, so the group with
                // the BIGGER mid-band accent IS 2&4, and the QUIETER one is 1&3.
                //
                // So the default marks the QUIETER group.  The first build marked
                // the stronger one because of an inverted comparison, which is
                // exactly why it landed on 2&4 and then held it.  g.flip != 0 asks
                // for the other side; 'F' teaches it in a single keypress and the
                // steer keeps the corrected side from the audio thereafter.
                const bool wantBC1 = (g.flip == 0) ? g0Quieter : g1Quieter;
                gsTargetBC = wantBC1 ? 1u : 2u;
                gsValid    = true;
              }
              gsSum[0] = gsSum[1] = 0;   // re-measure from scratch
              gsN[0]   = gsN[1]   = 0;
            }
            // The music takes over as soon as it has a real reading.  Until then the
            // compile-time default / 'F' stands, so on material with no split the
            // behaviour is EXACTLY as it was before this change.
            const bool wantOdd = gsValid ? (gsTargetBC == 1u)
                                         : (MARK_ON_ODD != (g.flip != 0));
            const bool mark = (confirmCnt >= MARK_AFTER_ONSETS &&
                               ((beatCount == 1) == wantOdd));
            // Latch ONLY once the phase has SETTLED.  While the grid is half a beat
            // out the marked grid beats are on the WRONG musical group, so latching
            // then captures 2-and-4 and holds it - the reported "settled on 2 and 4
            // for a good while, then changed to 1 and 3".  The latch must see a grid
            // that is ON the beat.  Marking itself is NOT delayed.
            if (mark && !targetLatched && phaseSettled) {
              targetLatched = true;
              targetParity  = onsetParity;
            }
            g.markNow    = (uint8_t)(mark ? 1 : 0);
            g.beatSeq    = g.beatSeq + 1;
            g.parInv     = parityInvert ? 1 : 0;
            g.grpLatched = targetLatched ? 1 : 0;
            g.grpParity  = targetParity ? 1 : 0;
            g.resyncs    = resyncs;
            g.resyncRun  = resyncRun;
            g.reLatch    = reLatch;
            g.phaseSettled = phaseSettled ? 1 : 0;
            g.phaseRun     = phaseRun;

            // Record that this grid beat was MARKED.  The accent that belongs to it
            // is not known yet - it arrives with the NEXT onset - so the window is
            // fed from the onset path, not here.  See the note there for why.
            if (mark) {
              justMarked = true;
              // Count agreement between the group CH10 is marking and the rule's
              // 1-and-3.  MEASURED, not assumed - see the note in the state block.
              if (ruleWin < 1000000u) {
                ruleWin++;
                if (onsetOddRule) ruleAgree++;
              }
            }
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
      // The UNCLAMPED mean over the SAME averaging window, published before pdN is
      // cleared.  pdMs saturates at the bound and has been hiding errors larger than
      // it - MEASURED at the bound on 14 of 115 seconds of the Billy Jean baseline.
      g.pdRawMs   = (pdN > 0)
                    ? (int32_t)((pdRawAcc * 1000) /
                                ((int64_t)pdN * (int64_t)TARGET_FS))
                    : 0;
      g.pdClamp   = pdClampWin;
      pdRawAcc    = 0;
      pdClampWin  = 0;
      pdAcc = 0;
      pdN   = 0;
      g.eMidPk = midPkWin;
      midPkWin = 0;

      // ---- THE PERIOD SPAN, AND THE MATERIAL THE PHASE ERROR IS MADE OF -----
      g.per4Now = per4Win;
      g.per8Now = per8Win;
      g.gateWide = gateWide ? 1u : 0u;
      g.tightRej = tightRejRun;
      g.gsA = gsMean[0];
      g.gsB = gsMean[1];
      g.gsT = gsTargetBC;
      g.gsV = gsValid ? 1u : 0u;
      // The last 8 accepted intervals (samples) and the last 8 raw dd values (ms)
      // as text.  Alternation in these IS the per-group detector latency; a steady
      // one-sided run is a period bias.  Printing them removes the guesswork.
      {
        char *p = (char *)g.ivsStr, *e = (char *)g.ivsStr + sizeof(g.ivsStr) - 1;
        *p = 0;
        for (uint8_t i = 0; i < ivCnt; i++) {
          const uint8_t j = (uint8_t)((ivIdx + 8u - ivCnt + i) & 7u);
          p = appendInt(p, e, (long)ivRing[j], i != 0);
        }
      }
      {
        char *p = (char *)g.ddsStr, *e = (char *)g.ddsStr + sizeof(g.ddsStr) - 1;
        *p = 0;
        for (uint8_t i = 0; i < ddCnt; i++) {
          const uint8_t j = (uint8_t)((ddIdx + 8u - ddCnt + i) & 7u);
          p = appendInt(p, e, (long)ddRing[j], i != 0);
        }
      }
      g.maxRatLow  = (uint32_t)(maxRatLowW  < 0 ? 0 : maxRatLowW);
      g.maxRatMid  = (uint32_t)(maxRatMidW  < 0 ? 0 : maxRatMidW);
      g.maxRatHigh = (uint32_t)(maxRatHighW < 0 ? 0 : maxRatHighW);
      maxRatLowW = maxRatMidW = maxRatHighW = 0;

      // Two-group contrast over the last 64 beats.  Bit 0 is newest, so EVEN bit
      // positions are one alternating group and ODD positions the other - the 2&4
      // versus 1&3 split, measured without needing to know which is which.
      {
        const uint32_t ev = (uint32_t)__builtin_popcountll(
            accMask & 0x5555555555555555ULL);
        const uint32_t od = (uint32_t)__builtin_popcountll(
            accMask & 0xAAAAAAAAAAAAAAAAULL);
        g.accEvenPct = (uint8_t)((ev * 100u) / 32u);
        g.accOddPct  = (uint8_t)((od * 100u) / 32u);
      }

      // ---- the restored stack's behaviour readout ---------------------------
      g.structEv    = structCnt;
      g.phraseBd    = phraseCnt;
      g.lastGap     = lastGap;
      g.sameGapRun  = sameGapRun;
      g.anchorMoves = anchorCnt;
      g.centMark    = centMarks;
      g.crashMark   = crashMarks;
      g.lowCand     = lowCandWin;
      g.lowEarly    = lowEarlyWin;
      g.lowValid    = lowValidWin;
      g.lowRej      = lowRejWin;
      lowCandWin = 0; lowEarlyWin = 0; lowValidWin = 0; lowRejWin = 0;
      // The two gains and the three envelopes, so the normaliser's behaviour is
      // visible rather than assumed.  gN is Q12: 4096 = 1.0x.
      g.gNorm = gNorm; g.gAgc = gAgc;
      g.parRate   = parRate;
      g.parDec    = parDec;
      g.parLast   = parLast;
      g.ruleWin   = ruleWin;
      g.ruleAgree = ruleAgree;
      g.fastEnv = fastEnv;
      g.shortEnv = (int32_t)(shortEnv >> DC_Q);
      g.longEnv  = (int32_t)(longEnv  >> DC_Q);

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

  // PHASE A odd/even evidence, on its OWN line so every field above stays
  // comparable with previous runs.
  //   ErM / ErL  mid and low band contrast = (second LOWEST mean x 100) / lowest.
  //              Above ~120 on the MID band means a real odd/even cue exists.
  //              The LOW band is the CONTROL and is expected near 100: the kick is
  //              on all four beats, which is exactly what defeated the
  //              professional baseline (its Er measured 109%, no dominance).
  //   mE         the four MEAN mid-band energies in LABEL order.  Beat 1 should be
  //              the LOWEST of the four if the snare is on 2 and 4.
  //   win        the label holding that lowest mean - the rule's answer.
  //   pOff/anc   the phase the rule WOULD adopt, and how often it would have moved.
  //              DIAGNOSTIC ONLY - the marker is still driven by beatCount.
  //   bars       bars in the SLIDING WINDOW (0..BAR_HIST), NOT a track total.  The
  //              rule needs >= DOWNBEAT_MIN_BARS before it will speak at all.
  //   mPk        peak mid flux this second - the mid band's LIVENESS.  Must be
  //              non-zero even in silence (the noise floor).  Exactly 0 means the
  //              Goertzel chain is dead and every ErM reading below is worthless.
  //   pp         where in the inter-onset interval the mid peak falls, 0..255.
  //              See the note in Stats - near 0 means a one-beat offset in the
  //              attribution, near 255 means it is correct as written.
  Serial.printf("ee %lu  ErM=%lu%% ErL=%lu%%  mE=%ld/%ld/%ld/%ld  win=%lu "
                "pOff=%lu anc=%lu bars=%lu act=%lu cnf=%u  mPk=%ld  pp=%lu\n",
                (unsigned long)sec,
                (unsigned long)g.eRatioMid, (unsigned long)g.eRatioLow,
                (long)g.eMeanMid[0], (long)g.eMeanMid[1],
                (long)g.eMeanMid[2], (long)g.eMeanMid[3],
                (unsigned long)g.eWin, (unsigned long)g.ePhaseOff,
                (unsigned long)g.eAnchors, (unsigned long)g.eBars,
                (unsigned long)g.eActed,
                (unsigned)g.eCnf,
                (long)g.eMidPk,
                (unsigned long)g.ePp);

  // THE RESTORED-STACK LINE.  Its own line so the numbers can be read together.
  //   str   structural shifts seen (a drop-out or a rise vs the long-term level)
  //   phr   QUALIFIED phrase boundaries - only those on the document's 16/32-beat
  //         grid move Beat 1.  A handful per track is healthy; dozens means the
  //         qualification is not working and the anchor is thrashing.  MEASURED in
  //         the professional baseline: unqualified, it fired 22 times a track about
  //         4 beats apart, which destroys the count it exists to protect.
  //   amv   times the anchor ACTUALLY moved Beat 1
  //   cmk   spectral-centroid phrase cues     xmk   cymbal-crash phrase cues
  //   cand  low-band candidates  ely  confirmed early by a mid/high accent
  //   vld   accepted on expiry   rej  queue overruns
  //         A healthy MIX of ely and vld proves the accents really do coincide with
  //         the low-band transients - the document's premise, and the thing to
  //         check before trusting anything built on the accent bands.
  //   gN/gA the two gains, Q12 (4096 = 1.0x).  gN should MOVE with the music: if it
  //         sits at 4096 forever the normaliser is not running.
  //   gap/srun  the MEASURED phrase period.  `gap` is the last interval between
  //          structural events in BEATS and `srun` how many times in a row it has
  //          repeated.  srun reaching PHRASE_REPEATS is what arms the anchor - and
  //          this is the number that tells us what this material's phrase length
  //          actually is, instead of assuming 16 or 32.
  Serial.printf("an %lu  str=%lu phr=%lu amv=%lu  gap=%lu srun=%u  cmk=%lu xmk=%lu  "
                "cand=%lu ely=%lu vld=%lu rej=%lu  gN=%ld gA=%ld  "
                "fEnv=%ld sEnv=%ld lEnv=%ld\n",
                (unsigned long)sec,
                (unsigned long)g.structEv, (unsigned long)g.phraseBd,
                (unsigned long)g.anchorMoves,
                (unsigned long)g.lastGap, (unsigned)g.sameGapRun,
                (unsigned long)g.centMark, (unsigned long)g.crashMark,
                (unsigned long)g.lowCand, (unsigned long)g.lowEarly,
                (unsigned long)g.lowValid, (unsigned long)g.lowRej,
                (long)g.gNorm, (long)g.gAgc,
                (long)g.fastEnv, (long)g.shortEnv, (long)g.longEnv);

  // THE MARKER-DECISION LINE - the connection that was missing, made visible.
  //   inv    1 = the analysis has inverted which group CH10 marks
  //   rate   the MID-accent rate of the beats CH10 is MARKING, %
  //            near 0    the marked beats are the UNaccented group => 1 and 3
  //            near 100  the marked beats ARE the accented group => inverted
  //            near 50   the accent is not a clean beat-group cue on this
  //                      material, so the decision HOLDS and never moves
  //   dec    how many 32-marked-beat judgements have been made
  //   last   1 = the last judgement inverted the group, 0 = kept it
  // If `dec` climbs and `inv` settles at 1 or 0 and STAYS there, the decision has
  // converged and the group is being chosen from the music.
  //   rule   the DECISIVE number.  Agreement between the beats CH10 actually marks
  //          and the restored odd/even rule's 1-and-3, over `marked` beats:
  //            ~100%  wiring the rule keeps the group CH10 already holds
  //            ~0%    the rule's sense is flipped for this material - one line
  //            ~50%   the rule's labels are not tracking the beat at all
  //          The marker is NOT changed by this measurement, so a run costs nothing.
  //   tgt    the SLIP-PROOF marker group: `latched/parity`.  Once latched=1, CH10's
  //          half-bar is fixed to the MUSIC's beat parity, so a grid that hiccups
  //          can no longer move it - the section-3 flip cannot recur.
  //   rsy    times the alternation had to be re-aligned to the music.  0 on a clean
  //          run.  Each one is a grid slip that WOULD have moved CH10 half a bar and
  //          has been put back immediately, so a count that climbs while tgt stays
  //          put is the fix doing its job.
  //   acq    `settled/consecutive-onsets-within-10ms`.  acq=0 with the second number
  //          climbing means the grid is still WALKING onto the beat, and the group is
  //          deliberately not latched until it reads 1.  If acq=1 arrives within a
  //          beat or two, the fast acquisition is working; if the second number
  //          keeps resetting to 0, the grid never lands on the beat at all - a very
  //          different fault, and one worth knowing about.
  //   rlt    times the group was re-latched from a MEASURED PHRASE BOUNDARY.  This is
  //          Audio.md Stage 4's absolute reference taking control of the group instead
  //          of the coin-flip first mark.  Non-zero with tgt changing to match is the
  //          mechanism working; if it stays 0, the anchor never armed.
  Serial.printf("mk %lu  inv=%u rate=%lu%% dec=%lu last=%d  "
                "rule=%lu%% of %lu marked  tgt=%u/%u  rsy=%lu run=%u  "
                "acq=%u/%u  rlt=%lu\n",
                (unsigned long)sec,
                (unsigned)g.parInv, (unsigned long)g.parRate,
                (unsigned long)g.parDec, (int)g.parLast,
                (unsigned long)(g.ruleWin ? (g.ruleAgree * 100u) / g.ruleWin : 0u),
                (unsigned long)g.ruleWin,
                (unsigned)g.grpLatched, (unsigned)g.grpParity,
                (unsigned long)g.resyncs, (unsigned)g.resyncRun,
                (unsigned)g.phaseSettled, (unsigned)g.phaseRun,
                (unsigned long)g.reLatch);

  // THE TWO-GROUP LINE - the direct measurement the user asked for.
  // One character per BEAT, oldest first, newest last, PER BAND:
  //   'X' an accent landed on that beat,  '.'  none.
  // INDEPENDENT of the bar position and of all parity reasoning.  A pattern
  // alternating in PAIRS is a real cue; all-X, all-dot or irregular is not.
  //
  // THE BANDS ARE PRINTED SEPARATELY, and that matters.  MEASURED 28 Oct: combined
  // into one flag the line read XXXXXXXXXXXXXXXX on every beat, because the HIGH
  // band fires ~3/s on this material (470 accents in 157 s) - it is following the
  // hi-hats on the eighth notes.  The MID band fired 134 times in the same time,
  // 0.85/s, about 39% of beats.  Only the separate lines can show which band, if
  // either, carries the two-group cue.
  {
    char pm[ACC_PAT_N + 1], ph[ACC_PAT_N + 1];
    for (uint8_t i = 0; i <= ACC_PAT_N; i++) {
      pm[i] = g.accPatMid[i];
      ph[i] = g.accPatHigh[i];
    }
    //   2grp  THE DIRECT TEST FOR THE CUE.  The MID accent rate of each of the two
    //         alternating beat groups over the last 64 beats, as `groupA/groupB` in
    //         per cent.  Read it like this:
    //           one near 100 and the other near 0   the material HAS the cue, and the
    //                                               high group is the clap/snare one
    //           both near 100                        an accent on every beat - no split
    //           both near 50                         no cue in the MID band at all
    //         It needs no knowledge of which group is 1&3, so it separates "the cue is
    //         not in this material" from "the cue is there and we are reading its sign
    //         wrongly" - the question that has blocked every earlier conclusion.
    Serial.printf("ap %lu  beats=%lu  accMid=%lu accHigh=%lu  "
                  "mid=%s (%lu)  high=%s (%lu)  2grp=%u%%/%u%%\n",
                  (unsigned long)sec,
                  (unsigned long)g.accBeats,
                  (unsigned long)g.accMid, (unsigned long)g.accHigh,
                  pm, (unsigned long)g.accXMid,
                  ph, (unsigned long)g.accXHigh,
                  (unsigned)g.accEvenPct, (unsigned)g.accOddPct);

    // ---- THE STRENGTH LINE: what the X/. flags CANNOT show (29 Oct 2026) -----
    // The mid=/high= patterns above are SATURATED on this material - they read all-X
    // on every track measured, because at ACCENT_MARGIN_PCT the detector fires on
    // every beat.  A saturated flag cannot answer "is one beat group accented more
    // than the other?", which is the ONLY question the group decision needs.  These
    // three strings carry the STRENGTH per beat instead, one code per beat, oldest
    // first, base 36 in 50% steps of that band's own local average:
    //   '0' under 50%   '2' 100-149%   '9' 450-499%   'A' 500-549% ... 'Z' 1750%+
    // READ THEM AS PATTERNS:
    //   C2C2C2C2C2C2C2C2   the two groups carry DIFFERENT strengths - the cue IS in
    //                      this band, and the cut is between 2 and C
    //   9999999999999999   flat - no group cue in this band at ANY threshold
    //   L = LOW (the kick)   M = MID (snare/clap)   H = HIGH (hats)
    // The LOW string is the most promising one on this material: the kick sits on a
    // beat GROUP where a snare/hi-hat does not, and it needs no sign convention.
    // mx= is the strongest accent of this second per band, in per cent of the local
    // average - the exact number behind the codes.
    char pl[ACC_PAT_N + 1], pmm[ACC_PAT_N + 1], phh[ACC_PAT_N + 1];
    for (uint8_t i = 0; i <= ACC_PAT_N; i++) {
      pl[i]  = g.margPatLow[i];
      pmm[i] = g.margPatMid[i];
      phh[i] = g.margPatHigh[i];
    }
    //   gs   THE GROUP STEER'S READING - the music's own split, as measured:
    //        gs=<meanA>/<meanB> are the MID-band accent means of the two groups in
    //        per cent of each group's own local average.  A clear gap IS the factor
    //        that separates the two groups; gsV=1 means a real split was measured
    //        and the music is choosing, gsV=0 means it showed none and the default
    //        stands.  gsT is the beatCount CH10 marks.  `F` inverts gsT AND the
    //        preference, so a wrong sign is one keystroke, and the steer then holds
    //        the corrected side from the audio for the rest of the material.
    Serial.printf("st %lu  L=%s  M=%s  H=%s  mx=%lu/%lu/%lu  "
                  "gs=%ld/%ld gsT=%u gsV=%u\n",
                  (unsigned long)sec, pl, pmm, phh,
                  (unsigned long)g.maxRatLow, (unsigned long)g.maxRatMid,
                  (unsigned long)g.maxRatHigh,
                  (long)g.gsA, (long)g.gsB,
                  (unsigned)g.gsT, (unsigned)g.gsV);
  }

  // ---- THE TIMING LINES: WHY the grid is where it is (29 Oct 2026) -----------
  //   per    the period actually in use, samples
  //   per4   the period over the last 4 ACCEPTED intervals - one bar.  Across 4
  //          beats each group appears an equal number of times, so the per-group
  //          detector LATENCY CANCELS.  This is what the grid now runs on.
  //   per8   the same over 8 intervals, for comparison.  Steadier if the material
  //          alternates over more than a bar, but half as quick to respond.
  //   pd     the mean CLAMPED phase error - saturates at +/-period/4
  //   raw    the mean UNCLAMPED phase error.  THIS is the honest number; pd cannot
  //          show anything larger than its bound.
  //   clamp  how many phase samples hit that bound this second.  Non-zero means the
  //          pull could not keep up, stated by the servo rather than inferred.
  //   iv     the last 8 accepted intervals, samples - ALTERNATION here is a per-group
  //          detector latency; a steady one-sided run is a period bias
  //   dd     the last 8 RAW phase errors, ms - the same test on the phase itself
  // A correct fix shows: per4 steadier than per, raw centring on 0, clamp at 0, and
  // no alternation in either sequence.
  //   gw / tr  the tight gate: 1 = it has WIDENED itself back to +/-25% (six tight
  //            rejections in a row - the tempo moved), and the running count of
  //            tight rejections.  On a steady track both stay 0, which is what
  //            proves the +/-12% gate is rejecting flams and not beats.
  Serial.printf("tm %lu  per=%lu per4=%lu per8=%lu  pd=%ldms/%lu raw=%ldms  "
                "clamp=%lu  gw=%u tr=%u\n",
                (unsigned long)sec,
                (unsigned long)g.periodNow, (unsigned long)g.per4Now,
                (unsigned long)g.per8Now,
                (long)g.pdMs, (unsigned long)g.pdN,
                (long)g.pdRawMs, (unsigned long)g.pdClamp,
                (unsigned)g.gateWide, (unsigned)g.tightRej);
  Serial.printf("iv %lu  %s\n", (unsigned long)sec, (const char *)g.ivsStr);
  Serial.printf("dd %lu  %s\n", (unsigned long)sec, (const char *)g.ddsStr);
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
  // NO esp_task_wdt_reset() HERE.  The include and the call were left over from
  // the professional baseline, which DID configure the TWDT (wcfg.timeout_ms =
  // 5000; esp_task_wdt_init; add).  beat_simple never initialises or subscribes
  // it, so the reset could not find its task and printed
  //     E (10337) task_wdt: esp_task_wdt_reset(707): task not found
  // at ~200 lines per second - measured: 194 of 199 lines/second of the whole
  // serial log.  That drowned the log AND put needless work in the loop that
  // services the flashes, which is where flash-timing jitter comes from.
  // "task not found" also proves this task is NOT watched, so dropping the call
  // cannot cause a watchdog reboot.  MEASURED after removal: the log is clean.
  s_io48.service(PIN_IO48);
  s_ch10.service(PIN_CH10);

  // 'F' is a MANUAL ESCAPE HATCH ONLY.  The group CH10 marks is now decided from
  // the music by the accent rate (see THE TWO-GROUP DECISION in the DSP task), so
  // the operator no longer has to pick it.  Press 'F' only if the automatic
  // decision is visibly wrong, and expect the next judgement to override it.
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

