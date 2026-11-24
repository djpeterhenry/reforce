#include "render/VideoMips.hpp"

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

#include <algorithm>
#include <stdexcept>

#include "engine/Perf.hpp"
#include "render/GpuPoison.hpp"
#include "render/ShaderCompiler.hpp"

namespace reforce::render {
namespace {

// Half-size box filter: each target texel's center maps to the corner shared
// by a 2x2 block of source texels, so one bilinear tap averages exactly that
// block (odd sizes: the last column/row is clamped).
constexpr const char* kShaders = R"MSL(
#include <metal_stdlib>
using namespace metal;
struct VOut { float4 position [[position]]; float2 uv; };
vertex VOut vmips_vs(uint vid [[vertex_id]]) {
    const float2 p = float2((vid << 1) & 2, vid & 2);
    VOut o;
    o.position = float4(p * 2.0f - 1.0f, 0.0f, 1.0f);
    o.uv = float2(p.x, 1.0f - p.y);
    return o;
}
fragment float4 vmips_fs(VOut in [[stage_in]], texture2d<float> src [[texture(0)]]) {
    constexpr sampler smp(filter::linear, address::clamp_to_edge);
    return float4(src.sample(smp, in.uv).rgb, 1.0f);
}
)MSL";

constexpr std::uint64_t kKeepTicks = 120;  // unused slots are recycled after ~2 s of steps

}  // namespace

int VideoMips::levelsFor(int w, int h) {
    int levels = 1;
    for (int m = std::max(w, h); m > 1; m >>= 1) ++levels;
    return levels;
}

VideoMips::VideoMips(MTL::Device* device) : device_(device) {
    MTL::Library* lib = compileLibrary(device, kShaders, "reforce.videomips");
    MTL::Function* v = loadFunction(lib, "vmips_vs");
    MTL::Function* f = loadFunction(lib, "vmips_fs");
    MTL::RenderPipelineDescriptor* desc = MTL::RenderPipelineDescriptor::alloc()->init();
    desc->setLabel(NS::String::string("video mips", NS::UTF8StringEncoding));
    desc->setVertexFunction(v);
    desc->setFragmentFunction(f);
    desc->colorAttachments()->object(0)->setPixelFormat(MTL::PixelFormatBGRA8Unorm);
    NS::Error* err = nullptr;
    pso_ = device->newRenderPipelineState(desc, &err);
    desc->release();
    v->release();
    f->release();
    lib->release();
    if (!pso_)
        throw std::runtime_error(std::string("video mips pipeline: ") +
                                 (err ? err->localizedDescription()->utf8String() : "unknown error"));
}

VideoMips::~VideoMips() {
    for (auto& [k, s] : slots_)
        if (s.tex) s.tex->release();
    for (auto* t : free_) t->release();
    if (pso_) pso_->release();
}

MTL::Texture* VideoMips::acquire(int w, int h) {
    for (auto it = free_.begin(); it != free_.end(); ++it)
        if (int((*it)->width()) == w && int((*it)->height()) == h) {
            MTL::Texture* t = *it;
            free_.erase(it);
            return t;
        }
    MTL::TextureDescriptor* td =
        MTL::TextureDescriptor::texture2DDescriptor(MTL::PixelFormatBGRA8Unorm, NS::UInteger(w), NS::UInteger(h), true);
    td->setMipmapLevelCount(NS::UInteger(levelsFor(w, h)));
    td->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
    td->setStorageMode(MTL::StorageModePrivate);
    perf::countRenderAlloc();  // once per new size (a new video)
    MTL::Texture* t = makeTexture(device_, td);
    if (t) t->setLabel(NS::String::string("video mips", NS::UTF8StringEncoding));
    return t;
}

void VideoMips::recycle(MTL::Texture* t) {
    if (!t) return;
    // In-flight command buffers retain it; keep a couple per pool, drop the rest.
    if (free_.size() >= 4) {
        free_.front()->release();
        free_.erase(free_.begin());
    }
    free_.push_back(t);
}

MTL::Texture* VideoMips::prepare(MTL::CommandBuffer* cmd, const void* slotKey, MTL::Texture* frame,
                                 const std::shared_ptr<const void>& id, double scale) {
    if (!frame || !cmd || !(scale < kMinScale) || scale <= 0) return frame;
    const int fw = int(frame->width()), fh = int(frame->height());
    if (fw < 4 || fh < 4) return frame;
    const int w = halfSize(fw), h = halfSize(fh);
    Slot& s = slots_[slotKey];
    s.used = tick_;
    if (s.tex && (int(s.tex->width()) != w || int(s.tex->height()) != h)) {
        recycle(s.tex);
        s.tex = nullptr;
        s.valid = false;
    }
    if (!s.tex) {
        s.tex = acquire(w, h);
        s.valid = false;
        if (!s.tex) return frame;
    }
    const bool same = s.valid && id && !s.id.owner_before(id) && !id.owner_before(s.id);
    if (same) return s.tex;

    MTL::RenderPassDescriptor* pass = MTL::RenderPassDescriptor::renderPassDescriptor();
    auto* att = pass->colorAttachments()->object(0);
    att->setTexture(s.tex);
    att->setLevel(0);
    att->setLoadAction(MTL::LoadActionDontCare);
    att->setStoreAction(MTL::StoreActionStore);
    MTL::RenderCommandEncoder* enc = makeRenderEncoder(cmd, pass);
    enc->setLabel(NS::String::string("video downscale", NS::UTF8StringEncoding));
    enc->setRenderPipelineState(pso_);
    enc->setFragmentTexture(frame, 0);
    enc->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(3));
    enc->endEncoding();
    if (s.tex->mipmapLevelCount() > 1) {
        MTL::BlitCommandEncoder* blit = cmd->blitCommandEncoder();
        blit->setLabel(NS::String::string("video mips", NS::UTF8StringEncoding));
        blit->generateMipmaps(s.tex);
        blit->endEncoding();
    }
    s.id = id;
    s.valid = id != nullptr;
    ++builds_;
    return s.tex;
}

void VideoMips::collect() {
    ++tick_;
    for (auto it = slots_.begin(); it != slots_.end();) {
        if (tick_ - it->second.used > kKeepTicks) {
            recycle(it->second.tex);
            it = slots_.erase(it);
        } else {
            ++it;
        }
    }
}

}  // namespace reforce::render
