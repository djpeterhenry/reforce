#include <doctest/doctest.h>

#include "audio/Analyzer.hpp"
#include "audio/AudioFileSource.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

using namespace reforce::audio;

namespace {

std::string tempWav(const char* name) {
    return (std::filesystem::temp_directory_path() / name).string();
}

// 2 s stereo: L = 1 kHz sine 0.5, R = same (so the mono downmix is the sine).
std::vector<float> makeStereo(double sr, double seconds, double hz, double amp) {
    const size_t n = static_cast<size_t>(sr * seconds);
    std::vector<float> v(n * 2);
    for (size_t i = 0; i < n; ++i) {
        const float s = static_cast<float>(amp * std::sin(2.0 * M_PI * hz * double(i) / sr));
        v[2 * i] = s;
        v[2 * i + 1] = s;
    }
    return v;
}

}  // namespace

TEST_CASE("AudioFileSource: deterministic advance decodes exact frames and feeds the analyzer") {
    const double sr = 44100.0;
    const std::string path = tempWav("reforce_test_1k.wav");
    const auto data = makeStereo(sr, 2.0, 1000.0, 0.5);
    REQUIRE(writeWavFloat(path, data.data(), data.size() / 2, 2, sr));

    AudioFileSource src;
    FileOptions fo;
    fo.path = path;
    REQUIRE(src.open(fo));
    CHECK(src.sampleRate() == doctest::Approx(sr));
    CHECK(src.fileChannels() == 2);
    CHECK(src.lengthFrames() == static_cast<int64_t>(data.size() / 2));

    // First chunk is bit-exact with the input (float WAV, mono downmix of equal channels).
    CHECK(src.advance(100) == 100);
    std::vector<float> got(100);
    REQUIRE(src.ring().read(got.data(), 100) == 100);
    for (size_t i = 0; i < 100; ++i) CHECK(got[i] == doctest::Approx(data[2 * i]).epsilon(1e-6));

    Analyzer an({}, src.sampleRate());
    const size_t perFrame = 735;  // 44100 / 60
    for (int f = 0; f < 60; ++f) {
        CHECK(src.advance(perFrame) == perFrame);
        CHECK(an.consume(src.ring()) == perFrame);
        an.analyze(perFrame / sr);
    }
    CHECK(src.positionFrames() == 100 + 60 * 735);
    // G-Force-fitted model (docs/audio-probe-results.md): a -6 dBFS 1 kHz tone lights fft() broadly and
    // reads LEVEL ~0.5 after 1 s (G-Force: 0.5-0.6).
    CHECK(an.frame().fft[size_t(an.binForFrequency(1000.0))] > 0.3f);
    CHECK(an.frame().level > 0.35f);
    CHECK(an.frame().level < 0.7f);

    // Run past the end: zero-padded, state Finished.
    size_t real = 0;
    for (int f = 0; f < 120; ++f) real += src.advance(perFrame);
    CHECK(real == data.size() / 2 - 100 - 60 * 735);
    CHECK(src.state() == SourceState::Finished);
    std::remove(path.c_str());
}

TEST_CASE("AudioFileSource: looping wraps to the start") {
    const double sr = 48000.0;
    const std::string path = tempWav("reforce_test_loop.wav");
    std::vector<float> ramp(1000);
    for (size_t i = 0; i < ramp.size(); ++i) ramp[i] = static_cast<float>(i) / 1000.f;
    REQUIRE(writeWavFloat(path, ramp.data(), ramp.size(), 1, sr));
    AudioFileSource src;
    FileOptions fo;
    fo.path = path;
    fo.loop = true;
    REQUIRE(src.open(fo));
    CHECK(src.advance(2500) == 2500);
    std::vector<float> out(2500);
    REQUIRE(src.ring().read(out.data(), 2500) == 2500);
    CHECK(out[999] == doctest::Approx(0.999f));
    CHECK(out[1000] == 0.f);
    CHECK(out[2100] == doctest::Approx(0.1f));
    CHECK(src.state() != SourceState::Finished);
    std::remove(path.c_str());
}

TEST_CASE("AudioFileSource: realtime mode (no monitoring) is paced by the clock") {
    const double sr = 48000.0;
    const std::string path = tempWav("reforce_test_rt.wav");
    const auto data = makeStereo(sr, 1.0, 440.0, 0.3);
    REQUIRE(writeWavFloat(path, data.data(), data.size() / 2, 2, sr));
    AudioFileSource src;
    FileOptions fo;
    fo.path = path;
    fo.realtime = true;  // monitor stays off: no sound in tests
    REQUIRE(src.open(fo));
    REQUIRE(src.start());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const size_t avail = src.ring().available();
    src.stop();
    // ~0.2 s worth (generous bounds for scheduler jitter), far less than the whole file.
    CHECK(avail > static_cast<size_t>(0.1 * sr));
    CHECK(avail < static_cast<size_t>(0.6 * sr));
    std::remove(path.c_str());
}

TEST_CASE("AudioFileSource: missing file reports an error") {
    AudioFileSource src;
    FileOptions fo;
    fo.path = "/nonexistent/reforce.wav";
    CHECK_FALSE(src.open(fo));
    CHECK(src.state() == SourceState::Error);
    CHECK_FALSE(src.lastError().empty());
}
