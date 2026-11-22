#include "render/FieldPipeline.hpp"

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <string>

#include "render/ScenePipeline.hpp"
#include "render/ShaderCompiler.hpp"

namespace reforce::render {
namespace {

constexpr const char* kFieldShaders = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct FOut { float4 position [[position]]; float2 uv; };

// A full-screen triangle; uv origin top-left.
vertex FOut field_vs(uint vid [[vertex_id]]) {
    const float2 p = float2((vid << 1) & 2, vid & 2);
    FOut o;
    o.position = float4(p * 2.0f - 1.0f, 0.0f, 1.0f);
    o.uv = float2(p.x, 1.0f - p.y);
    return o;
}

// ---- ripples: one substep of the wave equation (see render/FieldPipeline.hpp)
struct RippleP { int2 size; float c2; float damp; float sponge; int reflect; int drops; int pad; float4 drop[32]; };

static float at(texture2d<float> t, int2 q, int2 m) { return t.read(uint2(clamp(q, int2(0), m))).r; }

fragment float4 ripple_step_fs(FOut in [[stage_in]], constant RippleP& P [[buffer(0)]],
                               texture2d<float> cur [[texture(0)]], texture2d<float> prev [[texture(1)]]) {
    const int2 p = int2(in.position.xy);
    const int2 m = P.size - 1;
    const float h = at(cur, p, m);
    // Clamped reads: the edges reflect (no flux through them).
    const float lap = at(cur, p + int2(1, 0), m) + at(cur, p - int2(1, 0), m) + at(cur, p + int2(0, 1), m) +
                      at(cur, p - int2(0, 1), m) - 4.0f * h;
    float n = (2.0f * h - at(prev, p, m) + P.c2 * lap) * P.damp;
    if (P.reflect == 0 && P.sponge > 0.0f) {
        // Absorbing edges: a sponge P.sponge cells wide.
        const float e = float(min(min(p.x, m.x - p.x), min(p.y, m.y - p.y)));
        if (e < P.sponge) {
            const float k = 1.0f - e / P.sponge;
            n *= 1.0f - 0.15f * k * k;
        }
    }
    for (int i = 0; i < P.drops; ++i) {
        const float4 d = P.drop[i];  // x, y (cells), radius (cells), amplitude
        const float2 q = float2(p) + 0.5f - d.xy;
        n += d.w * exp(-dot(q, q) / (d.z * d.z));
    }
    return float4(n, 0.0f, 0.0f, 0.0f);
}

// ---- ripples: crests (and a touch of the slope, lit from the top left) into the RG8 target, premultiplied.
struct ShowP { float gain; float slope; float2 texel; };

fragment float4 ripple_show_fs(FOut in [[stage_in]], constant ShowP& P [[buffer(0)]],
                               texture2d<float> h [[texture(0)]]) {
    constexpr sampler s(filter::linear, address::clamp_to_edge);
    const float v = h.sample(s, in.uv).r;
    const float gx = h.sample(s, in.uv + float2(P.texel.x, 0)).r - h.sample(s, in.uv - float2(P.texel.x, 0)).r;
    const float gy = h.sample(s, in.uv + float2(0, P.texel.y)).r - h.sample(s, in.uv - float2(0, P.texel.y)).r;
    // The low chatter (old rings' tails) stays dark: only what clears the floor is drawn.
    const float raw = P.gain * (max(v, 0.0f) + P.slope * max(0.0f, -gx - gy));
    const float lum = saturate((raw - 0.15f) / 0.85f);
    const float a = saturate(lum * 3.0f);
    return float4(lum * a, a, 0.0f, 0.0f);
}

// ---- Julia set (see render/FieldPipeline.hpp)
struct JuliaP { float2 c; float2 centre; float2 inv; float aspect; int iter; float gain; float bands; float phase;
                float pad; };

fragment float4 julia_fs(FOut in [[stage_in]], constant JuliaP& P [[buffer(0)]]) {
    const float2 p = float2((in.uv.x * 2.0f - 1.0f) * P.aspect, 1.0f - 2.0f * in.uv.y) - P.centre;
    float2 z = 1.6f * float2(P.inv.x * p.x + P.inv.y * p.y, -P.inv.y * p.x + P.inv.x * p.y);
    float r2 = dot(z, z);
    int i = 0;
    for (; i < P.iter && r2 <= 256.0f; ++i) {
        z = float2(z.x * z.x - z.y * z.y, 2.0f * z.x * z.y) + P.c;
        r2 = dot(z, z);
    }
    if (r2 <= 256.0f) return float4(0.0f);  // inside: dark
    const float mu = max(0.0f, float(i) + 1.0f - log2(0.5f * log(r2)));
    const float v = mu / float(P.iter);
    float lum = saturate(pow(v, 0.4f) * P.gain);
    if (P.bands > 0.0f) lum *= 0.5f + 0.5f * cos(6.2831853f * P.bands * v + P.phase);
    const float a = saturate(lum * 3.0f);
    return float4(lum * a, a, 0.0f, 0.0f);
}
)MSL";

struct RippleP {
    int32_t size[2];
    float c2, damp, sponge;
    int32_t reflect, drops, pad;
    float drop[32][4];
};
static_assert(sizeof(RippleP) == 32 + 32 * 16, "RippleP must match the MSL struct");
struct ShowP {
    float gain, slope;
    float texel[2];
};
struct JuliaP {
    float c[2], centre[2], inv[2];
    float aspect;
    int32_t iter;
    float gain, bands, phase, pad;
};
static_assert(sizeof(JuliaP) == 48, "JuliaP must match the MSL struct");

MTL::RenderPipelineState* makePso(MTL::Device* dev, MTL::Library* lib, const char* fs, MTL::PixelFormat fmt,
                                  const char* label) {
    MTL::Function* v = loadFunction(lib, "field_vs");
    MTL::Function* f = loadFunction(lib, fs);
    MTL::RenderPipelineDescriptor* d = MTL::RenderPipelineDescriptor::alloc()->init();
    d->setLabel(NS::String::string(label, NS::UTF8StringEncoding));
    d->setVertexFunction(v);
    d->setFragmentFunction(f);
    d->colorAttachments()->object(0)->setPixelFormat(fmt);
    NS::Error* err = nullptr;
    MTL::RenderPipelineState* pso = dev->newRenderPipelineState(d, &err);
    d->release();
    v->release();
    f->release();
    if (!pso)
        throw std::runtime_error(std::string("pipeline '") + label +
                                 "' failed: " + (err ? err->localizedDescription()->utf8String() : "?"));
    return pso;
}

MTL::Texture* newTarget(MTL::Device* dev, MTL::PixelFormat fmt, uint32_t w, uint32_t h, const char* label) {
    MTL::TextureDescriptor* td = MTL::TextureDescriptor::texture2DDescriptor(fmt, w, h, false);
    td->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
    td->setStorageMode(MTL::StorageModePrivate);
    MTL::Texture* t = dev->newTexture(td);
    if (!t) throw std::runtime_error(std::string(label) + " allocation failed");
    t->setLabel(NS::String::string(label, NS::UTF8StringEncoding));
    return t;
}

void fullScreen(MTL::CommandBuffer* cmd, MTL::Texture* target, MTL::RenderPipelineState* pso, const void* bytes,
                std::size_t size, MTL::Texture* t0, MTL::Texture* t1, const char* label) {
    MTL::RenderPassDescriptor* pass = MTL::RenderPassDescriptor::renderPassDescriptor();
    auto* att = pass->colorAttachments()->object(0);
    att->setTexture(target);
    att->setLoadAction(MTL::LoadActionDontCare);
    att->setStoreAction(MTL::StoreActionStore);
    MTL::RenderCommandEncoder* enc = cmd->renderCommandEncoder(pass);
    enc->setLabel(NS::String::string(label, NS::UTF8StringEncoding));
    enc->setRenderPipelineState(pso);
    enc->setFragmentBytes(bytes, NS::UInteger(size), 0);
    if (t0) enc->setFragmentTexture(t0, 0);
    if (t1) enc->setFragmentTexture(t1, 1);
    enc->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(3));
    enc->endEncoding();
}

}  // namespace

FieldPipeline::FieldPipeline(ScenePipeline& scene, const FieldPipelineOptions& opts)
    : scene_(scene), device_(scene.device()->retain()), width_(scene.width()), height_(scene.height()) {
    rows_ = std::clamp(opts.rippleRows, 32, std::max(32, int(height_)));
    cols_ = std::max(8, int(std::lround(double(rows_) * double(width_) / double(std::max(1u, height_)))));
    MTL::Library* lib = compileLibrary(device_, kFieldShaders, "reforce.field");
    try {
        stepPso_ = makePso(device_, lib, "ripple_step_fs", MTL::PixelFormatR32Float, "ripple step");
        showPso_ = makePso(device_, lib, "ripple_show_fs", MTL::PixelFormatRG8Unorm, "ripple show");
        juliaPso_ = makePso(device_, lib, "julia_fs", MTL::PixelFormatRG8Unorm, "julia");
    } catch (...) {
        lib->release();
        throw;
    }
    lib->release();
    const int n = std::clamp(opts.maxLive, 0, 16);
    for (int i = 0; i < n; ++i) {
        Slot s;
        s.fresh.reserve(kMaxDrops);  // no render-thread allocations
        s.undo.reserve(kMaxDrops);
        s.tex = newTarget(device_, MTL::PixelFormatRG8Unorm, width_, height_, "organic field target");
        for (auto& h : s.h)
            h = newTarget(device_, MTL::PixelFormatR32Float, uint32_t(cols_), uint32_t(rows_), "ripple height");
        s.handle = scene_.addSolidTexture(s.tex);
        slots_.push_back(s);
    }
}

FieldPipeline::~FieldPipeline() {
    for (auto& s : slots_) {
        scene_.removeSpriteTexture(s.handle);
        s.tex->release();
        for (auto* h : s.h) h->release();
    }
    if (stepPso_) stepPso_->release();
    if (showPso_) showPso_->release();
    if (juliaPso_) juliaPso_->release();
    device_->release();
}

int FieldPipeline::substeps(double speed, double dt) const {
    const double cells = std::max(0.0, speed) * double(rows_) * std::max(0.0, dt);
    return std::clamp(int(std::ceil(cells / 0.5)), 1, kMaxSubsteps);
}

MTL::Texture* FieldPipeline::height(int slot) const {
    if (slot < 0 || slot >= maxLive()) return nullptr;
    const Slot& s = slots_[std::size_t(slot)];
    return s.h[s.cur];
}

void FieldPipeline::clearFields(MTL::CommandBuffer* cmd, int slot) {
    for (MTL::Texture* h : slots_[std::size_t(slot)].h) {
        MTL::RenderPassDescriptor* pass = MTL::RenderPassDescriptor::renderPassDescriptor();
        auto* att = pass->colorAttachments()->object(0);
        att->setTexture(h);
        att->setLoadAction(MTL::LoadActionClear);
        att->setClearColor(MTL::ClearColor(0, 0, 0, 0));
        att->setStoreAction(MTL::StoreActionStore);
        MTL::RenderCommandEncoder* enc = cmd->renderCommandEncoder(pass);
        enc->setLabel(NS::String::string("ripple clear", NS::UTF8StringEncoding));
        enc->endEncoding();
    }
}

SolidQuad FieldPipeline::encode(MTL::CommandBuffer* cmd, int slot, const engine::OrganicField& f) {
    SolidQuad q;
    if (slot < 0 || slot >= maxLive()) return q;
    Slot& s = slots_[std::size_t(slot)];
    const double aspect = double(width_) / double(std::max(1u, height_));
    if (f.kind == engine::OrganicField::Kind::Julia) {
        const double sc = std::abs(f.scale) > 1e-6 ? f.scale : 1e-6;
        JuliaP p{};
        p.c[0] = float(f.cre);
        p.c[1] = float(f.cim);
        p.centre[0] = float(f.x);
        p.centre[1] = float(f.y);
        p.inv[0] = float(std::cos(f.rot) / sc);
        p.inv[1] = float(std::sin(f.rot) / sc);
        p.aspect = float(aspect);
        p.iter = std::clamp(f.iter, 1, 1000);
        p.gain = float(f.gain);
        p.bands = float(f.bands);
        p.phase = float(f.phase);
        fullScreen(cmd, s.tex, juliaPso_, &p, sizeof(p), nullptr, nullptr, "julia");
    } else {
        if (f.reset) {
            clearFields(cmd, slot);
            s.cur = 1;
            s.undo.clear();
        }
        const double dt = std::max(0.0, f.dt);
        const int n = dt > 0 ? substeps(f.speed, dt) : 1;
        const double c = dt > 0 ? std::min(0.5, f.speed * double(rows_) * dt / n) : 0.0;
        RippleP p{};
        p.size[0] = cols_;
        p.size[1] = rows_;
        p.c2 = float(c * c);
        p.damp = float(dt > 0 ? std::exp(-dt / n / std::max(1e-3, f.decay)) : 1.0);
        p.sponge = float(std::max(4.0, 0.03 * rows_));
        p.reflect = f.reflect ? 1 : 0;
        // A drop is a displacement, not a push: added in one substep and taken back from the next one's
        // (h' = 2 h - h_prev + ...: the bump stays, with no velocity). With one substep the take-back waits for the
        // next step.
        auto add = [&](const std::array<float, 4>& d, float sign) {
            if (p.drops >= 2 * kMaxDrops) return;
            float* o = p.drop[p.drops++];
            o[0] = d[0];
            o[1] = d[1];
            o[2] = d[2];
            o[3] = sign * d[3];
        };
        std::vector<std::array<float, 4>>& fresh = s.fresh;
        fresh.clear();
        for (const auto& d : f.drops) {
            if (int(fresh.size()) >= kMaxDrops) break;
            fresh.push_back({float(d.x * cols_), float(d.y * rows_), float(std::max(1.5, d.radius * rows_)),
                             float(d.amp)});
        }
        for (int i = 0; i < n; ++i) {
            p.drops = 0;
            if (i == 0) {
                for (const auto& d : s.undo) add(d, -1.0f);
                s.undo.clear();
                for (const auto& d : fresh) add(d, 1.0f);
            } else if (i == 1) {
                for (const auto& d : fresh) add(d, -1.0f);
                fresh.clear();
            }
            const int next = (s.cur + 1) % 3, prev = (s.cur + 2) % 3;
            fullScreen(cmd, s.h[next], stepPso_, &p, sizeof(p), s.h[s.cur], s.h[prev], "ripple step");
            s.cur = next;
        }
        s.undo.swap(fresh);  // one substep: take them back next step
        ShowP sp{float(f.gain), 6.0f, {1.0f / float(cols_), 1.0f / float(rows_)}};
        fullScreen(cmd, s.tex, showPso_, &sp, sizeof(sp), s.h[s.cur], nullptr, "ripple show");
    }
    q.visible = true;
    q.texture = s.handle;
    q.cx = float(width_) * 0.5f;
    q.cy = float(height_) * 0.5f;
    q.w = float(width_);
    q.h = float(height_);
    q.u0 = q.v0 = 0;
    q.u1 = q.v1 = 1;
    return q;
}

}  // namespace reforce::render
