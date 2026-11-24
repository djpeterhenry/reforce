#include "render/DancerPipeline.hpp"

#include "engine/DancerInstances.hpp"

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>

#include "engine/Perf.hpp"
#include "render/GpuPoison.hpp"
#include "render/ShaderCompiler.hpp"

namespace reforce::render {
namespace {

constexpr int kMaxShadows = 104;  // 3 a dancer: 32 on stage plus a few waving goodbye (= StageU.shadows in the MSL)

constexpr const char* kDancerShaders = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct CapV { packed_float3 dir; float end; };
struct Inst {
    float4 m0, m1, m2, m3;   // model columns (scale folded into m0 / m2)
    float4 n0, n1, n2;       // normal matrix columns
    float4 c0;               // color0, material
    float4 c1;               // color1, split (local y)
    float4 geo;              // len, r0, r1, outline
    float4 clip;             // local plane: discard where dot(p, xyz) > w
    float4 face;             // mouth, blink, brow, strand look (hair)
    float4 eyes;             // eye color, parting (hair)
    float4 style;            // pattern0, pattern1, fabric0, fabric1 (Lens: shape, side, fabric, -)
    float4 ink;              // print color, repeat
    float4 faceA;            // eye shape, brows, mouth style, beard
    float4 faceB;            // lashes, freckles, blush, lipstick
    float4 lips;             // hair: the second clip plane
    float4 beard;
};
struct StageU {
    float4x4 viewProj;
    float4 eye;              // xyz, proj11
    float4 viewport;         // width, height, outline px, lighting
    float4 floorP;           // floor alpha, shadow strength, beat, pattern
    float4 misc;             // pulse, shadow count, horizon v, half width
    float4 floor2;           // -, wall z, reflection strength
    float4 shadows[104];     // x, z, radius, strength (kMaxShadows)
};

struct BodyOut {
    float4 position [[position]];
    float3 world;
    float3 n;
    float3 local;
    uint iid [[flat]];
};

static float3 capsuleLocal(CapV v, Inst I, thread float3& nl) {
    const float r = mix(I.geo.y, I.geo.z, v.end);
    const float3 d = float3(v.dir);
    if (abs(d.y) < 1e-4f) nl = normalize(float3(d.x, (I.geo.y - I.geo.z) / max(I.geo.x, 1e-4f), d.z));
    else nl = d;
    return float3(d.x * r, v.end * I.geo.x + d.y * r, d.z * r);
}

static BodyOut bodyVertex(uint vid, uint iid, const device CapV* mesh, const device Inst* inst, constant StageU& U) {
    const Inst I = inst[iid];
    float3 nl;
    const float3 local = capsuleLocal(mesh[vid], I, nl);
    const float3 w = I.m0.xyz * local.x + I.m1.xyz * local.y + I.m2.xyz * local.z + I.m3.xyz;
    BodyOut o;
    o.position = U.viewProj * float4(w, 1.0f);
    o.world = w;
    o.n = I.n0.xyz * nl.x + I.n1.xyz * nl.y + I.n2.xyz * nl.z;
    o.local = local;
    o.iid = iid;
    return o;
}

vertex BodyOut body_vs(uint vid [[vertex_id]], uint iid [[instance_id]], const device CapV* mesh [[buffer(0)]],
                       const device Inst* inst [[buffer(1)]], constant StageU& U [[buffer(2)]]) {
    return bodyVertex(vid, iid, mesh, inst, U);
}

vertex BodyOut outline_vs(uint vid [[vertex_id]], uint iid [[instance_id]], const device CapV* mesh [[buffer(0)]],
                          const device Inst* inst [[buffer(1)]], constant StageU& U [[buffer(2)]]) {
    const Inst I = inst[iid];
    float3 nl;
    const float3 local = capsuleLocal(mesh[vid], I, nl);
    float3 w = I.m0.xyz * local.x + I.m1.xyz * local.y + I.m2.xyz * local.z + I.m3.xyz;
    const float3 n = normalize(I.n0.xyz * nl.x + I.n1.xyz * nl.y + I.n2.xyz * nl.z);
    const float4 c = U.viewProj * float4(w, 1.0f);
    w += n * (U.viewport.z * c.w * 2.0f / (U.viewport.y * U.eye.w));
    BodyOut o;
    o.position = I.geo.w > 0.5f ? U.viewProj * float4(w, 1.0f) : float4(0, 0, 2, 1);
    o.world = w;
    o.n = n;
    o.local = local;
    o.iid = iid;
    return o;
}

static float aaStep(float edge, float x, float w) { return smoothstep(edge - w, edge + w, x); }

static float hash21(float2 p) { return fract(sin(dot(p, float2(127.1f, 311.7f))) * 43758.5453f); }

// The face, drawn on the head (head-local direction d, unit sphere, +z forward). Variety (Look): eye shape, lashes,
// brows, mouth style, lipstick, freckles, blush, beard.
static float3 drawFace(float3 skin, float3 d, Inst I) {
    // The features are laid out in angles (u around, v up). Towards the sides of the head u changes very fast near
    // the crown and chin, so fwidth(u) (the antialiasing width) grew huge there and smeared the eyes' soft edges
    // into a dashed dark seam along the cut-off below (a vertical line from the crown past the ear to the chin).
    // So: the derivatives first, for every pixel (fwidth compares neighboring pixels); the width capped; and the
    // face faded out well before the cut-off (no feature reaches that far round).
    const float u = atan2(d.x, d.z), v = asin(clamp(d.y, -1.0f, 1.0f));
    const float aa = min(max(fwidth(u), fwidth(v)) * 0.9f, 0.1f) + 1e-4f;
    if (d.z < 0.1f) return skin;
    const float faceFade = smoothstep(0.1f, 0.3f, d.z);
    float3 col = skin;
    const float mouth = I.face.x, blink = I.face.y, brow = I.face.z;
    const float3 eyeC = I.eyes.rgb;
    const int eyeShape = int(I.faceA.x + 0.5f), browStyle = int(I.faceA.y + 0.5f);
    const int mouthStyle = int(I.faceA.z + 0.5f), beard = int(I.faceA.w + 0.5f);
    const bool lashes = I.faceB.x > 0.5f, freckles = I.faceB.y > 0.5f, lipstick = I.faceB.w > 0.5f;
    const float3 hairC = I.beard.rgb;
    // blush
    for (int s = -1; s <= 1; s += 2) {
        const float r = length(float2(u - s * 0.52f, (v + 0.17f) * 1.2f));
        col = mix(col, float3(1.0f, 0.55f, 0.55f), I.faceB.z * (1.0f - smoothstep(0.03f, 0.095f, r)));
    }
    // freckles: a few dots across the nose and cheeks
    if (freckles) {
        for (int k = 0; k < 14; ++k) {
            const float fk = float(k);
            const float2 c = float2((fract(fk * 0.618f) - 0.5f) * 1.05f, -0.1f - 0.11f * fract(fk * 0.377f + 0.2f));
            if (abs(c.x) < 0.1f) continue;
            const float r = length(float2(u, v) - c);
            col = mix(col, skin * 0.72f, 0.8f * (1.0f - aaStep(0.013f, r, aa)));
        }
    }
    // Stubble (the other beards are 3D, DancerRig buildBody): a speckled shadow where a full beard grows (below a
    // line from the sideburns down to under the mouth) and over the upper lip.
    if (beard == 1) {
        const float top = -0.52f + 0.25f * abs(d.x);
        const float jaw = smoothstep(0.03f, -0.03f, d.y - top) * smoothstep(0.1f, 0.2f, d.z);
        const float lip = (1.0f - smoothstep(0.035f, 0.06f, abs(v + 0.245f + 0.4f * u * u))) * (1.0f - smoothstep(0.28f, 0.34f, abs(u)));
        const float speck = step(0.45f, hash21(floor(float2(u, v) * 110.0f)));
        col = mix(col, mix(col, hairC, 0.55f), max(jaw, lip) * (0.3f + 0.4f * speck));
    }
    // Under a moustache the mouth sits a little lower, clear of it.
    const float mouthV = beard >= 2 && beard != 3 ? -0.35f : -0.30f;  // every beard but stubble and goatee
    // eyes
    for (int s = -1; s <= 1; s += 2) {
        const float2 e = float2(u - s * 0.34f, v - 0.05f);
        float rx = 0.095f, ry = 0.155f;
        if (eyeShape == 1) { rx = 0.11f; ry = 0.12f; }
        if (eyeShape == 4) { rx = 0.125f; ry = 0.18f; }
        if (eyeShape == 5) { rx = 0.05f; ry = 0.055f; }
        if (eyeShape == 3) {
            // happy: closed upward arcs ^ ^
            const float yc = 0.02f - 2.2f * e.x * e.x + 0.06f * (1.0f - blink);
            const float dd = abs(e.y - yc) + max(0.0f, abs(e.x) - 0.1f);
            col = mix(col, eyeC, 1.0f - aaStep(0.02f, dd, aa));
        } else {
            ry = ry * (1.0f - 0.93f * blink) + 0.008f;
            const float q = length(e / float2(rx, ry));
            float inside = 1.0f - aaStep(1.0f, q, aa / min(rx, ry));
            float lid = 0.0f;
            if (eyeShape == 2) {  // sleepy: the upper half under a lid
                lid = 1.0f - aaStep(0.015f, -e.y + ry * 0.1f, aa);
                inside *= 1.0f - lid;
            }
            col = mix(col, eyeC, inside);
            if (blink < 0.5f && eyeShape != 5) {
                const float hl = eyeShape == 4 ? 0.045f : 0.034f;
                const float h = length(e - float2(0.03f, eyeShape == 2 ? 0.0f : 0.06f));
                col = mix(col, float3(1.0f), (1.0f - aaStep(hl, h, aa)) * inside);
                if (eyeShape == 4) {
                    const float h2 = length(e - float2(-0.035f, -0.07f));
                    col = mix(col, float3(1.0f), (1.0f - aaStep(0.02f, h2, aa)) * inside);
                }
            }
            if (eyeShape == 2) {  // the lid line
                const float ll = abs(e.y - ry * 0.1f) + max(0.0f, abs(e.x) - rx);
                col = mix(col, eyeC * 0.8f, 1.0f - aaStep(0.012f, ll, aa));
            }
            if (lashes && blink < 0.5f) {  // three short flicks off the outer top of the eye, pointing outwards
                for (int k = 0; k < 3; ++k) {
                    const float th = 0.35f + 0.38f * float(k);  // from the outer corner up towards the top
                    float2 base = float2(s * rx * cos(th), ry * sin(th));
                    if (eyeShape == 2) base.y = min(base.y, ry * 0.1f);  // sleepy: from the lid
                    const float2 dir = normalize(float2(s * cos(th) / rx, sin(th) / ry) + float2(s * 0.6f, 0.0f) * 9.0f);
                    const float2 pq = e - base;
                    const float t = clamp(dot(pq, dir), 0.0f, 0.05f);
                    const float dl = length(pq - dir * t);
                    col = mix(col, eyeC * 0.7f, 1.0f - aaStep(0.008f, dl, aa));
                }
            }
        }
        // brows
        const float by = 0.29f + 0.05f * brow + (blink > 0.5f ? -0.02f : 0.0f) + (eyeShape == 4 ? 0.04f : 0.0f);
        float2 b = float2(u - s * 0.34f, v - by);
        b.y += (b.x * s) * (-0.25f * brow);
        if (browStyle == 3) b.y += 3.0f * b.x * b.x - 0.01f;  // arched
        const float bw = browStyle == 1 ? 0.03f : browStyle == 2 ? 0.009f : 0.017f;
        const float bl = browStyle == 1 ? 0.085f : 0.075f;
        const float2 bc = float2(clamp(b.x, -bl, bl), 0.0f);
        const float bd = length(b - bc);
        col = mix(col, (browStyle == 1 ? hairC : eyeC) * 0.8f, 1.0f - aaStep(bw, bd, aa));
    }
    // mouth
    const float3 lip = lipstick ? I.lips.rgb : float3(0.3f, 0.07f, 0.09f);
    const float width = mouthStyle == 1 ? 0.26f : mouthStyle == 2 ? 0.08f : 0.18f;
    if (mouth < 0.12f) {
        float yc = mouthV + (mouthStyle == 1 ? 1.6f : 1.3f) * u * u;
        if (mouthStyle == 3) yc += 0.35f * u * (u > 0.0f ? 1.0f : 0.15f);  // smirk: one corner up
        const float dd = abs(v - yc) + max(0.0f, abs(u) - width);
        col = mix(col, lip, 1.0f - aaStep(lipstick ? 0.032f : 0.024f, dd, aa));
        if (mouthStyle == 1) {  // grin: a row of teeth under the line
            const float t = (1.0f - aaStep(0.03f, abs(v - (yc - 0.035f)), aa)) * (1.0f - aaStep(width * 0.8f, abs(u), aa));
            col = mix(col, float3(1.0f), t * step(v, yc));
        }
    } else {
        const float m = clamp((mouth - 0.12f) / 0.88f, 0.0f, 1.0f);
        const float wk = width / 0.18f;
        const float rx = (0.13f + 0.05f * m) * wk, ry = 0.04f + 0.1f * m;
        float2 e = float2(u, v - mouthV);
        e.y = e.y > 0.0f ? e.y * 2.2f : e.y;  // flatter top: a D-shaped open smile
        const float q = length(e / float2(rx, ry));
        const float inside = 1.0f - aaStep(1.0f, q, aa / ry);
        float3 inner = float3(0.32f, 0.06f, 0.09f);
        const float tongue = 1.0f - smoothstep(0.5f, 0.7f, length((e + float2(0.0f, ry * 0.55f)) / float2(rx * 0.7f, ry * 0.5f)));
        inner = mix(inner, float3(0.92f, 0.45f, 0.5f), tongue);
        col = mix(col, inner, inside);
        if (lipstick) col = mix(col, lip, 1.0f - aaStep(0.02f, abs(q - 1.0f) * ry, aa));
    }
    return mix(skin, col, faceFade);
}

// A print on the cloth (Look patterns), in the part's own coordinates: around the axis (arc length) and along it.
static float printMask(int pattern, float3 local, float len, float period) {
    if (pattern <= 0 || period <= 0.0f) return 0.0f;
    const float r = max(length(local.xz), 1e-3f);
    const float2 p = float2(atan2(local.x, local.z) * r, local.y) / period;
    const float2 w = max(fwidth(p), float2(1e-4f));
    switch (pattern) {
        case 1: {  // stripes (bands around the body)
            const float f = abs(fract(p.y) - 0.5f);
            return smoothstep(0.25f + w.y, 0.25f - w.y, f);
        }
        case 2: {  // dots, offset every other row
            float2 q = p * 1.3f;
            q.x += 0.5f * fmod(floor(q.y), 2.0f);
            const float d = length(fract(q) - 0.5f);
            return smoothstep(0.24f + w.x * 1.3f, 0.24f - w.x * 1.3f, d);
        }
        case 3: {  // plaid: two crossing sets of bands, thin lines on top
            const float a = smoothstep(0.3f + w.y, 0.3f - w.y, abs(fract(p.y) - 0.5f));
            const float b = smoothstep(0.3f + w.x, 0.3f - w.x, abs(fract(p.x) - 0.5f));
            const float ln = smoothstep(0.06f + w.y, 0.06f - w.y, abs(fract(p.y * 2.0f + 0.25f) - 0.5f));
            return clamp(0.4f * a + 0.4f * b + 0.5f * ln * (1.0f - a), 0.0f, 1.0f);
        }
        case 4: {  // gradient: towards the ink along the part
            return smoothstep(0.0f, 1.0f, clamp(local.y / max(len, 0.05f), 0.0f, 1.0f)) * 0.85f;
        }
        case 5: {  // zigzag bands
            const float z = p.y + 0.6f * abs(fract(p.x * 0.8f) - 0.5f);
            const float f = abs(fract(z) - 0.5f);
            return smoothstep(0.2f + w.y * 1.5f, 0.2f - w.y * 1.5f, f);
        }
    }
    return 0.0f;
}

// Hair (material 4, BodyPart::strands): strands, a sheen band across them and a parting. 1D value noise, periodic
// over 60 cells (strands round an axis wrap without a seam).
static float strandNoise(float x) {
    const float i = floor(x), f = fract(x);
    const float a = hash21(float2(fmod(i + 600.0f, 60.0f), 0.37f));
    const float b = hash21(float2(fmod(i + 601.0f, 60.0f), 0.37f));
    return mix(a, b, f * f * (3.0f - 2.0f * f));
}
struct HairLook {
    float factor;   // on the albedo
    float v;        // this strand's random value (shifts the sheen)
    float3 T;       // the strand direction (world); zero: no sheen band
};
// q: the local point / r0; n: the world normal; ax, ay, az: the part's world axes.
static HairLook hairLook(int look, float3 q, float part, float3 n, float3 ax, float3 ay, float3 az) {
    HairLook h{1.0f, 0.5f, float3(0.0f)};
    const int mode = look & 7;
    if (mode <= 2) {
        float s, fw;
        if (mode == 1) {
            s = q.z * 11.0f;
            fw = fwidth(s);
            h.T = cross(n, az);
        } else if (mode == 2) {
            s = q.x * 11.0f;
            fw = fwidth(s);
            h.T = cross(n, ax);
        } else {
            s = atan2(q.x, q.z) * 9.5492966f;  // 60 strands round the axis
            fw = length(fwidth(q.xz)) / max(length(q.xz), 1e-3f) * 9.5492966f;
            h.T = ay - n * dot(n, ay);
        }
        const float v = 0.62f * strandNoise(s) + 0.38f * strandNoise(s * 2.7f + 13.0f);
        const float c = 1.0f - smoothstep(0.3f, 0.8f, fw);
        h.v = mix(0.5f, v, c);
        h.factor = 1.0f + (v - 0.5f) * 0.6f * c;
        h.T = normalize(h.T + 1e-5f);
        if (mode == 1 && part < 5.0f) {
            // The parting: a fine line of scalp from the forehead over the top.
            const float d = abs(q.x - part);
            const float m = (1.0f - smoothstep(0.02f, 0.02f + 1.5f * fwidth(q.x), d)) * smoothstep(0.25f, 0.5f, q.y) *
                            smoothstep(-0.55f, -0.05f, q.z);
            h.factor = mix(h.factor, -1.0f, m);  // < 0: scalp (shadeBody)
        }
    } else if (mode == 3) {
        // 360 waves: ripples in rings round the crown.
        const float ph = (q.y * 7.0f + 0.25f * sin(atan2(q.x, q.z) * 3.0f)) * 6.2831853f;
        const float c = 1.0f - smoothstep(0.25f, 0.6f, fwidth(q.y * 7.0f));
        h.factor = 1.0f + 0.28f * sin(ph) * c;
    } else if (mode == 4) {
        // Coils: a nubbly texture.
        const float3 p = q * 9.0f;
        const float k = sin(p.x + 2.0f * sin(p.y * 0.8f)) * sin(p.y + 2.0f * sin(p.z * 0.8f)) * sin(p.z + 2.0f * sin(p.x * 0.8f));
        const float c = 1.0f - smoothstep(0.35f, 0.9f, length(fwidth(p)));
        h.factor = 1.0f + 0.35f * k * c;
    }
    return h;
}

// Sunglasses lens shapes (q: the lens disc, about -1..1): < 0 inside.
static float lensShape(int shape, int side, float2 q) {
    switch (shape) {
        case 2: return length(float2(q.x * (1.0f + 0.2f * q.y), q.y * 1.05f + 0.12f * q.y * q.y)) - 1.0f;  // aviator
        case 3: case 8: return pow(pow(abs(q.x), 4.0f) + pow(abs(q.y), 4.0f), 0.25f) - 1.0f;           // visor
        case 4: {                                                                                      // heart
            float2 h = float2(abs(q.x), q.y * 1.1f + 0.55f) * 0.62f;
            if (h.y + h.x > 1.0f) return (length(h - float2(0.25f, 0.75f)) - 0.3536f) / 0.62f;
            const float a = dot(h - float2(0.0f, 1.0f), h - float2(0.0f, 1.0f));
            const float2 m = h - 0.5f * max(h.x + h.y, 0.0f);
            return sqrt(min(a, dot(m, m))) * sign(h.x - h.y) / 0.62f;
        }
        case 5: {                                                                                      // star
            const float2 k1 = float2(0.809016994f, -0.587785252f), k2 = float2(-k1.x, k1.y);
            float2 p = float2(abs(q.x), q.y * 1.05f + 0.08f);
            p -= 2.0f * max(dot(k1, p), 0.0f) * k1;
            p -= 2.0f * max(dot(k2, p), 0.0f) * k2;
            p.x = abs(p.x);
            p.y -= 1.0f;
            const float2 ba = 0.5f * float2(-k1.y, k1.x) - float2(0.0f, 1.0f);
            const float hh = clamp(dot(p, ba) / dot(ba, ba), 0.0f, 1.0f);
            return length(p - ba * hh) * sign(p.y * ba.x - p.x * ba.y);
        }
        case 6: {                                                                                      // cat-eye
            const float xo = side == 0 ? q.x : -q.x;
            return length(q) - 1.0f - 0.4f * smoothstep(0.1f, 1.0f, xo) * smoothstep(-0.1f, 0.9f, q.y);
        }
        case 7: return pow(pow(abs(q.x), 6.0f) + pow(abs(q.y), 6.0f), 1.0f / 6.0f) - 1.0f;               // square
    }
    return length(q) - 1.0f;                                                                            // round
}

// The stage pass has a second target: how much of the frame's Syphon alpha each pixel must keep opaque
// (reforce.Syphon.Alpha): 1 on the bodies, the floor's coverage on the floor (shadows included): the dancers and the
// disco floor are never keyed.
// Blended with max, resolved with the color, added to the frame's alpha by the composite.
struct StageOut {
    float4 color [[color(0)]];
    float opaque [[color(1)]];
};

static StageOut shadeBody(BodyOut in, bool front, const device Inst* inst, constant StageU& U,
                          texture2d<float> light) {
    const Inst I = inst[in.iid];
    if (dot(in.local, I.clip.xyz) > I.clip.w) discard_fragment();
    const int material = int(I.c0.w + 0.5f);
    if (material == 4 && dot(in.local, I.lips.xyz) > I.lips.w) discard_fragment();
    constexpr sampler smp(filter::linear, mip_filter::linear, address::clamp_to_edge);
    float3 n = normalize(in.n);
    const float split = I.c1.w;
    const float sw = max(fwidth(in.local.y), 1e-5f);
    const float region = material == 3 ? 0.0f : smoothstep(split - sw, split + sw, in.local.y);
    float3 albedo = mix(I.c0.rgb, I.c1.rgb, region);
    const int ri = region > 0.5f ? 1 : 0;
    int fabric = int((ri == 0 ? I.style.z : I.style.w) + 0.5f);
    const float beat = U.floorP.z;
    const float pulse = exp(-fract(beat) * 4.0f);

    float lensRim = 0.0f;
    if (material == 3) {
        // A lens: the disc cut to its shape, a frame rim along the edge.
        const float squash = max(I.c1.w, 0.3f);
        const float2 q = float2(in.local.x, in.local.y / squash) / max(I.geo.y, 1e-4f);
        const float d = lensShape(int(I.style.x + 0.5f), int(I.style.y + 0.5f), q);
        const float aa = max(fwidth(d), 1e-4f);
        if (d > aa) discard_fragment();
        const int kind = int(I.style.z + 0.5f);  // 0 dark glass, 1 mirrored, 2 clear: just the frame
        const float rimW = kind == 2 ? 0.26f : 0.2f;
        lensRim = smoothstep(-rimW - aa, -rimW + aa, d);
        if (kind == 2 && lensRim < 0.5f) discard_fragment();
        fabric = kind == 1 ? 1 : 0;
    } else if (material != 1) {
        const int pat = int((ri == 0 ? I.style.x : I.style.y) + 0.5f);
        const float ink = printMask(pat, in.local, I.geo.x, I.ink.w);
        albedo = mix(albedo, I.ink.rgb, ink);
    }
    if ((material == 2 || material == 4) && !front) {
        // The inside of a hair shell seen through its cut: shaded as solid hair, as if the cut were filled in (mostly
        // facing the viewer, a little of the shell's curve), not as a dark hollow.
        n = normalize(mix(-n, normalize(U.eye.xyz - in.world), 0.7f));
        albedo *= 0.85f;
    }
    if (material == 1) albedo = drawFace(albedo, normalize(in.local), I);
    HairLook hair{1.0f, 0.5f, float3(0.0f)};
    if (material == 4) {
        hair = hairLook(int(I.face.w + 0.5f), in.local / max(I.geo.y, 1e-4f), I.eyes.w, n, normalize(I.m0.xyz),
                        normalize(I.m1.xyz), normalize(I.m2.xyz));
        if (!front) hair.T = float3(0.0f);  // no sheen on the inside of a shell
        albedo = hair.factor < 0.0f ? mix(albedo, float3(0.42f, 0.31f, 0.26f), 0.7f) : albedo * hair.factor;
    }

    const float3 V = normalize(U.eye.xyz - in.world);
    const float2 uv = in.position.xy / U.viewport.xy;
    const float2 ns = float2(n.x, -n.y);
    const float lighting = U.viewport.w;
    if (fabric == 4) {
        // Neon: glows by itself (bright on a black frame), a brighter core, pulsing on the beat.
        const float core = pow(saturate(dot(n, V)), 2.0f);
        const float3 hue = albedo / max(max(albedo.r, max(albedo.g, albedo.b)), 1e-3f);  // full-strength color
        float3 col = hue * (0.95f + 0.35f * pulse) * (0.75f + 0.35f * core) + float3(0.12f) * core * core;
        return StageOut{float4(col, 1.0f), 1.0f};
    }
    // Light from the visuals: around this point of the screen, towards where the normal faces.
    const float3 lv = light.sample(smp, clamp(uv + ns * 0.16f, 0.0f, 1.0f), level(4.5f)).rgb;
    const float3 amb = light.sample(smp, float2(0.5f), level(9.0f)).rgb;
    // Studio key light (upper left, front), soft two-band toon ramp.
    const float3 Lk = normalize(float3(-0.45f, 0.75f, 0.55f));
    const float wrapL = saturate((dot(n, Lk) + 0.35f) / 1.35f);
    const float toon = mix(wrapL, smoothstep(0.4f, 0.52f, wrapL), 0.55f);
    const float3 hemi = mix(float3(0.30f, 0.28f, 0.36f), float3(0.55f, 0.56f, 0.64f), n.y * 0.5f + 0.5f);
    const float3 studio = hemi * 0.62f + float3(1.0f, 0.96f, 0.9f) * toon * 0.72f;
    const float3 visual = (0.14f + 1.7f * lv + 0.6f * amb) * (0.6f + 0.4f * toon);
    const float3 lit = max(mix(studio, visual, lighting), studio * 0.6f);
    const float fres = pow(1.0f - saturate(dot(n, V)), 3.0f);
    const float3 h = normalize(Lk + V);
    float3 col;
    if (fabric == 1 || fabric == 2) {
        // Metallic (lame, sequins, mirrored lenses): mostly a tinted reflection of the visuals around it.
        const float3 Rr = reflect(-V, n);
        const float3 env = light.sample(smp, clamp(uv + float2(Rr.x, -Rr.y) * 0.3f, 0.0f, 1.0f), level(2.5f)).rgb;
        const float3 studioEnv = mix(float3(0.12f, 0.11f, 0.14f), float3(1.1f, 1.05f, 1.0f), smoothstep(-0.3f, 0.6f, Rr.y));
        const float3 refl = mix(studioEnv, 0.25f + 2.2f * env, lighting);
        float3 base = albedo * (0.25f * lit + 0.95f * refl);
        float sparkle = 0.0f;
        if (fabric == 2) {
            // Sequins: little facets, each catching the light on its own beat.
            const float r = max(length(in.local.xz), 1e-3f);
            const float2 cp = float2(atan2(in.local.x, in.local.z) * r, in.local.y) / max(I.ink.w * 0.22f, 1e-4f);
            const float2 cell = floor(cp);
            const float hsh = hash21(cell);
            base *= 0.7f + 0.6f * hsh;
            const float tw = fract(hsh * 13.7f + floor(beat * 2.0f) * 0.618f);
            const float dot0 = 1.0f - smoothstep(0.15f, 0.42f, length(fract(cp) - 0.5f));
            sparkle = step(0.86f, tw) * dot0 * (0.6f + 0.8f * pulse);
        }
        col = base + pow(saturate(dot(n, h)), 70.0f) * 1.1f + sparkle * float3(1.6f) + fres * albedo * 0.6f;
    } else if (fabric == 3) {
        // Velvet: darker where it faces you, a soft sheen in its own color at the edges.
        const float sheen = pow(1.0f - saturate(dot(n, V)), 1.6f);
        col = albedo * lit * 0.62f + albedo * sheen * (0.9f + 0.9f * lighting * (lv.r + lv.g + lv.b) / 1.5f);
    } else {
        col = albedo * lit;
        const float3 tint = mix(float3(1.0f), 0.4f + 1.6f * lv, lighting);
        if (material == 4 && length_squared(hair.T) > 0.5f) {
            // Hair sheen (Kajiya-Kay): a bright band across the strands, broken up strand by strand, and a broad
            // glow in the hair's own color.
            const float th = dot(hair.T, h) + (hair.v - 0.5f) * 0.35f - 0.1f;
            const float sn = sqrt(saturate(1.0f - th * th));
            col += (pow(sn, 90.0f) * 0.3f + pow(sn, 14.0f) * 0.2f * (albedo + 0.05f)) * tint * (0.35f + 0.65f * toon);
        } else {
            // Plastic highlight.
            const float spec = pow(saturate(dot(n, h)), 40.0f) * (material == 2 || material == 4 ? 0.12f : 0.28f);
            col += spec * tint;
        }
    }
    if (material == 3) {
        // Lens: dark glass with a strong highlight (a Shiny lens is mirrored: col above); the frame on the rim.
        if (fabric == 0) col = albedo * lit * 0.6f + pow(saturate(dot(n, h)), 30.0f) * 0.9f + fres * 0.3f;
        const float glint = smoothstep(0.75f, 1.0f, saturate(dot(normalize(float2(in.local.x, in.local.y) + 1e-5f), float2(-0.6f, 0.8f))));
        col += glint * 0.25f * (1.0f - lensRim);
        col = mix(col, I.c1.rgb * lit, lensRim);
        return StageOut{float4(col, 1.0f), 1.0f};
    }
    // Rim: the frame around the silhouette (plus a little studio rim).
    const float3 rimC = light.sample(smp, clamp(uv + ns * 0.08f, 0.0f, 1.0f), level(2.0f)).rgb * 1.8f * lighting +
                        float3(0.3f) * (1.0f - lighting);
    col += fres * rimC * (fabric == 3 ? 0.25f : 0.7f);
    return StageOut{float4(col, 1.0f), 1.0f};
}

fragment StageOut body_fs(BodyOut in [[stage_in]], bool front [[front_facing]], const device Inst* inst [[buffer(0)]],
                        constant StageU& U [[buffer(1)]], texture2d<float> light [[texture(0)]]) {
    return shadeBody(in, front, inst, U, light);
}

// Reflections in the floor (reforce.Dancer.Reflection): the bodies mirrored under the floor plane, drawn opaque with
// depth into their own half-size target first (only the nearest surface per pixel: no doubling where parts overlap),
// shaded like the bodies, a little darker, fading with depth below the floor. floor_fs blends that image in once.
vertex BodyOut body_reflect_vs(uint vid [[vertex_id]], uint iid [[instance_id]], const device CapV* mesh [[buffer(0)]],
                               const device Inst* inst [[buffer(1)]], constant StageU& U [[buffer(2)]]) {
    BodyOut o = bodyVertex(vid, iid, mesh, inst, U);
    o.world.y = -o.world.y;
    o.n.y = -o.n.y;
    o.position = U.viewProj * float4(o.world, 1.0f);
    return o;
}

fragment float4 body_reflect_fs(BodyOut in [[stage_in]], bool front [[front_facing]],
                                const device Inst* inst [[buffer(0)]], constant StageU& U [[buffer(1)]],
                                texture2d<float> light [[texture(0)]]) {
    if (in.world.y > 0.0f) discard_fragment();  // a foot dipping under the floor would mirror above it
    const StageOut o = shadeBody(in, !front, inst, U, light);  // mirrored: the winding flips
    const float k = U.floor2.z * exp(-2.4f * -in.world.y);
    return float4(o.color.rgb * 0.85f * k, k);  // premultiplied
}

fragment StageOut outline_fs(BodyOut in [[stage_in]], const device Inst* inst [[buffer(0)]]) {
    const Inst I = inst[in.iid];
    if (dot(in.local, I.clip.xyz) > I.clip.w) discard_fragment();
    if (int(I.c0.w + 0.5f) == 4 && dot(in.local, I.lips.xyz) > I.lips.w) discard_fragment();
    return StageOut{float4(I.c0.rgb * 0.18f + 0.01f, 1.0f), 1.0f};
}

// ---- floor
struct FloorOut { float4 position [[position]]; float2 xz; };

vertex FloorOut floor_vs(uint vid [[vertex_id]], constant StageU& U [[buffer(2)]]) {
    const float2 c[6] = {float2(0, 0), float2(1, 0), float2(0, 1), float2(1, 0), float2(1, 1), float2(0, 1)};
    const float hw = U.misc.w * 1.6f;
    const float2 xz = float2(mix(-hw, hw, c[vid].x), mix(2.6f, -6.0f, c[vid].y));
    FloorOut o;
    o.position = U.viewProj * float4(xz.x, 0.0f, xz.y, 1.0f);
    o.xz = xz;
    return o;
}

static float3 hue(float h) { return saturate(abs(fract(h + float3(0.0f, 2.0f / 3.0f, 1.0f / 3.0f)) * 6.0f - 3.0f) - 1.0f); }

fragment StageOut floor_fs(FloorOut in [[stage_in]], constant StageU& U [[buffer(1)]],
                         texture2d<float> light [[texture(0)]], texture2d<float> frame [[texture(1)]],
                         texture2d<float> dancers [[texture(2)]]) {
    constexpr sampler smp(filter::linear, mip_filter::linear, address::clamp_to_edge);
    // The dancers' reflection (body_reflect_fs, premultiplied), blended into the floor's surface once.
    float4 dref = float4(0.0f);
    if (U.floor2.z > 0.0f) {
        constexpr sampler rs(filter::linear, address::clamp_to_edge);
        dref = dancers.sample(rs, in.position.xy / U.viewport.xy);
    }
    const float2 xz = in.xz;
    const float tile = 0.5f;
    const float2 cell = floor(xz / tile);
    const float2 f = fract(xz / tile);
    const float grout = min(min(f.x, 1.0f - f.x), min(f.y, 1.0f - f.y));
    const float gw = max(fwidth(xz.x / tile), fwidth(xz.y / tile));
    const float g = smoothstep(0.025f, 0.025f + gw * 1.5f + 0.02f, grout);
    const float beat = U.floorP.z;
    const float bi = floor(beat), ph = fract(beat);
    const float decay = exp(-ph * 3.0f);
    const int pattern = int(U.floorP.w + 0.5f) & 3;
    float on = 0.0f;
    if (pattern == 0) on = fmod(abs(cell.x + cell.y + bi), 2.0f) < 0.5f ? 1.0f : 0.0f;
    else if (pattern == 1) on = fract((cell.x - bi) / 4.0f) < 0.26f ? 1.0f : 0.0f;
    else if (pattern == 2) on = abs(length(cell + 0.5f) - fract(beat / 2.0f) * 9.0f) < 1.0f ? 1.0f : 0.0f;
    else on = hash21(cell + bi * 17.0f) > 0.62f ? 1.0f : 0.0f;
    const float lit = on * (0.3f + 0.7f * decay) + 0.08f;
    const float lighting = U.viewport.w;
    // Tile colors from the visuals (a spot of the frame per tile), or a rainbow without them.
    float3 vc = light.sample(smp, float2(hash21(cell), hash21(cell + 3.1f)), level(4.0f)).rgb;
    vc = vc / max(max(vc.r, max(vc.g, vc.b)), 0.2f);
    const float3 tc = mix(hue(hash21(cell * 1.7f + floor(bi / 4.0f))), vc, lighting);
    // A glossy opaque floor in front of a wall of visuals: the view ray reflects off the floor (from the eye's
    // mirror image below it) and meets the wall at z = wallZ; what the camera sees at that point of the wall is
    // what the floor shows there.
    const float3 E = U.eye.xyz;
    const float3 P = float3(xz.x, 0.0f, xz.y);
    const float3 Em = float3(E.x, -E.y, E.z);
    const float3 dr = P - Em;
    const float wallZ = U.floor2.y;
    float3 col = float3(0.025f, 0.025f, 0.035f);
    float reflW = 0.0f;
    float3 refl = float3(0.0f);
    if (dr.z < -1e-4f && P.z > wallZ) {
        const float3 W = P + dr * ((wallZ - P.z) / dr.z);
        const float4 c = U.viewProj * float4(W, 1.0f);
        float2 uvR = float2(c.x / c.w * 0.5f + 0.5f, 0.5f - c.y / c.w * 0.5f);
        const float edge = smoothstep(-0.02f, 0.03f, min(min(uvR.x, 1.0f - uvR.x), min(uvR.y, 1.0f - uvR.y)));
        uvR = clamp(uvR, 0.0f, 1.0f);
        constexpr sampler fs(filter::linear, address::clamp_to_edge);
        // Slightly glossy: the sharp frame with a little of its blurred copy.
        refl = mix(frame.sample(fs, uvR).rgb, light.sample(smp, uvR, level(1.0f)).rgb, 0.3f);
        // Fresnel: stronger at grazing angles (towards the back).
        const float3 V = normalize(E - P);
        reflW = (0.4f + 0.5f * pow(1.0f - saturate(V.y), 3.0f)) * mix(0.55f, 1.0f, edge);
    }
    col += tc * lit * 0.85f + refl * reflW * mix(0.35f, 1.0f, g) * (1.0f - 0.45f * lit);
    col = col * (1.0f - dref.a) + dref.rgb;
    col *= mix(0.45f, 1.0f, g);
    // Opaque to the frame's sides; meets the wall behind the dancers.
    const float hw = U.misc.w;
    const float ex = 1.0f - smoothstep(hw * 1.3f, hw * 1.55f, abs(xz.x));
    const float ez = smoothstep(wallZ, wallZ + 0.45f, xz.y) * (1.0f - smoothstep(1.2f, 2.5f, xz.y));
    const float a = U.floorP.x * ex * ez;
    float s = 0.0f;
    const int count = int(U.misc.y + 0.5f);
    for (int i = 0; i < count; ++i) {
        const float4 sh = U.shadows[i];
        const float d = length(xz - sh.xy) / sh.z;
        s = max(s, (1.0f - smoothstep(0.25f, 1.0f, d)) * sh.w);
    }
    s *= U.floorP.y;
    const float alpha = max(a, s * 0.55f);
    return StageOut{float4(col * a * (1.0f - 0.8f * s), alpha), alpha};
}

// ---- light texture and composite
struct QuadOut { float4 position [[position]]; float2 uv; };

vertex QuadOut quad_vs(uint vid [[vertex_id]]) {
    const float2 p = float2((vid << 1) & 2, vid & 2);
    QuadOut o;
    o.position = float4(p * 2.0f - 1.0f, 0.0f, 1.0f);
    o.uv = float2(p.x, 1.0f - p.y);
    return o;
}

fragment float4 light_fs(QuadOut in [[stage_in]], texture2d<float> src [[texture(0)]], constant float2& texel [[buffer(0)]]) {
    constexpr sampler smp(filter::linear, address::clamp_to_edge);
    const float2 d = texel * 0.25f;
    const float3 c = 0.25f * (src.sample(smp, in.uv + float2(-d.x, -d.y)).rgb + src.sample(smp, in.uv + float2(d.x, -d.y)).rgb +
                              src.sample(smp, in.uv + float2(-d.x, d.y)).rgb + src.sample(smp, in.uv + float2(d.x, d.y)).rgb);
    return float4(c, 1.0f);
}

// `opacity`: the whole stage fades (reforce: hiding the dancers, DancerStage::opacity).
fragment float4 composite_fs(QuadOut in [[stage_in]], texture2d<float> stage [[texture(0)]], constant float& opacity [[buffer(0)]]) {
    return stage.read(uint2(in.position.xy)) * opacity;
}

// The tracked Syphon alpha (alpha writes only): a' = opaque + (1 - opaque) * a.
fragment float4 composite_alpha_fs(QuadOut in [[stage_in]], texture2d<float> opaque [[texture(0)]],
                                   constant float& opacity [[buffer(0)]]) {
    return float4(0.0f, 0.0f, 0.0f, opaque.read(uint2(in.position.xy)).r * opacity);
}
)MSL";

struct Inst {
    float m0[4], m1[4], m2[4], m3[4];
    float n0[4], n1[4], n2[4];
    float c0[4], c1[4];
    float geo[4];
    float clip[4];
    float face[4];
    float eyes[4];
    float style[4];   // pattern0, pattern1, fabric0, fabric1 (Lens: shape, side, fabric, -)
    float ink[4];     // print color, repeat (world units)
    float faceA[4];   // eye shape, brows, mouth style, beard
    float faceB[4];   // lashes, freckles, blush, lipstick
    float lips[4];
    float beard[4];
};
static_assert(sizeof(Inst) == 304, "Inst must match the MSL struct");
static_assert(sizeof(Inst) == sizeof(float) * engine::kDancerInstFloats, "Inst is packed by engine::packDancerPart");

struct StageU {
    float viewProj[16];
    float eye[4];
    float viewport[4];
    float floorP[4];
    float misc[4];
    float floor2[4];
    float shadows[kMaxShadows][4];
};

struct CapV {
    float dir[3];
    float end;
};
static_assert(sizeof(CapV) == 16);

// The capsule mesh: a unit sphere split at the equator, the lower half at end 0 and the upper half at end 1, joined
// by a band (the two equator rings). Counter-clockwise from outside.
// Indexed: each ring's vertices once (the seam column s = segments is kept separate from s = 0, exactly as the
// triangles used it before indexing), triangles as 16-bit indices in the same order and winding.
struct CapMesh {
    std::vector<CapV> verts;
    std::vector<std::uint16_t> indices;
};
CapMesh capsuleMesh(int segments = 32, int rings = 12) {
    struct Ring {
        float y, r, end;
    };
    std::vector<Ring> rs;
    const float pi = 3.14159265358979f;
    for (int i = 0; i <= rings; ++i) {  // bottom pole -> equator
        const float a = -pi / 2 + pi / 2 * float(i) / float(rings);
        rs.push_back({std::sin(a), std::cos(a), 0});
    }
    for (int i = 0; i <= rings; ++i) {  // equator -> top pole
        const float a = pi / 2 * float(i) / float(rings);
        rs.push_back({std::sin(a), std::cos(a), 1});
    }
    CapMesh m;
    auto at = [&](const Ring& r, int s) {
        const float t = 2 * pi * float(s) / float(segments);
        // +z at s = 0, going towards +x: seen from outside with y up, this winds counter-clockwise below.
        return CapV{{r.r * std::sin(t), r.y, r.r * std::cos(t)}, r.end};
    };
    const int cols = segments + 1;
    for (const Ring& r : rs)
        for (int s = 0; s < cols; ++s) m.verts.push_back(at(r, s));
    auto idx = [&](std::size_t ring, int s) { return std::uint16_t(int(ring) * cols + s); };
    for (std::size_t i = 0; i + 1 < rs.size(); ++i) {
        for (int s = 0; s < segments; ++s) {
            const auto a = idx(i, s), b = idx(i, s + 1), c = idx(i + 1, s + 1), d = idx(i + 1, s);
            // outward winding: (a, b, c), (a, c, d)
            m.indices.insert(m.indices.end(), {a, b, c, a, c, d});
        }
    }
    return m;
}

// stage: the stage pass (a second, R8 "opaque" target, blended with max). mask: the color write mask.
MTL::RenderPipelineState* makePso(MTL::Device* dev, MTL::Library* lib, const char* vsName, const char* fsName,
                                  int samples, bool depth, bool blend, const char* label, bool stage = false,
                                  MTL::ColorWriteMask mask = MTL::ColorWriteMaskAll) {
    MTL::Function* vs = loadFunction(lib, vsName);
    MTL::Function* fs = loadFunction(lib, fsName);
    MTL::RenderPipelineDescriptor* d = MTL::RenderPipelineDescriptor::alloc()->init();
    d->setLabel(NS::String::string(label, NS::UTF8StringEncoding));
    d->setVertexFunction(vs);
    d->setFragmentFunction(fs);
    auto* ca = d->colorAttachments()->object(0);
    ca->setPixelFormat(MTL::PixelFormatBGRA8Unorm);
    if (blend) {
        ca->setBlendingEnabled(true);
        ca->setRgbBlendOperation(MTL::BlendOperationAdd);
        ca->setAlphaBlendOperation(MTL::BlendOperationAdd);
        ca->setSourceRGBBlendFactor(MTL::BlendFactorOne);
        ca->setSourceAlphaBlendFactor(MTL::BlendFactorOne);
        ca->setDestinationRGBBlendFactor(MTL::BlendFactorOneMinusSourceAlpha);
        ca->setDestinationAlphaBlendFactor(MTL::BlendFactorOneMinusSourceAlpha);
    }
    ca->setWriteMask(mask);
    if (stage) {
        auto* oa = d->colorAttachments()->object(1);
        oa->setPixelFormat(MTL::PixelFormatR8Unorm);
        oa->setBlendingEnabled(true);
        oa->setRgbBlendOperation(MTL::BlendOperationMax);
        oa->setAlphaBlendOperation(MTL::BlendOperationMax);
        oa->setSourceRGBBlendFactor(MTL::BlendFactorOne);
        oa->setDestinationRGBBlendFactor(MTL::BlendFactorOne);
        oa->setSourceAlphaBlendFactor(MTL::BlendFactorOne);
        oa->setDestinationAlphaBlendFactor(MTL::BlendFactorOne);
    }
    if (depth) d->setDepthAttachmentPixelFormat(MTL::PixelFormatDepth32Float);
    d->setRasterSampleCount(NS::UInteger(samples));
    NS::Error* err = nullptr;
    MTL::RenderPipelineState* p = dev->newRenderPipelineState(d, &err);
    d->release();
    vs->release();
    fs->release();
    if (!p)
        throw std::runtime_error(std::string("pipeline '") + label +
                                 "' failed: " + (err ? err->localizedDescription()->utf8String() : "?"));
    return p;
}

void put3(float* d, engine::dance::V3 v, float w) {
    d[0] = v.x;
    d[1] = v.y;
    d[2] = v.z;
    d[3] = w;
}

}  // namespace

DancerPipeline::DancerPipeline(MTL::Device* device, uint32_t width, uint32_t height, const DancerPipelineOptions& opts)
    : device_(device->retain()), width_(width), height_(height), maxParts_(std::clamp(opts.maxParts, 64, 8192)) {
    msaa_ = opts.msaa >= 4 ? 4 : 1;
    if (msaa_ > 1 && !device_->supportsTextureSampleCount(NS::UInteger(msaa_))) msaa_ = 1;
    const bool memoryless = device_->supportsFamily(MTL::GPUFamilyApple7);

    const CapMesh mesh = capsuleMesh();
    meshIndices_ = uint32_t(mesh.indices.size());
    mesh_ = device_->newBuffer(mesh.verts.data(), NS::UInteger(mesh.verts.size() * sizeof(CapV)),
                               MTL::ResourceStorageModeShared);
    meshIdx_ = device_->newBuffer(mesh.indices.data(), NS::UInteger(mesh.indices.size() * sizeof(std::uint16_t)),
                                  MTL::ResourceStorageModeShared);
    if (!mesh_ || !meshIdx_) throw std::runtime_error("dancer mesh allocation failed");
    for (int i = 0; i < 8; ++i) {
        MTL::Buffer* b = makeBuffer(device_, NS::UInteger(maxParts_) * sizeof(Inst), MTL::ResourceStorageModeShared);
        if (!b) throw std::runtime_error("dancer instance buffer allocation failed");
        b->setLabel(NS::String::string("dancer instances", NS::UTF8StringEncoding));
        ring_.push_back(b);
    }

    MTL::Library* lib = compileLibrary(device_, kDancerShaders, "reforce.dancer");
    bodyPso_ = makePso(device_, lib, "body_vs", "body_fs", msaa_, true, true, "dancer body", true);
    reflectPso_ = makePso(device_, lib, "body_reflect_vs", "body_reflect_fs", 1, true, false, "dancer reflection");
    outlinePso_ = makePso(device_, lib, "outline_vs", "outline_fs", msaa_, true, true, "dancer outline", true);
    floorPso_ = makePso(device_, lib, "floor_vs", "floor_fs", msaa_, true, true, "dancer floor", true);
    lightPso_ = makePso(device_, lib, "quad_vs", "light_fs", 1, false, false, "dancer light");
    // Color only: the frame's alpha is 1, or the tracked Syphon alpha (compositeAlphaPso_).
    compositePso_ = makePso(device_, lib, "quad_vs", "composite_fs", 1, false, true, "dancer composite", false,
                            MTL::ColorWriteMask(MTL::ColorWriteMaskRed | MTL::ColorWriteMaskGreen |
                                                MTL::ColorWriteMaskBlue));
    compositeAlphaPso_ = makePso(device_, lib, "quad_vs", "composite_alpha_fs", 1, false, true,
                                 "dancer composite (Syphon alpha)", false, MTL::ColorWriteMaskAlpha);
    lib->release();

    MTL::DepthStencilDescriptor* dd = MTL::DepthStencilDescriptor::alloc()->init();
    dd->setDepthCompareFunction(MTL::CompareFunctionLess);
    dd->setDepthWriteEnabled(true);
    depthWrite_ = device_->newDepthStencilState(dd);
    dd->setDepthCompareFunction(MTL::CompareFunctionAlways);
    depthFloor_ = device_->newDepthStencilState(dd);
    dd->release();

    auto tex = [&](MTL::PixelFormat f, uint32_t w, uint32_t h, bool scratch, bool mips, const char* label) {
        MTL::TextureDescriptor* td = MTL::TextureDescriptor::texture2DDescriptor(f, w, h, mips);
        if (scratch) {
            td->setUsage(MTL::TextureUsageRenderTarget);
            td->setStorageMode(memoryless ? MTL::StorageModeMemoryless : MTL::StorageModePrivate);
            if (msaa_ > 1) {
                td->setTextureType(MTL::TextureType2DMultisample);
                td->setSampleCount(NS::UInteger(msaa_));
            }
        } else {
            td->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
            td->setStorageMode(MTL::StorageModePrivate);
        }
        MTL::Texture* t = makeTexture(device_, td);
        if (!t) throw std::runtime_error(std::string(label) + " allocation failed");
        t->setLabel(NS::String::string(label, NS::UTF8StringEncoding));
        return t;
    };
    target_ = tex(MTL::PixelFormatBGRA8Unorm, width_, height_, false, false, "dancer stage");
    if (msaa_ > 1) msaaColor_ = tex(MTL::PixelFormatBGRA8Unorm, width_, height_, true, false, "dancer MSAA color");
    opaque_ = tex(MTL::PixelFormatR8Unorm, width_, height_, false, false, "dancer opaque");
    if (msaa_ > 1) msaaOpaque_ = tex(MTL::PixelFormatR8Unorm, width_, height_, true, false, "dancer MSAA opaque");
    depth_ = tex(MTL::PixelFormatDepth32Float, width_, height_, true, false, "dancer depth");
    const uint32_t lw = 256, lh = std::max<uint32_t>(16, uint32_t(std::lround(256.0 * height_ / std::max(1u, width_))));
    light_ = tex(MTL::PixelFormatBGRA8Unorm, lw, lh, false, true, "dancer light");
    // The reflection at half size: cheap, and a little soft like a glossy floor.
    const uint32_t rw = std::max(1u, width_ / 2), rh = std::max(1u, height_ / 2);
    reflect_ = tex(MTL::PixelFormatBGRA8Unorm, rw, rh, false, false, "dancer reflection");
    {
        MTL::TextureDescriptor* td = MTL::TextureDescriptor::texture2DDescriptor(MTL::PixelFormatDepth32Float, rw, rh,
                                                                                 false);
        td->setUsage(MTL::TextureUsageRenderTarget);
        td->setStorageMode(memoryless ? MTL::StorageModeMemoryless : MTL::StorageModePrivate);
        reflectDepth_ = makeTexture(device_, td);
        if (!reflectDepth_) throw std::runtime_error("dancer reflection depth allocation failed");
    }
}

DancerPipeline::~DancerPipeline() {
    for (auto* b : ring_) b->release();
    for (auto* t : {target_, msaaColor_, opaque_, msaaOpaque_, depth_, light_, reflect_, reflectDepth_})
        if (t) t->release();
    for (auto* p : {lightPso_, floorPso_, bodyPso_, reflectPso_, outlinePso_, compositePso_, compositeAlphaPso_})
        if (p) p->release();
    if (depthWrite_) depthWrite_->release();
    if (depthFloor_) depthFloor_->release();
    if (mesh_) mesh_->release();
    if (meshIdx_) meshIdx_->release();
    device_->release();
}

bool DancerPipeline::encode(MTL::CommandBuffer* cmd, MTL::Texture* output, const DancerStage& st, bool trackAlpha) {
    lastParts_ = 0;
    if (st.bodyCount <= 0 && st.floor <= 0.001f) return false;

    // ---- instances
    MTL::Buffer* buf = ring_[ringNext_];
    ringNext_ = (ringNext_ + 1) % ring_.size();
    Inst* inst = static_cast<Inst*>(buf->contents());
    int n = 0;
    StageU U{};
    int shadows = 0;
    auto addShadow = [&](engine::dance::V3 p, float r, float s) {
        if (shadows >= kMaxShadows) return;
        U.shadows[shadows][0] = p.x;
        U.shadows[shadows][1] = p.z;
        U.shadows[shadows][2] = r;
        U.shadows[shadows][3] = s;
        ++shadows;
    };
    for (int b = 0; b < st.bodyCount && b < int(st.bodies.size()); ++b) {
        const auto& body = st.bodies[std::size_t(b)];
        for (const auto& p : body.parts) {
            if (n >= maxParts_) break;
            engine::packDancerPart(p, body, reinterpret_cast<float*>(&inst[n++]));
        }
        const float scale = std::max(0.0f, body.headCenter.y) / 0.82f;
        addShadow(body.rootFloor, 0.24f * std::min(1.0f, scale), 0.75f);
        for (const auto& ft : body.feet) addShadow(ft, 0.1f, 0.7f * std::clamp(1.0f - (ft.y - 0.05f) / 0.3f, 0.0f, 1.0f));
    }
    lastParts_ = n;

    std::memcpy(U.viewProj, st.cam.viewProj.m, sizeof(U.viewProj));
    put3(U.eye, st.cam.eye, st.cam.proj11);
    U.viewport[0] = float(width_);
    U.viewport[1] = float(height_);
    U.viewport[2] = st.outline * float(height_) / 1080.0f;
    U.viewport[3] = std::clamp(st.lighting, 0.0f, 1.0f);
    U.floorP[0] = std::clamp(st.floor, 0.0f, 1.0f);
    U.floorP[1] = std::clamp(st.shadows, 0.0f, 1.0f);
    U.floorP[2] = float(std::fmod(st.beat, 4096.0));
    U.floorP[3] = float(st.pattern & 3);
    U.misc[0] = st.pulse;
    U.misc[1] = float(shadows);
    const engine::dance::V3 hz = engine::projectStage(st.cam, {0, 0, -6.0f});
    U.misc[2] = std::isfinite(hz.y) ? hz.y : 0.5f;
    U.misc[3] = float(st.cam.halfWidth);
    U.floor2[1] = -2.0f;  // the wall of visuals stands just where the floor fades out behind the dancers
    U.floor2[2] = std::clamp(st.reflection, 0.0f, 1.0f);

    // ---- 1. light texture from the finished frame
    {
        MTL::RenderPassDescriptor* pass = MTL::RenderPassDescriptor::renderPassDescriptor();
        auto* att = pass->colorAttachments()->object(0);
        att->setTexture(light_);
        att->setLoadAction(MTL::LoadActionDontCare);
        att->setStoreAction(MTL::StoreActionStore);
        MTL::RenderCommandEncoder* enc = makeRenderEncoder(cmd, pass);
        enc->setLabel(NS::String::string("dancer light", NS::UTF8StringEncoding));
        enc->setRenderPipelineState(lightPso_);
        enc->setFragmentTexture(output, 0);
        const float texel[2] = {1.0f / float(light_->width()), 1.0f / float(light_->height())};
        enc->setFragmentBytes(texel, sizeof(texel), 0);
        enc->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(3));
        enc->endEncoding();
        MTL::BlitCommandEncoder* blit = cmd->blitCommandEncoder();
        blit->generateMipmaps(light_);
        blit->endEncoding();
    }

    // ---- 1b. the dancers' reflection (opaque, nearest surface only), for the floor to blend in
    if (U.floor2[2] > 0.001f && U.floorP[0] > 0.001f && n > 0) {
        MTL::RenderPassDescriptor* pass = MTL::RenderPassDescriptor::renderPassDescriptor();
        auto* att = pass->colorAttachments()->object(0);
        att->setTexture(reflect_);
        att->setLoadAction(MTL::LoadActionClear);
        att->setClearColor(MTL::ClearColor(0, 0, 0, 0));
        att->setStoreAction(MTL::StoreActionStore);
        auto* da = pass->depthAttachment();
        da->setTexture(reflectDepth_);
        da->setLoadAction(MTL::LoadActionClear);
        da->setClearDepth(1.0);
        da->setStoreAction(MTL::StoreActionDontCare);
        MTL::RenderCommandEncoder* enc = makeRenderEncoder(cmd, pass);
        enc->setLabel(NS::String::string("dancer reflection", NS::UTF8StringEncoding));
        enc->setFrontFacingWinding(MTL::WindingCounterClockwise);
        enc->setRenderPipelineState(reflectPso_);
        enc->setDepthStencilState(depthWrite_);
        enc->setCullMode(MTL::CullModeNone);
        enc->setVertexBuffer(mesh_, 0, 0);
        enc->setVertexBuffer(buf, 0, 1);
        enc->setVertexBytes(&U, sizeof(U), 2);
        enc->setFragmentBuffer(buf, 0, 0);
        enc->setFragmentBytes(&U, sizeof(U), 1);
        enc->setFragmentTexture(light_, 0);
        enc->drawIndexedPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(meshIndices_), MTL::IndexTypeUInt16,
                                   meshIdx_, NS::UInteger(0), NS::UInteger(n));
        enc->endEncoding();
    } else {
        U.floor2[2] = 0;  // floor_fs: no reflection to blend
    }

    // ---- 2. stage
    {
        MTL::RenderPassDescriptor* pass = MTL::RenderPassDescriptor::renderPassDescriptor();
        auto* att = pass->colorAttachments()->object(0);
        att->setLoadAction(MTL::LoadActionClear);
        att->setClearColor(MTL::ClearColor(0, 0, 0, 0));
        if (msaa_ > 1) {
            att->setTexture(msaaColor_);
            att->setResolveTexture(target_);
            att->setStoreAction(MTL::StoreActionMultisampleResolve);
        } else {
            att->setTexture(target_);
            att->setStoreAction(MTL::StoreActionStore);
        }
        auto* oa = pass->colorAttachments()->object(1);
        oa->setLoadAction(MTL::LoadActionClear);
        oa->setClearColor(MTL::ClearColor(0, 0, 0, 0));
        if (msaa_ > 1) {
            oa->setTexture(msaaOpaque_);
            oa->setResolveTexture(opaque_);
            oa->setStoreAction(MTL::StoreActionMultisampleResolve);
        } else {
            oa->setTexture(opaque_);
            oa->setStoreAction(MTL::StoreActionStore);
        }
        auto* da = pass->depthAttachment();
        da->setTexture(depth_);
        da->setLoadAction(MTL::LoadActionClear);
        da->setClearDepth(1.0);
        da->setStoreAction(MTL::StoreActionDontCare);
        MTL::RenderCommandEncoder* enc = makeRenderEncoder(cmd, pass);
        enc->setLabel(NS::String::string("dancers", NS::UTF8StringEncoding));
        enc->setFrontFacingWinding(MTL::WindingCounterClockwise);
        // Floor and shadows (no depth: everything else stands on it).
        if (shadows > 0 || U.floorP[0] > 0.001f) {
            enc->setRenderPipelineState(floorPso_);
            enc->setDepthStencilState(depthFloor_);
            enc->setCullMode(MTL::CullModeNone);
            enc->setVertexBytes(&U, sizeof(U), 2);
            enc->setFragmentBytes(&U, sizeof(U), 1);
            enc->setFragmentTexture(light_, 0);
            enc->setFragmentTexture(output, 1);
            enc->setFragmentTexture(reflect_, 2);
            enc->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(6));
        }
        if (n > 0) {
            enc->setDepthStencilState(depthWrite_);
            enc->setVertexBuffer(mesh_, 0, 0);
            enc->setVertexBuffer(buf, 0, 1);
            enc->setVertexBytes(&U, sizeof(U), 2);
            enc->setFragmentBuffer(buf, 0, 0);
            enc->setFragmentBytes(&U, sizeof(U), 1);
            enc->setFragmentTexture(light_, 0);
            // Bodies: hair shells and the skirt are two-sided (their cut-open insides show), the rest culled.
            enc->setRenderPipelineState(bodyPso_);
            enc->setCullMode(MTL::CullModeNone);
            enc->drawIndexedPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(meshIndices_), MTL::IndexTypeUInt16, meshIdx_,
                                       NS::UInteger(0), NS::UInteger(n));
            if (U.viewport[2] > 0.01f) {
                enc->setRenderPipelineState(outlinePso_);
                enc->setCullMode(MTL::CullModeFront);
                enc->drawIndexedPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(meshIndices_),
                                           MTL::IndexTypeUInt16, meshIdx_, NS::UInteger(0), NS::UInteger(n));
            }
        }
        enc->endEncoding();
    }

    // ---- 3. composite over the frame
    {
        MTL::RenderPassDescriptor* pass = MTL::RenderPassDescriptor::renderPassDescriptor();
        auto* att = pass->colorAttachments()->object(0);
        att->setTexture(output);
        att->setLoadAction(MTL::LoadActionLoad);
        att->setStoreAction(MTL::StoreActionStore);
        MTL::RenderCommandEncoder* enc = makeRenderEncoder(cmd, pass);
        enc->setLabel(NS::String::string("dancer composite", NS::UTF8StringEncoding));
        const float opacity = std::clamp(st.opacity, 0.0f, 1.0f);
        enc->setRenderPipelineState(compositePso_);
        enc->setFragmentTexture(target_, 0);
        enc->setFragmentBytes(&opacity, sizeof opacity, 0);
        enc->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(3));
        if (trackAlpha) {
            enc->setRenderPipelineState(compositeAlphaPso_);
            enc->setFragmentTexture(opaque_, 0);
            enc->setFragmentBytes(&opacity, sizeof opacity, 0);
            enc->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(3));
        }
        enc->endEncoding();
    }
    return true;
}

}  // namespace reforce::render
