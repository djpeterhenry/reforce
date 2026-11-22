#pragma once
// reforce extension: room lights that follow the visuals (docs/lights.md): Nanoleaf panels driven directly over the
// local network, in place of the Nanoleaf Desktop app's screen mirroring.
//
// The connection lives on its own thread: it reads the panels' layout, keeps what they were showing, switches them
// to external control (Nanoleaf Open API, extControl v2: UDP colour packets), and sends a colour per panel about 20
// times a second from the newest capture (submit, any thread). Turned off (setEnabled(false)) or destroyed, it puts
// the panels back: on/off as they were, and their effect when it is a saved one. Every 10 s it checks the panels still
// answer; when they don't, it reconnects every 5 s.
//
// The device and its token are in nanoleaf.json next to settings.json ({"host", "port", "auth_token"}; "udpPort" for
// tests). Pairing writes it (lights/Nanoleaf: pairNanoleaf).
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "lights/LightMap.hpp"

namespace reforce::lights {

struct DeviceConfig {
    std::string host;  // "Light-Panels-5c-1b-0b.local" or an address
    int port = 16021;
    std::string token;
    int udpPort = 60222;
};
std::optional<DeviceConfig> loadDeviceConfig(const std::filesystem::path& path, std::string* error = nullptr);
bool saveDeviceConfig(const std::filesystem::path& path, const DeviceConfig& cfg, std::string* error = nullptr);

// What GET /api/v1/<token>/ says (the parts we use).
struct DeviceState {
    std::string name, model, firmware;
    bool on = false;
    std::string effect;                // effects.select ("*Dynamic*", "*Solid*" or a saved effect)
    std::vector<std::string> effects;  // effects.effectsList
    Layout layout;
};
std::optional<DeviceState> parseDeviceState(std::string_view json, std::string* error = nullptr);

// Pairing: the controller's power button held 5-7 s (its LEDs flash), then this within 30 s. The token, or nullopt.
std::optional<std::string> pairNanoleaf(const std::string& host, int port, std::string* error = nullptr);
// The first Nanoleaf announcing itself on the local network (Bonjour _nanoleafapi._tcp): its host name and port.
std::optional<std::pair<std::string, int>> discoverNanoleaf(double timeoutSeconds, std::string* error = nullptr);

struct LightsStatus {
    std::string state = "off";  // off | not paired | connecting | streaming | pairing | error
    std::string message;        // the last problem, or what is going on
    std::string device;         // the panels' name
    int panels = 0;
    double sideLength = 150;    // layout units
    double sendHz = 0;          // packets a second (last second)
    bool pulsing = false;       // the beat pulse runs (on, and the beat clock is confident)
};

struct PanelView {
    int id = 0;
    double x = 0, y = 0, o = 0;  // layout (y up), orientation in degrees
    double u = 0, v = 0;         // centre in the frame, 0..1
    Rgb colour;                  // as last sent
};

class LightsOutput {
public:
    explicit LightsOutput(std::filesystem::path configPath);
    ~LightsOutput();
    LightsOutput(const LightsOutput&) = delete;
    LightsOutput& operator=(const LightsOutput&) = delete;

    // Stops the thread, putting the panels back first (blocks up to a few seconds). Idempotent; the destructor calls it.
    void shutdown();
    void setEnabled(bool on);
    bool enabled() const { return enabled_.load(); }
    void setSettings(const MapSettings& s);
    // Streaming: a capture is wanted (the caller may skip the GPU work otherwise).
    bool wantsFrames() const { return streaming_.load(std::memory_order_relaxed); }
    // The newest capture (BGRA8, top row first). Copied; any thread.
    void submit(const std::uint8_t* bgra, int w, int h, int rowBytes);
    // The beat clock now (sync/BeatClock: running count, tempo, confident), for the pulse. Any thread.
    void setBeat(double beat, double bpm, bool confident);
    // Pairing (the panels go back first): finds the panels (the paired host, else Bonjour), then asks for a token
    // every 2 s for a minute while the power button is held; saves nanoleaf.json. Status "pairing" meanwhile.
    void startPairing();
    void cancelPairing();

    LightsStatus status() const;
    std::vector<PanelView> panels() const;
    const std::filesystem::path& configPath() const { return configPath_; }

private:
    void run();
    bool connect();  // config, state, extControl; false with status set
    void restore();
    bool request(const char* method, const std::string& path, const std::string& body, std::string* response,
                 double timeout);
    void setStatus(const std::string& state, const std::string& message);
    void pair();  // the thread's pairing run (m_ not held)

    std::filesystem::path configPath_;
    std::thread thread_;
    mutable std::mutex m_;
    std::condition_variable cv_;
    bool quit_ = false;
    std::atomic<bool> enabled_{false};
    std::atomic<bool> streaming_{false};
    MapSettings settings_;

    // Captures: submit fills pending_, the thread swaps it with work_.
    std::vector<std::uint8_t> pending_, work_;
    int pw_ = 0, ph_ = 0, prow_ = 0;
    std::uint64_t pendingSeq_ = 0;
    bool pairRequested_ = false, pairCancel_ = false;
    double beat_ = 0, bpm_ = 0;
    bool beatConfident_ = false;
    std::chrono::steady_clock::time_point beatAt_{};

    // Thread-owned connection.
    DeviceConfig cfg_;
    int sock_ = -1;
    std::vector<std::uint8_t> addr_;  // sockaddr_in
    std::optional<DeviceState> saved_;  // what to put back
    LightMapper mapper_;
    std::vector<int> ids_;
    std::vector<Rgb> colours_, sent_;
    std::vector<std::uint8_t> packet_;

    mutable std::mutex viewM_;
    LightsStatus status_;
    std::vector<PanelView> view_;
};

}  // namespace reforce::lights
