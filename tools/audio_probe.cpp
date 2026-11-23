// reforce-audio-probe: exercise the audio subsystem from the command line.
//
//   reforce-audio-probe --list
//   reforce-audio-probe --device "DDJ-1000 Loopback" --seconds 3
//   reforce-audio-probe --device default --seconds 3
//   reforce-audio-probe --file song.m4a [--seconds N] [--realtime [--monitor]]
//
// Prints one line per second: LEVEL, BASS, ENERGY (max over the second), raw
// peak, and the fft bin with the highest average value. Nothing is recorded.

#include "audio/Analyzer.hpp"
#include "audio/AudioFileSource.hpp"
#include "audio/CoreAudioInput.hpp"
#include "audio/MicPermission.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace reforce::audio;

namespace {

void usage() {
    std::fprintf(stderr,
                 "usage: reforce-audio-probe --list\n"
                 "       reforce-audio-probe --device NAME|UID|default [--seconds N] [--channels 0,1]\n"
                 "       reforce-audio-probe --file PATH [--seconds N] [--realtime [--monitor]] [--loop]\n"
                 "options: --fps F (default 60)  --fft-params \"Key=v ...\"\n");
}

struct SecondStats {
    double levelMax = 0, bassMax = 0, energyMax = 0, peak = 0, rmsSum = 0;
    double levelSum = 0, bassSum = 0, energySum = 0, fftAll = 0, fftOver1 = 0;
    int frames = 0;
    std::vector<double> fftSum;
    void reset(size_t bins) { *this = {}; fftSum.assign(bins, 0.0); }
    void add(const AudioFrame& f) {
        levelMax = std::max(levelMax, double(f.level));
        bassMax = std::max(bassMax, double(f.bass));
        energyMax = std::max(energyMax, double(f.energy));
        peak = std::max(peak, double(f.peak));
        rmsSum += f.rms;
        levelSum += f.level;
        bassSum += f.bass;
        energySum += f.energy;
        for (size_t i = 0; i < fftSum.size(); ++i) {
            fftSum[i] += f.fft[i];
            fftAll += f.fft[i];
            fftOver1 += f.fft[i] > 1.f ? 1 : 0;
        }
        ++frames;
    }
    void print(int sec, const AudioFrame& f) const {
        size_t best = 0;
        for (size_t i = 1; i < fftSum.size(); ++i) if (fftSum[i] > fftSum[best]) best = i;
        const double n = std::max(1, frames);
        const double nb = std::max<size_t>(1, fftSum.size()) * n;
        std::printf("t=%3ds  LEVEL %.3f/%.3f  BASS %.2f/%.2f  ENERGY %.2f/%.2f  (avg/max)  peak %.3f  rms %.4f  "
                    "fft mean %.2f >1 %4.1f%%  top bin %3zu (%5.0f Hz) %.2f\n",
                    sec, levelSum / n, levelMax, bassSum / n, bassMax, energySum / n, energyMax, peak, rmsSum / n,
                    fftAll / nb, 100.0 * fftOver1 / nb, best, 0.5 * (f.binLoHz[best] + f.binHiHz[best]),
                    fftSum[best] / n);
        std::fflush(stdout);
    }
};

int runList() {
    std::printf("microphone permission (this process / responsible app): %s\n", micPermissionName(microphonePermission()));
    for (const auto& d : listInputDevices()) {
        std::printf("%s %-36s ch=%-2d %6.0f Hz  uid=%s\n", d.isDefaultInput ? "*" : " ", d.name.c_str(),
                    d.inputChannels, d.sampleRate, d.uid.c_str());
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::string device, file, fftParams = kGForceFftParams;
    bool list = false, realtime = false, monitor = false, loop = false, haveDevice = false;
    double seconds = 3, fps = 60;
    std::vector<int> channels;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) { usage(); std::exit(2); }
            return argv[++i];
        };
        if (a == "--list") list = true;
        else if (a == "--device") { device = next(); haveDevice = true; }
        else if (a == "--file") file = next();
        else if (a == "--seconds") seconds = std::atof(next().c_str());
        else if (a == "--fps") fps = std::atof(next().c_str());
        else if (a == "--realtime") realtime = true;
        else if (a == "--monitor") monitor = true;
        else if (a == "--loop") loop = true;
        else if (a == "--fft-params") fftParams = next();
        else if (a == "--channels") {
            std::string s = next();
            for (char* p = s.data(); *p;) { channels.push_back(static_cast<int>(std::strtol(p, &p, 10))); if (*p) ++p; }
        } else { usage(); return 2; }
    }
    if (list) return runList();
    if (!haveDevice && file.empty()) { usage(); return 2; }
    if (fps <= 0) fps = 60;

    AnalyzerConfig cfg;
    const std::string unknown = applyGForceFftParams(cfg, fftParams);
    if (!unknown.empty()) std::fprintf(stderr, "note: unrecognized FFT params: %s\n", unknown.c_str());

    if (!file.empty()) {
        AudioFileSource src;
        FileOptions fo;
        fo.path = file;
        fo.realtime = realtime;
        fo.monitor = realtime && monitor;
        fo.loop = loop;
        if (!src.open(fo)) { std::fprintf(stderr, "error: %s\n", src.lastError().c_str()); return 1; }
        std::printf("%s, %.2f s\n", src.description().c_str(), src.durationSeconds());
        Analyzer an(cfg, src.sampleRate());
        SecondStats st;
        st.reset(cfg.fftBins);
        if (!realtime) {
            // Deterministic: exactly sr/fps samples per frame (fractional carry).
            const double perFrame = src.sampleRate() / fps;
            double carry = 0;
            const int totalFrames = static_cast<int>(seconds * fps);
            for (int fr = 0; fr < totalFrames; ++fr) {
                carry += perFrame;
                const size_t n = static_cast<size_t>(carry);
                carry -= static_cast<double>(n);
                src.advance(n);
                an.consume(src.ring());
                st.add(an.analyze(static_cast<double>(n) / src.sampleRate()));
                if ((fr + 1) % static_cast<int>(fps) == 0) {
                    st.print((fr + 1) / static_cast<int>(fps), an.frame());
                    st.reset(cfg.fftBins);
                }
                if (src.state() == SourceState::Finished) break;
            }
            return 0;
        }
        src.start();
        const auto t0 = std::chrono::steady_clock::now();
        auto last = t0;
        int sec = 0;
        while (src.state() == SourceState::Running) {
            std::this_thread::sleep_for(std::chrono::microseconds(static_cast<int>(1e6 / fps)));
            const auto now = std::chrono::steady_clock::now();
            const double dt = std::chrono::duration<double>(now - last).count();
            last = now;
            an.consume(src.ring());
            st.add(an.analyze(dt));
            const double el = std::chrono::duration<double>(now - t0).count();
            if (el >= sec + 1) { st.print(++sec, an.frame()); st.reset(cfg.fftBins); }
            if (el >= seconds) break;
        }
        src.stop();
        return 0;
    }

    // Live device.
    std::printf("microphone permission: %s\n", micPermissionName(microphonePermission()));
    CoreAudioInput in;
    InputOptions io;
    io.device = (device == "default") ? std::string() : device;
    io.channels = channels;
    if (!in.open(io)) {
        std::fprintf(stderr, "error: %s\n", in.lastError().c_str());
        return 1;
    }
    std::printf("%s\n", in.description().c_str());
    Analyzer an(cfg, in.sampleRate());
    SecondStats st;
    st.reset(cfg.fftBins);
    const auto t0 = std::chrono::steady_clock::now();
    auto last = t0;
    int sec = 0;
    double maxPeak = 0;
    uint64_t consumed = 0;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::microseconds(static_cast<int>(1e6 / fps)));
        in.service();
        if (in.sampleRate() > 0) an.setSampleRate(in.sampleRate());
        const auto now = std::chrono::steady_clock::now();
        const double dt = std::chrono::duration<double>(now - last).count();
        last = now;
        consumed += an.consume(in.ring());
        const AudioFrame& f = an.analyze(dt);
        maxPeak = std::max(maxPeak, double(f.peak));
        st.add(f);
        const double el = std::chrono::duration<double>(now - t0).count();
        if (el >= sec + 1) {
            st.print(++sec, f);
            st.reset(cfg.fftBins);
            if (in.state() != SourceState::Running)
                std::printf("  state: %s %s\n", sourceStateName(in.state()), in.lastError().c_str());
        }
        if (el >= seconds) break;
    }
    std::printf("callbacks %llu, samples %llu, render errors %llu, dropped %llu, state %s\n",
                static_cast<unsigned long long>(in.callbackCount()), static_cast<unsigned long long>(consumed),
                static_cast<unsigned long long>(in.renderErrors()),
                static_cast<unsigned long long>(in.ring().droppedSamples()), sourceStateName(in.state()));
    if (consumed > 0 && maxPeak == 0.0)
        std::printf("WARNING: input is exactly zero. Either the source is silent or microphone access (TCC) is "
                    "denied for the responsible app.\n");
    in.close();
    return 0;
}
