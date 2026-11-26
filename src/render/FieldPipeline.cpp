#include "render/FieldPipeline.hpp"

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <string>

#include "render/GpuPoison.hpp"
#include "render/ScenePipeline.hpp"
#include "render/ShaderCompiler.hpp"

namespace reforce::render {
namespace {

#define REFORCE_FIELD_VS R"MSL(
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
)MSL"

constexpr const char* kFieldShaders = REFORCE_FIELD_VS R"MSL(

// ---- Julia set (see render/FieldPipeline.hpp)
struct JuliaP { float2 c; float2 center; float2 inv; float aspect; int iter; float gain; float bands; float phase;
                float pad; };

fragment float4 julia_fs(FOut in [[stage_in]], constant JuliaP& P [[buffer(0)]]) {
    const float2 p = float2((in.uv.x * 2.0f - 1.0f) * P.aspect, 1.0f - 2.0f * in.uv.y) - P.center;
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

// The Mandelbrot dive, compiled without fast math: double-float (df64) arithmetic relies on exact rounding
// (two-sum, fma two-product; Dekker / Knuth, as in Thall's "Extended-precision floating-point numbers for GPU
// computation"). A df64 value is float2(hi, lo), hi + lo, |lo| <= ulp(hi) / 2: ~48 significant bits.
constexpr const char* kMandelShaders = REFORCE_FIELD_VS R"MSL(
static float2 two_sum(float a, float b) {
    const float s = a + b;
    const float v = s - a;
    return float2(s, (a - (s - v)) + (b - v));
}
static float2 fast_two_sum(float a, float b) {
    const float s = a + b;
    return float2(s, b - (s - a));
}
// The "sloppy" sum: its error is ~2^-48 of the larger operand (absolute), not of the result, which is all the
// iteration needs (|z| <= 16; what matters is the error next to a pixel's size), at about half the cost.
static float2 df_add(float2 a, float2 b) {
    const float2 s = two_sum(a.x, b.x);
    return fast_two_sum(s.x, s.y + (a.y + b.y));
}
static float2 df_mul(float2 a, float2 b) {
    const float p = a.x * b.x;
    float e = fma(a.x, b.x, -p);
    e = fma(a.x, b.y, e);
    e = fma(a.y, b.x, e);
    return fast_two_sum(p, e);
}
static float2 df_sqr(float2 a) {
    const float p = a.x * a.x;
    float e = fma(a.x, a.x, -p);
    e = fma(2.0f * a.x, a.y, e);
    return fast_two_sum(p, e);
}

// c: the view center as df64 (hi re, hi im, lo re, lo im); rs: cos, sin of the turn x plane units per tree unit.
// pix: plane units per pixel; glow: the edge's glow in pixels (0 = none). mode: 0 float32, 1 double-float,
// 2 perturbation around the reference orbit `ref` (refLen + 1 points, ref[0] = 0) of the view center.
struct MandelP { float4 c; float2 center; float2 rs; float aspect; int iter; float gain; float band; float phase;
                 int mode; float pix; float glow; int refLen; int mini; float2 nOff; float2 invS; int pad0; int pad1; };

static float2 cmul(float2 a, float2 b) { return float2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x); }

fragment float4 mandel_fs(FOut in [[stage_in]], constant MandelP& P [[buffer(0)]],
                          const device float2* ref [[buffer(1)]]) {
    const float2 p = float2((in.uv.x * 2.0f - 1.0f) * P.aspect, 1.0f - 2.0f * in.uv.y) - P.center;
    const float2 d = float2(P.rs.x * p.x - P.rs.y * p.y, P.rs.y * p.x + P.rs.x * p.y);  // offset from the center
    const float2 c0 = P.c.xy + (P.c.zw + d);
    int i = 0;
    float r2 = 0.0f;
    float2 dv = float2(0.0f);  // dz/dc (float is plenty): the distance estimate
    // The main cardioid and the period-2 bulb are inside: skip them (most of the overview's dark).
    const float qx = c0.x - 0.25f, q = qx * qx + c0.y * c0.y;
    const bool inside = q * (q + qx) <= 0.25f * c0.y * c0.y || (c0.x + 1.0f) * (c0.x + 1.0f) + c0.y * c0.y <= 0.0625f;
    // A mini filling the frame: w = (c - nucleus) / size is where c sits on the copy; its main cardioid and period-2
    // bulb (shrunk 10 %: the copy is not exact at the edges) are inside, so most of its dark costs nothing.
    bool miniInside = false;
    if (P.mini != 0) {
        const float2 w = cmul(d - P.nOff, P.invS);
        const float wx = w.x - 0.25f, wq = wx * wx + w.y * w.y;
        miniInside = wq * (wq + wx) <= 0.225f * w.y * w.y - 0.005f ||
                     (w.x + 1.0f) * (w.x + 1.0f) + w.y * w.y <= 0.0625f * 0.81f;
    }
    if ((inside && P.mode == 0) || miniInside) {
        i = P.iter;
    } else if (P.mode == 0) {
        float2 z = float2(0.0f);
        for (; i < P.iter; ++i) {
            dv = 2.0f * cmul(z, dv) + float2(1.0f, 0.0f);
            z = float2(z.x * z.x - z.y * z.y, 2.0f * z.x * z.y) + c0;
            r2 = dot(z, z);
            if (r2 > 256.0f) break;
        }
    } else if (P.mode == 1) {
        const float2 cx = df_add(float2(P.c.x, P.c.z), float2(d.x, 0.0f));
        const float2 cy = df_add(float2(P.c.y, P.c.w), float2(d.y, 0.0f));
        float2 x = float2(0.0f), y = float2(0.0f);
        for (; i < P.iter; ++i) {
            dv = 2.0f * cmul(float2(x.x, y.x), dv) + float2(1.0f, 0.0f);
            const float2 x2 = df_sqr(x), y2 = df_sqr(y), xy = df_mul(x, y);
            x = df_add(df_add(x2, -y2), cx);
            y = df_add(2.0f * xy, cy);
            r2 = x.x * x.x + y.x * y.x;
            if (r2 > 256.0f) break;
        }
    } else {
        // Perturbation: z = Z_m + e, with Z the reference orbit (the view center's, computed in double on the CPU)
        // and e the pixel's difference from it, e' = 2 Z_m e + e^2 + d, all small numbers float holds well.
        // Rebasing (Zhuoran 2021): when |z| < |e| (or the reference runs out) carry on from e = z, m = 0: no
        // glitches and one reference orbit for the frame.
        float2 e = float2(0.0f), z = float2(0.0f);
        int m = 0;
        for (; i < P.iter; ++i) {
            dv = 2.0f * cmul(z, dv) + float2(1.0f, 0.0f);
            e = 2.0f * cmul(ref[m], e) + cmul(e, e) + d;
            ++m;
            z = ref[m] + e;
            r2 = dot(z, z);
            if (r2 > 256.0f) break;
            if (r2 < dot(e, e) || m >= P.refLen) {
                e = z;
                m = 0;
            }
        }
    }
    const bool escaped = i < P.iter;
    // Smooth escape count; inside: the cap (so the derivatives below stay defined across the quad).
    const float mu = escaped ? max(0.0f, float(i) + 2.0f - log2(0.5f * log(r2))) : float(P.iter);
    // Distance to the set in pixels (the distance estimate |z| ln|z| / |dz/dc|): the shading depends on it, not on
    // the escape count's size, so it looks the same at every depth. Bright at the edge, fading out over ~2^10 px.
    const float dist = escaped ? 0.5f * sqrt(r2) * log(r2) / max(length(dv), 1e-30f) / P.pix : 0.0f;
    const float near = saturate(1.0f - log2(max(dist, 1.0f)) * (1.0f / 12.0f));
    float lum = near;
    if (P.band > 0.0f) {
        // Bands of the escape count (P.band iterations each), faded toward their mean where they would alias.
        const float t = mu / P.band;
        const float w = saturate(1.5f - 1.5f * fwidth(t));
        lum *= 0.5f + 0.5f * w * cos(6.2831853f * t + P.phase);
    }
    lum = saturate(lum * P.gain);
    if (!escaped) return float4(0.0f);  // inside: dark
    // The edge glows: within P.glow pixels of the set fully lit, then falling off as 1 / distance^2.
    if (P.glow > 0.0f) {
        const float g = saturate(P.glow / max(dist, 1e-6f));
        lum = max(lum, saturate(P.gain * g * g));
    }
    const float a = saturate(lum * 3.0f);
    return float4(lum * a, a, 0.0f, 0.0f);
}
)MSL";

struct JuliaP {
    float c[2], center[2], inv[2];
    float aspect;
    int32_t iter;
    float gain, bands, phase, pad;
};
static_assert(sizeof(JuliaP) == 48, "JuliaP must match the MSL struct");
struct MandelP {
    float c[4], center[2], rs[2];
    float aspect;
    int32_t iter;
    float gain, band, phase;
    int32_t mode;
    float pix, glow;
    int32_t refLen, mini;
    float nOff[2], invS[2];
    int32_t pad[2];
};
static_assert(sizeof(MandelP) == 96, "MandelP must match the MSL struct");

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
    MTL::Texture* t = makeTexture(dev, td);
    if (!t) throw std::runtime_error(std::string(label) + " allocation failed");
    t->setLabel(NS::String::string(label, NS::UTF8StringEncoding));
    return t;
}

void fullScreen(MTL::CommandBuffer* cmd, MTL::Texture* target, MTL::RenderPipelineState* pso, const void* bytes,
                std::size_t size, MTL::Texture* t0, MTL::Texture* t1, const char* label, double scale = 1.0,
                MTL::Buffer* b1 = nullptr) {
    MTL::RenderPassDescriptor* pass = MTL::RenderPassDescriptor::renderPassDescriptor();
    auto* att = pass->colorAttachments()->object(0);
    att->setTexture(target);
    att->setLoadAction(MTL::LoadActionDontCare);
    att->setStoreAction(MTL::StoreActionStore);
    MTL::RenderCommandEncoder* enc = makeRenderEncoder(cmd, pass);
    enc->setLabel(NS::String::string(label, NS::UTF8StringEncoding));
    enc->setRenderPipelineState(pso);
    if (scale < 1.0) {
        // Only the top-left corner (scale x the target): the full-screen triangle maps onto it.
        enc->setViewport(MTL::Viewport{0.0, 0.0, std::ceil(double(target->width()) * scale),
                                       std::ceil(double(target->height()) * scale), 0.0, 1.0});
    }
    enc->setFragmentBytes(bytes, NS::UInteger(size), 0);
    if (t0) enc->setFragmentTexture(t0, 0);
    if (t1) enc->setFragmentTexture(t1, 1);
    if (b1) enc->setFragmentBuffer(b1, 0, 1);
    enc->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(3));
    enc->endEncoding();
}

}  // namespace

FieldPipeline::FieldPipeline(ScenePipeline& scene, const FieldPipelineOptions& opts)
    : scene_(scene), device_(scene.device()->retain()), width_(scene.width()), height_(scene.height()) {
    MTL::Library* lib = compileLibrary(device_, kFieldShaders, "reforce.field");
    try {
        juliaPso_ = makePso(device_, lib, "julia_fs", MTL::PixelFormatRG8Unorm, "julia");
    } catch (...) {
        lib->release();
        throw;
    }
    lib->release();
    lib = compileLibrary(device_, kMandelShaders, "reforce.field.mandel", /*fastMath=*/false);
    try {
        mandelPso_ = makePso(device_, lib, "mandel_fs", MTL::PixelFormatRG8Unorm, "mandel");
    } catch (...) {
        lib->release();
        throw;
    }
    lib->release();
    const int n = std::clamp(opts.maxLive, 0, 16);
    for (int i = 0; i < n; ++i) {
        Slot s;
        s.tex = newTarget(device_, MTL::PixelFormatRG8Unorm, width_, height_, "organic field target");
        for (auto& b : s.ref) {
            b = makeBuffer(device_, NS::UInteger((kMaxRef + 1) * 2 * sizeof(float)), MTL::ResourceStorageModeShared);
            if (!b) throw std::runtime_error("mandel reference buffer allocation failed");
        }
        s.handle = scene_.addSolidTexture(s.tex);
        slots_.push_back(s);
    }
}

FieldPipeline::~FieldPipeline() {
    for (auto& s : slots_) {
        scene_.removeSpriteTexture(s.handle);
        s.tex->release();
        for (auto* b : s.ref) b->release();
    }
    if (juliaPso_) juliaPso_->release();
    if (mandelPso_) mandelPso_->release();
    device_->release();
}

SolidQuad FieldPipeline::encode(MTL::CommandBuffer* cmd, int slot, const engine::OrganicField& f) {
    SolidQuad q;
    if (slot < 0 || slot >= maxLive()) return q;
    double res = 1.0;  // Mandel's Res
    Slot& s = slots_[std::size_t(slot)];
    const double aspect = double(width_) / double(std::max(1u, height_));
    if (f.kind == engine::OrganicField::Kind::Julia) {
        const double sc = std::abs(f.scale) > 1e-6 ? f.scale : 1e-6;
        JuliaP p{};
        p.c[0] = float(f.cre);
        p.c[1] = float(f.cim);
        p.center[0] = float(f.x);
        p.center[1] = float(f.y);
        p.inv[0] = float(std::cos(f.rot) / sc);
        p.inv[1] = float(std::sin(f.rot) / sc);
        p.aspect = float(aspect);
        p.iter = std::clamp(f.iter, 1, 1000);
        p.gain = float(f.gain);
        p.bands = float(f.bands);
        p.phase = float(f.phase);
        fullScreen(cmd, s.tex, juliaPso_, &p, sizeof(p), nullptr, nullptr, "julia");
    } else {  // Mandel
        const double sc = std::abs(f.scale) > 1e-6 ? f.scale : 1e-6;
        res = std::clamp(std::isfinite(f.res) ? f.res : 1.0, 0.25, 1.0);
        MandelP p{};
        // The center in double-float: hi = the nearest float, lo = the rest.
        const double cx = std::isfinite(f.cx) ? f.cx : 0.0, cy = std::isfinite(f.cy) ? f.cy : 0.0;
        p.c[0] = float(cx);
        p.c[1] = float(cy);
        p.c[2] = float(cx - double(p.c[0]));
        p.c[3] = float(cy - double(p.c[1]));
        p.center[0] = float(f.x);
        p.center[1] = float(f.y);
        const double k = (std::isfinite(f.half) ? f.half : 1.0) / sc;
        p.rs[0] = float(std::cos(f.rot) * k);
        p.rs[1] = float(std::sin(f.rot) * k);
        p.aspect = float(aspect);
        p.iter = std::clamp(f.iter, 1, 20000);
        p.gain = float(f.gain);
        p.band = float(f.bands);
        p.phase = float(std::remainder(f.phase, 2.0 * 3.141592653589793));
        p.mode = std::clamp(f.precision, 0, 2);
        MTL::Buffer* refBuf = s.ref[0];  // bound in every mode (the shader declares it)
        if (p.mode == 2) {
            // The view center's orbit in double, stored as float (it only multiplies the small difference).
            refBuf = s.ref[s.refNext];
            s.refNext = (s.refNext + 1) % int(std::size(s.ref));
            auto* z = static_cast<float*>(refBuf->contents());
            const int n = std::min(p.iter, kMaxRef);
            double zx = 0, zy = 0;
            z[0] = z[1] = 0.0f;
            int len = 0;
            while (len < n) {
                const double t = zx * zx - zy * zy + cx;
                zy = 2 * zx * zy + cy;
                zx = t;
                ++len;
                z[2 * len] = float(zx);
                z[2 * len + 1] = float(zy);
                if (zx * zx + zy * zy > 1e4) break;  // the center escapes: pixels rebase onto ref[0] when it ends
            }
            p.refLen = len;
        }
        p.pix = float(2.0 * k / double(std::max(1u, height_)));
        const double s2 = f.sre * f.sre + f.sim * f.sim;
        if (f.mini && s2 > 0 && std::isfinite(s2)) {
            // In the shader's offset units (plane units from the center): the nucleus's offset, 1 / size.
            p.mini = 1;
            p.nOff[0] = float(f.nre - cx);
            p.nOff[1] = float(f.nim - cy);
            p.invS[0] = float(f.sre / s2);
            p.invS[1] = float(-f.sim / s2);
        }
        p.glow = float(std::max(0.0, f.glow));
        fullScreen(cmd, s.tex, mandelPso_, &p, sizeof(p), nullptr, nullptr, "mandel", res, refBuf);
    }
    q.visible = true;
    q.texture = s.handle;
    q.cx = float(width_) * 0.5f;
    q.cy = float(height_) * 0.5f;
    q.w = float(width_);
    q.h = float(height_);
    q.u0 = q.v0 = 0;
    q.u1 = q.v1 = 1;
    if (res < 1.0) {
        // Sample the rendered corner, half a texel in from its far edges (no bleed from the unrendered rest).
        q.u1 = float((std::ceil(double(width_) * res) - 0.5) / double(width_));
        q.v1 = float((std::ceil(double(height_) * res) - 0.5) / double(height_));
    }
    return q;
}

}  // namespace reforce::render
