#include "render/Presenter.hpp"

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>

#include <pthread.h>

#include <algorithm>
#include <chrono>
#include <cstdio>

#include "render/Renderer.hpp"

namespace reforce::render {

// State shared with command-buffer completion handlers (which may run after
// the Presenter is gone, so they hold it by shared_ptr).
struct Presenter::Shared {
    std::mutex mutex;
    std::condition_variable cv;
    bool stop = false;
    unsigned writing = 0;      // slot bits: render-thread copies in flight
    unsigned reading = 0;      // slot bits: a present in flight
    int latest = -1;           // newest completed slot
    std::uint64_t latestSeq = 0, captureSeq = 0;
    Stats stats;
};

Presenter::Presenter(MTL::Device* device, const Renderer& renderer)
    : renderer_(renderer), device_(device->retain()), shared_(std::make_shared<Shared>()) {
    queue_ = device->newCommandQueue();
    queue_->setLabel(NS::String::string("reforce present", NS::UTF8StringEncoding));
    MTL::TextureDescriptor* td = MTL::TextureDescriptor::texture2DDescriptor(
        MTL::PixelFormatBGRA8Unorm, renderer.width(), renderer.height(), false);
    td->setUsage(MTL::TextureUsageShaderRead);
    td->setStorageMode(MTL::StorageModePrivate);
    for (auto& s : slots_) s = device->newTexture(td);
}

Presenter::~Presenter() {
    stop();
    for (auto* s : slots_)
        if (s) s->release();
    if (layer_) layer_->release();
    queue_->release();
    device_->release();
}

void Presenter::setLayer(CA::MetalLayer* layer) {
    if (!layer || layer == layer_) return;
    if (thread_.joinable()) return;  // one window for the app's lifetime
    layer_ = layer->retain();
    thread_ = std::thread([this] { run(); });
}

bool Presenter::encodeCapture(MTL::CommandBuffer* cmd, MTL::Texture* frame) {
    int slot = -1;
    std::uint64_t seq = 0;
    {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        if (!thread_.joinable() || shared_->stop) return false;
        for (int i = 0; i < kSlots; ++i) {
            const unsigned bit = 1u << i;
            if (i != shared_->latest && !(shared_->writing & bit) && !(shared_->reading & bit)) {
                slot = i;
                break;
            }
        }
        if (slot < 0) {
            ++shared_->stats.slotsFull;
            return false;
        }
        shared_->writing |= 1u << slot;
        seq = ++shared_->captureSeq;
        ++shared_->stats.captured;
    }
    MTL::BlitCommandEncoder* blit = cmd->blitCommandEncoder();
    blit->setLabel(NS::String::string("present capture", NS::UTF8StringEncoding));
    blit->copyFromTexture(frame, slots_[slot]);
    blit->endEncoding();
    auto shared = shared_;
    cmd->addCompletedHandler([shared, slot, seq](MTL::CommandBuffer* cb) {
        std::lock_guard<std::mutex> lock(shared->mutex);
        shared->writing &= ~(1u << slot);
        if (cb->status() == MTL::CommandBufferStatusCompleted && seq > shared->latestSeq) {
            shared->latest = slot;
            shared->latestSeq = seq;
            shared->cv.notify_all();
        }
    });
    return true;
}

void Presenter::stop() {
    {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        shared_->stop = true;
    }
    shared_->cv.notify_all();
    if (thread_.joinable()) thread_.join();
}

Presenter::Stats Presenter::takeStats() {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    Stats s = shared_->stats;
    shared_->stats = {};
    return s;
}

void Presenter::run() {
    pthread_setname_np("reforce.present");
    std::uint64_t shown = 0;
    MTL::CommandBuffer* previous = nullptr;
    for (;;) {
        int slot = -1;
        {
            std::unique_lock<std::mutex> lock(shared_->mutex);
            shared_->cv.wait(lock, [&] { return shared_->stop || (shared_->latest >= 0 && shared_->latestSeq != shown); });
            if (shared_->stop) break;
            slot = shared_->latest;
            shown = shared_->latestSeq;
            shared_->reading |= 1u << slot;
        }
        NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
        const auto t0 = std::chrono::steady_clock::now();
        // May block (up to ~1 s while the window is not on screen); this is
        // the only thread that ever waits for a drawable.
        CA::MetalDrawable* drawable = layer_->nextDrawable();
        const double waitMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (drawable) {
            MTL::CommandBuffer* cmd = queue_->commandBuffer();
            cmd->setLabel(NS::String::string("reforce present", NS::UTF8StringEncoding));
            renderer_.encodePresent(cmd, drawable, slots_[slot]);
            cmd->presentDrawable(drawable);
            auto shared = shared_;
            cmd->addCompletedHandler([shared, slot](MTL::CommandBuffer*) {
                std::lock_guard<std::mutex> lock(shared->mutex);
                shared->reading &= ~(1u << slot);
            });
            cmd->commit();
            if (previous) previous->release();
            previous = cmd->retain();
        } else {
            std::lock_guard<std::mutex> lock(shared_->mutex);
            shared_->reading &= ~(1u << slot);
        }
        {
            std::lock_guard<std::mutex> lock(shared_->mutex);
            if (drawable) ++shared_->stats.presented;
            else ++shared_->stats.noDrawable;
            shared_->stats.maxAcquireMs = std::max(shared_->stats.maxAcquireMs, waitMs);
        }
        pool->release();
        // One present in flight: its slot stays reserved until it is done.
        if (previous) previous->waitUntilCompleted();
    }
    if (previous) {
        previous->waitUntilCompleted();
        previous->release();
    }
}

}  // namespace reforce::render
