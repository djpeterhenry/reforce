#pragma once
// Live input capture through an AUHAL (kAudioUnitSubType_HALOutput) input
// AudioUnit bound to a chosen CoreAudio device.
//
// Why AUHAL and not AVAudioEngine: AVAudioEngine's inputNode follows the
// system default input; selecting another device means reaching into its
// underlying AUHAL anyway, and it brings in Objective-C. AUHAL is a plain C API,
// lets us pick any device by ID, and gives us the IO callback directly.
//
// The IO callback renders into preallocated buffers, downmixes the selected
// channels to mono (vDSP) and writes the ring buffer: no allocation, locks,
// logging or ObjC on the audio thread.
//
// Device changes: property listeners (sample rate, stream configuration,
// device alive, device list, default input) only set atomic flags. service()
// (owner thread) acts on them: restarts on a format change, reports
// Disconnected when the device dies, and reopens it when a device with the
// same UID (or, for a name, the same name) appears again.
//
// System Audio (device "system", see SystemAudioTap.hpp): start() creates a
// process tap and its private aggregate device and reads that; teardown()
// removes both.
//
// Microphone permission (TCC): when the process is not authorized, CoreAudio
// still runs the callback but delivers zeros. See MicPermission.hpp.

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "audio/AudioSource.hpp"
#include "audio/SystemAudioTap.hpp"

namespace reforce::audio {

struct InputDeviceInfo {
    uint32_t id = 0;  // AudioObjectID (valid for this session only)
    std::string name;
    std::string uid;  // stable across reboots
    int inputChannels = 0;
    double sampleRate = 0.0;  // nominal
    bool isDefaultInput = false;
};

// Devices with at least one input channel (not System Audio's own aggregate).
std::vector<InputDeviceInfo> listInputDevices();

struct InputOptions {
    // Device UID, or exact name, or case-insensitive name. Empty = the system
    // default input (followed when the default changes). kSystemAudioId =
    // System Audio.
    std::string device;
    // 0-based device channels to average into mono. Empty = all channels.
    std::vector<int> channels;
    size_t ringCapacity = 1u << 17;  // ~2.7 s at 48 kHz
};

class CoreAudioInput final : public AudioSource {
public:
    CoreAudioInput();
    ~CoreAudioInput() override;
    CoreAudioInput(const CoreAudioInput&) = delete;
    CoreAudioInput& operator=(const CoreAudioInput&) = delete;

    // Resolve the device and start capture. Returns false if the device was not
    // found or failed to start (state() is then Disconnected or Error and
    // service() keeps retrying when the device list changes).
    bool open(const InputOptions& opts);
    void close();

    SpscRingBuffer& ring() override { return *ring_; }
    double sampleRate() const override { return sampleRate_.load(); }
    SourceState state() const override { return state_.load(); }
    std::string description() const override;
    void service() override;

    const std::string& lastError() const { return lastError_; }
    const InputDeviceInfo& device() const { return dev_; }

    // Diagnostics (atomic counters written by the IO thread).
    uint64_t callbackCount() const;
    uint64_t renderErrors() const;
    uint64_t oversizeCallbacks() const;  // callbacks larger than the preallocated buffers

    struct Rt;  // IO-thread context (public for the C callbacks)

private:
    bool start();     // resolve + build + start the unit
    void teardown();  // stop + dispose the unit, remove device listeners
    void addSystemListeners();
    void removeSystemListeners();

    InputOptions opts_;
    InputDeviceInfo dev_;
    std::unique_ptr<SpscRingBuffer> ring_;
    std::unique_ptr<Rt> rt_;
    std::unique_ptr<SystemAudioTap> tap_;  // System Audio only
    std::atomic<double> sampleRate_{0.0};
    std::atomic<SourceState> state_{SourceState::Stopped};
    std::string lastError_;
    bool systemListeners_ = false;
    bool opened_ = false;

public:
    // Flags set by CoreAudio listener callbacks (any thread).
    std::atomic<bool> flagRestart_{false};
    std::atomic<bool> flagDeviceDied_{false};
    std::atomic<bool> flagDevicesChanged_{false};
};

}  // namespace reforce::audio
