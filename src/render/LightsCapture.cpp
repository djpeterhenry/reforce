#include "render/LightsCapture.hpp"
#include "render/GpuPoison.hpp"

#include <Metal/Metal.hpp>

#include <algorithm>

namespace reforce::render {

struct LightsCapture::Shared {
    MTL::Texture* tex = nullptr;  // full size, mipmapped
    std::array<MTL::Buffer*, 3> buffers{};
    std::array<std::atomic<bool>, 3> busy{};
    std::uint32_t width = 0, height = 0, level = 0;
    int gw = 0, gh = 0, rowBytes = 0;
    Sink sink;

    ~Shared() {
        for (MTL::Buffer* b : buffers)
            if (b) b->release();
        if (tex) tex->release();
    }
};

LightsCapture::LightsCapture(MTL::Device* device, std::uint32_t width, std::uint32_t height, Sink sink)
    : s_(std::make_shared<Shared>()) {
    s_->width = width, s_->height = height;
    s_->sink = std::move(sink);
    // The first level at most 160 pixels wide (1920 -> 120 x 67, 1280 -> 160 x 90).
    std::uint32_t level = 0;
    while ((width >> level) > 160 && (height >> level) > 8) ++level;
    s_->level = level;
    s_->gw = int(std::max(1u, width >> level));
    s_->gh = int(std::max(1u, height >> level));
    s_->rowBytes = s_->gw * 4;
    MTL::TextureDescriptor* td =
        MTL::TextureDescriptor::texture2DDescriptor(MTL::PixelFormatBGRA8Unorm, width, height, true);
    td->setUsage(MTL::TextureUsageShaderRead);
    td->setStorageMode(MTL::StorageModePrivate);
    s_->tex = makeTexture(device, td);
    for (auto& b : s_->buffers)
        b = makeBuffer(device, NS::UInteger(s_->rowBytes) * NS::UInteger(s_->gh), MTL::ResourceStorageModeShared);
    for (auto& b : s_->busy) b.store(false);
}

LightsCapture::~LightsCapture() = default;

bool LightsCapture::valid() const {
    return s_->tex && std::all_of(s_->buffers.begin(), s_->buffers.end(), [](MTL::Buffer* b) { return b != nullptr; });
}
int LightsCapture::gridWidth() const { return s_->gw; }
int LightsCapture::gridHeight() const { return s_->gh; }

bool LightsCapture::encode(MTL::CommandBuffer* cmd, MTL::Texture* frame) {
    if (!valid() || !frame || frame->width() != s_->width || frame->height() != s_->height) return false;
    int slot = -1;
    for (int i = 0; i < 3 && slot < 0; ++i) {
        bool expected = false;
        if (s_->busy[std::size_t(i)].compare_exchange_strong(expected, true)) slot = i;
    }
    if (slot < 0) return false;
    MTL::BlitCommandEncoder* blit = cmd->blitCommandEncoder();
    const MTL::Size full(s_->width, s_->height, 1);
    blit->copyFromTexture(frame, 0, 0, MTL::Origin(0, 0, 0), full, s_->tex, 0, 0, MTL::Origin(0, 0, 0));
    blit->generateMipmaps(s_->tex);
    MTL::Buffer* buf = s_->buffers[std::size_t(slot)];
    blit->copyFromTexture(s_->tex, 0, s_->level, MTL::Origin(0, 0, 0), MTL::Size(NS::UInteger(s_->gw), NS::UInteger(s_->gh), 1),
                          buf, 0, NS::UInteger(s_->rowBytes), NS::UInteger(s_->rowBytes) * NS::UInteger(s_->gh));
    blit->endEncoding();
    std::shared_ptr<Shared> s = s_;
    cmd->addCompletedHandler([s, slot](MTL::CommandBuffer* cb) {
        if (cb->status() == MTL::CommandBufferStatusCompleted && s->sink)
            s->sink(static_cast<const std::uint8_t*>(s->buffers[std::size_t(slot)]->contents()), s->gw, s->gh,
                    s->rowBytes);
        s->busy[std::size_t(slot)].store(false);
    });
    return true;
}

}  // namespace reforce::render
