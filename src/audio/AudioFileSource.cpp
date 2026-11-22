#include "audio/AudioFileSource.hpp"

#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

namespace reforce::audio {

namespace {
constexpr size_t kChunk = 4096;
constexpr UInt32 kMaxOutFrames = 8192;

ExtAudioFileRef asFile(void* p) { return static_cast<ExtAudioFileRef>(p); }

std::string statusStr(const char* what, OSStatus st) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "%s failed: OSStatus %d", what, static_cast<int>(st));
    return buf;
}
}  // namespace

// Output (monitoring) context: the render callback pulls interleaved stereo
// from `play`, outputs it, and pushes its mono downmix into the analysis ring.
struct AudioFileSource::Monitor {
    AudioUnit unit = nullptr;
    SpscRingBuffer play{1u << 16};
    SpscRingBuffer* analysis = nullptr;
    std::vector<float> mono = std::vector<float>(kMaxOutFrames);
};

static OSStatus monitorProc(void* refCon, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32,
                            UInt32 frames, AudioBufferList* io) {
    auto* m = static_cast<AudioFileSource::Monitor*>(refCon);
    auto* out = static_cast<float*>(io->mBuffers[0].mData);
    const size_t want = static_cast<size_t>(frames) * 2;
    size_t got = m->play.read(out, want);
    got -= got % 2;
    if (got < want) std::memset(out + got, 0, (want - got) * sizeof(float));
    const size_t n = std::min<size_t>(got / 2, kMaxOutFrames);
    for (size_t i = 0; i < n; ++i) m->mono[i] = 0.5f * (out[2 * i] + out[2 * i + 1]);
    if (n) m->analysis->write(m->mono.data(), n);
    return noErr;
}

AudioFileSource::AudioFileSource() = default;
AudioFileSource::~AudioFileSource() { close(); }

std::string AudioFileSource::description() const {
    return "file \"" + opts_.path + "\" " + std::to_string(static_cast<int>(sampleRate_)) + " Hz, " +
           std::to_string(channels_) + " ch, " + sourceStateName(state());
}

bool AudioFileSource::open(const FileOptions& opts) {
    close();
    opts_ = opts;
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(
        nullptr, reinterpret_cast<const UInt8*>(opts.path.c_str()), static_cast<CFIndex>(opts.path.size()), false);
    ExtAudioFileRef f = nullptr;
    OSStatus st = ExtAudioFileOpenURL(url, &f);
    CFRelease(url);
    if (st != noErr) {
        lastError_ = statusStr("ExtAudioFileOpenURL", st) + " (" + opts.path + ")";
        state_ = SourceState::Error;
        return false;
    }
    file_ = f;
    AudioStreamBasicDescription ff{};
    UInt32 size = sizeof ff;
    ExtAudioFileGetProperty(f, kExtAudioFileProperty_FileDataFormat, &size, &ff);
    SInt64 len = 0;
    size = sizeof len;
    ExtAudioFileGetProperty(f, kExtAudioFileProperty_FileLengthFrames, &size, &len);
    channels_ = static_cast<int>(std::max<UInt32>(1, ff.mChannelsPerFrame));
    sampleRate_ = ff.mSampleRate > 0 ? ff.mSampleRate : 44100.0;
    lengthFrames_ = len;

    AudioStreamBasicDescription cf{};
    cf.mSampleRate = sampleRate_;
    cf.mFormatID = kAudioFormatLinearPCM;
    cf.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    cf.mBitsPerChannel = 32;
    cf.mChannelsPerFrame = static_cast<UInt32>(channels_);
    cf.mFramesPerPacket = 1;
    cf.mBytesPerFrame = cf.mBytesPerPacket = sizeof(float) * static_cast<UInt32>(channels_);
    st = ExtAudioFileSetProperty(f, kExtAudioFileProperty_ClientDataFormat, sizeof cf, &cf);
    if (st != noErr) {
        lastError_ = statusStr("set client format", st);
        close();
        state_ = SourceState::Error;
        return false;
    }
    interleaved_.assign(kChunk * static_cast<size_t>(channels_), 0.f);
    mono_.assign(kChunk, 0.f);
    ring_ = std::make_unique<SpscRingBuffer>(opts.ringCapacity);
    position_ = 0;
    state_ = SourceState::Stopped;
    return true;
}

void AudioFileSource::close() {
    stop();
    if (file_) {
        ExtAudioFileDispose(asFile(file_));
        file_ = nullptr;
    }
    state_ = SourceState::Stopped;
}

// Decodes up to `frames` (<= kChunk) frames into interleaved_ and the mono
// downmix into dst. Returns frames decoded (0 at end of a non-looping file).
size_t AudioFileSource::decode(float* dst, size_t frames) {
    if (!file_) return 0;
    frames = std::min(frames, kChunk);
    size_t done = 0;
    bool rewound = false;
    while (done < frames) {
        AudioBufferList abl{};
        abl.mNumberBuffers = 1;
        abl.mBuffers[0].mNumberChannels = static_cast<UInt32>(channels_);
        abl.mBuffers[0].mData = interleaved_.data() + done * channels_;
        abl.mBuffers[0].mDataByteSize = static_cast<UInt32>((frames - done) * channels_ * sizeof(float));
        UInt32 n = static_cast<UInt32>(frames - done);
        if (ExtAudioFileRead(asFile(file_), &n, &abl) != noErr) n = 0;
        if (n == 0) {
            if (opts_.loop && !rewound && lengthFrames_ > 0) {
                ExtAudioFileSeek(asFile(file_), 0);
                position_ = 0;
                rewound = true;
                continue;
            }
            break;
        }
        rewound = false;
        done += n;
        position_ += n;
    }
    const float inv = 1.f / static_cast<float>(channels_);
    for (size_t i = 0; i < done; ++i) {
        float s = 0.f;
        for (int c = 0; c < channels_; ++c) s += interleaved_[i * channels_ + c];
        dst[i] = s * inv;
    }
    return done;
}

size_t AudioFileSource::advance(size_t frames) {
    if (!file_ || opts_.realtime) return 0;
    size_t real = 0;
    while (frames > 0) {
        const size_t want = std::min(frames, kChunk);
        size_t n = decode(mono_.data(), want);
        real += n;
        if (n < want) {
            std::fill(mono_.begin() + static_cast<long>(n), mono_.begin() + static_cast<long>(want), 0.f);
            state_ = SourceState::Finished;
        } else if (state_ != SourceState::Finished) {
            state_ = SourceState::Running;
        }
        ring_->write(mono_.data(), want);
        frames -= want;
    }
    return real;
}

bool AudioFileSource::start() {
    if (!file_ || !opts_.realtime) return false;
    stop();
    if (opts_.monitor) {
        monitor_ = std::make_unique<Monitor>();
        monitor_->analysis = ring_.get();
        AudioComponentDescription desc{kAudioUnitType_Output, kAudioUnitSubType_DefaultOutput,
                                       kAudioUnitManufacturer_Apple, 0, 0};
        AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
        OSStatus st = comp ? AudioComponentInstanceNew(comp, &monitor_->unit) : -1;
        AudioStreamBasicDescription f{};
        f.mSampleRate = sampleRate_;
        f.mFormatID = kAudioFormatLinearPCM;
        f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
        f.mBitsPerChannel = 32;
        f.mChannelsPerFrame = 2;
        f.mFramesPerPacket = 1;
        f.mBytesPerFrame = f.mBytesPerPacket = 2 * sizeof(float);
        if (st == noErr)
            st = AudioUnitSetProperty(monitor_->unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f,
                                      sizeof f);
        AURenderCallbackStruct cb{monitorProc, monitor_.get()};
        if (st == noErr)
            st = AudioUnitSetProperty(monitor_->unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input,
                                      0, &cb, sizeof cb);
        if (st == noErr) st = AudioUnitInitialize(monitor_->unit);
        if (st != noErr) {
            lastError_ = statusStr("monitor output setup", st);
            if (monitor_->unit) AudioComponentInstanceDispose(monitor_->unit);
            monitor_.reset();
            state_ = SourceState::Error;
            return false;
        }
    }
    stopFeeder_ = false;
    state_ = SourceState::Running;
    feeder_ = std::thread([this] { feederLoop(); });
    if (monitor_) AudioOutputUnitStart(monitor_->unit);
    return true;
}

void AudioFileSource::stop() {
    stopFeeder_ = true;
    if (feeder_.joinable()) feeder_.join();
    if (monitor_) {
        AudioOutputUnitStop(monitor_->unit);
        AudioUnitUninitialize(monitor_->unit);
        AudioComponentInstanceDispose(monitor_->unit);
        monitor_.reset();
    }
}

void AudioFileSource::feederLoop() {
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    int64_t produced = 0;
    std::vector<float> stereo(kChunk * 2);
    while (!stopFeeder_) {
        if (monitor_) {
            // Keep ~0.2 s queued for the output callback, which paces playback.
            const size_t target = static_cast<size_t>(0.2 * sampleRate_) * 2;
            while (!stopFeeder_ && monitor_->play.available() < target) {
                const size_t free = monitor_->play.capacity() - monitor_->play.available();
                const size_t n = decode(mono_.data(), std::min(kChunk, free / 2));
                if (n == 0) break;
                for (size_t i = 0; i < n; ++i) {
                    const float l = interleaved_[i * channels_];
                    const float r = channels_ > 1 ? interleaved_[i * channels_ + 1] : l;
                    stereo[2 * i] = l;
                    stereo[2 * i + 1] = r;
                }
                monitor_->play.write(stereo.data(), n * 2);
            }
        } else {
            const double elapsed = std::chrono::duration<double>(clock::now() - t0).count();
            const int64_t target = static_cast<int64_t>(elapsed * sampleRate_);
            while (!stopFeeder_ && produced < target) {
                const size_t n = decode(mono_.data(), static_cast<size_t>(std::min<int64_t>(kChunk, target - produced)));
                if (n == 0) break;
                ring_->write(mono_.data(), n);
                produced += static_cast<int64_t>(n);
            }
        }
        if (!opts_.loop && lengthFrames_ > 0 && position_.load() >= lengthFrames_) {
            state_ = SourceState::Finished;
            if (!monitor_ || monitor_->play.available() == 0) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

bool writeWavFloat(const std::string& path, const float* data, size_t frames, int channels, double sampleRate) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };  // little-endian host (arm64/x86_64)
    auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    const uint32_t dataBytes = static_cast<uint32_t>(frames * static_cast<size_t>(channels) * sizeof(float));
    std::fwrite("RIFF", 1, 4, f);
    u32(4 + (8 + 18) + (8 + 4) + (8 + dataBytes));
    std::fwrite("WAVE", 1, 4, f);
    std::fwrite("fmt ", 1, 4, f);
    u32(18);
    u16(3);  // WAVE_FORMAT_IEEE_FLOAT
    u16(static_cast<uint16_t>(channels));
    u32(static_cast<uint32_t>(sampleRate));
    u32(static_cast<uint32_t>(sampleRate) * static_cast<uint32_t>(channels) * 4);
    u16(static_cast<uint16_t>(channels * 4));
    u16(32);
    u16(0);  // cbSize
    std::fwrite("fact", 1, 4, f);
    u32(4);
    u32(static_cast<uint32_t>(frames));
    std::fwrite("data", 1, 4, f);
    u32(dataBytes);
    std::fwrite(data, sizeof(float), frames * static_cast<size_t>(channels), f);
    return std::fclose(f) == 0;
}

}  // namespace reforce::audio
