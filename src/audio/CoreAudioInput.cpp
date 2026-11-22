#include "audio/CoreAudioInput.hpp"

#include <Accelerate/Accelerate.h>
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace reforce::audio {

const char* sourceStateName(SourceState s) {
    switch (s) {
        case SourceState::Stopped: return "stopped";
        case SourceState::Running: return "running";
        case SourceState::Reconfiguring: return "reconfiguring";
        case SourceState::Disconnected: return "disconnected";
        case SourceState::Finished: return "finished";
        case SourceState::Error: return "error";
    }
    return "?";
}

namespace {

constexpr UInt32 kMaxFrames = 8192;

std::string cfToString(CFStringRef s) {
    if (!s) return {};
    const CFIndex len = CFStringGetLength(s);
    const CFIndex max = CFStringGetMaximumSizeForEncoding(len, kCFStringEncodingUTF8) + 1;
    std::string out(static_cast<size_t>(max), '\0');
    if (!CFStringGetCString(s, out.data(), max, kCFStringEncodingUTF8)) return {};
    out.resize(std::strlen(out.c_str()));
    return out;
}

std::string getStringProp(AudioObjectID obj, AudioObjectPropertySelector sel) {
    AudioObjectPropertyAddress addr{sel, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    CFStringRef s = nullptr;
    UInt32 size = sizeof(s);
    if (AudioObjectGetPropertyData(obj, &addr, 0, nullptr, &size, &s) != noErr || !s) return {};
    std::string out = cfToString(s);
    CFRelease(s);
    return out;
}

int inputChannelCount(AudioObjectID dev) {
    AudioObjectPropertyAddress addr{kAudioDevicePropertyStreamConfiguration, kAudioObjectPropertyScopeInput,
                                    kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(dev, &addr, 0, nullptr, &size) != noErr || size == 0) return 0;
    std::vector<uint8_t> buf(size);
    auto* abl = reinterpret_cast<AudioBufferList*>(buf.data());
    if (AudioObjectGetPropertyData(dev, &addr, 0, nullptr, &size, abl) != noErr) return 0;
    int ch = 0;
    for (UInt32 i = 0; i < abl->mNumberBuffers; ++i) ch += static_cast<int>(abl->mBuffers[i].mNumberChannels);
    return ch;
}

double nominalRate(AudioObjectID dev) {
    AudioObjectPropertyAddress addr{kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    Float64 sr = 0;
    UInt32 size = sizeof(sr);
    AudioObjectGetPropertyData(dev, &addr, 0, nullptr, &size, &sr);
    return sr;
}

AudioObjectID defaultInputDevice() {
    AudioObjectPropertyAddress addr{kAudioHardwarePropertyDefaultInputDevice, kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    AudioObjectID id = kAudioObjectUnknown;
    UInt32 size = sizeof(id);
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr, &size, &id);
    return id;
}

bool deviceAlive(AudioObjectID dev) {
    AudioObjectPropertyAddress addr{kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    UInt32 alive = 0;
    UInt32 size = sizeof(alive);
    if (AudioObjectGetPropertyData(dev, &addr, 0, nullptr, &size, &alive) != noErr) return false;
    return alive != 0;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string osStatusString(OSStatus st) {
    char buf[64];
    const UInt32 be = CFSwapInt32HostToBig(static_cast<UInt32>(st));
    char fourcc[5];
    std::memcpy(fourcc, &be, 4);
    fourcc[4] = 0;
    bool printable = true;
    for (int i = 0; i < 4; ++i) printable &= std::isprint(static_cast<unsigned char>(fourcc[i])) != 0;
    if (printable) std::snprintf(buf, sizeof buf, "OSStatus %d '%s'", static_cast<int>(st), fourcc);
    else std::snprintf(buf, sizeof buf, "OSStatus %d", static_cast<int>(st));
    return buf;
}

}  // namespace

std::vector<InputDeviceInfo> listInputDevices() {
    std::vector<InputDeviceInfo> out;
    AudioObjectPropertyAddress addr{kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &addr, 0, nullptr, &size) != noErr) return out;
    std::vector<AudioObjectID> ids(size / sizeof(AudioObjectID));
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr, &size, ids.data()) != noErr)
        return out;
    const AudioObjectID def = defaultInputDevice();
    for (AudioObjectID id : ids) {
        const int ch = inputChannelCount(id);
        if (ch <= 0) continue;
        InputDeviceInfo d;
        d.id = id;
        d.name = getStringProp(id, kAudioObjectPropertyName);
        d.uid = getStringProp(id, kAudioDevicePropertyDeviceUID);
        d.inputChannels = ch;
        d.sampleRate = nominalRate(id);
        d.isDefaultInput = (id == def);
        out.push_back(std::move(d));
    }
    return out;
}

// ---------------------------------------------------------------------------
// IO-thread context. Everything the callback touches is allocated up front.
struct CoreAudioInput::Rt {
    AudioUnit unit = nullptr;
    SpscRingBuffer* ring = nullptr;
    int numChannels = 0;
    std::vector<float> channelData;          // numChannels * kMaxFrames
    std::vector<uint8_t> ablStorage;         // AudioBufferList with numChannels buffers
    std::vector<int> selected;               // channels to average
    std::vector<float> mono;                 // kMaxFrames
    float mixScale = 1.f;
    std::atomic<uint64_t> callbacks{0};
    std::atomic<uint64_t> renderErrors{0};
    std::atomic<uint64_t> oversize{0};
    AudioObjectID device = kAudioObjectUnknown;
};

static OSStatus inputProc(void* refCon, AudioUnitRenderActionFlags* flags, const AudioTimeStamp* ts,
                          UInt32 bus, UInt32 frames, AudioBufferList*) {
    auto* rt = static_cast<CoreAudioInput::Rt*>(refCon);
    rt->callbacks.fetch_add(1, std::memory_order_relaxed);
    if (frames > kMaxFrames) {
        rt->oversize.fetch_add(1, std::memory_order_relaxed);
        return noErr;
    }
    auto* abl = reinterpret_cast<AudioBufferList*>(rt->ablStorage.data());
    for (int c = 0; c < rt->numChannels; ++c) {
        abl->mBuffers[c].mNumberChannels = 1;
        abl->mBuffers[c].mDataByteSize = frames * sizeof(float);
        abl->mBuffers[c].mData = rt->channelData.data() + static_cast<size_t>(c) * kMaxFrames;
    }
    const OSStatus st = AudioUnitRender(rt->unit, flags, ts, bus, frames, abl);
    if (st != noErr) {
        rt->renderErrors.fetch_add(1, std::memory_order_relaxed);
        return st;
    }
    float* mono = rt->mono.data();
    const float* first = rt->channelData.data() + static_cast<size_t>(rt->selected[0]) * kMaxFrames;
    if (rt->selected.size() == 1) {
        rt->ring->write(first, frames);
        return noErr;
    }
    std::memcpy(mono, first, frames * sizeof(float));
    for (size_t i = 1; i < rt->selected.size(); ++i)
        vDSP_vadd(mono, 1, rt->channelData.data() + static_cast<size_t>(rt->selected[i]) * kMaxFrames, 1, mono,
                  1, frames);
    vDSP_vsmul(mono, 1, &rt->mixScale, mono, 1, frames);
    rt->ring->write(mono, frames);
    return noErr;
}

static OSStatus deviceListener(AudioObjectID, UInt32 n, const AudioObjectPropertyAddress* addrs, void* refCon) {
    auto* self = static_cast<CoreAudioInput*>(refCon);
    for (UInt32 i = 0; i < n; ++i) {
        switch (addrs[i].mSelector) {
            case kAudioDevicePropertyDeviceIsAlive: self->flagDeviceDied_ = true; break;
            default: self->flagRestart_ = true; break;  // sample rate / stream configuration
        }
    }
    return noErr;
}

static OSStatus systemListener(AudioObjectID, UInt32 n, const AudioObjectPropertyAddress* addrs, void* refCon) {
    auto* self = static_cast<CoreAudioInput*>(refCon);
    for (UInt32 i = 0; i < n; ++i) {
        if (addrs[i].mSelector == kAudioHardwarePropertyDefaultInputDevice) self->flagRestart_ = true;
        self->flagDevicesChanged_ = true;
    }
    return noErr;
}

static const AudioObjectPropertyAddress kDeviceProps[] = {
    {kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain},
    {kAudioDevicePropertyStreamConfiguration, kAudioObjectPropertyScopeInput, kAudioObjectPropertyElementMain},
    {kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain},
};
static const AudioObjectPropertyAddress kSystemProps[] = {
    {kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain},
    {kAudioHardwarePropertyDefaultInputDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain},
};

CoreAudioInput::CoreAudioInput() = default;

CoreAudioInput::~CoreAudioInput() { close(); }

uint64_t CoreAudioInput::callbackCount() const { return rt_ ? rt_->callbacks.load() : 0; }
uint64_t CoreAudioInput::renderErrors() const { return rt_ ? rt_->renderErrors.load() : 0; }
uint64_t CoreAudioInput::oversizeCallbacks() const { return rt_ ? rt_->oversize.load() : 0; }

std::string CoreAudioInput::description() const {
    std::string s = "input \"" + dev_.name + "\" (" + dev_.uid + ") ";
    s += std::to_string(static_cast<int>(sampleRate())) + " Hz, " + sourceStateName(state());
    return s;
}

bool CoreAudioInput::open(const InputOptions& opts) {
    close();
    opts_ = opts;
    ring_ = std::make_unique<SpscRingBuffer>(opts.ringCapacity);
    opened_ = true;
    addSystemListeners();
    return start();
}

void CoreAudioInput::close() {
    if (!opened_) return;
    teardown();
    removeSystemListeners();
    opened_ = false;
    state_ = SourceState::Stopped;
}

void CoreAudioInput::addSystemListeners() {
    if (systemListeners_) return;
    for (const auto& a : kSystemProps) AudioObjectAddPropertyListener(kAudioObjectSystemObject, &a, systemListener, this);
    systemListeners_ = true;
}

void CoreAudioInput::removeSystemListeners() {
    if (!systemListeners_) return;
    for (const auto& a : kSystemProps)
        AudioObjectRemovePropertyListener(kAudioObjectSystemObject, &a, systemListener, this);
    systemListeners_ = false;
}

bool CoreAudioInput::start() {
    teardown();
    flagRestart_ = flagDeviceDied_ = flagDevicesChanged_ = false;

    // Resolve the device.
    const auto devices = listInputDevices();
    const InputDeviceInfo* found = nullptr;
    if (opts_.device.empty()) {
        for (const auto& d : devices) if (d.isDefaultInput) found = &d;
    } else {
        for (const auto& d : devices) if (d.uid == opts_.device) { found = &d; break; }
        if (!found) for (const auto& d : devices) if (d.name == opts_.device) { found = &d; break; }
        if (!found) {
            const std::string want = lower(opts_.device);
            for (const auto& d : devices) if (lower(d.name) == want) { found = &d; break; }
        }
    }
    if (!found) {
        lastError_ = opts_.device.empty() ? "no default input device"
                                          : "input device not found: " + opts_.device;
        state_ = SourceState::Disconnected;
        return false;
    }
    dev_ = *found;

    auto fail = [&](const char* what, OSStatus st) {
        lastError_ = std::string(what) + ": " + osStatusString(st);
        teardown();
        state_ = SourceState::Error;
        return false;
    };

    auto rt = std::make_unique<Rt>();
    rt->ring = ring_.get();
    rt->device = dev_.id;

    AudioComponentDescription desc{kAudioUnitType_Output, kAudioUnitSubType_HALOutput,
                                   kAudioUnitManufacturer_Apple, 0, 0};
    AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
    if (!comp) {
        lastError_ = "AUHAL component not found";
        state_ = SourceState::Error;
        return false;
    }
    OSStatus st = AudioComponentInstanceNew(comp, &rt->unit);
    if (st != noErr) return fail("AudioComponentInstanceNew", st);
    rt_ = std::move(rt);
    Rt& r = *rt_;

    UInt32 one = 1, zero = 0;
    if ((st = AudioUnitSetProperty(r.unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1, &one,
                                   sizeof one)) != noErr)
        return fail("EnableIO(input)", st);
    if ((st = AudioUnitSetProperty(r.unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0, &zero,
                                   sizeof zero)) != noErr)
        return fail("EnableIO(output)", st);
    AudioObjectID devId = dev_.id;
    if ((st = AudioUnitSetProperty(r.unit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0,
                                   &devId, sizeof devId)) != noErr)
        return fail("CurrentDevice", st);

    AudioStreamBasicDescription hw{};
    UInt32 size = sizeof hw;
    if ((st = AudioUnitGetProperty(r.unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 1, &hw,
                                   &size)) != noErr)
        return fail("StreamFormat(get)", st);
    const double rate = hw.mSampleRate > 0 ? hw.mSampleRate : nominalRate(dev_.id);
    const int nch = static_cast<int>(hw.mChannelsPerFrame > 0 ? hw.mChannelsPerFrame : dev_.inputChannels);
    if (nch <= 0 || rate <= 0) {
        lastError_ = "device reports no input format";
        teardown();
        state_ = SourceState::Error;
        return false;
    }

    AudioStreamBasicDescription cf{};
    cf.mSampleRate = rate;  // AUHAL does not resample on the input side
    cf.mFormatID = kAudioFormatLinearPCM;
    cf.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagIsNonInterleaved;
    cf.mBytesPerPacket = sizeof(float);
    cf.mFramesPerPacket = 1;
    cf.mBytesPerFrame = sizeof(float);
    cf.mChannelsPerFrame = static_cast<UInt32>(nch);
    cf.mBitsPerChannel = 32;
    if ((st = AudioUnitSetProperty(r.unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 1, &cf,
                                   sizeof cf)) != noErr)
        return fail("StreamFormat(set)", st);
    UInt32 maxFrames = kMaxFrames;
    AudioUnitSetProperty(r.unit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &maxFrames,
                         sizeof maxFrames);

    r.numChannels = nch;
    r.channelData.assign(static_cast<size_t>(nch) * kMaxFrames, 0.f);
    r.ablStorage.assign(offsetof(AudioBufferList, mBuffers) + sizeof(AudioBuffer) * static_cast<size_t>(nch), 0);
    reinterpret_cast<AudioBufferList*>(r.ablStorage.data())->mNumberBuffers = static_cast<UInt32>(nch);
    r.mono.assign(kMaxFrames, 0.f);
    for (int c : opts_.channels)
        if (c >= 0 && c < nch) r.selected.push_back(c);
    if (r.selected.empty())
        for (int c = 0; c < nch; ++c) r.selected.push_back(c);
    r.mixScale = 1.f / static_cast<float>(r.selected.size());

    AURenderCallbackStruct cb{inputProc, &r};
    if ((st = AudioUnitSetProperty(r.unit, kAudioOutputUnitProperty_SetInputCallback, kAudioUnitScope_Global, 0,
                                   &cb, sizeof cb)) != noErr)
        return fail("SetInputCallback", st);
    if ((st = AudioUnitInitialize(r.unit)) != noErr) return fail("AudioUnitInitialize", st);

    for (const auto& a : kDeviceProps) AudioObjectAddPropertyListener(dev_.id, &a, deviceListener, this);

    sampleRate_ = rate;
    if ((st = AudioOutputUnitStart(r.unit)) != noErr) return fail("AudioOutputUnitStart", st);
    lastError_.clear();
    state_ = SourceState::Running;
    return true;
}

void CoreAudioInput::teardown() {
    if (!rt_) return;
    if (rt_->unit) {
        AudioOutputUnitStop(rt_->unit);  // synchronous: no callback runs after this returns
        AudioUnitUninitialize(rt_->unit);
        AudioComponentInstanceDispose(rt_->unit);
        rt_->unit = nullptr;
    }
    if (rt_->device != kAudioObjectUnknown)
        for (const auto& a : kDeviceProps) AudioObjectRemovePropertyListener(rt_->device, &a, deviceListener, this);
    rt_.reset();
}

void CoreAudioInput::service() {
    if (!opened_) return;
    const SourceState s = state_.load();
    if (flagDeviceDied_.exchange(false) && s == SourceState::Running) {
        if (!deviceAlive(dev_.id)) {
            teardown();
            lastError_ = "device disconnected: " + dev_.name;
            state_ = SourceState::Disconnected;
            return;
        }
    }
    if (flagRestart_.exchange(false) && (s == SourceState::Running || s == SourceState::Reconfiguring)) {
        state_ = SourceState::Reconfiguring;
        start();
        return;
    }
    if (flagDevicesChanged_.exchange(false) &&
        (s == SourceState::Disconnected || s == SourceState::Error)) {
        start();
    }
}

}  // namespace reforce::audio
