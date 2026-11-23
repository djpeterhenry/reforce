#pragma once
// reforce extension (Phase 5C.5, not in G-Force): onset (BEAT) and tempo (TEMPO)
// detection on the analyzer's mono sample stream.
//
// Onset strength: every hop (hopSeconds, ~10 ms) a Hann FFT of the last fftSize
// samples is taken; per bin L = log(1 + compression * |X|) (amplitude, full-scale
// sine = 1), and the spectral flux sum(max(0, L - L_prev)) / bins is computed in
// a low band (lowLo..lowHi Hz, kick drum) and a mid band (midLo..midHi Hz, snare,
// claps, keys). Onset strength o = lowWeight * fluxLow + midWeight * fluxMid.
//
// Onsets (BEAT): o at hop n-1 is a local maximum, exceeds an adaptive threshold
// mean + thresholdK * stddev of o over the last thresholdWindowSeconds (and the
// absolute minFlux, so silence and steady tones never trigger), and the previous
// onset is at least minIntervalSeconds ago. BEAT = exp(-age / pulseDecaySeconds)
// after the latest onset (1 at the onset, ~0 when age > 5 decay times).
//
// Tempo (TEMPO, BPM): every tempoUpdateSeconds the autocorrelation of the
// mean-removed onset strength over the last tempoWindowSeconds is evaluated for
// lags between 60/maxBpm and 60/minBpm (only with >= minOnsets onsets in the window), weighted by a log-normal prior centered on
// priorBpm (priorOctaves wide; picks 120 rather than 60 or 240 for a 4/4 kick)
// and peak-refined. Confidence = normalized autocorrelation at that lag. A
// candidate with confidence >= minConfidence moves the estimate (one-pole,
// tempoSmoothSeconds) or replaces it when it is a different tempo seen on two
// consecutive updates. TEMPO is 0 until the first confident estimate and falls
// back to 0 after holdSeconds without one (e.g. silence, ambient music).
// Octave folding (Peter, 2026-11-15): every candidate is doubled or halved into
// [foldMinBpm, 2 * foldMinBpm] (80-160 BPM), so a half-time pick (62) reads 124
// and drum and bass (174) reads 87. Near the edges a candidate one octave from
// the current tempo keeps the current octave (within foldSlack), so a song at
// ~80 or ~160 BPM does not flip between the two. foldMinBpm = 0: no folding.
//
// Steady tempo (steady = true, reforce.BeatClock.SteadyTempo; Peter, 2026-11-15: house music, breakdowns): once
// there is a tempo, the music is assumed to keep it.
//   - Tracking: the strongest autocorrelation peak within +-steadyWindow of the current tempo (any octave, folded)
//     moves it (smoothed), so pitch-fader drift and small tempo changes are followed.
//   - Switching: a peak at a different tempo replaces it only after it has won (raw autocorrelation above the
//     current tempo's own peak, confidence >= switchMinConfidence) for switchSeconds, each update counting
//     min(1, confidence / 0.5) of its period; updates where it does not win take the evidence back down.
//   - Holding: without a confident reading the tempo stays for steadyHoldSeconds, unless there is no sound at all
//     (peak 30 Hz-4 kHz amplitude below silenceLevel) for silenceSeconds: then TEMPO = 0.
//   - Kick gate (Peter, 2026-11-15: a long breakdown read 83 for 125, a dotted-eighth riff): the kick onset level
//     (flux in kickLoHz..kickHiHz, below the low band's top so notes near 200 Hz do not leak in; smoothed over
//     kickLevelSeconds) is compared with its own recent peak (decaying over
//     kickRefSeconds). Below kickPresentRatio of it there is no kick (a breakdown): the tempo is frozen, no drift,
//     no switching, no hold timeout (silence still ends it). Tempo changes need the kick.
// steady = false: the quick-reacting behavior above (two agreeing updates switch, holdSeconds).
//
// Single-threaded (the analyzer's thread). push() allocates nothing.

#include <cstddef>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>

namespace reforce::audio {

struct BeatConfig {
    double hopSeconds = 0.01;
    int fftSize = 1024;
    double compression = 100.0;
    double lowLoHz = 30.0, lowHiHz = 200.0;
    double midLoHz = 200.0, midHiHz = 4000.0;
    double lowWeight = 1.0, midWeight = 0.5;
    double thresholdWindowSeconds = 1.5;
    double thresholdK = 1.5;
    double minFlux = 0.01;
    double minIntervalSeconds = 0.12;
    double pulseDecaySeconds = 0.12;
    double tempoWindowSeconds = 8.0;
    double tempoUpdateSeconds = 0.25;
    double minBpm = 60.0, maxBpm = 200.0;
    double priorBpm = 120.0, priorOctaves = 1.0;
    // Bar structure (PLAN 5J.12): a candidate's score also counts the autocorrelation at 2x and 4x its lag (2 and 4
    // beats), so the real beat beats a 3:4 impostor (a riff every 3/4 beat). 0 = off.
    bool combPeaks = true;   // candidates from the peaks of the combined (1 beat + bar) score, not the 1-beat one
    bool refine4 = true;   // refine each candidate's tempo from its peak 4 beats later
    double combHalf = 0.0;  // weight of half a beat (subdivision: off-beat hats; 2/3's half is 0.75 beat)
    double comb8 = 0.0;  // ... and 8 beats (needs tempoWindowSeconds > 8 beats)
    double comb2 = 1.0, comb4 = 2.0;  // with combPeaks: held-out TEMPO right 86.3 -> 91.9 %, on the beat 68.8 -> 73.2 %
    double foldMinBpm = 80.0;  // TEMPO folded into [foldMinBpm, 2 * foldMinBpm]; 0 = off
    double foldSlack = 0.06;   // relative: how far past the range edge the current octave may stay
    double minConfidence = 0.25;
    int minOnsets = 4;  // onsets needed within the tempo window for an estimate (rhythm, not a hum)
    double tempoSmoothSeconds = 2.0;
    double holdSeconds = 4.0;
    bool steady = true;
    double steadyWindow = 0.04;        // relative: the current tempo's own peak
    double steadySmoothSeconds = 10.0;  // steady: smoothing of small drift (< steadyFastBand)
    double steadyFastBand = 0.005;       // ... larger moves (a new track's tempo), the same way for steadyFastSeconds,
    double steadyFastSeconds = 2.0;     //     use tempoSmoothSeconds
    double steadyDriftMinConfidence = 0.25;  // the local peak's confidence needed to follow drift (kick playing;
                                             //   0.12 helped nudges but cost songs ~1 % on the beat)
    double switchSeconds = 8.0;        // confidence-weighted seconds a different tempo must win
    double switchMinConfidence = 0.35;
    double firstLockSeconds = 20.0;    // after a tempo is first found, react quickly (two agreeing updates) this long
    double steadyHoldSeconds = 60.0;   // keep the tempo this long without a confident reading
    double silenceSeconds = 3.0;       // ...unless silent this long
    double silenceLevel = 0.003;       // peak band amplitude (full-scale sine = 1), about -50 dBFS
    double kickLoHz = 30.0, kickHiHz = 120.0;  // the kick gate's band
    double kickLevelSeconds = 0.5;     // smoothing of the kick-band onset level
    double kickRefSeconds = 120.0;     // its reference: the recent peak, decaying with this time constant
    double kickPresentRatio = 0.3;     // kick present: level >= ratio * reference
    double kickPeakSeconds = 30.0;     // lastOnsetKickness reference decay
    // On-beat probability of an onset: sigmoid(beatnessBias + sum beatnessW[b] * band b of the onset's hop +
    // beatnessWNext[b] * band b of the next hop), fitted on Peter's library (tools/beat_eval/train_beatness.py).
    // All zero = 0.5 for every onset (no information). Fitted 2026-11-15 on 2.8 M onsets of 2,727 tuning songs,
    // gathered with the round-4 detector, kick sections only (breakdowns / intros / outros left out: their notes near
    // a beat are noise as labels). Held-out AUC 0.762 (kick sections); kick-ness alone 0.54.
    std::array<double, 9> beatnessW{0.0409, 0.1122, 0.6552, -0.3792, -0.2053, 0.3954, 0.3758, -0.1081, 0.0991};
    std::array<double, 9> beatnessWNext{0.2748, 0.4074, 0.6071, -0.5243, -0.2779, 0.1565, -0.1471, -0.1589, 0.0708};
    double beatnessBias = -2.2836;
    double acfBeatness = 1.0;  // tempo autocorrelation signal: strength x (1 - a + a p) for a <= 1, strength x p^a above
                               //   (p = the hop's on-beat probability)
    // With a tempo, onsets use a model that also sees the bands half a beat before (lastOnsetBandsHalf): a beat has
    // hats there, an off-beat has the kick. Fitted 2026-11-15 on 2.9 M kick-section onsets of 2,815 tuning songs:
    // held-out AUC 0.805 (0.757 without the half-beat context). useHalfModel = false: the model above only.
    bool useHalfModel = true;
    std::array<double, 9> halfW{0.0481, 0.1139, 0.5544, -0.3231, -0.2008, 0.3478, 0.3852, -0.0563, -0.0808};
    std::array<double, 9> halfWNext{0.2367, 0.3522, 0.5134, -0.4345, -0.2470, 0.1581, -0.0792, -0.1630, -0.0030};
    std::array<double, 9> halfWHalf{-0.0562, 0.0877, -0.4766, -0.0592, 0.0981, -0.4852, -0.4993, 0.3497, 1.1946};
    double halfBias = -2.2169;
    double bandMeanSeconds = 4.0;      // the band features' reference (recent mean flux)
    double kickPhaseSeconds = 4.0;     // kick phase average decay
    double kickPhasePower = 2.0;       // flux exponent (emphasizes the strongest transients: the kick)
    // Beat fold (PLAN 5J.12, "which half is the beat"): per hop, the band flux (hop band features) and band level
    // (mean log magnitude) are added into kFoldBins bins of the beat at TEMPO (phi as for the kick phase), decaying
    // over foldSeconds. A learnt linear score (foldW, kFoldBins x kFoldFeat, bin-major, beat at bin 0) is evaluated at
    // every rotation of the normalized profile; the best is the learnt beat (foldPhase). All-zero foldW: off.
    // Phase filter (PLAN 5J.12, research idea 1: a forward filter over the beat's place in the beat, at TEMPO):
    // kPhaseStates states on phi's circle; per hop a random walk (pfDiffusion, beats per sqrt(s)), and while a kick
    // plays the hop's on-beat activation (onset strength x on-beat probability, relative to its recent mean, minus
    // 1) sharpens the states near the hop's phi: x (1 + pfGain * (a - 1) * exp(-d^2 / 2 pfWidth^2)), floored at
    // pfFloor. Every fold score multiplies in the fold's softmax^pfFoldGain. Starts over on a new tempo.
    static constexpr int kPhaseStates = 64;
    double pfDiffusion = 0.01, pfGain = 0.3, pfWidth = 0.04, pfFloor = 0.5, pfFoldGain = 0.05, pfActSeconds = 4.0;
    // While TEMPO is moving (a pitch ride: more than pfMovingBand from its pfMovingSeconds average), the random walk
    // is pfDiffusionMoving and the fold's weight pfFoldGainMoving (its 12 s are smeared by the ride), so the beat
    // can slide along. tools/beat_eval --mixes: on the beat during rides 62.5 -> 79.3 %, songs unchanged. (A loop
    // trimming phi's rate from the peak's drift ran away: with the fold in the same frame the filter stayed
    // confident while a wrong trim slid the beat. Not kept.)
    double pfDiffusionMoving = 0.04, pfMovingBand = 0.002, pfMovingSeconds = 4.0, pfFoldGainMoving = 0.0;
    // The filter's tempo dimension (PLAN 5J.12, Peter's quick 1-3 BPM nudges): pfTempoRows rows of absolute tempi
    // pfTempoStep apart, centered on TEMPO (re-centered as TEMPO moves). Each row's beat drifts along phi's circle at
    // its tempo minus TEMPO, so the filter can follow a nudge's beat from the onsets while TEMPO still lags. Rows
    // mix as a random walk while a kick plays (frozen in breakdowns): pfTempoDiffusion (relative per sqrt(s)) for
    // pfOpenHoldSeconds after the autocorrelation's local peak (confidence >= pfOpenMinConfidence) reads a move of
    // more than pfOpenBand, pfTempoDiffusionSteady otherwise (< 0: always pfTempoDiffusion). Starts over around TEMPO
    // (pfTempoPrior rows wide). filterTempo(): the row mean near the beat. 1 row: the plain phase filter (default).
    // tools/beat_eval, with the kick grid below: 7 rows nudges on the beat 77.6 -> 79.7 %, pitch rides 79.4 ->
    // 81.1 %, songs -0.2 % (recent and held out); 15 rows always open: nudges ~81 %, songs -1 % (the filter slides
    // onto off-beats in songs with live drift or off-beat accents).
    static constexpr int kMaxTempoRows = 25;
    int pfTempoRows = 1;
    double pfTempoStep = 0.004, pfTempoDiffusion = 0.01, pfTempoPrior = 1.0;
    double pfTempoDiffusionSteady = 0.0005;
    double pfOpenBand = 0.003, pfOpenMinConfidence = 0.12, pfOpenHoldSeconds = 4.0;
    // Kick-grid tempo (PLAN 5J.12, Peter's quick nudges): on-beat onsets (on-beat probability >= kgMinBeatness) while
    // a kick plays are numbered by beat (the first on the phase filter's beat, or any onset >= kgRestartBeatness;
    // then from their own fit, or along phi, within kgGate beats of a beat), and a line t = a + P n is fitted over
    // the last kgSeconds: 60 / P is the tempo, ~2 s behind the music instead of the autocorrelation's ~8 s. Trusted
    // with >= kgMinPoints onsets over >= kgMinBeats beats and an RMS residual < kgMaxResidual beats. Steady TEMPO
    // follows it (smoothing kgSmoothSeconds) when it is more than kgBand away, and the autocorrelation's drift
    // following waits a tempo window. Breakdowns: no kick, no update, the tempo holds. kgSeconds 0: off.
    // tools/beat_eval: nudges on the beat 74.9 -> 77.6 % (the nudges it catches, 2 in 3: 96.5 %; the true tempo fed
    // in: 96.1 %); songs, blends and rides unchanged.
    double kgSeconds = 5.0, kgMinBeatness = 0.5, kgGate = 0.12, kgBand = 0.003, kgSmoothSeconds = 1.0;
    int kgMinPoints = 6;
    double kgMinBeats = 6.0, kgMaxResidual = 0.04;
    double kgRestartBeatness = 0.8, kgLostSeconds = 3.0;  // restart from such an onset; start over after this gap (s)
    static constexpr int kFoldBins = 16, kFoldFeat = 18;
    double foldSeconds = 12.0;
    // Fitted 2026-11-15 on 509 k kick-section folds (foldSeconds 12) of 1,134 tuning songs: right half 92.7 % held out
    // (kick-band flux peak: 88.2 %); confidence >= 0.8 79 % of the time, then right 98.4 %.
    std::array<double, kFoldBins * kFoldFeat> foldW{
        0.1570, 0.0402, 0.1053, -0.1096, 0.1491, 0.1853, 0.0669, -0.0625, 0.0147, 0.0852, -0.0554, 0.0318, 0.0525, 0.1249, -0.1097, 0.1645, 0.1233, 0.1866, 0.1726, -0.0341, 0.2539, 0.2991, 0.2133, 0.2273, -0.0003, 0.1434, 0.1260, 0.0527, 0.0862, 0.1345, 0.0100, -0.0508, 0.1672, -0.1197, -0.3022, -0.3580, 0.0747, -0.1026, 0.2068, 0.0638, -0.0355, -0.1658, -0.2295, 0.0297, -0.0441, -0.2174, 0.0272, -0.0204, -0.3753, -0.5446, 0.1266, 0.3263, 0.0980, 0.2075, -0.1040, 0.0386, -0.3087, -0.0775, -0.0161, 0.2045, -0.0467, -0.0325, -0.1326, 0.1602, 0.0387, -0.2178, 0.0353, 0.2435, 0.2021, 0.3341, -0.0580, 0.3898, 0.1045, -0.1433, -0.0598, -0.0355, 0.0754, -0.1577, 0.0788, -0.1207, -0.0335, 0.0166, -0.0575, -0.0071, -0.0067, -0.0866, -0.0475, -0.0568, 0.1207, -0.0056, -0.1503, -0.0015, -0.1658, 0.0157, -0.0205, -0.0190, 0.0365, -0.0376, -0.1334, 0.2356, -0.0424, 0.0688, 0.0580, 0.0381, -0.3274, -0.4295, 0.1212, -0.2406, -0.0292, 0.1357, 0.0831, -0.0490, -0.1732, -0.0161, -0.0268, 0.0419, 0.0985, -0.1406, 0.0156, -0.1537, -0.1049, 0.1356, 0.2040, 0.0187, -0.1088, -0.1527, -0.0667, 0.0682, 0.0177, 0.0490, -0.1038, -0.0507, -0.1458, -0.0498, -0.0565, 0.0858, -0.0187, 0.0397, 0.0334, -0.1005, 0.0047, 0.0482, -0.4291, -0.1412, -0.0051, -0.0661, -0.0310, -0.1317, 0.2051, -0.0735, -0.2310, 0.1769, 0.0046, -0.0681, -0.1362, -0.1188, -0.0429, 0.1728, 0.1589, 0.0621, -0.4061, -0.1155, -0.0196, 0.0088, -0.2066, -0.0873, -0.1061, 0.0082, 0.4353, -0.0588, 0.0684, 0.0150, -0.0948, -0.0867, -0.0189, -0.1105, -0.2720, -0.2259, 0.6937, 0.3557, -0.0992, 0.0788, -0.1088, -0.0681, -0.0303, -0.1904, 0.2806, -0.2039, -0.0394, 0.0963, 0.0302, 0.0066, 0.2240, 0.1005, -0.2812, 0.0098, 0.2049, 0.5112, -0.0570, -0.0436, 0.0719, 0.0014, -0.1357, -0.2593, -0.0967, 0.0964, -0.2486, -0.1248, -0.0315, 0.1465, -0.0302, 0.3122, 0.1197, -0.3419, -0.1566, -0.4530, -0.0451, -0.0591, -0.0500, -0.1192, 0.0371, -0.0594, 0.0282, -0.1244, 0.2411, -0.1013, 0.0015, -0.0079, 0.1385, -0.3802, 0.1694, -0.1411, 0.0027, -0.1128, -0.1261, 0.0959, -0.0555, 0.0564, 0.0568, -0.1633, -0.0604, 0.1532, -0.0565, -0.0072, 0.1825, -0.0603, -0.1183, 0.1964, 0.1145, -0.1397, -0.0395, 0.3537, -0.2623, 0.1070, -0.1807, 0.0260, -0.1279, 0.0082, -0.1703, -0.1440, -0.0755, -0.2357, -0.1132, 0.0497, 0.2447, 0.1814, 0.0732, -0.1671, -0.5178, -0.4346, -0.2215, -0.0310, -0.0450, 0.0632, -0.1844, 0.0156, -0.1140, 0.0695, 0.0784, 0.1101, 0.0196, -0.0277, -0.0880, -0.1921, -0.3381, 0.5839, 0.3620, -0.1959};
};

class BeatTracker {
public:
    explicit BeatTracker(const BeatConfig& cfg = {}, double sampleRate = 48000.0);
    ~BeatTracker();
    BeatTracker(const BeatTracker&) = delete;
    BeatTracker& operator=(const BeatTracker&) = delete;

    void setSampleRate(double sampleRate);
    void reset();
    // Feed new samples (in order). Processes every completed hop.
    void push(const float* samples, size_t n);

    // Values at the current stream position (the end of the pushed samples).
    double beat() const;                // BEAT, 0..1
    double tempo() const { return tempo_; }  // TEMPO, BPM (0 = unsure)
    double confidence() const { return confidence_; }
    uint64_t onsetCount() const { return onsets_; }
    double lastOnsetSeconds() const { return lastOnset_; }  // stream time of the latest onset (-1: none)
    // How far the latest onset stood out: (strength - mean) / stddev over the threshold window (> thresholdK).
    double lastOnsetStrength() const { return lastOnsetStrength_; }
    // Share of the latest onset's spectral flux from the low band (band totals, 30-200 Hz of 30 Hz-4 kHz): 0..1;
    // ~0.4 for a kick drum, 0.05-0.3 for chord stabs and noise hits.
    double lastOnsetLowShare() const { return lastOnsetLowShare_; }
    // Steady mode's kick gate: a kick is playing (see above). Also for diagnostics.
    // How kick-like the latest onset was: its 30-120 Hz flux relative to the recent strongest onsets' (peak hold,
    // decaying over kickPeakSeconds), 0..1. ~1 for a kick, near 0 for hats, claps, vocals (real music; the share
    // above misses real kicks, whose click spreads over the mid band).
    double lastOnsetKickness() const { return lastOnsetKickness_; }
    // Onset features (PLAN 5J.12): the onset's spectral flux in kBands bands (30-60-120-250-500-1k-2k-4k-8k-16k Hz),
    // each as log(1 + flux / its recent mean), and the learnt on-beat probability from them (BeatConfig::beatness*).
    static constexpr int kBands = 9;
    const std::array<double, kBands>& lastOnsetBands() const { return lastOnsetBands_; }
    const std::array<double, kBands>& lastOnsetBandsNext() const { return lastOnsetBandsNext_; }  // the hop after
    // ... and around half a beat before the onset (at TEMPO; max over +-1 hop; zeros without a tempo)
    const std::array<double, kBands>& lastOnsetBandsHalf() const { return lastOnsetBandsHalf_; }
    double lastOnsetBeatness() const { return lastOnsetBeatness_; }
    // Kick phase (PLAN 5J.12): where the kick-band (30-120 Hz) flux sits in the beat at TEMPO, from a decaying
    // (kickPhaseSeconds) average of flux^kickPhasePower * e^(i 2 pi phi) over the hops; phi runs at TEMPO.
    // kickPhase(): the beat fraction at the end of the pushed audio, 0 = on a kick (-1 = no tempo / no data);
    // kickPhaseLock(): concentration 0..1 (1 = all the kick energy at one point of the beat).
    double kickPhase() const;
    double kickPhaseLock() const { return kpW_ > 1e-12 ? std::hypot(kpRe_, kpIm_) / kpW_ : 0.0; }
    bool kickPresent() const { return kickRef_ > 1e-6 && kickLevel_ >= cfg_.kickPresentRatio * kickRef_; }
    // Beat fold (BeatConfig::foldW): the learnt beat's fraction at the end of the pushed audio (-1: no tempo /
    // weights / data), and its confidence: softmax probability of the beat's half of the rotations (0.5..1) minus
    // that of the opposite half, 0..1.
    double foldPhase() const {
        if (foldBeatPhi_ < 0.0 || tempo_ <= 0.0) return -1.0;
        const double p = kpPhi_ - foldBeatPhi_;
        return p - std::floor(p);
    }
    double foldConfidence() const { return foldConf_; }
    // Phase filter: its most likely beat's fraction at the end of the pushed audio (-1: no tempo), and confidence:
    // the probability within 0.1 beat of it (0..1).
    double filterPhase() const {
        if (pfBeatPhi_ < 0.0 || tempo_ <= 0.0) return -1.0;
        const double p = kpPhi_ - pfBeatPhi_;
        return p - std::floor(p);
    }
    double filterConfidence() const { return pfConf_; }
    double kickGridTempo() const { return kgTempo_; }  // the kick-grid tempo (BPM), 0: none
    // Diagnostics (tools/beat_eval --oracle-tempo): replace TEMPO (once there is one) by the true tempo.
    void overrideTempo(double bpm) { if (tempo_ > 0.0) tempo_ = bpm; }
    // The phase filter's tempo (BPM; 0: none): TEMPO plus the filter's own estimate of how far the beat is drifting.
    double filterTempo() const { return pfBeatPhi_ < 0.0 || tempo_ <= 0.0 ? 0.0 : pfRef_ * (1.0 + pfTempoOff_); }
    // Raw fold (bin means, kFoldBins x kFoldFeat, bin-major; bin 0 = phi in [0, 1/kFoldBins)) and phi (for training).
    std::array<float, BeatConfig::kFoldBins * BeatConfig::kFoldFeat> foldProfile() const;
    double foldPhi() const { return kpPhi_; }
    double kickLevelRatio() const { return kickRef_ > 1e-6 ? kickLevel_ / kickRef_ : 0.0; }
    double streamSeconds() const { return double(samples_) / sampleRate_; }
    double onsetStrength() const { return lastStrength_; }
    // Diagnostics (tools/beat_eval): the tempo search's raw autocorrelation at `bpm`'s lag and at 4x it, on the
    // current window (best of +-1 hop; parabolic peak not refined). {r1, r4}; {0, 0} without enough data.
    std::array<double, 2> debugAcf(double bpm) const;

private:
    struct Impl;
    void rebuild();
    void processHop();
    void updateTempo();

    BeatConfig cfg_;
    double sampleRate_ = 48000.0;
    std::unique_ptr<Impl> impl_;

    uint64_t samples_ = 0;   // total samples pushed
    uint64_t onsets_ = 0;
    double lastOnset_ = -1.0;
    double lastOnsetStrength_ = 0.0;
    double lastStrength_ = 0.0;
    double tempo_ = 0.0, confidence_ = 0.0;
    double lastConfident_ = -1.0;
    double candidate_ = 0.0;
    double switchBpm_ = 0.0, switchEvidence_ = 0.0;
    double acquiredAt_ = -1.0;
    double fastRun_ = 0.0;  // steady: seconds the local reading has been > steadyFastBand away, signed by direction  // stream time the current tempo was first found (from 0)  // steady: the challenger and its evidence (seconds)
    double lastSound_ = 0.0;                          // stream time of the latest non-silent hop
    double lastOnsetLowShare_ = 0.0, hopLowShare_ = 0.0;
    double kickLevel_ = 0.0, kickRef_ = 0.0;  // kick gate
    double hopKick_ = 0.0, kickPeak_ = 0.0, kickPeakT_ = 0.0, lastOnsetKickness_ = 0.0;
    double kpPhi_ = 0.0, kpRe_ = 0.0, kpIm_ = 0.0, kpW_ = 0.0;  // kick phase
    std::array<double, kBands> bandMean_{}, hopBands_{}, lastOnsetBands_{}, lastOnsetBandsNext_{}, lastOnsetBandsHalf_{};
    std::array<std::array<float, kBands>, 256> bandHist_{};  // hop band features, ring by hop index
    double lastOnsetBeatness_ = 0.5;
    std::array<double, BeatConfig::kFoldBins * BeatConfig::kFoldFeat> fold_{};  // decayed sums
    std::array<double, BeatConfig::kFoldBins> foldN_{};                        // decayed weights
    double foldBeatPhi_ = -1.0, foldConf_ = 0.0, nextFoldScore_ = 0.0;  // the learnt beat on phi's circle
    void scoreFold();
    // Phase filter belief, row-major (tempo row x phase state; sums to 1), each row's sub-state drift, the tempo row
    // 0 is defined against (pfRef_ * (1 + (k - center) * step)), and the estimated offset from pfRef_.
    std::array<double, BeatConfig::kPhaseStates * BeatConfig::kMaxTempoRows> pf_{}, pfNext_{};
    std::array<double, BeatConfig::kMaxTempoRows> pfAcc_{};
    double pfRef_ = 0.0, pfTempoOff_ = 0.0;
    // Kick grid: ring of (onset time, beat number), its fit's tempo, when it was last good, and how long it leads.
    static constexpr int kKgMax = 48;
    std::array<double, kKgMax> kgT_{}, kgN_{};
    int kgHead_ = 0, kgCount_ = 0, kgMiss_ = 0;
    double kgA_ = 0.0, kgP_ = 0.0, kgTempo_ = 0.0, kgGood_ = -1.0, kgLeadUntil_ = -1.0, kpCum_ = 0.0, kgCumOff_ = 0.0;
    void addKickGrid(double t, double cum, double beatness);
    double pfOpenUntil_ = -1.0;  // stream time until which the filter's tempo dimension is open (the acf reads a move)
    std::array<double, BeatConfig::kFoldBins> foldProb_{};  // the fold's latest softmax (beat at bin r)
    bool foldFresh_ = false;
    double pfTempo_ = 0.0, pfActMean_ = 0.0, pfBeatPhi_ = -1.0, pfConf_ = 0.0;
    double pfTempoAvg_ = 0.0;  // TEMPO's recent average (pfMovingSeconds)
    void stepFilter(double act, double phiAct, double hopSeconds);
    double nextTempoUpdate_ = 0.0;
    std::array<double, 64> onsetTimes_{};  // ring of the latest onset times
};

}  // namespace reforce::audio
