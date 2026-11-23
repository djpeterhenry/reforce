#pragma once
// Camera permission (macOS TCC "Camera") as a small, testable state machine.
//
// The system calls are injected: `status` reads the current authorization
// (AVCaptureDevice authorizationStatusForMediaType, fast, any thread) and
// `request` shows the system prompt (requestAccessForMediaType: returns at
// once and calls back later on an arbitrary queue, possibly never if the user
// ignores the prompt). The gate coalesces concurrent requests (a camera on
// the color layer and as a sprite ask once) and never blocks its caller, so
// opening a camera source on the render thread can not hitch or hang.
// systemCameraGate() (VideoSourceMac.mm) is the AVFoundation-backed instance.

#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace reforce::video {

enum class CameraAccess {
    NotDetermined,  // never asked: the prompt appears the first time a camera is opened
    Requesting,     // the prompt is up, waiting for the user's answer
    Granted,
    Denied,         // denied or restricted (System Settings > Privacy & Security > Camera)
    NoUsageKey,     // this app bundle has no NSCameraUsageDescription (asking would get it killed)
};
// "ask", "waiting", "granted", "denied", "unavailable" (web API values).
const char* cameraAccessName(CameraAccess a);
// One-line user-facing text for a state that blocks a camera ("" when granted).
std::string cameraAccessMessage(CameraAccess a);

class CameraGate {
public:
    using StatusFn = std::function<CameraAccess()>;  // NotDetermined / Granted / Denied / NoUsageKey
    using RequestFn = std::function<void(std::function<void(bool granted)>)>;
    CameraGate(StatusFn status, RequestFn request);

    // Current state (Requesting while our prompt is out).
    CameraAccess access() const;
    // Calls done(granted) exactly once: right away when the answer is known,
    // otherwise when the (single, shared) system request answers. Never blocks;
    // `done` may run on any thread.
    void whenDecided(std::function<void(bool granted)> done);
    // Number of system requests made (tests).
    int requests() const;

private:
    StatusFn status_;
    RequestFn request_;
    mutable std::mutex m_;
    bool requesting_ = false;
    int requests_ = 0;
    std::vector<std::function<void(bool)>> waiters_;
};

}  // namespace reforce::video
