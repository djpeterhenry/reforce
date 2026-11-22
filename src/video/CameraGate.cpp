#include "video/CameraGate.hpp"

#include <utility>

namespace reforce::video {

const char* cameraAccessName(CameraAccess a) {
    switch (a) {
        case CameraAccess::NotDetermined: return "ask";
        case CameraAccess::Requesting: return "waiting";
        case CameraAccess::Granted: return "granted";
        case CameraAccess::Denied: return "denied";
        case CameraAccess::NoUsageKey: return "unavailable";
    }
    return "ask";
}

std::string cameraAccessMessage(CameraAccess a) {
    switch (a) {
        case CameraAccess::NotDetermined: return "camera access not asked yet (macOS asks when a camera is opened)";
        case CameraAccess::Requesting: return "waiting for camera permission (answer the macOS prompt)";
        case CameraAccess::Granted: return "";
        case CameraAccess::Denied:
            return "camera access denied: System Settings > Privacy & Security > Camera, turn on reforce";
        case CameraAccess::NoUsageKey:
            return "this reforce.app has no NSCameraUsageDescription (an old build): reinstall it "
                   "(scripts/install-app.sh)";
    }
    return "";
}

CameraGate::CameraGate(StatusFn status, RequestFn request) : status_(std::move(status)), request_(std::move(request)) {}

CameraAccess CameraGate::access() const {
    {
        std::lock_guard<std::mutex> lock(m_);
        if (requesting_) return CameraAccess::Requesting;
    }
    return status_();
}

int CameraGate::requests() const {
    std::lock_guard<std::mutex> lock(m_);
    return requests_;
}

void CameraGate::whenDecided(std::function<void(bool granted)> done) {
    if (!done) return;
    const CameraAccess s = status_();
    if (s == CameraAccess::Granted) return done(true);
    if (s == CameraAccess::Denied || s == CameraAccess::NoUsageKey) return done(false);
    {
        std::lock_guard<std::mutex> lock(m_);
        waiters_.push_back(std::move(done));
        if (requesting_) return;  // one prompt for everyone
        requesting_ = true;
        ++requests_;
    }
    // Outside the lock: a request may answer synchronously (mocks, or a
    // status that changed in between).
    request_([this](bool granted) {
        std::vector<std::function<void(bool)>> waiters;
        {
            std::lock_guard<std::mutex> lock(m_);
            requesting_ = false;
            waiters.swap(waiters_);
        }
        for (auto& w : waiters) w(granted);
    });
}

}  // namespace reforce::video
