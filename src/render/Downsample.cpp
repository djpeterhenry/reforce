// reforce extension (reforce.Supersample): see Downsample.hpp.
#include "render/Downsample.hpp"

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

#include <algorithm>
#include <stdexcept>
#include <string>

#include "render/ShaderCompiler.hpp"

namespace reforce::render {
namespace {

constexpr const char* kShaders = R"MSL(
#include <metal_stdlib>
using namespace metal;
struct VOut { float4 position [[position]]; };
vertex VOut down_vs(uint vid [[vertex_id]]) {
    const float2 p = float2((vid << 1) & 2, vid & 2);
    VOut o;
    o.position = float4(p * 2.0f - 1.0f, 0.0f, 1.0f);
    return o;
}
static float3 to_linear(float3 c) {
    return select(pow((c + 0.055f) / 1.055f, float3(2.4f)), c / 12.92f, c <= 0.04045f);
}
static float3 to_srgb(float3 c) {
    return select(1.055f * pow(c, float3(1.0f / 2.4f)) - 0.055f, c * 12.92f, c <= 0.0031308f);
}
fragment float4 down_fs(VOut in [[stage_in]], texture2d<float, access::read> src [[texture(0)]],
                        constant int& factor [[buffer(0)]]) {
    const uint2 base = uint2(in.position.xy) * uint(factor);
    float3 c = 0.0f;
    float a = 0.0f;
    for (int y = 0; y < factor; ++y)
        for (int x = 0; x < factor; ++x) {
            const float4 s = src.read(base + uint2(x, y));
            c += to_linear(saturate(s.rgb));
            a += s.a;
        }
    const float n = float(factor * factor);
    return float4(to_srgb(c / n), a / n);
}
)MSL";

}  // namespace

Downsampler::Downsampler(MTL::Device* device, std::uint32_t outW, std::uint32_t outH, int factor, int slots)
    : factor_(std::clamp(factor, 1, 4)), slots_(std::clamp(slots, 1, 4)) {
    MTL::Library* lib = compileLibrary(device, kShaders, "reforce.downsample");
    MTL::Function* v = loadFunction(lib, "down_vs");
    MTL::Function* f = loadFunction(lib, "down_fs");
    MTL::RenderPipelineDescriptor* d = MTL::RenderPipelineDescriptor::alloc()->init();
    d->setLabel(NS::String::string("supersample downsample", NS::UTF8StringEncoding));
    d->setVertexFunction(v);
    d->setFragmentFunction(f);
    d->colorAttachments()->object(0)->setPixelFormat(MTL::PixelFormatBGRA8Unorm);
    NS::Error* e = nullptr;
    pso_ = device->newRenderPipelineState(d, &e);
    d->release();
    v->release();
    f->release();
    lib->release();
    if (!pso_)
        throw std::runtime_error(std::string("pipeline 'supersample downsample' failed: ") +
                                 (e ? e->localizedDescription()->utf8String() : "?"));
    for (int i = 0; i < slots_; ++i) {
        MTL::TextureDescriptor* td = MTL::TextureDescriptor::texture2DDescriptor(MTL::PixelFormatBGRA8Unorm, outW, outH, false);
        td->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
        td->setStorageMode(MTL::StorageModePrivate);
        out_[i] = device->newTexture(td);
        if (!out_[i]) throw std::runtime_error("supersample output allocation failed");
        out_[i]->setLabel(NS::String::string("supersampled frame", NS::UTF8StringEncoding));
    }
}

Downsampler::~Downsampler() {
    for (auto* t : out_)
        if (t) t->release();
    if (pso_) pso_->release();
}

MTL::Texture* Downsampler::encode(MTL::CommandBuffer* cmd, MTL::Texture* src, int slot) {
    MTL::Texture* out = out_[std::clamp(slot, 0, slots_ - 1)];
    MTL::RenderPassDescriptor* pass = MTL::RenderPassDescriptor::renderPassDescriptor();
    auto* a = pass->colorAttachments()->object(0);
    a->setTexture(out);
    a->setLoadAction(MTL::LoadActionDontCare);
    a->setStoreAction(MTL::StoreActionStore);
    MTL::RenderCommandEncoder* enc = cmd->renderCommandEncoder(pass);
    enc->setLabel(NS::String::string("supersample downsample", NS::UTF8StringEncoding));
    enc->setRenderPipelineState(pso_);
    enc->setFragmentTexture(src, 0);
    const int32_t k = factor_;
    enc->setFragmentBytes(&k, sizeof(k), 0);
    enc->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(3));
    enc->endEncoding();
    return out;
}

}  // namespace reforce::render
