#include <doctest/doctest.h>

#include "audio/Analyzer.hpp"
#include "audio/SpscRingBuffer.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <vector>

using namespace reforce::audio;

namespace {

constexpr double kSr = 48000.0;
constexpr double kFps = 60.0;
constexpr size_t kPerFrame = 800;  // 48000 / 60

struct Feeder {
    Analyzer& an;
    uint64_t n = 0;
    std::vector<float> buf = std::vector<float>(kPerFrame);
    // Feed `frames` render frames of gen(sampleIndex) and analyze each.
    const AudioFrame& run(int frames, const std::function<float(uint64_t)>& gen) {
        for (int f = 0; f < frames; ++f) {
            for (size_t i = 0; i < kPerFrame; ++i) buf[i] = gen(n++);
            an.pushSamples(buf.data(), kPerFrame);
            an.analyze(1.0 / kFps);
        }
        return an.frame();
    }
};

std::function<float(uint64_t)> sine(double hz, double amp) {
    return [=](uint64_t i) { return static_cast<float>(amp * std::sin(2.0 * M_PI * hz * double(i) / kSr)); };
}
float silence(uint64_t) { return 0.f; }

// Deterministic pink-ish noise (Paul Kellet's economy filter on an LCG), scaled to `rms`.
std::function<float(uint64_t)> pink(double rms) {
    struct S { uint32_t x = 12345; double b0 = 0, b1 = 0, b2 = 0; };
    auto st = std::make_shared<S>();
    return [=](uint64_t) {
        st->x = st->x * 1664525u + 1013904223u;
        const double w = (double(st->x >> 8) / double(1u << 24)) * 2.0 - 1.0;
        st->b0 = 0.99765 * st->b0 + w * 0.0990460;
        st->b1 = 0.96300 * st->b1 + w * 0.2965164;
        st->b2 = 0.57000 * st->b2 + w * 1.0526913;
        // ~unit RMS for uniform white input of RMS 1/sqrt(3)
        return static_cast<float>(rms * (st->b0 + st->b1 + st->b2 + w * 0.1848) * 0.42);
    };
}

}  // namespace

TEST_CASE("Analyzer: G-Force FFT param string parses fully") {
    AnalyzerConfig cfg;
    cfg.envelopes = 3;
    cfg.longRamp = 1;
    CHECK(applyGForceFftParams(cfg, kGForceFftParams).empty());
    CHECK(cfg.envelopes == 16);
    CHECK(cfg.longRamp == doctest::Approx(185.0));
    CHECK(cfg.persistLowThresh == doctest::Approx(0.035));
    CHECK(cfg.floorRaiseMax == doctest::Approx(0.7));
    // Extra whitespace, case, unknown keys.
    CHECK(applyGForceFftParams(cfg, "  shortimpulse=0.5    Bogus=3 junk ") == "Bogus=3 junk");
    CHECK(cfg.shortImpulse == doctest::Approx(0.5));
}

TEST_CASE("Analyzer: output sizes and G-Force bin layout (120 Hz .. 5.5 kHz, log)") {
    Analyzer an({}, kSr);
    const AudioFrame& f = an.analyze(1.0 / kFps);
    CHECK(f.wave.size() == 255);
    CHECK(f.fft.size() == 255);
    CHECK(an.binCenterHz(0) == doctest::Approx(120.0));
    CHECK(an.binCenterHz(254) == doctest::Approx(5500.0));
    CHECK(an.binCenterHz(127) == doctest::Approx(std::sqrt(120.0 * 5500.0)).epsilon(1e-6));
    for (size_t i = 1; i < f.binLoHz.size(); ++i) CHECK(f.binLoHz[i] > f.binLoHz[i - 1]);
    CHECK(an.binForFrequency(1000.0) == doctest::Approx(141).epsilon(0.01));  // G-Force: 1 kHz at s ~ .56

    AnalyzerConfig lin;
    lin.linearSpacing = true;
    lin.freqLoHz = 100.0;
    lin.freqHiHz = 2640.0;
    Analyzer al(lin, kSr);
    CHECK(al.binCenterHz(127) == doctest::Approx(1370.0));
}

TEST_CASE("Analyzer: silence gives zeros") {
    Analyzer an({}, kSr);
    Feeder fd{an};
    const AudioFrame& f = fd.run(30, silence);
    CHECK(*std::max_element(f.fft.begin(), f.fft.end()) == 0.f);
    CHECK(*std::max_element(f.wave.begin(), f.wave.end()) == 0.f);
    CHECK(f.level == 0.f);
    CHECK(f.bass == 0.f);
    CHECK(f.energy == 0.f);
    CHECK(f.mid == 0.f);
    CHECK(f.treble == 0.f);
    CHECK(f.beat == 0.f);
    CHECK(f.tempo == 0.f);
}

// Expected values below come from the Phase 5C.4 fit to G-Force (docs/audio-probe-results.md);
// the G-Force measurement is quoted next to each check.
TEST_CASE("Analyzer: 1 kHz tone is broad and LEVEL rises with level (G-Force tone steps)") {
    Analyzer an({}, kSr);
    Feeder fd{an};
    const AudioFrame& f = fd.run(120, sine(1000.0, 1.0));  // 0 dBFS for 2 s
    const int k = an.binForFrequency(1000.0);
    // Raw band amplitude (before the tilt) peaks at the tone: a_j / tilt_j is largest at 1 kHz.
    size_t best = 0;
    for (size_t i = 0; i < f.fftRaw.size(); ++i)
        if (f.fftRaw[i] / std::pow(an.binCenterHz(int(i)) / 1000.0, an.config().tilt) >
            f.fftRaw[best] / std::pow(an.binCenterHz(int(best)) / 1000.0, an.config().tilt))
            best = i;
    CHECK(std::abs(int(best) - k) <= 2);
    CHECK(f.fft[size_t(k)] > 0.4f);  // G-Force ~0.92 (clamped plateau); the fitted model reads ~0.56
    // Broad: still lit an octave below and above (G-Force: half max ~ -1.4 / +1.9 octaves at 0 dBFS).
    CHECK(f.fft[size_t(an.binForFrequency(500.0))] > 0.4f);
    CHECK(f.fft[size_t(an.binForFrequency(2000.0))] > 0.15f);
    CHECK(f.level == doctest::Approx(0.60).epsilon(0.15));  // G-Force 0.60
    CHECK(f.bass == doctest::Approx(4.0 * f.level));          // G-Force BASS = 4.0 x LEVEL

    Analyzer quiet({}, kSr);
    Feeder fq{quiet};
    fq.run(120, sine(1000.0, 0.01));                           // -40 dBFS
    CHECK(quiet.frame().level < 0.05f);                        // G-Force 0.00-0.02
    fq.run(120, sine(1000.0, 0.1));                            // -20 dBFS
    CHECK(quiet.frame().level == doctest::Approx(0.27).epsilon(0.3));  // G-Force 0.25-0.33
}

TEST_CASE("Analyzer: noise onset overshoots, then settles; release is instant (G-Force attack_release)") {
    Analyzer an({}, kSr);
    Feeder fd{an};
    fd.run(150, silence);
    auto noise = pink(0.25);  // -12 dBFS RMS
    fd.run(12, noise);        // 0.2 s
    const double early = an.frame().level;
    fd.run(78, noise);        // 1.5 s
    const double settled = an.frame().level;
    CHECK(early > settled);                                   // G-Force 0.52 -> 0.28
    CHECK(settled == doctest::Approx(0.33).epsilon(0.3));
    fd.run(4, silence);                                       // ~67 ms
    CHECK(an.frame().level < 0.03f);                          // G-Force: 0 within 1-2 frames
    CHECK(an.frame().fft[100] < 0.03f);
}

TEST_CASE("Analyzer: ENERGY rises over ~2 s and decays with ~1.4 s (G-Force)") {
    Analyzer an({}, kSr);
    Feeder fd{an};
    fd.run(60, pink(0.25));
    const double e1 = an.frame().energy;
    fd.run(60, pink(0.25));
    const double e2 = an.frame().energy;
    CHECK(e1 > 0.15);
    CHECK(e2 > e1);                                           // G-Force: ~0.35 at 1 s, ~0.6 at 2 s
    CHECK(e2 < 0.9);
    fd.run(120, silence);
    // follow() with 1.43 s release from e2 for 2 s, plus the peak-hold tail
    CHECK(an.frame().energy == doctest::Approx(e2 * std::exp(-2.0 / 1.43)).epsilon(0.35));
    CHECK(an.frame().energy <= 1.f);
}

TEST_CASE("Analyzer: Audio.FFT.Smooth sets the per-bin release") {
    for (double smooth : {1.0, 8.0}) {
        AnalyzerConfig cfg;
        cfg.fftReleaseSeconds = smooth / 60.0;
        Analyzer an(cfg, kSr);
        Feeder fd{an};
        fd.run(60, sine(1000.0, 0.5));
        const double on = an.frame().fft[141];
        fd.run(6, silence);  // 0.1 s: the 2048-sample window has emptied after ~3 frames
        const double off = an.frame().fft[141];
        if (smooth == 1.0) CHECK(off < 0.01 * on);
        else CHECK(off > 0.1 * on);
    }
}

TEST_CASE("Analyzer: Audio.Response.Scale scales fft()") {
    AnalyzerConfig cfg;
    cfg.responseScale = 2.0;
    Analyzer a2(cfg, kSr), a1({}, kSr);
    Feeder f2{a2}, f1{a1};
    f2.run(30, sine(700.0, 0.3));
    f1.run(30, sine(700.0, 0.3));
    CHECK(a2.frame().fft[120] == doctest::Approx(2.0 * a1.frame().fft[120]));
}

TEST_CASE("Analyzer: wave[] is the raw sample (G-Force mag()), optional auto-gain and smoothing") {
    AnalyzerConfig cfg;
    cfg.pcmSmoothSeconds = 0.0;
    cfg.pcmBoxcar = 1;  // raw samples (the G-Force default moving average is tested below)
    Analyzer raw(cfg, kSr);
    Feeder fr{raw};
    const AudioFrame& a = fr.run(10, sine(440.0, 0.5));
    const float pk = *std::max_element(a.wave.begin(), a.wave.end());
    CHECK(pk == doctest::Approx(0.5).epsilon(0.02));          // G-Force: -6 dBFS tone -> mag +-0.48..0.5
    // Newest sample is last.
    const float newest = static_cast<float>(0.5 * std::sin(2.0 * M_PI * 440.0 * double(fr.n - 1) / kSr));
    CHECK(a.wave.back() == doctest::Approx(newest).epsilon(1e-4));

    AnalyzerConfig agcCfg;
    agcCfg.pcmAutoGain = true;  // reforce option: peaks normalized to ~1
    Analyzer agc(agcCfg, kSr);
    Feeder fa{agc};
    const AudioFrame& b = fa.run(10, sine(440.0, 0.5));
    CHECK(*std::max_element(b.wave.begin(), b.wave.end()) == doctest::Approx(1.0).epsilon(0.1));
}

TEST_CASE("Analyzer: mag() moving average (G-Force: 7 points, probe run 16)") {
    // Default config at 44.1 kHz: 1 kHz passes at ~0.96, 4 kHz at ~0.5, 6.3 kHz (= SR / 7, the first null) at ~0.
    // (These tests run at 48 kHz: |H(f)| = |sin(7 pi f / SR) / (7 sin(pi f / SR))|.)
    auto peakAfter = [](double hz) {
        Analyzer an({}, kSr);
        Feeder fd{an};
        const AudioFrame& f = fd.run(10, sine(hz, 0.5));
        float pk = 0.f;
        for (size_t i = 16; i < f.wave.size(); ++i) pk = std::max(pk, std::fabs(f.wave[i]));
        return pk / 0.5f;
    };
    CHECK(peakAfter(1000.0) == doctest::Approx(0.96).epsilon(0.03));
    CHECK(peakAfter(4000.0) == doctest::Approx(0.533).epsilon(0.05));
    CHECK(peakAfter(kSr / 7.0) < 0.05f);
    // Zero history at the oldest end: the first point is x0 / 7.
    AnalyzerConfig raw;
    raw.pcmBoxcar = 1;
    Analyzer a7({}, kSr), a1(raw, kSr);
    Feeder f7{a7}, f1{a1};
    const AudioFrame& w7 = f7.run(3, sine(300.0, 0.5));
    const AudioFrame& w1 = f1.run(3, sine(300.0, 0.5));
    CHECK(w7.wave[0] == doctest::Approx(w1.wave[0] / 7.f).epsilon(1e-3));
}

TEST_CASE("Analyzer: MID / TREBLE follow their bands") {
    Analyzer an({}, kSr);
    Feeder fd{an};
    fd.run(60, sine(800.0, 0.3));
    CHECK(an.frame().mid > 0.7f);
    CHECK(an.frame().treble < 0.2f);
    fd.run(60, sine(6000.0, 0.3));
    CHECK(an.frame().treble > 0.7f);
    CHECK(an.frame().mid < 0.2f);
}

TEST_CASE("Analyzer: consume() drains a ring and is deterministic") {
    SpscRingBuffer ring(1 << 14);
    Analyzer a({}, kSr), b({}, kSr);
    std::vector<float> buf(kPerFrame);
    uint64_t n = 0;
    auto gen = sine(330.0, 0.3);
    for (int f = 0; f < 30; ++f) {
        for (auto& s : buf) {
            s = gen(n) + (n % 7 == 0 ? 0.05f : 0.f);
            ++n;
        }
        ring.write(buf.data(), buf.size());
        CHECK(a.consume(ring) == kPerFrame);
        b.pushSamples(buf.data(), buf.size());
        a.analyze(1.0 / kFps);
        b.analyze(1.0 / kFps);
    }
    CHECK(a.frame().fft == b.frame().fft);
    CHECK(a.frame().wave == b.frame().wave);
    CHECK(a.frame().bass == b.frame().bass);
    CHECK(a.frame().samplesConsumed == 30 * kPerFrame);
}

TEST_CASE("Analyzer: starvation decays outputs to zero") {
    Analyzer an({}, kSr);
    Feeder fd{an};
    fd.run(60, sine(300.0, 0.5));
    CHECK(an.frame().level > 0.2f);
    for (int i = 0; i < 180; ++i) an.analyze(1.0 / kFps);  // no new samples for 3 s
    CHECK(an.frame().level < 0.01f);
    CHECK(*std::max_element(an.frame().fft.begin(), an.frame().fft.end()) == 0.f);
}
