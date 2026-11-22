// reforce-audio-play: play a WAV (any ExtAudioFile format) to ONE named CoreAudio output device.
//
//   reforce-audio-play --list-outputs
//   reforce-audio-play --device "reforce probe" --check          exit 0 if the device exists, else 1
//   reforce-audio-play --device "reforce probe" FILE.wav [--gain DB]
//   reforce-audio-play --device "reforce probe" --route-test FILE.wav
//       Plays FILE and at the same time records the INPUT side of the same device (a Loopback device
//       outputs what it is played), then finds the file's sync click (its loudest 50 ms burst near the
//       start) in the recording and prints route_latency_ms = captured click time - scheduled click time
//       (both as host times), the capture/playback gain and the residual after aligning the whole file.
//
// Built for the Phase 5C audio probes: test tones go to the silent Loopback device "reforce probe" (which
// G-Force reads as its input). Safety: the device must match by exact name or UID. There is NO fallback to
// the default output: if the device is missing, cannot be bound, or disappears, the tool exits with an
// error before (or instead of) producing sound.
//
// Output (stdout, one key=value per line, for scripts):
//   device=<name> uid=<uid> device_rate=<Hz> channels=<n> latency_frames=<n> safety_frames=<n>
//   start_host_time=<mach ticks> start_host_seconds=<s>   timestamp of the first rendered sample
//                                                          (the AUHAL render time stamp, i.e. when that
//                                                          sample is scheduled at the device; add the
//                                                          device latency for the analog/loopback edge)
//   done frames=<n>
// The whole file is decoded to memory first, so playback starts sample-accurately with the first render
// callback and runs without disk I/O. SIGINT/SIGTERM stop playback and exit 130.

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>

#include <atomic>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

std::string cfToString(CFStringRef s) {
    if (!s) return {};
    char buf[512];
    if (CFStringGetCString(s, buf, sizeof buf, kCFStringEncodingUTF8)) return buf;
    return {};
}

std::string stringProp(AudioObjectID obj, AudioObjectPropertySelector sel) {
    AudioObjectPropertyAddress a{sel, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    CFStringRef s = nullptr;
    UInt32 size = sizeof s;
    if (AudioObjectGetPropertyData(obj, &a, 0, nullptr, &size, &s) != noErr || !s) return {};
    std::string r = cfToString(s);
    CFRelease(s);
    return r;
}

int outputChannels(AudioObjectID dev) {
    AudioObjectPropertyAddress a{kAudioDevicePropertyStreamConfiguration, kAudioObjectPropertyScopeOutput,
                                 kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(dev, &a, 0, nullptr, &size) != noErr || size == 0) return 0;
    std::vector<unsigned char> buf(size);
    auto* list = reinterpret_cast<AudioBufferList*>(buf.data());
    if (AudioObjectGetPropertyData(dev, &a, 0, nullptr, &size, list) != noErr) return 0;
    int n = 0;
    for (UInt32 i = 0; i < list->mNumberBuffers; ++i) n += static_cast<int>(list->mBuffers[i].mNumberChannels);
    return n;
}

UInt32 u32Prop(AudioObjectID dev, AudioObjectPropertySelector sel, AudioObjectPropertyScope scope) {
    AudioObjectPropertyAddress a{sel, scope, kAudioObjectPropertyElementMain};
    UInt32 v = 0, size = sizeof v;
    if (AudioObjectGetPropertyData(dev, &a, 0, nullptr, &size, &v) != noErr) return 0;
    return v;
}

double nominalRate(AudioObjectID dev) {
    AudioObjectPropertyAddress a{kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal,
                                 kAudioObjectPropertyElementMain};
    Float64 r = 0;
    UInt32 size = sizeof r;
    AudioObjectGetPropertyData(dev, &a, 0, nullptr, &size, &r);
    return r;
}

bool deviceAlive(AudioObjectID dev) {
    return u32Prop(dev, kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal) != 0;
}

struct OutDev {
    AudioObjectID id = 0;
    std::string name, uid;
    int channels = 0;
    double rate = 0;
};

std::vector<OutDev> listOutputs() {
    AudioObjectPropertyAddress a{kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal,
                                 kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    std::vector<OutDev> out;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, nullptr, &size) != noErr) return out;
    std::vector<AudioObjectID> ids(size / sizeof(AudioObjectID));
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, nullptr, &size, ids.data()) != noErr) return out;
    for (AudioObjectID id : ids) {
        const int ch = outputChannels(id);
        if (ch <= 0) continue;
        out.push_back({id, stringProp(id, kAudioObjectPropertyName), stringProp(id, kAudioDevicePropertyDeviceUID), ch,
                       nominalRate(id)});
    }
    return out;
}

// Exact name or exact UID. Deliberately no case-folding, prefix or "default" matching.
bool findOutput(const std::string& want, OutDev& found) {
    for (const auto& d : listOutputs())
        if (d.name == want || d.uid == want) { found = d; return true; }
    return false;
}

struct Player {
    std::vector<float> mono;  // whole file, mono mix, at fileRate
    std::atomic<size_t> pos{0};
    std::atomic<uint64_t> startHost{0};
    std::atomic<bool> finished{false};
    float gain = 1.f;
};

OSStatus render(void* ref, AudioUnitRenderActionFlags* flags, const AudioTimeStamp* ts, UInt32, UInt32 frames,
                AudioBufferList* io) {
    auto* p = static_cast<Player*>(ref);
    size_t pos = p->pos.load(std::memory_order_relaxed);
    if (pos == 0 && p->startHost.load(std::memory_order_relaxed) == 0 && (ts->mFlags & kAudioTimeStampHostTimeValid))
        p->startHost.store(ts->mHostTime ? ts->mHostTime : 1, std::memory_order_relaxed);
    const size_t n = p->mono.size();
    for (UInt32 b = 0; b < io->mNumberBuffers; ++b) {
        auto* dst = static_cast<float*>(io->mBuffers[b].mData);
        for (UInt32 i = 0; i < frames; ++i) dst[i] = (pos + i < n) ? p->mono[pos + i] * p->gain : 0.f;
    }
    pos += frames;
    p->pos.store(pos, std::memory_order_relaxed);
    if (pos >= n) p->finished.store(true, std::memory_order_relaxed);
    (void)flags;
    return noErr;
}

bool loadFile(const char* path, std::vector<float>& mono, double& rate) {
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, reinterpret_cast<const UInt8*>(path),
                                                           static_cast<CFIndex>(std::strlen(path)), false);
    ExtAudioFileRef f = nullptr;
    OSStatus st = ExtAudioFileOpenURL(url, &f);
    CFRelease(url);
    if (st != noErr) { std::fprintf(stderr, "error: cannot open %s (%d)\n", path, int(st)); return false; }
    AudioStreamBasicDescription in{};
    UInt32 size = sizeof in;
    ExtAudioFileGetProperty(f, kExtAudioFileProperty_FileDataFormat, &size, &in);
    rate = in.mSampleRate;
    const UInt32 ch = in.mChannelsPerFrame;
    AudioStreamBasicDescription cl{};
    cl.mSampleRate = rate;
    cl.mFormatID = kAudioFormatLinearPCM;
    cl.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    cl.mChannelsPerFrame = ch;
    cl.mBitsPerChannel = 32;
    cl.mBytesPerFrame = cl.mBytesPerPacket = 4 * ch;
    cl.mFramesPerPacket = 1;
    if (ExtAudioFileSetProperty(f, kExtAudioFileProperty_ClientDataFormat, sizeof cl, &cl) != noErr) {
        ExtAudioFileDispose(f);
        std::fprintf(stderr, "error: unsupported format\n");
        return false;
    }
    std::vector<float> buf(4096 * ch);
    for (;;) {
        AudioBufferList bl;
        bl.mNumberBuffers = 1;
        bl.mBuffers[0] = {ch, static_cast<UInt32>(buf.size() * 4), buf.data()};
        UInt32 frames = 4096;
        if (ExtAudioFileRead(f, &frames, &bl) != noErr) { ExtAudioFileDispose(f); return false; }
        if (frames == 0) break;
        for (UInt32 i = 0; i < frames; ++i) {
            float s = 0;
            for (UInt32 c = 0; c < ch; ++c) s += buf[i * ch + c];
            mono.push_back(s / static_cast<float>(ch));
        }
    }
    ExtAudioFileDispose(f);
    return true;
}

// ---- --route-test: capture the input side of the same device -------------------------------------------
struct Capture {
    AudioUnit unit = nullptr;
    std::vector<float> data;      // preallocated, mono (channel 0)
    std::vector<float> scratch;   // per-callback render buffer
    std::atomic<size_t> n{0};
    std::atomic<uint64_t> firstHost{0};
    std::atomic<uint64_t> firstSampleTime{0};
    double rate = 0;
};

OSStatus captureProc(void* ref, AudioUnitRenderActionFlags* flags, const AudioTimeStamp* ts, UInt32 bus, UInt32 frames,
                     AudioBufferList*) {
    auto* c = static_cast<Capture*>(ref);
    if (frames > c->scratch.size() / 2) return noErr;
    struct { AudioBufferList l; AudioBuffer extra; } abl{};
    abl.l.mNumberBuffers = 2;
    abl.l.mBuffers[0] = {1, frames * 4, c->scratch.data()};
    abl.l.mBuffers[1] = {1, frames * 4, c->scratch.data() + frames};
    if (AudioUnitRender(c->unit, flags, ts, bus, frames, &abl.l) != noErr) return noErr;
    size_t n = c->n.load(std::memory_order_relaxed);
    if (n == 0 && c->firstHost.load(std::memory_order_relaxed) == 0) c->firstHost.store(ts->mHostTime);
    for (UInt32 i = 0; i < frames && n < c->data.size(); ++i) c->data[n++] = c->scratch[i];
    c->n.store(n, std::memory_order_relaxed);
    return noErr;
}

bool startCapture(Capture& c, AudioObjectID dev, double seconds) {
    AudioComponentDescription cd{kAudioUnitType_Output, kAudioUnitSubType_HALOutput, kAudioUnitManufacturer_Apple, 0, 0};
    AudioComponent comp = AudioComponentFindNext(nullptr, &cd);
    if (!comp || AudioComponentInstanceNew(comp, &c.unit) != noErr) return false;
    UInt32 one = 1, zero = 0;
    if (AudioUnitSetProperty(c.unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1, &one, 4) != noErr ||
        AudioUnitSetProperty(c.unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0, &zero, 4) != noErr ||
        AudioUnitSetProperty(c.unit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &dev,
                             sizeof dev) != noErr)
        return false;
    AudioStreamBasicDescription hw{};
    UInt32 sz = sizeof hw;
    AudioUnitGetProperty(c.unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 1, &hw, &sz);
    c.rate = hw.mSampleRate;
    AudioStreamBasicDescription f{};
    f.mSampleRate = hw.mSampleRate;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagIsNonInterleaved;
    f.mChannelsPerFrame = 2;
    f.mBitsPerChannel = 32;
    f.mBytesPerFrame = f.mBytesPerPacket = 4;
    f.mFramesPerPacket = 1;
    AURenderCallbackStruct cb{captureProc, &c};
    if (AudioUnitSetProperty(c.unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 1, &f, sizeof f) != noErr ||
        AudioUnitSetProperty(c.unit, kAudioOutputUnitProperty_SetInputCallback, kAudioUnitScope_Global, 0, &cb,
                             sizeof cb) != noErr)
        return false;
    c.data.assign(static_cast<size_t>(seconds * c.rate), 0.f);
    c.scratch.assign(2 * 8192, 0.f);
    return AudioUnitInitialize(c.unit) == noErr && AudioOutputUnitStart(c.unit) == noErr;
}

void stopCapture(Capture& c) {
    if (!c.unit) return;
    AudioOutputUnitStop(c.unit);
    AudioUnitUninitialize(c.unit);
    AudioComponentInstanceDispose(c.unit);
    c.unit = nullptr;
}

// Centre (in samples, fractional) of the loudest 2 kHz-ish burst in [0, limit): envelope-weighted centroid
// of |x| within +-40 ms of the peak.
double burstCentre(const std::vector<float>& x, size_t count, double rate, double limitSeconds, float* peakOut) {
    const size_t lim = std::min(count, static_cast<size_t>(limitSeconds * rate));
    size_t pk = 0;
    float pv = 0;
    for (size_t i = 0; i < lim; ++i) if (std::fabs(x[i]) > pv) { pv = std::fabs(x[i]); pk = i; }
    if (peakOut) *peakOut = pv;
    if (pv <= 0) return -1;
    const size_t w = static_cast<size_t>(0.04 * rate);
    double sw = 0, sx = 0;
    for (size_t i = pk > w ? pk - w : 0; i < std::min(lim, pk + w); ++i) {
        const double e = double(x[i]) * x[i];
        sw += e;
        sx += e * double(i);
    }
    return sx / sw;
}

std::atomic<bool> gStop{false};
void onSignal(int) { gStop.store(true); }

double hostSeconds(uint64_t t) {
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    return static_cast<double>(t) * tb.numer / tb.denom * 1e-9;
}

void usage() {
    std::fprintf(stderr,
                 "usage: reforce-audio-play --list-outputs\n"
                 "       reforce-audio-play --device NAME|UID --check\n"
                 "       reforce-audio-play --device NAME|UID [--gain DB] [--route-test] FILE\n"
                 "The device must match exactly (name or UID); there is never a fallback to the default output.\n");
}

}  // namespace

int main(int argc, char** argv) {
    std::string device, file;
    bool list = false, check = false, routeTest = false;
    double gainDb = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--list-outputs") list = true;
        else if (a == "--check") check = true;
        else if (a == "--route-test") routeTest = true;
        else if (a == "--device" && i + 1 < argc) device = argv[++i];
        else if (a == "--gain" && i + 1 < argc) gainDb = std::atof(argv[++i]);
        else if (!a.empty() && a[0] != '-' && file.empty()) file = a;
        else { usage(); return 2; }
    }
    if (list) {
        for (const auto& d : listOutputs())
            std::printf("%-36s ch=%-2d %6.0f Hz  uid=%s\n", d.name.c_str(), d.channels, d.rate, d.uid.c_str());
        return 0;
    }
    if (device.empty()) { usage(); return 2; }
    OutDev dev;
    if (!findOutput(device, dev)) {
        std::fprintf(stderr, "error: output device \"%s\" not found (exact name or UID required; nothing played). "
                             "Use --list-outputs.\n", device.c_str());
        return 1;
    }
    if (check) {
        std::printf("device=%s uid=%s device_rate=%.0f channels=%d\n", dev.name.c_str(), dev.uid.c_str(), dev.rate,
                    dev.channels);
        return 0;
    }
    if (file.empty()) { usage(); return 2; }

    Player p;
    double fileRate = 0;
    if (!loadFile(file.c_str(), p.mono, fileRate)) return 1;
    p.gain = static_cast<float>(std::pow(10.0, gainDb / 20.0));

    AudioComponentDescription cd{kAudioUnitType_Output, kAudioUnitSubType_HALOutput, kAudioUnitManufacturer_Apple, 0, 0};
    AudioComponent comp = AudioComponentFindNext(nullptr, &cd);
    AudioUnit au = nullptr;
    if (!comp || AudioComponentInstanceNew(comp, &au) != noErr) { std::fprintf(stderr, "error: no AUHAL\n"); return 1; }
    auto fail = [&](const char* what, OSStatus st) {
        std::fprintf(stderr, "error: %s (%d); nothing played\n", what, int(st));
        AudioComponentInstanceDispose(au);
        return 1;
    };
    // Bind the device BEFORE initializing: an uninitialized, unbound AUHAL would use the default output.
    AudioObjectID id = dev.id;
    if (OSStatus st = AudioUnitSetProperty(au, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &id,
                                           sizeof id); st != noErr)
        return fail("cannot bind output device", st);
    AudioObjectID bound = 0;
    UInt32 sz = sizeof bound;
    if (AudioUnitGetProperty(au, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &bound, &sz) != noErr ||
        bound != dev.id)
        return fail("device binding did not stick", -1);

    AudioStreamBasicDescription fmt{};
    fmt.mSampleRate = fileRate;  // AUHAL converts to the device rate if needed
    fmt.mFormatID = kAudioFormatLinearPCM;
    fmt.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagIsNonInterleaved;
    fmt.mChannelsPerFrame = static_cast<UInt32>(dev.channels);  // mono copied to every device channel
    fmt.mBitsPerChannel = 32;
    fmt.mBytesPerFrame = fmt.mBytesPerPacket = 4;
    fmt.mFramesPerPacket = 1;
    if (OSStatus st = AudioUnitSetProperty(au, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &fmt, sizeof fmt);
        st != noErr)
        return fail("cannot set stream format", st);
    AURenderCallbackStruct cb{render, &p};
    if (OSStatus st = AudioUnitSetProperty(au, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb,
                                           sizeof cb); st != noErr)
        return fail("cannot set render callback", st);
    if (OSStatus st = AudioUnitInitialize(au); st != noErr) return fail("AudioUnitInitialize", st);
    // Re-check after initialize (paranoia: never play anywhere else).
    sz = sizeof bound;
    if (AudioUnitGetProperty(au, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &bound, &sz) != noErr ||
        bound != dev.id || !deviceAlive(dev.id)) {
        AudioUnitUninitialize(au);
        return fail("device not bound/alive after initialize", -1);
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    const UInt32 lat = u32Prop(dev.id, kAudioDevicePropertyLatency, kAudioObjectPropertyScopeOutput);
    const UInt32 saf = u32Prop(dev.id, kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeOutput);
    std::printf("device=%s uid=%s device_rate=%.0f channels=%d latency_frames=%u safety_frames=%u\n", dev.name.c_str(),
                dev.uid.c_str(), nominalRate(dev.id), dev.channels, lat, saf);
    std::printf("file=%s file_rate=%.0f frames=%zu seconds=%.3f\n", file.c_str(), fileRate, p.mono.size(),
                p.mono.size() / fileRate);
    std::fflush(stdout);
    Capture cap;
    if (routeTest) {
        // The same device's input side (a Loopback device has both).
        AudioObjectPropertyAddress a{kAudioDevicePropertyStreamConfiguration, kAudioObjectPropertyScopeInput,
                                     kAudioObjectPropertyElementMain};
        UInt32 isz = 0;
        if (AudioObjectGetPropertyDataSize(dev.id, &a, 0, nullptr, &isz) != noErr || isz == 0 ||
            !startCapture(cap, dev.id, p.mono.size() / fileRate + 2.0)) {
            stopCapture(cap);
            AudioUnitUninitialize(au);
            return fail("--route-test: cannot capture the device's input side", -1);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));  // capture running before playback starts
    }
    if (OSStatus st = AudioOutputUnitStart(au); st != noErr) {
        AudioUnitUninitialize(au);
        return fail("AudioOutputUnitStart", st);
    }
    bool printed = false, died = false;
    while (!gStop.load() && !p.finished.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        const uint64_t h = p.startHost.load();
        if (!printed && h) {
            std::printf("start_host_time=%llu start_host_seconds=%.6f\n", static_cast<unsigned long long>(h),
                        hostSeconds(h));
            std::fflush(stdout);
            printed = true;
        }
        if (!deviceAlive(dev.id)) { died = true; break; }
    }
    // Let the last buffer drain (device latency + one IO cycle).
    if (!gStop.load() && !died) std::this_thread::sleep_for(std::chrono::milliseconds(150));
    AudioOutputUnitStop(au);
    AudioUnitUninitialize(au);
    AudioComponentInstanceDispose(au);
    if (died) { std::fprintf(stderr, "error: device disappeared during playback\n"); stopCapture(cap); return 1; }
    if (routeTest) {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        stopCapture(cap);
        const size_t n = cap.n.load();
        float srcPeak = 0, capPeak = 0;
        const double srcC = burstCentre(p.mono, p.mono.size(), fileRate, 1.5, &srcPeak);
        const double capOff = (hostSeconds(p.startHost.load()) - hostSeconds(cap.firstHost.load())) * cap.rate;
        const double capC = burstCentre(cap.data, n, cap.rate, std::max(0.0, capOff / cap.rate) + 1.5, &capPeak);
        if (capPeak < 1e-4 || !p.startHost.load() || !cap.firstHost.load()) {
            std::printf("route_test=FAIL captured_peak=%.6f (nothing arrived on the input side)\n", capPeak);
            return 1;
        }
        const double tClickPlay = hostSeconds(p.startHost.load()) + srcC / fileRate;
        const double tClickCap = hostSeconds(cap.firstHost.load()) + capC / cap.rate;
        const double lat = (tClickCap - tClickPlay) * 1e3;
        // Whole-file residual after aligning by the click (same rate only).
        double res = 0, ref = 0;
        const long shift = std::lround(capC - srcC);
        if (cap.rate == fileRate)
            for (size_t i = 0; i < p.mono.size(); ++i) {
                const long j = static_cast<long>(i) + shift;
                if (j < 0 || static_cast<size_t>(j) >= n) continue;
                const double d = cap.data[j] - p.mono[i] * p.gain;
                res += d * d;
                ref += double(p.mono[i]) * p.mono[i];
            }
        std::printf("route_test=OK captured_samples=%zu capture_rate=%.0f click_src_s=%.6f click_cap_s=%.6f "
                    "route_latency_ms=%.3f gain_db=%.2f residual_db=%.1f\n",
                    n, cap.rate, srcC / fileRate, capC / cap.rate, lat, 20 * std::log10(capPeak / srcPeak),
                    ref > 0 ? 10 * std::log10(std::max(res, 1e-30) / ref) : 0.0);
    }
    std::printf("done frames=%zu%s\n", std::min(p.pos.load(), p.mono.size()), gStop.load() ? " (interrupted)" : "");
    return gStop.load() ? 130 : 0;
}
