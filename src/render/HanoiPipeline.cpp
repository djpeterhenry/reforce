#include "render/HanoiPipeline.hpp"

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

namespace hn = engine::hanoi;

constexpr const char* kHanoiShaders = R"MSL(
#include <metal_stdlib>
using namespace metal;

// ---- geometry: a lathe around the segment [-L, L] on the part's x axis (L = 0: a circle), from a profile of
// (rho, y) points (rho: the distance from the segment). The perimeter: 2 (na + 1) points, a half circle round each end.
struct HanoiV {
    float4x4 mvp;    // part -> clip
    float4x4 model;  // part -> world
    float4 lathe;    // x: L, y: profile points, z: 1 closed profile, w: na
    float4 prof[48]; // (rho, y, -, -)
};
struct HanoiOut { float4 position [[position]]; float3 wp; float3 lp; };

static float3 lathePoint(constant HanoiV& V, int i, int j) {
    const int na = int(V.lathe.w);
    const int P = 2 * (na + 1);
    i = i % P;
    float a, cx;
    if (i <= na) {
        a = -M_PI_F / 2 + M_PI_F * float(i) / float(na);
        cx = V.lathe.x;
    } else {
        a = M_PI_F / 2 + M_PI_F * float(i - na - 1) / float(na);
        cx = -V.lathe.x;
    }
    const float2 pr = V.prof[j].xy;
    return float3(cx + pr.x * cos(a), pr.y, pr.x * sin(a));
}

vertex HanoiOut hanoi_vs(uint vid [[vertex_id]], constant HanoiV& V [[buffer(1)]]) {
    const int na = int(V.lathe.w), P = 2 * (na + 1), pts = int(V.lathe.y);
    const int q = int(vid) / 6, c = int(vid) % 6;
    const int i = q % P, j = q / P;
    // Counter-clockwise from outside: (i, j) (i, j + 1) (i + 1, j + 1), (i, j) (i + 1, j + 1) (i + 1, j).
    const int di[6] = {0, 0, 1, 0, 1, 1};
    const int dj[6] = {0, 1, 1, 0, 1, 0};
    int jj = j + dj[c];
    if (jj >= pts) jj -= pts;
    const float3 p = lathePoint(V, i + di[c], jj);
    HanoiOut o;
    o.position = V.mvp * float4(p, 1.0f);
    o.wp = (V.model * float4(p, 1.0f)).xyz;
    o.lp = p;
    return o;
}

// ---- optics
struct HanoiF {
    float4x4 toLocal;  // world -> part
    float4x4 model;    // part -> world
    float4x4 vp;       // world -> clip
    float4 cam;        // xyz: the camera (world), w: 1 perspective
    float4 tint;       // rgb: the glass's color, a: the tint amount
    float4 shape;      // disc: Rc, a (half width), b (half height); peg: radius, bottom, top (round top's center);
                       // base: L, R, T, bevel
    float2 size;       // output px
    float zBack;       // the visuals' plane (world z)
    float eta;         // index of refraction (green)
    float disp;        // added per channel (red -, blue +)
    float frostMix;    // the frosted share
    float spec;
    float glint;
    float thick;       // the tint's reference length (part units)
    int kind;          // 0 disc, 1 peg, 2 base
    float frostLod;    // the frost: the half-size copy's level (Fros)
    float pad1;
};

static float sdPart(float3 p, constant HanoiF& F) {
    if (F.kind == 0) {
        float2 q = float2(length(p.xz) - F.shape.x, p.y);
        const float e = max(F.shape.y - F.shape.z, 0.0f);
        q.x -= clamp(q.x, -e, e);
        return length(q) - F.shape.z;
    }
    if (F.kind == 1) {
        float3 a = p;
        a.y -= clamp(p.y, F.shape.y, F.shape.z);
        return length(a) - F.shape.x;
    }
    const float rho = length(float2(p.x - clamp(p.x, -F.shape.x, F.shape.x), p.z));
    const float2 c = float2(rho, p.y + 0.5f * F.shape.z);
    const float2 h = float2(F.shape.y, 0.5f * F.shape.z) - F.shape.w;
    const float2 d = abs(c) - h;
    return length(max(d, 0.0f)) + min(max(d.x, d.y), 0.0f) - F.shape.w;
}

// The field's gradient (outward normal), analytic.
static float3 nrmPart(float3 p, constant HanoiF& F) {
    if (F.kind == 0) {
        const float rho = max(length(p.xz), 1e-5f);
        float2 q = float2(rho - F.shape.x, p.y);
        const float e = max(F.shape.y - F.shape.z, 0.0f);
        q.x -= clamp(q.x, -e, e);
        const float2 g = length(q) > 1e-6f ? normalize(q) : float2(0.0f, sign(p.y) + (p.y == 0.0f));
        return float3(g.x * p.x / rho, g.y, g.x * p.z / rho);
    }
    if (F.kind == 1) {
        float3 a = p;
        a.y -= clamp(p.y, F.shape.y, F.shape.z);
        return length(a) > 1e-6f ? normalize(a) : float3(0.0f, 1.0f, 0.0f);
    }
    const float2 off = float2(p.x - clamp(p.x, -F.shape.x, F.shape.x), p.z);
    const float rho = length(off);
    const float2 dir = rho > 1e-5f ? off / rho : float2(0.0f, 1.0f);
    const float2 c = float2(rho, p.y + 0.5f * F.shape.z);
    const float2 h = float2(F.shape.y, 0.5f * F.shape.z) - F.shape.w;
    const float2 d = abs(c) - h;
    float2 g;
    if (d.x > 0.0f && d.y > 0.0f) g = normalize(d) * float2(1.0f, sign(c.y));
    else if (d.x > d.y) g = float2(1.0f, 0.0f);
    else g = float2(0.0f, c.y >= 0.0f ? 1.0f : -1.0f);
    return normalize(float3(g.x * dir.x, g.y, g.x * dir.y));
}

// Where the ray q + t s (inside) leaves the glass, when that is simple: through a flat face of a disc or the base (the
// path staying in their solid core), or anywhere on a peg (a capsule). Else false: traced.
static bool exitAnalytic(float3 q, float3 t, constant HanoiF& F, thread float& s, thread float3& nOut) {
    if (F.kind == 1) {
        const float r = F.shape.x;
        // The rod (vertical cylinder): the far root.
        const float a = dot(t.xz, t.xz), b = dot(q.xz, t.xz), c = dot(q.xz, q.xz) - r * r;
        float sc = 1e9f;
        if (a > 1e-8f) sc = (-b + sqrt(max(b * b - a * c, 0.0f))) / a;
        const float y = q.y + t.y * sc;
        if (y <= F.shape.z) {
            s = max(sc, 0.0f);
            nOut = normalize(float3(q.x + t.x * s, 0.0f, q.z + t.z * s));
            return true;
        }
        // The round top: the far root on its sphere.
        const float3 o = q - float3(0.0f, F.shape.z, 0.0f);
        const float bb = dot(o, t), cc = dot(o, o) - r * r;
        s = max(-bb + sqrt(max(bb * bb - cc, 0.0f)), 0.0f);
        nOut = normalize(o + t * s);
        return true;
    }
    if (abs(t.y) < 1e-4f) return false;
    if (F.kind == 0) {
        const float yp = t.y < 0.0f ? -F.shape.z : F.shape.z;
        s = (yp - q.y) / t.y;
        if (!(s > 0.0f)) return false;
        const float3 e = q + t * s;
        const float core = max(F.shape.y - F.shape.z, 0.0f);
        // Both ends in the core annulus (full height) and the path never nearer the axis than its inner edge.
        if (abs(length(e.xz) - F.shape.x) > core || abs(length(q.xz) - F.shape.x) > core) return false;
        const float2 dxz = e.xz - q.xz;
        const float k = clamp(-dot(q.xz, dxz) / max(dot(dxz, dxz), 1e-8f), 0.0f, 1.0f);
        if (length(q.xz + dxz * k) < F.shape.x - core) return false;
        nOut = float3(0.0f, sign(t.y), 0.0f);
        return true;
    }
    // The base (convex): its bottom or top inside the flat part of the footprint.
    const float yp = t.y < 0.0f ? -F.shape.z : 0.0f;
    s = (yp - q.y) / t.y;
    if (!(s > 0.0f)) return false;
    const float3 e = q + t * s;
    if (length(float2(e.x - clamp(e.x, -F.shape.x, F.shape.x), e.z)) > F.shape.y - F.shape.w) return false;
    nOut = float3(0.0f, sign(t.y), 0.0f);
    return true;
}

// Where a ray leaving the glass at P (world) along D meets the visuals' plane, on the screen (uv, y down).
static float2 backdropUV(float3 P, float3 D, constant HanoiF& F) {
    const float dz = min(D.z, -0.08f);
    const float t = max((F.zBack - P.z) / dz, 0.0f);
    const float3 Q = P + float3(D.xy, dz) * t;
    const float4 c = F.vp * float4(Q, 1.0f);
    const float2 ndc = c.xy / max(c.w, 1e-4f);
    return float2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f);
}

// The frame at `uv` from the mipmapped half-size copy, at the ray's footprint (`lod` in full-size px), so minified
// samples don't shimmer.
static float3 sampleFrame(texture2d<float> halfT, float2 uv, float lod) {
    constexpr sampler tri(filter::linear, mip_filter::linear, address::clamp_to_edge);
    return halfT.sample(tri, uv, level(max(lod - 1.0f, 0.0f))).rgb;
}

fragment float4 hanoi_fs(HanoiOut in [[stage_in]], constant HanoiF& F [[buffer(0)]],
                         texture2d<float> halfT [[texture(0)]]) {
    constexpr sampler tri(filter::linear, mip_filter::linear, address::clamp_to_edge);
    const float3 Vw = F.cam.w > 0.5f ? normalize(in.wp - F.cam.xyz) : float3(0.0f, 0.0f, -1.0f);
    const float3 v = normalize((F.toLocal * float4(Vw, 0.0f)).xyz);
    // The true surface (the mesh is a polygon of it, within a hair): the mesh's point moved onto it along the normal.
    float3 n = nrmPart(in.lp, F);
    const float3 p = in.lp - n * sdPart(in.lp, F);
    if (dot(n, v) > 0.0f) n = -n;
    const float ndv = saturate(-dot(n, v));
    const float eta = max(F.eta, 1.0001f);
    // Through the glass (green): refract in, trace to where the ray leaves, refract out; a total internal reflection
    // bounces once more inside.
    float3 t = refract(v, n, 1.0f / eta);
    float3 q = p;
    float len = 0.0f, dim = 1.0f;
    float3 nOut = n, outDir = t;
    bool straight = true;
    for (int bounce = 0; bounce < 2; ++bounce) {
        float hi;
        if (!exitAnalytic(q, t, F, hi, nOut)) {
            if (bounce > 0) break;  // (after a total internal reflection: only a simple way out is followed)
            // Traced: sphere tracing inside the field (it never steps past the surface), then bisected.
            float s = 2e-3f, prev = 0.0f;
            for (int i = 0; i < 32; ++i) {
                const float d = sdPart(q + t * s, F);
                if (d >= 0.0f) break;
                prev = s;
                s += max(-d, 4e-3f);
            }
            float lo = prev;
            hi = s;
            for (int i = 0; i < 5; ++i) {
                const float m = 0.5f * (lo + hi);
                if (sdPart(q + t * m, F) < 0.0f) lo = m; else hi = m;
            }
            nOut = nrmPart(q + t * hi, F);
        }
        q += t * hi;
        len += hi;
        const float3 o = refract(t, -nOut, eta);
        if (dot(o, o) > 1e-6f) {
            outDir = o;
            break;
        }
        straight = false;
        t = reflect(t, -nOut);
        outDir = t;
        dim *= 0.8f;
    }
    // Per channel (dispersion: blue bends more), when the ray went straight through.
    const float3 P = (F.model * float4(q, 1.0f)).xyz;
    float3 dirs[3];
    for (int c = 0; c < 3; ++c) {
        float3 o = outDir;
        if (straight) {
            const float e = eta + F.disp * float(c - 1);
            const float3 tc = refract(v, n, 1.0f / e);
            const float3 oc = refract(tc, -nOut, e);
            if (dot(oc, oc) > 1e-6f) o = oc;
        }
        dirs[c] = normalize((F.model * float4(o, 0.0f)).xyz);
    }
    const float2 uvG = backdropUV(P, dirs[1], F);
    const float2 uvR = backdropUV(P, dirs[0], F), uvB = backdropUV(P, dirs[2], F);
    // The ray's footprint in full-size px picks the detail (no shimmer where the glass minifies).
    const float2 dx = dfdx(uvG) * F.size, dy = dfdy(uvG) * F.size;
    const float lod = clamp(log2(max(max(length(dx), length(dy)), 1.0f)), 0.0f, 8.0f);
    float3 col = float3(sampleFrame(halfT, uvR, lod).r, sampleFrame(halfT, uvG, lod).g, sampleFrame(halfT, uvB, lod).b);
    // Slightly frosted: a share of the view blurred (the half-size copy's coarser levels).
    col = mix(col, halfT.sample(tri, uvG, level(max(F.frostLod, lod - 1.0f))).rgb, F.frostMix);
    // Rays that leave the frame see the dark room.
    const float beyond = max(max(-uvG.x, uvG.x - 1.0f), max(-uvG.y, uvG.y - 1.0f));
    col *= mix(0.06f, 1.0f, 1.0f - smoothstep(0.0f, 0.12f, beyond));
    // Colored glass: absorbed along the path (thicker = deeper).
    const float k = F.tint.a * len / max(F.thick, 1e-3f);
    col *= pow(max(F.tint.rgb, float3(0.02f)), float3(k)) * dim;
    // The surface: Fresnel reflecting a dim room, a soft highlight from the upper left, darker rims (so the glass reads
    // over white too), and a landing's flash on the rim.
    const float3 nw = normalize((transpose(F.toLocal) * float4(n, 0.0f)).xyz);
    const float3 R = reflect(Vw, nw);
    const float f0 = pow((eta - 1.0f) / (eta + 1.0f), 2.0f);
    const float fr = f0 + (1.0f - f0) * pow(1.0f - ndv, 5.0f);
    const float3 env = mix(float3(0.05f, 0.055f, 0.065f), float3(0.55f, 0.58f, 0.62f), smoothstep(-0.2f, 0.9f, R.y));
    const float3 L = normalize(float3(-0.45f, 0.6f, 0.65f));
    const float rl = saturate(dot(R, L));
    const float hl = F.spec * (1.6f * pow(rl, 90.0f) + 0.22f * pow(rl, 8.0f));
    col *= 0.72f + 0.28f * smoothstep(0.0f, 0.45f, ndv);
    // (The reflections take a little of the glass's color, so the tints show over black too.)
    const float3 tinge = mix(float3(1.0f), F.tint.rgb, 0.6f * saturate(F.tint.a * 1.6f));
    col = col * (1.0f - fr) + fr * env * tinge + hl;
    // A sheen along the rims (the room's light caught by the curved glass), so the edges read over black too.
    col += float3(0.85f, 0.9f, 1.0f) * tinge * (0.28f * pow(1.0f - ndv, 3.0f));
    col += F.glint * (0.12f + 0.55f * pow(1.0f - ndv, 2.0f));
    return float4(saturate(col), 1.0f);
}

// The frame at half size (4 bilinear taps: a 4 x 4 box) into the copy's level 0.
struct FullOut { float4 position [[position]]; float2 uv; };
vertex FullOut hanoi_full_vs(uint vid [[vertex_id]]) {
    const float2 p = float2((vid << 1) & 2, vid & 2);
    FullOut o;
    o.position = float4(p * 2.0f - 1.0f, 0.0f, 1.0f);
    o.uv = float2(p.x, 1.0f - p.y);
    return o;
}
fragment float4 hanoi_down_fs(FullOut in [[stage_in]], texture2d<float> src [[texture(0)]]) {
    constexpr sampler smp(filter::linear, address::clamp_to_edge);
    const float2 d = 0.5f / float2(src.get_width(), src.get_height());
    return 0.25f * (src.sample(smp, in.uv + float2(-d.x, -d.y)) + src.sample(smp, in.uv + float2(d.x, -d.y)) +
                    src.sample(smp, in.uv + float2(-d.x, d.y)) + src.sample(smp, in.uv + float2(d.x, d.y)));
}
)MSL";

struct HanoiV {
    float mvp[16], model[16];
    float lathe[4];
    float prof[48][4];
};
static_assert(sizeof(HanoiV) == 128 + 16 + 48 * 16, "HanoiV must match the MSL struct");
struct HanoiF {
    float toLocal[16], model[16], vp[16];
    float cam[4], tint[4], shape[4];
    float size[2];
    float zBack, eta, disp, frostMix, spec, glint, thick;
    int32_t kind;
    float frostLod, pad1;
};
static_assert(sizeof(HanoiF) == 288, "HanoiF must match the MSL struct");

constexpr double kPi = 3.141592653589793;

engine::Mat4 translation(double x, double y, double z) {
    engine::Mat4 m = engine::Mat4::identity();
    m.m[12] = float(x);
    m.m[13] = float(y);
    m.m[14] = float(z);
    return m;
}
engine::Mat4 scaling(double x, double y, double z) {
    engine::Mat4 m = engine::Mat4::identity();
    m.m[0] = float(x);
    m.m[5] = float(y);
    m.m[10] = float(z);
    return m;
}
engine::Mat4 rotationZ(double a) {
    engine::Mat4 m = engine::Mat4::identity();
    const float c = float(std::cos(a)), s = float(std::sin(a));
    m.m[0] = c;
    m.m[1] = s;
    m.m[4] = -s;
    m.m[5] = c;
    return m;
}
// The inverse of an affine matrix (a 3 x 3 part and a translation).
engine::Mat4 affineInverse(const engine::Mat4& a) {
    const float* m = a.m;
    const double a00 = m[0], a01 = m[4], a02 = m[8], a10 = m[1], a11 = m[5], a12 = m[9], a20 = m[2], a21 = m[6],
                 a22 = m[10];
    const double c00 = a11 * a22 - a12 * a21, c01 = a02 * a21 - a01 * a22, c02 = a01 * a12 - a02 * a11;
    const double c10 = a12 * a20 - a10 * a22, c11 = a00 * a22 - a02 * a20, c12 = a02 * a10 - a00 * a12;
    const double c20 = a10 * a21 - a11 * a20, c21 = a01 * a20 - a00 * a21, c22 = a00 * a11 - a01 * a10;
    double det = a00 * c00 + a01 * c10 + a02 * c20;
    if (!(std::fabs(det) > 1e-20)) det = 1e-20;
    const double id = 1.0 / det;
    const double inv[3][3] = {{c00 * id, c01 * id, c02 * id}, {c10 * id, c11 * id, c12 * id}, {c20 * id, c21 * id, c22 * id}};
    engine::Mat4 r = engine::Mat4::identity();
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col) r.m[col * 4 + row] = float(inv[row][col]);
    const double t[3] = {m[12], m[13], m[14]};
    for (int row = 0; row < 3; ++row)
        r.m[12 + row] = float(-(inv[row][0] * t[0] + inv[row][1] * t[1] + inv[row][2] * t[2]));
    return r;
}

MTL::RenderPipelineState* makePso(MTL::Device* dev, MTL::Library* lib, const char* fs, int msaa, const char* label) {
    MTL::Function* v = loadFunction(lib, "hanoi_vs");
    MTL::Function* f = loadFunction(lib, fs);
    MTL::RenderPipelineDescriptor* d = MTL::RenderPipelineDescriptor::alloc()->init();
    d->setLabel(NS::String::string(label, NS::UTF8StringEncoding));
    d->setVertexFunction(v);
    d->setFragmentFunction(f);
    auto* a = d->colorAttachments()->object(0);
    a->setPixelFormat(MTL::PixelFormatRGBA8Unorm);
    d->setDepthAttachmentPixelFormat(MTL::PixelFormatDepth32Float);
    d->setRasterSampleCount(NS::UInteger(std::max(1, msaa)));
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

// Profiles (rho, y), bottom to top on the outside.
struct Profile {
    float pts[48][2];
    int count = 0;
    bool closed = false;
    void add(double rho, double y) {
        if (count < 48) {
            pts[count][0] = float(rho);
            pts[count][1] = float(y);
            ++count;
        }
    }
};
// A disc: an annulus around the hole with half-round rims (radius b), centered on y = 0.
Profile discProfile(double rc, double a, double b) {
    Profile p;
    p.closed = true;
    const double e = std::max(0.0, a - b);
    constexpr int kS = 10;
    for (int k = 0; k <= kS; ++k) {
        const double t = -kPi / 2 + kPi * k / kS;
        p.add(rc + e + b * std::cos(t), b * std::sin(t));
    }
    for (int k = 0; k <= kS; ++k) {
        const double t = kPi / 2 + kPi * k / kS;
        p.add(rc - e + b * std::cos(t), b * std::sin(t));
    }
    return p;
}
// A peg: a rod from `bottom` with a round top at `top` (the rounding's center).
Profile pegProfile(double r, double bottom, double top) {
    Profile p;
    p.add(r, bottom);
    constexpr int kS = 8;
    for (int k = 0; k <= kS; ++k) {
        const double t = kPi / 2 * k / kS;
        p.add(r * std::cos(t), top + r * std::sin(t));
    }
    return p;
}
// The base: a slab from y = -T to 0, its edges rounded (radius w), around the pegs' segment.
Profile baseProfile(double R, double T, double w) {
    Profile p;
    p.add(0, -T);
    constexpr int kS = 6;
    for (int k = 0; k <= kS; ++k) {
        const double t = -kPi / 2 + kPi / 2 * k / kS;
        p.add(R - w + w * std::cos(t), -T + w + w * std::sin(t));
    }
    for (int k = 0; k <= kS; ++k) {
        const double t = kPi / 2 * k / kS;
        p.add(R - w + w * std::cos(t), -w + w * std::sin(t));
    }
    p.add(0, 0);
    return p;
}

}  // namespace

struct HanoiPipeline::Part {
    int kind = 0;  // 0 disc, 1 peg, 2 base
    engine::Mat4 model;
    Profile profile;
    double segment = 0;  // the lathe's segment half length
    int arcs = 32;       // perimeter segments per half circle
    float shape[4] = {0, 0, 0, 0};
    std::array<float, 3> tint{1, 1, 1};
    float tintAmount = 0;
    float glint = 0;
};

engine::Mat4 hanoiModel(const engine::SolidPose& pose, int discs, double aspect) {
    const double yMid = 0.5 * (hn::pegHeight(discs) - hn::kBaseT);
    return engine::solidModel(pose, aspect) * scaling(1.0 / hn::kRadius, 1.0 / hn::kRadius, 1.0 / hn::kRadius) *
           translation(0, -yMid, 0);
}

HanoiPipeline::HanoiPipeline(SolidPipeline& solids) : solids_(solids) {
    MTL::Texture* half = solids.halfSharp();
    if (!half || solids.maxLive() < 1) throw std::runtime_error("no glass textures or solid slots");
    device_ = half->device()->retain();
    width_ = uint32_t(solids.target(0)->width());
    height_ = uint32_t(solids.target(0)->height());
    MTL::Library* lib = compileLibrary(device_, kHanoiShaders, "reforce.hanoi");
    glassPso_ = makePso(device_, lib, "hanoi_fs", solids.msaa(), "hanoi glass");
    {
        MTL::Function* v = loadFunction(lib, "hanoi_full_vs");
        MTL::Function* f = loadFunction(lib, "hanoi_down_fs");
        MTL::RenderPipelineDescriptor* pd = MTL::RenderPipelineDescriptor::alloc()->init();
        pd->setLabel(NS::String::string("hanoi downsample", NS::UTF8StringEncoding));
        pd->setVertexFunction(v);
        pd->setFragmentFunction(f);
        pd->colorAttachments()->object(0)->setPixelFormat(MTL::PixelFormatBGRA8Unorm);
        NS::Error* e = nullptr;
        downPso_ = device_->newRenderPipelineState(pd, &e);
        pd->release();
        v->release();
        f->release();
        if (!downPso_)
            throw std::runtime_error(std::string("pipeline 'hanoi downsample' failed: ") +
                                     (e ? e->localizedDescription()->utf8String() : "?"));
    }
    lib->release();
    MTL::DepthStencilDescriptor* dd = MTL::DepthStencilDescriptor::alloc()->init();
    dd->setDepthCompareFunction(MTL::CompareFunctionLess);
    dd->setDepthWriteEnabled(true);
    depth_ = device_->newDepthStencilState(dd);
    dd->release();
    MTL::TextureDescriptor* td = MTL::TextureDescriptor::texture2DDescriptor(
        MTL::PixelFormatBGRA8Unorm, std::max<uint32_t>(1, width_ / 2), std::max<uint32_t>(1, height_ / 2), true);
    td->setUsage(MTL::TextureUsageShaderRead | MTL::TextureUsageRenderTarget);
    td->setStorageMode(MTL::StorageModePrivate);
    mip_ = makeTexture(device_, td);
    if (!mip_) throw std::runtime_error("hanoi frame copy allocation failed");
    mip_->setLabel(NS::String::string("hanoi frame copy", NS::UTF8StringEncoding));
}

HanoiPipeline::~HanoiPipeline() {
    for (auto* p : {glassPso_, downPso_})
        if (p) p->release();
    if (mip_) mip_->release();
    if (depth_) depth_->release();
    if (device_) device_->release();
}

void HanoiPipeline::drawPart(MTL::RenderCommandEncoder* enc, const Part& p, const HanoiDraw& d,
                             const engine::Mat4& adj, const engine::Mat4& vp, float W, float H) {
    HanoiV v{};
    const engine::Mat4 mvp = adj * vp * p.model;
    std::copy(std::begin(mvp.m), std::end(mvp.m), v.mvp);
    std::copy(std::begin(p.model.m), std::end(p.model.m), v.model);
    v.lathe[0] = float(p.segment);
    v.lathe[1] = float(p.profile.count);
    v.lathe[2] = p.profile.closed ? 1.0f : 0.0f;
    v.lathe[3] = float(p.arcs);
    for (int i = 0; i < p.profile.count; ++i) {
        v.prof[i][0] = p.profile.pts[i][0];
        v.prof[i][1] = p.profile.pts[i][1];
    }
    const int around = 2 * (p.arcs + 1);
    const int rings = p.profile.closed ? p.profile.count : p.profile.count - 1;
    enc->setVertexBytes(&v, sizeof(v), 1);
    {
        HanoiF f{};
        const engine::Mat4 inv = affineInverse(p.model);
        std::copy(std::begin(inv.m), std::end(inv.m), f.toLocal);
        std::copy(std::begin(p.model.m), std::end(p.model.m), f.model);
        std::copy(std::begin(vp.m), std::end(vp.m), f.vp);
        f.cam[2] = float(engine::solidCameraZ(40.0));
        f.cam[3] = d.perspective ? 1.0f : 0.0f;
        for (int k = 0; k < 3; ++k) f.tint[k] = std::clamp(p.tint[std::size_t(k)], 0.0f, 1.0f);
        f.tint[3] = p.tintAmount;
        std::copy(std::begin(p.shape), std::end(p.shape), f.shape);
        f.size[0] = W;
        f.size[1] = H;
        auto fin = [](float x, float lo, float hi, float fb) { return std::isfinite(x) ? std::clamp(x, lo, hi) : fb; };
        f.zBack = -2.0f * fin(d.depth, 0.0f, 4.0f, 0.5f);
        const float bend = fin(d.bend, 0.0f, 4.0f, 1.0f);
        f.eta = 1.0f + 0.5f * bend;
        f.disp = 0.018f * bend;
        const float frost = fin(d.frost, 0.0f, 60.0f, 6.0f);
        // The frosted share grows with Fros (6: .32, 11: .42, 18: .56; at most .85), so more frost reads as frostier
        // glass, not only a blurrier 30 % (Peter 2026-11-26: "pretty frosted").
        f.frostMix = frost > 0 ? std::min(0.85f, 0.2f + 0.02f * frost) : 0.0f;
        // A box blur of 2^lod full-size px is about a Gaussian of .4 x that sigma: lod on the half-size copy.
        f.frostLod = std::max(0.0f, std::log2(std::max(1.0f, 2.5f * frost * H / 1080.0f)) - 1.0f);
        f.spec = fin(d.spec, 0.0f, 8.0f, 1.0f);
        f.glint = fin(p.glint, 0.0f, 1.0f, 0.0f);
        f.thick = float(hn::kThick);
        f.kind = p.kind;
        enc->setFragmentBytes(&f, sizeof(f), 0);
    }
    enc->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(around * rings * 6));
}

void HanoiPipeline::encode(MTL::CommandBuffer* cmd, MTL::Texture* output, const HanoiDraw& d, bool trackAlpha) {
    if (!output || !(d.opacity > 0) || d.slot < 0 || d.slot >= solids_.maxLive()) return;
    const hn::Frame& fr = d.frame;
    const int n = std::clamp(fr.discs, hn::kMinDiscs, hn::kMaxDiscs);
    const int Wi = int(width_), Hi = int(height_);
    const float W = float(width_), H = float(height_);
    auto fin = [](double x, double lo, double hi, double fb) { return std::isfinite(x) ? std::clamp(x, lo, hi) : fb; };
    // The parts, in the tower's frame.
    const double aspect = double(width_) / double(height_);
    const engine::Mat4 tower = hanoiModel(d.pose, n, aspect);
    const engine::Mat4 vp = engine::solidViewProjection(aspect, 40.0, d.perspective);
    const double pegTop = hn::pegHeight(n);
    Part base;
    base.kind = 2;
    base.model = tower;
    base.profile = baseProfile(hn::kBaseR, hn::kBaseT, hn::kBaseBevel);
    base.segment = hn::kSpacing;
    base.arcs = 24;
    base.shape[0] = float(hn::kSpacing);
    base.shape[1] = float(hn::kBaseR);
    base.shape[2] = float(hn::kBaseT);
    base.shape[3] = float(hn::kBaseBevel);
    base.tint = d.clear;
    base.tintAmount = float((d.oneGlass ? 1.0 : 0.6) * fin(d.tintAmount, 0.0, 1.0, 0.5));
    std::array<Part, hn::kPegs> pegs;
    for (int k = 0; k < hn::kPegs; ++k) {
        Part& p = pegs[std::size_t(k)];
        p.kind = 1;
        p.model = tower * translation(hn::pegX(k), 0, 0);
        const double bottom = -0.6 * hn::kBaseT, top = pegTop - hn::kPegR;
        p.profile = pegProfile(hn::kPegR, bottom, top);
        p.arcs = 12;
        p.shape[0] = float(hn::kPegR);
        p.shape[1] = float(bottom);
        p.shape[2] = float(top);
        p.tint = d.clear;
        p.tintAmount = base.tintAmount;
    }
    std::array<Part, hn::kMaxDiscs> discs;
    for (int i = 0; i < n; ++i) {
        const hn::Disc& o = fr.disc[std::size_t(i)];
        const double r = hn::discRadius(i, n);
        const double a = 0.5 * (r - hn::kHole), b = 0.5 * hn::kThick, rc = 0.5 * (r + hn::kHole);
        const double sq = fin(o.squash, 0.5, 1.5, 1.0);
        const double wide = 1.0 + 0.5 * (1.0 - sq);
        Part& p = discs[std::size_t(i)];
        p.kind = 0;
        p.model = tower * translation(fin(o.x, -10, 10, 0), fin(o.y, -10, 20, 0), fin(o.z, -10, 10, 0)) *
                  rotationZ(fin(o.tilt, -100, 100, 0)) * scaling(wide, sq, wide);
        p.profile = discProfile(rc, a, b);
        p.arcs = 32;
        p.shape[0] = float(rc);
        p.shape[1] = float(a);
        p.shape[2] = float(b);
        p.tint = d.tint[std::size_t(i)];
        p.tintAmount = float(fin(d.tintAmount, 0.0, 1.0, 0.5));
        p.glint = float(fin(o.glint, 0.0, 1.0, 0.0));
    }
    // The screen box: the parts' bounding boxes projected (the whole frame when a corner is behind the camera), so the
    // passes and the composites cost what the tower covers.
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    bool behind = false;
    auto box = [&](const engine::Mat4& m, double ax, double ay, double az, double bx, double by, double bz) {
        const engine::Mat4 mvp = vp * m;
        for (int k = 0; k < 8; ++k) {
            const auto c = engine::transform(mvp, {float(k & 1 ? bx : ax), float(k & 2 ? by : ay),
                                                   float(k & 4 ? bz : az), 1.0f});
            if (!(c[3] > 1e-4f)) {
                behind = true;
                continue;
            }
            x0 = std::min(x0, c[0] / c[3]);
            x1 = std::max(x1, c[0] / c[3]);
            y0 = std::min(y0, c[1] / c[3]);
            y1 = std::max(y1, c[1] / c[3]);
        }
    };
    const double ex = hn::kSpacing + hn::kBaseR;
    box(tower, -ex, -hn::kBaseT, -hn::kBaseR, ex, pegTop, hn::kBaseR);
    for (int i = 0; i < n; ++i) {
        const double r = hn::discRadius(i, n) * 1.1;
        box(discs[std::size_t(i)].model, -r, -r, -r, r, r, r);
    }
    int px0 = 0, py0 = 0, bw = Wi, bh = Hi;
    if (!behind) {
        if (!(x1 > -1.0f && x0 < 1.0f && y1 > -1.0f && y0 < 1.0f)) return;  // off screen
        px0 = std::clamp(int(std::floor((x0 + 1.0f) * 0.5f * W)) - 2, 0, Wi);
        const int px1 = std::clamp(int(std::ceil((x1 + 1.0f) * 0.5f * W)) + 2, 0, Wi);
        py0 = std::clamp(int(std::floor((1.0f - y1) * 0.5f * H)) - 2, 0, Hi);
        const int py1 = std::clamp(int(std::ceil((1.0f - y0) * 0.5f * H)) + 2, 0, Hi);
        bw = px1 - px0;
        bh = py1 - py0;
        if (bw <= 0 || bh <= 0) return;
    }
    // Clip space shifted so the box's top-left pixel lands on the target's (0, 0) (a translation in NDC).
    engine::Mat4 adj = engine::Mat4::identity();
    adj.m[12] = -2.0f * float(px0) / W;
    adj.m[13] = 2.0f * float(py0) / H;

    // 1. The frame at half size with mips: the refraction's minified samples and the frost.
    {
        MTL::RenderPassDescriptor* rp = MTL::RenderPassDescriptor::renderPassDescriptor();
        auto* a = rp->colorAttachments()->object(0);
        a->setTexture(mip_);
        a->setLevel(0);
        a->setLoadAction(MTL::LoadActionDontCare);
        a->setStoreAction(MTL::StoreActionStore);
        MTL::RenderCommandEncoder* enc = makeRenderEncoder(cmd, rp);
        enc->setLabel(NS::String::string("hanoi downsample", NS::UTF8StringEncoding));
        enc->setRenderPipelineState(downPso_);
        enc->setFragmentTexture(output, 0);
        enc->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(3));
        enc->endEncoding();
        MTL::BlitCommandEncoder* blit = cmd->blitCommandEncoder();
        blit->generateMipmaps(mip_);
        blit->endEncoding();
    }
    const bool msaa = solids_.msaa() > 1 && solids_.msaaColorTexture();
    MTL::Texture* target = solids_.target(d.slot);
    auto pass = [&](const char* label) {
        MTL::RenderPassDescriptor* rp = MTL::RenderPassDescriptor::renderPassDescriptor();
        auto* att = rp->colorAttachments()->object(0);
        att->setLoadAction(MTL::LoadActionClear);
        att->setClearColor(MTL::ClearColor(0, 0, 0, 0));
        if (msaa) {
            att->setTexture(solids_.msaaColorTexture());
            att->setResolveTexture(target);
            att->setStoreAction(MTL::StoreActionMultisampleResolve);
        } else {
            att->setTexture(target);
            att->setStoreAction(MTL::StoreActionStore);
        }
        auto* da = rp->depthAttachment();
        da->setTexture(solids_.depthTexture());
        da->setLoadAction(MTL::LoadActionClear);
        da->setClearDepth(1.0);
        da->setStoreAction(MTL::StoreActionDontCare);
        rp->setRenderTargetWidth(NS::UInteger(bw));
        rp->setRenderTargetHeight(NS::UInteger(bh));
        MTL::RenderCommandEncoder* enc = makeRenderEncoder(cmd, rp);
        enc->setLabel(NS::String::string(label, NS::UTF8StringEncoding));
        enc->setViewport(MTL::Viewport{0.0, 0.0, double(W), double(H), 0.0, 1.0});
        enc->setScissorRect(MTL::ScissorRect{0, 0, NS::UInteger(bw), NS::UInteger(bh)});
        enc->setDepthStencilState(depth_);
        enc->setFrontFacingWinding(MTL::WindingCounterClockwise);
        enc->setCullMode(MTL::CullModeBack);
        enc->setFragmentTexture(mip_, 0);
        return enc;
    };
    const float op = float(fin(d.opacity, 0.0, 1.0, 0.0));
    const bool opaque = trackAlpha && d.opaque;
    // 2. The glass, with depth, then over the frame.
    {
        MTL::RenderCommandEncoder* enc = pass("hanoi glass");
        enc->setRenderPipelineState(glassPso_);
        drawPart(enc, base, d, adj, vp, W, H);
        for (const Part& p : pegs) drawPart(enc, p, d, adj, vp, W, H);
        for (int i = 0; i < n; ++i) drawPart(enc, discs[std::size_t(i)], d, adj, vp, W, H);
        enc->endEncoding();
    }
    solids_.encodeGlassComposite(cmd, output, d.slot, op, opaque, px0, py0, bw, bh);
}

}  // namespace reforce::render
