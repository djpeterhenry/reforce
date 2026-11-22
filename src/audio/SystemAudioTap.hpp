#pragma once
// "System Audio": everything the Mac plays, captured with a Core Audio process
// tap (macOS 14.2+), with no virtual audio driver (Loopback, BlackHole) needed.
//
// How: a global stereo tap of all processes except reforce itself, wrapped in a
// private aggregate device. (reforce plays no sound today; leaving itself out
// only guards against feedback should it ever do so.) The aggregate
// shows up as an ordinary input device to this process only, so CoreAudioInput
// reads it through the same AUHAL path as any other input.
//
// The tap does not change what is heard (unmuted). Its audio is what apps play
// before it reaches an output device, so it does not depend on which device
// that is.
//
// Permission: macOS asks once for "System Audio Recording Only" (TCC, needs
// NSAudioCaptureUsageDescription in Info.plist). It is not the microphone
// permission. Without it the tap delivers silence. For a command-line run, the
// terminal (the responsible process) is asked instead.

#include <cstdint>
#include <string>

namespace reforce::audio {

// Audio menu id / Audio.InputSource / --audio-device value for this source
// (like "default" and "none").
inline constexpr const char* kSystemAudioId = "system";
inline constexpr const char* kSystemAudioName = "Everything the Mac Plays";  // Audio menu title

// The OS has process taps (macOS 14.2 or later).
bool systemAudioSupported();

// True for the private aggregate devices this creates (kept out of the input list).
bool isSystemAudioAggregateUid(const std::string& uid);

class SystemAudioTap {
public:
    SystemAudioTap() = default;
    ~SystemAudioTap() { destroy(); }
    SystemAudioTap(const SystemAudioTap&) = delete;
    SystemAudioTap& operator=(const SystemAudioTap&) = delete;

    // Creates the tap and the aggregate device. On failure returns false with
    // `error` set and nothing left behind.
    bool create(std::string& error);
    void destroy();

    uint32_t aggregateDevice() const { return aggregate_; }  // AudioObjectID, 0 if none
    const std::string& aggregateUid() const { return aggregateUid_; }

private:
    uint32_t tap_ = 0;
    uint32_t aggregate_ = 0;
    std::string aggregateUid_;
};

}  // namespace reforce::audio
