#include "audio/BeatTracker.hpp"

#include <Accelerate/Accelerate.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace reforce::audio {

namespace {
size_t pow2AtLeast(size_t n) {
    size_t p = 1;
    while (p < n) p <<= 1;
    return p;
}
}  // namespace

struct BeatTracker::Impl {
    size_t hop = 441;
    size_t n = 1024;
    vDSP_Length log2n = 10;
    FFTSetup setup = nullptr;
    std::vector<float> window, ring, timeBuf, re, im, mag, logPrev, logPrevBand;
    size_t ringPos = 0;       // write index into ring (size n)
    size_t sinceHop = 0;      // samples since the last processed hop
    int lowK0 = 1, lowK1 = 0, midK0 = 1, midK1 = 0;
    int kickK0 = 1, kickK1 = 0;
    int bandK[BeatTracker::kBands + 1] = {};  // band edges (bins)
    bool havePrev = false;
    // Onset strength history (ring, oldest overwritten), at the hop rate.
    std::vector<double> strength;
    std::vector<double> acfSig;  // the tempo autocorrelation's signal: strength x (1 - a + a x on-beat probability)
    size_t strengthCount = 0;  // total hops processed
    double hopSeconds = 0.01;
    // Scratch for the tempo autocorrelation.
    std::vector<double> lin;
    std::vector<double> acfCache;  // combPeaks: autocorrelation by lag, this update

    ~Impl() {
        if (setup) vDSP_destroy_fftsetup(setup);
    }
    double at(size_t hopIndex) const { return strength[hopIndex % strength.size()]; }
};

BeatTracker::BeatTracker(const BeatConfig& cfg, double sampleRate)
    : cfg_(cfg), sampleRate_(sampleRate > 0 ? sampleRate : 48000.0), impl_(std::make_unique<Impl>()) {
    rebuild();
}

BeatTracker::~BeatTracker() = default;

void BeatTracker::setSampleRate(double sampleRate) {
    if (sampleRate <= 0.0 || sampleRate == sampleRate_) return;
    sampleRate_ = sampleRate;
    rebuild();
}

void BeatTracker::rebuild() {
    Impl& m = *impl_;
    m.n = pow2AtLeast(static_cast<size_t>(std::max(64, cfg_.fftSize)));
    m.log2n = static_cast<vDSP_Length>(std::lround(std::log2(double(m.n))));
    if (m.setup) vDSP_destroy_fftsetup(m.setup);
    m.setup = vDSP_create_fftsetup(m.log2n, kFFTRadix2);
    m.hop = std::max<size_t>(16, static_cast<size_t>(std::lround(cfg_.hopSeconds * sampleRate_)));
    m.hopSeconds = double(m.hop) / sampleRate_;
    m.window.assign(m.n, 0.f);
    vDSP_hann_window(m.window.data(), m.n, vDSP_HANN_DENORM);
    m.ring.assign(m.n, 0.f);
    m.timeBuf.assign(m.n, 0.f);
    m.re.assign(m.n / 2, 0.f);
    m.im.assign(m.n / 2, 0.f);
    m.mag.assign(m.n / 2, 0.f);
    m.logPrev.assign(m.n / 2, 0.f);
    m.logPrevBand.assign(m.n / 2, 0.f);
    const double df = sampleRate_ / double(m.n);
    const int top = int(m.n / 2) - 1;
    auto k0 = [&](double hz) { return std::clamp(int(std::ceil(hz / df)), 1, top); };
    auto k1 = [&](double hz) { return std::clamp(int(std::floor(hz / df)), 1, top); };
    m.lowK0 = k0(cfg_.lowLoHz);
    m.lowK1 = std::max(m.lowK0, k1(cfg_.lowHiHz));
    m.midK0 = std::max(m.lowK1 + 1, k0(cfg_.midLoHz));
    m.midK1 = std::max(m.midK0, k1(cfg_.midHiHz));
    m.kickK0 = std::max(m.lowK0, k0(cfg_.kickLoHz));
    m.kickK1 = std::clamp(k1(cfg_.kickHiHz), m.kickK0, m.lowK1);
    static const double kEdges[kBands + 1] = {30, 60, 120, 250, 500, 1000, 2000, 4000, 8000, 16000};
    for (int b = 0; b <= kBands; ++b) m.bandK[b] = std::clamp(k0(kEdges[b]), 1, top);
    const size_t histHops = static_cast<size_t>(
        std::ceil(std::max(cfg_.tempoWindowSeconds, cfg_.thresholdWindowSeconds) / m.hopSeconds)) + 4;
    m.strength.assign(histHops, 0.0);
    m.acfSig.assign(histHops, 0.0);
    m.lin.assign(histHops, 0.0);
    reset();
}

void BeatTracker::reset() {
    Impl& m = *impl_;
    std::fill(m.ring.begin(), m.ring.end(), 0.f);
    std::fill(m.logPrev.begin(), m.logPrev.end(), 0.f);
    std::fill(m.logPrevBand.begin(), m.logPrevBand.end(), 0.f);
    std::fill(m.strength.begin(), m.strength.end(), 0.0);
    std::fill(m.acfSig.begin(), m.acfSig.end(), 0.0);
    m.ringPos = 0;
    m.sinceHop = 0;
    m.havePrev = false;
    m.strengthCount = 0;
    samples_ = 0;
    onsets_ = 0;
    lastOnset_ = -1.0;
    lastOnsetStrength_ = 0.0;
    lastStrength_ = 0.0;
    tempo_ = confidence_ = 0.0;
    lastConfident_ = -1.0;
    candidate_ = 0.0;
    switchBpm_ = switchEvidence_ = 0.0;
    lastSound_ = 0.0;
    lastOnsetLowShare_ = hopLowShare_ = 0.0;
    kickLevel_ = kickRef_ = 0.0;
    hopKick_ = kickPeak_ = kickPeakT_ = lastOnsetKickness_ = 0.0;
    kpPhi_ = kpRe_ = kpIm_ = kpW_ = 0.0;
    fold_.fill(0.0);
    foldN_.fill(0.0);
    foldBeatPhi_ = -1.0, foldConf_ = 0.0, nextFoldScore_ = 0.0;
    pf_.fill(0.0);
    pfAcc_.fill(0.0);
    pfRef_ = pfTempoOff_ = 0.0;
    pfOpenUntil_ = -1.0;
    kgHead_ = kgCount_ = kgMiss_ = 0;
    kgA_ = kgP_ = kgTempo_ = kpCum_ = kgCumOff_ = 0.0, kgGood_ = kgLeadUntil_ = -1.0;
    foldFresh_ = false;
    pfTempo_ = pfActMean_ = pfTempoAvg_ = 0.0, pfBeatPhi_ = -1.0, pfConf_ = 0.0;
    bandMean_.fill(0.0);
    hopBands_.fill(0.0);
    lastOnsetBands_.fill(0.0);
    lastOnsetBeatness_ = 0.5;
    nextTempoUpdate_ = cfg_.tempoUpdateSeconds;
}

void BeatTracker::push(const float* s, size_t count) {
    Impl& m = *impl_;
    for (size_t i = 0; i < count; ++i) {
        m.ring[m.ringPos] = s[i];
        m.ringPos = (m.ringPos + 1) & (m.n - 1);
        ++samples_;
        if (++m.sinceHop >= m.hop) {
            m.sinceHop = 0;
            processHop();
        }
    }
}

std::array<double, 2> BeatTracker::debugAcf(double bpm) const {
    const Impl& m = *impl_;
    const size_t n = std::min(m.strengthCount, std::min(m.strength.size(), size_t(cfg_.tempoWindowSeconds / m.hopSeconds)));
    if (n < 64 || bpm <= 0.0) return {0.0, 0.0};
    std::vector<double> x(n);
    double mean = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const size_t h = m.strengthCount - n + i;
        x[i] = cfg_.acfBeatness > 0.0 ? m.acfSig[h % m.acfSig.size()] : m.at(h);
        mean += x[i];
    }
    mean /= double(n);
    double r0 = 0.0;
    for (double& v : x) v -= mean, r0 += v * v;
    if (r0 <= 1e-12) return {0.0, 0.0};
    auto acf = [&](size_t lag) {
        if (lag == 0 || lag + 8 >= n) return 0.0;
        double r = 0.0;
        for (size_t i = lag; i < n; ++i) r += x[i] * x[i - lag];
        return r / r0 * double(n) / double(n - lag);
    };
    auto around = [&](double l) {
        const size_t c = size_t(std::lround(l));
        return std::max({acf(c - 1), acf(c), acf(c + 1)});
    };
    const double lag = 60.0 / bpm / m.hopSeconds;
    return {around(lag), around(4.0 * lag)};
}

double BeatTracker::kickPhase() const {
    if (tempo_ <= 0.0 || kpW_ <= 1e-12 || std::hypot(kpRe_, kpIm_) <= 1e-12) return -1.0;
    const double where = std::atan2(kpIm_, kpRe_) / 6.283185307179586;  // the kick's position on phi's circle
    const double p = kpPhi_ - where;
    return p - std::floor(p);
}

std::array<float, BeatConfig::kFoldBins * BeatConfig::kFoldFeat> BeatTracker::foldProfile() const {
    constexpr int B = BeatConfig::kFoldBins, F = BeatConfig::kFoldFeat;
    std::array<float, B * F> out{};
    for (int b = 0; b < B; ++b)
        for (int f = 0; f < F; ++f)
            out[size_t(b * F + f)] = foldN_[size_t(b)] > 1e-9 ? float(fold_[size_t(b * F + f)] / foldN_[size_t(b)]) : 0.f;
    return out;
}

// Normalised profile (as tools/beat_eval/train_fold.py): per feature, the bin means minus their mean over the beat,
// each group (flux, level) divided by its RMS. Scores every rotation r (beat at bin r) with foldW; softmax.
void BeatTracker::scoreFold() {
    constexpr int B = BeatConfig::kFoldBins, F = BeatConfig::kFoldFeat;
    foldBeatPhi_ = -1.0, foldConf_ = 0.0;
    double nMin = 1e9;
    for (double n : foldN_) nMin = std::min(nMin, n);
    if (nMin < 2.0) return;
    bool any = false;
    for (double w : cfg_.foldW) any = any || w != 0.0;
    if (!any) return;
    std::array<double, B * F> p{};
    for (int f = 0; f < F; ++f) {
        double mean = 0.0;
        for (int b = 0; b < B; ++b) mean += fold_[size_t(b * F + f)] / foldN_[size_t(b)];
        mean /= B;
        for (int b = 0; b < B; ++b) p[size_t(b * F + f)] = fold_[size_t(b * F + f)] / foldN_[size_t(b)] - mean;
    }
    for (int g = 0; g < 2; ++g) {
        double ss = 0.0;
        for (int b = 0; b < B; ++b)
            for (int f = g * kBands; f < (g + 1) * kBands; ++f) ss += p[size_t(b * F + f)] * p[size_t(b * F + f)];
        const double inv = 1.0 / (std::sqrt(ss / (B * kBands)) + 1e-6);
        for (int b = 0; b < B; ++b)
            for (int f = g * kBands; f < (g + 1) * kBands; ++f) p[size_t(b * F + f)] *= inv;
    }
    std::array<double, B> sc{};
    int best = 0;
    for (int r = 0; r < B; ++r) {
        double z = 0.0;
        for (int b = 0; b < B; ++b) {
            const double* w = &cfg_.foldW[size_t(b * F)];
            const double* x = &p[size_t(((b + r) % B) * F)];
            for (int f = 0; f < F; ++f) z += w[f] * x[f];
        }
        sc[size_t(r)] = z;
        if (z > sc[size_t(best)]) best = r;
    }
    double tot = 0.0, same = 0.0;
    for (int r = 0; r < B; ++r) {
        const double e = std::exp(sc[size_t(r)] - sc[size_t(best)]);
        tot += e;
        const int d = std::min((r - best + B) % B, (best - r + B) % B);
        same += d < B / 4 ? e : d == B / 4 ? 0.5 * e : 0.0;
    }
    foldConf_ = 2.0 * same / tot - 1.0;
    for (int r = 0; r < B; ++r) foldProb_[size_t(r)] = std::exp(sc[size_t(r)] - sc[size_t(best)]) / tot;
    foldFresh_ = true;
    const double l = sc[size_t((best + B - 1) % B)], c = sc[size_t(best)], rr = sc[size_t((best + 1) % B)];
    const double den = l - 2.0 * c + rr, off = std::abs(den) > 1e-12 ? std::clamp(0.5 * (l - rr) / den, -0.5, 0.5) : 0.0;
    foldBeatPhi_ = (best + off + 0.5) / B;  // where the beat sits on phi's circle
}

void BeatTracker::stepFilter(double act, double phiAct, double hopSeconds) {
    constexpr int M = BeatConfig::kPhaseStates, B = BeatConfig::kFoldBins;
    const int R = std::clamp(cfg_.pfTempoRows, 1, BeatConfig::kMaxTempoRows), C = R / 2;
    const double step = cfg_.pfTempoStep;
    auto row = [&](std::array<double, M * BeatConfig::kMaxTempoRows>& a, int k) { return a.data() + size_t(k) * M; };
    if (pfTempo_ <= 0.0 || std::abs(tempo_ / pfTempo_ - 1.0) > 0.02) {  // a new tempo: start over
        double wsum = 0.0;
        for (int k = 0; k < R; ++k) {
            const double z = (k - C) / std::max(1e-3, cfg_.pfTempoPrior);
            wsum += std::exp(-0.5 * z * z);
        }
        for (int k = 0; k < R; ++k) {
            const double z = (k - C) / std::max(1e-3, cfg_.pfTempoPrior);
            std::fill(row(pf_, k), row(pf_, k) + M, std::exp(-0.5 * z * z) / wsum / M);
        }
        pfAcc_.fill(0.0);
        pfRef_ = tempo_;
        pfTempoOff_ = 0.0;
        pfTempo_ = tempo_;
        foldFresh_ = false;
        pfBeatPhi_ = -1.0;
    }
    pfTempo_ = tempo_;
    if (R == 1) pfRef_ = tempo_;
    // Re-centre the rows on TEMPO (row k's tempo stays where it is; the edge row is repeated).
    while (tempo_ > pfRef_ * (1.0 + 0.5 * step)) {
        std::copy(row(pf_, 1), row(pf_, R), row(pf_, 0));
        std::copy(pfAcc_.begin() + 1, pfAcc_.begin() + R, pfAcc_.begin());
        pfRef_ *= 1.0 + step;
    }
    while (tempo_ < pfRef_ * (1.0 - 0.5 * step)) {
        std::copy_backward(row(pf_, 0), row(pf_, R - 1), row(pf_, R));
        std::copy_backward(pfAcc_.begin(), pfAcc_.begin() + R - 1, pfAcc_.begin() + R);
        pfRef_ /= 1.0 + step;
    }
    pfTempoAvg_ = pfTempoAvg_ > 0.0 ? pfTempoAvg_ + (tempo_ - pfTempoAvg_) * std::min(1.0, hopSeconds / std::max(1e-3, cfg_.pfMovingSeconds))
                                    : tempo_;
    const bool moving = std::abs(tempo_ / pfTempoAvg_ - 1.0) > cfg_.pfMovingBand;
    const double diff = moving ? cfg_.pfDiffusionMoving : cfg_.pfDiffusion;
    // Each row's beat drifts along phi's circle at (its tempo - TEMPO): whole states at a time (the rest carried).
    if (R > 1)
        for (int k = 0; k < R; ++k) {
            double& acc = pfAcc_[size_t(k)];
            acc -= (pfRef_ * (1.0 + (k - C) * step) - tempo_) / 60.0 * hopSeconds * M;
            double* r = row(pf_, k);
            while (acc >= 0.5) std::rotate(r, r + M - 1, r + M), acc -= 1.0;  // the beat moves up phi's circle
            while (acc < -0.5) std::rotate(r, r + 1, r + M), acc += 1.0;
        }
    // Random walk in phase: a 3-tap blur with variance diff^2 * hop (in states^2); in tempo while a kick plays.
    const double q = std::min(0.25, 0.5 * diff * diff * M * M * hopSeconds);
    const bool kick = kickPresent();
    const double td = cfg_.pfTempoDiffusionSteady >= 0.0 && streamSeconds() > pfOpenUntil_ ? cfg_.pfTempoDiffusionSteady
                                                                                         : cfg_.pfTempoDiffusion;
    const double qt = R > 1 && kick ? std::min(0.25, 0.5 * std::pow(td / step, 2) * hopSeconds) : 0.0;
    for (int k = 0; k < R; ++k) {
        const double* a = row(pf_, k);
        const double* lo = row(pf_, std::max(0, k - 1));
        const double* hi = row(pf_, std::min(R - 1, k + 1));
        double* o = row(pfNext_, k);
        for (int j = 0; j < M; ++j) {
            const double v = (1 - 2 * qt) * a[j] + qt * (lo[j] + hi[j]);
            o[j] = v;
        }
    }
    for (int k = 0; k < R; ++k) {
        const double* a = row(pfNext_, k);
        double* o = row(pf_, k);
        for (int j = 0; j < M; ++j) o[j] = (1 - 2 * q) * a[j] + q * (a[(j + M - 1) % M] + a[(j + 1) % M]);
    }
    std::array<double, M> mult;
    mult.fill(1.0);
    bool any = false;
    pfActMean_ += (act - pfActMean_) * std::min(1.0, hopSeconds / std::max(1e-3, cfg_.pfActSeconds));
    if (kick && pfActMean_ > 1e-9) {
        const double a = std::min(10.0, act / pfActMean_) - 1.0;
        const double inv = 1.0 / (2.0 * cfg_.pfWidth * cfg_.pfWidth);
        for (int j = 0; j < M; ++j) {
            double d = phiAct - (j + 0.5) / M;
            d -= std::round(d);
            mult[size_t(j)] *= std::max(cfg_.pfFloor, 1.0 + cfg_.pfGain * a * std::exp(-d * d * inv));
        }
        any = true;
    }
    const double foldGain = moving ? cfg_.pfFoldGainMoving : cfg_.pfFoldGain;
    if (foldFresh_ && foldGain > 0.0) {  // the fold's softmax (bin r's centre at (r + .5) / B), interpolated
        foldFresh_ = false;
        for (int j = 0; j < M; ++j) {
            const double pos = (j + 0.5) * B / M - 0.5, fl = std::floor(pos), fr = pos - fl;
            const int r0 = (int(fl) % B + B) % B, r1 = (r0 + 1) % B;
            const double pr = (1 - fr) * foldProb_[size_t(r0)] + fr * foldProb_[size_t(r1)];
            mult[size_t(j)] *= std::pow(std::max(pr, 1e-6), foldGain);
        }
        any = true;
    }
    std::array<double, M> marg{};
    double tot = 0.0;
    for (int k = 0; k < R; ++k) {
        double* r = row(pf_, k);
        for (int j = 0; j < M; ++j) {
            if (any) r[j] *= mult[size_t(j)];
            marg[size_t(j)] += r[j];
        }
    }
    int best = 0;
    for (int j = 0; j < M; ++j) {
        tot += marg[size_t(j)];
        if (marg[size_t(j)] > marg[size_t(best)]) best = j;
    }
    if (!(tot > 1e-300)) {
        std::fill(pf_.begin(), pf_.begin() + R * M, 1.0 / (R * M));
        return;
    }
    for (int i = 0; i < R * M; ++i) pf_[size_t(i)] /= tot;
    for (double& v : marg) v /= tot;
    const double l = marg[size_t((best + M - 1) % M)], c = marg[size_t(best)], r = marg[size_t((best + 1) % M)];
    const double den = l - 2.0 * c + r, off = std::abs(den) > 1e-15 ? std::clamp(0.5 * (l - r) / den, -0.5, 0.5) : 0.0;
    pfBeatPhi_ = (best + off + 0.5) / M;
    double near = 0.0, tw = 0.0, to = 0.0;
    for (int j = 0; j < M; ++j) {
        double d = (j + 0.5) / M - pfBeatPhi_;
        d -= std::round(d);
        if (std::abs(d) > 0.1) continue;
        near += marg[size_t(j)];
        for (int k = 0; k < R; ++k) {
            const double v = pf_[size_t(k * M + j)];
            tw += v, to += v * (k - C) * step;
        }
    }
    pfConf_ = near;
    pfTempoOff_ = tw > 1e-300 ? to / tw : 0.0;
}

void BeatTracker::addKickGrid(double t, double cum, double beatness) {
    constexpr int K = kKgMax;
    auto at = [&](int i) { return (kgHead_ + i) % K; };  // i-th oldest
    while (kgCount_ > 0 && t - kgT_[size_t(at(0))] > cfg_.kgSeconds) kgHead_ = at(1), --kgCount_;
    const int last = kgCount_ > 0 ? at(kgCount_ - 1) : -1;
    double n = 0.0;
    bool ok = false;
    if (kgCount_ >= 3 && kgP_ > 0.0) {  // number it from the fit
        n = std::round((t - kgA_) / kgP_);
        ok = std::abs(t - (kgA_ + kgP_ * n)) < cfg_.kgGate * kgP_;
    } else if (last >= 0) {  // from the previous kick, along phi
        const double lastCum = kgN_[size_t(last)] + kgCumOff_;
        const double d = cum - lastCum;
        n = kgN_[size_t(last)] + std::round(d);
        ok = std::abs(d - std::round(d)) < cfg_.kgGate;
    } else {  // the first one: on the phase filter's beat, or any clearly on-beat onset (only the period matters)
        double d = (cum - std::floor(cum)) - pfBeatPhi_;
        d -= std::round(d);
        ok = (pfBeatPhi_ >= 0.0 && pfConf_ >= 0.5 && std::abs(d) < cfg_.kgGate) || beatness >= cfg_.kgRestartBeatness;
        n = 0.0;
        kgCumOff_ = cum;  // beat number 0 sits at this phi count
    }
    if (ok && last >= 0 && n <= kgN_[size_t(last)]) return;  // a second hit on the same beat
    if (!ok) {
        if (last >= 0 && t - kgT_[size_t(last)] > cfg_.kgLostSeconds)
            kgCount_ = 0, kgTempo_ = 0.0, kgP_ = 0.0;  // lost the grid: start over
        return;
    }
    kgMiss_ = 0;
    if (kgCount_ == K) kgHead_ = at(1), --kgCount_;
    const int w = at(kgCount_);
    kgT_[size_t(w)] = t, kgN_[size_t(w)] = n;
    ++kgCount_;
    if (kgCount_ < 3) return;
    double sn = 0, st = 0, snn = 0, snt = 0;
    for (int i = 0; i < kgCount_; ++i) {
        const double x = kgN_[size_t(at(i))], y = kgT_[size_t(at(i))];
        sn += x, st += y, snn += x * x, snt += x * y;
    }
    const double N = kgCount_, den = N * snn - sn * sn;
    if (den <= 1e-9) return;
    const double P = (N * snt - sn * st) / den, a = (st - P * sn) / N;
    if (!(P > 0.0)) return;
    double rss = 0;
    for (int i = 0; i < kgCount_; ++i) {
        const double r = kgT_[size_t(at(i))] - (a + P * kgN_[size_t(at(i))]);
        rss += r * r;
    }
    const double span = kgN_[size_t(at(kgCount_ - 1))] - kgN_[size_t(at(0))];
    kgA_ = a, kgP_ = P;
    const double bpm = 60.0 / P;
    if (kgCount_ >= cfg_.kgMinPoints && span >= cfg_.kgMinBeats && std::sqrt(rss / N) < cfg_.kgMaxResidual * P &&
        std::abs(bpm / tempo_ - 1.0) < cfg_.steadyWindow) {
        kgTempo_ = bpm;
        kgGood_ = t;
    }
}

double BeatTracker::beat() const {
    if (lastOnset_ < 0.0 || cfg_.pulseDecaySeconds <= 0.0) return 0.0;
    const double age = std::max(0.0, streamSeconds() - lastOnset_);
    const double b = std::exp(-age / cfg_.pulseDecaySeconds);
    return b < 1e-3 ? 0.0 : b;
}

void BeatTracker::processHop() {
    Impl& m = *impl_;
    // Oldest-first copy of the ring (ringPos is the oldest sample), windowed.
    const size_t tail = m.n - m.ringPos;
    std::copy(m.ring.begin() + long(m.ringPos), m.ring.end(), m.timeBuf.begin());
    std::copy(m.ring.begin(), m.ring.begin() + long(m.ringPos), m.timeBuf.begin() + long(tail));
    vDSP_vmul(m.timeBuf.data(), 1, m.window.data(), 1, m.timeBuf.data(), 1, m.n);
    DSPSplitComplex split{m.re.data(), m.im.data()};
    vDSP_ctoz(reinterpret_cast<const DSPComplex*>(m.timeBuf.data()), 2, &split, 1, m.n / 2);
    vDSP_fft_zrip(m.setup, &split, 1, m.log2n, kFFTDirection_Forward);
    m.re[0] = m.im[0] = 0.f;
    vDSP_zvabs(&split, 1, m.mag.data(), 1, m.n / 2);
    // vDSP_fft_zrip returns 2x the DFT, Hann coherent gain 0.5: sine amplitude A -> |X| = A * n / 2.
    const float scale = 2.f / float(m.n);
    double fluxLow = 0.0, fluxMid = 0.0, fluxKick = 0.0;
    float peak = 0.f;
    vDSP_maxv(m.mag.data() + m.lowK0, 1, &peak, vDSP_Length(m.midK1 - m.lowK0 + 1));
    for (int k = m.lowK0; k <= m.midK1; ++k) {
        const float l = std::log1p(float(cfg_.compression) * m.mag[size_t(k)] * scale);
        const float d = m.havePrev ? l - m.logPrev[size_t(k)] : 0.f;
        m.logPrev[size_t(k)] = l;
        if (d <= 0.f) continue;
        if (k >= m.kickK0 && k <= m.kickK1) fluxKick += d;
        if (k <= m.lowK1) fluxLow += d;
        else fluxMid += d;
    }
    // Band features: flux per band over the whole 30 Hz - 16 kHz range (bins above midK1 get their own logPrev).
    std::array<double, kBands> bandFlux{}, bandLevel{};
    for (int b = 0; b < kBands; ++b) {
        double f = 0.0, lv = 0.0;
        for (int k = m.bandK[b]; k < m.bandK[b + 1]; ++k) {
            const float l = std::log1p(float(cfg_.compression) * m.mag[size_t(k)] * scale);
            const float d = l - m.logPrevBand[size_t(k)];
            m.logPrevBand[size_t(k)] = l;
            lv += l;
            if (m.havePrev && d > 0.f) f += d;
        }
        bandFlux[size_t(b)] = f / double(std::max(1, m.bandK[b + 1] - m.bandK[b]));
        bandLevel[size_t(b)] = lv / double(std::max(1, m.bandK[b + 1] - m.bandK[b]));
    }
    m.havePrev = true;
    // Kick-band share of the hop's flux, from the band totals (not the per-bin means below: the low band has few
    // bins, so any sharp attack's splatter would read as a kick).
    const double shareNow = fluxLow + fluxMid > 0.0 ? fluxLow / (fluxLow + fluxMid) : 0.0;
    fluxLow /= double(m.lowK1 - m.lowK0 + 1);
    fluxMid /= double(m.midK1 - m.midK0 + 1);
    const double o = cfg_.lowWeight * fluxLow + cfg_.midWeight * fluxMid;
    fluxKick /= double(m.kickK1 - m.kickK0 + 1);
    const double candKick = hopKick_;  // the previous hop: the peak-picking candidate below
    hopKick_ = fluxKick;
    const std::array<double, kBands> candBands = hopBands_;
    for (int b = 0; b < kBands; ++b)  // this hop's (pre-update) features belong to hop strengthCount - 1
        if (m.strengthCount >= 1) bandHist_[(m.strengthCount - 1) % bandHist_.size()][size_t(b)] = float(hopBands_[size_t(b)]);
    const double candStrength = m.strengthCount >= 1 ? m.at(m.strengthCount - 1) : 0.0;
    {
        const double a = std::min(1.0, m.hopSeconds / std::max(1e-3, cfg_.bandMeanSeconds));
        for (int b = 0; b < kBands; ++b) {
            const size_t i = size_t(b);
            hopBands_[i] = std::log1p(bandFlux[i] / (bandMean_[i] + 1e-6));
            bandMean_[i] += a * (bandFlux[i] - bandMean_[i]);
        }
    }
    const double now0 = streamSeconds();
    if (tempo_ > 0.0) {
        kpPhi_ += m.hopSeconds * tempo_ / 60.0;
        kpPhi_ -= std::floor(kpPhi_);
        kpCum_ += m.hopSeconds * tempo_ / 60.0;
        const double keep = std::exp(-m.hopSeconds / std::max(1e-3, cfg_.kickPhaseSeconds));
        const double w = std::pow(std::max(0.0, fluxKick), cfg_.kickPhasePower);
        constexpr double kTwoPi = 6.283185307179586;
        kpRe_ = kpRe_ * keep + w * std::cos(kTwoPi * kpPhi_);
        kpIm_ = kpIm_ * keep + w * std::sin(kTwoPi * kpPhi_);
        kpW_ = kpW_ * keep + w;
        // Beat fold: this hop's band flux and level into the two bins around phi (bin b's centre: (b + .5) / B).
        constexpr int B = BeatConfig::kFoldBins, F = BeatConfig::kFoldFeat;
        const double fk = std::exp(-m.hopSeconds / std::max(1e-3, cfg_.foldSeconds));
        for (double& v : fold_) v *= fk;
        for (double& v : foldN_) v *= fk;
        const double pos = kpPhi_ * B - 0.5, fl = std::floor(pos), fr = pos - fl;
        const int b0 = (int(fl) % B + B) % B, b1 = (b0 + 1) % B;
        for (int f = 0; f < kBands; ++f) {
            const double vf = hopBands_[size_t(f)], vl = bandLevel[size_t(f)];
            fold_[size_t(b0 * F + f)] += (1 - fr) * vf, fold_[size_t(b1 * F + f)] += fr * vf;
            fold_[size_t(b0 * F + kBands + f)] += (1 - fr) * vl, fold_[size_t(b1 * F + kBands + f)] += fr * vl;
        }
        foldN_[size_t(b0)] += 1 - fr, foldN_[size_t(b1)] += fr;
        if (now0 >= nextFoldScore_) {
            nextFoldScore_ = now0 + 0.1;
            scoreFold();
        }
    } else {
        foldBeatPhi_ = -1.0, foldConf_ = 0.0;
    }
    kickLevel_ += (fluxKick - kickLevel_) * std::min(1.0, m.hopSeconds / std::max(1e-3, cfg_.kickLevelSeconds));
    kickRef_ = std::max(kickRef_ * std::exp(-m.hopSeconds / std::max(1e-3, cfg_.kickRefSeconds)), kickLevel_);
    if (cfg_.acfBeatness > 0.0) {  // the previous hop's on-beat probability (needs this hop as its "next")
        double z = cfg_.beatnessBias;
        for (int b = 0; b < kBands; ++b)
            z += cfg_.beatnessW[size_t(b)] * candBands[size_t(b)] + cfg_.beatnessWNext[size_t(b)] * hopBands_[size_t(b)];
        const double pb = 1.0 / (1.0 + std::exp(-z));
        if (m.strengthCount >= 1)
            m.acfSig[(m.strengthCount - 1) % m.acfSig.size()] =
                candStrength * (cfg_.acfBeatness <= 1.0 ? 1.0 - cfg_.acfBeatness + cfg_.acfBeatness * pb
                                                        : std::pow(pb, cfg_.acfBeatness));
        m.acfSig[m.strengthCount % m.acfSig.size()] = o;  // provisional until the next hop
        if (tempo_ > 0.0 && m.strengthCount >= 1)  // the previous hop's activation, at its phi
            stepFilter(candStrength * pb, kpPhi_ - m.hopSeconds * tempo_ / 60.0, m.hopSeconds);
    }
    if (tempo_ <= 0.0) pfBeatPhi_ = -1.0, pfConf_ = 0.0, pfTempo_ = 0.0;
    m.strength[m.strengthCount % m.strength.size()] = o;
    ++m.strengthCount;
    lastStrength_ = o;
    const double now = streamSeconds();
    if (double(peak * scale) >= cfg_.silenceLevel) lastSound_ = now;
    const double candShare = hopLowShare_;  // the previous hop: the peak-picking candidate below
    hopLowShare_ = shareNow;

    // Peak picking on the previous hop (needs one hop of look-ahead).
    if (m.strengthCount >= 3) {
        const size_t c = m.strengthCount - 2;  // candidate hop
        const double prev = m.at(c - 1), cur = m.at(c), next = m.at(c + 1);
        if (cur >= prev && cur > next && cur > cfg_.minFlux) {
            const size_t win = std::min<size_t>(c, static_cast<size_t>(cfg_.thresholdWindowSeconds / m.hopSeconds));
            double sum = 0.0, sum2 = 0.0;
            for (size_t i = 0; i < win; ++i) {
                const double v = m.at(c - 1 - i);
                sum += v;
                sum2 += v * v;
            }
            const double mean = win ? sum / double(win) : 0.0;
            const double sd = win ? std::sqrt(std::max(0.0, sum2 / double(win) - mean * mean)) : 0.0;
            const double tOnset = now - m.hopSeconds;  // stream time of the candidate hop
            if (cur > mean + cfg_.thresholdK * sd &&
                (lastOnset_ < 0.0 || tOnset - lastOnset_ >= cfg_.minIntervalSeconds)) {
                lastOnset_ = tOnset;
                lastOnsetStrength_ = sd > 0 ? (cur - mean) / sd : cfg_.thresholdK;
                lastOnsetLowShare_ = candShare;
                kickPeak_ = std::max(kickPeak_ * std::exp(-(tOnset - kickPeakT_) / std::max(1e-3, cfg_.kickPeakSeconds)),
                                     candKick);
                kickPeakT_ = tOnset;
                lastOnsetKickness_ = kickPeak_ > 1e-9 ? candKick / kickPeak_ : 0.0;
                lastOnsetBands_ = candBands;
                lastOnsetBandsNext_ = hopBands_;
                lastOnsetBandsHalf_.fill(0.0);
                if (tempo_ > 0.0) {  // half a beat before the candidate hop (c = strengthCount - 2 after the push)
                    const size_t c = m.strengthCount - 2, back = size_t(std::lround(30.0 / tempo_ / m.hopSeconds));
                    if (back + 2 < c && back + 2 < bandHist_.size())
                        for (size_t k = c - back - 1; k <= c - back + 1; ++k)
                            for (int b = 0; b < kBands; ++b)
                                lastOnsetBandsHalf_[size_t(b)] =
                                    std::max(lastOnsetBandsHalf_[size_t(b)], double(bandHist_[k % bandHist_.size()][size_t(b)]));
                }
                double z = cfg_.beatnessBias;
                if (cfg_.useHalfModel && tempo_ > 0.0) {
                    z = cfg_.halfBias;
                    for (int b = 0; b < kBands; ++b) {
                        const size_t i = size_t(b);
                        z += cfg_.halfW[i] * candBands[i] + cfg_.halfWNext[i] * hopBands_[i] +
                             cfg_.halfWHalf[i] * lastOnsetBandsHalf_[i];
                    }
                } else {
                    for (int b = 0; b < kBands; ++b)
                        z += cfg_.beatnessW[size_t(b)] * candBands[size_t(b)] + cfg_.beatnessWNext[size_t(b)] * hopBands_[size_t(b)];
                }
                lastOnsetBeatness_ = 1.0 / (1.0 + std::exp(-z));

                if (cfg_.kgSeconds > 0.0 && tempo_ > 0.0 && kickPresent() && lastOnsetBeatness_ >= cfg_.kgMinBeatness)
                    addKickGrid(tOnset, kpCum_ - m.hopSeconds * tempo_ / 60.0, lastOnsetBeatness_);
                onsetTimes_[onsets_ % onsetTimes_.size()] = tOnset;
                ++onsets_;
            }
        }
    }

    if (now >= nextTempoUpdate_) {
        nextTempoUpdate_ = now + cfg_.tempoUpdateSeconds;
        updateTempo();
    }
}

void BeatTracker::updateTempo() {
    Impl& m = *impl_;
    const double now = streamSeconds();
    const size_t n = std::min(m.strengthCount,
                              std::min(m.strength.size(), size_t(cfg_.tempoWindowSeconds / m.hopSeconds)));
    const size_t minLag = std::max<size_t>(1, size_t(std::floor(60.0 / cfg_.maxBpm / m.hopSeconds)));
    const size_t maxLag = size_t(std::ceil(60.0 / cfg_.minBpm / m.hopSeconds));
    const bool comb = cfg_.comb2 > 0.0 || cfg_.comb4 > 0.0;
    // Octave folding into [foldMinBpm, 2 * foldMinBpm] (no hysteresis).
    auto fold = [&](double bpm) {
        if (cfg_.foldMinBpm <= 0.0 || bpm <= 0.0) return bpm;
        while (bpm < cfg_.foldMinBpm) bpm *= 2.0;
        while (bpm > 2.0 * cfg_.foldMinBpm) bpm /= 2.0;
        return bpm;
    };
    // Same beat as the current tempo, in any octave, within `tol`.
    auto sameBeat = [&](double bpm, double ref, double tol) {
        if (bpm <= 0.0 || ref <= 0.0) return false;
        for (const double k : {1.0, 2.0, 0.5})
            if (std::abs(bpm * k / ref - 1.0) < tol) return true;
        return false;
    };
    double bestScore = 0.0, bestLag = 0.0, bestConf = 0.0;
    double localConf = 0.0, localBpm = 0.0;  // steady: the current tempo's own peak
    int recent = 0;
    for (uint64_t i = 0; i < std::min<uint64_t>(onsets_, onsetTimes_.size()); ++i)
        if (now - onsetTimes_[i] <= cfg_.tempoWindowSeconds) ++recent;
    if (n >= 2 * maxLag && recent >= cfg_.minOnsets) {
        double mean = 0.0;
        for (size_t i = 0; i < n; ++i) {
            m.lin[i] = cfg_.acfBeatness > 0.0 ? m.acfSig[(m.strengthCount - n + i) % m.acfSig.size()]
                                              : m.at(m.strengthCount - n + i);
            mean += m.lin[i];
        }
        mean /= double(n);
        double r0 = 0.0;
        for (size_t i = 0; i < n; ++i) {
            m.lin[i] -= mean;
            r0 += m.lin[i] * m.lin[i];
        }
        if (r0 > 1e-12) {
            auto acf = [&](size_t lag) {
                double r = 0.0;
                for (size_t i = lag; i < n; ++i) r += m.lin[i] * m.lin[i - lag];
                return r / r0 * double(n) / double(n - lag);  // unbiased, normalized
            };
            if (cfg_.combPeaks) {
                // Candidates are the peaks of the combined score (1 beat + the bar), not of the 1-beat
                // autocorrelation alone: a real beat's 1-beat repeat can be weak (no local peak) while its bar
                // repeat is strong; a 3:4 impostor is the reverse (tools/beat_eval traces, PLAN 5J.12).
                const size_t top = std::min(n - 9, size_t(std::ceil(8.0 * double(maxLag + 2))));
                std::vector<double>& cache = m.acfCache;
                cache.assign(top + 2, std::numeric_limits<double>::quiet_NaN());
                auto ac = [&](size_t k) {
                    if (k == 0 || k > top) return 0.0;
                    if (std::isnan(cache[k])) cache[k] = acf(k);
                    return cache[k];
                };
                // Best of +-w hops around k x the lag: the integer lag is up to half a hop off the beat, so k x it is
                // up to k / 2 hops off (at 129 BPM the beat is 46.5 hops: 4 x 46 = 184 missed the bar peak at 186
                // with +-1, and 2/3 = 86 BPM won).
                auto around = [&](double l, int w) {
                    const size_t c = size_t(std::lround(l));
                    double b = ac(c);
                    for (int d = 1; d <= w; ++d) b = std::max({b, ac(c - size_t(d)), ac(c + size_t(d))});
                    return b;
                };
                const double norm = 1.0 + cfg_.comb2 + cfg_.comb4 + cfg_.comb8 + cfg_.combHalf;
                auto comb = [&](size_t lag) {
                    double s = ac(lag) + cfg_.comb2 * around(2.0 * double(lag), 1) + cfg_.comb4 * around(4.0 * double(lag), 2) +
                               cfg_.comb8 * around(8.0 * double(lag), 4);
                    if (cfg_.combHalf > 0.0) s += cfg_.combHalf * around(0.5 * double(lag), 1);
                    return s / norm;
                };
                double sPrev = comb(minLag - 1), sCur = comb(minLag);
                for (size_t lag = minLag; lag <= maxLag; ++lag) {
                    const double sNext = comb(lag + 1);
                    if (sCur > 0.0 && sCur >= sPrev && sCur >= sNext) {
                        const double den = sPrev - 2.0 * sCur + sNext;
                        const double off = std::abs(den) > 1e-12 ? std::clamp(0.5 * (sPrev - sNext) / den, -0.5, 0.5) : 0.0;
                        const double lagF = double(lag) + off;
                        double bpm = 60.0 / (lagF * m.hopSeconds);
                        double lagR = lagF;
                        if (cfg_.refine4) {  // the bar peak / 4
                            const double c4 = 4.0 * lagF;
                            const size_t lo = size_t(std::max(2.0, std::floor(c4 - 3.0))), hi = size_t(std::ceil(c4 + 3.0));
                            if (hi + 1 <= top) {
                                size_t bk = lo;
                                double bv = -1e9;
                                for (size_t k = lo; k <= hi; ++k)
                                    if (ac(k) > bv) bv = ac(k), bk = k;
                                if (bk > lo && bk < hi) {
                                    const double a0 = ac(bk - 1), a2 = ac(bk + 1), d2 = a0 - 2.0 * bv + a2;
                                    const double o = std::abs(d2) > 1e-12 ? std::clamp(0.5 * (a0 - a2) / d2, -0.5, 0.5) : 0.0;
                                    lagR = (double(bk) + o) / 4.0;
                                    if (std::abs(lagR / lagF - 1.0) < 0.03) bpm = 60.0 / (lagR * m.hopSeconds);
                                    else lagR = lagF;
                                }
                            }
                        }
                        const double oct = std::log2(bpm / cfg_.priorBpm) / std::max(1e-3, cfg_.priorOctaves);
                        const double score = sCur * std::exp(-0.5 * oct * oct);
                        if (score > bestScore) {
                            bestScore = score;
                            bestLag = lagR;
                            bestConf = sCur;
                        }
                        if (cfg_.steady && tempo_ > 0.0 && sCur > localConf && sameBeat(bpm, tempo_, cfg_.steadyWindow)) {
                            localConf = sCur;
                            localBpm = bpm;
                        }
                    }
                    sPrev = sCur;
                    sCur = sNext;
                }
            } else {
                double rPrev = acf(minLag - 1), rCur = acf(minLag);
                for (size_t lag = minLag; lag <= maxLag; ++lag) {
                    const double rNext = acf(lag + 1);
                    if (rCur > 0.0 && rCur >= rPrev && rCur >= rNext) {
                        // Parabolic refinement of the peak position.
                        const double den = rPrev - 2.0 * rCur + rNext;
                        const double off = std::abs(den) > 1e-12 ? std::clamp(0.5 * (rPrev - rNext) / den, -0.5, 0.5) : 0.0;
                        const double lagF = double(lag) + off;
                        double bpm = 60.0 / (lagF * m.hopSeconds);
                        double lagR = lagF;  // refined: the peak 4 beats later, / 4 (4x the resolution)
                        if (cfg_.refine4) {
                            const double c4 = 4.0 * lagF;
                            const size_t lo = size_t(std::max(2.0, std::floor(c4 - 3.0))), hi = size_t(std::ceil(c4 + 3.0));
                            if (hi + 2 < n) {
                                size_t bk = lo;
                                double bv = -1e9;
                                for (size_t k = lo; k <= hi; ++k) {
                                    const double v = acf(k);
                                    if (v > bv) bv = v, bk = k;
                                }
                                if (bk > lo && bk < hi) {
                                    const double a0 = acf(bk - 1), a2 = acf(bk + 1), den = a0 - 2.0 * bv + a2;
                                    const double o = std::abs(den) > 1e-12 ? std::clamp(0.5 * (a0 - a2) / den, -0.5, 0.5) : 0.0;
                                    lagR = (double(bk) + o) / 4.0;
                                    if (std::abs(lagR / lagF - 1.0) < 0.03) bpm = 60.0 / (lagR * m.hopSeconds);
                                    else lagR = lagF;
                                }
                            }
                        }
                        const double oct = std::log2(bpm / cfg_.priorBpm) / std::max(1e-3, cfg_.priorOctaves);
                        double r = rCur;
                        if (comb) {  // best of +-1 hop around 2x and 4x the lag (within the window)
                            auto around = [&](double l) {
                                const size_t c = size_t(std::lround(l));
                                double b = 0.0;
                                for (size_t k = c - 1; k <= c + 1; ++k)
                                    if (k > 0 && k + 8 < n) b = std::max(b, acf(k));
                                return b;
                            };
                            r += cfg_.comb2 * around(2.0 * lagF) + cfg_.comb4 * around(4.0 * lagF) +
                                 cfg_.comb8 * around(8.0 * lagF);
                        }
                        const double score = r * std::exp(-0.5 * oct * oct);
                        if (score > bestScore) {
                            bestScore = score;
                            bestLag = lagR;
                            bestConf = rCur;
                        }
                        if (cfg_.steady && tempo_ > 0.0 && rCur > localConf && sameBeat(bpm, tempo_, cfg_.steadyWindow)) {
                            localConf = rCur;
                            localBpm = bpm;
                        }
                    }
                    rPrev = rCur;
                    rCur = rNext;
                }
            }
        }
    }
    confidence_ = bestConf;
    double bpm = bestLag > 0.0 ? fold(60.0 / (bestLag * m.hopSeconds)) : 0.0;
    // Hysteresis at the folding edges: stay in the current tempo's octave when it is the same beat.
    if (cfg_.foldMinBpm > 0.0 && tempo_ > 0.0 && bpm > 0.0) {
        const double lo = cfg_.foldMinBpm, hi = 2.0 * cfg_.foldMinBpm;
        for (const double alt : {bpm * 2.0, bpm * 0.5})
            if (std::abs(alt / tempo_ - 1.0) < 0.08 && alt >= lo * (1.0 - cfg_.foldSlack) &&
                alt <= hi * (1.0 + cfg_.foldSlack))
                bpm = alt;
    }
    const double smoothK = 1.0 - std::exp(-cfg_.tempoUpdateSeconds / std::max(1e-3, cfg_.tempoSmoothSeconds));

    if (cfg_.steady && tempo_ > 0.0 && now - lastSound_ > cfg_.silenceSeconds) {  // the music stopped
        tempo_ = 0.0;
        switchBpm_ = switchEvidence_ = 0.0;
        candidate_ = 0.0;
        return;
    }
    if (cfg_.steady && tempo_ > 0.0 && now - acquiredAt_ >= cfg_.firstLockSeconds) {
        if (!kickPresent()) {  // a breakdown: keep the tempo exactly as it is
            switchBpm_ = switchEvidence_ = 0.0;
            lastConfident_ = now;
            return;
        }
        double target = localBpm;  // the local peak, in the current tempo's octave
        for (const double k : {2.0, 0.5})
            if (target > 0.0 && std::abs(target * k / tempo_ - 1.0) < std::abs(target / tempo_ - 1.0)) target *= k;
        if (target > 0.0 && localConf >= cfg_.pfOpenMinConfidence && std::abs(target / tempo_ - 1.0) > cfg_.pfOpenBand)
            pfOpenUntil_ = now + cfg_.pfOpenHoldSeconds;
        if (cfg_.kgSeconds > 0.0 && kgTempo_ > 0.0 && now - kgGood_ < 1.0 &&
            std::abs(kgTempo_ / tempo_ - 1.0) > cfg_.kgBand) {  // the kick grid leads
            tempo_ += (1.0 - std::exp(-cfg_.tempoUpdateSeconds / std::max(1e-3, cfg_.kgSmoothSeconds))) * (kgTempo_ - tempo_);
            kgLeadUntil_ = now + cfg_.tempoWindowSeconds;
            fastRun_ = 0.0;
            lastConfident_ = now;
        }
        if (localConf >= cfg_.steadyDriftMinConfidence && now >= kgLeadUntil_) {
            // Follow drift.
            const double dev = target / tempo_ - 1.0;
            if (std::abs(dev) <= cfg_.steadyFastBand) fastRun_ = 0.0;
            else if ((dev > 0) == (fastRun_ >= 0)) fastRun_ += dev > 0 ? cfg_.tempoUpdateSeconds : -cfg_.tempoUpdateSeconds;
            else fastRun_ = dev > 0 ? cfg_.tempoUpdateSeconds : -cfg_.tempoUpdateSeconds;
            const double sm =
                std::abs(fastRun_) >= cfg_.steadyFastSeconds ? cfg_.tempoSmoothSeconds : cfg_.steadySmoothSeconds;
            const double k = 1.0 - std::exp(-cfg_.tempoUpdateSeconds / std::max(1e-3, sm));
            tempo_ += k * (target - tempo_);
            lastConfident_ = now;
        }
        const bool challenger = bpm > 0.0 && bestConf >= cfg_.switchMinConfidence && bestConf > localConf &&
                                !sameBeat(bpm, tempo_, cfg_.steadyWindow);
        if (challenger) {
            const double w = std::min(1.0, bestConf / 0.5) * cfg_.tempoUpdateSeconds;
            if (switchBpm_ > 0.0 && std::abs(bpm / switchBpm_ - 1.0) < cfg_.steadyWindow) {
                switchEvidence_ += w;
                switchBpm_ += 0.3 * (bpm - switchBpm_);
            } else if (switchEvidence_ > w) {
                switchEvidence_ -= w;  // a third tempo: weakens the challenger, does not replace it yet
            } else {
                switchBpm_ = bpm;
                switchEvidence_ = w;
            }
            if (switchEvidence_ >= cfg_.switchSeconds) {
                tempo_ = switchBpm_;
                switchBpm_ = switchEvidence_ = 0.0;
                lastConfident_ = now;
            }
        } else {
            switchEvidence_ = std::max(0.0, switchEvidence_ - cfg_.tempoUpdateSeconds);
            if (switchEvidence_ == 0.0) switchBpm_ = 0.0;
        }
        if (lastConfident_ >= 0.0 && now - lastConfident_ > cfg_.steadyHoldSeconds) {
            tempo_ = 0.0;
            switchBpm_ = switchEvidence_ = 0.0;
        }
        return;
    }

    if (bpm > 0.0 && bestConf >= cfg_.minConfidence) {
        lastConfident_ = now;
        if (tempo_ <= 0.0) {
            tempo_ = bpm;
            acquiredAt_ = now;
            switchBpm_ = switchEvidence_ = 0.0;
        } else if (std::abs(bpm / tempo_ - 1.0) < 0.08) {
            tempo_ += smoothK * (bpm - tempo_);
            candidate_ = 0.0;
        } else if (candidate_ > 0.0 && std::abs(bpm / candidate_ - 1.0) < 0.08) {
            tempo_ = bpm;  // a new tempo, confirmed twice in a row
            candidate_ = 0.0;
        } else {
            candidate_ = bpm;
        }
    } else if (tempo_ > 0.0 && lastConfident_ >= 0.0 &&
               now - lastConfident_ > (cfg_.steady ? cfg_.steadyHoldSeconds : cfg_.holdSeconds)) {
        tempo_ = 0.0;
        candidate_ = 0.0;
    }
}

}  // namespace reforce::audio
