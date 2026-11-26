#include "render/SolidPipeline.hpp"

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <stdexcept>

#include "render/GpuPoison.hpp"
#include "render/ScenePipeline.hpp"
#include "render/ShaderCompiler.hpp"

namespace reforce::render {
namespace {

// MeshVertex in MSL: packed types, 36 bytes, the same offsets as engine::MeshVertex (checked below and in
// tests/test_render_solids.cpp).
constexpr const char* kSolidShaders = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct MeshVertex { packed_float3 pos; packed_float3 normal; packed_float2 uv; float edge; };
struct SolidV { float4x4 mvp; float4 n0; float4 n1; float4 n2; };
struct SolidF { float4 light; float fill; float shade; float edge; float edgeWidth; int text; int pad[3]; };
struct SolidOut { float4 position [[position]]; float3 n; float2 uv; float edge; };

vertex SolidOut solid_vs(uint vid [[vertex_id]], const device MeshVertex* v [[buffer(0)]],
                         constant SolidV& U [[buffer(1)]]) {
    const MeshVertex m = v[vid];
    SolidOut o;
    o.position = U.mvp * float4(float3(m.pos), 1.0f);
    const float3 n = float3(m.normal);
    o.n = U.n0.xyz * n.x + U.n1.xyz * n.y + U.n2.xyz * n.z;
    o.uv = float2(m.uv);
    o.edge = m.edge;
    return o;
}

// See render/SolidPipeline.hpp. text: 0 / 1 = sample the bound tile (WaveShape / frame), 2 = flat, 3 = no source
// yet (tex = 0).
fragment float4 solid_fs(SolidOut in [[stage_in]], constant SolidF& U [[buffer(0)]],
                         texture2d<float> tile [[texture(0)]]) {
    constexpr sampler smp(filter::linear, mip_filter::linear, address::clamp_to_edge);
    const float tex = U.text == 2 ? 1.0f : U.text == 3 ? 0.0f : tile.sample(smp, in.uv).r;
    float lum = U.fill + (1.0f - U.fill) * tex;
    const float3 n = normalize(in.n);
    lum *= mix(1.0f, max(0.0f, dot(n, U.light.xyz)), U.shade);
    lum = max(lum, U.edge * (1.0f - smoothstep(0.0f, max(U.edgeWidth, 1e-4f), in.edge)));
    return float4(saturate(lum), 1.0f, 0.0f, 0.0f);
}

// Rubik cubie (MeshKind::Rubik): uv = (direction 0..5, 1 sticker / 0 plastic); lv = this cubie's sticker intensity
// per direction (< 0: no sticker there, plastic).
struct RubikF { float4 light; float4 lv0; float4 lv1; float shade; float body; float pad0; float pad1; };

fragment float4 rubik_fs(SolidOut in [[stage_in]], constant RubikF& U [[buffer(0)]]) {
    const int d = clamp(int(in.uv.x + 0.5f), 0, 5);
    const float lv = d < 4 ? U.lv0[d] : U.lv1[d - 4];
    const bool sticker = in.uv.y > 0.5f && lv >= 0.0f;
    const float lum = sticker ? lv : U.body;
    // Shading would change the intensity and so the color: it goes to a instead, and the color pass darkens the
    // color by it. b: sticker share, so the color pass anti-aliases sticker / plastic borders in color (after the
    // resolve, r = sticker share x sticker intensity + plastic share x body, g = coverage, b = sticker share,
    // a = coverage x shading).
    const float shade = mix(1.0f, max(0.0f, dot(normalize(in.n), U.light.xyz)), U.shade);
    return float4(saturate(lum), 1.0f, sticker ? 1.0f : 0.0f, saturate(shade));
}

// 15-puzzle tile (MeshKind::Fifteen): uv 0..1 on the top face (< 0 elsewhere). number 0 = the tray.
struct FifteenF { float4 light; float level; float body; float shade; int number; };

static float segment(float2 p, float2 a, float2 b) {
    const float2 pa = p - a, ba = b - a;
    const float h = saturate(dot(pa, ba) / dot(ba, ba));
    return length(pa - ba * h);
}
// Seven-segment digit `d` centered at the origin, height 1 (y up); distance to its nearest lit segment's axis.
static float digit_dist(float2 p, int d) {
    // a b c d e f g
    const int masks[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};
    const int m = masks[clamp(d, 0, 9)];
    const float x0 = -0.24f, x1 = 0.24f, y0 = -0.44f, y1 = 0.44f, ym = 0.0f, g = 0.06f;
    float r = 1e9f;
    if (m & 0x01) r = min(r, segment(p, float2(x0 + g, y1), float2(x1 - g, y1)));   // a
    if (m & 0x02) r = min(r, segment(p, float2(x1, y1 - g), float2(x1, ym + g)));   // b
    if (m & 0x04) r = min(r, segment(p, float2(x1, ym - g), float2(x1, y0 + g)));   // c
    if (m & 0x08) r = min(r, segment(p, float2(x0 + g, y0), float2(x1 - g, y0)));   // d
    if (m & 0x10) r = min(r, segment(p, float2(x0, ym - g), float2(x0, y0 + g)));   // e
    if (m & 0x20) r = min(r, segment(p, float2(x0, y1 - g), float2(x0, ym + g)));   // f
    if (m & 0x40) r = min(r, segment(p, float2(x0 + g, ym), float2(x1 - g, ym)));   // g
    return r;
}

// Horizontal extent of a digit's lit segments (glyph units): a "1" is only its right-hand segments.
static float2 digit_span(int d) { return d == 1 ? float2(0.24f, 0.24f) : float2(-0.24f, 0.24f); }

fragment float4 fifteen_fs(SolidOut in [[stage_in]], constant FifteenF& U [[buffer(0)]]) {
    float share = 0.0f;  // tile color (vs plastic)
    if (U.number > 0 && in.uv.x < 0.0f) share = 1.0f;  // a tile's sides: its color all the way down to the tray
    if (U.number > 0 && in.uv.x >= 0.0f) {
        // The number, about half the tile high, digits side by side.
        const float2 p = (in.uv - 0.5f) / 0.5f;
        float d;
        // Centered on the tile by the lit segments' extent (so "1", "11" and "17" sit in the middle too).
        if (U.number < 10) {
            const float2 s = digit_span(U.number);
            d = digit_dist(p + float2(0.5f * (s.x + s.y), 0.0f), U.number);
        } else {
            const int a = U.number / 10, b = U.number % 10;
            const float k = 0.9f, gap = 0.3f;  // two digits a little smaller, `gap` between their extents
            const float2 sa = digit_span(a), sb = digit_span(b);
            // Left digit at offset oa, right at ob (glyph units before the scale), the pair centered on the lit
            // strokes. For spacing a right-hand "1" counts as half a digit wide (its stroke alone would sit as close
            // to the digit before as two strokes of any other digit: "11" looked cramped).
            const float bLeft = b == 1 ? 0.0f : sb.x;
            const float ob = sa.y - bLeft + gap;  // with oa = 0
            const float mid = 0.5f * (sa.x + ob + sb.y);
            const float oa = -mid * k, obs = (ob - mid) * k;
            d = min(digit_dist((p - float2(oa, 0.0f)) / k, a) * k, digit_dist((p - float2(obs, 0.0f)) / k, b) * k);
        }
        const float w = 0.065f;  // stroke half width
        const float aa = max(fwidth(d), 1e-4f);
        share = smoothstep(w - aa, w + aa, d);  // 0 on the stroke, 1 on the tile
    }
    // r: the tile's intensity under its number too, so the color pass knows the tile color in every pixel (it draws
    // the number in a contrasting shade of it); b: the share outside the number's strokes.
    const float lum = U.number > 0 ? U.level : U.body;
    const float shade = mix(1.0f, max(0.0f, dot(normalize(in.n), U.light.xyz)), U.shade);  // applied in color (a)
    return float4(saturate(lum), 1.0f, share, saturate(shade));
}

// Knight's tour (MeshKind::Knight; docs/3d-design.md "Knight's tour"). part 0: the piece (level x shading, plus a rim
// where the surface turns away from the viewer); 1: a board square (uv across it, edge 1 = light): its fill and grid
// lines `width` wide (uv units), nothing where both are dark (the visuals show through); 2: flat at level (web).
struct KnightF { float4 light; float level; float shade; int part; float line; float lightFill; float darkFill;
                 float width; float rim; };

fragment float4 knight_fs(SolidOut in [[stage_in]], constant KnightF& U [[buffer(0)]]) {
    float lum = U.level;
    if (U.part == 0) {
        const float3 n = normalize(in.n);
        lum = U.level * mix(1.0f, max(0.0f, dot(n, U.light.xyz)), U.shade);
        lum += U.rim * U.level * pow(1.0f - saturate(n.z), 3.0f);
    } else if (U.part == 1) {
        const float2 uv = in.uv;
        const float d = min(min(uv.x, 1.0f - uv.x), min(uv.y, 1.0f - uv.y));
        const float aa = max(fwidth(d), 1e-4f);
        const float line = 1.0f - smoothstep(U.width - aa, U.width + aa, d);
        const float fill = in.edge > 0.5f ? U.lightFill : U.darkFill;
        lum = max(fill, U.line * line);
        if (lum < 0.004f) discard_fragment();
    }
    return float4(saturate(lum), 1.0f, 0.0f, 0.0f);
}

// ---- labyrinth (MeshKind::Labyrinth): the board, walls, holes and ball, each drawn with its own transform --------
// `local` takes the mesh into board units (untilted: z = 0 is the board's top), for the ball's shadow.
struct LabV { float4x4 mvp; float4 n0; float4 n1; float4 n2; float4x4 local; };
struct LabOut { float4 position [[position]]; float3 n; float2 uv; float3 local; };
// ball: x, y (board units), height of its center above the board, radius. part: 0 board, 1 walls / rings, 2 ball,
// 3 hole (black).
struct LabF { float4 light; float4 ball; float level; float shade; int part; float alpha; float flash; float shadow;
              float pad0; float pad1; };

vertex LabOut labyrinth_vs(uint vid [[vertex_id]], const device MeshVertex* v [[buffer(0)]],
                           constant LabV& U [[buffer(1)]]) {
    const MeshVertex m = v[vid];
    const float4 p = float4(float3(m.pos), 1.0f);
    LabOut o;
    o.position = U.mvp * p;
    const float3 n = float3(m.normal);
    o.n = U.n0.xyz * n.x + U.n1.xyz * n.y + U.n2.xyz * n.z;
    o.uv = float2(m.uv);
    o.local = (U.local * p).xyz;
    return o;
}

// As the puzzles: r = intensity x share, g = coverage, b = share (0 = black: the holes), a = coverage x shading / 2
// (shading 0..2: above 1 the color pass mixes towards white, the ball's highlight). The board's top writes its
// opacity (Floor) as coverage, premultiplied, so the visuals show through it in the color pass.
fragment float4 labyrinth_fs(LabOut in [[stage_in]], constant LabF& U [[buffer(0)]]) {
    const float3 n = normalize(in.n);
    const float lambert = max(0.0f, dot(n, U.light.xyz));
    float s = mix(1.0f, lambert, U.shade);
    float a = 1.0f, share = 1.0f;
    if (U.part == 0) {
        if (in.uv.x >= 0.0f) {  // the top: translucent, with the ball's soft shadow
            a = U.alpha;
            const float h = max(U.ball.z, 0.0f);
            const float2 sp = U.ball.xy - U.light.xy / max(U.light.z, 0.3f) * h;
            const float d = length(in.local.xy - sp);
            s *= 1.0f - 0.55f * U.shadow * (1.0f - smoothstep(0.4f * U.ball.w, 1.25f * U.ball.w, d));
        } else {
            s *= 0.8f;  // the board's edges
        }
    } else if (U.part == 2) {
        // Polished steel in the ball's color: a bright sky above, darker below, a white highlight, and a glint
        // around the rim just after a clack.
        const float3 r = reflect(float3(0.0f, 0.0f, -1.0f), n);
        float env = mix(0.28f, 1.0f, smoothstep(-0.35f, 0.75f, r.y));
        env *= 0.55f + 0.45f * lambert;
        const float spec = pow(max(dot(r, U.light.xyz), 0.0f), 40.0f);
        const float rim = pow(1.0f - saturate(n.z), 2.0f);
        s = env + 1.1f * spec + U.flash * 0.8f * rim;
    } else if (U.part == 3) {
        share = 0.0f;
        s = 0.0f;
    }
    return float4(saturate(U.level) * share * a, a, share * a, saturate(0.5f * s) * a);
}

// Soma cube (MeshKind::Soma): uv 0..1 across the face, edge = the face direction d; codes = the face borders
// (engine::soma::edgeCodes: 2 = the piece's outer edge, 1 = a seam inside the piece, 0 = none); col = the cube's world
// rotation (its local axes), for the borders' directions.
struct SomaF { float4 light; float4 col0; float4 col1; float4 col2;
               float level; float shade; float flash; float bevel; uint codes0; uint codes1; float ambient; float pad; };

fragment float4 soma_fs(SolidOut in [[stage_in]], constant SomaF& U [[buffer(0)]]) {
    const int d = clamp(int(in.edge + 0.5f), 0, 5);
    const uint codes = (d < 3 ? (U.codes0 >> (8 * d)) : (U.codes1 >> (8 * (d - 3)))) & 0xFFu;
    int ua = (d / 2 + 1) % 3, va = (d / 2 + 2) % 3;
    if (d % 2) { const int k = ua; ua = va; va = k; }
    const float3 cols[3] = {U.col0.xyz, U.col1.xyz, U.col2.xyz};
    const float3 uw = cols[ua], vw = cols[va];
    const float dist[4] = {in.uv.x, 1.0f - in.uv.x, in.uv.y, 1.0f - in.uv.y};
    const float3 dirs[4] = {-uw, uw, -vw, vw};
    float3 n = normalize(in.n);
    float line = 1.0f;
    for (int e = 0; e < 4; ++e) {
        const uint code = (codes >> (2 * e)) & 3u;
        const float x = dist[e];
        const float aa = max(fwidth(x), 1e-4f);
        if (code == 2u) {
            // A rounded outer edge: the normal turns towards the edge (about 45 degrees at it), and a groove where
            // two pieces meet.
            const float t = saturate(1.0f - x / max(U.bevel, 1e-3f));
            n += dirs[e] * (t * t * 1.2f);
            line *= mix(0.4f, 1.0f, smoothstep(0.012f - aa, 0.012f + aa, x));
        } else if (code == 1u) {
            line *= mix(0.82f, 1.0f, smoothstep(0.008f - aa, 0.008f + aa, x));  // a faint seam
        }
    }
    n = normalize(n);
    const float lam = U.ambient + (1.0f - U.ambient) * max(0.0f, dot(n, U.light.xyz));
    float shade = mix(1.0f, lam, U.shade);
    shade = mix(shade, 1.0f, 0.45f * saturate(U.flash)) * line;  // a lock lights the piece up for a moment
    return float4(saturate(U.level), 1.0f, 1.0f, saturate(shade));
}

// ---- glass (encodeGlass) -----------------------------------------------------------------------------------------
struct FullOut { float4 position [[position]]; float2 uv; };
vertex FullOut glass_full_vs(uint vid [[vertex_id]]) {
    const float2 p = float2((vid << 1) & 2, vid & 2);
    FullOut o;
    o.position = float4(p * 2.0f - 1.0f, 0.0f, 1.0f);
    o.uv = float2(p.x, 1.0f - p.y);
    return o;
}
// Half-size copy: 4 bilinear taps (a 4 x 4 box).
fragment float4 glass_down_fs(FullOut in [[stage_in]], texture2d<float> src [[texture(0)]],
                              constant float2& texel [[buffer(0)]]) {
    constexpr sampler smp(filter::linear, address::clamp_to_edge);
    const float2 d = texel * 0.5f;
    return 0.25f * (src.sample(smp, in.uv + float2(-d.x, -d.y)) + src.sample(smp, in.uv + float2(d.x, -d.y)) +
                    src.sample(smp, in.uv + float2(-d.x, d.y)) + src.sample(smp, in.uv + float2(d.x, d.y)));
}
// Separable Gaussian: 13 taps `step` texels apart along `dir` (texel units), sigma in texels.
struct BlurP { float2 dir; float sigma; float step; };
fragment float4 glass_blur_fs(FullOut in [[stage_in]], texture2d<float> src [[texture(0)]],
                              constant BlurP& B [[buffer(0)]]) {
    constexpr sampler smp(filter::linear, address::clamp_to_edge);
    float4 acc = 0.0f;
    float wsum = 0.0f;
    for (int i = -6; i <= 6; ++i) {
        const float x = float(i) * B.step;
        const float w = exp(-0.5f * x * x / max(B.sigma * B.sigma, 1e-4f));
        acc += w * src.sample(smp, in.uv + B.dir * x);
        wsum += w;
    }
    return acc / wsum;
}

// The glass buttons (engine::makeGlassButton), 3D with MSAA into the puzzle's slot target (premultiplied by the
// resolve), then laid over the frame by glass_composite_fs. World space = view orientation (the camera looks down -z).
struct GlassV { float4x4 mvp; float4x4 model; float4 n0; float4 n1; float4 n2; };
struct GlassT { float4 tint; float4 numeral; float4 cam;  // cam.xyz camera position, cam.w = 1 perspective
                float2 size; float bendPx; float tintAmount; float numeralMode; int number; float pad0; float pad1; };
struct GlassOut { float4 position [[position]]; float3 wp; float3 n; float2 local; float cap; };

vertex GlassOut glass_vs(uint vid [[vertex_id]], const device MeshVertex* v [[buffer(0)]],
                         constant GlassV& V [[buffer(1)]]) {
    const MeshVertex m = v[vid];
    GlassOut o;
    o.position = V.mvp * float4(float3(m.pos), 1.0f);
    o.wp = (V.model * float4(float3(m.pos), 1.0f)).xyz;
    const float3 n = float3(m.normal);
    o.n = V.n0.xyz * n.x + V.n1.xyz * n.y + V.n2.xyz * n.z;
    o.local = float2(m.uv);
    o.cap = m.edge;
    return o;
}

fragment float4 glass_fs(GlassOut in [[stage_in]], constant GlassT& T [[buffer(0)]],
                         texture2d<float> sharp [[texture(0)]], texture2d<float> blur [[texture(1)]]) {
    constexpr sampler smp(filter::linear, address::clamp_to_edge);
    const float3 n = normalize(in.n);
    const float3 V = T.cam.w > 0.5f ? normalize(T.cam.xyz - in.wp) : float3(0.0f, 0.0f, 1.0f);  // towards the eye
    const float ndv = saturate(dot(n, V));
    // Refraction: the surface's tilt across the view moves the sample (screen y points down): the curved rim and the
    // walls show what lies beside the button, like a lens edge.
    const float3 lat = n - V * dot(n, V);
    const float2 offPx = float2(-lat.x, lat.y) * T.bendPx;
    const float2 uv = (in.position.xy + offPx) / T.size;
    // Mostly clear: half the frame as it is (half size), half frosted (Fros).
    float3 col = mix(sharp.sample(smp, uv).rgb, blur.sample(smp, uv).rgb, 0.5f);
    // A light tint, the same for every button; the walls, seen through more glass, a little more.
    const float3 tint = T.tint.rgb;
    const float ta = saturate(T.tintAmount * (1.0f + 0.8f * (1.0f - ndv)));
    col = mix(col, col * (0.4f + 1.2f * tint) + 0.1f * tint, ta);
    // The structure: glass seen at a grazing angle (the walls, the rounded edge) darkens a little and catches a
    // sheen; a broad soft highlight from the upper left (a tight one showed as a dot and a hairline).
    const float3 L = normalize(float3(-0.45f, 0.6f, 0.65f));
    const float spec = pow(saturate(dot(reflect(-V, n), L)), 6.0f);
    const float fres = pow(1.0f - ndv, 3.0f);
    col *= 0.8f + 0.2f * ndv;
    col += float3(1.0f) * (0.2f * spec + 0.35f * fres);
    // The number, on the flat cap.
    if (T.number > 0 && in.cap > 0.5f) {
        const float2 q = in.local / 0.47f;
        float dd;
        if (T.number < 10) {
            const float2 s = digit_span(T.number);
            dd = digit_dist(q + float2(0.5f * (s.x + s.y), 0.0f), T.number);
        } else {
            const int a = T.number / 10, b = T.number % 10;
            const float k = 0.9f, gap = 0.3f;
            const float2 sa = digit_span(a), sb = digit_span(b);
            const float bLeft = b == 1 ? 0.0f : sb.x;
            const float ob = sa.y - bLeft + gap;
            const float mid = 0.5f * (sa.x + ob + sb.y);
            dd = min(digit_dist((q - float2(-mid * k, 0.0f)) / k, a) * k,
                     digit_dist((q - float2((ob - mid) * k, 0.0f)) / k, b) * k);
        }
        // Etched-glass numbers: near white, the same on every button, on a soft dark halo so they read over bright
        // and dark visuals alike.
        const float daa = max(fwidth(dd), 1e-4f);
        const float ink = 1.0f - smoothstep(0.065f - daa, 0.065f + daa, dd);
        const float halo = 1.0f - smoothstep(0.065f, 0.17f, dd);
        col = saturate(col) * (1.0f - 0.6f * halo);  // (after the clamp: over white it must still darken)
        col = mix(col, T.numeral.rgb, 0.92f * ink);
    }
    return float4(saturate(col), 1.0f);
}

// The resolved buttons (premultiplied by coverage) over the frame.
// (P.yz: the target's top-left pixel holds the frame's pixel (P.y, P.z): a box drawn at the target's corner.)
fragment float4 glass_composite_fs(FullOut in [[stage_in]], texture2d<float> src [[texture(0)]],
                                   constant float4& P [[buffer(0)]]) {
    const float4 c = src.read(uint2(max(in.position.xy - P.yz, float2(0.0f))));
    return c * P.x;
}

// Text=1: the previous intensity frame's centered square crop into the frame tile. A full-screen triangle; crop =
// (scale x, scale y) of the source UV around the center. 4 bilinear taps half a tile texel apart (a ~4x4 source
// footprint) against aliasing when a 1080-px crop shrinks to 512.
struct FrameTileOut { float4 position [[position]]; float2 uv; };
struct FrameTileP { float2 crop; float2 texel; };

vertex FrameTileOut frame_tile_vs(uint vid [[vertex_id]]) {
    const float2 p = float2((vid << 1) & 2, vid & 2);
    FrameTileOut o;
    o.position = float4(p * 2.0f - 1.0f, 0.0f, 1.0f);
    o.uv = float2(p.x, 1.0f - p.y);
    return o;
}

fragment float4 frame_tile_fs(FrameTileOut in [[stage_in]], constant FrameTileP& P [[buffer(0)]],
                              texture2d<float> src [[texture(0)]]) {
    constexpr sampler smp(filter::linear, address::clamp_to_edge);
    const float2 uv = 0.5f + (in.uv - 0.5f) * P.crop;
    const float2 d = P.texel * 0.25f;
    const float v = 0.25f * (src.sample(smp, uv + float2(-d.x, -d.y)).r + src.sample(smp, uv + float2(d.x, -d.y)).r +
                             src.sample(smp, uv + float2(-d.x, d.y)).r + src.sample(smp, uv + float2(d.x, d.y)).r);
    return float4(saturate(v), 0.0f, 0.0f, 1.0f);
}
)MSL";

static_assert(offsetof(engine::MeshVertex, pos) == 0);
static_assert(offsetof(engine::MeshVertex, normal) == 12);
static_assert(offsetof(engine::MeshVertex, uv) == 24);
static_assert(offsetof(engine::MeshVertex, edge) == 32);
static_assert(sizeof(engine::MeshVertex) == 36);

struct SolidV {
    float mvp[16];
    float n0[4], n1[4], n2[4];  // normal matrix columns
};
static_assert(sizeof(SolidV) == 112, "SolidV must match the MSL struct");
struct SolidF {
    float light[4];
    float fill, shade, edge, edgeWidth;
    int32_t text;
    int32_t pad[3];
};
static_assert(sizeof(SolidF) == 48, "SolidF must match the MSL struct");
struct RubikF {
    float light[4];
    float lv[8];
    float shade, body, pad0, pad1;
};
static_assert(sizeof(RubikF) == 64, "RubikF must match the MSL struct");
struct FifteenF {
    float light[4];
    float level, body, shade;
    int32_t number;
};
static_assert(sizeof(FifteenF) == 32, "FifteenF must match the MSL struct");
struct KnightF {
    float light[4];
    float level, shade;
    int32_t part;
    float line, lightFill, darkFill, width, rim;
};
static_assert(sizeof(KnightF) == 48, "KnightF must match the MSL struct");
struct LabV {
    float mvp[16];
    float n0[4], n1[4], n2[4];
    float local[16];
};
static_assert(sizeof(LabV) == 176, "LabV must match the MSL struct");
struct LabF {
    float light[4];
    float ball[4];
    float level, shade;
    int32_t part;
    float alpha, flash, shadow, pad0, pad1;
};
static_assert(sizeof(LabF) == 64, "LabF must match the MSL struct");
struct SomaF {
    float light[4];
    float col0[4], col1[4], col2[4];
    float level, shade, flash, bevel;
    uint32_t codes0, codes1;
    float ambient, pad;
};
static_assert(sizeof(SomaF) == 96, "SomaF must match the MSL struct");
struct FrameTileP {
    float crop[2];
    float texel[2];  // source pixels per tile pixel, in source UV
};

constexpr int kMeshKinds = int(engine::MeshKind::Shatter) + 1;  // Model .. Shatter: empty (their own pipelines)

// Right-handed rotation about `axis` (as engine::solidModel's).
engine::Mat4 axisRotation(int axis, double a) {
    engine::Mat4 r = engine::Mat4::identity();
    const float c = float(std::cos(a)), s = float(std::sin(a));
    const int i = (axis + 1) % 3, j = (axis + 2) % 3;
    r.m[i * 4 + i] = c;
    r.m[j * 4 + i] = -s;
    r.m[i * 4 + j] = s;
    r.m[j * 4 + j] = c;
    return r;
}
engine::Mat4 translation(float x, float y, float z) {
    engine::Mat4 t = engine::Mat4::identity();
    t.m[12] = x;
    t.m[13] = y;
    t.m[14] = z;
    return t;
}
engine::Mat4 uniformScale(float k) {
    engine::Mat4 t = engine::Mat4::identity();
    t.m[0] = t.m[5] = t.m[10] = k;
    return t;
}
// The rubik cube spans [-1.5, 1.5]^3 in cubie units; this makes its circumradius 1 (Scal = circumradius, as for
// the other meshes).
const float kRubikUnit = float(1.0 / (1.5 * std::sqrt(3.0)));
// The soma in cell units: a 3 x 3 x 3 figure's circumradius is 1 (Scal), like the rubik.
const float kSomaUnit = float(1.0 / (1.5 * std::sqrt(3.0)));
// Towards the soma's light (world, before normalizing): upper left, from the front, so tops are the brightest.
constexpr float kSomaLight[3] = {-0.35f, 0.9f, 0.5f};
constexpr float kSomaAmbient = 0.38f, kSomaBevel = 0.09f;

// The 15 puzzle in board units: cells 1 apart, tiles .94 square and .4 thick (no tray: the gaps and the blank show the
// visuals); a 4.3 square around the board is the circumradius 1 and the screen box.
constexpr float kTray = 4.3f, kTileSize = 0.94f, kTileThick = 0.4f;
const float kFifteenUnit = float(1.0 / (kTray * 0.5 * std::sqrt(2.0)));
// The labyrinth's board: the slab reaches kLabyrinthRim beyond the outer walls' center lines and is this thick.
constexpr float kLabyrinthRim = 0.2f, kLabyrinthSlab = 0.3f;
engine::Mat4 boxScale(float x, float y, float z) {
    engine::Mat4 t = engine::Mat4::identity();
    t.m[0] = x;
    t.m[5] = y;
    t.m[10] = z;
    return t;
}
// The knight's board in board units (1 per square): the board's corner (4, 4) is the circumradius 1. The piece is
// drawn kPieceScale squares per mesh unit (its base .82 of a square wide, 1.18 squares high).
const float kKnightUnit = float(1.0 / (4.0 * std::sqrt(2.0)));
constexpr float kPieceScale = 1.1f;
constexpr float kKnightTop = 2.6f;  // above the board: the highest hop plus the piece (screen box)

std::array<float, 3> normalized(const std::array<float, 3>& v) {
    const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (!(l > 1e-12f)) return {0, 0, 1};
    return {v[0] / l, v[1] / l, v[2] / l};
}

// Cofactor matrix of the model's upper 3x3 (the inverse transpose up to a positive factor; normalized in the shader).
void normalMatrix(const engine::Mat4& m, float n[3][3]) {
    auto a = [&](int r, int c) { return m.m[c * 4 + r]; };
    float cof[3][3];  // [row][col]
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) {
            const int r1 = (r + 1) % 3, r2 = (r + 2) % 3, c1 = (c + 1) % 3, c2 = (c + 2) % 3;
            cof[r][c] = a(r1, c1) * a(r2, c2) - a(r1, c2) * a(r2, c1);
        }
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r) n[c][r] = cof[r][c];  // column c
}

std::array<float, 3> mul3(const engine::Mat4& m, const std::array<float, 3>& v, float w) {
    const auto r = engine::transform(m, {v[0], v[1], v[2], w});
    return {r[0], r[1], r[2]};
}

}  // namespace

// ---------------------------------------------------------------- CPU reference

SolidReference solidReference(const engine::Mesh& mesh, const SolidDraw& d, int width, int height) {
    SolidReference ref;
    const double aspect = double(width) / double(height);
    const engine::Mat4 model = engine::solidModel(d.pose, aspect);
    const engine::Mat4 mvp = engine::solidViewProjection(aspect, d.fovDegrees, d.perspective) * model;
    std::vector<std::array<double, 2>> pts;
    auto project = [&](const std::array<float, 3>& p, double& x, double& y) {
        const auto c = engine::transform(mvp, {p[0], p[1], p[2], 1.0f});
        if (c[3] <= 1e-6f) return false;
        x = (c[0] / c[3] + 1.0) * 0.5 * width;
        y = (1.0 - c[1] / c[3]) * 0.5 * height;
        return true;
    };
    ref.x0 = ref.y0 = 1e9f;
    ref.x1 = ref.y1 = -1e9f;
    for (const auto& v : mesh.vertices) {
        double x, y;
        if (!project({v.pos[0], v.pos[1], v.pos[2]}, x, y)) continue;
        pts.push_back({x, y});
        ref.x0 = std::min(ref.x0, float(x));
        ref.x1 = std::max(ref.x1, float(x));
        ref.y0 = std::min(ref.y0, float(y));
        ref.y1 = std::max(ref.y1, float(y));
    }
    // 2D convex hull (monotone chain) + shoelace.
    std::sort(pts.begin(), pts.end());
    pts.erase(std::unique(pts.begin(), pts.end()), pts.end());
    if (pts.size() >= 3) {
        auto cross = [](const std::array<double, 2>& o, const std::array<double, 2>& a, const std::array<double, 2>& b) {
            return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0]);
        };
        std::vector<std::array<double, 2>> h(2 * pts.size());
        std::size_t k = 0;
        for (std::size_t i = 0; i < pts.size(); ++i) {
            while (k >= 2 && cross(h[k - 2], h[k - 1], pts[i]) <= 0) --k;
            h[k++] = pts[i];
        }
        for (std::size_t i = pts.size() - 1, t = k + 1; i > 0; --i) {
            while (k >= t && cross(h[k - 2], h[k - 1], pts[i - 1]) <= 0) --k;
            h[k++] = pts[i - 1];
        }
        h.resize(k - 1);
        double a = 0;
        for (std::size_t i = 0; i < h.size(); ++i) {
            const auto& p = h[i];
            const auto& q = h[(i + 1) % h.size()];
            a += p[0] * q[1] - q[0] * p[1];
        }
        ref.area = std::fabs(a) * 0.5;
    }
    // Front faces (polyhedra).
    float nm[3][3];
    normalMatrix(model, nm);
    const auto light = normalized(d.shading.light);
    const double camZ = engine::solidCameraZ(d.fovDegrees);
    for (const auto& f : mesh.hullFaces) {
        std::array<float, 3> c{0, 0, 0};
        for (int i : f)
            for (int j = 0; j < 3; ++j) c[std::size_t(j)] += mesh.hullVertices[std::size_t(i)][std::size_t(j)] / float(f.size());
        const auto& a = mesh.hullVertices[std::size_t(f[0])];
        const auto& b = mesh.hullVertices[std::size_t(f[1])];
        const auto& e = mesh.hullVertices[std::size_t(f[2])];
        std::array<float, 3> n{(b[1] - a[1]) * (e[2] - a[2]) - (b[2] - a[2]) * (e[1] - a[1]),
                               (b[2] - a[2]) * (e[0] - a[0]) - (b[0] - a[0]) * (e[2] - a[2]),
                               (b[0] - a[0]) * (e[1] - a[1]) - (b[1] - a[1]) * (e[0] - a[0])};
        if (n[0] * c[0] + n[1] * c[1] + n[2] * c[2] < 0) n = {-n[0], -n[1], -n[2]};  // outward
        std::array<float, 3> wn{nm[0][0] * n[0] + nm[1][0] * n[1] + nm[2][0] * n[2],
                                nm[0][1] * n[0] + nm[1][1] * n[1] + nm[2][1] * n[2],
                                nm[0][2] * n[0] + nm[1][2] * n[1] + nm[2][2] * n[2]};
        wn = normalized(wn);
        const auto wc = mul3(model, c, 1.0f);
        const std::array<float, 3> toCam =
            d.perspective ? normalized({-wc[0], -wc[1], float(camZ) - wc[2]}) : std::array<float, 3>{0, 0, 1};
        const float facing = wn[0] * toCam[0] + wn[1] * toCam[1] + wn[2] * toCam[2];
        if (facing <= 0) continue;
        SolidReference::Face face;
        double x, y;
        if (!project(c, x, y)) continue;
        face.x = float(x);
        face.y = float(y);
        face.facing = facing;
        const float ndl = std::max(0.0f, wn[0] * light[0] + wn[1] * light[1] + wn[2] * light[2]);
        const float lum = (d.shading.fill + (1.0f - d.shading.fill)) * (1.0f + (ndl - 1.0f) * d.shading.shade);
        face.lum = std::clamp(lum, 0.0f, 1.0f);
        ref.faces.push_back(face);
    }
    std::sort(ref.faces.begin(), ref.faces.end(), [](const auto& p, const auto& q) { return p.facing > q.facing; });
    return ref;
}

// ---------------------------------------------------------------- test hook spec

bool parseSolidTest(const std::string& spec, SolidTestSpec& out, std::string* error) {
    auto fail = [&](const std::string& m) {
        if (error) *error = m;
        return false;
    };
    SolidTestSpec s;
    s.text = spec;
    std::vector<std::string> parts;
    std::string cur;
    for (char c : spec) {
        if (c == ',' || c == ':') {
            parts.push_back(cur);
            cur.clear();
        } else if (!std::isspace(static_cast<unsigned char>(c))) {
            cur.push_back(char(std::tolower(static_cast<unsigned char>(c))));
        }
    }
    parts.push_back(cur);
    const auto kind = engine::parseMeshKind(parts[0]);
    if (!kind) return fail("unknown mesh \"" + parts[0] + "\" (tetrahedron, cube, octahedron, dodecahedron, "
                           "icosahedron, sphere, torus, prism, rubik, fifteen, labyrinth, knight, soma)");
    s.draw.mesh = *kind;
    for (std::size_t i = 1; i < parts.size(); ++i) {
        if (parts[i].empty()) continue;
        const auto eq = parts[i].find('=');
        if (eq == std::string::npos) return fail("expected key=value: " + parts[i]);
        const std::string k = parts[i].substr(0, eq), v = parts[i].substr(eq + 1);
        char* end = nullptr;
        const double x = std::strtod(v.c_str(), &end);
        if (v.empty() || !end || *end) return fail("not a number: " + parts[i]);
        auto& p = s.draw.pose;
        auto& sh = s.draw.shading;
        if (k == "x") p.x = x;
        else if (k == "y") p.y = x;
        else if (k == "z") p.z = x;
        else if (k == "scale" || k == "scal") p.scale = x;
        else if (k == "rotx") p.rotX = x;
        else if (k == "roty") p.rotY = x;
        else if (k == "rotz") p.rotZ = x;
        else if (k == "spinx") s.spin[0] = x;
        else if (k == "spiny") s.spin[1] = x;
        else if (k == "spinz") s.spin[2] = x;
        else if (k == "fill") sh.fill = float(x);
        else if (k == "shade" || k == "shad") sh.shade = float(x);
        else if (k == "edge") sh.edge = float(x);
        else if (k == "edgewidth") sh.edgeWidth = float(x);
        else if (k == "lightx") sh.light[0] = float(x);
        else if (k == "lighty") sh.light[1] = float(x);
        else if (k == "lightz") sh.light[2] = float(x);
        else if (k == "pen") s.pen = x;
        else if (k == "epen") s.epen = x;
        else if (k == "flag") s.flags = unsigned(std::max(0.0, x));
        else if (k == "opacity") s.opacity = x;
        else if (k == "pers") s.draw.perspective = x != 0;
        else if (k == "turn") s.turn = x;
        else if (k == "seed") s.seed = x;
        else if (k == "moves") s.moves = int(x);
        else if (k == "shuf") s.shuffle = x;
        else if (k == "glass") s.glass = x != 0;
        else if (k == "sym") s.sym = x;
        else if (k == "hover") s.hover = x;
        else if (k == "board") s.draw.knightLook.board = int(x);
        else if (k == "orbit") s.draw.knightLook.orbit = float(x);
        else if (k == "follow") s.draw.knightLook.follow = float(x);
        else if (k == "flash") s.draw.knightLook.flash = float(x);
        else if (k == "grid") s.grid = int(std::clamp(x, -1e6, 1e6));
        else if (k == "beats") s.beats = int(std::clamp(x, -1e6, 1e6));
        else if (k == "traps") s.traps = int(std::clamp(x, -1e6, 1e6));
        else if (k == "floor") s.draw.floor = float(std::clamp(x, 0.0, 1.0));
        else if (k == "figure") s.figure = int(std::clamp(x, 0.0, 99.0));
        else if (k == "text") {
            if (x != 0 && x != 1 && x != 2) return fail("text must be 0, 1 or 2: " + parts[i]);
            s.draw.text = SolidText(int(x));
        }
        else return fail("unknown key: " + k);
    }
    if (s.draw.mesh == engine::MeshKind::Rubik) {
        const int moves = std::clamp(s.moves, 1, engine::rubik::kMaxMoves);
        s.draw.rubik = engine::rubik::frameAt(uint64_t(std::max(0.0, s.seed)), moves, s.turn < 0 ? 2.0 * moves : s.turn);
    }
    if (s.draw.mesh == engine::MeshKind::Fifteen) {
        const int moves = std::clamp(s.moves, 1, engine::fifteen::kMaxMoves);
        s.draw.fifteen = engine::fifteen::frameAt(uint64_t(std::max(0.0, s.seed)), moves, s.turn < 0 ? double(moves) : s.turn,
                                                  s.shuffle);
    }
    if (s.draw.mesh == engine::MeshKind::Knight) {
        const uint64_t seed = uint64_t(std::clamp(s.seed, 0.0, 1e15));
        s.draw.tour = engine::knight::makeTour(seed, s.sym != 0);
        if (seed > 0) s.draw.prevTour = engine::knight::makeTour(seed - 1, s.sym != 0);
        s.draw.knight = engine::knight::frameAt(s.draw.tour, seed > 0 ? s.draw.prevTour : s.draw.tour,
                                                s.turn < 0 ? double(engine::knight::kSquares) : s.turn, s.hover);
    }
    if (s.draw.mesh == engine::MeshKind::Labyrinth) {
        engine::labyrinth::Player player(s.beats);
        const double turn = s.turn < 0 ? double(s.beats) : s.turn;
        s.draw.labyrinth = player.at(uint64_t(std::clamp(s.seed, 0.0, 1e15)), s.grid, s.beats, turn, s.traps);
    }
    if (s.draw.mesh == engine::MeshKind::Soma) {
        engine::soma::Player player({std::clamp(s.figure, 0, engine::soma::figureCount() - 1)});
        const engine::soma::Timing tm;
        s.draw.soma = player.frame(s.seed, s.turn < 0 ? tm.len : s.turn, tm, 0.0, 0.0,
                                   {s.draw.pose.rotY, s.draw.pose.rotX});
    }
    out = s;
    return true;
}

// ---------------------------------------------------------------- pipeline

SolidPipeline::SolidPipeline(ScenePipeline& scene, const SolidPipelineOptions& opts)
    : scene_(scene), device_(scene.device()->retain()), width_(scene.width()), height_(scene.height()) {
    msaa_ = opts.msaa >= 4 ? 4 : 1;
    if (msaa_ > 1 && !device_->supportsTextureSampleCount(NS::UInteger(msaa_))) msaa_ = 1;
    cullBack_ = opts.cullBack;
    const bool memoryless = device_->supportsFamily(MTL::GPUFamilyApple7);

    // Meshes: one vertex buffer, a range per kind.
    std::vector<engine::MeshVertex> all;
    for (int k = 0; k < kMeshKinds; ++k) {
        meshes_.push_back(engine::makeMesh(engine::MeshKind(k)));
        ranges_.push_back({uint32_t(all.size()), uint32_t(meshes_.back().vertices.size())});
        all.insert(all.end(), meshes_.back().vertices.begin(), meshes_.back().vertices.end());
    }
    // The knight's board and web parts (engine/Knight.hpp), after the MeshKind ranges.
    auto extra = [&](const engine::Mesh& em, Range& r) {
        r = {uint32_t(all.size()), uint32_t(em.vertices.size())};
        all.insert(all.end(), em.vertices.begin(), em.vertices.end());
    };
    extra(engine::knight::makeBoard(), knightBoard_);
    extra(engine::knight::makeSegmentQuad(), knightSegment_);
    extra(engine::knight::makeDisc(24, 0.0), knightDisc_);
    extra(engine::knight::makeDisc(48, 0.82), knightRing_);
    for (Range* r : {&discRange_, &ringRange_}) {  // the labyrinth's holes (disc) and start / goal rings
        const engine::Mesh ring = engine::makeFlatRing(r == &discRange_ ? 0.0 : 0.8);
        *r = {uint32_t(all.size()), uint32_t(ring.vertices.size())};
        all.insert(all.end(), ring.vertices.begin(), ring.vertices.end());
    }
    vertices_ = device_->newBuffer(all.data(), NS::UInteger(all.size() * sizeof(engine::MeshVertex)),
                                   MTL::ResourceStorageModeShared);
    if (!vertices_) throw std::runtime_error("solid vertex buffer allocation failed");
    vertices_->setLabel(NS::String::string("solid meshes", NS::UTF8StringEncoding));

    MTL::Library* lib = compileLibrary(device_, kSolidShaders, "reforce.solid");
    MTL::Function* vs = loadFunction(lib, "solid_vs");
    MTL::Function* fs = loadFunction(lib, "solid_fs");
    MTL::RenderPipelineDescriptor* desc = MTL::RenderPipelineDescriptor::alloc()->init();
    desc->setLabel(NS::String::string("solid", NS::UTF8StringEncoding));
    desc->setVertexFunction(vs);
    desc->setFragmentFunction(fs);
    desc->colorAttachments()->object(0)->setPixelFormat(MTL::PixelFormatRGBA8Unorm);
    desc->setDepthAttachmentPixelFormat(MTL::PixelFormatDepth32Float);
    desc->setRasterSampleCount(NS::UInteger(msaa_));
    NS::Error* err = nullptr;
    pso_ = device_->newRenderPipelineState(desc, &err);
    desc->release();
    vs->release();
    fs->release();
    if (!pso_)
        throw std::runtime_error(std::string("pipeline 'solid' failed: ") +
                                 (err ? err->localizedDescription()->utf8String() : "?"));
    {
        MTL::Function* rvs = loadFunction(lib, "solid_vs");
        MTL::Function* rfs = loadFunction(lib, "rubik_fs");
        MTL::RenderPipelineDescriptor* rd = MTL::RenderPipelineDescriptor::alloc()->init();
        rd->setLabel(NS::String::string("solid rubik", NS::UTF8StringEncoding));
        rd->setVertexFunction(rvs);
        rd->setFragmentFunction(rfs);
        rd->colorAttachments()->object(0)->setPixelFormat(MTL::PixelFormatRGBA8Unorm);
        rd->setDepthAttachmentPixelFormat(MTL::PixelFormatDepth32Float);
        rd->setRasterSampleCount(NS::UInteger(msaa_));
        NS::Error* rerr = nullptr;
        rubikPso_ = device_->newRenderPipelineState(rd, &rerr);
        rd->release();
        rvs->release();
        rfs->release();
        if (!rubikPso_)
            throw std::runtime_error(std::string("pipeline 'solid rubik' failed: ") +
                                     (rerr ? rerr->localizedDescription()->utf8String() : "?"));
        MTL::Function* fvs = loadFunction(lib, "solid_vs");
        MTL::Function* ffs = loadFunction(lib, "fifteen_fs");
        MTL::RenderPipelineDescriptor* fd = MTL::RenderPipelineDescriptor::alloc()->init();
        fd->setLabel(NS::String::string("solid fifteen", NS::UTF8StringEncoding));
        fd->setVertexFunction(fvs);
        fd->setFragmentFunction(ffs);
        fd->colorAttachments()->object(0)->setPixelFormat(MTL::PixelFormatRGBA8Unorm);
        fd->setDepthAttachmentPixelFormat(MTL::PixelFormatDepth32Float);
        fd->setRasterSampleCount(NS::UInteger(msaa_));
        NS::Error* ferr = nullptr;
        fifteenPso_ = device_->newRenderPipelineState(fd, &ferr);
        fd->release();
        fvs->release();
        ffs->release();
        if (!fifteenPso_)
            throw std::runtime_error(std::string("pipeline 'solid fifteen' failed: ") +
                                     (ferr ? ferr->localizedDescription()->utf8String() : "?"));
        MTL::Function* kvs = loadFunction(lib, "solid_vs");
        MTL::Function* kfs = loadFunction(lib, "knight_fs");
        MTL::RenderPipelineDescriptor* kd = MTL::RenderPipelineDescriptor::alloc()->init();
        kd->setLabel(NS::String::string("solid knight", NS::UTF8StringEncoding));
        kd->setVertexFunction(kvs);
        kd->setFragmentFunction(kfs);
        kd->colorAttachments()->object(0)->setPixelFormat(MTL::PixelFormatRGBA8Unorm);
        kd->setDepthAttachmentPixelFormat(MTL::PixelFormatDepth32Float);
        kd->setRasterSampleCount(NS::UInteger(msaa_));
        NS::Error* kerr = nullptr;
        knightPso_ = device_->newRenderPipelineState(kd, &kerr);
        kd->release();
        kvs->release();
        kfs->release();
        if (!knightPso_)
            throw std::runtime_error(std::string("pipeline 'solid knight' failed: ") +
                                     (kerr ? kerr->localizedDescription()->utf8String() : "?"));
        MTL::Function* lvs = loadFunction(lib, "labyrinth_vs");
        MTL::Function* lfs = loadFunction(lib, "labyrinth_fs");
        MTL::RenderPipelineDescriptor* ld = MTL::RenderPipelineDescriptor::alloc()->init();
        ld->setLabel(NS::String::string("solid labyrinth", NS::UTF8StringEncoding));
        ld->setVertexFunction(lvs);
        ld->setFragmentFunction(lfs);
        ld->colorAttachments()->object(0)->setPixelFormat(MTL::PixelFormatRGBA8Unorm);
        ld->setDepthAttachmentPixelFormat(MTL::PixelFormatDepth32Float);
        ld->setRasterSampleCount(NS::UInteger(msaa_));
        NS::Error* lerr = nullptr;
        labyrinthPso_ = device_->newRenderPipelineState(ld, &lerr);
        ld->release();
        lvs->release();
        lfs->release();
        if (!labyrinthPso_)
            throw std::runtime_error(std::string("pipeline 'solid labyrinth' failed: ") +
                                     (lerr ? lerr->localizedDescription()->utf8String() : "?"));
        MTL::Function* svs = loadFunction(lib, "solid_vs");
        MTL::Function* sfs = loadFunction(lib, "soma_fs");
        MTL::RenderPipelineDescriptor* sd = MTL::RenderPipelineDescriptor::alloc()->init();
        sd->setLabel(NS::String::string("solid soma", NS::UTF8StringEncoding));
        sd->setVertexFunction(svs);
        sd->setFragmentFunction(sfs);
        sd->colorAttachments()->object(0)->setPixelFormat(MTL::PixelFormatRGBA8Unorm);
        sd->setDepthAttachmentPixelFormat(MTL::PixelFormatDepth32Float);
        sd->setRasterSampleCount(NS::UInteger(msaa_));
        NS::Error* serr = nullptr;
        somaPso_ = device_->newRenderPipelineState(sd, &serr);
        sd->release();
        svs->release();
        sfs->release();
        if (!somaPso_)
            throw std::runtime_error(std::string("pipeline 'solid soma' failed: ") +
                                     (serr ? serr->localizedDescription()->utf8String() : "?"));
    }
    {
        // Glass: the half-size blur chain and the tile pipeline (straight alpha over the BGRA frame; the opaque
        // variant also raises the tracked alpha).
        auto make = [&](const char* vsName, const char* fsName, MTL::PixelFormat fmt, int blend, const char* label) {
            MTL::Function* v = loadFunction(lib, vsName);
            MTL::Function* f = loadFunction(lib, fsName);
            MTL::RenderPipelineDescriptor* d = MTL::RenderPipelineDescriptor::alloc()->init();
            d->setLabel(NS::String::string(label, NS::UTF8StringEncoding));
            d->setVertexFunction(v);
            d->setFragmentFunction(f);
            auto* a = d->colorAttachments()->object(0);
            a->setPixelFormat(fmt);
            if (blend) {
                a->setBlendingEnabled(true);
                a->setRgbBlendOperation(MTL::BlendOperationAdd);
                a->setSourceRGBBlendFactor(MTL::BlendFactorSourceAlpha);
                a->setDestinationRGBBlendFactor(MTL::BlendFactorOneMinusSourceAlpha);
                a->setAlphaBlendOperation(MTL::BlendOperationAdd);
                a->setSourceAlphaBlendFactor(MTL::BlendFactorOne);
                a->setDestinationAlphaBlendFactor(MTL::BlendFactorOneMinusSourceAlpha);
                if (blend == 1)  // RGB only: the frame's (tracked) alpha is kept
                    a->setWriteMask(MTL::ColorWriteMaskRed | MTL::ColorWriteMaskGreen | MTL::ColorWriteMaskBlue);
            }
            NS::Error* e = nullptr;
            MTL::RenderPipelineState* p = device_->newRenderPipelineState(d, &e);
            d->release();
            v->release();
            f->release();
            if (!p)
                throw std::runtime_error(std::string("pipeline '") + label + "' failed: " +
                                         (e ? e->localizedDescription()->utf8String() : "?"));
            return p;
        };
        downPso_ = make("glass_full_vs", "glass_down_fs", MTL::PixelFormatBGRA8Unorm, 0, "glass downsample");
        blurPso_ = make("glass_full_vs", "glass_blur_fs", MTL::PixelFormatBGRA8Unorm, 0, "glass blur");
        // The buttons: 3D with depth and MSAA into a slot target (RGBA8), like the solids.
        {
            MTL::Function* v = loadFunction(lib, "glass_vs");
            MTL::Function* f = loadFunction(lib, "glass_fs");
            MTL::RenderPipelineDescriptor* d = MTL::RenderPipelineDescriptor::alloc()->init();
            d->setLabel(NS::String::string("glass buttons", NS::UTF8StringEncoding));
            d->setVertexFunction(v);
            d->setFragmentFunction(f);
            d->colorAttachments()->object(0)->setPixelFormat(MTL::PixelFormatRGBA8Unorm);
            d->setDepthAttachmentPixelFormat(MTL::PixelFormatDepth32Float);
            d->setRasterSampleCount(NS::UInteger(msaa_));
            NS::Error* e = nullptr;
            glassPso_ = device_->newRenderPipelineState(d, &e);
            d->release();
            v->release();
            f->release();
            if (!glassPso_)
                throw std::runtime_error(std::string("pipeline 'glass buttons' failed: ") +
                                         (e ? e->localizedDescription()->utf8String() : "?"));
        }
        // Their resolved target over the frame: premultiplied over (RGB only, or the tracked alpha too).
        auto composite = [&](bool withAlpha, const char* label) {
            MTL::Function* v = loadFunction(lib, "glass_full_vs");
            MTL::Function* f = loadFunction(lib, "glass_composite_fs");
            MTL::RenderPipelineDescriptor* d = MTL::RenderPipelineDescriptor::alloc()->init();
            d->setLabel(NS::String::string(label, NS::UTF8StringEncoding));
            d->setVertexFunction(v);
            d->setFragmentFunction(f);
            auto* a = d->colorAttachments()->object(0);
            a->setPixelFormat(MTL::PixelFormatBGRA8Unorm);
            a->setBlendingEnabled(true);
            a->setRgbBlendOperation(MTL::BlendOperationAdd);
            a->setSourceRGBBlendFactor(MTL::BlendFactorOne);
            a->setDestinationRGBBlendFactor(MTL::BlendFactorOneMinusSourceAlpha);
            a->setAlphaBlendOperation(MTL::BlendOperationAdd);
            a->setSourceAlphaBlendFactor(MTL::BlendFactorOne);
            a->setDestinationAlphaBlendFactor(MTL::BlendFactorOneMinusSourceAlpha);
            if (!withAlpha) a->setWriteMask(MTL::ColorWriteMaskRed | MTL::ColorWriteMaskGreen | MTL::ColorWriteMaskBlue);
            NS::Error* e = nullptr;
            MTL::RenderPipelineState* p = device_->newRenderPipelineState(d, &e);
            d->release();
            v->release();
            f->release();
            if (!p)
                throw std::runtime_error(std::string("pipeline '") + label + "' failed: " +
                                         (e ? e->localizedDescription()->utf8String() : "?"));
            return p;
        };
        glassCompositePso_ = composite(false, "glass composite");
        glassCompositeOpaquePso_ = composite(true, "glass composite (opaque alpha)");
        const engine::Mesh button = engine::makeGlassButton();
        glassVertexCount_ = uint32_t(button.vertices.size());
        glassVertices_ = device_->newBuffer(button.vertices.data(),
                                            NS::UInteger(button.vertices.size() * sizeof(engine::MeshVertex)),
                                            MTL::ResourceStorageModeShared);
        if (!glassVertices_) throw std::runtime_error("glass button buffer allocation failed");
        const NS::UInteger hw = std::max<NS::UInteger>(1, width_ / 2), hh = std::max<NS::UInteger>(1, height_ / 2);
        auto half = [&](const char* label) {
            MTL::TextureDescriptor* td =
                MTL::TextureDescriptor::texture2DDescriptor(MTL::PixelFormatBGRA8Unorm, hw, hh, false);
            td->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
            td->setStorageMode(MTL::StorageModePrivate);
            MTL::Texture* t = makeTexture(device_, td);
            if (!t) throw std::runtime_error(std::string(label) + " allocation failed");
            t->setLabel(NS::String::string(label, NS::UTF8StringEncoding));
            return t;
        };
        halfSharp_ = half("glass half frame");
        halfBlur_ = half("glass blur");
        halfScratch_ = half("glass blur scratch");
    }

    {
        MTL::Function* fvs = loadFunction(lib, "frame_tile_vs");
        MTL::Function* ffs = loadFunction(lib, "frame_tile_fs");
        MTL::RenderPipelineDescriptor* fd = MTL::RenderPipelineDescriptor::alloc()->init();
        fd->setLabel(NS::String::string("solid frame tile", NS::UTF8StringEncoding));
        fd->setVertexFunction(fvs);
        fd->setFragmentFunction(ffs);
        fd->colorAttachments()->object(0)->setPixelFormat(MTL::PixelFormatR8Unorm);
        NS::Error* ferr = nullptr;
        framePso_ = device_->newRenderPipelineState(fd, &ferr);
        fd->release();
        fvs->release();
        ffs->release();
        lib->release();
        if (!framePso_)
            throw std::runtime_error(std::string("pipeline 'solid frame tile' failed: ") +
                                     (ferr ? ferr->localizedDescription()->utf8String() : "?"));
    }
    // Face texture tiles (Text=0 / 1) and the line pipeline + buffers for the WaveShape tile: all at startup.
    tileSize_ = std::clamp(opts.tileSize, 32, 4096);
    auto tile = [&](const char* label) {
        MTL::TextureDescriptor* td = MTL::TextureDescriptor::texture2DDescriptor(
            MTL::PixelFormatR8Unorm, NS::UInteger(tileSize_), NS::UInteger(tileSize_), true);
        td->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
        td->setStorageMode(MTL::StorageModePrivate);
        MTL::Texture* t = makeTexture(device_, td);
        if (!t) throw std::runtime_error(std::string(label) + " allocation failed");
        t->setLabel(NS::String::string(label, NS::UTF8StringEncoding));
        return t;
    };
    waveTile_ = tile("solid WaveShape tile");
    frameTile_ = tile("solid frame tile");
    scene_.enableTileLines();

    MTL::DepthStencilDescriptor* dd = MTL::DepthStencilDescriptor::alloc()->init();
    dd->setDepthCompareFunction(MTL::CompareFunctionLess);
    dd->setDepthWriteEnabled(true);
    depth_ = device_->newDepthStencilState(dd);
    dd->release();

    auto scratch = [&](MTL::PixelFormat f, const char* label) {
        MTL::TextureDescriptor* td = MTL::TextureDescriptor::texture2DDescriptor(f, width_, height_, false);
        td->setUsage(MTL::TextureUsageRenderTarget);
        td->setStorageMode(memoryless ? MTL::StorageModeMemoryless : MTL::StorageModePrivate);
        if (msaa_ > 1) {
            td->setTextureType(MTL::TextureType2DMultisample);
            td->setSampleCount(NS::UInteger(msaa_));
        }
        MTL::Texture* t = makeTexture(device_, td);
        if (!t) throw std::runtime_error(std::string("solid ") + label + " allocation failed");
        t->setLabel(NS::String::string(label, NS::UTF8StringEncoding));
        return t;
    };
    if (msaa_ > 1) msaaColor_ = scratch(MTL::PixelFormatRGBA8Unorm, "solid MSAA color");
    depthTex_ = scratch(MTL::PixelFormatDepth32Float, "solid depth");

    const int n = std::clamp(opts.maxLive, 0, 16);
    for (int i = 0; i < n; ++i) {
        MTL::TextureDescriptor* td =
            MTL::TextureDescriptor::texture2DDescriptor(MTL::PixelFormatRGBA8Unorm, width_, height_, false);
        td->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
        td->setStorageMode(MTL::StorageModePrivate);
        Slot s;
        s.tex = makeTexture(device_, td);
        if (!s.tex) throw std::runtime_error("solid target allocation failed");
        s.tex->setLabel(NS::String::string("solid target", NS::UTF8StringEncoding));
        s.handle = scene_.addSolidTexture(s.tex);
        slots_.push_back(s);
    }
}

SolidPipeline::~SolidPipeline() {
    for (auto& s : slots_) {
        scene_.removeSpriteTexture(s.handle);
        s.tex->release();
    }
    if (waveTile_) waveTile_->release();
    if (frameTile_) frameTile_->release();
    if (framePso_) framePso_->release();
    if (msaaColor_) msaaColor_->release();
    if (depthTex_) depthTex_->release();
    if (depth_) depth_->release();
    if (pso_) pso_->release();
    if (rubikPso_) rubikPso_->release();
    if (fifteenPso_) fifteenPso_->release();
    if (knightPso_) knightPso_->release();
    if (labyrinthPso_) labyrinthPso_->release();
    if (somaPso_) somaPso_->release();
    for (auto* p : {downPso_, blurPso_, glassPso_, glassCompositePso_, glassCompositeOpaquePso_})
        if (p) p->release();
    if (glassVertices_) glassVertices_->release();
    for (auto* t : {halfSharp_, halfBlur_, halfScratch_})
        if (t) t->release();
    if (vertices_) vertices_->release();
    device_->release();
}

void SolidPipeline::encodeWaveTile(MTL::CommandBuffer* cmd, const engine::LineSegment* segments, std::size_t count) {
    scene_.encodeTileLines(cmd, waveTile_, segments, count);
    MTL::BlitCommandEncoder* blit = cmd->blitCommandEncoder();
    blit->generateMipmaps(waveTile_);
    blit->endEncoding();
    waveValid_ = true;
}

void SolidPipeline::encodeFrameTile(MTL::CommandBuffer* cmd) {
    MTL::Texture* src = scene_.previousIntensity(cmd);
    // Centered square crop: the short side of the frame fills the tile.
    const float W = float(width_), H = float(height_);
    FrameTileP p{{W > H ? H / W : 1.0f, H > W ? W / H : 1.0f},
                 {std::min(W, H) / float(tileSize_) / W, std::min(W, H) / float(tileSize_) / H}};
    MTL::RenderPassDescriptor* pass = MTL::RenderPassDescriptor::renderPassDescriptor();
    auto* att = pass->colorAttachments()->object(0);
    att->setTexture(frameTile_);
    att->setLoadAction(MTL::LoadActionDontCare);
    att->setStoreAction(MTL::StoreActionStore);
    MTL::RenderCommandEncoder* enc = makeRenderEncoder(cmd, pass);
    enc->setLabel(NS::String::string("solid frame tile", NS::UTF8StringEncoding));
    enc->setRenderPipelineState(framePso_);
    enc->setFragmentTexture(src, 0);
    enc->setFragmentBytes(&p, sizeof(p), 0);
    enc->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(3));
    enc->endEncoding();
    MTL::BlitCommandEncoder* blit = cmd->blitCommandEncoder();
    blit->generateMipmaps(frameTile_);
    blit->endEncoding();
    frameValid_ = true;
}

SolidQuad SolidPipeline::encode(MTL::CommandBuffer* cmd, int slot, const SolidDraw& d) {
    SolidQuad q;
    if (slot < 0 || slot >= maxLive()) return q;
    const engine::Mesh& m = mesh(d.mesh);
    const double aspect = double(width_) / double(height_);
    const engine::Mat4 model = engine::solidModel(d.pose, aspect);
    const engine::Mat4 vp = engine::solidViewProjection(aspect, d.fovDegrees, d.perspective);
    const engine::Mat4 mvp = vp * model;
    const bool rubik = d.mesh == engine::MeshKind::Rubik;
    const bool fifteen = d.mesh == engine::MeshKind::Fifteen;
    const bool labyrinth = d.mesh == engine::MeshKind::Labyrinth;
    const bool knight = d.mesh == engine::MeshKind::Knight;
    // The labyrinth: board units (a cell is 1), the board's corner at the circumradius, tilted about its center.
    const int labN = std::clamp(d.labyrinth.maze.n, engine::labyrinth::kMinGrid, engine::labyrinth::kMaxGrid);
    const float labHalf = 0.5f * float(labN) + kLabyrinthRim;
    engine::Mat4 boardModel = model;
    if (fifteen) boardModel = model * uniformScale(kFifteenUnit);
    if (labyrinth) {
        const double gain = std::isfinite(d.tiltGain) ? std::clamp(double(d.tiltGain), 0.0, 4.0) : 1.0;
        auto angle = [&](float s) { return gain * std::asin(std::clamp(std::isfinite(s) ? double(s) : 0.0, -0.5, 0.5)); };
        boardModel = model * uniformScale(float(1.0 / (labHalf * std::sqrt(2.0)))) *
                     axisRotation(1, angle(d.labyrinth.tiltX)) * axisRotation(0, -angle(d.labyrinth.tiltY));
    }
    // The knight's board: board units (a square is 1), turned about its normal (Orbt), sliding after the knight (Folw).
    if (knight) {
        const engine::knight::Frame& kn = d.knight;
        const float follow = std::isfinite(d.knightLook.follow) ? std::clamp(d.knightLook.follow, 0.0f, 1.0f) : 0.0f;
        const float orbit = std::isfinite(d.knightLook.orbit) ? std::fmod(d.knightLook.orbit, 6.2831853f) : 0.0f;
        const float knX = std::isfinite(kn.x) ? std::clamp(kn.x, -4.0f, 4.0f) : 0.0f;
        const float knY = std::isfinite(kn.y) ? std::clamp(kn.y, -4.0f, 4.0f) : 0.0f;
        boardModel = model * uniformScale(kKnightUnit) * axisRotation(2, orbit) *
                     translation(-follow * knX, -follow * knY, 0.0f);
    }
    const bool soma = d.mesh == engine::MeshKind::Soma;
    // The soma turns about its own vertical first (RotY), then RotX, RotZ, so the figure orbits upright.
    const engine::Mat4 somaTurn =
        axisRotation(2, d.pose.rotZ) * axisRotation(0, d.pose.rotX) * axisRotation(1, d.pose.rotY);
    const engine::Mat4 somaModel = translation(float(d.pose.x * aspect), float(d.pose.y), float(d.pose.z)) * somaTurn *
                                   uniformScale(float(d.pose.scale) * kSomaUnit);
    auto somaPiece = [&](int p) {  // the piece's place: rot (row-major) and pos, in cell units
        const engine::soma::PieceFrame& pf = d.soma.pieces[std::size_t(p)];
        engine::Mat4 m = engine::Mat4::identity();
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) {
                const float v = pf.rot[std::size_t(r * 3 + c)];
                m.m[c * 4 + r] = std::isfinite(v) ? std::clamp(v, -1.0f, 1.0f) : (r == c ? 1.0f : 0.0f);
            }
        for (int k = 0; k < 3; ++k) {
            const float v = pf.pos[std::size_t(k)];
            m.m[12 + k] = std::isfinite(v) ? std::clamp(v, -100.0f, 100.0f) : 0.0f;
        }
        return m;
    };
    const engine::Mat4 cubeModel = rubik ? model * uniformScale(kRubikUnit) : model;
    const engine::Mat4 turnRot =
        rubik && d.rubik.turning ? axisRotation(d.rubik.move.axis, d.rubik.angle) : engine::Mat4::identity();
    engine::ScreenBox b;
    if (rubik) {
        // The cube's corners, and the turning layer's slab corners turned (both convex: their projection holds the
        // whole cube).
        const engine::Mat4 cmvp = vp * cubeModel;
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
        bool any = false;
        auto add = [&](const engine::Mat4& mm, float x, float y, float z) {
            const auto c = engine::transform(mm, {x, y, z, 1.0f});
            if (c[3] <= 1e-6f) return;
            x0 = std::min(x0, c[0] / c[3]);
            x1 = std::max(x1, c[0] / c[3]);
            y0 = std::min(y0, c[1] / c[3]);
            y1 = std::max(y1, c[1] / c[3]);
            any = true;
        };
        const engine::Mat4 tm = cmvp * turnRot;
        for (int k = 0; k < 8; ++k) {
            const float c[3] = {k & 1 ? 1.5f : -1.5f, k & 2 ? 1.5f : -1.5f, k & 4 ? 1.5f : -1.5f};
            add(cmvp, c[0], c[1], c[2]);
            if (d.rubik.turning) {
                float sc[3] = {c[0], c[1], c[2]};
                const int a = d.rubik.move.axis;
                sc[a] = c[a] > 0 ? (d.rubik.move.layer > 0 ? 1.5f : -0.5f) : (d.rubik.move.layer > 0 ? 0.5f : -1.5f);
                add(tm, sc[0], sc[1], sc[2]);
            }
        }
        if (any && !(x1 < -1 || x0 > 1 || y1 < -1 || y0 > 1)) {
            b.x0 = std::max(x0, -1.0f);
            b.y0 = std::max(y0, -1.0f);
            b.x1 = std::min(x1, 1.0f);
            b.y1 = std::min(y1, 1.0f);
            b.visible = b.x1 > b.x0 && b.y1 > b.y0;
        }
    } else if (soma) {
        // Every cube's corners.
        const engine::Mat4 smvp = vp * somaModel;
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
        bool any = false;
        for (int p = 0; p < engine::soma::kPieces; ++p) {
            const engine::Mat4 pm = smvp * somaPiece(p);
            const engine::soma::Piece& pc = engine::soma::pieces()[std::size_t(p)];
            for (int i = 0; i < pc.count; ++i)
                for (int k = 0; k < 8; ++k) {
                    const engine::soma::Cell& cell = pc.cells[std::size_t(i)];
                    const auto c = engine::transform(pm, {cell.x + (k & 1 ? 0.5f : -0.5f), cell.y + (k & 2 ? 0.5f : -0.5f),
                                                          cell.z + (k & 4 ? 0.5f : -0.5f), 1.0f});
                    if (c[3] <= 1e-6f) continue;
                    x0 = std::min(x0, c[0] / c[3]);
                    x1 = std::max(x1, c[0] / c[3]);
                    y0 = std::min(y0, c[1] / c[3]);
                    y1 = std::max(y1, c[1] / c[3]);
                    any = true;
                }
        }
        if (any && !(x1 < -1 || x0 > 1 || y1 < -1 || y0 > 1)) {
            b.x0 = std::max(x0, -1.0f);
            b.y0 = std::max(y0, -1.0f);
            b.x1 = std::min(x1, 1.0f);
            b.y1 = std::min(y1, 1.0f);
            b.visible = b.x1 > b.x0 && b.y1 > b.y0;
        }
    } else if (fifteen || labyrinth || knight) {
        // The board's square and everything above it (tiles, the shuffle's hop; the board's slab, walls and ball; the
        // knight and its hops).
        const engine::Mat4 bmvp = vp * boardModel;
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
        bool any = false;
        for (int k = 0; k < 8; ++k) {
            const float h = labyrinth ? labHalf : knight ? 4.1f : kTray * 0.5f;
            const float z = labyrinth ? (k & 4 ? 1.0f : -kLabyrinthSlab) : knight ? (k & 4 ? kKnightTop : 0.0f)
                                                                                : (k & 4 ? 1.4f : 0.0f);
            const auto c = engine::transform(bmvp, {k & 1 ? h : -h, k & 2 ? h : -h, z, 1.0f});
            if (c[3] <= 1e-6f) {
                // A corner at or behind the camera (a close-up of the knight's board): the whole frame.
                if (knight) x0 = y0 = -1.0f, x1 = y1 = 1.0f, any = true;
                continue;
            }
            x0 = std::min(x0, c[0] / c[3]);
            x1 = std::max(x1, c[0] / c[3]);
            y0 = std::min(y0, c[1] / c[3]);
            y1 = std::max(y1, c[1] / c[3]);
            any = true;
        }
        if (any && !(x1 < -1 || x0 > 1 || y1 < -1 || y0 > 1)) {
            b.x0 = std::max(x0, -1.0f);
            b.y0 = std::max(y0, -1.0f);
            b.x1 = std::min(x1, 1.0f);
            b.y1 = std::min(y1, 1.0f);
            b.visible = b.x1 > b.x0 && b.y1 > b.y0;
        }
    } else {
        b = engine::projectBounds(m, mvp);
    }
    if (!b.visible) return q;
    const int W = int(width_), H = int(height_);
    const int px0 = std::clamp(int(std::floor((b.x0 + 1.0f) * 0.5f * float(W))) - 1, 0, W);
    const int px1 = std::clamp(int(std::ceil((b.x1 + 1.0f) * 0.5f * float(W))) + 1, 0, W);
    const int py0 = std::clamp(int(std::floor((1.0f - b.y1) * 0.5f * float(H))) - 1, 0, H);
    const int py1 = std::clamp(int(std::ceil((1.0f - b.y0) * 0.5f * float(H))) + 1, 0, H);
    const int bw = px1 - px0, bh = py1 - py0;
    if (bw <= 0 || bh <= 0) return q;

    // Shift clip space so the box's top-left pixel lands on the target's (0, 0): x' = x - (2 px0 / W) w,
    // y' = y + (2 py0 / H) w. A pure translation in NDC, so rasterization is unchanged.
    engine::Mat4 adj = engine::Mat4::identity();
    adj.m[12] = -2.0f * float(px0) / float(W);
    adj.m[13] = 2.0f * float(py0) / float(H);
    SolidV v{};
    const engine::Mat4 full = adj * mvp;
    std::copy(std::begin(full.m), std::end(full.m), v.mvp);
    float nm[3][3];
    normalMatrix(model, nm);
    for (int r = 0; r < 3; ++r) {
        v.n0[r] = nm[0][r];
        v.n1[r] = nm[1][r];
        v.n2[r] = nm[2][r];
    }
    const auto l = normalized(d.shading.light);
    int text = int(d.text);
    if ((d.text == SolidText::Wave && !waveValid_) || (d.text == SolidText::Frame && !frameValid_)) text = 3;
    SolidF f{{l[0], l[1], l[2], 0.0f}, d.shading.fill, d.shading.shade, d.shading.edge, d.shading.edgeWidth, text,
             {0, 0, 0}};

    MTL::Texture* target = slots_[std::size_t(slot)].tex;
    MTL::RenderPassDescriptor* pass = MTL::RenderPassDescriptor::renderPassDescriptor();
    auto* att = pass->colorAttachments()->object(0);
    att->setLoadAction(MTL::LoadActionClear);
    att->setClearColor(MTL::ClearColor(0, 0, 0, 0));
    if (msaa_ > 1) {
        att->setTexture(msaaColor_);
        att->setResolveTexture(target);
        att->setStoreAction(MTL::StoreActionMultisampleResolve);
    } else {
        att->setTexture(target);
        att->setStoreAction(MTL::StoreActionStore);
    }
    auto* da = pass->depthAttachment();
    da->setTexture(depthTex_);
    da->setLoadAction(MTL::LoadActionClear);
    da->setClearDepth(1.0);
    da->setStoreAction(MTL::StoreActionDontCare);
    pass->setRenderTargetWidth(NS::UInteger(bw));
    pass->setRenderTargetHeight(NS::UInteger(bh));
    MTL::RenderCommandEncoder* enc = makeRenderEncoder(cmd, pass);
    enc->setLabel(NS::String::string("solid", NS::UTF8StringEncoding));
    enc->setViewport(MTL::Viewport{0.0, 0.0, double(W), double(H), 0.0, 1.0});
    enc->setScissorRect(MTL::ScissorRect{0, 0, NS::UInteger(bw), NS::UInteger(bh)});
    enc->setRenderPipelineState(pso_);
    enc->setDepthStencilState(depth_);
    enc->setFrontFacingWinding(MTL::WindingCounterClockwise);
    enc->setCullMode(cullBack_ ? MTL::CullModeBack : MTL::CullModeNone);
    enc->setVertexBuffer(vertices_, 0, 0);
    enc->setVertexBytes(&v, sizeof(v), 1);
    enc->setFragmentBytes(&f, sizeof(f), 0);
    enc->setFragmentTexture(d.text == SolidText::Frame ? frameTile_ : waveTile_, 0);
    const Range& r = ranges_[std::size_t(d.mesh)];
    if (rubik) {
        enc->setRenderPipelineState(rubikPso_);
        RubikF rf{};
        for (int k = 0; k < 3; ++k) rf.light[k] = l[std::size_t(k)];
        rf.shade = d.shading.shade;
        rf.body = std::clamp(d.body, 0.0f, 1.0f);
        const auto& st = d.rubik.state;
        for (const engine::rubik::Cubie& c : st.cubies) {
            const bool inTurn = d.rubik.turning && c.pos[std::size_t(d.rubik.move.axis)] == d.rubik.move.layer;
            const engine::Mat4 cm = cubeModel * (inTurn ? turnRot : engine::Mat4::identity()) *
                                    translation(float(c.pos[0]), float(c.pos[1]), float(c.pos[2]));
            SolidV cv{};
            const engine::Mat4 cf = adj * vp * cm;
            std::copy(std::begin(cf.m), std::end(cf.m), cv.mvp);
            float cn[3][3];
            normalMatrix(cm, cn);
            for (int k = 0; k < 3; ++k) {
                cv.n0[k] = cn[0][k];
                cv.n1[k] = cn[1][k];
                cv.n2[k] = cn[2][k];
            }
            for (int dir = 0; dir < 6; ++dir) {
                const int col = c.face[std::size_t(dir)];
                rf.lv[dir] = col >= 0 && col < 6 ? std::clamp(d.stickers[std::size_t(col)], 0.0f, 1.0f) : -1.0f;
            }
            enc->setVertexBytes(&cv, sizeof(cv), 1);
            enc->setFragmentBytes(&rf, sizeof(rf), 0);
            enc->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(r.first), NS::UInteger(r.count));
        }
    } else if (fifteen) {
        enc->setRenderPipelineState(fifteenPso_);
        FifteenF ff{};
        for (int k = 0; k < 3; ++k) ff.light[k] = l[std::size_t(k)];
        ff.shade = d.shading.shade;
        ff.body = std::clamp(d.body, 0.0f, 1.0f);
        auto draw = [&](const engine::Mat4& local, int number, float level) {
            const engine::Mat4 cm = boardModel * local;
            SolidV cv{};
            const engine::Mat4 cf = adj * vp * cm;
            std::copy(std::begin(cf.m), std::end(cf.m), cv.mvp);
            float cn[3][3];
            normalMatrix(cm, cn);
            for (int k = 0; k < 3; ++k) {
                cv.n0[k] = cn[0][k];
                cv.n1[k] = cn[1][k];
                cv.n2[k] = cn[2][k];
            }
            ff.number = number;
            ff.level = level;
            enc->setVertexBytes(&cv, sizeof(cv), 1);
            enc->setFragmentBytes(&ff, sizeof(ff), 0);
            enc->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(r.first), NS::UInteger(r.count));
        };
        for (const engine::fifteen::Tile& t : d.fifteen.tiles) {
            if (t.number <= 0) continue;
            const int row = std::clamp((t.number - 1) / engine::fifteen::kSize, 0, 3);
            const float level = std::clamp(d.stickers[std::size_t(kFifteenRowColor[row])], 0.0f, 1.0f);
            draw(translation(t.x, t.y, kTileThick * 0.5f + t.z) * boxScale(kTileSize, kTileSize, kTileThick),
                 t.number, level);
        }
    } else if (knight) {
        encodeKnight(enc, d, boardModel, adj * vp, l);
    } else if (labyrinth) {
        namespace lab = engine::labyrinth;
        const lab::Frame& L = d.labyrinth;
        enc->setRenderPipelineState(labyrinthPso_);
        auto level = [&](int k) { return std::clamp(d.stickers[std::size_t(k)], 0.0f, 1.0f); };
        LabF lf{};
        for (int k = 0; k < 3; ++k) lf.light[k] = l[std::size_t(k)];
        lf.shade = d.shading.shade;
        lf.alpha = std::clamp(d.floor, 0.0f, 1.0f);
        lf.flash = std::clamp(L.flash, 0.0f, 1.0f);
        const float bz = std::isfinite(L.ballZ) ? L.ballZ : 0.0f;
        lf.ball[0] = L.ballX;
        lf.ball[1] = L.ballY;
        lf.ball[2] = float(lab::kBall) + bz;
        lf.ball[3] = float(lab::kBall);
        lf.shadow = std::clamp(1.0f + bz / float(lab::kBall), 0.0f, 1.0f);
        auto draw = [&](const Range& rg, const engine::Mat4& local, int part, float lv) {
            const engine::Mat4 cm = boardModel * local;
            LabV cv{};
            const engine::Mat4 cf = adj * vp * cm;
            std::copy(std::begin(cf.m), std::end(cf.m), cv.mvp);
            std::copy(std::begin(local.m), std::end(local.m), cv.local);
            float cn[3][3];
            normalMatrix(cm, cn);
            for (int k = 0; k < 3; ++k) {
                cv.n0[k] = cn[0][k];
                cv.n1[k] = cn[1][k];
                cv.n2[k] = cn[2][k];
            }
            lf.part = part;
            lf.level = lv;
            enc->setVertexBytes(&cv, sizeof(cv), 1);
            enc->setFragmentBytes(&lf, sizeof(lf), 0);
            enc->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(rg.first), NS::UInteger(rg.count));
        };
        const float floorLv = level(kLabyrinthFloor), wallLv = level(kLabyrinthWalls), ballLv = level(kLabyrinthBall);
        // The board.
        draw(r, translation(0, 0, -0.5f * kLabyrinthSlab) * boxScale(2 * labHalf, 2 * labHalf, kLabyrinthSlab), 0,
             floorLv);
        // Walls of a maze at a height fraction: runs of walls along each grid line, one box per run.
        auto walls = [&](const lab::Maze& m, float frac) {
            const float h = float(lab::kWallHeight) * std::clamp(frac, 0.0f, 1.0f);
            if (m.n != labN || h < 0.004f) return;
            const float w = float(lab::kWall), o = 0.5f * float(m.n - 1);
            for (int vertical = 0; vertical < 2; ++vertical)
                for (int k = 0; k <= m.n; ++k) {
                    int run = -1;
                    for (int i = 0; i <= m.n; ++i) {
                        bool on = false;
                        if (i < m.n) {
                            // horizontal line k: above row k (the last: below row n - 1); vertical: left of column k
                            const int cell = vertical ? i * m.n + std::min(k, m.n - 1) : std::min(k, m.n - 1) * m.n + i;
                            const int side = vertical ? (k < m.n ? lab::West : lab::East) : (k < m.n ? lab::North : lab::South);
                            on = m.wall(cell, side);
                        }
                        if (on && run < 0) run = i;
                        if (!on && run >= 0) {
                            const float len = float(i - run) + w, mid = 0.5f * float(run + i - 1) - o;
                            const float line = float(k) - 0.5f * float(m.n);
                            if (vertical) draw(r, translation(line, -mid, 0.5f * h) * boxScale(w, len, h), 1, wallLv);
                            else draw(r, translation(mid, -line, 0.5f * h) * boxScale(len, w, h), 1, wallLv);
                            run = -1;
                        }
                    }
                }
        };
        // Holes (black discs) and rings of a maze at a size fraction: the goal ringed in the ball's color, the start
        // (a small ball return the ball pops out of) ringed in the walls' color, the traps.
        auto holes = [&](const lab::Maze& m, float frac) {
            const float s = std::clamp(frac, 0.0f, 1.0f);
            if (m.n != labN || s < 0.01f) return;
            const float R = float(lab::kHole) * s;
            auto ring = [&](int cell, float radius, float lv) {
                draw(ringRange_,
                     translation(float(m.cellX(cell)), float(m.cellY(cell)), 0.012f) * boxScale(radius, radius, 1), 1,
                     lv);
            };
            auto disc = [&](int cell, float radius) {
                draw(discRange_,
                     translation(float(m.cellX(cell)), float(m.cellY(cell)), 0.02f) * boxScale(radius, radius, 1), 3,
                     0.0f);
            };
            disc(m.goal, R);
            ring(m.goal, R + 0.09f * s, ballLv);
            disc(m.start, 0.2f * s);
            ring(m.start, 0.3f * s, wallLv);
            for (int i = 0; i < std::clamp(m.trapCount, 0, lab::kMaxTraps); ++i) {
                disc(m.traps[std::size_t(i)], R);
                ring(m.traps[std::size_t(i)], R + 0.05f * s, wallLv);
            }
        };
        holes(L.maze, L.holes);
        holes(L.previous, L.oldHoles);
        walls(L.maze, L.rise);
        walls(L.previous, L.sink);
        if (bz > -2.2f * float(lab::kBall))
            draw(ranges_[std::size_t(engine::MeshKind::Sphere)],
                 translation(L.ballX, L.ballY, float(lab::kBall) + bz) * uniformScale(float(lab::kBall)), 2, ballLv);
    } else if (soma) {
        enc->setRenderPipelineState(somaPso_);
        SomaF sf{};
        const auto sl = normalized({kSomaLight[0], kSomaLight[1], kSomaLight[2]});
        for (int k = 0; k < 3; ++k) sf.light[k] = sl[std::size_t(k)];
        sf.shade = std::clamp(d.shading.shade, 0.0f, 1.0f);
        sf.bevel = kSomaBevel;
        sf.ambient = kSomaAmbient;
        for (int p = 0; p < engine::soma::kPieces; ++p) {
            const engine::soma::PieceFrame& pf = d.soma.pieces[std::size_t(p)];
            const engine::Mat4 place = somaPiece(p);
            // The cube's world rotation (its axes), for the borders' directions: the turn x the piece's rotation.
            engine::Mat4 rot = place;
            rot.m[12] = rot.m[13] = rot.m[14] = 0.0f;
            const engine::Mat4 wr = somaTurn * rot;
            for (int k = 0; k < 3; ++k) {
                sf.col0[k] = wr.m[0 * 4 + k];
                sf.col1[k] = wr.m[1 * 4 + k];
                sf.col2[k] = wr.m[2 * 4 + k];
            }
            sf.level = std::clamp(d.pieces[std::size_t(p)], 0.0f, 1.0f);
            sf.flash = std::isfinite(pf.flash) ? std::clamp(pf.flash, 0.0f, 1.0f) : 0.0f;
            const engine::soma::Piece& pc = engine::soma::pieces()[std::size_t(p)];
            for (int i = 0; i < pc.count; ++i) {
                const engine::soma::Cell& cell = pc.cells[std::size_t(i)];
                const engine::Mat4 cm = somaModel * place * translation(float(cell.x), float(cell.y), float(cell.z));
                SolidV cv{};
                const engine::Mat4 cf = adj * vp * cm;
                std::copy(std::begin(cf.m), std::end(cf.m), cv.mvp);
                float cn[3][3];
                normalMatrix(cm, cn);
                for (int k = 0; k < 3; ++k) {
                    cv.n0[k] = cn[0][k];
                    cv.n1[k] = cn[1][k];
                    cv.n2[k] = cn[2][k];
                }
                const auto codes = engine::soma::edgeCodes(p, i);
                sf.codes0 = codes[0];
                sf.codes1 = codes[1];
                enc->setVertexBytes(&cv, sizeof(cv), 1);
                enc->setFragmentBytes(&sf, sizeof(sf), 0);
                enc->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(r.first), NS::UInteger(r.count));
            }
        }
    } else {
        enc->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(r.first), NS::UInteger(r.count));
    }
    enc->endEncoding();

    q.visible = true;
    q.texture = slots_[std::size_t(slot)].handle;
    q.cx = float(px0) + float(bw) * 0.5f;
    q.cy = float(py0) + float(bh) * 0.5f;
    q.w = float(bw);
    q.h = float(bh);
    q.u0 = 0;
    q.v0 = 0;
    q.u1 = float(bw) / float(W);
    q.v1 = float(bh) / float(H);
    return q;
}

void SolidPipeline::encodeFrost(MTL::CommandBuffer* cmd, MTL::Texture* output, float frost) {
    if (!output) return;
    const float W = float(output->width()), H = float(output->height());
    auto pass = [&](MTL::Texture* target, const char* label) {
        MTL::RenderPassDescriptor* rp = MTL::RenderPassDescriptor::renderPassDescriptor();
        auto* a = rp->colorAttachments()->object(0);
        a->setTexture(target);
        a->setLoadAction(MTL::LoadActionDontCare);
        a->setStoreAction(MTL::StoreActionStore);
        MTL::RenderCommandEncoder* enc = makeRenderEncoder(cmd, rp);
        enc->setLabel(NS::String::string(label, NS::UTF8StringEncoding));
        return enc;
    };
    // The frame at half size, then a separable Gaussian (sigma = frost at 1080p, scaled to this frame).
    {
        MTL::RenderCommandEncoder* enc = pass(halfSharp_, "glass downsample");
        enc->setRenderPipelineState(downPso_);
        const float texel[2] = {1.0f / W, 1.0f / H};
        enc->setFragmentTexture(output, 0);
        enc->setFragmentBytes(texel, sizeof(texel), 0);
        enc->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(3));
        enc->endEncoding();
    }
    const float hw = float(halfSharp_->width()), hh = float(halfSharp_->height());
    const float sigma = std::max(0.5f, (std::isfinite(frost) ? frost : 12.0f) * (H / 1080.0f) * 0.5f);  // half-size texels
    const float step = std::max(1.0f, sigma / 3.0f);
    struct {
        float dir[2];
        float sigma, step;
    } bp{};
    for (int axis = 0; axis < 2; ++axis) {
        MTL::RenderCommandEncoder* enc = pass(axis == 0 ? halfScratch_ : halfBlur_, axis == 0 ? "glass blur x" : "glass blur y");
        enc->setRenderPipelineState(blurPso_);
        bp.dir[0] = axis == 0 ? 1.0f / hw : 0.0f;
        bp.dir[1] = axis == 0 ? 0.0f : 1.0f / hh;
        bp.sigma = sigma;
        bp.step = step;
        enc->setFragmentTexture(axis == 0 ? halfSharp_ : halfScratch_, 0);
        enc->setFragmentBytes(&bp, sizeof(bp), 0);
        enc->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(3));
        enc->endEncoding();
    }
}

void SolidPipeline::encodeKnight(MTL::RenderCommandEncoder* enc, const SolidDraw& d, const engine::Mat4& board,
                                 const engine::Mat4& viewProj, const std::array<float, 3>& light) {
    namespace kt = engine::knight;
    const KnightLook& look = d.knightLook;
    const kt::Frame& kn = d.knight;
    auto fin = [](float v, float lo, float hi, float fb) { return std::isfinite(v) ? std::clamp(v, lo, hi) : fb; };
    const float flash = fin(look.flash, 0.0f, 1.0f, 0.0f);
    KnightF kf{};
    for (int k = 0; k < 3; ++k) kf.light[k] = light[std::size_t(k)];
    kf.shade = std::clamp(d.shading.shade, 0.0f, 1.0f);
    kf.line = fin(look.line, 0.0f, 1.0f, 0.34f) * (1.0f + 0.5f * flash);
    const int style = std::clamp(look.board, 0, 2);
    kf.lightFill = style >= 1 ? fin(look.lightFill, 0.0f, 1.0f, 0.16f) : 0.0f;
    kf.darkFill = style >= 2 ? fin(look.darkFill, 0.0f, 1.0f, 0.07f) : 0.0f;
    kf.width = 0.022f;
    kf.rim = 0.25f;
    auto draw = [&](const engine::Mat4& local, const Range& r, int part, float level) {
        const engine::Mat4 cm = board * local;
        SolidV cv{};
        const engine::Mat4 cf = viewProj * cm;
        std::copy(std::begin(cf.m), std::end(cf.m), cv.mvp);
        float cn[3][3];
        normalMatrix(cm, cn);
        for (int k = 0; k < 3; ++k) {
            cv.n0[k] = cn[0][k];
            cv.n1[k] = cn[1][k];
            cv.n2[k] = cn[2][k];
        }
        kf.part = part;
        kf.level = std::clamp(level, 0.0f, 1.0f);
        enc->setVertexBytes(&cv, sizeof(cv), 1);
        enc->setFragmentBytes(&kf, sizeof(kf), 0);
        enc->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(r.first), NS::UInteger(r.count));
    };
    enc->setRenderPipelineState(knightPso_);
    enc->setCullMode(MTL::CullModeNone);  // the flat parts show from below too
    draw(engine::Mat4::identity(), knightBoard_, 1, 0.0f);

    // The web: lines between the square centers, slightly above the board, each newer one a little higher so it
    // stays on top where lines cross.
    const float width = fin(look.width, 0.005f, 0.5f, 0.075f) * (1.0f + 0.7f * flash);
    const float web = fin(look.web, 0.0f, 1.0f, 0.68f);
    int order = 0;
    auto segment = [&](int a, int b, float part, float level) {
        const float ax = kt::squareX(a), ay = kt::squareY(a);
        const float dx = kt::squareX(b) - ax, dy = kt::squareY(b) - ay;
        const float len = std::sqrt(dx * dx + dy * dy) * part;
        if (!(len > 1e-4f)) return;
        const float z = 0.012f + 0.0002f * float(order++);
        draw(translation(ax, ay, z) * axisRotation(2, std::atan2(dy, dx)) * boxScale(len, width, 1.0f), knightSegment_,
             2, level);
    };
    auto disc = [&](int s, float radius, float z, float level, const Range& r) {
        draw(translation(kt::squareX(s), kt::squareY(s), z) * boxScale(radius, radius, 1.0f), r, 2, level);
    };
    const bool valid = kt::isClosedTour(d.tour);
    const int landed = std::clamp(kn.landed, 0, kt::kSquares);
    const float since = fin(kn.since, 0.0f, 1.0f, 0.0f);
    // What is left of the previous tour's web: it unravels from its first segment over the first .75 of hop 1.
    if (landed == 0 && kt::isClosedTour(d.prevTour)) {
        const float cut = float(kt::kSquares) * std::clamp(since / 0.75f, 0.0f, 1.0f);
        for (int i = 1; i <= kt::kSquares; ++i)
            if (float(i) > cut)
                segment(d.prevTour[std::size_t(i - 1)], d.prevTour[std::size_t(i % kt::kSquares)], 1.0f,
                        web + (1.0f - web) * flash);
    }
    if (valid) {
        for (int i = 1; i <= landed; ++i) {
            const float age = float(landed - i) + since;
            float level = web + (1.0f - web) * std::exp(-age / 6.0f);
            level += (1.0f - level) * flash;
            segment(d.tour[std::size_t(i - 1)], d.tour[std::size_t(i % kt::kSquares)], 1.0f, level);
        }
        if (landed < kt::kSquares && kn.grow > 0.0f)
            segment(d.tour[std::size_t(landed)], d.tour[std::size_t((landed + 1) % kt::kSquares)],
                    std::clamp(kn.grow, 0.0f, 1.0f), 1.0f);
        // Visited squares: a dot each (joining the lines), the start square a ring as well.
        const float dot = width * 1.3f;
        for (int i = 0; i <= std::min(landed, kt::kSquares - 1); ++i)
            disc(d.tour[std::size_t(i)], dot, 0.03f, 0.85f + 0.15f * flash, knightDisc_);
        disc(d.tour[0], dot * 2.6f, 0.028f, 0.8f, knightRing_);
        // The landing: a ring spreads from the square over half a hop.
        if (since < 0.5f && (landed > 0 || kt::isClosedTour(d.prevTour))) {
            const float u = since / 0.5f;
            disc(d.tour[std::size_t(landed % kt::kSquares)], 0.18f + 0.42f * u, 0.026f, 1.0f - u, knightRing_);
        }
    }
    // The knight, lifted a hair off the board so its foot doesn't cut into the squares in a jagged line.
    const float sq = fin(kn.squash, 0.5f, 1.5f, 1.0f);
    const float side = 1.0f / std::sqrt(sq);
    const engine::Mat4 piece =
        translation(fin(kn.x, -8.0f, 8.0f, 0.0f), fin(kn.y, -8.0f, 8.0f, 0.0f), fin(kn.z, -2.0f, 4.0f, 0.0f) + 0.012f) *
        axisRotation(2, fin(kn.heading, -100.0f, 100.0f, 0.0f)) * translation(0.0f, 0.0f, 0.5f) *
        axisRotation(1, -fin(kn.pitch, -1.5f, 1.5f, 0.0f)) * translation(0.0f, 0.0f, -0.5f) *
        boxScale(side, side, sq) * uniformScale(kPieceScale);
    enc->setCullMode(cullBack_ ? MTL::CullModeBack : MTL::CullModeNone);
    draw(piece, ranges_[std::size_t(engine::MeshKind::Knight)], 0, fin(look.piece, 0.0f, 1.0f, 1.0f));
}

void SolidPipeline::encodeGlass(MTL::CommandBuffer* cmd, MTL::Texture* output, const GlassDraw& g, bool trackAlpha) {
    if (!output || !(g.opacity > 0)) return;
    const float W = float(output->width()), H = float(output->height());
    // 1. The frame at half size, sharp and frosted (encodeFrost).
    encodeFrost(cmd, output, g.frost);
    // 2. The buttons in 3D, 4x MSAA with depth, into the puzzle's slot target (cleared; resolved premultiplied).
    if (g.slot < 0 || g.slot >= maxLive()) return;
    MTL::Texture* target = slots_[std::size_t(g.slot)].tex;
    const double aspect = double(width_) / double(height_);
    const engine::Mat4 board = engine::solidModel(g.pose, aspect) * uniformScale(kFifteenUnit);
    const engine::Mat4 vp = engine::solidViewProjection(aspect, 40.0, g.perspective);
    const double camZ = engine::solidCameraZ(40.0);
    {
        MTL::RenderPassDescriptor* rp = MTL::RenderPassDescriptor::renderPassDescriptor();
        auto* att = rp->colorAttachments()->object(0);
        att->setLoadAction(MTL::LoadActionClear);
        att->setClearColor(MTL::ClearColor(0, 0, 0, 0));
        if (msaa_ > 1) {
            att->setTexture(msaaColor_);
            att->setResolveTexture(target);
            att->setStoreAction(MTL::StoreActionMultisampleResolve);
        } else {
            att->setTexture(target);
            att->setStoreAction(MTL::StoreActionStore);
        }
        auto* da = rp->depthAttachment();
        da->setTexture(depthTex_);
        da->setLoadAction(MTL::LoadActionClear);
        da->setClearDepth(1.0);
        da->setStoreAction(MTL::StoreActionDontCare);
        MTL::RenderCommandEncoder* enc = makeRenderEncoder(cmd, rp);
        enc->setLabel(NS::String::string("glass buttons", NS::UTF8StringEncoding));
        enc->setRenderPipelineState(glassPso_);
        enc->setDepthStencilState(depth_);
        enc->setFrontFacingWinding(MTL::WindingCounterClockwise);
        enc->setCullMode(MTL::CullModeBack);
        enc->setVertexBuffer(glassVertices_, 0, 0);
        enc->setFragmentTexture(halfSharp_, 0);
        enc->setFragmentTexture(halfBlur_, 1);
        struct GlassV {
            float mvp[16], model[16];
            float n0[4], n1[4], n2[4];
        };
        struct GlassT {
            float tint[4], numeral[4], cam[4];
            float size[2];
            float bendPx, tintAmount, numeralMode;
            int32_t number;
            float pad0, pad1;
        };
        static_assert(sizeof(GlassT) == 80, "GlassT must match the MSL struct");
        // Pixels per board unit at the board's center (for the refraction's reach).
        const engine::Mat4 bmvp = vp * board;
        auto px = [&](float x, float y) {
            const auto c = engine::transform(bmvp, {x, y, 0.0f, 1.0f});
            const float w = std::fabs(c[3]) > 1e-6f ? c[3] : 1e-6f;
            return std::array<float, 2>{c[0] / w * 0.5f * W, c[1] / w * 0.5f * H};
        };
        const auto p0 = px(0, 0), p1 = px(1, 0), p2 = px(0, 1);
        const float unitPx = 0.5f * (std::hypot(p1[0] - p0[0], p1[1] - p0[1]) + std::hypot(p2[0] - p0[0], p2[1] - p0[1]));
        for (const engine::fifteen::Tile& t : g.fifteen.tiles) {
            if (t.number <= 0) continue;
            const engine::Mat4 model = board * translation(t.x, t.y, t.z);
            const engine::Mat4 mvp = vp * model;
            GlassV v{};
            std::copy(std::begin(mvp.m), std::end(mvp.m), v.mvp);
            std::copy(std::begin(model.m), std::end(model.m), v.model);
            float nm[3][3];
            normalMatrix(model, nm);
            for (int k = 0; k < 3; ++k) {
                v.n0[k] = nm[0][k];
                v.n1[k] = nm[1][k];
                v.n2[k] = nm[2][k];
            }
            const int row = std::clamp((t.number - 1) / engine::fifteen::kSize, 0, 3);
            GlassT gt{};
            for (int k = 0; k < 3; ++k) {
                gt.tint[k] = g.tint[std::size_t(row)][std::size_t(k)];
                gt.numeral[k] = g.numeral[std::size_t(row)][std::size_t(k)];
            }
            gt.cam[2] = float(camZ);
            gt.cam[3] = g.perspective ? 1.0f : 0.0f;
            gt.size[0] = W;
            gt.size[1] = H;
            gt.bendPx = 0.22f * g.bend * unitPx;
            gt.tintAmount = g.tintAmount;
            gt.numeralMode = g.numeralMode;
            gt.number = t.number;
            enc->setVertexBytes(&v, sizeof(v), 1);
            enc->setFragmentBytes(&gt, sizeof(gt), 0);
            enc->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(glassVertexCount_));
        }
        enc->endEncoding();
    }
    // 3. Over the frame.
    encodeGlassComposite(cmd, output, g.slot, g.opacity, trackAlpha && g.opaque);
}

void SolidPipeline::encodeGlassComposite(MTL::CommandBuffer* cmd, MTL::Texture* output, int slot, float opacity,
                                         bool opaque, int x0, int y0, int w, int h) {
    if (!output || slot < 0 || slot >= maxLive()) return;
    const int W = int(output->width()), H = int(output->height());
    if (w < 0 || h < 0) {
        x0 = y0 = 0;
        w = W;
        h = H;
    }
    x0 = std::clamp(x0, 0, W);
    y0 = std::clamp(y0, 0, H);
    w = std::clamp(w, 0, W - x0);
    h = std::clamp(h, 0, H - y0);
    if (w == 0 || h == 0) return;
    MTL::RenderPassDescriptor* rp = MTL::RenderPassDescriptor::renderPassDescriptor();
    auto* a = rp->colorAttachments()->object(0);
    a->setTexture(output);
    a->setLoadAction(MTL::LoadActionLoad);
    a->setStoreAction(MTL::StoreActionStore);
    MTL::RenderCommandEncoder* enc = makeRenderEncoder(cmd, rp);
    enc->setLabel(NS::String::string("glass composite", NS::UTF8StringEncoding));
    enc->setRenderPipelineState(opaque ? glassCompositeOpaquePso_ : glassCompositePso_);
    enc->setScissorRect(MTL::ScissorRect{NS::UInteger(x0), NS::UInteger(y0), NS::UInteger(w), NS::UInteger(h)});
    enc->setFragmentTexture(slots_[std::size_t(slot)].tex, 0);
    const float p[4] = {std::isfinite(opacity) ? std::clamp(opacity, 0.0f, 1.0f) : 0.0f, float(x0), float(y0), 0.0f};
    enc->setFragmentBytes(p, sizeof(p), 0);
    enc->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(3));
    enc->endEncoding();
}

}  // namespace reforce::render
