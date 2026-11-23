#include "audio/Analyzer.hpp"

#include "audio/SpscRingBuffer.hpp"

#include <Accelerate/Accelerate.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

namespace reforce::audio {

namespace {

double onePoleK(double dt, double tau) {
    if (tau <= 0.0) return 1.0;
    return 1.0 - std::exp(-dt / tau);
}

// Attack/release follower step.
double follow(double cur, double target, double dt, double attackTau, double releaseTau) {
    const double k = onePoleK(dt, target > cur ? attackTau : releaseTau);
    return cur + k * (target - cur);
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

size_t nextPow2(size_t n) {
    size_t p = 1;
    while (p < n) p <<= 1;
    return p;
}

}  // namespace

std::string applyGForceFftParams(AnalyzerConfig& cfg, std::string_view params) {
    std::string unknown;
    size_t i = 0;
    while (i < params.size()) {
        while (i < params.size() && std::isspace(static_cast<unsigned char>(params[i]))) ++i;
        size_t j = i;
        while (j < params.size() && !std::isspace(static_cast<unsigned char>(params[j]))) ++j;
        if (j == i) break;
        std::string_view tok = params.substr(i, j - i);
        i = j;
        const size_t eq = tok.find('=');
        if (eq == std::string_view::npos) {
            if (!unknown.empty()) unknown += ' ';
            unknown += tok;
            continue;
        }
        const std::string key = lower(tok.substr(0, eq));
        const std::string valStr(tok.substr(eq + 1));
        char* end = nullptr;
        const double v = std::strtod(valStr.c_str(), &end);
        const bool ok = end && end != valStr.c_str();
        bool used = ok;
        if (!ok) {
        } else if (key == "floorraisemax") cfg.floorRaiseMax = v;
        else if (key == "envelopes") cfg.envelopes = std::max(1, static_cast<int>(v));
        else if (key == "initialvolume") cfg.initialVolume = v;
        else if (key == "shortimpulse") cfg.shortImpulse = v;
        else if (key == "shortwindow") cfg.shortWindow = v;
        else if (key == "shortramp") cfg.shortRamp = v;
        else if (key == "longimpulse") cfg.longImpulse = v;
        else if (key == "longwindow") cfg.longWindow = v;
        else if (key == "longramp") cfg.longRamp = v;
        else if (key == "exciterimpulse") cfg.exciterImpulse = v;
        else if (key == "persistcoef") cfg.persistCoef = v;
        else if (key == "persistlowthresh") cfg.persistLowThresh = v;
        else used = false;
        if (!used) {
            if (!unknown.empty()) unknown += ' ';
            unknown += tok;
        }
    }
    return unknown;
}

struct Analyzer::Impl {
    // History (circular, power-of-two).
    std::vector<float> hist;
    size_t histMask = 0;
    uint64_t histWrite = 0;
    size_t newSinceAnalyze = 0;
    float peakSinceAnalyze = 0.f;
    std::vector<float> chunk;   // consume() staging
    std::vector<float> zeros;   // starvation fill
    std::vector<float> recent;  // linear copy of recent samples (size hist)

    // FFT.
    FFTSetup setup = nullptr;
    vDSP_Length log2n = 0;
    size_t n = 0;
    std::vector<float> window, timeBuf, re, im, power;
    float ampScale = 1.f;  // full-scale sine -> magnitude 1

    // Vis bins: center frequencies and sparse band weights over FFT bins.
    std::vector<double> centerHz;
    std::vector<int> wFirst, wCount;  // per vis bin: first FFT bin, number of weights
    std::vector<float> weights;       // concatenated
    std::vector<float> tiltGain;
    // Envelope groups: per bin interpolation between group centers.
    std::vector<int> groupStart, groupEnd;
    std::vector<int> binG0, binG1;
    std::vector<float> binT;
    std::vector<double> gRef;
    std::vector<float> fftState;  // per-bin smoothed value (before fftMax / responseScale)

    // Scalars.
    double levelPeak = 0.0, energy = 0.0;
    struct Band {
        int k0 = 1, k1 = 0;
        double ref = 0.0, value = 0.0;
    } mid, treble;
    // PCM.
    double peakEnv = 0.0;
    bool waveInit = false;
    std::vector<float> waveNew;

    double starved = 0.0;
    BeatTracker beat;

    explicit Impl(const BeatConfig& bc, double sr) : beat(bc, sr) {}
    ~Impl() {
        if (setup) vDSP_destroy_fftsetup(setup);
    }
};

Analyzer::Analyzer(const AnalyzerConfig& cfg, double sampleRate)
    : cfg_(cfg), sampleRate_(sampleRate), impl_(std::make_unique<Impl>(cfg.beat, sampleRate)) {
    cfg_.sampleBins = std::max(1, cfg_.sampleBins);
    cfg_.pcmStride = std::max(1, cfg_.pcmStride);
    cfg_.fftBins = std::max(2, cfg_.fftBins);
    cfg_.fftSize = static_cast<int>(nextPow2(static_cast<size_t>(std::max(64, cfg_.fftSize))));
    cfg_.envelopes = std::clamp(cfg_.envelopes, 1, cfg_.fftBins);
    rebuild();
}

Analyzer::~Analyzer() = default;

void Analyzer::setSampleRate(double sampleRate) {
    if (sampleRate <= 0.0 || sampleRate == sampleRate_) return;
    sampleRate_ = sampleRate;
    impl_->beat.setSampleRate(sampleRate);
    rebuild();
}

void Analyzer::rebuild() {
    Impl& m = *impl_;
    const size_t histSize = nextPow2(std::max<size_t>(
        {static_cast<size_t>(cfg_.fftSize),
         static_cast<size_t>(cfg_.sampleBins) * static_cast<size_t>(cfg_.pcmStride),
         static_cast<size_t>(sampleRate_ / 4)}));
    m.hist.assign(histSize, 0.f);
    m.histMask = histSize - 1;
    m.recent.assign(histSize, 0.f);
    m.chunk.assign(4096, 0.f);
    m.zeros.assign(4096, 0.f);

    // FFT setup.
    m.n = static_cast<size_t>(cfg_.fftSize);
    m.log2n = static_cast<vDSP_Length>(std::lround(std::log2(static_cast<double>(m.n))));
    if (m.setup) vDSP_destroy_fftsetup(m.setup);
    m.setup = vDSP_create_fftsetup(m.log2n, kFFTRadix2);
    m.window.assign(m.n, 0.f);
    vDSP_hann_window(m.window.data(), m.n, vDSP_HANN_DENORM);  // periodic, peak 1, mean 0.5
    m.timeBuf.assign(m.n, 0.f);
    m.re.assign(m.n / 2, 0.f);
    m.im.assign(m.n / 2, 0.f);
    m.power.assign(m.n / 2, 0.f);
    // vDSP_fft_zrip returns 2x the DFT; Hann coherent gain is 0.5:
    // a sine of amplitude A at a bin center gives |X| = A * N / 2.
    m.ampScale = 2.f / static_cast<float>(m.n);

    // Vis bin centers.
    const double nyq = sampleRate_ * 0.5;
    double lo = cfg_.freqLoHz > 0 ? cfg_.freqLoHz : kGForceFreqLoHz;
    double hi = cfg_.freqHiHz > 0 ? cfg_.freqHiHz : kGForceFreqHiHz;
    hi = std::min(hi, nyq * 0.95);
    lo = std::clamp(lo, 1.0, hi * 0.5);
    const int nb = cfg_.fftBins;
    m.centerHz.assign(nb, 0.0);
    for (int i = 0; i < nb; ++i) {
        const double s = double(i) / double(nb - 1);
        m.centerHz[i] = cfg_.linearSpacing ? lo + (hi - lo) * s : lo * std::pow(hi / lo, s);
    }
    frame_.binLoHz.assign(nb, 0.f);
    frame_.binHiHz.assign(nb, 0.f);
    for (int i = 0; i < nb; ++i) {
        const double c = m.centerHz[i];
        const double prev = i > 0 ? m.centerHz[i - 1] : (cfg_.linearSpacing ? 2 * c - m.centerHz[1] : c * c / m.centerHz[1]);
        const double next = i + 1 < nb ? m.centerHz[i + 1]
                                       : (cfg_.linearSpacing ? 2 * c - m.centerHz[i - 1] : c * c / m.centerHz[i - 1]);
        frame_.binLoHz[i] = float(cfg_.linearSpacing ? 0.5 * (prev + c) : std::sqrt(std::max(prev, 1e-9) * c));
        frame_.binHiHz[i] = float(cfg_.linearSpacing ? 0.5 * (c + next) : std::sqrt(c * next));
    }

    // Band weights w(log2(f_k / f_j) / bandOctaves), peak-normalized; tiny tails dropped.
    const double df = sampleRate_ / static_cast<double>(m.n);
    const int kTop = static_cast<int>(m.n / 2) - 1;
    m.wFirst.assign(nb, 1);
    m.wCount.assign(nb, 0);
    m.weights.clear();
    m.tiltGain.assign(nb, 1.f);
    const double bw = std::max(1e-3, cfg_.bandOctaves);
    for (int i = 0; i < nb; ++i) {
        const double fc = m.centerHz[i];
        int first = -1, last = -1;
        std::vector<float> w;  // rebuild() may allocate
        double wmax = 0.0;
        for (int k = 1; k <= kTop; ++k) {
            const double d = std::log2(k * df / fc) / bw;
            const double v = 1.0 / (1.0 + std::pow(std::abs(d), cfg_.bandShape));
            wmax = std::max(wmax, v);
        }
        for (int k = 1; k <= kTop; ++k) {
            const double d = std::log2(k * df / fc) / bw;
            const double v = 1.0 / (1.0 + std::pow(std::abs(d), cfg_.bandShape)) / wmax;
            if (v < 1e-4) {
                if (first >= 0) break;  // past the band (weights fall monotonically away from the center)
                continue;
            }
            if (first < 0) first = k;
            last = k;
            w.push_back(float(v));
        }
        if (first < 0) {  // degenerate: nearest bin
            first = last = std::clamp(int(std::lround(fc / df)), 1, kTop);
            w.assign(1, 1.f);
        }
        m.wFirst[i] = first;
        m.wCount[i] = last - first + 1;
        m.weights.insert(m.weights.end(), w.begin(), w.end());
        m.tiltGain[i] = float(std::pow(fc / 1000.0, cfg_.tilt));
    }

    // Envelope groups and per-bin interpolation between group centers.
    const int ng = cfg_.envelopes;
    m.groupStart.assign(ng, 0);
    m.groupEnd.assign(ng, 0);
    std::vector<double> center(ng);
    for (int g = 0; g < ng; ++g) {
        m.groupStart[g] = g * nb / ng;
        m.groupEnd[g] = (g + 1) * nb / ng;
        center[g] = 0.5 * (m.groupStart[g] + m.groupEnd[g] - 1);
    }
    m.binG0.assign(nb, 0);
    m.binG1.assign(nb, 0);
    m.binT.assign(nb, 0.f);
    for (int i = 0; i < nb; ++i) {
        int g = 0;
        while (g + 1 < ng && center[g + 1] <= i) ++g;
        if (g + 1 >= ng || i <= center[0]) {
            m.binG0[i] = m.binG1[i] = (i <= center[0]) ? 0 : ng - 1;
            m.binT[i] = 0.f;
        } else {
            m.binG0[i] = g;
            m.binG1[i] = g + 1;
            m.binT[i] = static_cast<float>((i - center[g]) / (center[g + 1] - center[g]));
        }
    }
    m.gRef.assign(ng, 0.0);
    m.fftState.assign(nb, 0.f);

    auto band = [&](double loHz, double hiHz) {
        Impl::Band b;
        b.k0 = std::clamp(static_cast<int>(std::ceil(loHz / df)), 1, kTop);
        b.k1 = std::clamp(static_cast<int>(std::floor(std::min(hiHz, nyq * 0.999) / df)), 1, kTop);
        if (b.k1 < b.k0) b.k1 = b.k0;
        return b;
    };
    m.mid = band(cfg_.midLoHz, cfg_.midHiHz);
    m.treble = band(cfg_.trebleLoHz, cfg_.trebleHiHz);

    frame_.wave.assign(cfg_.sampleBins, 0.f);
    m.waveNew.assign(cfg_.sampleBins, 0.f);
    frame_.fft.assign(nb, 0.f);
    frame_.fftRaw.assign(nb, 0.f);
    frame_.sampleRate = sampleRate_;
    reset();
}

void Analyzer::reset() {
    Impl& m = *impl_;
    std::fill(m.hist.begin(), m.hist.end(), 0.f);
    m.histWrite = 0;
    m.newSinceAnalyze = 0;
    m.peakSinceAnalyze = 0.f;
    const double init = std::max(cfg_.refInitial / std::max(1e-6, cfg_.initialVolume), cfg_.refMin);
    std::fill(m.gRef.begin(), m.gRef.end(), init);
    m.levelPeak = m.energy = 0.0;
    m.mid.ref = m.mid.value = m.treble.ref = m.treble.value = 0.0;
    m.peakEnv = 0.0;
    m.waveInit = false;
    m.starved = 0.0;
    m.beat.reset();
    std::fill(frame_.wave.begin(), frame_.wave.end(), 0.f);
    std::fill(frame_.fft.begin(), frame_.fft.end(), 0.f);
    std::fill(m.fftState.begin(), m.fftState.end(), 0.f);
    std::fill(frame_.fftRaw.begin(), frame_.fftRaw.end(), 0.f);
    frame_.bass = frame_.level = frame_.energy = frame_.rms = frame_.peak = 0.f;
    frame_.mid = frame_.treble = frame_.beat = frame_.tempo = 0.f;
    frame_.timeSeconds = 0.0;
    frame_.samplesConsumed = 0;
    frame_.frameIndex = 0;
}

void Analyzer::writeHistory(const float* s, size_t n) {
    Impl& m = *impl_;
    m.beat.push(s, n);
    const size_t cap = m.hist.size();
    float pk = m.peakSinceAnalyze;
    if (n > cap) {  // only the most recent `cap` samples matter for analysis
        for (size_t i = 0; i < n - cap; ++i) pk = std::max(pk, std::fabs(s[i]));
        const size_t skip = n - cap;
        m.histWrite += skip;
        s += skip;
        n = cap;
    }
    for (size_t i = 0; i < n; ++i) {
        const float v = s[i];
        m.hist[(m.histWrite + i) & m.histMask] = v;
        pk = std::max(pk, std::fabs(v));
    }
    m.histWrite += n;
    m.peakSinceAnalyze = pk;
}

void Analyzer::pushSamples(const float* samples, size_t n) {
    Impl& m = *impl_;
    writeHistory(samples, n);
    m.newSinceAnalyze += n;
    frame_.samplesConsumed += n;
}

size_t Analyzer::consume(SpscRingBuffer& ring) {
    Impl& m = *impl_;
    size_t total = 0;
    for (;;) {
        const size_t got = ring.read(m.chunk.data(), m.chunk.size());
        if (got == 0) break;
        pushSamples(m.chunk.data(), got);
        total += got;
    }
    return total;
}

void Analyzer::copyRecent(float* dst, size_t n) const {
    const Impl& m = *impl_;
    const uint64_t start = m.histWrite - n;  // may "underflow" before enough data: hist is zero there
    for (size_t i = 0; i < n; ++i) dst[i] = m.hist[(start + i) & m.histMask];
}

int Analyzer::binForFrequency(double hz) const {
    for (size_t i = 0; i < frame_.binLoHz.size(); ++i)
        if (hz >= frame_.binLoHz[i] && hz < frame_.binHiHz[i]) return static_cast<int>(i);
    return -1;
}

double Analyzer::binCenterHz(int i) const {
    const auto& c = impl_->centerHz;
    return c.empty() ? 0.0 : c[size_t(std::clamp(i, 0, int(c.size()) - 1))];
}

const AudioFrame& Analyzer::analyze(double dt) {
    Impl& m = *impl_;
    if (!(dt > 0.0)) dt = 1.0 / 60.0;

    // Starvation: feed silence so outputs decay when the source stops.
    if (m.newSinceAnalyze == 0) {
        m.starved += dt;
        if (m.starved > cfg_.starveSeconds) {
            size_t fill = std::min(static_cast<size_t>(dt * sampleRate_), m.hist.size());
            while (fill > 0) {
                const size_t c = std::min(fill, m.zeros.size());
                writeHistory(m.zeros.data(), c);
                m.newSinceAnalyze += c;
                fill -= c;
            }
        }
    } else {
        m.starved = 0.0;
    }

    // ---- frame RMS (diagnostics) ---------------------------------------------
    {
        const size_t lvlN = std::clamp<size_t>(m.newSinceAnalyze, 1, m.hist.size());
        copyRecent(m.recent.data(), lvlN);
        float ms = 0.f;
        vDSP_measqv(m.recent.data(), 1, &ms, lvlN);
        frame_.rms = std::sqrt(ms);
        frame_.peak = m.peakSinceAnalyze;
    }

    // ---- wave[] (mag) ------------------------------------------------------
    {
        const size_t nb = static_cast<size_t>(cfg_.sampleBins);
        const size_t stride = static_cast<size_t>(cfg_.pcmStride);
        const size_t span = nb * stride;
        copyRecent(m.recent.data(), span);
        m.peakEnv = std::max(static_cast<double>(m.peakSinceAnalyze),
                             m.peakEnv * std::exp(-dt / std::max(1e-6, cfg_.pcmAgcReleaseSeconds)));
        const double gain = cfg_.pcmAutoGain ? cfg_.pcmGain / std::max(m.peakEnv, cfg_.pcmAgcFloor)
                                             : cfg_.pcmGain;
        for (size_t i = 0; i < nb; ++i)
            m.waveNew[i] = std::clamp(static_cast<float>(m.recent[i * stride] * gain), -2.f, 2.f);
        if (cfg_.pcmBoxcar > 1) {
            // G-Force: mag() is a causal moving average over pcmBoxcar points that starts from zero at the
            // window's oldest end (probe run 16: 1/2/4 kHz read 0.96/0.84/0.50x, null near SR / 7, and a ramp
            // over the first ~7 points of each frame's curve).
            const size_t len = static_cast<size_t>(cfg_.pcmBoxcar);
            float run = 0.f;
            float hist[64] = {};
            const size_t L = std::min<size_t>(len, 64);
            for (size_t i = 0; i < nb; ++i) {
                run += m.waveNew[i] - hist[i % L];
                hist[i % L] = m.waveNew[i];
                m.waveNew[i] = run / static_cast<float>(L);
            }
        }
        const double carry = (cfg_.pcmSmoothSeconds > 0.0 && m.waveInit)
                                 ? std::exp(-dt / cfg_.pcmSmoothSeconds) : 0.0;
        for (size_t i = 0; i < nb; ++i)
            frame_.wave[i] = static_cast<float>((1.0 - carry) * m.waveNew[i] + carry * frame_.wave[i]);
        m.waveInit = true;
    }

    // ---- Spectrum ----------------------------------------------------------
    copyRecent(m.timeBuf.data(), m.n);
    vDSP_vmul(m.timeBuf.data(), 1, m.window.data(), 1, m.timeBuf.data(), 1, m.n);
    DSPSplitComplex split{m.re.data(), m.im.data()};
    vDSP_ctoz(reinterpret_cast<const DSPComplex*>(m.timeBuf.data()), 2, &split, 1, m.n / 2);
    vDSP_fft_zrip(m.setup, &split, 1, m.log2n, kFFTDirection_Forward);
    m.im[0] = 0.f;  // packed Nyquist; DC is ignored below anyway
    m.re[0] = 0.f;
    vDSP_zvmags(&split, 1, m.power.data(), 1, m.n / 2);
    const float pscale = m.ampScale * m.ampScale;
    vDSP_vsmul(m.power.data(), 1, &pscale, m.power.data(), 1, m.n / 2);
    // power[k] is now (amplitude)^2 at bin centers; a Hann-windowed sine spreads
    // 1.5x its peak power over neighboring bins (ENBW).
    constexpr double kEnbw = 1.5;

    const int nbins = cfg_.fftBins;
    {
        const float* w = m.weights.data();
        for (int i = 0; i < nbins; ++i) {
            float acc = 0.f;
            vDSP_dotpr(m.power.data() + m.wFirst[i], 1, w, 1, &acc, vDSP_Length(m.wCount[i]));
            w += m.wCount[i];
            frame_.fftRaw[i] = std::sqrt(std::max(0.f, acc)) * m.tiltGain[i];
        }
    }

    // Group references.
    const int ng = cfg_.envelopes;
    const double ka = onePoleK(dt, cfg_.refAttackSeconds);
    const double kr = onePoleK(dt, cfg_.refReleaseSeconds);
    for (int g = 0; g < ng; ++g) {
        double sum = 0.0;
        for (int i = m.groupStart[g]; i < m.groupEnd[g]; ++i) sum += frame_.fftRaw[i];
        const double x = sum / std::max(1, m.groupEnd[g] - m.groupStart[g]);
        double& r = m.gRef[g];
        r += (x > r ? ka : kr) * (x - r);
        r = std::max(r, cfg_.refMin);
    }

    // Per-bin normalization and smoothing; LEVEL = mean of the result.
    const double keep = cfg_.fftReleaseSeconds > 0.0
                            ? std::exp(-dt / std::max(1e-6, cfg_.fftReleaseSeconds * cfg_.persistCoef)) : 0.0;
    double sumFft = 0.0;
    for (int i = 0; i < nbins; ++i) {
        const int g0 = m.binG0[i], g1 = m.binG1[i];
        const double t = m.binT[i];
        const double r = (1.0 - t) * m.gRef[g0] + t * m.gRef[g1];
        double v = cfg_.fftGain * frame_.fftRaw[i] / r;
        if (v < cfg_.fftLowThresh) v = 0.0;
        double sm = std::max(v, keep * m.fftState[i]);
        if (sm < 1e-6) sm = 0.0;  // let decays reach exactly zero (and avoid denormals)
        m.fftState[i] = static_cast<float>(sm);
        const double out = std::min(sm, cfg_.fftMax) * cfg_.responseScale;
        frame_.fft[i] = static_cast<float>(out);
        sumFft += out;
    }

    // ---- LEVEL / BASS / ENERGY ---------------------------------------------
    const double level = std::clamp(cfg_.levelGain * sumFft / nbins, 0.0, 1.0);
    frame_.level = static_cast<float>(level);
    frame_.bass = static_cast<float>(std::clamp(cfg_.bassGain * level, 0.0, cfg_.bassMax));
    m.levelPeak = std::max(level, m.levelPeak * std::exp(-dt / std::max(1e-6, cfg_.energyPeakSeconds)));
    m.energy = follow(m.energy, cfg_.energyGain * m.levelPeak, dt, cfg_.energyAttackSeconds, cfg_.energyReleaseSeconds);
    m.energy = std::clamp(m.energy, 0.0, cfg_.energyMax);
    frame_.energy = static_cast<float>(m.energy);

    // ---- MID / TREBLE (reforce extensions) ---------------------------------
    auto bandMeter = [&](Impl::Band& b) {
        double sum = 0.0;
        for (int k = b.k0; k <= b.k1; ++k) sum += m.power[k];
        const double a = std::sqrt(sum / kEnbw);
        b.ref = follow(b.ref, a, dt, cfg_.bandRefAttackSeconds, cfg_.bandRefReleaseSeconds);
        const double v = a / std::max(b.ref, cfg_.bandRefMin);
        b.value = follow(b.value, v, dt, cfg_.bandAttackSeconds, cfg_.bandReleaseSeconds);
        if (b.value < 1e-6) b.value = 0.0;
        return static_cast<float>(std::clamp(b.value, 0.0, cfg_.bandMax));
    };
    frame_.mid = bandMeter(m.mid);
    frame_.treble = bandMeter(m.treble);

    // ---- BEAT / TEMPO (reforce extensions) ---------------------------------
    frame_.beat = static_cast<float>(m.beat.beat());
    frame_.beatStrength = static_cast<float>(m.beat.lastOnsetStrength());
    frame_.beatLowShare = static_cast<float>(m.beat.lastOnsetLowShare());
    frame_.kickPresent = m.beat.kickPresent();
    frame_.beatKickness = static_cast<float>(m.beat.lastOnsetKickness());
    frame_.beatBeatness = static_cast<float>(m.beat.lastOnsetBeatness());
    frame_.kickPhase = static_cast<float>(m.beat.kickPhase());
    frame_.kickPhaseLock = static_cast<float>(m.beat.kickPhaseLock());
    frame_.beatFoldPhase = static_cast<float>(m.beat.foldPhase());
    frame_.beatFoldConf = static_cast<float>(m.beat.foldConfidence());
    frame_.beatFilterPhase = static_cast<float>(m.beat.filterPhase());
    frame_.beatFilterConf = static_cast<float>(m.beat.filterConfidence());
    frame_.tempo = static_cast<float>(m.beat.tempo());

    m.newSinceAnalyze = 0;
    m.peakSinceAnalyze = 0.f;
    frame_.timeSeconds += dt;
    frame_.sampleRate = sampleRate_;
    ++frame_.frameIndex;
    return frame_;
}

}  // namespace reforce::audio
