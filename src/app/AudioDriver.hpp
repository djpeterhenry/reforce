#pragma once
// Audio for the app: one source (live CoreAudio device, or a file) feeding the
// Analyzer, configured from the G-Force prefs.
//
//  * live device: CoreAudioInput (prefs Audio.InputSource, else system default;
//    "system" = System Audio, see audio/SystemAudioTap.hpp)
//  * file, realtime: AudioFileSource paced by the wall clock, NOT played out loud
//  * file, deterministic: exactly sampleRate / fps samples per frame
//  * none: silence (the analyzer decays to zero)
//
// Threads (Phase 5D): once startControl() runs (windowed app), live-device
// opens/closes (requestDevice, any thread) and the device's housekeeping
// (service(): reconnects, format restarts) happen on a control thread; the
// render thread's update() only reads the current source (swapped in with an
// atomic pointer) and never waits for CoreAudio. Without startControl()
// (offline, tests) everything runs on the caller as before.

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "audio/Analyzer.hpp"
#include "engine/Prefs.hpp"

namespace reforce::audio { class AudioSource; class AudioFileSource; class CoreAudioInput; }

namespace reforce::app {

// AnalyzerConfig from the G-Force prefs (bins, FFT params, smoothing, range).
audio::AnalyzerConfig analyzerConfigFromPrefs(const engine::Settings& prefs);

class AudioDriver {
public:
    explicit AudioDriver(const audio::AnalyzerConfig& cfg);
    ~AudioDriver();

    // Live device by name/UID; "" = system default. Falls back to the system
    // default if a named device is missing. Synchronous (startup).
    bool openDevice(const std::string& device);
    // Asynchronous open ("" = system default) / close on the control thread
    // (synchronous when it is not running). Any thread.
    void requestDevice(const std::string& device);
    void requestClose();
    // Starts the control thread (see above).
    void startControl();
    bool openFile(const std::string& path, bool deterministic, bool loop);
    // Stops a live device (silence from here on).
    void closeDevice();

    // Live device state for the Audio menu.
    bool hasDevice() const;
    bool hasFile() const { return file_ != nullptr; }
    bool followsDefault() const;
    bool isSystemAudio() const;  // the live input is System Audio
    std::string deviceName() const;  // "" if none
    std::string deviceUid() const;

    // Live/realtime: drain the ring and analyze `dt` seconds.
    const audio::AudioFrame& update(double dt);
    // Deterministic: decode exactly the samples of one frame at `fps`.
    const audio::AudioFrame& step(double fps);

    std::string description() const;
    // Render thread: the description without waiting for the control thread
    // (the last one seen when it is busy).
    std::string cachedDescription() const;
    bool fileFinished() const;

private:
    std::unique_ptr<audio::CoreAudioInput> openInput(const std::string& device, std::string& requested);
    void closeDeviceLocked();
    void setSource(audio::AudioSource* s);
    void controlLoop();

    audio::Analyzer analyzer_;
    mutable std::mutex mutex_;  // device_, requested_ (control/main threads)
    std::unique_ptr<audio::CoreAudioInput> device_;
    std::unique_ptr<audio::AudioFileSource> file_;
    std::atomic<audio::AudioSource*> source_{nullptr};  // what update() reads
    std::atomic<bool> inUpdate_{false};                 // update() is using source_
    std::string requested_;  // device asked for ("" = system default, also after a fallback)
    bool deterministic_ = false;
    double carry_ = 0;

    std::thread control_;
    std::condition_variable cv_;
    bool stop_ = false;
    std::optional<std::optional<std::string>> request_;  // pending: device (nullopt = close)
    mutable std::mutex descMutex_;
    mutable std::string desc_ = "silence";
};

}  // namespace reforce::app
