#include "render/ScenePipeline.hpp"

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>

#include "engine/Perf.hpp"
#include "engine/Sprites.hpp"
#include "render/ShaderCompiler.hpp"

namespace reforce::render {
namespace {

constexpr const char* kSceneShaders = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct VOut { float4 position [[position]]; };

vertex VOut scene_fullscreen_vs(uint vid [[vertex_id]]) {
    float2 p = float2((vid << 1) & 2, vid & 2);
    VOut o;
    o.position = float4(p * 2.0 - 1.0, 0.0, 1.0);
    return o;
}

// ---- 1. flow + decay ------------------------------------------------------
// Transition blend between field A (current/from) and B (incoming), weight w
// (architecture §5a, probe #10, #18). mode 0 gforce: linear source lerp;
// 1 polar: complex ratio src/p, log-magnitude lerp + shortest-arc angle;
// 2 crossfade: sample through both fields and mix the values.
struct FlowParams { uint2 size; float decay; float weight; int mode; int active; int useMask; float aspect; int truncate;
                    float kickZoom; float kickRotate; };

// Frame coordinates of pixel p on the FlowField grid (Evaluate.cpp rowXs/rowY: edge pixels on the edges).
static float2 field_point(uint2 p, uint2 size) {
    return float2(float(p.x) / max(float(size.x) - 1.0f, 1.0f) * 2.0f - 1.0f,
                  1.0f - float(p.y) / max(float(size.y) - 1.0f, 1.0f) * 2.0f);
}
static bool in_frame(float2 s) { return abs(s.x) <= 1.0f && abs(s.y) <= 1.0f; }
// FlowFieldBuilder writes kOutOfFrame (1e9) for NaN/inf sources; anything that large is "no valid source".
static bool is_invalid(float2 s) { return !(abs(s.x) < 1.0e8f && abs(s.y) < 1.0e8f); }

// q (FlowParams.truncate): 0 = bilinear, GPU round-to-nearest store; 1 = bilinear + floor; 2 = point sample + floor;
// 3 = bilinear with 1/16-px weights + floor; 4 = bilinear with 1/256-px weights + floor (integer-math emulations).
static float sample_prev(texture2d<float, access::read> prev, float2 s, uint2 size, int q) {
    // Out of frame (also the NaN/inf sentinel and NaN itself): black (probe #13).
    if (!in_frame(s)) return 0.0f;
    const float2 sz = float2(size);
    // Inverse of the field's pixel grid (x = -1 + 2c/(W-1), y = 1 - 2r/(H-1); Evaluate.cpp rowXs/rowY):
    // the frame edges are the centers of the edge pixels, so every in-frame source lies in [0, W-1] x [0, H-1].
    const float2 px = float2((s.x + 1.0f) * 0.5f * (sz.x - 1.0f), (1.0f - s.y) * 0.5f * (sz.y - 1.0f));
    const int2 mx = int2(size) - 1;
    if (q == 2) {
        const int2 n = clamp(int2(floor(px + 0.5f)), int2(0), mx);
        return prev.read(uint2(n)).r;
    }
    float2 f0 = floor(px);
    float2 fr = px - f0;
    if (q == 3 || q == 4) {
        // Fixed-point source position (1/16 or 1/256 px), truncated. The small bias absorbs float32 error in
        // px: an exact pixel center (e.g. srcY = y) must not land 1 ulp low and pick up 15/16 of the row
        // above (sweep: "Bits Between Bits" rows decayed into the neighbor every few rows).
        const float k = q == 3 ? 16.0f : 256.0f;
        const float2 fx = floor(px * k + 0.01f);
        f0 = floor(fx / k);
        fr = (fx - f0 * k) / k;
    }
    const int2 a = clamp(int2(f0), int2(0), mx);
    const int2 b = clamp(int2(f0) + 1, int2(0), mx);
    const float v00 = prev.read(uint2(a.x, a.y)).r;
    const float v10 = prev.read(uint2(b.x, a.y)).r;
    const float v01 = prev.read(uint2(a.x, b.y)).r;
    const float v11 = prev.read(uint2(b.x, b.y)).r;
    return mix(mix(v00, v10, fr.x), mix(v01, v11, fr.x), fr.y);
}

static float2 blend_polar(float2 p, float2 a, float2 b, float w, float aspect) {
    const float2 P = float2(p.x * aspect, p.y), A = float2(a.x * aspect, a.y), B = float2(b.x * aspect, b.y);
    const float rp = length(P), ra = length(A), rb = length(B);
    const float eps = 1e-4f;
    if (!(rp > eps && ra > eps && rb > eps)) return mix(a, b, w);
    const float ta = atan2(A.y, A.x), tb = atan2(B.y, B.x);
    float d = tb - ta;
    d -= 6.28318530718f * floor(d / 6.28318530718f + 0.5f);  // shortest arc
    const float ang = ta + d * w;
    const float mag = rp * exp(mix(log(ra / rp), log(rb / rp), w));
    return float2(mag * cos(ang) / aspect, mag * sin(ang));
}

// Kick zoom (PLAN 5O.3): scale the source towards the center by (1 - zoom) and turn it, in square units.
static float2 kick_src(float2 s, constant FlowParams& P) {
    if (is_invalid(s)) return s;
    float2 q = float2(s.x * P.aspect, s.y) * (1.0f - P.kickZoom);
    const float c = cos(P.kickRotate), n = sin(P.kickRotate);
    q = float2(c * q.x - n * q.y, n * q.x + c * q.y);
    return float2(q.x / P.aspect, q.y);
}

fragment float4 flow_fs(VOut in [[stage_in]],
                        texture2d<float, access::read> prev   [[texture(0)]],
                        texture2d<float, access::read> fieldA [[texture(1)]],
                        texture2d<float, access::read> fieldB [[texture(2)]],
                        texture2d<float, access::read> mask   [[texture(3)]],
                        constant FlowParams& P [[buffer(0)]]) {
    const uint2 p = uint2(in.position.xy);
    float2 sa = fieldA.read(p).xy;
    if (P.kickZoom != 0.0f || P.kickRotate != 0.0f) sa = kick_src(sa, P);
    float v;
    if (P.active == 0) {
        v = sample_prev(prev, sa, P.size, P.truncate);
    } else {
        float2 sb = fieldB.read(p).xy;
        if (P.kickZoom != 0.0f || P.kickRotate != 0.0f) sb = kick_src(sb, P);
        float w = P.weight;
        if (P.useMask != 0) w *= clamp(mask.read(p).r, 0.0f, 1.0f);  // Union: w * clamp(ALFA) (probe #18)
        if (P.mode == 2) {
            v = mix(sample_prev(prev, sa, P.size, P.truncate), sample_prev(prev, sb, P.size, P.truncate), w);
        } else if (w <= 0.0f) {
            v = sample_prev(prev, sa, P.size, P.truncate);
        } else if (w >= 1.0f) {
            v = sample_prev(prev, sb, P.size, P.truncate);
        } else if (is_invalid(sa) || is_invalid(sb)) {
            // Only the NaN/inf sentinel blocks blending. Ordinary out-of-frame coordinates are lerped first and
            // tested afterwards in sample_prev, matching G-Force's linear source-coordinate lerp (probe #10).
            v = 0.0f;
        } else if (P.mode == 1) {
            const float2 dst = field_point(p, P.size);
            v = sample_prev(prev, blend_polar(dst, sa, sb, w, P.aspect), P.size, P.truncate);
        } else {
            v = sample_prev(prev, mix(sa, sb, w), P.size, P.truncate);  // gforce: linear source lerp (probe #10)
        }
    }
    // Linear decay of ~1/255 per frame (probe #6).
    // G-Force resamples with integer math that truncates (corpus sweep 5B.4: floor turned 20/88 FlowField
    // matches into 83/88). Truncate to the 8-bit level before the decay; "nearest" keeps hardware rounding.
    if (P.truncate != 0) v = floor(v * 255.0f + 1e-3f) / 255.0f;  // modes 1-4 truncate
    return float4(max(v - P.decay, 0.0f), 0.0, 0.0, 1.0);
}

// ---- 1b. bake an interrupted transition's blend into one field ---------------
// Same math as engine::bakeFieldBlend / blendFieldPoint (CPU reference):
// the out-of-frame sentinel (|s| >= 1e6) on either side -> plain lerp;
// polar mode only when both sides and the point are away from the center;
// a result that is not finite or not < 1e6 -> the sentinel.
static bool valid_src(float2 s) { return abs(s.x) < 1.0e6f && abs(s.y) < 1.0e6f; }
static bool is_nan_bits(float x) { return (as_type<uint>(x) & 0x7fffffffu) > 0x7f800000u; }

fragment float4 bake_fs(VOut in [[stage_in]],
                        texture2d<float, access::read> fieldA [[texture(0)]],
                        texture2d<float, access::read> fieldB [[texture(1)]],
                        texture2d<float, access::read> mask   [[texture(2)]],
                        constant FlowParams& P [[buffer(0)]]) {
    const uint2 p = uint2(in.position.xy);
    const float2 a = fieldA.read(p).xy;
    const float2 b = fieldB.read(p).xy;
    float w = P.weight;
    if (P.useMask != 0) {
        const float al = mask.read(p).r;
        w *= is_nan_bits(al) ? 0.0f : clamp(al, 0.0f, 1.0f);
    }
    float2 s;
    if (w <= 0.0f) s = a;
    else if (w >= 1.0f) s = b;
    else if (!valid_src(a) || !valid_src(b)) s = a + (b - a) * w;
    else if (P.mode == 1) {
        const float2 dst = field_point(p, P.size);
        s = blend_polar(dst, a, b, w, P.aspect);
    } else s = a + (b - a) * w;
    const bool ok = !is_nan_bits(s.x) && !is_nan_bits(s.y) && valid_src(s);
    return float4(ok ? s : float2(1.0e9f), 0.0, 1.0);
}

// ---- 2. lines (capsules, overwrite) ------------------------------------------
// Each segment is a capsule (round caps; a dot is a zero-length capsule) on an
// instanced quad. `path` identifies the polyline (path) the segment belongs to.
struct LineSeg { float2 a; float2 b; float penA; float penB; float radius; float path; };
// aa: 0 = off, 1 = analytic per segment (legacy), 2 = per path, 3 = 4x MSAA (comparison),
//     4 = per path with G-Force's rasterization (ThinLines::GForce)
struct LineParams { float2 size; float aa; float pad; float2 samples[4]; };

// G-Force maps frame -> pixel with truncation toward the center: col = W/2 + trunc(x W/2), row = H/2 + trunc(-y H/2)
// (probe #32: the run 19 rows/columns, both signs). p is reforce's continuous pixel position (FrameMapping:
// (x + 1) W/2 + 0.5); the result is the center of G-Force's pixel. Odd sizes: center = floor(W/2) (unmeasured).
static float2 snap_px(float2 p, float2 size) {
    const float2 c = floor(size * 0.5f);
    return c + trunc(p - 0.5f - size * 0.5f) + 0.5f;
}

struct LOut {
    float4 position [[position]];
    float2 a [[flat]];
    float2 b [[flat]];
    float2 pen [[flat]];
    float radius [[flat]];
    float path [[flat]];
};

vertex LOut line_vs(uint vid [[vertex_id]], uint iid [[instance_id]],
                    const device LineSeg* segs [[buffer(0)]],
                    constant LineParams& P [[buffer(1)]]) {
    LineSeg s = segs[iid];
    if (P.aa > 3.5f) {  // G-Force rasterization: endpoints snapped to pixel centers (probe #32)
        s.a = snap_px(s.a, P.size);
        s.b = snap_px(s.b, P.size);
    }
    const float2 d = s.b - s.a;
    const float len = length(d);
    const float2 dir = len > 1e-4f ? d / len : float2(1.0, 0.0);
    const float2 n = float2(-dir.y, dir.x);
    const float R = max(abs(s.radius), 0.5f) + 1.5f;  // room for the AA fringe (radius < 0: hard edge, below)
    float2 c = (vid & 1) ? s.b + dir * R : s.a - dir * R;
    c += ((vid & 2) ? 1.0f : -1.0f) * n * R;
    LOut o;
    o.position = float4(c.x / P.size.x * 2.0f - 1.0f, 1.0f - c.y / P.size.y * 2.0f, 0.0, 1.0);
    o.a = s.a;
    o.b = s.b;
    o.pen = float2(s.penA, s.penB);
    o.radius = s.radius;
    o.path = s.path;
    return o;
}

// Closest point on the segment: returns the distance, `h` = position 0..1 along it.
static float seg_dist(float2 p, float2 a, float2 b, thread float& h) {
    const float2 ba = b - a;
    const float bb = dot(ba, ba);
    h = bb > 1e-8f ? clamp(dot(p - a, ba) / bb, 0.0f, 1.0f) : 0.0f;
    return length(p - a - ba * h);
}

// Off (cov 0/1) and the legacy analytic mode: one blend per segment,
// out = pen * cov + dst * (1 - cov) (fixed-function), lines narrower than 1 px drawn 1 px wide.
fragment float4 line_fs(LOut in [[stage_in]], constant LineParams& P [[buffer(0)]]) {
    float h;
    const float dist = seg_dist(in.position.xy, in.a, in.b, h);  // pixel center, origin top-left
    const float r = max(abs(in.radius), 0.5f);
    float cov;
    // A negative radius is a hard-edged segment (organic cells): no in-between intensities at its rim, which the
    // ColorMap would paint in unrelated colors (a speckled fringe on big dots).
    if (P.aa > 0.5f && in.radius >= 0.0f) cov = clamp(r + 0.5f - dist, 0.0f, 1.0f);
    else cov = dist <= r ? 1.0f : 0.0f;
    if (cov <= 0.0f) discard_fragment();
    // Blend: out = pen * cov + dst * (1 - cov)  -> overwrite where cov == 1.
    return float4(mix(in.pen.x, in.pen.y, h), 0.0, 0.0, cov);
}

// Per-path coverage (Phase 5E). Coverage of one path is the max over its
// segments (a joint is covered once, not blended twice), and each path is
// composited over what the earlier paths left: dst = mix(base, pen, cov), with
// base = the pixel before this path touched it. The scratch attachment
// (memoryless tile memory, cleared per pass) holds (path, base, cov) per pixel;
// both attachments are read in place (programmable blending, which Apple GPUs
// order by primitive submission, so path order is kept). The pen comes from the
// segment that has the max coverage at the pixel (ties: the later segment, so
// a self-crossing overwrites as it does without AA).
// Thin lines: the footprint is at least 1 px wide, with the coverage scaled
// by width / footprint (dots: by area), so the integrated intensity stays
// proportional to the width instead of a 1-px line at full pen.
struct PathOut { float4 color [[color(0)]]; float4 scratch [[color(1)]]; };

// ThinLines::Smooth. Lines: energy per unit length = width. Dots: energy = G-Force's dot area (probe #32: column
// spans of half-height sqrt(r^2 - dx^2) for integer dx, i.e. 2r below r = 1, 2r + 4 sqrt(r^2 - 1) up to r = 2;
// ~pi r^2 beyond) over the AA disc's own energy pi (R^2 + 1/12), capped at 1 (r >= ~1.4 is unscaled).
static float line_cov_energy(float dist, float radius, bool dot) {
    const float rr = max(radius, 0.0f);
    const float r = max(rr, 0.5f);
    float k = rr / r;
    if (dot) {
        const float e = 2.0f * rr + (rr > 1.0f ? 4.0f * sqrt(rr * rr - 1.0f) : 0.0f);
        k = rr < 2.0f ? min(1.0f, e / (M_PI_F * (r * r + 1.0f / 12.0f))) : 1.0f;
    }
    return k * clamp(r + 0.5f - dist, 0.0f, 1.0f);
}

// ThinLines::GForce (probe #32): coverage = length of the capsule (a, b, r) along the pixel's center line in the
// segment's major axis (x-major and dots: the vertical line through the pixel center), clipped to the pixel.
// The capsule meets that line in one interval (it is convex): the union of the two end discs' chords and the
// body's slab, whose half-height is r sqrt(1 + m^2) (perpendicular width kept), limited to 0 <= t <= 1.
static float span_cov(float2 p, float2 a, float2 b, float r) {
    const bool xmaj = abs(b.x - a.x) >= abs(b.y - a.y);
    if (!xmaj) {
        p = p.yx;
        a = a.yx;
        b = b.yx;
    }
    float lo = 1e30f, hi = -1e30f;
    const float r2 = r * r;
    const float qa = r2 - (p.x - a.x) * (p.x - a.x), qb = r2 - (p.x - b.x) * (p.x - b.x);
    if (qa >= 0.0f) { const float c = sqrt(qa); lo = min(lo, a.y - c); hi = max(hi, a.y + c); }
    if (qb >= 0.0f) { const float c = sqrt(qb); lo = min(lo, b.y - c); hi = max(hi, b.y + c); }
    const float2 d = b - a;
    if (d.x != 0.0f) {
        const float t0 = (p.x - a.x) / d.x;  // centerline point at this u lies at parameter t0
        const float m = d.y / d.x, vc = a.y + m * (p.x - a.x), hh = r * sqrt(1.0f + m * m);
        float blo = vc - hh, bhi = vc + hh;
        if (d.y != 0.0f) {
            // t(v) = t0 + (v - vc) d.y / |d|^2: keep 0 <= t <= 1
            const float L2 = dot(d, d);
            const float v0 = vc - t0 * L2 / d.y, v1 = vc + (1.0f - t0) * L2 / d.y;
            blo = max(blo, min(v0, v1));
            bhi = min(bhi, max(v0, v1));
        } else if (t0 < 0.0f || t0 > 1.0f) {
            blo = 1e30f;
            bhi = -1e30f;
        }
        if (blo <= bhi) { lo = min(lo, blo); hi = max(hi, bhi); }
    }
    return max(0.0f, min(hi, p.y + 0.5f) - max(lo, p.y - 0.5f));
}

fragment PathOut line_path_fs(LOut in [[stage_in]], float4 dst [[color(0)]], float4 st [[color(1)]],
                              constant LineParams& P [[buffer(0)]]) {
    float h;
    const float dist = seg_dist(in.position.xy, in.a, in.b, h);
    const float cov = in.radius < 0.0f ? (dist <= -in.radius ? 1.0f : 0.0f)  // hard edge (see line_fs)
                      : P.aa > 3.5f ? span_cov(in.position.xy, in.a, in.b, max(in.radius, 0.0f))
                                    : line_cov_energy(dist, in.radius, all(in.a == in.b));
    if (cov <= 0.0f) discard_fragment();
    const bool same = st.x == in.path;
    const float base = same ? st.y : dst.r;
    if (same && cov < st.z) discard_fragment();
    PathOut o;
    o.color = float4(mix(base, mix(in.pen.x, in.pen.y, h), cov), 0.0, 0.0, 1.0);
    o.scratch = float4(in.path, base, cov, 0.0);
    return o;
}

// 4x MSAA comparison baseline: per-sample coverage of the capsule as the
// sample mask, value = pen, no blending (overwrite per sample), resolved by
// averaging.
struct MsaaOut { float4 color [[color(0)]]; uint mask [[sample_mask]]; };

fragment MsaaOut line_msaa_fs(LOut in [[stage_in]], constant LineParams& P [[buffer(0)]]) {
    const float2 px = floor(in.position.xy);
    const float r = max(abs(in.radius), 0.5f);
    uint mask = 0;
    for (uint i = 0; i < 4; ++i) {
        float hs;
        if (seg_dist(px + P.samples[i], in.a, in.b, hs) <= r) mask |= 1u << i;
    }
    if (mask == 0) discard_fragment();
    float h;
    seg_dist(in.position.xy, in.a, in.b, h);
    MsaaOut o;
    o.color = float4(mix(in.pen.x, in.pen.y, h), 0.0, 0.0, 1.0);
    o.mask = mask;
    return o;
}

// Color edges (reforce.LineEdges = color): this step's line coverage for the display. Premultiplied over (One,
// OneMinusSourceAlpha), so each pixel ends with the topmost line's (pen x cov, cov); hard-edged segments have no
// in-between edge to fix and are left out. The colorize pass mixes the ColorMap colors of the pen and of the
// intensity under the line by the coverage, instead of looking up the blended intensity (which on a non-monotonic
// ColorMap lands on unrelated colors: multi-colored fringes).
fragment float4 line_cover_fs(LOut in [[stage_in]], constant LineParams& P [[buffer(0)]]) {
    if (in.radius < 0.0f) discard_fragment();
    float h;
    const float dist = seg_dist(in.position.xy, in.a, in.b, h);
    const float cov = P.aa > 3.5f ? min(1.0f, span_cov(in.position.xy, in.a, in.b, max(in.radius, 0.0f)))
                    : P.aa > 1.5f ? line_cov_energy(dist, in.radius, all(in.a == in.b))
                                  : clamp(max(in.radius, 0.5f) + 0.5f - dist, 0.0f, 1.0f);
    if (cov <= 0.0f) discard_fragment();
    return float4(mix(in.pen.x, in.pen.y, h) * cov, 0.0, 0.0, cov);
}

// ---- 2b. sprites ---------------------------------------------------------------
// Texture: r = straight luminance, g = alpha (image alpha, or luminance when
// the file has none). Modes = engine::SpriteBlend; the fixed-function blend
// state per mode is chosen on the CPU (over / add / max / premultiplied over).
// Movie / live sprites (Phase 5F): `video` textures are BGRA frames; luminance
// (Rec. 601, like toLumaAlpha) is computed here, inverted per `invert`, and
// doubles as the alpha (videos have no alpha channel).
struct SpriteP { float2 size; float2 center; float2 halfSize; float pen; float opacity; int mode; int hasAlpha;
                 float threshold; int video; int invert; int op; float mix; int levels; float black; float white;
                 float gamma; int pad0; float4 uv; };
struct SOut { float4 position [[position]]; float2 uv; };

vertex SOut sprite_vs(uint vid [[vertex_id]], constant SpriteP& P [[buffer(0)]]) {
    const float2 corner = float2((vid & 1) ? 1.0f : -1.0f, (vid & 2) ? 1.0f : -1.0f);
    const float2 c = P.center + corner * P.halfSize;
    SOut o;
    o.position = float4(c.x / P.size.x * 2.0f - 1.0f, 1.0f - c.y / P.size.y * 2.0f, 0.0, 1.0);
    o.uv = mix(P.uv.xy, P.uv.zw, corner * 0.5f + 0.5f);  // (0,0) = top-left texel; P.uv = sub-rectangle
    return o;
}

static float2 sprite_la(SOut in, texture2d<float> tex, constant SpriteP& P) {
    constexpr sampler smp(filter::linear, mip_filter::linear, address::clamp_to_edge);
    float2 la;
    if (P.video == 1) {
        const float3 rgb = tex.sample(smp, in.uv).rgb;
        float l = dot(rgb, float3(0.299f, 0.587f, 0.114f));
        // Movie levels (PLAN 5K), on the movie's own luminance (before '~' inverts it).
        if (P.levels != 0) l = pow(saturate((l - P.black) / max(P.white - P.black, 1e-4f)), P.gamma);
        if (P.invert != 0) l = 1.0f - l;
        la = float2(l, l);
    } else {
        la = tex.sample(smp, in.uv).rg;
        // 3D solid targets (video == 2) hold premultiplied luminance (MSAA resolve): back to straight.
        if (P.video == 2) la.r = la.g > 0.0f ? min(la.r / la.g, 1.0f) : 0.0f;
    }
    return la;
}

fragment float4 sprite_fs(SOut in [[stage_in]], texture2d<float> tex [[texture(0)]],
                          constant SpriteP& P [[buffer(0)]]) {
    const float2 la = sprite_la(in, tex, P);
    const float lum = la.r, a = la.g;
    const float cov = P.hasAlpha != 0 ? a : 1.0f;  // transparent pixels composite over black
    // Keyed copy (probes #40-#42, the default and ThresholdCopy): replaces the target with
    // x = envelope * pen * lum where x reaches P.threshold (~10 levels; a fading Pen stamp is flat at
    // fade x Pen even on a moving field); darker pixels leave the target alone, so Pen 0 draws nothing.
    if (P.op == 1) {
        const float x = P.opacity * P.pen * lum * cov;
        if (x < P.threshold) discard_fragment();
        return float4(saturate(x), 0.0, 0.0, 1.0);
    }
    switch (P.mode) {
        case 0: return float4(P.pen * lum * cov, 0.0, 0.0, P.opacity);                // copy (over by opacity)
        case 1: if (lum * cov < P.threshold) discard_fragment();                       // threshold copy
                return float4(P.pen * lum, 0.0, 0.0, P.opacity);
        case 2: case 3: return float4(P.pen * lum * cov * P.opacity, 0.0, 0.0, 1.0);  // additive / max
        case 4: if (a < P.threshold) discard_fragment();                               // alpha mask
                return float4(P.pen * lum, 0.0, 0.0, P.opacity);
        default: return float4(P.pen * lum * a * P.opacity, 0.0, 0.0, a * P.opacity); // alpha blend (premultiplied)
    }
}

// Blends that read the target (Apple GPU framebuffer fetch), no fixed-function blending. Probe #42, with
// x = envelope * pen * lum: Selective (and an EPen-only default draw) F + 0.5 * max(0, x - F); Additive
// min(1, F + x); AlphaMask F * (1 - x); AlphaBlend F + x * (1 - F). Modes = engine::SpriteBlend.
// Sprite Mix (PLAN 5K, engine::spriteLayerPlan), p = P.mix > 0: the target under the sprite is dimmed first,
// F' = F * (1 - p * e * c) (e = envelope, c = coverage), then the draw's own formula runs on F' with the half-way
// lighten fraction 0.5 + 0.5 p. Keyed copies keep their key (F' where they do not copy). op 0 (melt's fixed-function
// draws) is computed here as sprite_fs + its blend state would.
static float sprite_mix(float2 la, float F, constant SpriteP& P) {
    const float lum = la.r, a = la.g;
    const float cov = P.hasAlpha != 0 ? a : 1.0f;
    const float e = P.opacity, p = P.mix;
    const float Fd = F * (1.0f - p * e * cov);
    if (P.op == 1) {
        const float x = e * P.pen * lum * cov;
        return x < P.threshold ? Fd : saturate(x);
    }
    if (P.op == 0) {
        switch (P.mode) {
            case 0: return mix(Fd, P.pen * lum * cov, e);                               // copy (over by opacity)
            case 1: return lum * cov < P.threshold ? Fd : mix(Fd, P.pen * lum, e);      // threshold copy
            case 2: return Fd + P.pen * lum * cov * e;                                  // additive
            case 3: return max(Fd, P.pen * lum * cov * e);                              // max
            case 4: return a < P.threshold ? Fd : mix(Fd, P.pen * lum, e);              // alpha mask
            default: return P.pen * lum * a * e + Fd * (1.0f - a * e);                  // alpha blend (premultiplied)
        }
    }
    const float x = saturate(e * P.pen * lum * cov);
    switch (P.mode) {
        case 2: return Fd + x;              // additive
        case 4: return Fd * (1.0f - x);     // alpha mask
        case 5: return Fd + x * (1.0f - Fd); // alpha blend (screen)
        default: return Fd + (0.5f + 0.5f * p) * max(0.0f, x - Fd);
    }
}

fragment float4 sprite_fetch_fs(SOut in [[stage_in]], float4 dst [[color(0)]], texture2d<float> tex [[texture(0)]],
                                constant SpriteP& P [[buffer(0)]]) {
    const float2 la = sprite_la(in, tex, P);
    if (P.mix > 0.0f) return float4(saturate(sprite_mix(la, dst.r, P)), 0.0, 0.0, 1.0);
    const float cov = P.hasAlpha != 0 ? la.g : 1.0f;
    const float x = saturate(P.opacity * P.pen * la.r * cov), F = dst.r;
    float o;
    switch (P.mode) {
        case 2: o = F + x; break;              // additive
        case 4: o = F * (1.0f - x); break;     // alpha mask
        case 5: o = F + x * (1.0f - F); break; // alpha blend (screen)
        default: o = F + 0.5f * max(0.0f, x - F); break;
    }
    return float4(saturate(o), 0.0, 0.0, 1.0);
}

// ---- 3c. full-color movie sprites (PLAN 5K, reforce.MovieSprite.Color = over) ---------------------------------
// Drawn over the colorized frame (same pass) with a normal alpha-over (blend state; RGB only, alpha stays 1, or with
// the Syphon alpha tracked and ColorKey = 0 also over on alpha: opaque there, see encodeSyphonAlpha):
// the frame's color through the movie's levels per channel, '~' inverted, alpha = P.opacity.
fragment float4 color_sprite_fs(SOut in [[stage_in]], texture2d<float> tex [[texture(0)]],
                                 constant SpriteP& P [[buffer(0)]]) {
    constexpr sampler smp(filter::linear, mip_filter::linear, address::clamp_to_edge);
    float3 c = tex.sample(smp, in.uv).rgb;
    if (P.levels != 0) c = pow(saturate((c - P.black) / max(P.white - P.black, 1e-4f)), P.gamma);
    if (P.invert != 0) c = 1.0f - c;
    return float4(saturate(c), saturate(P.opacity));
}

// ---- 3c'. opaque solids (ColorKey = 0, docs/3d-design.md): with the Syphon alpha tracked, a 3D solid's coverage
// (its RG8 target's alpha) times its opacity raises the frame's alpha (max blend, alpha only), so the whole solid,
// dark colors and all, stays opaque in the Syphon output and over the camera backdrop.
fragment float4 solid_opaque_fs(SOut in [[stage_in]], texture2d<float> tex [[texture(0)]],
                                constant SpriteP& P [[buffer(0)]]) {
    constexpr sampler smp(filter::linear, address::clamp_to_edge);
    return float4(0.0f, 0.0f, 0.0f, saturate(P.opacity * tex.sample(smp, in.uv).g));
}

// ---- 3. colorize ------------------------------------------------------------
static float3 color_of(float i, constant float4* pal, constant float* lut) {
    const float x = saturate(i) * 255.0f;
    const int k0 = min(int(x), 254);
    const float m = saturate(mix(lut[k0], lut[k0 + 1], x - float(k0)));
    const float y = m * 255.0f;
    const int j0 = min(int(y), 254);
    return saturate(mix(pal[j0].rgb, pal[j0 + 1].rgb, y - float(j0)));
}

// ---- 3c''. the rubik in color (docs/3d-design.md): drawn over the colorized frame from its solid target (resolved
// r = sticker share x sticker intensity, g = coverage, b = sticker share, a = coverage x shading; the plastic is
// intensity 0). The color is darkened by the shading (Shad), so faces shade without changing hue. Each pixel
// mixes the ColorMap colors of its sticker and of the plastic by their shares, so the anti-aliased borders blend
// the two colors instead of passing through the ColorMap's colors in between. Alpha = coverage x opacity (over;
// on the tracked alpha too when the solid has ColorKey = 0).
fragment float4 solid_color_fs(SOut in [[stage_in]], texture2d<float> tex [[texture(0)]],
                                constant SpriteP& P [[buffer(0)]], constant float4* pal [[buffer(1)]],
                                constant float* lut [[buffer(2)]]) {
    constexpr sampler smp(filter::linear, address::clamp_to_edge);
    const float4 t = tex.sample(smp, in.uv);
    const float c = t.g;
    if (c < 1.0f / 512.0f) discard_fragment();
    const float f = min(t.b, c);
    const float L = f > 1.0f / 512.0f ? t.r / f : 0.0f;
    const float shade = saturate(t.a / c);  // the puzzle's shading (Shad), applied to the color
    float3 col;
    if (P.mode == 1) {
        // fifteen: r = the tile intensity over the whole tile (numbers too), b = the share outside the numbers. The
        // numbers are the tile's color pushed far darker (the light tiles engine::TilePicker picks) or, on a map
        // without good light colors, far lighter; P.threshold = 0 dark .. 1 light (faded when the choice changes).
        const float3 tile = color_of(t.r / c, pal, lut);
        const float3 num = mix(tile * 0.15f, mix(tile, float3(1.0f), 0.8f), saturate(P.threshold));
        col = (f * tile + (c - f) * num) / c;
    } else {
        col = (f * color_of(L, pal, lut) + (c - f) * color_of(0.0f, pal, lut)) / c;
    }
    col *= shade;
    return float4(col, saturate(c * P.opacity));
}

static float3 colorize_at(texture2d<float, access::read> inten, uint2 p, constant float4* pal, constant float* lut) {
    const float i = saturate(inten.read(p).r);
    const float x = i * 255.0f;
    const int k0 = min(int(x), 254);
    const float m = saturate(mix(lut[k0], lut[k0 + 1], x - float(k0)));
    const float y = m * 255.0f;
    const int j0 = min(int(y), 254);
    return saturate(mix(pal[j0].rgb, pal[j0 + 1].rgb, y - float(j0)));
}

// Color edges: where this step's lines partly cover the pixel (edge = (pen x cov, cov), line_cover_fs), the color is
// cov x color(pen) + (1 - cov) x color(under), with the intensity under the line recovered from the blend
// I = pen cov + under (1 - cov); nearly covered pixels take the pen's color.
static float3 colorize_edges(texture2d<float, access::read> inten, texture2d<float, access::read> edge, uint2 p,
                             constant float4* pal, constant float* lut, int edges) {
    const float I = saturate(inten.read(p).r);
    if (edges != 0) {
        const float4 e = edge.read(p);
        const float cov = saturate(e.a);
        if (cov > 1.0f / 255.0f) {
            const float pen = saturate(e.r / cov);
            const float under = cov < 0.98f ? saturate((I - pen * cov) / (1.0f - cov)) : I;
            return mix(color_of(under, pal, lut), color_of(pen, pal, lut), cov);
        }
    }
    return color_of(I, pal, lut);
}

// Alpha: 1, or 0 while the Syphon alpha is tracked (the visuals are keyed; layers drawn on top raise it where they
// must stay opaque, encodeSyphonAlpha).
fragment float4 colorize_fs(VOut in [[stage_in]],
                            texture2d<float, access::read> inten [[texture(0)]],
                            texture2d<float, access::read> edge [[texture(3)]],
                            constant float4* pal [[buffer(0)]],
                            constant float* lut [[buffer(1)]],
                            constant float& alpha [[buffer(2)]],
                            constant int& edges [[buffer(4)]]) {
    return float4(colorize_edges(inten, edge, uint2(in.position.xy), pal, lut, edges), alpha);
}

// ---- 3b. colorize + color video layer (Phase 5F) ---------------------------
// Mirrors engine/VideoComposite.cpp (the CPU reference): fit, key, crossfade
// A -> B (premultiplied), then blend over or under the colorized visualizer.
struct VideoP { float4 uvA; float4 uvB; float4 key; float4 misc; int4 mode; int4 mode2; };

static float smooth_edge(float e0, float e1, float x) {
    if (e1 <= e0) return x >= e0 ? 1.0f : 0.0f;
    return smoothstep(e0, e1, x);
}

static float4 sample_video(texture2d<float> tex, float2 uv, float4 xf, constant VideoP& V) {
    const float2 s = uv * xf.xy + xf.zw;
    // Mip filtering: minified frames come with mips (render/VideoMips); sample
    // before the bounds test so the LOD derivatives stay well defined.
    constexpr sampler smp(filter::linear, mip_filter::linear, address::clamp_to_edge);
    const float3 c = tex.sample(smp, s).rgb;
    if (any(s < 0.0f) || any(s > 1.0f)) return float4(0.0f);  // outside the video (contain)
    float a = 1.0f;
    if (V.mode2.x == 1) {
        a = smooth_edge(V.misc.z, V.misc.z + max(V.key.w, 0.0f), dot(c, float3(0.299f, 0.587f, 0.114f)));
    } else if (V.mode2.x == 2) {
        const float2 cc = float2(dot(c, float3(-0.168736f, -0.331264f, 0.5f)), dot(c, float3(0.5f, -0.418688f, -0.081312f)));
        a = smooth_edge(V.key.z, V.key.z + max(V.key.w, 0.0f), length(cc - V.key.xy));
    }
    return float4(c, a);
}

static float3 blend_video(int mode, float3 base, float3 top) {
    switch (mode) {
        case 1: return min(1.0f, base + top);
        case 2: return 1.0f - (1.0f - base) * (1.0f - top);
        case 3: return base * top;
        default: return top;
    }
}

fragment float4 colorize_video_fs(VOut in [[stage_in]],
                                  texture2d<float, access::read> inten [[texture(0)]],
                                  texture2d<float> vidA [[texture(1)]],
                                  texture2d<float> vidB [[texture(2)]],
                                  constant float4* pal [[buffer(0)]],
                                  constant float* lut [[buffer(1)]],
                                  constant VideoP& V [[buffer(2)]],
                                  constant float& outAlpha [[buffer(3)]],
                                  texture2d<float, access::read> edge [[texture(3)]],
                                  constant int& edges [[buffer(4)]]) {
    const uint2 p = uint2(in.position.xy);
    const float3 v = colorize_edges(inten, edge, p, pal, lut, edges);
    const float2 uv = (float2(p) + 0.5f) / float2(inten.get_width(), inten.get_height());
    const float4 a = V.mode.x != 0 ? sample_video(vidA, uv, V.uvA, V) : float4(0.0f);
    const float4 b = V.mode.y != 0 ? sample_video(vidB, uv, V.uvB, V) : float4(0.0f);
    const float w = saturate(V.misc.x);
    const float alpha = mix(a.w, b.w, w);
    const float3 pm = mix(a.rgb * a.w, b.rgb * b.w, w);
    const float3 c = alpha > 1e-6f ? saturate(pm / alpha) : float3(0.0f);
    const float k = saturate(V.misc.y) * saturate(alpha);
    float3 o;
    if (V.mode.w == 0) {
        o = v + (blend_video(V.mode.z, v, c) - v) * k;
    } else {
        const float3 u = c * k;
        o = V.mode.z == 0 ? v + u * (1.0f - max(v.r, max(v.g, v.b))) : blend_video(V.mode.z, u, v);
    }
    return float4(saturate(o), outAlpha);
}

// ---- 3d. the Syphon output's alpha (reforce.Syphon.Alpha, docs/syphon-alpha.md) --------------------------------
// The frame's alpha holds how much must stay opaque (dancers, ColorKey = 0 movies); everything else is keyed like
// OBS's Color Key (v2) on black: alpha = clamp((distance from black - similarity) / smoothness), on the stored
// (sRGB-encoded) values as OBS measures them. Color unchanged (straight alpha) unless premultiplied.
struct KeyP { float similarity; float smoothness; int premultiply; int pad; };

fragment float4 syphon_alpha_fs(VOut in [[stage_in]], texture2d<float, access::read> src [[texture(0)]],
                                constant KeyP& K [[buffer(0)]]) {
    const float4 c = src.read(uint2(in.position.xy));
    const float k = saturate(max(length(c.rgb) - K.similarity, 0.0f) / max(K.smoothness, 1e-4f));
    const float a = saturate(c.a + (1.0f - c.a) * k);
    return float4(K.premultiply != 0 ? c.rgb * a : c.rgb, a);
}

// ---- 3e. camera backdrop (reforce.Backdrop, docs/backdrop.md) ------------------------------------------------------
// In place on the finished frame (colorized with the Syphon alpha tracked, dancers and text drawn): the backdrop shows
// through exactly where the Syphon alpha would let OBS show a layer behind reforce (syphon_alpha_fs), straight alpha
// over it in the stored (sRGB-encoded) values like OBS. The result is opaque (alpha 1).
// Body tracking (docs/body.md): the person mask (same camera coordinates) cuts the visuals by `front` so the camera
// shows the person in front of them; `show` tints the mask and marks the joints (screen uv + weight).
struct BackdropP {
    float similarity; float smoothness; float opacity; int mirror;
    float2 scale; float2 offset;   // output uv (top-left) -> video uv (engine::fitTransform)
    float front; int show; int hasMask; int joints;
};

fragment float4 backdrop_fs(VOut in [[stage_in]], float4 dst [[color(0)]], texture2d<float> cam [[texture(0)]],
                            texture2d<float> mask [[texture(1)]], constant BackdropP& P [[buffer(0)]],
                            constant float2& size [[buffer(1)]], constant float4* J [[buffer(2)]]) {
    const float k = saturate(max(length(dst.rgb) - P.similarity, 0.0f) / max(P.smoothness, 1e-4f));
    float a = saturate(dst.a + (1.0f - dst.a) * k);
    float2 uv = in.position.xy / size;
    if (P.mirror != 0) uv.x = 1.0f - uv.x;
    const float2 s = uv * P.scale + P.offset;
    constexpr sampler smp(filter::linear, mip_filter::linear, address::clamp_to_edge);
    const bool inside = all(s >= 0.0f) && all(s <= 1.0f);
    float3 c = float3(0.0f);
    if (inside) c = cam.sample(smp, s).rgb * P.opacity;
    const float m = (P.hasMask != 0 && inside) ? mask.sample(smp, s).r : 0.0f;
    a *= 1.0f - saturate(P.front) * m;
    float3 o = dst.rgb * a + c * (1.0f - a);
    if (P.show != 0) {
        o = mix(o, float3(1.0f, 0.15f, 0.15f), 0.35f * m);
        const float r = max(4.0f, size.y / 140.0f);
        for (int i = 0; i < P.joints; ++i) {
            if (J[i].z <= 0.01f) continue;
            const float d = length(in.position.xy - J[i].xy * size);
            const float cov = saturate(r - d) * J[i].z;
            o = mix(o, float3(0.2f, 1.0f, 0.3f), cov);
        }
    }
    return float4(o, 1.0f);
}

// ---- 3f. the silhouette into the trail buffer (docs/body.md) ----------------------------------------------------
// In place on the newest intensity frame after the step, so the flow field carries it from the next step on:
// fill = the mask, outline = its edge, motion = where it changed since the previous mask. I = max(I, amount * x).
struct BodyBurnP {
    float2 scale; float2 offset;   // output uv -> camera / mask uv (the backdrop's fit)
    int mirror; int mode; float amount; float pad;
    float2 texel;                  // one mask texel in uv
};

fragment float4 body_burn_fs(VOut in [[stage_in]], float4 dst [[color(0)]], texture2d<float> mask [[texture(0)]],
                             texture2d<float> prev [[texture(1)]], constant BodyBurnP& P [[buffer(0)]],
                             constant float2& size [[buffer(1)]]) {
    float2 uv = in.position.xy / size;
    if (P.mirror != 0) uv.x = 1.0f - uv.x;
    const float2 s = uv * P.scale + P.offset;
    if (any(s < 0.0f) || any(s > 1.0f)) return float4(dst.r, 0.0, 0.0, 1.0);
    constexpr sampler smp(filter::linear, address::clamp_to_edge);
    const float m = mask.sample(smp, s).r;
    float x = m;
    if (P.mode == 2) {
        const float2 t = P.texel * 1.5f;
        const float gx = mask.sample(smp, s + float2(t.x, 0)).r - mask.sample(smp, s - float2(t.x, 0)).r;
        const float gy = mask.sample(smp, s + float2(0, t.y)).r - mask.sample(smp, s - float2(0, t.y)).r;
        x = saturate(length(float2(gx, gy)) * 1.5f);
    } else if (P.mode == 3) {
        x = saturate(abs(m - prev.sample(smp, s).r) * 3.0f);
    }
    return float4(max(dst.r, saturate(P.amount * x)), 0.0, 0.0, 1.0);
}

// ---- trail copy into a buffer of the other format (live reforce.Intensity switch) ----
fragment float4 copy_fs(VOut in [[stage_in]], texture2d<float, access::read> src [[texture(0)]]) {
    return float4(src.read(uint2(in.position.xy)).r, 0.0, 0.0, 1.0);
}

// ---- seeded value noise (static demo) -----------------------------------------
static float hash21(uint2 q, uint seed) {
    uint h = q.x * 0x8da6b343u ^ q.y * 0xd8163841u ^ seed * 0xcb1ab31fu;
    h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
    return float(h & 0xffffu) / 65535.0f;
}

fragment float4 noise_fs(VOut in [[stage_in]], constant uint& seed [[buffer(0)]]) {
    const float2 g = in.position.xy / 24.0f;
    const uint2 c = uint2(floor(g));
    const float2 f = smoothstep(0.0f, 1.0f, fract(g));
    const float v = mix(mix(hash21(c, seed), hash21(c + uint2(1, 0), seed), f.x),
                        mix(hash21(c + uint2(0, 1), seed), hash21(c + uint2(1, 1), seed), f.x), f.y);
    return float4(v, 0.0, 0.0, 1.0);
}
)MSL";

struct FlowParams {
    uint32_t size[2];
    float decay;
    float weight;
    int32_t mode;
    int32_t active;
    int32_t useMask;
    float aspect;
    int32_t truncate;
    float kickZoom = 0;    // PLAN 5O.3
    float kickRotate = 0;
    int32_t pad = 0;  // the MSL struct is 48 bytes (uint2 alignment)
};
static_assert(sizeof(FlowParams) == 48, "FlowParams must match the MSL struct");
struct LineParams {
    float size[2];
    float aa;
    float pad;
    float samples[4][2];
};
struct SpriteP {
    float size[2];
    float center[2];
    float halfSize[2];
    float pen;
    float opacity;
    int32_t mode;
    int32_t hasAlpha;
    float threshold;
    int32_t video;
    int32_t invert;
    int32_t op;  // 1 = keyed copy (engine::SpriteOp)
    float mix;   // sprite Mix (> 0: drawn by sprite_fetch_fs / sprite_mix)
    int32_t levels;  // video: apply black / white / gamma (VideoLevels not identity)
    float black, white, gamma;
    int32_t pad0;
    float uv[4];  // sampled sub-rectangle: u0, v0, u1, v1
};
static_assert(sizeof(SpriteP) == 96, "SpriteP must match the MSL struct");

// Color video layer (Phase 5F, engine/VideoComposite.hpp).
struct VideoP {
    float uvA[4];   // scale.xy, offset.zw
    float uvB[4];
    float key[4];   // key Cb, key Cr, tolerance, softness
    float misc[4];  // crossfade weight, opacity, luma threshold, 0
    int32_t mode[4];   // hasA, hasB, blend, order
    int32_t mode2[4];  // key, 0, 0, 0
};

// Per-path line AA scratch: (path id, base intensity, coverage). Path ids are
// segment indices (exact in float32 up to 2^24).
constexpr MTL::PixelFormat kScratchFormat = MTL::PixelFormatRGBA32Float;

// Fixed-function blend states.
enum BlendKind : int {
    BlendNone = 0, BlendOver = 1, BlendAdd = 2, BlendMax = 3, BlendPremul = 4, BlendOverRgb = 5,
    BlendOverAll = 6,  // over on RGB and on alpha (a' = a + (1 - a) * dst.a)
    BlendAlphaMax = 7, // alpha only: a' = max(a, dst.a); RGB untouched
};

MTL::PixelFormat intensityFormat(IntensityFormat f) {
    return f == IntensityFormat::Half16 ? MTL::PixelFormatR16Float : MTL::PixelFormatR8Unorm;
}

// Extra attachment layout for the per-path line pass / MSAA comparison pass.
struct PsoLayout {
    MTL::PixelFormat scratch = MTL::PixelFormatInvalid;  // color(1); written only by the per-path line shader
    bool writeScratch = false;
    int samples = 1;
};

MTL::RenderPipelineState* makePso(MTL::Device* device, MTL::Library* lib, const char* vs, const char* fs,
                                  MTL::PixelFormat format, int blend, const char* label,
                                  const PsoLayout& layout = {}) {
    MTL::Function* v = loadFunction(lib, vs);
    MTL::Function* f = loadFunction(lib, fs);
    MTL::RenderPipelineDescriptor* desc = MTL::RenderPipelineDescriptor::alloc()->init();
    desc->setLabel(NS::String::string(label, NS::UTF8StringEncoding));
    desc->setVertexFunction(v);
    desc->setFragmentFunction(f);
    auto* att = desc->colorAttachments()->object(0);
    att->setPixelFormat(format);
    if (layout.scratch != MTL::PixelFormatInvalid) {
        auto* sc = desc->colorAttachments()->object(1);
        sc->setPixelFormat(layout.scratch);
        if (!layout.writeScratch) sc->setWriteMask(MTL::ColorWriteMaskNone);
    }
    if (layout.samples > 1) desc->setRasterSampleCount(NS::UInteger(layout.samples));
    if (blend != BlendNone) {
        att->setBlendingEnabled(true);
        att->setRgbBlendOperation(blend == BlendMax ? MTL::BlendOperationMax : MTL::BlendOperationAdd);
        switch (blend) {
            case BlendAdd:
            case BlendMax:
                att->setSourceRGBBlendFactor(MTL::BlendFactorOne);
                att->setDestinationRGBBlendFactor(MTL::BlendFactorOne);
                break;
            case BlendPremul:
                att->setSourceRGBBlendFactor(MTL::BlendFactorOne);
                att->setDestinationRGBBlendFactor(MTL::BlendFactorOneMinusSourceAlpha);
                break;
            default:
                att->setSourceRGBBlendFactor(MTL::BlendFactorSourceAlpha);
                att->setDestinationRGBBlendFactor(MTL::BlendFactorOneMinusSourceAlpha);
                break;
        }
        if (blend == BlendOverRgb)  // over, but the target's alpha is kept (color sprites on the BGRA output)
            att->setWriteMask(MTL::ColorWriteMaskRed | MTL::ColorWriteMaskGreen | MTL::ColorWriteMaskBlue);
        att->setAlphaBlendOperation(blend == BlendAlphaMax ? MTL::BlendOperationMax : MTL::BlendOperationAdd);
        att->setSourceAlphaBlendFactor(MTL::BlendFactorOne);
        att->setDestinationAlphaBlendFactor(blend == BlendOverAll ? MTL::BlendFactorOneMinusSourceAlpha
                                            : blend == BlendAlphaMax ? MTL::BlendFactorOne
                                                                     : MTL::BlendFactorZero);
        if (blend == BlendAlphaMax) att->setWriteMask(MTL::ColorWriteMaskAlpha);
    }
    NS::Error* error = nullptr;
    MTL::RenderPipelineState* pso = device->newRenderPipelineState(desc, &error);
    desc->release();
    v->release();
    f->release();
    if (!pso)
        throw std::runtime_error(std::string("pipeline '") + label +
                                 "' failed: " + (error ? error->localizedDescription()->utf8String() : "?"));
    return pso;
}

MTL::RenderCommandEncoder* beginPass(MTL::CommandBuffer* cmd, MTL::Texture* target, MTL::LoadAction load,
                                     const char* label) {
    MTL::RenderPassDescriptor* pass = MTL::RenderPassDescriptor::renderPassDescriptor();
    auto* att = pass->colorAttachments()->object(0);
    att->setTexture(target);
    att->setLoadAction(load);
    att->setClearColor(MTL::ClearColor(0, 0, 0, 1));
    att->setStoreAction(MTL::StoreActionStore);
    MTL::RenderCommandEncoder* enc = cmd->renderCommandEncoder(pass);
    enc->setLabel(NS::String::string(label, NS::UTF8StringEncoding));
    return enc;
}

}  // namespace

// ---------------------------------------------------------------- texture pool

PooledTexture::~PooledTexture() {
    if (pool_ && tex_) pool_->release(tex_);
}

TexturePool::TexturePool(MTL::Device* device, unsigned long pixelFormat, uint32_t width, uint32_t height,
                         bool renderTarget, const char* label,
                         std::shared_ptr<std::atomic<std::uint64_t>> completedSerial)
    : device_(device->retain()),
      format_(pixelFormat),
      width_(width),
      height_(height),
      renderTarget_(renderTarget),
      label_(label),
      completed_(std::move(completedSerial)) {}

TexturePool::~TexturePool() {
    for (auto& f : free_) f.tex->release();
    device_->release();
}

MTL::Texture* TexturePool::make() {
    MTL::TextureDescriptor* td =
        MTL::TextureDescriptor::texture2DDescriptor(MTL::PixelFormat(format_), width_, height_, false);
    td->setUsage(MTL::TextureUsageShaderRead | (renderTarget_ ? MTL::TextureUsageRenderTarget : 0));
    td->setStorageMode(device_->hasUnifiedMemory() ? MTL::StorageModeShared : MTL::StorageModeManaged);
    MTL::Texture* tex = device_->newTexture(td);
    if (!tex) throw std::runtime_error(std::string(label_) + " texture allocation failed");
    tex->setLabel(NS::String::string(label_, NS::UTF8StringEncoding));
    created_.fetch_add(1);
    return tex;
}

void TexturePool::reserve(int n) {
    std::lock_guard<std::mutex> lock(mutex_);
    while (int(free_.size()) < n) free_.push_back({make(), 0});
}

TextureHandle TexturePool::acquire(bool renderThread) {
    MTL::Texture* tex = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::uint64_t done = completed_->load(std::memory_order_acquire);
        // Workers leave one ready texture for the render thread (the GPU bake
        // target) and create a new one instead, off the render thread.
        std::size_t ready = 0;
        for (const auto& f : free_) ready += f.serial <= done;
        if (renderThread || ready > 1) {
            for (std::size_t i = 0; i < free_.size(); ++i)
                if (free_[i].serial <= done) {
                    tex = free_[i].tex;
                    free_.erase(free_.begin() + std::ptrdiff_t(i));
                    break;
                }
        }
    }
    if (!tex) {
        if (renderThread) perf::countRenderAlloc();
        tex = make();
    }
    return std::make_shared<PooledTexture>(shared_from_this(), tex);
}

void TexturePool::release(MTL::Texture* tex) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Reusable once the GPU has completed every step encoded so far.
    free_.push_back({tex, submitted_.load(std::memory_order_acquire)});
}

TextureHandle TexturePool::upload(const void* data, std::size_t bytesPerRow, bool renderThread) {
    TextureHandle h = acquire(renderThread);
    h->texture()->replaceRegion(MTL::Region(0, 0, width_, height_), 0, data, NS::UInteger(bytesPerRow));
    return h;
}

// ---------------------------------------------------------------- pipeline

const char* lineAaName(LineAa m) {
    switch (m) {
        case LineAa::PerPath: return "path";
        case LineAa::Analytic: return "analytic";
        case LineAa::Msaa4: return "msaa";
    }
    return "?";
}

bool parseLineAa(const std::string& s, LineAa& out) {
    for (LineAa m : {LineAa::PerPath, LineAa::Analytic, LineAa::Msaa4})
        if (s == lineAaName(m)) {
            out = m;
            return true;
        }
    return false;
}

const char* ScenePipeline::lineAaMethod() const {
    if (!opts_.antialias) return "off";
    return lineAaName(linePathPso_ ? LineAa::PerPath : lineMsaaPso_ ? LineAa::Msaa4 : LineAa::Analytic);
}

ScenePipeline::ScenePipeline(MTL::Device* device, uint32_t width, uint32_t height, const ScenePipelineOptions& opts)
    : device_(device->retain()),
      width_(width),
      height_(height),
      opts_(opts),
      completedSerial_(std::make_shared<std::atomic<std::uint64_t>>(0)) {
    const MTL::PixelFormat fmt = intensityFormat(opts.format);
    for (int i = 0; i < 2; ++i) intensity_[i] = newIntensityTexture(opts.format, i ? "intensity B" : "intensity A");

    MTL::Library* lib = compileLibrary(device, kSceneShaders, "reforce.scene");
    lib_ = lib;
    // The line AA method is fixed here (AA itself can be switched on and off
    // live): programmable blending and memoryless targets need Apple GPUs (M1 = Apple7).
    if (opts_.lineAa != LineAa::Analytic && !device->supportsFamily(MTL::GPUFamilyApple7))
        opts_.lineAa = LineAa::Analytic;
    if (opts_.lineAa == LineAa::PerPath) {
        MTL::TextureDescriptor* sd = MTL::TextureDescriptor::texture2DDescriptor(kScratchFormat, width, height, false);
        sd->setUsage(MTL::TextureUsageRenderTarget);
        sd->setStorageMode(MTL::StorageModeMemoryless);
        scratch_ = device->newTexture(sd);
        if (!scratch_) throw std::runtime_error("line AA scratch texture allocation failed");
    } else if (opts_.lineAa == LineAa::Msaa4) {
        MTL::SamplePosition pos[4];
        device->getDefaultSamplePositions(pos, 4);
        for (int i = 0; i < 4; ++i) {
            samplePos_[i][0] = pos[i].x;
            samplePos_[i][1] = pos[i].y;
        }
    }
    sets_[int(opts.format)] = makeFormatSet(opts.format);
    bindFormatSet(opts.format);
    bakePso_ = makePso(device, lib, "scene_fullscreen_vs", "bake_fs", MTL::PixelFormatRG32Float, false, "field bake");
    colorizePso_ = makePso(device, lib, "scene_fullscreen_vs", "colorize_fs", MTL::PixelFormatBGRA8Unorm, false,
                           "colorize");
    lineCoverPso_ = makePso(device, lib, "line_vs", "line_cover_fs", MTL::PixelFormatRGBA8Unorm, BlendPremul,
                            "line coverage (color edges)");
    colorizeVideoPso_ = makePso(device, lib, "scene_fullscreen_vs", "colorize_video_fs", MTL::PixelFormatBGRA8Unorm,
                                false, "colorize + video");
    colorSpritePso_ = makePso(device, lib, "sprite_vs", "color_sprite_fs", MTL::PixelFormatBGRA8Unorm, BlendOverRgb,
                               "color sprite");
    colorSpriteOpaquePso_ = makePso(device, lib, "sprite_vs", "color_sprite_fs", MTL::PixelFormatBGRA8Unorm,
                                     BlendOverAll, "color sprite (opaque in the Syphon alpha)");
    solidColorPso_ = makePso(device, lib, "sprite_vs", "solid_color_fs", MTL::PixelFormatBGRA8Unorm, BlendOverRgb,
                              "solid in color");
    solidColorOpaquePso_ = makePso(device, lib, "sprite_vs", "solid_color_fs", MTL::PixelFormatBGRA8Unorm,
                                    BlendOverAll, "solid in color (opaque in the Syphon alpha)");
    solidOpaquePso_ = makePso(device, lib, "sprite_vs", "solid_opaque_fs", MTL::PixelFormatBGRA8Unorm, BlendAlphaMax,
                              "opaque solids (Syphon alpha)");
    syphonAlphaPso_ = makePso(device, lib, "scene_fullscreen_vs", "syphon_alpha_fs", MTL::PixelFormatBGRA8Unorm,
                              BlendNone, "Syphon alpha");
    backdropPso_ = makePso(device, lib, "scene_fullscreen_vs", "backdrop_fs", MTL::PixelFormatBGRA8Unorm, BlendNone,
                           "camera backdrop");

    {
        MTL::TextureDescriptor* md = MTL::TextureDescriptor::texture2DDescriptor(MTL::PixelFormatR32Float, 1, 1, false);
        md->setUsage(MTL::TextureUsageShaderRead);
        md->setStorageMode(device->hasUnifiedMemory() ? MTL::StorageModeShared : MTL::StorageModeManaged);
        dummyMask_ = device->newTexture(md);
        const float zero = 0.f;
        dummyMask_->replaceRegion(MTL::Region(0, 0, 1, 1), 0, &zero, 4);
    }
    // Warm-up (Phase 5D): every texture and buffer the steady state needs.
    // Fields: current + incoming + bake target + preload + pending + 1 spare
    // for the GPU still reading a released one. Masks: current + pending + spare.
    fieldPool_ = std::make_shared<TexturePool>(device, MTL::PixelFormatRG32Float, width, height, true, "flow field",
                                               completedSerial_);
    maskPool_ = std::make_shared<TexturePool>(device, MTL::PixelFormatR32Float, width, height, false, "union mask",
                                              completedSerial_);
    fieldPool_->reserve(6);
    maskPool_->reserve(3);
    display_ = newIntensityTexture(opts.format, "displayed intensity");
    (void)fmt;
    // Line buffers: 2 frames in flight x up to 4 catch-up steps.
    for (int i = 0; i < 8; ++i) {
        LineBuffer lb{device->newBuffer(NS::UInteger(1u << 20),
                                        MTL::ResourceStorageModeShared | MTL::ResourceCPUCacheModeWriteCombined),
                      std::make_shared<std::atomic<bool>>(false)};
        if (!lb.buf) throw std::runtime_error("line buffer allocation failed");
        lineBuffers_.push_back(std::move(lb));
    }
    setField(engine::identityFlowField(int(width), int(height)));
}

ScenePipeline::~ScenePipeline() {
    if (warm_.valid()) {
        FormatSet w = warm_.get();
        w.release();
    }
    for (auto& fs : sets_) fs.release();
    for (auto& c : converts_) c.from->release();
    for (auto* p : {colorizePso_, colorizeVideoPso_, colorSpritePso_, colorSpriteOpaquePso_, solidOpaquePso_, solidColorPso_, solidColorOpaquePso_, syphonAlphaPso_, backdropPso_, bakePso_,
                    tileLinePso_})
        if (p) p->release();
    if (lib_) lib_->release();
    if (scratch_) scratch_->release();
    for (auto& [h, st] : spriteTex_)
        if (st.tex && !st.video) st.tex->release();
    if (display_) display_->release();
    if (edge_) edge_->release();
    if (lineCoverPso_) lineCoverPso_->release();
    for (auto* t : intensity_)
        if (t) t->release();
    bakes_.clear();
    field_[0].reset();
    field_[1].reset();
    mask_.reset();
    if (dummyMask_) dummyMask_->release();
    for (auto& lb : lineBuffers_)
        if (lb.buf) lb.buf->release();
    device_->release();
}

TextureHandle ScenePipeline::uploadField(const engine::FlowFieldGrid& grid) const {
    if (grid.width != int(width_) || grid.height != int(height_) ||
        grid.srcXY.size() != std::size_t(width_) * height_ * 2)
        throw std::runtime_error("flow field size does not match the render size");
    return fieldPool_->upload(grid.srcXY.data(), std::size_t(width_) * 8, false);
}

TextureHandle ScenePipeline::uploadMask(const engine::UnionMask& mask) const {
    if (mask.width != int(width_) || mask.height != int(height_) || mask.alpha.size() != std::size_t(width_) * height_)
        return nullptr;
    return maskPool_->upload(mask.alpha.data(), std::size_t(width_) * 4, false);
}

void ScenePipeline::setField(TextureHandle field) { field_[0] = std::move(field); }
void ScenePipeline::setIncomingField(TextureHandle field) { field_[1] = std::move(field); }

void ScenePipeline::promoteIncomingField() {
    if (!field_[1]) return;
    field_[0] = std::move(field_[1]);
    field_[1].reset();
}

void ScenePipeline::setUnionMask(TextureHandle mask) { mask_ = std::move(mask); }

TextureHandle ScenePipeline::bakeField(float w, int mode, bool useMask) {
    if (!field_[0]) return nullptr;
    Bake b;
    b.a = field_[0];
    b.b = field_[1] ? field_[1] : field_[0];
    b.mask = useMask ? mask_ : nullptr;
    b.useMask = useMask && mask_;
    b.w = w;
    b.mode = mode;
    b.out = fieldPool_->acquire(true);
    TextureHandle out = b.out;
    bakes_.push_back(std::move(b));
    field_[0] = out;
    field_[1].reset();
    mask_.reset();
    return out;
}

void ScenePipeline::markSubmitted(MTL::CommandBuffer* cmd) {
    const std::uint64_t serial = ++submittedSerial_;
    fieldPool_->setSubmittedSerial(serial);
    maskPool_->setSubmittedSerial(serial);
    auto done = completedSerial_;
    cmd->addCompletedHandler([done, serial](MTL::CommandBuffer*) {
        std::uint64_t cur = done->load(std::memory_order_relaxed);
        while (cur < serial && !done->compare_exchange_weak(cur, serial, std::memory_order_release)) {
        }
    });
}

void ScenePipeline::FormatSet::release() {
    for (auto* p : {flow, line, flowPath, linePath, flowMsaa, lineMsaa, noise, copy, sprite[0], sprite[1], sprite[2],
                    sprite[3], sprite[4], bodyBurn})
        if (p) p->release();
    if (msaa) msaa->release();
    *this = FormatSet{};
}

ScenePipeline::FormatSet ScenePipeline::makeFormatSet(IntensityFormat f) const {
    MTL::Device* device = device_;
    MTL::Library* lib = lib_;
    const MTL::PixelFormat fmt = intensityFormat(f);
    FormatSet s;
    s.flow = makePso(device, lib, "scene_fullscreen_vs", "flow_fs", fmt, false, "flow");
    s.line = makePso(device, lib, "line_vs", "line_fs", fmt, BlendOver, "lines");
    if (opts_.lineAa == LineAa::PerPath) {
        PsoLayout flow{kScratchFormat, false, 1};
        PsoLayout line{kScratchFormat, true, 1};
        s.flowPath = makePso(device, lib, "scene_fullscreen_vs", "flow_fs", fmt, BlendNone, "flow (path AA)", flow);
        s.linePath = makePso(device, lib, "line_vs", "line_path_fs", fmt, BlendNone, "lines (path AA)", line);
    } else if (opts_.lineAa == LineAa::Msaa4) {
        MTL::TextureDescriptor* sd = MTL::TextureDescriptor::texture2DDescriptor(fmt, width_, height_, false);
        sd->setUsage(MTL::TextureUsageRenderTarget);
        sd->setStorageMode(MTL::StorageModeMemoryless);
        sd->setTextureType(MTL::TextureType2DMultisample);
        sd->setSampleCount(4);
        s.msaa = device->newTexture(sd);
        if (!s.msaa) throw std::runtime_error("line AA MSAA texture allocation failed");
        PsoLayout ms{MTL::PixelFormatInvalid, false, 4};
        s.flowMsaa = makePso(device, lib, "scene_fullscreen_vs", "flow_fs", fmt, BlendNone, "flow (MSAA)", ms);
        s.lineMsaa = makePso(device, lib, "line_vs", "line_msaa_fs", fmt, BlendNone, "lines (MSAA)", ms);
    }
    s.sprite[0] = makePso(device, lib, "sprite_vs", "sprite_fs", fmt, BlendOver, "sprite over");
    s.sprite[1] = makePso(device, lib, "sprite_vs", "sprite_fs", fmt, BlendAdd, "sprite add");
    s.sprite[2] = makePso(device, lib, "sprite_vs", "sprite_fs", fmt, BlendMax, "sprite max");
    s.sprite[3] = makePso(device, lib, "sprite_vs", "sprite_fs", fmt, BlendPremul, "sprite premul");
    s.sprite[4] = makePso(device, lib, "sprite_vs", "sprite_fetch_fs", fmt, BlendNone, "sprite fetch");
    s.bodyBurn = makePso(device, lib, "scene_fullscreen_vs", "body_burn_fs", fmt, BlendNone, "body silhouette");
    s.noise = makePso(device, lib, "scene_fullscreen_vs", "noise_fs", fmt, false, "noise");
    s.copy = makePso(device, lib, "scene_fullscreen_vs", "copy_fs", fmt, false, "intensity format copy");
    s.ready = true;
    return s;
}

void ScenePipeline::bindFormatSet(IntensityFormat f) {
    const FormatSet& s = sets_[int(f)];
    flowPso_ = s.flow;
    linePso_ = s.line;
    flowPathPso_ = s.flowPath;
    linePathPso_ = s.linePath;
    flowMsaaPso_ = s.flowMsaa;
    lineMsaaPso_ = s.lineMsaa;
    msaa_ = s.msaa;
    noisePso_ = s.noise;
    for (int i = 0; i < 5; ++i) spritePso_[i] = s.sprite[i];
    bodyBurnPso_ = s.bodyBurn;
}

MTL::Texture* ScenePipeline::newIntensityTexture(IntensityFormat f, const char* label) const {
    MTL::TextureDescriptor* td = MTL::TextureDescriptor::texture2DDescriptor(intensityFormat(f), width_, height_, false);
    td->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
    td->setStorageMode(MTL::StorageModePrivate);
    MTL::Texture* t = device_->newTexture(td);
    if (!t) throw std::runtime_error("intensity texture allocation failed");
    t->setLabel(NS::String::string(label, NS::UTF8StringEncoding));
    return t;
}

void ScenePipeline::prewarmOtherFormat() {
    const IntensityFormat other =
        opts_.format == IntensityFormat::GForce8 ? IntensityFormat::Half16 : IntensityFormat::GForce8;
    if (sets_[int(other)].ready || warm_.valid()) return;
    warm_ = std::async(std::launch::async, [this, other] { return makeFormatSet(other); });
}

void ScenePipeline::setIntensityFormat(IntensityFormat f) {
    if (f == opts_.format) return;
    if (!sets_[int(f)].ready) {
        if (warm_.valid()) {
            FormatSet w = warm_.get();  // usually finished long ago
            sets_[int(f)] = w;
        } else {
            sets_[int(f)] = makeFormatSet(f);
        }
    }
    // Three textures, once per switch (not per frame).
    MTL::Texture* next[2] = {newIntensityTexture(f, "intensity A"), newIntensityTexture(f, "intensity B")};
    MTL::Texture* disp = newIntensityTexture(f, "displayed intensity");
    for (int i = 0; i < 3; ++i) perf::countRenderAlloc();
    // The current trail (and the displayed frame, which colorize may show before
    // the next step) are copied over; the other ping-pong half is overwritten by
    // the next flow pass anyway.
    if (initialized_) converts_.push_back({intensity_[current_], next[current_]});
    else intensity_[current_]->release();
    intensity_[current_ ^ 1]->release();
    if (displayValid_ && display_) converts_.push_back({display_, disp});
    else if (display_) display_->release();
    intensity_[0] = next[0];
    intensity_[1] = next[1];
    display_ = disp;
    opts_.format = f;
    bindFormatSet(f);
}

void ScenePipeline::flushConverts(MTL::CommandBuffer* cmd) {
    for (const Convert& c : converts_) {
        const int set = c.to->pixelFormat() == MTL::PixelFormatR16Float ? int(IntensityFormat::Half16)
                                                                         : int(IntensityFormat::GForce8);
        MTL::RenderCommandEncoder* enc = beginPass(cmd, c.to, MTL::LoadActionDontCare, "intensity format copy");
        enc->setRenderPipelineState(sets_[set].copy);
        enc->setFragmentTexture(c.from, 0);
        enc->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(3));
        enc->endEncoding();
        c.from->release();  // the command buffer keeps it alive until done
    }
    converts_.clear();
}

void ScenePipeline::encodeClear(MTL::CommandBuffer* cmd) {
    flushConverts(cmd);
    beginPass(cmd, intensity_[current_], MTL::LoadActionClear, "intensity clear")->endEncoding();
    initialized_ = true;
}

void ScenePipeline::encodeNoise(MTL::CommandBuffer* cmd, uint32_t seed) {
    flushConverts(cmd);
    MTL::RenderCommandEncoder* enc = beginPass(cmd, intensity_[current_], MTL::LoadActionDontCare, "noise");
    enc->setRenderPipelineState(noisePso_);
    enc->setFragmentBytes(&seed, sizeof(seed), 0);
    enc->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(3));
    enc->endEncoding();
    initialized_ = true;
}

void ScenePipeline::encodeStep(MTL::CommandBuffer* cmd, const engine::LineSegment* segments,
                               std::size_t segmentCount, const FlowBlend& blend) {
    encodeStep(cmd, segments, segmentCount, blend, nullptr, 0, true, nullptr, 0);
}

MTL::Texture* ScenePipeline::createSpriteTexture(MTL::Device* device, const std::uint8_t* la, int width,
                                                 int height) {
    if (!la || width <= 0 || height <= 0) return nullptr;
    int levels = 1;
    for (int m = std::max(width, height); m > 1; m >>= 1) ++levels;
    MTL::TextureDescriptor* td = MTL::TextureDescriptor::texture2DDescriptor(
        MTL::PixelFormatRG8Unorm, NS::UInteger(width), NS::UInteger(height), levels > 1);
    td->setUsage(MTL::TextureUsageShaderRead);
    td->setStorageMode(device->hasUnifiedMemory() ? MTL::StorageModeShared : MTL::StorageModeManaged);
    MTL::Texture* tex = device->newTexture(td);
    if (!tex) return nullptr;
    tex->setLabel(NS::String::string("sprite", NS::UTF8StringEncoding));
    tex->replaceRegion(MTL::Region(0, 0, NS::UInteger(width), NS::UInteger(height)), 0, la,
                       NS::UInteger(width) * 2);
    return tex;
}

int ScenePipeline::addSpriteTexture(MTL::Texture* tex, bool hasAlpha) {
    if (!tex) return -1;
    tex->retain();
    const int h = nextSpriteHandle_++;
    SpriteTex st;
    st.tex = tex;
    st.hasAlpha = hasAlpha;
    st.needsMips = tex->mipmapLevelCount() > 1;
    spriteTex_[h] = st;
    return h;
}

int ScenePipeline::addSpriteTexture(const std::uint8_t* la, int width, int height, bool hasAlpha) {
    perf::countRenderAlloc();
    MTL::Texture* tex = createSpriteTexture(device_, la, width, height);
    if (!tex) return -1;
    const int h = addSpriteTexture(tex, hasAlpha);
    tex->release();
    return h;
}

int ScenePipeline::addSolidTexture(MTL::Texture* tex) {
    const int h = addSpriteTexture(tex, true);
    if (h >= 0) {
        spriteTex_[h].premultiplied = true;
        spriteTex_[h].needsMips = false;
    }
    return h;
}

int ScenePipeline::addVideoSpriteTexture(bool invert, const VideoLevels& levels) {
    const int h = nextSpriteHandle_++;
    SpriteTex st;
    st.video = true;
    st.invert = invert;
    st.levels = levels;
    spriteTex_[h] = st;
    return h;
}

void ScenePipeline::setSpriteFrame(int handle, MTL::Texture* frame) {
    auto it = spriteTex_.find(handle);
    if (it != spriteTex_.end() && it->second.video) it->second.tex = frame;
}

void ScenePipeline::removeSpriteTexture(int handle) {
    auto it = spriteTex_.find(handle);
    if (it == spriteTex_.end()) return;
    // In-flight command buffers retain it. Video frames are owned by their
    // FramePtr (kept alive by the Visualizer until the GPU is done).
    if (it->second.tex && !it->second.video) it->second.tex->release();
    spriteTex_.erase(it);
}

void ScenePipeline::encodeSprites(MTL::CommandBuffer* cmd, MTL::Texture* target, const SpriteQuad* quads,
                                  std::size_t count, const char* label) {
    MTL::RenderCommandEncoder* enc = beginPass(cmd, target, MTL::LoadActionLoad, label);
    for (std::size_t i = 0; i < count; ++i) {
        const SpriteQuad& q = quads[i];
        auto it = spriteTex_.find(q.texture);
        if (it == spriteTex_.end() || !it->second.tex || q.w <= 0 || q.h <= 0) continue;
        int pso = 0;
        if (q.op == 2 || q.mix > 0.0f) pso = 4;  // reads the target (a Mix dims it, whatever the op)
        else switch (q.blend) {
            case 2: pso = 1; break;  // additive
            case 3: pso = 2; break;  // selective (max)
            case 5: pso = 3; break;  // alpha blend (premultiplied over)
            default: pso = 0; break; // copy / threshold / alpha mask: over by opacity
        }
        SpriteP sp{{float(width_), float(height_)}, {q.cx, q.cy}, {q.w * 0.5f, q.h * 0.5f}, q.pen, q.opacity,
                   q.blend, it->second.hasAlpha ? 1 : 0,
                   q.op == 1 ? engine::kSpriteKeyThreshold : engine::kSpriteThreshold,
                   it->second.video ? 1 : it->second.premultiplied ? 2 : 0, it->second.invert ? 1 : 0, q.op,
                   q.mix > 0.0f ? q.mix : 0.0f, it->second.video && !it->second.levels.identity() ? 1 : 0,
                   it->second.levels.black, it->second.levels.white, it->second.levels.gamma, 0,
                   {q.u0, q.v0, q.u1, q.v1}};
        enc->setRenderPipelineState(spritePso_[pso]);
        enc->setVertexBytes(&sp, sizeof(sp), 0);
        enc->setFragmentBytes(&sp, sizeof(sp), 0);
        enc->setFragmentTexture(it->second.tex, 0);
        enc->drawPrimitives(MTL::PrimitiveTypeTriangleStrip, NS::UInteger(0), NS::UInteger(4));
    }
    enc->endEncoding();
}

void ScenePipeline::enableTileLines(int extraLineBuffers) {
    if (tileLinePso_) return;
    tileLinePso_ = makePso(device_, lib_, "line_vs", "line_fs", MTL::PixelFormatR8Unorm, BlendOver, "lines (solid tile)");
    for (int i = 0; i < extraLineBuffers; ++i) {
        LineBuffer lb{device_->newBuffer(NS::UInteger(1u << 20),
                                         MTL::ResourceStorageModeShared | MTL::ResourceCPUCacheModeWriteCombined),
                      std::make_shared<std::atomic<bool>>(false)};
        if (!lb.buf) throw std::runtime_error("line buffer allocation failed");
        lineBuffers_.push_back(std::move(lb));
    }
}

void ScenePipeline::encodeTileLines(MTL::CommandBuffer* cmd, MTL::Texture* tile, const engine::LineSegment* segments,
                                    std::size_t count) {
    MTL::RenderPassDescriptor* pass = MTL::RenderPassDescriptor::renderPassDescriptor();
    auto* att = pass->colorAttachments()->object(0);
    att->setTexture(tile);
    att->setLevel(0);
    att->setLoadAction(MTL::LoadActionClear);
    att->setClearColor(MTL::ClearColor(0, 0, 0, 1));
    att->setStoreAction(MTL::StoreActionStore);
    MTL::RenderCommandEncoder* enc = cmd->renderCommandEncoder(pass);
    enc->setLabel(NS::String::string("solid tile lines", NS::UTF8StringEncoding));
    if (count > 0 && tileLinePso_) {
        LineParams lp{{float(tile->width()), float(tile->height())}, 1.f, 0.f, {}};
        enc->setRenderPipelineState(tileLinePso_);
        enc->setVertexBuffer(lineBuffer(cmd, segments, count * sizeof(engine::LineSegment)), 0, 0);
        enc->setVertexBytes(&lp, sizeof(lp), 1);
        enc->setFragmentBytes(&lp, sizeof(lp), 0);
        enc->drawPrimitives(MTL::PrimitiveTypeTriangleStrip, NS::UInteger(0), NS::UInteger(4), NS::UInteger(count));
    }
    enc->endEncoding();
}

MTL::Texture* ScenePipeline::previousIntensity(MTL::CommandBuffer* cmd) {
    flushConverts(cmd);
    if (!initialized_) encodeClear(cmd);
    return intensity_[current_];
}

MTL::Buffer* ScenePipeline::lineBuffer(MTL::CommandBuffer* cmd, const void* data, std::size_t bytes) {
    LineBuffer* slot = nullptr;
    for (auto& lb : lineBuffers_)
        if (!lb.busy->load(std::memory_order_acquire) && (!slot || lb.buf->length() > slot->buf->length())) slot = &lb;
    if (!slot) {
        lineBuffers_.push_back({nullptr, std::make_shared<std::atomic<bool>>(false)});
        slot = &lineBuffers_.back();
    }
    if (!slot->buf || slot->buf->length() < bytes) {
        if (slot->buf) slot->buf->release();
        // Headroom so a growing shape does not reallocate every frame.
        const std::size_t cap = std::max<std::size_t>(bytes + bytes / 2, 64 * 1024);
        perf::countRenderAlloc();
        slot->buf = device_->newBuffer(NS::UInteger(cap), MTL::ResourceStorageModeShared | MTL::ResourceCPUCacheModeWriteCombined);
        if (!slot->buf) throw std::runtime_error("line buffer allocation failed");
    }
    std::memcpy(slot->buf->contents(), data, bytes);
    slot->busy->store(true, std::memory_order_release);
    auto busy = slot->busy;
    cmd->addCompletedHandler([busy](MTL::CommandBuffer*) { busy->store(false, std::memory_order_release); });
    return slot->buf;
}

void ScenePipeline::encodeStep(MTL::CommandBuffer* cmd, const engine::LineSegment* segments,
                               std::size_t segmentCount, const FlowBlend& blend, const SpriteQuad* feedbackQuads,
                               std::size_t feedbackCount, bool feedbackBeforeLines, const SpriteQuad* displayQuads,
                               std::size_t displayCount) {
    flushConverts(cmd);
    if (!initialized_) encodeClear(cmd);
    markSubmitted(cmd);
    for (const Bake& b : bakes_) {
        MTL::RenderCommandEncoder* enc = beginPass(cmd, b.out->texture(), MTL::LoadActionDontCare, "field bake");
        FlowParams fp{{width_, height_}, 0.f, b.w, b.mode, 1, b.useMask ? 1 : 0, float(width_) / float(height_), 0};
        enc->setRenderPipelineState(bakePso_);
        enc->setFragmentTexture(b.a->texture(), 0);
        enc->setFragmentTexture(b.b->texture(), 1);
        enc->setFragmentTexture(b.useMask && b.mask ? b.mask->texture() : dummyMask_, 2);
        enc->setFragmentBytes(&fp, sizeof(fp), 0);
        enc->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(3));
        enc->endEncoding();
    }
    bakes_.clear();
    bool mips = false;
    for (auto& [h, st] : spriteTex_) mips = mips || st.needsMips;
    if (mips) {
        MTL::BlitCommandEncoder* blit = cmd->blitCommandEncoder();
        for (auto& [h, st] : spriteTex_)
            if (st.needsMips) {
                blit->generateMipmaps(st.tex);
                st.needsMips = false;
            }
        blit->endEncoding();
    }
    MTL::Texture* prev = intensity_[current_];
    MTL::Texture* next = intensity_[current_ ^ 1];

    // 1+2. flow, then lines, in one pass on `next` (split around the feedback sprites when they go
    // between the two, G-Force's order: flow -> Pen stamps -> lines).
    const bool aa = opts_.antialias;
    const bool pathAa = aa && linePathPso_ && segmentCount > 0;
    const bool msaa = aa && lineMsaaPso_ && segmentCount > 0;
    const bool split = feedbackCount > 0 && feedbackBeforeLines && !msaa;
    auto openPass = [&](MTL::LoadAction load, const char* label) {
        MTL::RenderPassDescriptor* pass = MTL::RenderPassDescriptor::renderPassDescriptor();
        auto* att = pass->colorAttachments()->object(0);
        att->setLoadAction(load);
        if (msaa) {
            att->setTexture(msaa_);
            att->setResolveTexture(next);
            att->setStoreAction(MTL::StoreActionMultisampleResolve);
        } else {
            att->setTexture(next);
            att->setStoreAction(MTL::StoreActionStore);
        }
        if (pathAa) {
            auto* sc = pass->colorAttachments()->object(1);
            sc->setTexture(scratch_);
            sc->setLoadAction(MTL::LoadActionClear);
            sc->setClearColor(MTL::ClearColor(-1, 0, 0, 0));  // no path yet
            sc->setStoreAction(MTL::StoreActionDontCare);
        }
        MTL::RenderCommandEncoder* e = cmd->renderCommandEncoder(pass);
        e->setLabel(NS::String::string(label, NS::UTF8StringEncoding));
        return e;
    };
    MTL::RenderCommandEncoder* enc = openPass(MTL::LoadActionDontCare, split ? "flow" : "flow + lines");
    const bool active = blend.active && field_[1];
    const bool useMask = active && blend.useMask && mask_;
    MTL::Texture* fieldA = field_[0]->texture();
    MTL::Texture* fieldB = active ? field_[1]->texture() : fieldA;
    FlowParams fp{{width_, height_}, opts_.decayPerFrame, blend.weight, blend.mode, active ? 1 : 0,
                  useMask ? 1 : 0, float(width_) / float(height_),
                  opts_.format == IntensityFormat::GForce8 ? int32_t(opts_.flowRounding) : 0, kickZoom_, kickRotate_};
    enc->setRenderPipelineState(pathAa ? flowPathPso_ : msaa ? flowMsaaPso_ : flowPso_);
    enc->setFragmentTexture(prev, 0);
    enc->setFragmentTexture(fieldA, 1);
    enc->setFragmentTexture(fieldB, 2);
    enc->setFragmentTexture(useMask ? mask_->texture() : dummyMask_, 3);
    enc->setFragmentBytes(&fp, sizeof(fp), 0);
    enc->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(3));
    if (split) {
        enc->endEncoding();
        encodeSprites(cmd, next, feedbackQuads, feedbackCount, "sprites (feedback)");
        enc = openPass(MTL::LoadActionLoad, "lines");
    }

    if (segmentCount > 0) {
        const std::size_t bytes = segmentCount * sizeof(engine::LineSegment);
        const float mode = !aa ? 0.f : pathAa ? (opts_.thinLines == ThinLines::GForce ? 4.f : 2.f) : msaa ? 3.f : 1.f;
        LineParams lp{{float(width_), float(height_)}, mode, 0.f, {}};
        std::memcpy(lp.samples, samplePos_, sizeof(lp.samples));
        enc->setRenderPipelineState(pathAa ? linePathPso_ : msaa ? lineMsaaPso_ : linePso_);
        MTL::Buffer* buf = lineBuffer(cmd, segments, bytes);
        enc->setVertexBuffer(buf, 0, 0);
        enc->setVertexBytes(&lp, sizeof(lp), 1);
        enc->setFragmentBytes(&lp, sizeof(lp), 0);
        // Primitives rasterize and blend in submission order: later segments
        // (later paths) overwrite earlier ones. Per-path AA reads the
        // attachments in the shader; Apple GPUs order those accesses by
        // submission too.
        enc->drawPrimitives(MTL::PrimitiveTypeTriangleStrip, NS::UInteger(0), NS::UInteger(4),
                            NS::UInteger(segmentCount));
    }
    enc->endEncoding();

    // 2a. Color edges (reforce.LineEdges = color): this step's line coverage for the colorize pass.
    edgeValid_ = false;
    if (opts_.colorEdges && aa && segmentCount > 0) {
        if (!edge_) {
            MTL::TextureDescriptor* td =
                MTL::TextureDescriptor::texture2DDescriptor(MTL::PixelFormatRGBA8Unorm, width_, height_, false);
            td->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
            td->setStorageMode(MTL::StorageModePrivate);
            perf::countRenderAlloc();  // once, when the option is first used
            edge_ = device_->newTexture(td);
            if (!edge_) throw std::runtime_error("line coverage texture allocation failed");
            edge_->setLabel(NS::String::string("line coverage", NS::UTF8StringEncoding));
        }
        MTL::RenderPassDescriptor* ep = MTL::RenderPassDescriptor::renderPassDescriptor();
        auto* ea = ep->colorAttachments()->object(0);
        ea->setTexture(edge_);
        ea->setLoadAction(MTL::LoadActionClear);
        ea->setClearColor(MTL::ClearColor(0, 0, 0, 0));
        ea->setStoreAction(MTL::StoreActionStore);
        MTL::RenderCommandEncoder* ee = cmd->renderCommandEncoder(ep);
        ee->setLabel(NS::String::string("line coverage", NS::UTF8StringEncoding));
        const float mode = pathAa ? (opts_.thinLines == ThinLines::GForce ? 4.f : 2.f) : 1.f;
        LineParams lp{{float(width_), float(height_)}, mode, 0.f, {}};
        ee->setRenderPipelineState(lineCoverPso_);
        ee->setVertexBuffer(lineBuffer(cmd, segments, segmentCount * sizeof(engine::LineSegment)), 0, 0);
        ee->setVertexBytes(&lp, sizeof(lp), 1);
        ee->setFragmentBytes(&lp, sizeof(lp), 0);
        ee->drawPrimitives(MTL::PrimitiveTypeTriangleStrip, NS::UInteger(0), NS::UInteger(4), NS::UInteger(segmentCount));
        ee->endEncoding();
        edgeValid_ = true;
    }

    // 2b. sprites. MSAA lines can't split their pass: the feedback stamps go after the lines there.
    if (feedbackCount > 0 && feedbackBeforeLines && !split)
        encodeSprites(cmd, next, feedbackQuads, feedbackCount, "sprites (feedback)");
    const bool late = feedbackCount > 0 && !feedbackBeforeLines;
    // Displayed frame = this frame + display sprites; late (melt) feedback stamps show from the next frame.
    displayValid_ = displayCount > 0 || late;
    if (displayValid_) {
        if (!display_) {
            MTL::TextureDescriptor* td =
                MTL::TextureDescriptor::texture2DDescriptor(next->pixelFormat(), width_, height_, false);
            td->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
            td->setStorageMode(MTL::StorageModePrivate);
            perf::countRenderAlloc();
            display_ = device_->newTexture(td);
            if (!display_) throw std::runtime_error("display texture allocation failed");
            display_->setLabel(NS::String::string("displayed intensity", NS::UTF8StringEncoding));
        }
        MTL::BlitCommandEncoder* blit = cmd->blitCommandEncoder();
        blit->copyFromTexture(next, display_);
        blit->endEncoding();
        if (displayCount) encodeSprites(cmd, display_, displayQuads, displayCount, "sprites (display)");
    }
    if (late) encodeSprites(cmd, next, feedbackQuads, feedbackCount, "sprites (feedback)");
    current_ ^= 1;
}

void ScenePipeline::encodeColorize(MTL::CommandBuffer* cmd, const engine::Palette& palette,
                                   const engine::MapLut& lut, MTL::Texture* output, const VideoLayerDraw* video,
                                const SpriteQuad* colorQuads, std::size_t colorCount, bool trackAlpha,
                                const SpriteQuad* opaqueSolids, std::size_t opaqueCount,
                                const SpriteQuad* colorSolids, std::size_t colorSolidCount) {
    flushConverts(cmd);
    const float outAlpha = trackAlpha ? 0.0f : 1.0f;
    if (!initialized_) encodeClear(cmd);
    const bool withVideo = video && (video->a || video->b);
    MTL::RenderCommandEncoder* enc =
        beginPass(cmd, output, MTL::LoadActionDontCare, withVideo ? "colorize + video" : "colorize");
    enc->setRenderPipelineState(withVideo ? colorizeVideoPso_ : colorizePso_);
    enc->setFragmentTexture(displayValid_ && display_ ? display_ : intensity_[current_], 0);
    enc->setFragmentBytes(palette.data(), sizeof(engine::Palette), 0);
    enc->setFragmentBytes(lut.data(), sizeof(engine::MapLut), 1);
    enc->setFragmentBytes(&outAlpha, sizeof(outAlpha), withVideo ? 3 : 2);
    const int32_t edges = edgeValid_ && edge_ && opts_.colorEdges ? 1 : 0;
    enc->setFragmentTexture(edges ? edge_ : dummyMask_, 3);
    enc->setFragmentBytes(&edges, sizeof(edges), 4);
    if (withVideo) {
        const engine::VideoLayerParams& p = video->params;
        VideoP vp{};
        auto uv = [](float* dst, const engine::UvTransform& t) {
            dst[0] = t.scale[0];
            dst[1] = t.scale[1];
            dst[2] = t.offset[0];
            dst[3] = t.offset[1];
        };
        uv(vp.uvA, video->uvA);
        uv(vp.uvB, video->uvB);
        const engine::Rgb& k = p.keyColor;
        vp.key[0] = -0.168736f * k[0] - 0.331264f * k[1] + 0.5f * k[2];
        vp.key[1] = 0.5f * k[0] - 0.418688f * k[1] - 0.081312f * k[2];
        vp.key[2] = p.tolerance;
        vp.key[3] = p.softness;
        vp.misc[0] = video->weight;
        vp.misc[1] = p.opacity;
        vp.misc[2] = p.lumaThreshold;
        vp.mode[0] = video->a ? 1 : 0;
        vp.mode[1] = video->b ? 1 : 0;
        vp.mode[2] = int(p.blend);
        vp.mode[3] = int(p.order);
        vp.mode2[0] = int(p.key);
        enc->setFragmentTexture(video->a ? video->a : dummyMask_, 1);
        enc->setFragmentTexture(video->b ? video->b : dummyMask_, 2);
        enc->setFragmentBytes(&vp, sizeof(vp), 2);
    }
    enc->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(3));
    for (std::size_t i = 0; i < colorCount; ++i) {
        const SpriteQuad& q = colorQuads[i];
        auto it = spriteTex_.find(q.texture);
        if (it == spriteTex_.end() || !it->second.tex || !it->second.video || q.w <= 0 || q.h <= 0 || !(q.opacity > 0))
            continue;
        const VideoLevels& lv = it->second.levels;
        SpriteP sp{{float(width_), float(height_)}, {q.cx, q.cy}, {q.w * 0.5f, q.h * 0.5f}, 1.0f, q.opacity, 0, 0, 0.0f,
                   1, it->second.invert ? 1 : 0, 0, 0.0f, lv.identity() ? 0 : 1, lv.black, lv.white, lv.gamma, 0,
                   {q.u0, q.v0, q.u1, q.v1}};
        enc->setRenderPipelineState(trackAlpha && q.opaque ? colorSpriteOpaquePso_ : colorSpritePso_);
        enc->setVertexBytes(&sp, sizeof(sp), 0);
        enc->setFragmentBytes(&sp, sizeof(sp), 0);
        enc->setFragmentTexture(it->second.tex, 0);
        enc->drawPrimitives(MTL::PrimitiveTypeTriangleStrip, NS::UInteger(0), NS::UInteger(4));
    }
    for (std::size_t i = 0; i < colorSolidCount; ++i) {
        const SpriteQuad& q = colorSolids[i];
        auto it = spriteTex_.find(q.texture);
        if (it == spriteTex_.end() || !it->second.tex || q.w <= 0 || q.h <= 0 || !(q.opacity > 0)) continue;
        SpriteP sp{{float(width_), float(height_)}, {q.cx, q.cy}, {q.w * 0.5f, q.h * 0.5f}, 1.0f, q.opacity,
                   q.puzzle, 0, q.numeral, 0, 0, 0, 0.0f, 0, 0.0f, 1.0f, 1.0f, 0, {q.u0, q.v0, q.u1, q.v1}};
        enc->setRenderPipelineState(trackAlpha && q.opaque ? solidColorOpaquePso_ : solidColorPso_);
        enc->setVertexBytes(&sp, sizeof(sp), 0);
        enc->setFragmentBytes(&sp, sizeof(sp), 0);
        enc->setFragmentBytes(palette.data(), sizeof(engine::Palette), 1);
        enc->setFragmentBytes(lut.data(), sizeof(engine::MapLut), 2);
        enc->setFragmentTexture(it->second.tex, 0);
        enc->drawPrimitives(MTL::PrimitiveTypeTriangleStrip, NS::UInteger(0), NS::UInteger(4));
    }
    if (trackAlpha) {
        for (std::size_t i = 0; i < opaqueCount; ++i) {
            const SpriteQuad& q = opaqueSolids[i];
            auto it = spriteTex_.find(q.texture);
            if (it == spriteTex_.end() || !it->second.tex || q.w <= 0 || q.h <= 0 || !(q.opacity > 0)) continue;
            SpriteP sp{{float(width_), float(height_)}, {q.cx, q.cy}, {q.w * 0.5f, q.h * 0.5f}, 1.0f, q.opacity, 0, 0,
                       0.0f, 0, 0, 0, 0.0f, 0, 0.0f, 1.0f, 1.0f, 0, {q.u0, q.v0, q.u1, q.v1}};
            enc->setRenderPipelineState(solidOpaquePso_);
            enc->setVertexBytes(&sp, sizeof(sp), 0);
            enc->setFragmentBytes(&sp, sizeof(sp), 0);
            enc->setFragmentTexture(it->second.tex, 0);
            enc->drawPrimitives(MTL::PrimitiveTypeTriangleStrip, NS::UInteger(0), NS::UInteger(4));
        }
    }
    if (trackAlpha) {
        for (std::size_t i = 0; i < opaqueCount; ++i) {
            const SpriteQuad& q = opaqueSolids[i];
            auto it = spriteTex_.find(q.texture);
            if (it == spriteTex_.end() || !it->second.tex || q.w <= 0 || q.h <= 0 || !(q.opacity > 0)) continue;
            SpriteP sp{{float(width_), float(height_)}, {q.cx, q.cy}, {q.w * 0.5f, q.h * 0.5f}, 1.0f, q.opacity, 0, 0,
                       0.0f, 0, 0, 0, 0.0f, 0, 0.0f, 1.0f, 1.0f, 0, {q.u0, q.v0, q.u1, q.v1}};
            enc->setRenderPipelineState(solidOpaquePso_);
            enc->setVertexBytes(&sp, sizeof(sp), 0);
            enc->setFragmentBytes(&sp, sizeof(sp), 0);
            enc->setFragmentTexture(it->second.tex, 0);
            enc->drawPrimitives(MTL::PrimitiveTypeTriangleStrip, NS::UInteger(0), NS::UInteger(4));
        }
    }
    enc->endEncoding();
}

void ScenePipeline::encodeSyphonAlpha(MTL::CommandBuffer* cmd, MTL::Texture* frame, MTL::Texture* output,
                                      float similarity, float smoothness, bool premultiply) {
    struct {
        float similarity, smoothness;
        int32_t premultiply, pad;
    } k{similarity, smoothness, premultiply ? 1 : 0, 0};
    MTL::RenderCommandEncoder* enc = beginPass(cmd, output, MTL::LoadActionDontCare, "Syphon alpha");
    enc->setRenderPipelineState(syphonAlphaPso_);
    enc->setFragmentTexture(frame, 0);
    enc->setFragmentBytes(&k, sizeof(k), 0);
    enc->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(3));
    enc->endEncoding();
}

void ScenePipeline::encodeBackdrop(MTL::CommandBuffer* cmd, MTL::Texture* frame, MTL::Texture* video,
                                   const engine::UvTransform& uv, bool mirror, float opacity, float similarity,
                                   float smoothness, const BackdropBody* body) {
    struct {
        float similarity, smoothness, opacity;
        int32_t mirror;
        float scale[2], offset[2];
        float front;
        int32_t show, hasMask, joints;
    } p{similarity, smoothness, video ? opacity : 0.0f, mirror ? 1 : 0, {uv.scale[0], uv.scale[1]},
        {uv.offset[0], uv.offset[1]}, 0.0f, 0, 0, 0};
    static_assert(sizeof(p) == 48);
    std::array<float, 4 * BackdropBody::kJoints> joints{};
    if (body) {
        p.front = body->front;
        p.show = body->show ? 1 : 0;
        p.hasMask = body->mask ? 1 : 0;
        p.joints = body->show ? BackdropBody::kJoints : 0;
        joints = body->joints;
    }
    const float size[2] = {float(frame->width()), float(frame->height())};
    MTL::RenderCommandEncoder* enc = beginPass(cmd, frame, MTL::LoadActionLoad, "camera backdrop");
    enc->setRenderPipelineState(backdropPso_);
    enc->setFragmentTexture(video ? video : frame, 0);  // no video: opacity 0 (keyed parts black, alpha 1)
    enc->setFragmentTexture(body && body->mask ? body->mask : dummyMask_, 1);
    enc->setFragmentBytes(&p, sizeof(p), 0);
    enc->setFragmentBytes(size, sizeof(size), 1);
    enc->setFragmentBytes(joints.data(), sizeof(joints), 2);
    enc->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(3));
    enc->endEncoding();
}

void ScenePipeline::encodeBodyBurn(MTL::CommandBuffer* cmd, MTL::Texture* mask, MTL::Texture* prevMask,
                                   const engine::UvTransform& uv, bool mirror, int mode, float amount) {
    if (!mask || mode <= 0 || amount <= 0 || !bodyBurnPso_) return;
    MTL::Texture* target = intensity_[current_];
    struct {
        float scale[2], offset[2];
        int32_t mirror, mode;
        float amount, pad;
        float texel[2];
    } p{{uv.scale[0], uv.scale[1]}, {uv.offset[0], uv.offset[1]}, mirror ? 1 : 0, mode, amount, 0.0f,
        {1.0f / float(mask->width()), 1.0f / float(mask->height())}};
    const float size[2] = {float(target->width()), float(target->height())};
    MTL::RenderCommandEncoder* enc = beginPass(cmd, target, MTL::LoadActionLoad, "body silhouette");
    enc->setRenderPipelineState(bodyBurnPso_);
    enc->setFragmentTexture(mask, 0);
    enc->setFragmentTexture(prevMask ? prevMask : mask, 1);
    enc->setFragmentBytes(&p, sizeof(p), 0);
    enc->setFragmentBytes(size, sizeof(size), 1);
    enc->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(3));
    enc->endEncoding();
}

}  // namespace reforce::render
