#pragma once
// The per-frame loop: render into the offscreen frame, publish it to Syphon,
// present it to the window. Platform-agnostic (talks to platform/ via the
// FrameHandler interface).

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <vector>

#include "audio/Analyzer.hpp"
#include "engine/FixedStep.hpp"
#include "engine/Perf.hpp"
#include "platform/Platform.hpp"

namespace MTL { class Device; class CommandQueue; class Texture; }
namespace reforce::render { class Renderer; class Presenter; class Downsampler; }
namespace reforce::app { class Director; class AudioDriver; }
namespace reforce::platform { class SyphonPublisher; }
namespace reforce::sync { class LinkClock; class DeckFeed; }

namespace reforce::app {

struct AppOptions {
    uint32_t frameWidth = 1920;   // the output (window, Syphon) size
    uint32_t frameHeight = 1080;
    int supersample = 1;          // reforce.Supersample: render at this x the output size, averaged down
    const char* syphonName = "reforce";
    double statsInterval = 5.0;   // seconds between stats lines on stderr; 0 = off
    bool fixedStep = false;       // --deterministic: exactly one simulation step per display frame
    double fps = 60.0;            // simulation rate (Graphics.TargetFrameRate)
    int maxCatchUpSteps = 4;      // simulation steps per display frame at most
    bool seedNoise = false;       // start from a noise image (M3 static demo)
    // Phase 5D instrumentation.
    double stressInterval = 0;    // --stress: switch all layers / spawn every ~N s (0 = off)
    double perfReport = 0;        // --perf-report SECONDS: print the frame-time report at shutdown
    double perfWarmup = 3.0;      // seconds excluded from the report (startup builds)
    // Ableton Link (Phase 5H): join a session and fill AudioFrame::link* every step.
    bool link = false;            // reforce.Link.Enabled (never with --deterministic / offline)
    double linkOffset = 0.0;      // seconds ahead to sample the session (reforce.Link.OffsetMs)
    double linkQuantum = 4.0;     // reforce.Link.Quantum
    bool linkFollow = true;       // reforce.Link.Follow
    bool linkDriveBeat = true;    // reforce.Link.DriveBeat
    // Deck starts (PLAN 5O.8): rkbx_link deck timing + DJ controller MIDI (never with --deterministic / offline).
    bool deckFeed = false;
    int deckFeedPort = 4470;           // reforce.Phrase.RkbxPort
    std::string deckFeedMidi;          // reforce.Phrase.MidiDevice ("" = any mapped controller, "none" = no MIDI)
    std::vector<std::filesystem::path> controllerDirs;  // DJ controller maps (sync/ControllerMap), later ones win
};

class App final : public platform::FrameHandler {
public:
    // `director`'s visualizer must render at options.frameWidth x frameHeight.
    // Both are owned by the App from here on. `audio` may be null (silence).
    App(MTL::Device* device, const AppOptions& options, std::unique_ptr<Director> director,
        std::unique_ptr<AudioDriver> audio);
    ~App() override;

    void onFrame(const platform::FrameInfo& frame) override;
    void onShutdown() override;
    bool onKey(const platform::KeyEvent& key) override;
    std::vector<platform::AudioInputItem> audioInputs() override;
    // "default" = system default input, "system" = System Audio (everything the Mac plays),
    // "none" = silence, else a device name/UID.
    void selectAudioInput(const std::string& id) override;
    // Called after the user picked an input in the Audio menu (to save it).
    // Opens input `id` (see selectAudioInput) from the frame loop once *answer
    // becomes 1 (microphone permission granted); 2 = denied: stay silent.
    void deferAudioInput(std::string id, std::shared_ptr<std::atomic<int>> answer) {
        deferredInput_ = std::move(id);
        deferredAnswer_ = std::move(answer);
    }
    void setAudioInputChanged(std::function<void(const std::string&)> fn) { audioInputChanged_ = std::move(fn); }
    // Video menu (Phase 5F): sources for the color layer / sprites, layer settings.
    std::vector<platform::MenuItem> videoMenu() override;
    void selectVideoMenu(const std::string& id) override;
    bool windowVisualsOnly() override;
    int lightsMenuState() override;

    // Measured display frames per second (any thread; web UI stats).
    double displayFps() const { return displayFps_.load(std::memory_order_relaxed); }
    // Frame-time summary since the warm-up (any thread; web UI stats, --perf-report).
    perf::PerfSummary perfSummary() const;

private:
    void logStats(double now);
    void stress(double now);
    void recordFrame(const platform::FrameInfo& frame, int steps, double cpuMs, double simMs, double colorizeMs,
                     double submitMs, double gpuMs, double intervalMs, double wallT, bool reserved);

    AppOptions options_;
    MTL::Device* device_;
    MTL::CommandQueue* queue_;
    std::unique_ptr<render::Renderer> renderer_;
    std::unique_ptr<render::Downsampler> downsampler_;  // supersample > 1: the frame averaged to the output size
    std::unique_ptr<render::Presenter> presenter_;  // window present on its own thread
    // Window copy with the color video layer when the Syphon output gets the
    // plain visualizer (reforce.Video.Layer.Syphon = 0); created on first use.
    MTL::Texture* windowFrame_ = nullptr;
    // The Syphon output with its alpha (reforce.Syphon.Alpha); created on first use.
    MTL::Texture* syphonFrame_ = nullptr;
    std::unique_ptr<platform::SyphonPublisher> syphon_;
    std::unique_ptr<Director> director_;
    std::unique_ptr<AudioDriver> audio_;
    std::unique_ptr<sync::LinkClock> link_;
    std::unique_ptr<sync::DeckFeed> deckFeed_;  // PLAN 5O.8
    audio::AudioFrame linkFrame_;  // the analyzer's frame plus the Link fields (copied per step, no allocation)
    std::function<void(const std::string&)> audioInputChanged_;
    std::string deferredInput_;
    std::shared_ptr<std::atomic<int>> deferredAnswer_;
    void openAudioInput(const std::string& id);
    engine::FixedStepClock clock_;
    double lastHost_ = -1;
    uint64_t frameIndex_ = 0;
    uint64_t simSteps_ = 0, stepsSinceStats_ = 0;
    struct GpuStats { std::atomic<double> sumMs{0}; std::atomic<int> count{0}; };
    double cpuMsSum_ = 0;
    uint64_t cpuFrames_ = 0;

    double startTime_ = -1;
    std::shared_ptr<std::atomic<int>> inFlight_;
    std::shared_ptr<GpuStats> gpu_;

    // stats since last log
    double lastStats_ = 0;
    bool windowVisible_ = false;
    uint64_t framesLink_ = 0, framesTimer_ = 0, framesSkipped_ = 0;
    uint64_t totalLink_ = 0, totalTimer_ = 0;
    // displayFps(): frames counted over ~1 s windows.
    std::atomic<double> displayFps_{0};
    double fpsWindowStart_ = -1;
    uint64_t fpsWindowFrames_ = 0;

    // Phase 5D: per-frame timings, hitch detector.
    std::shared_ptr<perf::FrameStats> stats_;
    bool statsWarm_ = false;
    double lastFrameHost_ = -1;
    double hitchLogWindow_ = 0;
    int hitchLogCount_ = 0;
    uint64_t hitchesSinceStats_ = 0;
    mutable std::mutex summaryMutex_;
    perf::PerfSummary summary_;
    double lastSummary_ = 0;
    // --stress
    std::mt19937 stressRng_{12345};
    double nextStress_ = -1;
    int stressTick_ = 0;
};

}  // namespace reforce::app
