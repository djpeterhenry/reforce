#pragma once
// Audio analyzer: turns a mono sample stream into one AudioFrame per render
// frame (wave[] for mag(), fft[] for fft(), the BASS / LEVEL / ENERGY scalars
// and the reforce extensions MID / TREBLE / BEAT / TEMPO).
//
// Threading: an Analyzer belongs to one thread (the render thread). Each frame:
//     analyzer.consume(source.ring());          // drain new samples (lock-free)
//     const AudioFrame& f = analyzer.analyze(dt);
// consume() is the SPSC consumer side; nothing here touches the audio thread.
// setSampleRate() and the constructor allocate; consume()/analyze() do not.
//
// All time constants are in seconds and applied per call as one-pole filters
// with coefficient 1 - exp(-dt / tau), so behavior is frame-rate independent.
// `dt` is the caller's frame time; in deterministic mode pass N / sampleRate.
//
// The defaults are FITTED to G-Force 5.9.7 measurements (Phase 5C.4; numbers
// and method in docs/audio-probe-results.md, parity in docs/audio-parity.md).
// G-Force facts the model encodes:
//   * mag() is the PCM sample (gain 1, no auto-gain) through a 7-point moving
//     average (lowpass: -6 dB near 4 kHz); a full-scale 1 kHz sine reads ~+-0.96.
//   * fft() bins are log-spaced from 120 Hz to 5.5 kHz (G-Force's stand-alone
//     SetFFTParams(120, 5500, ...) default; Audio.FFT.FreqLo/FreqHi override),
//     broad (a pure tone lights +-1 octave and more), normalized per band group
//     against a slowly adapting reference, and clamped at ~0.93.
//   * LEVEL is the mean of fft() (x ~1.34), BASS = 4 x LEVEL (it is not a
//     low-band measure in 5.9.7), ENERGY is a slow (~1.5-2 s) follower of
//     LEVEL's recent peaks, clamped to 1.
// Behavior the model does not reproduce (non-stationary fft() shapes for pure
// tones and sweeps) is listed in docs/audio-probe-results.md.

#include "audio/BeatTracker.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace reforce::audio {

class SpscRingBuffer;

// G-Force pref defaults (docs/gforce-defaults.md).
inline constexpr int kDefaultSampleBins = 255;  // Audio.PCM.NumBins  -> NUM_SAMPLE_BINS
inline constexpr int kDefaultFftBins = 255;     // Audio.FFT.NumBins.vis -> NUM_FFT_BINS
inline constexpr const char* kGForceFftParams =
    "FloorRaiseMax=.7 Envelopes=16 InitialVolume=1.0 ShortImpulse=0.60 ShortWindow=0.3 "
    "ShortRamp=44.0 LongImpulse=0.59 LongWindow=0.043 LongRamp=185.0 ExciterImpulse=1.2 "
    "PersistCoef=1.0 PersistLowThresh=.035";
// G-Force stand-alone analysis range when Audio.FFT.FreqLo/FreqHi are 0 (auto):
// SSEngine.py calls SetFFTParams(120, 5500, 8, .02, .07, False).
inline constexpr double kGForceFreqLoHz = 120.0;
inline constexpr double kGForceFreqHiHz = 5500.0;

struct AnalyzerConfig {
    // ---- PCM (mag) ---------------------------------------------------------
    int sampleBins = kDefaultSampleBins;  // NUM_SAMPLE_BINS
    // wave[] holds the most recent sampleBins * pcmStride input samples,
    // decimated by pcmStride, oldest first (wave[0] = mag(0)).
    int pcmStride = 1;
    // Optional inter-frame smoothing (reforce): wave = lerp(new, previous,
    // exp(-dt / pcmSmoothSeconds)); 0 disables. Not driven by Audio.PCM.Smooth:
    // G-Force at its default 0.008 shows no inter-frame smoothing (the RMS of
    // mag(.5) for a steady tone equals the raw sine's; docs/audio-probe-results.md).
    double pcmSmoothSeconds = 0.0;
    // G-Force: mag() = sample * 1 (measured; no auto-gain). pcmAutoGain is a
    // reforce option: sample * pcmGain / max(peakEnvelope, pcmAgcFloor) with an
    // instant-attack, pcmAgcReleaseSeconds-release peak envelope.
    // G-Force smooths mag() with a causal moving average of this many points (zero history at the window's
    // oldest end; measured at 44.1 kHz: 7). 1 = raw samples.
    int pcmBoxcar = 7;
    bool pcmAutoGain = false;
    double pcmGain = 1.0;
    double pcmAgcFloor = 0.25;
    double pcmAgcReleaseSeconds = 3.0;

    // ---- Spectrum (fft) ----------------------------------------------------
    int fftBins = kDefaultFftBins;  // NUM_FFT_BINS (vis bins)
    int fftSize = 2048;             // analysis window, power of two (Hann)
    double freqLoHz = 0.0;          // Audio.FFT.FreqLo; 0 = auto (kGForceFreqLoHz)
    double freqHiHz = 0.0;          // Audio.FFT.FreqHi; 0 = auto (kGForceFreqHiHz, capped below Nyquist)
    bool linearSpacing = false;     // bin centers linear in Hz instead of log
    // Band amplitude of vis bin j (center f_j, f_j = lo * (hi/lo)^(j/(bins-1))):
    //   a_j = sqrt(sum_k w(log2(f_k / f_j) / bandOctaves) * power_k) * (f_j / 1 kHz)^tilt,
    //   w(d) = 1 / (1 + |d|^bandShape)  (peak 1; long tails make tones broad as in G-Force).
    // power_k: Hann power spectrum scaled so a full-scale sine at a bin center has power 1.
    double bandOctaves = 0.428;
    double bandShape = 2.20;
    double tilt = -1.189;
    // Normalization: the bins are split into `envelopes` contiguous groups. Each
    // group's mean amplitude x drives a reference R (attack refAttackSeconds when
    // x > R, release refReleaseSeconds when x < R), R >= refMin, starting at
    // refInitial. Per bin (R interpolated between group centers):
    //   v = fftGain * a / R,   then per-bin smoothing (instant attack, one-pole
    //   release fftReleaseSeconds * PersistCoef), v < fftLowThresh -> 0,
    //   fft = min(v, fftMax) * responseScale.
    int envelopes = 16;               // Audio.FFT.Params Envelopes
    double refAttackSeconds = 0.0396;
    double refReleaseSeconds = 7.41;
    double refInitial = 0.517;        // scaled by 1 / InitialVolume
    double refMin = 0.00122;          // ~ -58 dBFS band amplitude
    double fftGain = 0.5275;
    double fftMax = 0.93;
    double fftLowThresh = 0.0;
    double fftReleaseSeconds = 1.0 / 60.0;  // Audio.FFT.Smooth (frames @ 60 Hz) / 60; G-Force default 1
    double responseScale = 1.0;       // Audio.Response.Scale
    // Audio.FFT.Params keys, parsed (applyGForceFftParams). Envelopes,
    // InitialVolume and PersistCoef drive the model above; the others are kept for
    // reference only (the fitted model has no equivalent; see the doc).
    double floorRaiseMax = 0.7, initialVolume = 1.0, shortImpulse = 0.60, shortWindow = 0.3,
           shortRamp = 44.0, longImpulse = 0.59, longWindow = 0.043, longRamp = 185.0,
           exciterImpulse = 1.2, persistCoef = 1.0, persistLowThresh = 0.035;

    // ---- LEVEL / BASS / ENERGY (G-Force) ------------------------------------
    double levelGain = 1.343;    // LEVEL = min(1, levelGain * mean(fft))
    double bassGain = 4.0;      // BASS = min(bassMax, bassGain * LEVEL)
    double bassMax = 4.0;
    // ENERGY: p = peak-hold of LEVEL (instant attack, energyPeakSeconds decay);
    // ENERGY follows energyGain * p with energyAttackSeconds (rising) /
    // energyReleaseSeconds (falling), clamped to [0, energyMax].
    double energyGain = 2.96;
    double energyPeakSeconds = 0.205;
    double energyAttackSeconds = 2.52;
    double energyReleaseSeconds = 1.43;
    double energyMax = 1.0;

    // ---- reforce extensions: MID / TREBLE -----------------------------------
    // Band amplitude a from the raw power spectrum in [lo, hi) Hz (not the fft()
    // bins), normalized against its own peak envelope (attack bandRefAttack,
    // release bandRefRelease, at least bandRefMin), smoothed (bandAttack /
    // bandRelease) and clamped to [0, bandMax]. A band at its recent loudest
    // reads ~1, quiet passages ~0; scale like BASS (0..~2).
    double midLoHz = 250.0, midHiHz = 2000.0;
    double trebleLoHz = 2000.0, trebleHiHz = 16000.0;
    double bandRefAttackSeconds = 0.1;
    double bandRefReleaseSeconds = 4.0;
    double bandRefMin = 0.02;
    double bandAttackSeconds = 0.01;
    double bandReleaseSeconds = 0.12;
    double bandMax = 2.0;

    // ---- reforce extensions: BEAT / TEMPO ----------------------------------
    BeatConfig beat;

    // If analyze() is called with no new samples for this long (device
    // stalled/disconnected), the analyzer feeds itself silence so outputs decay.
    double starveSeconds = 0.25;
};

// Parse a G-Force `Audio.FFT.Params` string ("Key=value Key=value ...", keys
// case-insensitive) into cfg. Returns the keys it did not recognize, space
// separated (empty when all were used).
std::string applyGForceFftParams(AnalyzerConfig& cfg, std::string_view params);

struct AudioFrame {
    std::vector<float> wave;     // NUM_SAMPLE_BINS, ~-1..1, oldest first
    std::vector<float> fft;      // NUM_FFT_BINS, 0..fftMax (normalized, smoothed)
    std::vector<float> fftRaw;   // NUM_FFT_BINS, band amplitude a_j before normalization
    std::vector<float> binLoHz;  // lower edge of each fft bin (geometric midpoints)
    std::vector<float> binHiHz;  // upper edge of each fft bin
    float bass = 0.f;    // BASS
    float level = 0.f;   // LEVEL
    float energy = 0.f;  // ENERGY
    float mid = 0.f;     // MID    (reforce extension)
    float treble = 0.f;  // TREBLE (reforce extension)
    float beat = 0.f;    // BEAT   (reforce extension), onset pulse 0..1
    float beatStrength = 0.f;  // how far the latest onset stood out (BeatTracker::lastOnsetStrength)
    float beatLowShare = 0.f;  // share of the latest onset from the kick band (BeatTracker::lastOnsetLowShare)
    float kickPhase = -1.f;     // kick band's beat fraction now, 0 = on a kick (BeatTracker::kickPhase); < 0: none
    float kickPhaseLock = 0.f;  // its concentration (BeatTracker::kickPhaseLock)
    float beatFoldPhase = -1.f; // the learnt beat's fraction now (BeatTracker::foldPhase); < 0: none
    float beatFoldConf = 0.f;   // its confidence (BeatTracker::foldConfidence)
    float beatFilterPhase = -1.f; // the phase filter's beat fraction now (BeatTracker::filterPhase); < 0: none
    float beatFilterConf = 0.f;   // its confidence (BeatTracker::filterConfidence)
    float beatKickness = -1.f;
    float beatBeatness = -1.f;  // the latest onset's learnt on-beat probability (BeatTracker::lastOnsetBeatness) // the latest onset's kick-ness 0..1 (BeatTracker::lastOnsetKickness)
    bool kickPresent = true;   // a kick is playing (BeatTracker::kickPresent; steady tempo's breakdown gate)
    float tempo = 0.f;   // TEMPO  (reforce extension), BPM, 0 = unsure
    // Ableton Link (filled by the app, not the analyzer; sync/LinkClock.hpp). With a
    // Link session (linkOn), BEAT/TEMPO above follow Link when reforce.Link.DriveBeat.
    bool linkEnabled = false;  // reforce.Link.Enabled and running
    bool linkListening = false; // follow mode: no other Link app heard yet (Link off)
    bool linkJoining = false;   // follow mode: another Link app heard, session forming
    int linkPeers = 0;         // other Link apps in the session
    bool linkOn = false;       // LINK_ON: enabled and >= 1 peer
    double linkBpm = 0.0;      // LINK_BPM (0 when !linkOn)
    double linkBeat = 0.0;     // LINK_BEAT: running beat count (session timeline)
    double linkPhase = 0.0;    // LINK_PHASE: 0..quantum, position in the bar
    double linkQuantum = 4.0;
    float rms = 0.f;     // unsmoothed RMS of this frame's samples
    float peak = 0.f;    // max |sample| consumed this frame
    double sampleRate = 0.0;
    double timeSeconds = 0.0;     // sum of dt passed to analyze()
    uint64_t samplesConsumed = 0; // total samples taken in (ring + push)
    uint64_t frameIndex = 0;
};

class Analyzer {
public:
    explicit Analyzer(const AnalyzerConfig& cfg = {}, double sampleRate = 48000.0);
    ~Analyzer();
    Analyzer(const Analyzer&) = delete;
    Analyzer& operator=(const Analyzer&) = delete;

    // Rebuilds bin tables and resets state when the rate changes (allocates).
    void setSampleRate(double sampleRate);
    double sampleRate() const { return sampleRate_; }
    const AnalyzerConfig& config() const { return cfg_; }

    // Drain everything available in ring into the analysis history.
    size_t consume(SpscRingBuffer& ring);
    // Feed samples directly (tests, or a source that isn't a ring).
    void pushSamples(const float* samples, size_t n);

    // Compute this frame's AudioFrame. dtSeconds > 0.
    const AudioFrame& analyze(double dtSeconds);
    const AudioFrame& frame() const { return frame_; }

    // Clear history and all envelopes (keeps config and sample rate).
    void reset();

    // Index of the fft bin whose [lo, hi) range contains hz, or -1.
    int binForFrequency(double hz) const;
    // Center frequency of fft bin i.
    double binCenterHz(int i) const;

private:
    struct Impl;
    void rebuild();
    void writeHistory(const float* s, size_t n);
    void copyRecent(float* dst, size_t n) const;

    AnalyzerConfig cfg_;
    double sampleRate_ = 48000.0;
    std::unique_ptr<Impl> impl_;
    AudioFrame frame_;
};

}  // namespace reforce::audio
