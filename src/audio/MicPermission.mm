#include "audio/MicPermission.hpp"

#import <AVFoundation/AVFoundation.h>

namespace reforce::audio {

const char* micPermissionName(MicPermission p) {
    switch (p) {
        case MicPermission::NotDetermined: return "not-determined";
        case MicPermission::Restricted: return "restricted";
        case MicPermission::Denied: return "denied";
        case MicPermission::Authorized: return "authorized";
        case MicPermission::Unknown: return "unknown";
    }
    return "?";
}

MicPermission microphonePermission() {
    switch ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio]) {
        case AVAuthorizationStatusNotDetermined: return MicPermission::NotDetermined;
        case AVAuthorizationStatusRestricted: return MicPermission::Restricted;
        case AVAuthorizationStatusDenied: return MicPermission::Denied;
        case AVAuthorizationStatusAuthorized: return MicPermission::Authorized;
    }
    return MicPermission::Unknown;
}

void requestMicrophonePermission(std::function<void(bool granted)> done) {
    auto cb = std::make_shared<std::function<void(bool)>>(std::move(done));
    [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio
                             completionHandler:^(BOOL granted) {
                               if (*cb) (*cb)(granted == YES);
                             }];
}

}  // namespace reforce::audio
