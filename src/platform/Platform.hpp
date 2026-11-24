#pragma once
// Platform layer interface. The only implementation is src/platform/mac
// (Cocoa + CAMetalLayer); everything outside src/platform talks to it through
// this header, which has no Objective-C in it.

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace CA { class MetalLayer; }
namespace MTL { class Device; }

namespace reforce::platform {

struct WindowConfig {
    int width = 1280;            // initial content size in points
    int height = 720;
    std::string title = "reforce";
    std::string controlPanelUrl;   // View > Open Control Panel (empty: item disabled)
    bool startFullScreen = false;  // settings reforce.Window.StartFullScreen
    // reforce.Window.Screen: the display to open on (macOS's name for it, or "external" = the first display that is
    // not built in); "" = where the window was last. Full screen at launch only happens when this display is there
    // (or none is named): otherwise reforce opens windowed where it was.
    std::string screen;
    bool openPanelOnLaunch = false;  // reforce.UI.OpenOnLaunch: open controlPanelUrl in the browser at launch
    // View > Start Like This / Don't Start Like This: saves the setup to the settings (null: the items are disabled).
    struct StartSetup {
        std::string screen;  // "" = none
        bool fullScreen = false;
        bool openPanel = false;
    };
    std::function<bool(const StartSetup&, std::string* error)> saveStartSetup;
    // Simulation rate: the display link is pinned to it when the screen
    // refreshes at a whole multiple (0 = every refresh).
    double targetFrameRate = 0;
    // The window frame (size, position, screen) and full-screen state are
    // remembered across launches under this name (NSUserDefaults of the bundle).
    std::string autosaveName = "reforce.MainWindow";
    // A development / test instance (--background): never activates the app, opens its window behind the others
    // without restoring or saving its frame, and has no Dock icon or menu bar, so a live instance stays undisturbed.
    bool background = false;
};

// Single instance (Phase 5A): another process of the same app (bundle id)
// that is still running. Pure so the decision can be tested.
struct RunningInstance {
    int pid = 0;
    bool terminated = false;
};
// Returns the pid of the instance to hand over to (activate) and exit, or 0
// to keep launching.
inline int instanceToActivate(int selfPid, const std::vector<RunningInstance>& running, bool allowMultiple) {
    if (allowMultiple) return 0;
    int best = 0;
    for (const RunningInstance& r : running)
        if (r.pid > 0 && r.pid != selfPid && !r.terminated && (best == 0 || r.pid < best)) best = r.pid;
    return best;  // the oldest (lowest pid) live one
}

// If another instance of this app (same bundle identifier) is running,
// activates it and returns true: the caller should exit. Unbundled binaries
// (no bundle id) always return false.
bool activateRunningInstance(bool allowMultiple);

// The app bundle's Contents/Resources folder, or "" when not running from a .app bundle.
std::string bundleResourcesDir();

// Audio > input device menu entries.
struct AudioInputItem {
    std::string id;     // what selectAudioInput takes: device name, "default", "system" or "none"
    std::string title;  // menu text
    bool checked = false;
    bool separatorBefore = false;
};

// Video menu entries (Phase 5F): a tree; `id` is what selectVideoMenu takes.
struct MenuItem {
    MenuItem() = default;
    MenuItem(std::string id_, std::string title_) : id(std::move(id_)), title(std::move(title_)) {}
    std::string id;     // "" = not selectable (header / submenu)
    std::string title;
    bool checked = false;
    bool enabled = true;
    bool separatorBefore = false;
    std::vector<MenuItem> children;  // non-empty: a submenu
};

// Debug/automation hooks, mostly for headless smoke tests. 0 = disabled.
struct DebugOptions {
    double quitAfter = 0;        // seconds after launch: quit
    double minimizeAfter = 0;    // seconds after launch: miniaturize the window
    double fullscreenAfter = 0;  // seconds after launch: toggle fullscreen
};

enum class FrameSource {
    DisplayLink,  // the view's CADisplayLink (window on screen)
    Timer,        // fallback timer (window occluded/minimized, display link paused)
};

struct FrameInfo {
    double hostTime = 0;              // seconds, CACurrentMediaTime() clock
    CA::MetalLayer* layer = nullptr;  // the window's layer (BGRA8Unorm)
    bool windowVisible = false;       // informational (stats); the present thread copes either way
    FrameSource source = FrameSource::DisplayLink;
    // Display refresh period in seconds (vsync spacing of the screen; 0 = unknown).
    double refreshPeriod = 0;
    // Rate the frame callbacks are paced at (Hz): the display link's pinned
    // rate, or the fallback timer's; 0 = unknown (every refresh).
    double callbackRate = 0;
};

struct KeyEvent {
    std::string chars;  // characters ignoring modifiers, lower-case ("w", "1", ...)
    bool shift = false, command = false, control = false, option = false;
    int keyCode = -1;   // virtual key code (kVK_*), -1 = unknown
};

inline constexpr int kKeyCodeEscape = 53;  // kVK_Escape

// What the window does with a key press (the NSView's keyDown and
// performKeyEquivalent both route through this; pure so it can be tested).
enum class WindowKey {
    Unhandled,         // not ours: pass up the responder chain
    Consumed,          // ours, nothing to do (Esc when not fullscreen)
    ToggleFullscreen,  // Cmd+Ctrl+F
    ExitFullscreen,    // Esc in fullscreen
    App,               // give it to FrameHandler::onKey (scene hotkeys)
};

// `keyEquivalent`: called from performKeyEquivalent (before the menu): only
// the fullscreen chord is ours there; Cmd+Q/H/M go on to the menu and every
// other key comes back as a keyDown.
inline WindowKey routeWindowKey(const KeyEvent& key, bool fullscreen, bool keyEquivalent = false) {
    if (key.command && key.control && !key.option && !key.shift && key.chars == "f")
        return WindowKey::ToggleFullscreen;
    if (keyEquivalent) return WindowKey::Unhandled;
    if (key.keyCode == kKeyCodeEscape) return fullscreen ? WindowKey::ExitFullscreen : WindowKey::Consumed;
    return key.chars.empty() ? WindowKey::Unhandled : WindowKey::App;
}

class FrameHandler {
public:
    virtual ~FrameHandler() = default;
    // Called on the render thread (not the main thread) inside an autorelease
    // pool, once per display link callback while the window is visible, and at
    // the simulation rate from a timer while it is not. The other callbacks
    // (keys, audio menu) come on the main thread.
    virtual void onFrame(const FrameInfo& frame) = 0;
    // Called once before runApplication returns.
    virtual void onShutdown() {}
    // Key press in the window (main thread). Return true when handled.
    virtual bool onKey(const KeyEvent& key) { return false; }
    // Audio menu (main thread): the entries to show, and the user's choice.
    virtual std::vector<AudioInputItem> audioInputs() { return {}; }
    virtual void selectAudioInput(const std::string& id) {}
    // Video menu (main thread): rebuilt each time it opens, and the choice.
    virtual std::vector<MenuItem> videoMenu() { return {}; }
    virtual void selectVideoMenu(const std::string& id) {}
    // View > Window Shows Visuals Only (main thread): its checkmark (reforce.Window.VisualsOnly); choosing it sends
    // selectVideoMenu("videoset:windowvisuals:on|off").
    virtual bool windowVisualsOnly() { return false; }
    // View > Room Lights (main thread): -1 unavailable in this instance, 0 off, 1 on (reforce.Lights.Enabled); choosing
    // it sends selectVideoMenu("videoset:lights:on|off").
    virtual int lightsMenuState() { return -1; }
};

// Creates the NSApplication, menu and window, then runs the event loop until
// the user quits. Returns a process exit code.
int runApplication(const WindowConfig& window, MTL::Device* device,
                   FrameHandler& handler, const DebugOptions& debug = {});

}  // namespace reforce::platform
