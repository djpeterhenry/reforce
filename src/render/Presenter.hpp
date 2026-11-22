#pragma once
// Window present on its own thread (architecture §6), so the sim/render/Syphon
// thread never blocks in CAMetalLayer nextDrawable.
//
// The render thread copies each finished frame into one of a few private
// "present slots" (a blit on its own command buffer); when that command
// buffer completes, the slot becomes the latest frame. The present thread
// waits for a new latest frame, acquires a drawable (this may block, e.g. up
// to a second while the window is occluded or minimized), scales the slot
// into it and presents. An occluded window therefore keeps updating whenever
// the system hands out drawables (OBS window capture), and Syphon is
// unaffected either way.
//
// Slots: at most 2 render frames in flight (writing) + the latest + the one
// being presented (one present in flight at a time) = 4.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

namespace MTL { class Device; class Texture; class CommandBuffer; class CommandQueue; }
namespace CA { class MetalLayer; }

namespace reforce::render {

class Renderer;

class Presenter {
public:
    static constexpr int kSlots = 4;

    // `renderer` provides the present pipeline and must outlive the Presenter.
    Presenter(MTL::Device* device, const Renderer& renderer);
    ~Presenter();  // stop()
    Presenter(const Presenter&) = delete;
    Presenter& operator=(const Presenter&) = delete;

    // Render thread: the window's layer (retained). The first call starts the
    // present thread; later calls with the same layer are cheap no-ops.
    void setLayer(CA::MetalLayer* layer);

    // Render thread: copies `frame` (the renderer's frame size) into a free
    // slot on `cmd`; it becomes the latest frame when `cmd` completes. False
    // when no slot was free (frame not offered to the window).
    bool encodeCapture(MTL::CommandBuffer* cmd, MTL::Texture* frame);

    // Stops and joins the present thread (waits for a blocked nextDrawable).
    void stop();

    struct Stats {
        std::uint64_t presented = 0;    // drawables presented
        std::uint64_t noDrawable = 0;   // nextDrawable returned nil (timed out)
        std::uint64_t captured = 0;     // frames offered
        std::uint64_t slotsFull = 0;    // frames not offered (no free slot)
        double maxAcquireMs = 0;        // longest nextDrawable wait
    };
    // Counts since the last call (any thread).
    Stats takeStats();

private:
    struct Shared;
    void run();

    const Renderer& renderer_;
    MTL::Device* device_;
    MTL::CommandQueue* queue_ = nullptr;
    MTL::Texture* slots_[kSlots] = {};
    CA::MetalLayer* layer_ = nullptr;
    std::shared_ptr<Shared> shared_;
    std::thread thread_;
};

}  // namespace reforce::render
