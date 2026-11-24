// GPU poison mode: see GpuPoison.hpp.
#include "render/GpuPoison.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <unordered_set>

namespace reforce::render {
namespace {

bool isFloatFormat(MTL::PixelFormat f) {
    switch (f) {
    case MTL::PixelFormatR16Float:
    case MTL::PixelFormatRG16Float:
    case MTL::PixelFormatRGBA16Float:
    case MTL::PixelFormatR32Float:
    case MTL::PixelFormatRG32Float:
    case MTL::PixelFormatRGBA32Float:
    case MTL::PixelFormatRG11B10Float:
    case MTL::PixelFormatRGB9E5Float:
        return true;
    default:
        return false;
    }
}

bool isDepthOrStencil(MTL::PixelFormat f) {
    switch (f) {
    case MTL::PixelFormatDepth16Unorm:
    case MTL::PixelFormatDepth32Float:
    case MTL::PixelFormatStencil8:
    case MTL::PixelFormatDepth24Unorm_Stencil8:
    case MTL::PixelFormatDepth32Float_Stencil8:
    case MTL::PixelFormatX32_Stencil8:
    case MTL::PixelFormatX24_Stencil8:
        return true;
    default:
        return false;
    }
}

// Formats check() can scan for NaN: channel count and bytes per channel (2 = half, 4 = float); 0 when not scannable.
struct ScanFormat { int channels = 0, bytes = 0; };
ScanFormat scanFormat(MTL::PixelFormat f) {
    switch (f) {
    case MTL::PixelFormatR16Float: return {1, 2};
    case MTL::PixelFormatRG16Float: return {2, 2};
    case MTL::PixelFormatRGBA16Float: return {4, 2};
    case MTL::PixelFormatR32Float: return {1, 4};
    case MTL::PixelFormatRG32Float: return {2, 4};
    case MTL::PixelFormatRGBA32Float: return {4, 4};
    default: return {};
    }
}

MTL::ClearColor poisonColor(MTL::PixelFormat f) {
    if (isFloatFormat(f)) {
        const double nan = std::numeric_limits<double>::quiet_NaN();
        return MTL::ClearColor(nan, nan, nan, nan);
    }
    return MTL::ClearColor(1, 0, 1, 1);
}

const char* formatName(MTL::PixelFormat f) {
    switch (f) {
    case MTL::PixelFormatR16Float: return "R16Float";
    case MTL::PixelFormatRG16Float: return "RG16Float";
    case MTL::PixelFormatRGBA16Float: return "RGBA16Float";
    case MTL::PixelFormatR32Float: return "R32Float";
    case MTL::PixelFormatRG32Float: return "RG32Float";
    case MTL::PixelFormatRGBA32Float: return "RGBA32Float";
    default: return "?";
    }
}

std::mutex gMutex;
MTL::Device* gQueueDevice = nullptr;
MTL::CommandQueue* gQueue = nullptr;  // poison's own work: fills and readbacks (synchronous)
bool gChecking = false;
std::unordered_set<MTL::Texture*> gWritten;  // retained until the next check()

// Caller holds gMutex.
MTL::CommandQueue* queueFor(MTL::Device* device) {
    if (gQueueDevice != device) {
        if (gQueue) gQueue->release();
        gQueue = device->newCommandQueue();
        gQueue->setLabel(NS::String::string("GPU poison", NS::UTF8StringEncoding));
        gQueueDevice = device;
    }
    return gQueue;
}

void fillTexture(MTL::Device* device, MTL::Texture* t) {
    NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
    std::lock_guard<std::mutex> lock(gMutex);
    MTL::CommandBuffer* cmd = queueFor(device)->commandBuffer();
    const MTL::ClearColor color = poisonColor(t->pixelFormat());
    for (NS::UInteger level = 0; level < t->mipmapLevelCount(); ++level)
        for (NS::UInteger slice = 0; slice < t->arrayLength(); ++slice) {
            MTL::RenderPassDescriptor* rp = MTL::RenderPassDescriptor::renderPassDescriptor();
            auto* a = rp->colorAttachments()->object(0);
            a->setTexture(t);
            a->setLevel(level);
            a->setSlice(slice);
            a->setLoadAction(MTL::LoadActionClear);
            a->setClearColor(color);
            a->setStoreAction(MTL::StoreActionStore);
            MTL::RenderCommandEncoder* enc = cmd->renderCommandEncoder(rp);
            enc->setLabel(NS::String::string("poison new texture", NS::UTF8StringEncoding));
            enc->endEncoding();
        }
    cmd->commit();
    cmd->waitUntilCompleted();
    pool->release();
}

// newTexture with RenderTarget usage added (so it can be cleared to poison), then the clear.
template <class Create>
MTL::Texture* makePoisoned(MTL::Device* device, MTL::TextureDescriptor* td, Create create) {
    if (!poison::enabled()) return create();
    const bool clearable = td->storageMode() != MTL::StorageModeMemoryless && !isDepthOrStencil(td->pixelFormat());
    const MTL::TextureUsage usage = td->usage();
    if (clearable) td->setUsage(usage | MTL::TextureUsageRenderTarget);
    MTL::Texture* t = create();
    td->setUsage(usage);  // callers reuse descriptors
    if (t && clearable) fillTexture(device, t);
    return t;
}

// Counts NaN values in a tightly packed readback; first NaN pixel in *first.
std::size_t countNaN(const std::uint8_t* p, std::size_t w, std::size_t h, ScanFormat sf, std::size_t* first) {
    std::size_t n = 0;
    const std::size_t values = w * h * std::size_t(sf.channels);
    for (std::size_t i = 0; i < values; ++i) {
        bool nan;
        if (sf.bytes == 2) {
            std::uint16_t v;
            std::memcpy(&v, p + i * 2, 2);
            nan = (v & 0x7C00u) == 0x7C00u && (v & 0x03FFu) != 0;
        } else {
            float v;
            std::memcpy(&v, p + i * 4, 4);
            nan = std::isnan(v);
        }
        if (nan && n++ == 0) *first = i / std::size_t(sf.channels);
    }
    return n;
}

}  // namespace

MTL::Texture* makeTexture(MTL::Device* device, MTL::TextureDescriptor* td) {
    return makePoisoned(device, td, [&] { return device->newTexture(td); });
}

MTL::Texture* makeTexture(MTL::Device* device, MTL::TextureDescriptor* td, IOSurfaceRef surface, NS::UInteger plane) {
    return makePoisoned(device, td, [&] { return device->newTexture(td, surface, plane); });
}

MTL::Buffer* makeBuffer(MTL::Device* device, NS::UInteger length, MTL::ResourceOptions options) {
    MTL::Buffer* b = device->newBuffer(length, options);
    if (!b || !poison::enabled()) return b;
    if (b->storageMode() == MTL::StorageModePrivate) {
        NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
        std::lock_guard<std::mutex> lock(gMutex);
        MTL::CommandBuffer* cmd = queueFor(device)->commandBuffer();
        MTL::BlitCommandEncoder* blit = cmd->blitCommandEncoder();
        blit->fillBuffer(b, NS::Range(0, length), 0xFF);
        blit->endEncoding();
        cmd->commit();
        cmd->waitUntilCompleted();
        pool->release();
    } else {
        std::memset(b->contents(), 0xFF, length);
        if (b->storageMode() == MTL::StorageModeManaged) b->didModifyRange(NS::Range(0, length));
    }
    return b;
}

MTL::RenderCommandEncoder* makeRenderEncoder(MTL::CommandBuffer* cmd, MTL::RenderPassDescriptor* pass) {
    if (poison::enabled()) {
        std::lock_guard<std::mutex> lock(gMutex);
        for (NS::UInteger i = 0; i < 8; ++i) {
            auto* a = pass->colorAttachments()->object(i);
            MTL::Texture* t = a->texture();
            if (!t) continue;
            if (a->loadAction() == MTL::LoadActionDontCare) {
                a->setLoadAction(MTL::LoadActionClear);
                a->setClearColor(poisonColor(t->pixelFormat()));
            }
            if (!gChecking) continue;
            // What the pass leaves behind: the resolve target for MSAA, else the attachment itself.
            MTL::Texture* out = a->resolveTexture() ? a->resolveTexture() : t;
            if (out->sampleCount() == 1 && out->storageMode() != MTL::StorageModeMemoryless &&
                scanFormat(out->pixelFormat()).channels > 0 && gWritten.insert(out).second)
                out->retain();
        }
    }
    return cmd->renderCommandEncoder(pass);
}

namespace poison {

bool enabled() {
    static const bool on = [] {
        const char* v = std::getenv("REFORCE_GPU_POISON");
        const bool on = v && *v && std::strcmp(v, "0") != 0;
        if (on) std::fprintf(stderr, "[reforce] GPU poison mode: undefined GPU memory reads as NaN / magenta\n");
        return on;
    }();
    return on;
}

void startChecks() {
    if (!enabled()) return;
    std::lock_guard<std::mutex> lock(gMutex);
    gChecking = true;
}

std::vector<std::string> check() {
    std::vector<std::string> problems;
    if (!enabled()) return problems;
    std::unordered_set<MTL::Texture*> written;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        written.swap(gWritten);
    }
    std::vector<std::uint8_t> bytes;
    for (MTL::Texture* t : written) {
        NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
        const ScanFormat sf = scanFormat(t->pixelFormat());
        const std::size_t w = t->width(), h = t->height();
        const std::size_t rowBytes = w * std::size_t(sf.channels * sf.bytes);
        bytes.resize(rowBytes * h);
        if (t->storageMode() == MTL::StorageModeShared) {
            t->getBytes(bytes.data(), rowBytes, MTL::Region(0, 0, w, h), 0);
        } else {
            std::lock_guard<std::mutex> lock(gMutex);
            MTL::Buffer* buf = t->device()->newBuffer(rowBytes * h, MTL::ResourceStorageModeShared);
            MTL::CommandBuffer* cmd = queueFor(t->device())->commandBuffer();
            MTL::BlitCommandEncoder* blit = cmd->blitCommandEncoder();
            blit->copyFromTexture(t, 0, 0, MTL::Origin(0, 0, 0), MTL::Size(w, h, 1), buf, 0, rowBytes, rowBytes * h);
            blit->endEncoding();
            cmd->commit();
            cmd->waitUntilCompleted();
            std::memcpy(bytes.data(), buf->contents(), bytes.size());
            buf->release();
        }
        std::size_t first = 0;
        if (const std::size_t n = countNaN(bytes.data(), w, h, sf, &first)) {
            const char* label = t->label() ? t->label()->utf8String() : "unlabeled";
            char line[256];
            std::snprintf(line, sizeof line, "NaN in '%s' (%s %zux%zu): %zu values, first at pixel (%zu, %zu)",
                          label, formatName(t->pixelFormat()), w, h, n, first % w, first / w);
            problems.emplace_back(line);
        }
        t->release();
        pool->release();
    }
    return problems;
}

}  // namespace poison
}  // namespace reforce::render
