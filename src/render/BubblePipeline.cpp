#include "render/BubblePipeline.hpp"

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

#include "render/GpuPoison.hpp"
#include "render/ShaderCompiler.hpp"
#include "render/SolidPipeline.hpp"

namespace reforce::render {
namespace {

constexpr const char* kBubbleShaders = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct FullOut { float4 position [[position]]; float2 uv; };
vertex FullOut bubble_full_vs(uint vid [[vertex_id]]) {
    const float2 p = float2((vid << 1) & 2, vid & 2);
    FullOut o;
    o.position = float4(p * 2.0f - 1.0f, 0.0f, 1.0f);
    o.uv = float2(p.x, 1.0f - p.y);
    return o;
}
fragment float4 bubble_copy_fs(FullOut in [[stage_in]], texture2d<float> src [[texture(0)]]) {
    constexpr sampler s(filter::linear, address::clamp_to_edge);
    return src.sample(s, in.uv);
}

// ---- 3D value noise on the sphere (the film's swirl)
static uint lowbias32(uint x) {
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return x;
}
static float h3(int3 p) {
    return float(lowbias32(uint(p.x) * 0x8da6b343u ^ uint(p.y) * 0xd8163841u ^ uint(p.z) * 0xcb1ab31fu)) *
           (1.0f / 4294967296.0f);
}
static float sq(float x) { return x * x; }
static float vn3(float3 p) {
    const float3 f = floor(p);
    const int3 i = int3(f);
    float3 u = p - f;
    u = u * u * (3.0f - 2.0f * u);
    const float a = mix(h3(i), h3(i + int3(1, 0, 0)), u.x);
    const float b = mix(h3(i + int3(0, 1, 0)), h3(i + int3(1, 1, 0)), u.x);
    const float c = mix(h3(i + int3(0, 0, 1)), h3(i + int3(1, 0, 1)), u.x);
    const float d = mix(h3(i + int3(0, 1, 1)), h3(i + int3(1, 1, 1)), u.x);
    return mix(mix(a, b, u.y), mix(c, d, u.y), u.z);
}

// ---- thin-film interference (render::filmReflectance is the same on the CPU)
constant float kLambda[10] = {406.0f, 438.0f, 470.0f, 502.0f, 534.0f, 566.0f, 598.0f, 630.0f, 662.0f, 694.0f};
constant float3 kCmf[10] = {
    float3(0.0107f, -0.0103f, 0.0762f), float3(0.0497f, -0.0629f, 0.5459f), float3(-0.0381f, 0.0150f, 0.4333f),
    float3(-0.1638f, 0.2226f, 0.0704f), float3(-0.1664f, 0.4557f, -0.0418f), float3(0.1998f, 0.3508f, -0.0505f),
    float3(0.5712f, 0.0775f, -0.0255f), float3(0.4243f, -0.0385f, -0.0064f), float3(0.1041f, -0.0105f, -0.0014f),
    float3(0.0084f, 0.0006f, -0.0003f)};
static float3 filmRGB(float nm, float cosI) {
    const float n = 1.33f;
    cosI = clamp(cosI, 1e-4f, 1.0f);
    const float cosT = sqrt(1.0f - (1.0f - cosI * cosI) / (n * n));
    const float rs = (cosI - n * cosT) / (cosI + n * cosT), rp = (n * cosI - cosT) / (n * cosI + cosT);
    const float R0 = min(0.5f * (rs * rs + rp * rp), 0.9999f);
    const float F = 4.0f * R0 / ((1.0f - R0) * (1.0f - R0));
    const float opd = 2.0f * n * max(nm, 0.0f) * cosT;
    float3 rgb = 0.0f;
    for (int k = 0; k < 10; ++k) {
        const float s = sin(M_PI_F * opd / kLambda[k]);
        const float s2 = s * s;
        rgb += kCmf[k] * (F * s2 / (1.0f + F * s2));
    }
    return max(rgb, 0.0f);
}

struct BubI { float4 a; float4 m; float4 f; float4 p; float4 w[3]; };
struct BubP {
    float2 size;      // output px
    float iris, swirl, drain, bend, spec, rim, depthPx, opacity, popSec;
    int opaque;
    float4 light;     // linear RGB of the light the bubbles reflect (the ColorMap's tint)
};

struct BubOut { float4 position [[position]]; float2 local; uint iid [[flat]]; };
vertex BubOut bubble_vs(uint vid [[vertex_id]], uint iid [[instance_id]], constant BubI* B [[buffer(0)]],
                        constant BubP& P [[buffer(1)]]) {
    const BubI b = B[iid];
    const float2 corner[6] = {float2(-1, -1), float2(1, -1), float2(-1, 1), float2(-1, 1), float2(1, -1), float2(1, 1)};
    const float dev = abs(b.m.x) + abs(b.m.y) + abs(b.m.z) + abs(b.m.w);
    const float ext = b.a.z * (1.0f + dev) + 2.0f;
    const float2 local = corner[vid] * ext;
    const float2 px = b.a.xy + local;
    BubOut o;
    o.position = float4(px.x / P.size.x * 2.0f - 1.0f, 1.0f - px.y / P.size.y * 2.0f, 0.0f, 1.0f);
    o.local = local;
    o.iid = iid;
    return o;
}

// The film's thickness (nm) at the shell's point N (unit; x right, y down, z towards the viewer).
static float filmAt(float3 N, BubI b, constant BubP& P) {
    const float t = b.f.w;
    const float s = clamp(P.swirl, 0.0f, 3.0f);
    // The film flows: each height circles the bubble at its own speed (faster near the top, against the turn lower
    // down), so the bands shear and wind into swirls instead of sliding past as a whole.
    const float spin = t * 0.22f * s * (0.35f - 0.95f * N.y) + b.f.z;
    const float cs = cos(spin), sn = sin(spin);
    const float3 Q = float3(cs * N.x - sn * N.z, N.y, sn * N.x + cs * N.z);
    float3 p = Q * 1.6f + float3(b.f.z, b.f.z * 0.7f, b.f.z * 1.3f);
    p.y -= t * 0.12f * P.drain;  // the bands flow down
    // Two levels of moving warp: the first churns, the second (warped by the first) curls it into eddies.
    const float3 w1 = float3(vn3(p * 1.1f + float3(0.0f, 0.0f, t * 0.31f)),
                             vn3(p * 1.1f + float3(5.2f, 1.3f, 2.8f - t * 0.27f)),
                             vn3(p * 1.1f + float3(1.7f, 9.2f, 4.1f + t * 0.23f)));
    const float3 q2 = p * 1.7f + s * 1.8f * (w1 - 0.5f);
    const float3 w = float3(vn3(q2 + float3(3.1f, 0.0f, t * 0.42f)), vn3(q2 + float3(0.0f, 7.4f, -t * 0.37f)),
                            vn3(q2 + float3(8.3f, 2.2f, t * 0.33f)));
    const float sw = vn3(p * 2.1f + s * 2.8f * (w - 0.5f));
    // Streaks (only where the bubble is big enough on screen to show them without grain), dragged along by the flow.
    const float fine = vn3(p * 6.3f + s * 4.0f * (w - 0.5f) + float3(0.0f, t * 0.18f, 0.0f));
    const float up = -N.y;  // +1 at the top
    float d = b.f.x * (1.0f - 0.42f * clamp(P.drain, 0.0f, 1.5f) * up);  // drained: thin on top, thick below
    const float streaks = 0.18f * smoothstep(12.0f, 50.0f, b.a.z);
    d *= max(0.05f, 1.0f + min(P.swirl, 2.0f) * (0.6f * (sw - 0.5f) + streaks * (fine - 0.5f)));
    // A coming pop: a black film (too thin to reflect) creeps down from the top.
    const float cap = 1.05f - 1.25f * b.f.y;
    d *= 1.0f - 0.95f * smoothstep(cap, cap + 0.3f, up);
    return d;
}

// The light along the reflected ray R (linear): the visuals Dpth behind (rays towards the screen), else the room in
// front (the visuals mirrored and soft, a sky above); `win` the window's light along it.
static float3 lightAlong(float3 R, float2 uv, constant BubP& P, texture2d<float> frame, thread float& win) {
    constexpr sampler tri(filter::linear, mip_filter::linear, address::clamp_to_edge);
    float3 behind = 0.0f, room = 0.0f;
    const float k = smoothstep(-0.25f, 0.2f, R.z);
    if (k < 1.0f) {
        float2 off = R.xy / max(-R.z, 0.3f) * P.depthPx;
        const float lo = length(off);
        if (lo > P.size.y) off *= P.size.y / lo;
        const float2 u = uv + off / P.size;
        const float beyond = max(max(-u.x, u.x - 1.0f), max(-u.y, u.y - 1.0f));
        behind = pow(frame.sample(tri, u, level(1.5f)).rgb, 2.2f) * mix(0.05f, 1.0f, 1.0f - smoothstep(0.0f, 0.1f, beyond));
        // (The space between the bubbles and the visuals is lit a little too, so rims read over black.)
        behind += P.light.rgb * (0.05f + 0.05f * P.rim);
    }
    if (k > 0.0f) {
        const float2 u = float2(0.5f - 0.48f * R.x, 0.5f + 0.48f * R.y);  // mirrored
        room = pow(frame.sample(tri, u, level(4.5f)).rgb, 2.2f) * 0.6f +
               P.light.rgb * (0.04f + 0.34f * smoothstep(-0.3f, 1.0f, -R.y));
    }
    // A soft, round light up and to the left, in front (no window panes: Peter 2026-11-26).
    win = 0.0f;
    const float3 W = normalize(float3(-0.45f, -0.52f, 0.73f));
    const float along = dot(R, W);
    if (along > 0.2f) {
        const float3 ex = normalize(float3(W.z, 0.0f, -W.x));
        const float3 ey = cross(W, ex);
        const float u = dot(R, ex) / along, v = dot(R, ey) / along;
        const float r = length(float2(u / 0.2f, v / 0.16f));
        win = ((1.0f - smoothstep(0.45f, 1.15f, r)) * 3.2f + 0.2f * exp(-(u * u + v * v) * 7.0f)) * P.spec;
    }
    return mix(behind, room, k);
}

fragment float4 bubble_fs(BubOut in [[stage_in]], float4 dst [[color(0)]], constant BubI* B [[buffer(0)]],
                          constant BubP& P [[buffer(1)]], texture2d<float> frame [[texture(0)]]) {
    const BubI b = B[in.iid];
    const float2 d = in.local;
    const float rho = length(d);
    const float th = atan2(d.y, d.x);
    // Shared walls: cut at the plane where the spheres meet; the owner draws the wall (the other gives it a pixel).
    // s[i]: pixels past wall i's plane; keep[i]: this bubble's side of it.
    float s[3] = {0.0f, 0.0f, 0.0f}, keep[3] = {1.0f, 1.0f, 1.0f};
    float modeK = 1.0f;
    for (int i = 0; i < 3; ++i) {
        const float4 w = b.w[i];
        if (w.w == 0.0f) continue;
        s[i] = dot(d, w.xy) - w.z;
        keep[i] = w.w > 0.0f ? saturate(0.5f - s[i]) : saturate(-0.5f - s[i]);
        // Near a wall the outline is the plain sphere, so both bubbles' edges meet the wall's ends at the same point.
        modeK = min(modeK, saturate(abs(s[i]) / (0.4f * b.a.z)));
    }
    const float R = b.a.z * (1.0f + modeK * (b.m.x * cos(2.0f * th) + b.m.y * sin(2.0f * th) +
                                             b.m.z * cos(3.0f * th) + b.m.w * sin(3.0f * th)));
    float cov = saturate(R - rho + 0.5f) * keep[0] * keep[1] * keep[2];
    float wall = 0.0f, wallT = 0.0f;
    for (int i = 0; i < 3; ++i) {
        const float4 w = b.w[i];
        if (!(w.w > 0.0f)) continue;
        const float t = dot(d, float2(-w.y, w.x));
        float line = saturate(1.25f - abs(s[i])) * saturate(w.w - abs(t) + 0.5f);
        // A wall ends where it meets this bubble's other walls (three walls meet at one point).
        for (int j = 0; j < 3; ++j)
            if (j != i) line *= keep[j];
        if (line > wall) {
            wall = line;
            wallT = t / w.w;
        }
    }
    const float alpha = b.a.w * P.opacity;
    if ((cov <= 0.0f && wall <= 0.0f) || alpha <= 0.0f) discard_fragment();
    constexpr sampler lin(filter::linear, mip_filter::linear, address::clamp_to_edge);
    const float2 uv = in.position.xy / P.size;
    const float q = min(rho / max(R, 1e-3f), 0.9995f);
    const float2 dir = rho > 1e-4f ? d / rho : float2(0.0f);
    const float2 nxy = dir * q;
    const float nz = sqrt(max(1.0f - q * q, 0.0f));
    const float3 Nf = float3(nxy, nz), Nb = float3(nxy, -nz);
    // The films' thickness and their interference reflectance (Rim widens the Fresnel band at the edge).
    const float cosI = pow(max(nz, 1e-4f), 1.0f + 0.8f * P.rim);
    float3 Rf = filmRGB(filmAt(Nf, b, P), cosI), Rb = filmRGB(filmAt(Nb, b, P), cosI);
    const float3 grayF = (Rf.r + Rf.g + Rf.b) / 3.0f, grayB = (Rb.r + Rb.g + Rb.b) / 3.0f;
    Rf = max(grayF + (Rf - grayF) * P.iris * 1.6f, 0.0f);
    Rb = max(grayB + (Rb - grayB) * P.iris * 1.6f, 0.0f);
    // A pop: the film is gone within an angle of the tear that grows to pi (accelerating out of the tear, as the
    // hole's rim does); the hole's edge is ragged, its rim a band of gathered liquid breaking into beads, and a ripple
    // runs through the film just ahead of it (Peter 2026-11-26: a smooth cap read as a scroll through the bubble).
    float rimF = 0.0f, rimB = 0.0f;
    if (b.p.x >= 0.0f) {
        const float u = saturate(b.p.x / P.popSec);
        const float hole = M_PI_F * 1.12f * (0.25f * u + 0.75f * u * u * (3.0f - 2.0f * u));
        const float3 T = b.p.yzw;
        const float3 e1 = normalize(abs(T.z) < 0.9f ? cross(T, float3(0.0f, 0.0f, 1.0f)) : cross(T, float3(1.0f, 0.0f, 0.0f)));
        const float3 e2 = cross(T, e1);
        const float seed = b.f.z;
        float kk[2], rr[2];
        const float3 Ns[2] = {Nf, Nb};
        for (int side = 0; side < 2; ++side) {
            const float3 N = Ns[side];
            const float a = acos(clamp(dot(N, T), -1.0f, 1.0f));
            const float az = atan2(dot(N, e2), dot(N, e1));
            // Ragged: the edge runs ahead in irregular tongues and lags in flaps around the tear (noise round the tear,
            // the tongues shrinking again as the last of the film closes, so it never ends in a star).
            const float3 ring = float3(cos(az), sin(az), 0.0f) * 2.2f + float3(seed, seed * 0.7f, seed * 1.3f);
            const float rag = 2.0f * (0.65f * vn3(ring) + 0.35f * vn3(ring * 2.3f + 5.1f)) - 1.0f;
            const float h = hole + 0.22f * rag * sin(min(hole, M_PI_F));
            kk[side] = smoothstep(h - 0.02f, h + 0.04f, a);
            // The rim: thicker as it gathers liquid, beaded.
            const float wdt = 0.05f + 0.07f * u;
            const float beads = 0.8f + 0.2f * sin(23.0f * az + 3.7f * seed + 9.0f * a);
            rr[side] = exp(-sq((a - h) / wdt)) * beads * (1.15f - 0.55f * u);
            // A ripple in the film just ahead of the rim (the film shudders as the tension goes).
            const float ahead = a - h;
            const float rip = ahead > 0.0f ? exp(-ahead / 0.3f) * sin(ahead * 38.0f) : 0.0f;
            kk[side] *= 1.0f + 0.45f * rip;
        }
        Rf *= kk[0];
        Rb *= kk[1];
        rimF = rr[0];
        rimB = rr[1];
    }
    // What each film reflects: the front a convex mirror, the back (seen from inside) a concave one.
    const float3 RvF = float3(2.0f * nz * nxy, 2.0f * nz * nz - 1.0f);
    const float3 RvB = float3(-2.0f * nz * nxy, 2.0f * nz * nz - 1.0f);
    float winF, winB;
    const float3 LF = lightAlong(RvF, uv, P, frame, winF), LB = lightAlong(RvB, uv, P, frame, winB);
    // (The room's reflection fades a little towards the middle, which stays clear; the window does not.)
    const float fade = 0.4f + 0.6f * smoothstep(0.1f, 0.8f, q);
    const float gain = 2.0f;
    const float3 refl = gain * (Rf * (LF * fade + P.light.rgb * winF) +
                                0.75f * (1.0f - Rf) * Rb * (LB * fade + P.light.rgb * winB));
    const float3 T = saturate(1.0f - Rf) * saturate(1.0f - Rb);
    // Through the shell: the visuals shift a little, most at the rim (added to what is there, other bubbles included).
    const float2 off = dir * (P.bend * b.a.z * 0.045f * q * q * q * q);
    const float3 shifted = frame.sample(lin, uv + off / P.size, level(0.0f)).rgb;
    const float3 here = frame.sample(lin, uv, level(0.0f)).rgb;
    const float3 under = max(dst.rgb + (shifted - here) * saturate(cov), 0.0f);
    float3 col = pow(under, 2.2f) * T + refl;
    // The torn rim: a thick ring of liquid catching the light.
    const float3 rimLight = LF * 0.5f + P.light.rgb * (0.35f + 0.5f * winF);
    col += (rimF + 0.6f * rimB) * rimLight;
    // A cluster's wall seen edge-on: a film at grazing incidence, bright, its color from its thickness.
    if (wall > 0.0f) {
        const float wn = b.f.x * (0.75f + 0.25f * sin(wallT * 2.7f + b.f.z));
        const float3 rw = filmRGB(wn, 0.08f);
        float wwin;
        const float3 lw = lightAlong(float3(0.0f, 0.0f, -1.0f), uv, P, frame, wwin) * 0.8f + P.light.rgb * 0.18f;
        col = mix(col, pow(under, 2.2f) * 0.55f + rw * lw * 1.4f, wall * 0.9f);
    }
    col = pow(max(col, 0.0f), 1.0f / 2.2f);
    const float k = saturate(max(cov, wall)) * alpha;
    float a = dst.a;
    if (P.opaque != 0) a = max(a, k * saturate(0.15f + 2.0f * dot(refl, float3(0.33f))));
    return float4(mix(dst.rgb, saturate(col), k), a);
}

// ---- the spray: tiny lenses (the visuals upside down in each) with a glint
struct DropI { float cx, cy, r, alpha; };
struct DropOut { float4 position [[position]]; float2 local; float r [[flat]]; float alpha [[flat]]; };
vertex DropOut droplet_vs(uint vid [[vertex_id]], uint iid [[instance_id]], constant DropI* D [[buffer(0)]],
                          constant BubP& P [[buffer(1)]]) {
    const DropI d = D[iid];
    const float2 corner[6] = {float2(-1, -1), float2(1, -1), float2(-1, 1), float2(-1, 1), float2(1, -1), float2(1, 1)};
    const float ext = d.r + 1.5f;
    const float2 local = corner[vid] * ext;
    const float2 px = float2(d.cx, d.cy) + local;
    DropOut o;
    o.position = float4(px.x / P.size.x * 2.0f - 1.0f, 1.0f - px.y / P.size.y * 2.0f, 0.0f, 1.0f);
    o.local = local;
    o.r = d.r;
    o.alpha = d.alpha;
    return o;
}
fragment float4 droplet_fs(DropOut in [[stage_in]], float4 dst [[color(0)]], constant BubP& P [[buffer(1)]],
                           texture2d<float> frame [[texture(0)]]) {
    constexpr sampler lin(filter::linear, mip_filter::linear, address::clamp_to_edge);
    const float rho = length(in.local);
    const float cov = saturate(in.r - rho + 0.5f) * in.alpha * P.opacity;
    if (cov <= 0.0f) discard_fragment();
    const float2 uv = in.position.xy / P.size;
    const float2 q = in.local / max(in.r, 0.5f);
    const float3 lens = frame.sample(lin, uv - q * in.r * 4.0f / P.size, level(1.0f)).rgb * 0.8f;
    const float glint = exp(-dot(q + 0.4f, q + 0.4f) * 5.0f);
    const float3 col = lens + P.light.rgb * glint * 0.9f;
    return float4(mix(dst.rgb, saturate(col), cov), dst.a);
}
)MSL";

struct BubP {
    float size[2];
    float iris, swirl, drain, bend, spec, rim, depthPx, opacity, popSec;
    int32_t opaque;
    float light[4];
};
static_assert(sizeof(BubP) == 64, "BubP must match the MSL struct");

// The CPU copy of the shader's tables.
constexpr double kLambda[10] = {406.0, 438.0, 470.0, 502.0, 534.0, 566.0, 598.0, 630.0, 662.0, 694.0};
constexpr double kCmf[10][3] = {{0.0107, -0.0103, 0.0762}, {0.0497, -0.0629, 0.5459}, {-0.0381, 0.0150, 0.4333},
                                {-0.1638, 0.2226, 0.0704}, {-0.1664, 0.4557, -0.0418}, {0.1998, 0.3508, -0.0505},
                                {0.5712, 0.0775, -0.0255}, {0.4243, -0.0385, -0.0064}, {0.1041, -0.0105, -0.0014},
                                {0.0084, 0.0006, -0.0003}};

MTL::RenderPipelineState* makePso(MTL::Device* dev, MTL::Library* lib, const char* vs, const char* fs, const char* label) {
    MTL::Function* v = loadFunction(lib, vs);
    MTL::Function* f = loadFunction(lib, fs);
    MTL::RenderPipelineDescriptor* d = MTL::RenderPipelineDescriptor::alloc()->init();
    d->setLabel(NS::String::string(label, NS::UTF8StringEncoding));
    d->setVertexFunction(v);
    d->setFragmentFunction(f);
    d->colorAttachments()->object(0)->setPixelFormat(MTL::PixelFormatBGRA8Unorm);
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

MTL::RenderCommandEncoder* pass(MTL::CommandBuffer* cmd, MTL::Texture* target, MTL::LoadAction load, const char* label) {
    MTL::RenderPassDescriptor* rp = MTL::RenderPassDescriptor::renderPassDescriptor();
    auto* a = rp->colorAttachments()->object(0);
    a->setTexture(target);
    a->setLevel(0);
    a->setLoadAction(load);
    a->setStoreAction(MTL::StoreActionStore);
    MTL::RenderCommandEncoder* enc = makeRenderEncoder(cmd, rp);
    enc->setLabel(NS::String::string(label, NS::UTF8StringEncoding));
    return enc;
}

}  // namespace

void filmReflectance(double nm, double cosI, double rgb[3]) {
    const double n = 1.33;
    cosI = std::isfinite(cosI) ? std::clamp(cosI, 1e-4, 1.0) : 1.0;
    nm = std::isfinite(nm) ? std::max(nm, 0.0) : 0.0;
    const double cosT = std::sqrt(1.0 - (1.0 - cosI * cosI) / (n * n));
    const double rs = (cosI - n * cosT) / (cosI + n * cosT), rp = (n * cosI - cosT) / (n * cosI + cosT);
    const double R0 = std::min(0.5 * (rs * rs + rp * rp), 0.9999);
    const double F = 4.0 * R0 / ((1.0 - R0) * (1.0 - R0));
    const double opd = 2.0 * n * nm * cosT;
    rgb[0] = rgb[1] = rgb[2] = 0;
    for (int k = 0; k < 10; ++k) {
        const double s = std::sin(3.141592653589793 * opd / kLambda[k]);
        const double R = F * s * s / (1.0 + F * s * s);
        for (int c = 0; c < 3; ++c) rgb[c] += kCmf[k][c] * R;
    }
    for (int c = 0; c < 3; ++c) rgb[c] = std::max(rgb[c], 0.0);
}

BubbleGpu bubbleGpu(const engine::bubbles::Sim& sim, std::size_t i, const BubbleLook& look, double H) {
    using engine::bubbles::Sim;
    const auto& all = sim.bubbles();
    const engine::bubbles::Bubble& b = all[i];
    BubbleGpu g{};
    const double r = Sim::drawnRadius(b);
    g.a[0] = float(b.x * H);
    g.a[1] = float(b.y * H);
    g.a[2] = float(std::max(0.5, r * H));
    const double grow = std::clamp(b.grow, 0.0, 1.0);
    g.a[3] = float(std::min(1.0, grow / 0.35));
    for (int k = 0; k < 4; ++k) g.m[k] = float(std::clamp(b.mode[std::size_t(k)], -0.2, 0.2));
    const float film = std::isfinite(look.film) ? std::clamp(look.film, 0.0f, 3000.0f) : 520.0f;
    g.f[0] = float(film * (0.55 + 0.45 * std::exp(-std::max(0.0, b.age) / 25.0)));  // the film thins as it ages
    g.f[1] = float(std::clamp(b.doom, 0.0, 1.0));
    g.f[2] = float(std::fmod(std::fabs(b.film), 1000.0));
    g.f[3] = float(std::fmod(std::max(0.0, b.age), 4096.0));
    g.p[0] = float(b.pop);
    g.p[1] = float(b.tear[0]);
    g.p[2] = float(b.tear[1]);
    g.p[3] = float(b.tear[2]);
    // Walls: where this sphere meets each bonded one (the plane at a from its center, the circle of radius rho).
    int nw = 0;
    for (std::uint32_t id : b.bond) {
        if (id == 0 || nw >= 3) continue;
        const engine::bubbles::Bubble* o = nullptr;
        for (const auto& c : all)
            if (c.id == id) o = &c;
        if (!o || o->pop >= 0) continue;
        const double ro = Sim::drawnRadius(*o);
        const double dx = o->x - b.x, dy = o->y - b.y, dist = std::hypot(dx, dy);
        if (!(dist > 1e-9) || dist >= r + ro || dist <= std::fabs(r - ro)) continue;
        const double a = (dist * dist + r * r - ro * ro) / (2.0 * dist);
        const double rho = std::sqrt(std::max(0.0, r * r - a * a));
        float* w = g.w[nw++];
        w[0] = float(dx / dist);
        w[1] = float(dy / dist);
        w[2] = float(a * H);
        w[3] = float(std::max(0.5, rho * H)) * (b.id < o->id ? 1.0f : -1.0f);
    }
    return g;
}

BubblePipeline::BubblePipeline(SolidPipeline& solids) {
    MTL::Texture* half = solids.halfSharp();
    if (!half) throw std::runtime_error("no glass frost textures");
    device_ = half->device()->retain();
    width_ = std::max<uint32_t>(1, uint32_t(half->width()) * 2);
    height_ = std::max<uint32_t>(1, uint32_t(half->height()) * 2);
    if (solids.maxLive() > 0) {
        width_ = uint32_t(solids.target(0)->width());
        height_ = uint32_t(solids.target(0)->height());
    }
    MTL::Library* lib = compileLibrary(device_, kBubbleShaders, "reforce.bubbles");
    copyPso_ = makePso(device_, lib, "bubble_full_vs", "bubble_copy_fs", "bubbles copy");
    bubblePso_ = makePso(device_, lib, "bubble_vs", "bubble_fs", "bubbles");
    dropletPso_ = makePso(device_, lib, "droplet_vs", "droplet_fs", "bubble spray");
    lib->release();
    MTL::TextureDescriptor* td = MTL::TextureDescriptor::texture2DDescriptor(MTL::PixelFormatBGRA8Unorm, width_, height_, true);
    td->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
    td->setStorageMode(MTL::StorageModePrivate);
    copy_ = makeTexture(device_, td);
    if (!copy_) throw std::runtime_error("bubbles frame copy allocation failed");
    copy_->setLabel(NS::String::string("bubbles frame copy", NS::UTF8StringEncoding));
}

BubblePipeline::~BubblePipeline() {
    for (auto* p : {copyPso_, bubblePso_, dropletPso_})
        if (p) p->release();
    if (copy_) copy_->release();
    if (device_) device_->release();
}

void BubblePipeline::encodeComposite(MTL::CommandBuffer* cmd, MTL::Texture* output, const engine::bubbles::Sim& sim,
                                     const BubbleLook& look, bool trackAlpha) {
    if (!output || !(look.opacity > 0)) return;
    if (sim.bubbles().empty() && sim.droplets().empty()) return;
    const float W = float(width_), H = float(height_);
    // 1. The frame, copied with mips (the shift through the shell, the reflections).
    {
        MTL::RenderCommandEncoder* enc = pass(cmd, copy_, MTL::LoadActionDontCare, "bubbles copy");
        enc->setRenderPipelineState(copyPso_);
        enc->setFragmentTexture(output, 0);
        enc->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(3));
        enc->endEncoding();
        MTL::BlitCommandEncoder* blit = cmd->blitCommandEncoder();
        blit->generateMipmaps(copy_);
        blit->endEncoding();
    }
    auto fin = [](float v, float lo, float hi, float fb) { return std::isfinite(v) ? std::clamp(v, lo, hi) : fb; };
    BubP p{};
    p.size[0] = W;
    p.size[1] = H;
    p.iris = fin(look.iris, 0.0f, 3.0f, 1.0f);
    p.swirl = fin(look.swirl, 0.0f, 3.0f, 1.0f);
    p.drain = fin(look.drain, 0.0f, 4.0f, 1.0f);
    p.bend = fin(look.bend, 0.0f, 4.0f, 1.0f);
    p.spec = fin(look.spec, 0.0f, 4.0f, 1.0f);
    p.rim = fin(look.rim, 0.0f, 3.0f, 1.0f);
    p.depthPx = fin(look.depth, 0.0f, 4.0f, 0.3f) * H;
    p.opacity = fin(look.opacity, 0.0f, 1.0f, 0.0f);
    p.popSec = float(engine::bubbles::kPopSeconds);
    p.opaque = trackAlpha && look.opaque ? 1 : 0;
    // The light: white, tinted a little towards the ColorMap's color (linear, kept about as bright).
    {
        const float tint = fin(look.tint, 0.0f, 1.0f, 0.2f);
        float c[3], mx = 1e-3f;
        for (int k = 0; k < 3; ++k) {
            c[k] = std::pow(fin(look.tintColor[k], 0.0f, 1.0f, 1.0f), 2.2f);
            mx = std::max(mx, c[k]);
        }
        for (int k = 0; k < 3; ++k) p.light[k] = 1.0f + (c[k] / mx - 1.0f) * tint;
        p.light[3] = 1.0f;
    }
    // 2. The bubbles straight over the frame (each reads what is under it), then the spray.
    MTL::RenderCommandEncoder* enc = pass(cmd, output, MTL::LoadActionLoad, "bubbles");
    enc->setRenderPipelineState(bubblePso_);
    enc->setVertexBytes(&p, sizeof(p), 1);
    enc->setFragmentBytes(&p, sizeof(p), 1);
    enc->setFragmentTexture(copy_, 0);
    {
        constexpr std::size_t kChunk = 4096 / sizeof(BubbleGpu);
        BubbleGpu chunk[kChunk];
        std::size_t n = 0;
        auto flush = [&]() {
            if (n == 0) return;
            enc->setVertexBytes(chunk, n * sizeof(BubbleGpu), 0);
            enc->setFragmentBytes(chunk, n * sizeof(BubbleGpu), 0);
            enc->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(6), NS::UInteger(n));
            n = 0;
        };
        for (std::size_t i = 0; i < sim.bubbles().size(); ++i) {
            chunk[n++] = bubbleGpu(sim, i, look, double(H));
            if (n == kChunk) flush();
        }
        flush();
    }
    if (!sim.droplets().empty()) {
        enc->setRenderPipelineState(dropletPso_);
        constexpr std::size_t kChunk = 4096 / sizeof(DropletGpu);
        DropletGpu chunk[kChunk];
        std::size_t n = 0;
        auto flush = [&]() {
            if (n == 0) return;
            enc->setVertexBytes(chunk, n * sizeof(DropletGpu), 0);
            enc->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(6), NS::UInteger(n));
            n = 0;
        };
        for (const auto& d : sim.droplets()) {
            if (d.age <= 0) continue;  // the rim has not reached it yet
            const double life = std::max(1e-3, d.life);
            chunk[n++] = {float(d.x * H), float(d.y * H), float(std::max(0.6, d.r * H)),
                          float(std::clamp(1.0 - d.age / life, 0.0, 1.0))};
            if (n == kChunk) flush();
        }
        flush();
    }
    enc->endEncoding();
}

}  // namespace reforce::render
