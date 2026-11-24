// reforce extension (PLAN 5L.14, docs/3d-design.md "Fractal zoom"): fractal Objects on the GPU. See FractalPipeline.hpp.
#include "render/FractalPipeline.hpp"

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

#include "render/GpuPoison.hpp"
#include "render/ShaderCompiler.hpp"

namespace reforce::render {
namespace {

constexpr const char* kFractalShaders = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct FracU {
    float4 eye;     // xyz the camera, w its distance to p (the scale everything is measured in)
    float4 right;   // xyz, w tan(fov / 2) x aspect
    float4 up;      // xyz, w tan(fov / 2)
    float4 fwd;     // xyz, w radians per render pixel
    float4 p;       // xyz the zoom's fixed point, w log2(lambda)
    float4 axis;    // xyz the zoom's rotation axis, w its angle in turns
    float4 shell;   // x R, y rho, z 1 / ln(1 / lambda), w 1 / ln(1 / piece scale)
    float4 look;    // x fill, y shade, z glow, w fog (x scale)
    float4 light;   // xyz towards the key light (world), w ambient occlusion strength
    float4 detail;  // x detail angle (Detl x pixel angle), y piece size at level 0, z max level, w far (x scale)
    int kind; int maxSteps; float2 size;
};

struct VOut { float4 position [[position]]; };

vertex VOut frac_vs(uint vid [[vertex_id]]) {
    const float2 q = float2((vid << 1) & 2, vid & 2);
    VOut o;
    o.position = float4(q * 2.0f - 1.0f, 0.0f, 1.0f);
    return o;
}

static float tetraSdf(float3 z) {
    const float m = max(max(-z.x - z.y - z.z, z.x + z.y - z.z), max(z.x - z.y + z.z, -z.x + z.y + z.z));
    return (m - 1.0f) * 0.57735027f;
}

// The Sierpinski tetrahedron at level `lev` (fractional: the two neighboring levels blended).
static float deSierpinski(float3 z, float lev) {
    const int n = int(lev);
    const float f = lev - float(n);
    float s = 1.0f, d0 = 0.0f;
    for (int i = 0; i <= n; ++i) {
        if (i == n) d0 = tetraSdf(z) * s;
        if (z.x + z.y < 0.0f) z.xy = -z.yx;
        if (z.x + z.z < 0.0f) z.xz = -z.zx;
        if (z.y + z.z < 0.0f) z.zy = -z.yz;
        z = z * 2.0f - 1.0f;
        s *= 0.5f;
    }
    return mix(d0, tetraSdf(z) * s, f);
}

// The Menger sponge (the cube [-1, 1]^3 less its crosses), in local cell coordinates so it stays precise when deep.
static float deMenger(float3 p, float lev) {
    const int n = int(lev);
    const float f = lev - float(n);
    const float3 q = abs(p) - 1.0f;
    float d = length(max(q, 0.0f)) + min(max(q.x, max(q.y, q.z)), 0.0f);
    float3 w = p - 2.0f * floor(p * 0.5f) - 1.0f;
    float s = 1.0f, d0 = d;
    for (int i = 0; i <= n; ++i) {
        if (i == n) d0 = d;
        s *= 3.0f;
        const float3 r = abs(1.0f - 3.0f * abs(w));
        const float da = max(r.x, r.y), db = max(r.y, r.z), dc = max(r.z, r.x);
        d = max(d, (min(da, min(db, dc)) - 1.0f) / s);
        const float3 v = 3.0f * w + 1.0f;
        w = v - 2.0f * floor(v * 0.5f) - 1.0f;
    }
    return mix(d0, d, f);
}

// S's distance estimate at x mapped by g^m (y = g^m(x)), in world units: the level makes the smallest pieces about
// Detl pixels at their distance from the camera; capped by rho - |y - p| (S_inf is S within rho of p).
static float deMapped(float3 x, float3 eye, float m, constant FracU& U) {
    const float3 v = x - U.p.xyz;
    const float s = exp2(m * U.p.w);  // lambda^m
    const float a = 6.2831853f * fract(m * U.axis.w);
    const float c = cos(a), sn = sin(a);
    const float3 k = U.axis.xyz;
    const float3 y = U.p.xyz + s * (v * c + cross(k, v) * sn + k * (dot(k, v) * (1.0f - c)));
    const float delta = s * U.detail.x * max(length(x - eye), 1e-3f * U.eye.w);
    const float lev = clamp(log(U.detail.y / delta) * U.shell.w, 0.0f, U.detail.z);
    const float d = U.kind == 0 ? deSierpinski(y, lev) : deMenger(y, lev);
    return min(d, U.shell.y - length(v) * s) / s;
}

// The distance to S_inf for marching (engine/FractalZoom.hpp): x mapped into the shell |y - p| in (lambda R, R],
// R = rho / 2. Capped by |x - p| instead of rho - |y - p| (a step from y may go rho - |y - p| >= |y - p|): the same
// in every shell, so the cap is continuous.
static float deInf(float3 x, float3 eye, constant FracU& U) {
    const float r = length(x - U.p.xyz);
    const float m = clamp(ceil(log(max(r, 1e-30f) / U.shell.x) * U.shell.z), -30.0f, 30.0f);
    return min(deMapped(x, eye, m, U), r);
}

// The distance for shading (normals, occlusion): S's estimate is only a bound off the surface, and it differs between
// two neighboring mappings (the box of S outside, a cross of tunnels inside), so a shell boundary would show as a ring
// in the occlusion. Here the two mappings that both lie within rho are blended smoothly across each shell (R0 =
// lambda rho), which is continuous everywhere.
static float deSmooth(float3 x, float3 eye, constant FracU& U) {
    const float r = max(length(x - U.p.xyz), 1e-30f);
    const float c = clamp(log(r / (U.shell.y * exp2(U.p.w))) * U.shell.z, -30.0f, 30.0f);
    const float m = ceil(c);
    const float f = smoothstep(0.0f, 1.0f, m - c);  // 0 at the shell's outer edge (|y - p| = R0) .. 1 at its inner
    const float hi = f < 0.999f ? deMapped(x, eye, m, U) : 0.0f;
    const float lo = f > 0.001f ? deMapped(x, eye, m - 1.0f, U) : 0.0f;
    return mix(hi, lo, f);
}

fragment float4 frac_fs(VOut in [[stage_in]], constant FracU& U [[buffer(0)]]) {
    const float2 ndc = float2(in.position.x / U.size.x * 2.0f - 1.0f, 1.0f - in.position.y / U.size.y * 2.0f);
    const float3 dir = normalize(U.fwd.xyz + ndc.x * U.right.w * U.right.xyz + ndc.y * U.up.w * U.up.xyz);
    const float3 eye = U.eye.xyz;
    const float scale = U.eye.w, pa = U.fwd.w;
    const float far = U.detail.w * scale;
    float t = 0.0f, nearMiss = 1e9f;
    bool hit = false;
    int i = 0;
    for (; i < U.maxSteps; ++i) {
        const float d = deInf(eye + dir * t, eye, U);
        const float px = pa * max(t, 1e-3f * scale);
        if (d < 0.6f * px) {
            hit = true;
            break;
        }
        nearMiss = min(nearMiss, d / px);
        t += 0.95f * d;
        if (t > far) break;
    }
    const float glow = U.look.z;
    float lum = 0.0f, a = 0.0f;
    if (hit) {
        const float3 x = eye + dir * t;
        const float h = pa * t;
        // The gradient from four taps on a tetrahedron set in the camera's frame (it turns with the camera, so the
        // normals are invariant under the zoom step's rotation, which need not map world-axis taps onto themselves).
        const float3 r = U.right.xyz, u = U.up.xyz, b = -U.fwd.xyz;
        const float3 k0 = r - u - b, k1 = -r - u + b, k2 = -r + u - b, k3 = r + u + b;
        const float3 n = normalize(k0 * deSmooth(x + k0 * h, eye, U) + k1 * deSmooth(x + k1 * h, eye, U) +
                                   k2 * deSmooth(x + k2 * h, eye, U) + k3 * deSmooth(x + k3 * h, eye, U));
        // Ambient occlusion: five samples along the normal out to a share of the hit distance.
        float occ = 0.0f, wgt = 1.0f;
        for (int k = 1; k <= 5; ++k) {
            const float hh = 0.03f * t * float(k);
            occ += wgt * saturate(1.0f - deSmooth(x + n * hh, eye, U) / hh);
            wgt *= 0.75f;
        }
        const float ao = mix(1.0f, saturate(1.0f - 0.42f * occ), U.light.w);
        // A key light from the camera's upper left, a soft headlight (faces towards the camera never go black), a rim.
        const float key = max(0.0f, dot(n, U.light.xyz));
        const float facing = saturate(dot(n, -dir));
        const float rim = pow(1.0f - facing, 3.0f);
        const float lit = ao * mix(1.0f, 0.22f + 0.8f * key + 0.3f * facing + 0.3f * rim, U.look.y);
        lum = saturate(U.look.x + (1.0f - U.look.x) * pow(saturate(lit), 0.85f) + glow * (0.35f + 0.65f * rim) * ao);
        // Fog by distance in units of the camera's distance to p, cut off softly before `far`.
        a = exp(-t / (U.look.w * scale)) * saturate((far - t) / (0.2f * far));
    }
    // Near misses glow with the beat (a halo along the edges), over whatever is behind.
    const float halo = glow * 0.5f * exp(-nearMiss / 2.5f) * (hit ? 0.0f : 1.0f);
    return float4(lum * a + halo * (1.0f - a), a + halo * (1.0f - a), 0.0f, 0.0f);
}
)MSL";

struct FracU {
    float eye[4], right[4], up[4], fwd[4], p[4], axis[4], shell[4], look[4], light[4], detail[4];
    std::int32_t kind, maxSteps;
    float size[2];
};
static_assert(sizeof(FracU) == 176, "FracU must match the MSL struct");

}  // namespace

FractalPipeline::FractalPipeline(SolidPipeline& solids) : solids_(solids) {
    if (solids.maxLive() < 1) throw std::runtime_error("no solid slots");
    MTL::Texture* t0 = solids.target(0);
    device_ = t0->device()->retain();
    width_ = std::uint32_t(t0->width());
    height_ = std::uint32_t(t0->height());
    MTL::Library* lib = compileLibrary(device_, kFractalShaders, "reforce.fractal");
    MTL::Function* v = loadFunction(lib, "frac_vs");
    MTL::Function* f = loadFunction(lib, "frac_fs");
    MTL::RenderPipelineDescriptor* d = MTL::RenderPipelineDescriptor::alloc()->init();
    d->setLabel(NS::String::string("fractal", NS::UTF8StringEncoding));
    d->setVertexFunction(v);
    d->setFragmentFunction(f);
    d->colorAttachments()->object(0)->setPixelFormat(t0->pixelFormat());
    NS::Error* e = nullptr;
    pso_ = device_->newRenderPipelineState(d, &e);
    d->release();
    v->release();
    f->release();
    lib->release();
    if (!pso_)
        throw std::runtime_error(std::string("pipeline 'fractal' failed: ") +
                                 (e ? e->localizedDescription()->utf8String() : "?"));
}

FractalPipeline::~FractalPipeline() {
    if (pso_) pso_->release();
    device_->release();
}

std::array<int, 2> FractalPipeline::renderSize(float res) const {
    const float r = std::isfinite(res) ? std::clamp(res, 0.25f, 1.0f) : 0.5f;
    return {std::max(1, int(std::lround(double(width_) * r))), std::max(1, int(std::lround(double(height_) * r)))};
}

SolidQuad FractalPipeline::encode(MTL::CommandBuffer* cmd, const FractalDraw& d) {
    SolidQuad q;
    if (!cmd || d.slot < 0 || d.slot >= solids_.maxLive()) return q;
    const auto [w, h] = renderSize(d.res);
    const engine::FractalCamera& c = d.cam;
    const double tanHalf = std::tan(std::clamp(c.fov, 10.0, 150.0) * M_PI / 360.0);
    const double aspect = double(width_) / double(height_);
    FracU u{};
    auto put = [](float* dst, const engine::FVec3& v, double w4) {
        dst[0] = float(v[0]);
        dst[1] = float(v[1]);
        dst[2] = float(v[2]);
        dst[3] = float(w4);
    };
    put(u.eye, c.eye, c.scale);
    put(u.right, c.right, tanHalf * aspect);
    put(u.up, c.up, tanHalf);
    put(u.fwd, c.forward, 2.0 * tanHalf / double(h));
    const double lam = std::clamp(d.g.lambda, 1e-3, 0.99);
    put(u.p, d.g.p, std::log2(lam));
    put(u.axis, d.g.axis, d.g.angle / (2.0 * M_PI));
    const double pieceScale = engine::fractalPieceScale(d.kind);
    u.shell[0] = float(d.g.shell());
    u.shell[1] = float(d.g.rho);
    u.shell[2] = float(1.0 / std::log(1.0 / lam));
    u.shell[3] = float(1.0 / std::log(1.0 / pieceScale));
    auto clampf = [](float v, float lo, float hi, float fb) { return std::isfinite(v) ? std::clamp(v, lo, hi) : fb; };
    u.look[0] = clampf(d.fill, 0.0f, 1.0f, 0.06f);
    u.look[1] = clampf(d.shade, 0.0f, 1.0f, 0.85f);
    u.look[2] = clampf(d.glow, 0.0f, 4.0f, 0.0f);
    const float fog = clampf(d.fog, 0.2f, 50.0f, 3.0f);
    u.look[3] = fog;
    // The key light from camera space to world (x right, y up, z back towards the viewer).
    const double lx = d.light[0], ly = d.light[1], lz = d.light[2];
    engine::FVec3 L{c.right[0] * lx + c.up[0] * ly - c.forward[0] * lz, c.right[1] * lx + c.up[1] * ly - c.forward[1] * lz,
                    c.right[2] * lx + c.up[2] * ly - c.forward[2] * lz};
    const double ll = std::sqrt(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
    if (!(ll > 1e-9) || !std::isfinite(ll)) L = {-c.forward[0], -c.forward[1], -c.forward[2]};
    else L = {L[0] / ll, L[1] / ll, L[2] / ll};
    put(u.light, L, clampf(d.ao, 0.0f, 1.0f, 1.0f));
    u.detail[0] = float(clampf(d.detail, 0.25f, 16.0f, 1.0f) * 2.0 * tanHalf / double(h));
    u.detail[1] = 2.0f;
    u.detail[2] = float(engine::fractalMaxLevel(d.kind));
    u.detail[3] = 4.0f * fog;
    u.kind = d.kind == engine::FractalKind::Sierpinski ? 0 : 1;
    u.maxSteps = std::clamp(d.maxSteps, 8, 400);
    u.size[0] = float(w);
    u.size[1] = float(h);

    MTL::RenderPassDescriptor* pass = MTL::RenderPassDescriptor::renderPassDescriptor();
    auto* a = pass->colorAttachments()->object(0);
    a->setTexture(solids_.target(d.slot));
    a->setLoadAction(MTL::LoadActionDontCare);
    a->setStoreAction(MTL::StoreActionStore);
    pass->setRenderTargetWidth(NS::UInteger(w));
    pass->setRenderTargetHeight(NS::UInteger(h));
    MTL::RenderCommandEncoder* enc = makeRenderEncoder(cmd, pass);
    enc->setLabel(NS::String::string("fractal", NS::UTF8StringEncoding));
    enc->setViewport(MTL::Viewport{0.0, 0.0, double(w), double(h), 0.0, 1.0});
    enc->setRenderPipelineState(pso_);
    enc->setFragmentBytes(&u, sizeof(u), 0);
    enc->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(3));
    enc->endEncoding();

    q.visible = true;
    q.texture = solids_.spriteTexture(d.slot);
    q.cx = float(width_) * 0.5f;
    q.cy = float(height_) * 0.5f;
    q.w = float(width_);
    q.h = float(height_);
    // The rendered sub-rectangle, inset by half a texel so bilinear filtering never reads past it.
    q.u0 = 0.5f / float(width_);
    q.v0 = 0.5f / float(height_);
    q.u1 = (float(w) - 0.5f) / float(width_);
    q.v1 = (float(h) - 0.5f) / float(height_);
    return q;
}

}  // namespace reforce::render
