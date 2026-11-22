#pragma once
// Audio file source (wav/aiff/mp3/m4a/... via ExtAudioFile) feeding the same
// ring-buffer -> Analyzer path as live input.
//
// Modes:
//  * Deterministic: nothing runs in the background. The owner calls
//    advance(N) once per frame, which decodes exactly N frames (downmixed to
//    mono) into ring(); then Analyzer::consume() + analyze(N / sampleRate()).
//    Runs as fast as the caller goes; output is bit-for-bit repeatable.
//  * Realtime: start() runs a feeder thread paced by the wall clock. With
//    `monitor`, the default output device plays the file and its render
//    callback is what feeds ring(), so the analysis follows what you hear.
//    Monitoring is OFF by default.

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "audio/AudioSource.hpp"

namespace reforce::audio {

struct FileOptions {
    std::string path;
    bool realtime = false;  // false = deterministic (advance())
    bool monitor = false;   // realtime only: also play to the default output
    bool loop = false;
    size_t ringCapacity = 1u << 17;
};

class AudioFileSource final : public AudioSource {
public:
    AudioFileSource();
    ~AudioFileSource() override;
    AudioFileSource(const AudioFileSource&) = delete;
    AudioFileSource& operator=(const AudioFileSource&) = delete;

    bool open(const FileOptions& opts);  // opens + reads format; does not start
    void close();

    // Deterministic mode: decode exactly `frames` mono frames into ring()
    // (zero-padded after the end of a non-looping file). Returns the number of
    // real (non-padding) frames decoded.
    size_t advance(size_t frames);

    // Realtime mode.
    bool start();
    void stop();

    SpscRingBuffer& ring() override { return *ring_; }
    double sampleRate() const override { return sampleRate_; }
    SourceState state() const override { return state_.load(); }
    std::string description() const override;

    int fileChannels() const { return channels_; }
    int64_t lengthFrames() const { return lengthFrames_; }
    double durationSeconds() const { return sampleRate_ > 0 ? lengthFrames_ / sampleRate_ : 0.0; }
    int64_t positionFrames() const { return position_.load(); }
    const std::string& lastError() const { return lastError_; }

    struct Monitor;  // output-unit context (public for the C callback)

private:
    // Decode up to `frames` frames, downmixed to mono, into dst. Handles looping.
    size_t decode(float* dst, size_t frames);
    void feederLoop();

    FileOptions opts_;
    void* file_ = nullptr;  // ExtAudioFileRef
    int channels_ = 0;
    double sampleRate_ = 0.0;
    int64_t lengthFrames_ = 0;
    std::atomic<int64_t> position_{0};
    std::unique_ptr<SpscRingBuffer> ring_;
    std::atomic<SourceState> state_{SourceState::Stopped};
    std::string lastError_;
    std::vector<float> interleaved_;  // decode scratch
    std::vector<float> mono_;         // decode scratch
    std::thread feeder_;
    std::atomic<bool> stopFeeder_{false};
    std::unique_ptr<Monitor> monitor_;
};

// Write a mono or interleaved-stereo 32-bit float WAV (tests, tools).
bool writeWavFloat(const std::string& path, const float* interleaved, size_t frames, int channels,
                   double sampleRate);

}  // namespace reforce::audio
