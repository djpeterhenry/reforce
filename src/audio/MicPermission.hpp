#pragma once
// Microphone (audio input) permission, i.e. macOS TCC "Microphone".
//
// Without authorization CoreAudio input still "works" but delivers silence.
// For a bundled app, TCC needs NSMicrophoneUsageDescription in Info.plist (we
// set it). For a command-line tool, TCC attributes the request to the
// responsible process (Terminal, VS Code, ...), so status() reports that
// process's permission.

#include <functional>

namespace reforce::audio {

enum class MicPermission { NotDetermined, Restricted, Denied, Authorized, Unknown };

const char* micPermissionName(MicPermission p);

MicPermission microphonePermission();

// Ask the user (shows the system prompt once). The callback runs on an
// arbitrary queue. Only call from an app bundle with NSMicrophoneUsageDescription.
void requestMicrophonePermission(std::function<void(bool granted)> done);

}  // namespace reforce::audio
