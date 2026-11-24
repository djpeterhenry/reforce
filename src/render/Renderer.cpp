#include "render/Renderer.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurface.h>

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>

#include <stdexcept>

#include "render/GpuPoison.hpp"
#include "render/Layout.hpp"
#include "render/ShaderCompiler.hpp"

namespace reforce::render {
namespace {

// Compiled at runtime, like the scene shaders will be.
constexpr const char* kShaderSource = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct VOut {
    float4 position [[position]];
    float2 uv;          // 0..1, (0,0) = top-left
};

// One triangle covering the viewport.
vertex VOut fullscreen_vs(uint vid [[vertex_id]]) {
    float2 p = float2((vid << 1) & 2, vid & 2);
    VOut o;
    o.position = float4(p * 2.0 - 1.0, 0.0, 1.0);
    o.uv = float2(p.x, 1.0 - p.y);
    return o;
}

struct PatternParams {
    float  time;
    float2 resolution;
};

// Animated test pattern: drifting color gradient, 16 bouncing bars, a
// sweeping scanline, and a 4px white border to check edges/letterboxing.
fragment float4 pattern_fs(VOut in [[stage_in]], constant PatternParams& P [[buffer(0)]]) {
    float2 uv = in.uv;
    float t = P.time;
    float3 grad = 0.5 + 0.5 * cos(6.2831853 * (uv.x * 0.6 + uv.y * 0.3 + t * 0.1)
                                  + float3(0.0, 2.0944, 4.1888));
    const float N = 16.0;
    float bx = uv.x * N;
    float i = floor(bx);
    float fx = fract(bx);
    float h = 0.5 + 0.45 * sin(t * 2.0 + i * 0.7) * sin(t * 0.37 + i * 1.3);
    float bar = step(1.0 - uv.y, h) * step(0.1, fx) * step(fx, 0.9);
    float3 c = mix(grad * 0.35, 1.0 - grad * 0.5, bar);
    c += smoothstep(0.004, 0.0, abs(uv.y - fract(t * 0.25)));
    float2 px = uv * P.resolution;
    if (px.x < 4.0 || px.y < 4.0 || px.x > P.resolution.x - 4.0 || px.y > P.resolution.y - 4.0)
        c = float3(1.0);
    return float4(saturate(c), 1.0);
}

fragment float4 present_fs(VOut in [[stage_in]], texture2d<float> src [[texture(0)]]) {
    constexpr sampler s(filter::linear, address::clamp_to_edge);
    return src.sample(s, in.uv);
}
)MSL";

struct PatternParams {   // must match the MSL struct layout
    float time;
    float pad_;
    float resolution[2];
};
static_assert(sizeof(PatternParams) == 16);

void setNumber(CFMutableDictionaryRef dict, CFStringRef key, long value) {
    CFNumberRef n = CFNumberCreate(kCFAllocatorDefault, kCFNumberLongType, &value);
    CFDictionarySetValue(dict, key, n);
    CFRelease(n);
}

IOSurfaceRef createSurface(uint32_t width, uint32_t height) {
    CFMutableDictionaryRef props = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    setNumber(props, kIOSurfaceWidth, width);
    setNumber(props, kIOSurfaceHeight, height);
    setNumber(props, kIOSurfaceBytesPerElement, 4);
    setNumber(props, kIOSurfaceBytesPerRow, static_cast<long>(IOSurfaceAlignProperty(kIOSurfaceBytesPerRow, width * 4)));
    setNumber(props, kIOSurfacePixelFormat, 'BGRA');
    IOSurfaceRef surface = IOSurfaceCreate(props);
    CFRelease(props);
    return surface;
}

MTL::RenderPipelineState* makePipeline(MTL::Device* device, MTL::Library* lib,
                                       const char* vs, const char* fs, const char* label) {
    MTL::Function* v = loadFunction(lib, vs);
    MTL::Function* f = loadFunction(lib, fs);
    MTL::RenderPipelineDescriptor* desc = MTL::RenderPipelineDescriptor::alloc()->init();
    desc->setLabel(NS::String::string(label, NS::UTF8StringEncoding));
    desc->setVertexFunction(v);
    desc->setFragmentFunction(f);
    desc->colorAttachments()->object(0)->setPixelFormat(MTL::PixelFormatBGRA8Unorm);
    NS::Error* error = nullptr;
    MTL::RenderPipelineState* pso = device->newRenderPipelineState(desc, &error);
    desc->release();
    v->release();
    f->release();
    if (!pso) {
        throw std::runtime_error(std::string("pipeline '") + label + "' failed: " +
                                 (error ? error->localizedDescription()->utf8String() : "?"));
    }
    return pso;
}

}  // namespace

Renderer::Renderer(MTL::Device* device, uint32_t width, uint32_t height)
    : device_(device->retain()), width_(width), height_(height) {
    surface_ = createSurface(width, height);
    if (!surface_) throw std::runtime_error("IOSurfaceCreate failed");

    MTL::TextureDescriptor* td = MTL::TextureDescriptor::texture2DDescriptor(
        MTL::PixelFormatBGRA8Unorm, width, height, false);
    td->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
    td->setStorageMode(device->hasUnifiedMemory() ? MTL::StorageModeShared : MTL::StorageModeManaged);
    frame_ = makeTexture(device, td, surface_, 0);
    if (!frame_) throw std::runtime_error("newTexture(iosurface) failed");
    frame_->setLabel(NS::String::string("reforce frame", NS::UTF8StringEncoding));

    MTL::Library* lib = compileLibrary(device, kShaderSource, "reforce.builtin");
    patternPipeline_ = makePipeline(device, lib, "fullscreen_vs", "pattern_fs", "test pattern");
    presentPipeline_ = makePipeline(device, lib, "fullscreen_vs", "present_fs", "present");
    lib->release();
}

Renderer::~Renderer() {
    if (presentPipeline_) presentPipeline_->release();
    if (patternPipeline_) patternPipeline_->release();
    if (frame_) frame_->release();
    if (surface_) CFRelease(surface_);
    device_->release();
}

void Renderer::encodeTestPattern(MTL::CommandBuffer* cmd, float timeSeconds) {
    MTL::RenderPassDescriptor* pass = MTL::RenderPassDescriptor::renderPassDescriptor();
    auto* att = pass->colorAttachments()->object(0);
    att->setTexture(frame_);
    att->setLoadAction(MTL::LoadActionDontCare);
    att->setStoreAction(MTL::StoreActionStore);

    PatternParams params{timeSeconds, 0.f, {float(width_), float(height_)}};
    MTL::RenderCommandEncoder* enc = makeRenderEncoder(cmd, pass);
    enc->setLabel(NS::String::string("test pattern", NS::UTF8StringEncoding));
    enc->setRenderPipelineState(patternPipeline_);
    enc->setFragmentBytes(&params, sizeof(params), 0);
    enc->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(3));
    enc->endEncoding();
}

void Renderer::encodePresent(MTL::CommandBuffer* cmd, CA::MetalDrawable* drawable, MTL::Texture* source) const {
    MTL::Texture* target = drawable->texture();
    const Rect r = aspectFit(width_, height_, double(target->width()), double(target->height()));

    MTL::RenderPassDescriptor* pass = MTL::RenderPassDescriptor::renderPassDescriptor();
    auto* att = pass->colorAttachments()->object(0);
    att->setTexture(target);
    att->setLoadAction(MTL::LoadActionClear);
    att->setClearColor(MTL::ClearColor(0, 0, 0, 1));
    att->setStoreAction(MTL::StoreActionStore);

    MTL::RenderCommandEncoder* enc = makeRenderEncoder(cmd, pass);
    enc->setLabel(NS::String::string("present", NS::UTF8StringEncoding));
    enc->setViewport(MTL::Viewport{r.x, r.y, r.width, r.height, 0.0, 1.0});
    enc->setRenderPipelineState(presentPipeline_);
    enc->setFragmentTexture(source ? source : frame_, 0);
    enc->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(3));
    enc->endEncoding();
}

}  // namespace reforce::render
