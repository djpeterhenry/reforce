#include "app/AudioDriver.hpp"

#include <pthread.h>

#include <chrono>
#include <cstdio>
#include <vector>

#include "audio/AudioFileSource.hpp"
#include "audio/CoreAudioInput.hpp"

namespace reforce::app {

audio::AnalyzerConfig analyzerConfigFromPrefs(const engine::Settings& prefs) {
    audio::AnalyzerConfig cfg;
    cfg.sampleBins = prefs.pcmNumBins;
    cfg.fftBins = prefs.fftNumBins;
    const std::string params = prefs.fftParams;
    const std::string unknown = audio::applyGForceFftParams(cfg, params);
    if (!unknown.empty()) std::fprintf(stderr, "[reforce] Audio.FFT.Params: ignored %s\n", unknown.c_str());
    // Audio.PCM.Smooth: no measurable effect in G-Force at its default (0.008); not mapped (see Analyzer.hpp).
    cfg.fftReleaseSeconds = prefs.fftSmooth / 60.0;
    cfg.freqLoHz = prefs.fftFreqLo;
    cfg.freqHiHz = prefs.fftFreqHi;
    cfg.responseScale = prefs.responseScale;
    cfg.beat.steady = prefs.beatClockSteadyTempo;
    return cfg;
}

AudioDriver::AudioDriver(const audio::AnalyzerConfig& cfg) : analyzer_(cfg) {}

AudioDriver::~AudioDriver() {
    if (control_.joinable()) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        cv_.notify_all();
        control_.join();
    }
    if (file_) file_->stop();
    std::lock_guard<std::mutex> lock(mutex_);
    if (device_) device_->close();
}

std::unique_ptr<audio::CoreAudioInput> AudioDriver::openInput(const std::string& device, std::string& requested) {
    requested = device;
    auto in = std::make_unique<audio::CoreAudioInput>();
    audio::InputOptions io;
    io.device = device;
    if (!in->open(io)) {
        std::fprintf(stderr, "[reforce] audio device \"%s\": %s\n", device.c_str(), in->lastError().c_str());
        // System Audio failing does not fall back to a microphone.
        if (device.empty() || device == audio::kSystemAudioId) return nullptr;
        std::fprintf(stderr, "[reforce] falling back to the system default input\n");
        in = std::make_unique<audio::CoreAudioInput>();
        io.device.clear();
        requested.clear();
        if (!in->open(io)) {
            std::fprintf(stderr, "[reforce] default input: %s\n", in->lastError().c_str());
            return nullptr;
        }
    }
    return in;
}

void AudioDriver::setSource(audio::AudioSource* s) {
    // Hazard wait: once the render thread is out of an update() that may have
    // loaded the old pointer, the old source can go.
    source_.store(s);
    while (inUpdate_.load()) std::this_thread::sleep_for(std::chrono::microseconds(200));
    std::lock_guard<std::mutex> lock(descMutex_);
    desc_ = s ? s->description() : std::string("silence");
}

bool AudioDriver::openDevice(const std::string& device) {
    std::string requested;
    auto in = openInput(device, requested);
    std::lock_guard<std::mutex> lock(mutex_);
    closeDeviceLocked();
    requested_ = requested;
    if (!in) return false;
    device_ = std::move(in);
    if (device_->sampleRate() > 0 && !control_.joinable()) analyzer_.setSampleRate(device_->sampleRate());
    setSource(device_.get());
    return true;
}

void AudioDriver::closeDeviceLocked() {
    if (!device_) return;
    if (source_.load() == device_.get()) setSource(file_.get());
    device_->close();
    device_.reset();
}

void AudioDriver::closeDevice() {
    std::lock_guard<std::mutex> lock(mutex_);
    closeDeviceLocked();
}

void AudioDriver::requestDevice(const std::string& device) {
    if (!control_.joinable()) {
        openDevice(device);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        request_ = std::optional<std::string>(device);
    }
    cv_.notify_all();
}

void AudioDriver::requestClose() {
    if (!control_.joinable()) {
        closeDevice();
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        request_ = std::optional<std::string>();
    }
    cv_.notify_all();
}

void AudioDriver::startControl() {
    if (control_.joinable() || file_) return;  // a file source needs no housekeeping thread
    control_ = std::thread([this] { controlLoop(); });
}

void AudioDriver::controlLoop() {
    pthread_setname_np("reforce.audio-control");
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stop_) {
        cv_.wait_for(lock, std::chrono::milliseconds(50), [&] { return stop_ || request_.has_value(); });
        if (stop_) break;
        if (request_) {
            const std::optional<std::string> want = *request_;
            request_.reset();
            if (!want) {
                closeDeviceLocked();
                std::fprintf(stderr, "[reforce] audio: input closed (silence)\n");
            } else {
                // Open without holding the lock (CoreAudio may take a while);
                // the menu reads the old state meanwhile.
                lock.unlock();
                std::string requested;
                auto in = openInput(*want, requested);
                lock.lock();
                closeDeviceLocked();
                requested_ = requested;
                if (in) {
                    device_ = std::move(in);
                    setSource(device_.get());
                }
                std::fprintf(stderr, "[reforce] audio: %s (input: %s)\n",
                             device_ ? device_->description().c_str() : "silence",
                             want->empty() ? "default" : want->c_str());
            }
        }
        // Housekeeping (reconnects, restarts after format changes).
        if (device_) {
            device_->service();
            std::lock_guard<std::mutex> d(descMutex_);
            if (source_.load() == device_.get()) desc_ = device_->description();
        }
    }
}

bool AudioDriver::hasDevice() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return device_ != nullptr && source_.load() == static_cast<audio::AudioSource*>(device_.get());
}

std::string AudioDriver::deviceName() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return device_ && source_.load() == device_.get() ? device_->device().name : std::string();
}

bool AudioDriver::followsDefault() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return device_ != nullptr && source_.load() == device_.get() && requested_.empty();
}

bool AudioDriver::isSystemAudio() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return device_ != nullptr && source_.load() == device_.get() && requested_ == audio::kSystemAudioId;
}

std::string AudioDriver::deviceUid() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return device_ && source_.load() == device_.get() ? device_->device().uid : std::string();
}

bool AudioDriver::openFile(const std::string& path, bool deterministic, bool loop) {
    file_ = std::make_unique<audio::AudioFileSource>();
    audio::FileOptions fo;
    fo.path = path;
    fo.realtime = !deterministic;
    fo.monitor = false;  // never play out loud
    fo.loop = loop;
    if (!file_->open(fo)) {
        std::fprintf(stderr, "[reforce] audio file %s: %s\n", path.c_str(), file_->lastError().c_str());
        file_.reset();
        return false;
    }
    deterministic_ = deterministic;
    setSource(file_.get());
    analyzer_.setSampleRate(file_->sampleRate());
    if (!deterministic) file_->start();
    return true;
}

const audio::AudioFrame& AudioDriver::update(double dt) {
    if (dt <= 0) dt = 1.0 / 60.0;
    inUpdate_.store(true);
    audio::AudioSource* src = source_.load();
    if (src) {
        // With the control thread, the live device is serviced there.
        if (!control_.joinable()) src->service();
        if (src->sampleRate() > 0 && src->sampleRate() != analyzer_.sampleRate())
            analyzer_.setSampleRate(src->sampleRate());
        analyzer_.consume(src->ring());
    }
    inUpdate_.store(false);
    return analyzer_.analyze(dt);
}

const audio::AudioFrame& AudioDriver::step(double fps) {
    if (!file_ || !deterministic_) return update(1.0 / fps);
    carry_ += file_->sampleRate() / fps;
    const auto n = static_cast<size_t>(carry_);
    carry_ -= double(n);
    file_->advance(n);
    analyzer_.consume(file_->ring());
    return analyzer_.analyze(double(n) / file_->sampleRate());
}

std::string AudioDriver::description() const {
    std::lock_guard<std::mutex> lock(mutex_);
    audio::AudioSource* src = source_.load();
    return src ? src->description() : std::string("silence");
}

std::string AudioDriver::cachedDescription() const {
    std::unique_lock<std::mutex> lock(descMutex_, std::try_to_lock);
    return lock.owns_lock() ? desc_ : std::string("(audio busy)");
}

bool AudioDriver::fileFinished() const { return file_ && file_->state() == audio::SourceState::Finished; }

}  // namespace reforce::app
