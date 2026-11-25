#include "render/RainPipeline.hpp"

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

#include "render/GpuPoison.hpp"
#include "render/ShaderCompiler.hpp"
#include "render/SolidPipeline.hpp"

namespace reforce::render {
namespace {

constexpr const char* kRainShaders = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct FullOut { float4 position [[position]]; float2 uv; };
vertex FullOut rain_full_vs(uint vid [[vertex_id]]) {
    const float2 p = float2((vid << 1) & 2, vid & 2);
    FullOut o;
    o.position = float4(p * 2.0f - 1.0f, 0.0f, 1.0f);
    o.uv = float2(p.x, 1.0f - p.y);
    return o;
}

// ---- puddles: the same value noise as engine::rain::puddle
static uint lowbias32(uint x) {
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return x;
}
static float lattice(int ix, int iy, uint seed) {
    const uint h = lowbias32(uint(ix) * 0x8da6b343u ^ uint(iy) * 0xd8163841u ^ seed * 0xcb1ab31fu);
    return float(h) * (1.0f / 4294967296.0f);
}
static float vnoise(float2 p, uint seed) {
    const float2 f = floor(p);
    const int ix = int(f.x), iy = int(f.y);
    float2 u = p - f;
    u = u * u * (3.0f - 2.0f * u);
    const float a = lattice(ix, iy, seed), b = lattice(ix + 1, iy, seed);
    const float c = lattice(ix, iy + 1, seed), d = lattice(ix + 1, iy + 1, seed);
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}
// One creeping octave: crossfading from one seeded layer to the next every `period` seconds of creep time.
static float creep(float2 p, float t, float freq, float period, float2 o, uint seed, uint salt) {
    const float s = t / period, i = floor(s);
    float f = s - i;
    f = f * f * (3.0f - 2.0f * f);
    const uint layer = uint(int(i));
    const float a = vnoise(p * freq + o, seed + layer * 0x9e3779b9u + salt);
    const float b = vnoise(p * freq + o, seed + (layer + 1u) * 0x9e3779b9u + salt);
    return mix(a, b, f);
}
static float puddleN(float2 p, uint seed, float t) {
    return 0.65f * vnoise(p * 2.2f, seed) + 0.35f * vnoise(p * 5.1f + float2(17.0f, 3.0f), seed) +
           0.16f * (creep(p, t, 3.4f, 9.0f, float2(5.3f, 11.7f), seed, 0x2545f491u) - 0.5f) +
           0.05f * (creep(p, t, 7.9f, 4.0f, float2(29.1f, 7.3f), seed, 0x68e31da4u) - 0.5f);
}
static float puddleThr(float wet) { return 0.5f + (0.5f - wet) * 0.6f; }
static float puddleOf(float n, float wet) {
    if (wet <= 0.001f) return 0.0f;
    if (wet >= 0.999f) return 1.0f;
    const float thr = puddleThr(wet);
    return smoothstep(thr - 0.025f, thr + 0.025f, n);
}

// ---- floor: the puddles' cover at the sim size, once per step (the wave substeps read it)
struct MaskP { float rows; float wet; uint seed; float t; };
fragment float4 rain_mask_fs(FullOut in [[stage_in]], constant MaskP& P [[buffer(0)]]) {
    const float2 p = in.position.xy / P.rows;
    return float4(puddleOf(puddleN(p, P.seed, P.t), P.wet), 0.0f, 0.0f, 0.0f);
}

// ---- the frame copy (any size into level 0 of the mipmapped copy)
fragment float4 rain_copy_fs(FullOut in [[stage_in]], texture2d<float> src [[texture(0)]]) {
    constexpr sampler s(filter::linear, address::clamp_to_edge);
    return src.sample(s, in.uv);
}

// ---- floor: one wave-equation substep (engine::rain::waveStep, WaveGrid)
struct WaveP { int2 size; float c2; float b; float nu; float damp; float wet; uint seed; float rows; int count;
               float sponge; float pad; float4 imp[48]; };
static float rd(texture2d<float> t, int2 q, int2 m) { return t.read(uint2(clamp(q, int2(0), m))).r; }
fragment float4 rain_wave_fs(FullOut in [[stage_in]], constant WaveP& P [[buffer(0)]],
                             texture2d<float> cur [[texture(0)]], texture2d<float> prev [[texture(1)]],
                             texture2d<float> mask [[texture(2)]]) {
    const int2 p = int2(in.position.xy);
    const int2 m = P.size - 1;
    const float h = rd(cur, p, m);
    const float e = rd(cur, p + int2(1, 0), m), w = rd(cur, p - int2(1, 0), m);
    const float s = rd(cur, p + int2(0, 1), m), n = rd(cur, p - int2(0, 1), m);
    const float l0 = e + w + s + n - 4.0f * h;
    const float le = rd(cur, p + int2(2, 0), m) + h + rd(cur, p + int2(1, 1), m) + rd(cur, p + int2(1, -1), m) - 4.0f * e;
    const float lw = h + rd(cur, p - int2(2, 0), m) + rd(cur, p + int2(-1, 1), m) + rd(cur, p + int2(-1, -1), m) - 4.0f * w;
    const float ls = rd(cur, p + int2(1, 1), m) + rd(cur, p + int2(-1, 1), m) + rd(cur, p + int2(0, 2), m) + h - 4.0f * s;
    const float ln = rd(cur, p + int2(1, -1), m) + rd(cur, p + int2(-1, -1), m) + h + rd(cur, p - int2(0, 2), m) - 4.0f * n;
    const float l2 = le + lw + ls + ln - 4.0f * l0;
    const float hp = rd(prev, p, m);
    const float lv = (e - rd(prev, p + int2(1, 0), m)) + (w - rd(prev, p - int2(1, 0), m)) +
                     (s - rd(prev, p + int2(0, 1), m)) + (n - rd(prev, p - int2(0, 1), m)) - 4.0f * (h - hp);
    float v = (2.0f * h - hp + P.c2 * l0 - P.b * l2 + P.nu * lv) * P.damp;
    // The dry glass between puddles holds the water at rest, so ripples bounce off a puddle's edge.
    if (P.wet < 0.999f) v *= mask.read(uint2(clamp(p, int2(0), m))).r;
    // The frame's edges absorb (a sponge).
    const float edge = float(min(min(p.x, m.x - p.x), min(p.y, m.y - p.y)));
    if (edge < P.sponge) {
        const float k = 1.0f - edge / P.sponge;
        v *= 1.0f - 0.12f * k * k;
    }
    for (int i = 0; i < P.count; ++i) {
        const float4 d = P.imp[i];  // x, y, radius (cells), amplitude (cells)
        const float2 q = float2(p) + 0.5f - d.xy;
        const float q2 = dot(q, q) / (d.z * d.z);
        v -= d.w * (1.0f - q2) * exp(-q2);  // a crater and its rim: no net push
    }
    return float4(v, 0.0f, 0.0f, 0.0f);
}

// ---- window: the fog (r) and the wet film (g), eased and wiped
struct FogP { float rows; float target; float refog; float film; int count; float pad0; float pad1; float pad2; float4 seg[192]; };
static float segDist(float2 p, float2 a, float2 b) {
    const float2 ab = b - a;
    const float t = clamp(dot(p - a, ab) / max(dot(ab, ab), 1e-12f), 0.0f, 1.0f);
    return length(p - (a + t * ab));
}
fragment float4 rain_fog_fs(FullOut in [[stage_in]], constant FogP& P [[buffer(0)]],
                            texture2d<float> prev [[texture(0)]]) {
    const float2 v = prev.read(uint2(in.position.xy)).rg;
    float fog = v.r + (P.target - v.r) * P.refog;
    float film = v.g * P.film;
    const float2 q = in.position.xy / P.rows;  // frame heights
    for (int i = 0; i < P.count; ++i) {
        const float4 s = P.seg[2 * i];
        const float r = P.seg[2 * i + 1].x;
        const float d = segDist(q, s.xy, s.zw);
        const float cov = 1.0f - smoothstep(r * 0.55f, r * 1.05f, d);
        fog *= 1.0f - cov;
        film = max(film, cov);
    }
    return float4(saturate(fog), saturate(film), 0.0f, 0.0f);
}

// ---- drops: spherical caps (r: height in px) and a halo outside the contact line (g), max-blended
struct DropI { float2 c; float r; float k; float2 axis; float stretch; float tail; };
struct DropOut { float4 position [[position]]; float2 local; float r; float k; float2 axis; float stretch; float tail; };
vertex DropOut rain_drop_vs(uint vid [[vertex_id]], uint iid [[instance_id]], constant DropI* D [[buffer(0)]],
                            constant float2& size [[buffer(1)]]) {
    const DropI d = D[iid];
    const float2 corner[6] = {float2(-1, -1), float2(1, -1), float2(-1, 1), float2(-1, 1), float2(1, -1), float2(1, 1)};
    const float s = max(d.stretch, 1.0f / max(d.stretch, 0.2f));
    const float ext = d.r * s * (1.0f + d.tail) * 1.5f + 2.0f;
    const float2 local = corner[vid] * ext;
    const float2 px = d.c + local;
    DropOut o;
    o.position = float4(px.x / size.x * 2.0f - 1.0f, 1.0f - px.y / size.y * 2.0f, 0.0f, 1.0f);
    o.local = local;
    o.r = d.r;
    o.k = d.k;
    o.axis = d.axis;
    o.stretch = d.stretch;
    o.tail = d.tail;
    return o;
}
fragment float4 rain_drop_fs(DropOut in [[stage_in]]) {
    const float u = dot(in.local, in.axis), v = dot(in.local, float2(-in.axis.y, in.axis.x));
    const float s = clamp(in.stretch, 0.3f, 4.0f);
    const float along = in.r * s * (u < 0.0f ? 1.0f + in.tail : 1.0f);
    float across = in.r / sqrt(s);
    if (u < 0.0f) across *= 1.0f - 0.45f * in.tail * saturate(-u / along);
    const float q2 = (u / along) * (u / along) + (v / across) * (v / across);
    const float halo = in.k < 0.0f ? 0.0f : 1.0f - smoothstep(1.0f, 1.5f, sqrt(q2));  // (k < 0: in the air, none)
    if (q2 >= 1.0f) return float4(0.0f, halo, 0.0f, 0.0f);
    const float k = max(abs(in.k), 0.05f);
    const float Rs = (1.0f + k * k) / (2.0f * k);
    const float hN = (sqrt(max(Rs * Rs - q2, 0.0f)) - (Rs - k)) / k;
    return float4(in.r * k * max(hN, 0.0f), halo, 0.0f, 0.0f);
}

// ---- the water over the frame
struct CompP {
    float2 size;      // output px
    float2 simTexel;  // 1 / sim size
    float depthPx;    // distance to the visuals, output px
    float fog;
    float spec;
    float rim;
    float opacity;
    int mode;         // 0 window, 1 floor
    float wet;
    uint seed;
    float aspect;
    float puddleDepth;  // output px
    int opaque;
    float waveGain;     // wave cells -> slope
    float pudl;         // the puddles' visibility (floor)
    float creepT;       // the puddles' creep time (engine::rain::Sim::puddleTime)
};

static float3 sampleFrame(texture2d<float> t, float2 uv, float2 dx, float2 dy) {
    constexpr sampler tri(filter::linear, mip_filter::linear, address::clamp_to_edge);
    return t.sample(tri, uv, gradient2d(dx, dy)).rgb;
}

fragment float4 rain_composite_fs(FullOut in [[stage_in]], constant CompP& C [[buffer(0)]],
                                  texture2d<float> frame [[texture(0)]], texture2d<float> blur [[texture(1)]],
                                  texture2d<float> drops [[texture(2)]], texture2d<float> wave [[texture(3)]],
                                  texture2d<float> fogT [[texture(4)]]) {
    constexpr sampler lin(filter::linear, address::clamp_to_edge);
    const float2 uv = in.uv;
    const int2 pi = int2(in.position.xy);
    const int2 dm = int2(drops.get_width(), drops.get_height()) - 1;
    // Slopes (dimensionless, y down): the drops (px heights; Sobel, the tilt over 3 x 3 texels, so a drop a few pixels
    // wide still reads round), the floor's waves and puddle edges, the window's wet film. (The drop target is the
    // render size, like `output`.)
    const float2 dc = drops.read(uint2(clamp(pi, int2(0), dm))).rg;
    float hn[3][3];
    for (int j = -1; j <= 1; ++j)
        for (int i = -1; i <= 1; ++i) hn[j + 1][i + 1] = drops.read(uint2(clamp(pi + int2(i, j), int2(0), dm))).r;
    float2 g = float2((hn[0][2] + 2.0f * hn[1][2] + hn[2][2]) - (hn[0][0] + 2.0f * hn[1][0] + hn[2][0]),
                      (hn[2][0] + 2.0f * hn[2][1] + hn[2][2]) - (hn[0][0] + 2.0f * hn[0][1] + hn[0][2])) / 8.0f;
    const float2 gDrop = g;
    const float water = saturate(dc.r * 1.5f);
    float fogAmt = 0.0f;
    float pudIn = 0.0f, pudRim = 0.0f, pudLine = 0.0f;  // the floor's puddles: inside, bright rim, dark contact line
    if (C.mode == 1) {
        const float2 t = C.simTexel;
        const float wx = wave.sample(lin, uv + float2(t.x, 0)).r - wave.sample(lin, uv - float2(t.x, 0)).r;
        const float wy = wave.sample(lin, uv + float2(0, t.y)).r - wave.sample(lin, uv - float2(0, t.y)).r;
        float2 gw = 0.5f * C.waveGain * float2(wx, wy);
        const float lw = length(gw);
        if (lw > 0.7f) gw *= 0.7f / lw;  // (the steepest crests of a big strike: no darker than water gets)
        g += gw;
        if (C.wet > 0.001f && C.wet < 0.999f) {
            const float e = 1.5f / C.size.y;
            const float2 p = float2(uv.x * C.aspect, uv.y);
            const float nE = puddleN(p + float2(e, 0), C.seed, C.creepT), nW = puddleN(p - float2(e, 0), C.seed, C.creepT);
            const float nS = puddleN(p + float2(0, e), C.seed, C.creepT), nN = puddleN(p - float2(0, e), C.seed, C.creepT);
            const float mx = puddleOf(nE, C.wet) - puddleOf(nW, C.wet);
            const float my = puddleOf(nS, C.wet) - puddleOf(nN, C.wet);
            g += C.puddleDepth * float2(mx, my) / 3.0f;
            if (C.pudl > 0.0f) {
                // The distance to the water's edge in output px (+ inside): the noise over its gradient, so the rim
                // and the contact line are a couple of pixels wide whatever the puddle's size.
                const float thr = puddleThr(C.wet), n0 = 0.25f * (nE + nW + nS + nN);
                const float2 gn = float2(nE - nW, nS - nN) / (2.0f * e);
                const float s = C.size.y / 1080.0f;
                const float dpx = (n0 - thr) / max(length(gn), 1e-4f) * C.size.y;
                const float a = (dpx - 1.5f * s) / (1.3f * s), b = (dpx + 2.0f * s) / (1.8f * s);
                pudRim = exp(-a * a);
                pudLine = exp(-b * b);
                pudIn = smoothstep(0.0f, 4.0f * s, dpx);
            }
        }
    } else {
        const float2 f = fogT.sample(lin, uv).rg;
        const float2 t = C.simTexel;
        const float fx = fogT.sample(lin, uv + float2(t.x, 0)).g - fogT.sample(lin, uv - float2(t.x, 0)).g;
        const float fy = fogT.sample(lin, uv + float2(0, t.y)).g - fogT.sample(lin, uv - float2(0, t.y)).g;
        g += 0.06f * float2(fx, fy);  // the streak's wet film: a faint lens at its edges
        fogAmt = C.fog * f.r * (1.0f - water) * (1.0f - 0.7f * f.g);
    }
    const float slope = length(g);
    const float3 N = normalize(float3(-g, 1.0f));
    // The view ray into the water (n 1.33), out through the flat glass into air, on to the visuals Dpth behind.
    const float3 V = float3(0.0f, 0.0f, -1.0f);
    float3 col;
    // Per channel: a touch of dispersion (blue bends more).
    const float etas[3] = {1.331f, 1.334f, 1.339f};
    float2 offR = 0.0f, offG = 0.0f, offB = 0.0f;
    bool tir = false;
    for (int c = 0; c < 3; ++c) {
        const float3 t1 = refract(V, N, 1.0f / etas[c]);
        const float3 t2 = refract(normalize(t1), float3(0.0f, 0.0f, 1.0f), etas[c]);
        float2 o;
        if (dot(t2, t2) < 1e-6f) {
            tir = true;
            o = normalize(g + 1e-6f) * C.size.y;  // total internal reflection: no way through
        } else {
            o = t2.xy / max(-t2.z, 0.08f) * C.depthPx;
        }
        o = clamp(o, -C.size.y, C.size.y);
        if (c == 0) offR = o; else if (c == 1) offG = o; else offB = o;
    }
    const float2 uvG = uv + offG / C.size;
    const float2 ddx = dfdx(uvG), ddy = dfdy(uvG);
    const float3 sR = sampleFrame(frame, uv + offR / C.size, ddx, ddy);
    const float3 sG = sampleFrame(frame, uvG, ddx, ddy);
    const float3 sB = sampleFrame(frame, uv + offB / C.size, ddx, ddy);
    float3 refr = float3(sR.r, sG.g, sB.b);
    // Rays that leave the frame see the dark room.
    const float2 u2 = uvG;
    const float beyond = max(max(-u2.x, u2.x - 1.0f), max(-u2.y, u2.y - 1.0f));
    refr *= mix(0.06f, 1.0f, 1.0f - smoothstep(0.0f, 0.12f, beyond));
    if (tir) refr *= 0.1f;
    // Fresnel, a dim environment, the highlight from a soft light at the upper left (a sharp core and a broad sheen).
    const float cosI = saturate(N.z);
    // (Only what the water's tilt adds: flat water, like the bare glass, leaves the frame as it is.)
    const float F = 0.98f * pow(1.0f - cosI, 5.0f);
    const float3 env = C.mode == 1 ? float3(0.30f, 0.32f, 0.36f) : float3(0.10f, 0.11f, 0.13f);
    const float3 R = reflect(V, N);
    // (The floor's light is higher: ripples are shallow.)
    const float3 L = C.mode == 1 ? normalize(float3(-0.28f, -0.36f, 0.89f)) : normalize(float3(-0.42f, -0.55f, 0.72f));
    const float rl = saturate(dot(R, L));
    const float hl = C.spec * (C.mode == 1 ? 2.0f * pow(rl, 300.0f) + 0.25f * pow(rl, 30.0f)
                                            : 2.5f * pow(rl, 120.0f) + 0.3f * pow(rl, 16.0f)) *
                     smoothstep(0.02f, 0.12f, slope);
    // The contact line: steep glass-water edges darken (rays there skim off into the room), and a soft halo just
    // outside each drop (Rim), so drops read over white as well as black.
    const float edge = C.rim * smoothstep(0.45f, 1.6f, length(gDrop)) * 0.75f;
    col = refr * (1.0f - F) * (1.0f - edge) + F * env + hl;
    if (C.pudl > 0.0f) {
        // A puddle: a little deeper and cooler inside, a bright meniscus on its edge, a dark contact line just outside.
        const float k = min(C.pudl, 1.0f), over = C.pudl;
        col = mix(col, col * 0.84f + float3(0.015f, 0.025f, 0.045f), k * pudIn);
        col *= 1.0f - min(0.6f, 0.42f * over * pudLine);
        col += min(0.6f, 0.32f * over * pudRim) * float3(0.92f, 0.96f, 1.0f);
    }
    const float halo = C.rim * dc.g * (1.0f - water) * (1.0f - pudIn) * 0.22f;  // (no drop's halo on open water)
    col *= 1.0f - halo;
    // Window fog: the frosted copy, a little milky, where no water clears it.
    const float4 orig = frame.read(uint2(clamp(pi, int2(0), int2(frame.get_width(), frame.get_height()) - 1)));
    if (fogAmt > 0.0f) {
        const float3 frost = blur.sample(lin, uv).rgb * 0.86f + 0.045f;
        col = mix(col, frost * (1.0f - halo), saturate(fogAmt));
    }
    const float3 rgb = mix(orig.rgb, saturate(col), C.opacity);
    float a = orig.a;
    if (C.opaque != 0) a = max(a, C.opacity * (C.mode == 1 ? 1.0f : max(water, saturate(fogAmt))));
    return float4(rgb, a);
}
)MSL";

struct WaveP {
    int32_t size[2];
    float c2, b, nu, damp, wet;
    uint32_t seed;
    float rows;
    int32_t count;
    float sponge, pad;
    float imp[48][4];
};
static_assert(sizeof(WaveP) == 48 + 48 * 16, "WaveP must match the MSL struct");
static_assert(engine::rain::kMaxImpulses <= 48);
struct MaskP {
    float rows, wet;
    uint32_t seed;
    float t;
};
static_assert(sizeof(MaskP) == 16, "MaskP must match the MSL struct");
struct FogP {
    float rows, target, refog, film;
    int32_t count;
    float pad[3];
    float seg[192][4];
};
static_assert(sizeof(FogP) == 32 + 192 * 16, "FogP must match the MSL struct");
static_assert(engine::rain::kMaxWipes * 2 <= 192);
struct CompP {
    float size[2], simTexel[2];
    float depthPx, fog, spec, rim, opacity;
    int32_t mode;
    float wet;
    uint32_t seed;
    float aspect, puddleDepth;
    int32_t opaque;
    float waveGain;
    float pudl, creepT;
};
static_assert(sizeof(CompP) == 72, "CompP must match the MSL struct");

MTL::RenderPipelineState* makePso(MTL::Device* dev, MTL::Library* lib, const char* vs, const char* fs,
                                  MTL::PixelFormat fmt, bool maxBlend, const char* label) {
    MTL::Function* v = loadFunction(lib, vs);
    MTL::Function* f = loadFunction(lib, fs);
    MTL::RenderPipelineDescriptor* d = MTL::RenderPipelineDescriptor::alloc()->init();
    d->setLabel(NS::String::string(label, NS::UTF8StringEncoding));
    d->setVertexFunction(v);
    d->setFragmentFunction(f);
    auto* a = d->colorAttachments()->object(0);
    a->setPixelFormat(fmt);
    if (maxBlend) {
        a->setBlendingEnabled(true);
        a->setRgbBlendOperation(MTL::BlendOperationMax);
        a->setAlphaBlendOperation(MTL::BlendOperationMax);
        a->setSourceRGBBlendFactor(MTL::BlendFactorOne);
        a->setDestinationRGBBlendFactor(MTL::BlendFactorOne);
        a->setSourceAlphaBlendFactor(MTL::BlendFactorOne);
        a->setDestinationAlphaBlendFactor(MTL::BlendFactorOne);
    }
    NS::Error* e = nullptr;
    MTL::RenderPipelineState* p = dev->newRenderPipelineState(d, &e);
    d->release();
    v->release();
    f->release();
    if (!p)
        throw std::runtime_error(std::string("pipeline '") + label +
                                 "' failed: " + (e ? e->localizedDescription()->utf8String() : "?"));
    return p;
}

MTL::Texture* makeTarget(MTL::Device* dev, MTL::PixelFormat fmt, uint32_t w, uint32_t h, bool mips, const char* label) {
    MTL::TextureDescriptor* td = MTL::TextureDescriptor::texture2DDescriptor(fmt, w, h, mips);
    td->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
    td->setStorageMode(MTL::StorageModePrivate);
    MTL::Texture* t = makeTexture(dev, td);
    if (!t) throw std::runtime_error(std::string(label) + " allocation failed");
    t->setLabel(NS::String::string(label, NS::UTF8StringEncoding));
    return t;
}

MTL::RenderCommandEncoder* pass(MTL::CommandBuffer* cmd, MTL::Texture* target, MTL::LoadAction load,
                                const char* label, const MTL::ClearColor& clear = MTL::ClearColor(0, 0, 0, 0)) {
    MTL::RenderPassDescriptor* rp = MTL::RenderPassDescriptor::renderPassDescriptor();
    auto* a = rp->colorAttachments()->object(0);
    a->setTexture(target);
    a->setLevel(0);
    a->setLoadAction(load);
    a->setClearColor(clear);
    a->setStoreAction(MTL::StoreActionStore);
    MTL::RenderCommandEncoder* enc = makeRenderEncoder(cmd, rp);
    enc->setLabel(NS::String::string(label, NS::UTF8StringEncoding));
    return enc;
}

constexpr double kPi = 3.141592653589793;

}  // namespace

RainDropGpu rainDropGpu(const engine::rain::Drop& d, double contactDegrees, double H) {
    RainDropGpu g{};
    // Aspect is applied by the caller's x (frame heights x H = px on both axes).
    g.cx = float(d.x * H);
    g.cy = float(d.y * H);
    double r = d.r * H;
    double k = std::tan(std::clamp(contactDegrees, 20.0, 85.0) * kPi / 360.0);  // cap height / radius
    // A fresh drop squashes flat and recoils (the first ~80 ms).
    if (d.age < 0.12) {
        const double e = std::exp(-d.age / 0.03);
        r *= 1.0 + 0.22 * e * std::cos(d.age * 2 * kPi * 14.0);
        k *= 1.0 - 0.45 * e;
    }
    double ax = 0, ay = 1, stretch = 1, tail = 0;
    if (d.sliding || d.stretch > 1.02) {
        const double v = std::hypot(d.vx, d.vy);
        if (v > 1e-6) {
            ax = d.vx / v;
            ay = d.vy / v;
        }
        stretch = d.stretch;
        tail = std::clamp((d.stretch - 1.0) * 1.3, 0.0, 1.0);
    }
    // A merge wobbles along the merge axis: the bigger of the two deformations wins.
    if (d.wobble > 0 && d.wobbleT < 1.5) {
        const double rpx = std::max(0.5, r);
        const double f = std::clamp(9.0 * std::pow(6.0 / rpx, 0.5), 3.0, 25.0);  // smaller drops ring faster
        const double w = d.wobble * std::exp(-d.wobbleT / 0.16) * std::cos(2 * kPi * f * d.wobbleT);
        if (std::fabs(w) > std::fabs(stretch - 1.0)) {
            ax = std::cos(d.wobbleAxis);
            ay = std::sin(d.wobbleAxis);
            stretch = 1.0 + w;
            tail = 0;
        }
    }
    // A crown droplet in the air: a small sphere above the water, not a bead on the glass: no contact line, no halo
    // (k < 0 tells the splat), and a gentler lens, so it never reads as a bead sitting on a puddle.
    const bool airborne = d.flight > 0;
    if (airborne) k = 0.55;
    g.r = float(std::max(1.2, r));
    g.k = float(airborne ? -k : k);
    g.ax = float(ax);
    g.ay = float(ay);
    g.stretch = float(std::clamp(stretch, 0.4, 2.5));
    g.tail = float(tail);
    return g;
}

RainPipeline::RainPipeline(SolidPipeline& solids) : solids_(solids) {
    MTL::Texture* half = solids.halfSharp();
    if (!half) throw std::runtime_error("no glass frost textures");
    device_ = half->device()->retain();
    width_ = std::max<uint32_t>(1, uint32_t(half->width()) * 2);
    height_ = std::max<uint32_t>(1, uint32_t(half->height()) * 2);
    if (solids.maxLive() > 0) {
        width_ = uint32_t(solids.target(0)->width());
        height_ = uint32_t(solids.target(0)->height());
    }
    rows_ = int(std::clamp<uint32_t>(height_ / 2, 16, 540));
    cols_ = std::max(16, int(std::lround(double(rows_) * double(width_) / double(height_))));

    MTL::Library* lib = compileLibrary(device_, kRainShaders, "reforce.rain");
    copyPso_ = makePso(device_, lib, "rain_full_vs", "rain_copy_fs", MTL::PixelFormatBGRA8Unorm, false, "rain copy");
    wavePso_ = makePso(device_, lib, "rain_full_vs", "rain_wave_fs", MTL::PixelFormatR32Float, false, "rain waves");
    maskPso_ = makePso(device_, lib, "rain_full_vs", "rain_mask_fs", MTL::PixelFormatR16Float, false, "rain puddles");
    fogPso_ = makePso(device_, lib, "rain_full_vs", "rain_fog_fs", MTL::PixelFormatRG32Float, false, "rain fog");
    dropPso_ = makePso(device_, lib, "rain_drop_vs", "rain_drop_fs", MTL::PixelFormatRG16Float, true, "rain drops");
    compositePso_ = makePso(device_, lib, "rain_full_vs", "rain_composite_fs", MTL::PixelFormatBGRA8Unorm, false,
                            "rain composite");
    lib->release();

    copy_ = makeTarget(device_, MTL::PixelFormatBGRA8Unorm, width_, height_, true, "rain frame copy");
    drops_ = makeTarget(device_, MTL::PixelFormatRG16Float, width_, height_, false, "rain drops");
    for (auto& w : wave_) w = makeTarget(device_, MTL::PixelFormatR32Float, uint32_t(cols_), uint32_t(rows_), false, "rain waves");
    mask_ = makeTarget(device_, MTL::PixelFormatR16Float, uint32_t(cols_), uint32_t(rows_), false, "rain puddles");
    for (auto& f : fog_) f = makeTarget(device_, MTL::PixelFormatRG32Float, uint32_t(cols_), uint32_t(rows_), false, "rain fog");
}

RainPipeline::~RainPipeline() {
    for (auto* p : {copyPso_, wavePso_, maskPso_, fogPso_, dropPso_, compositePso_})
        if (p) p->release();
    for (auto* t : {copy_, drops_, mask_, wave_[0], wave_[1], wave_[2], fog_[0], fog_[1]})
        if (t) t->release();
    if (device_) device_->release();
}

MTL::Texture* RainPipeline::waveTexture() const { return wave_[waveCur_]; }
MTL::Texture* RainPipeline::fogTexture() const { return fog_[fogCur_]; }

// The floor's puddle cover now: the look's Wet and the sim's swell (as Sim::puddleWet).
static float puddleWet(const RainLook& look, const engine::rain::Sim& sim) {
    const float w = std::isfinite(look.wet) ? std::clamp(look.wet, 0.0f, 1.0f) : 0.65f;
    if (w <= 0.001f || w >= 0.999f) return w;
    const double sw = std::isfinite(sim.swell()) ? std::clamp(sim.swell(), 0.0, 0.1) : 0.0;
    return std::min(0.98f, w + float(sw));
}

void RainPipeline::clearFields(MTL::CommandBuffer* cmd, float fog) {
    for (MTL::Texture* w : wave_) pass(cmd, w, MTL::LoadActionClear, "rain waves clear")->endEncoding();
    const double f0 = std::isfinite(fog) ? std::clamp(double(fog), 0.0, 1.0) : 0.0;
    for (MTL::Texture* f : fog_)
        pass(cmd, f, MTL::LoadActionClear, "rain fog clear", MTL::ClearColor(f0, 0, 0, 0))->endEncoding();
    waveCur_ = 0;
    fogCur_ = 0;
    valid_ = true;
}

void RainPipeline::encodeStep(MTL::CommandBuffer* cmd, const engine::rain::Sim& sim, const RainLook& look, bool reset) {
    if (reset || !valid_) clearFields(cmd, sim.settings().mode == engine::rain::Mode::Window && look.fog > 0 ? 1.0f : 0.0f);
    const double dt = sim.lastDt();
    const std::uint32_t seed32 = std::uint32_t(sim.settings().seed ^ (sim.settings().seed >> 32));
    if (sim.settings().mode == engine::rain::Mode::Floor) {
        if (!(dt > 0) && sim.impulses().empty()) return;
        const engine::rain::WaveStep w = engine::rain::waveStep(dt > 0 ? dt : 1.0 / 120.0, look.speed, look.decay, rows_);
        WaveP p{};
        p.size[0] = cols_;
        p.size[1] = rows_;
        p.c2 = float(w.c2);
        p.b = float(w.b);
        p.nu = float(w.nu);
        p.damp = float(w.damp);
        // The puddles now (their edges creep and swell with the rain): drawn once for this step's substeps.
        p.wet = puddleWet(look, sim);
        p.seed = seed32;
        p.rows = float(rows_);
        if (p.wet < 0.999f) {
            MaskP m{float(rows_), p.wet, seed32, float(sim.puddleTime())};
            MTL::RenderCommandEncoder* enc = pass(cmd, mask_, MTL::LoadActionDontCare, "rain puddles");
            enc->setRenderPipelineState(maskPso_);
            enc->setFragmentBytes(&m, sizeof(m), 0);
            enc->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(3));
            enc->endEncoding();
        }
        p.sponge = float(std::max(3.0, 0.03 * rows_));
        for (int i = 0; i < w.substeps; ++i) {
            p.count = 0;
            if (i == 0)
                for (const auto& im : sim.impulses()) {
                    if (p.count >= 48) break;
                    float* o = p.imp[p.count++];
                    o[0] = float(im.x * rows_);
                    o[1] = float(im.y * rows_);
                    o[2] = float(std::max(1.0, im.radius * rows_));
                    o[3] = float(im.amp * rows_);
                }
            const int next = (waveCur_ + 1) % 3, prev = (waveCur_ + 2) % 3;
            MTL::RenderCommandEncoder* enc = pass(cmd, wave_[next], MTL::LoadActionDontCare, "rain waves");
            enc->setRenderPipelineState(wavePso_);
            enc->setFragmentBytes(&p, sizeof(p), 0);
            enc->setFragmentTexture(wave_[waveCur_], 0);
            enc->setFragmentTexture(wave_[prev], 1);
            enc->setFragmentTexture(mask_, 2);
            enc->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(3));
            enc->endEncoding();
            waveCur_ = next;
        }
        return;
    }
    if (!(dt > 0) && sim.wipes().empty()) return;
    FogP p{};
    p.rows = float(rows_);
    p.target = std::isfinite(look.fog) && look.fog > 0 ? 1.0f : 0.0f;  // the field is coverage; Fog scales it
    const double refog = std::isfinite(look.refog) ? std::clamp(double(look.refog), 0.5, 600.0) : 25.0;
    p.refog = float(1.0 - std::exp(-dt / refog));
    p.film = float(std::exp(-dt / 2.5));
    for (const auto& s : sim.wipes()) {
        if (p.count >= engine::rain::kMaxWipes) break;
        float* a = p.seg[2 * p.count];
        float* b = p.seg[2 * p.count + 1];
        a[0] = float(s.x0);
        a[1] = float(s.y0);
        a[2] = float(s.x1);
        a[3] = float(s.y1);
        b[0] = float(s.radius);
        ++p.count;
    }
    const int next = 1 - fogCur_;
    MTL::RenderCommandEncoder* enc = pass(cmd, fog_[next], MTL::LoadActionDontCare, "rain fog");
    enc->setRenderPipelineState(fogPso_);
    enc->setFragmentBytes(&p, sizeof(p), 0);
    enc->setFragmentTexture(fog_[fogCur_], 0);
    enc->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(3));
    enc->endEncoding();
    fogCur_ = next;
}

void RainPipeline::encodeComposite(MTL::CommandBuffer* cmd, MTL::Texture* output, const engine::rain::Sim& sim,
                                   const RainLook& look, bool trackAlpha) {
    if (!output || !(look.opacity > 0)) return;
    if (!valid_) clearFields(cmd, sim.settings().mode == engine::rain::Mode::Window && look.fog > 0 ? 1.0f : 0.0f);
    const float W = float(width_), H = float(height_);
    // 1. The frame: frosted (half size, SolidPipeline), and copied with mips for the refraction.
    const bool window = sim.settings().mode == engine::rain::Mode::Window;
    if (window) solids_.encodeFrost(cmd, output, look.frost);
    {
        MTL::RenderCommandEncoder* enc = pass(cmd, copy_, MTL::LoadActionDontCare, "rain copy");
        enc->setRenderPipelineState(copyPso_);
        enc->setFragmentTexture(output, 0);
        enc->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(3));
        enc->endEncoding();
        MTL::BlitCommandEncoder* blit = cmd->blitCommandEncoder();
        blit->generateMipmaps(copy_);
        blit->endEncoding();
    }
    // 2. The drops' heights.
    {
        MTL::RenderCommandEncoder* enc = pass(cmd, drops_, MTL::LoadActionClear, "rain drops");
        enc->setRenderPipelineState(dropPso_);
        const float size[2] = {W, H};
        enc->setVertexBytes(size, sizeof(size), 1);
        constexpr std::size_t kChunk = 4096 / sizeof(RainDropGpu);
        RainDropGpu chunk[kChunk];
        std::size_t n = 0;
        auto flush = [&]() {
            if (n == 0) return;
            enc->setVertexBytes(chunk, n * sizeof(RainDropGpu), 0);
            enc->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(6), NS::UInteger(n));
            n = 0;
        };
        const double contact = sim.settings().contact;
        for (const auto& d : sim.drops()) {
            chunk[n++] = rainDropGpu(d, contact, double(H));
            if (n == kChunk) flush();
        }
        flush();
        enc->endEncoding();
    }
    // 3. The water over the frame.
    CompP c{};
    c.size[0] = W;
    c.size[1] = H;
    c.simTexel[0] = 1.0f / float(cols_);
    c.simTexel[1] = 1.0f / float(rows_);
    auto fin = [](float v, float lo, float hi, float fb) { return std::isfinite(v) ? std::clamp(v, lo, hi) : fb; };
    c.depthPx = fin(look.depth, 0.0f, 4.0f, 0.5f) * H;
    c.fog = window ? fin(look.fog, 0.0f, 1.0f, 0.0f) : 0.0f;
    c.spec = fin(look.spec, 0.0f, 8.0f, 1.0f);
    c.rim = fin(look.rim, 0.0f, 4.0f, 1.0f);
    c.opacity = fin(look.opacity, 0.0f, 1.0f, 0.0f);
    c.mode = window ? 0 : 1;
    c.wet = window ? fin(look.wet, 0.0f, 1.0f, 0.65f) : puddleWet(look, sim);
    c.creepT = float(sim.puddleTime());
    c.seed = std::uint32_t(sim.settings().seed ^ (sim.settings().seed >> 32));
    c.aspect = W / H;
    c.pudl = window ? 0.0f : fin(look.pudl, 0.0f, 3.0f, 1.0f);
    c.puddleDepth = (0.004f + 0.006f * std::min(c.pudl, 1.0f)) * H;  // the edge's lens: deeper with Pudl
    c.opaque = trackAlpha && look.opaque ? 1 : 0;
    c.waveGain = 1.0f;
    MTL::RenderCommandEncoder* enc = pass(cmd, output, MTL::LoadActionDontCare, "rain composite");
    enc->setRenderPipelineState(compositePso_);
    enc->setFragmentBytes(&c, sizeof(c), 0);
    enc->setFragmentTexture(copy_, 0);
    enc->setFragmentTexture(solids_.halfBlur(), 1);
    enc->setFragmentTexture(drops_, 2);
    enc->setFragmentTexture(wave_[waveCur_], 3);
    enc->setFragmentTexture(fog_[fogCur_], 4);
    enc->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(3));
    enc->endEncoding();
}

}  // namespace reforce::render
