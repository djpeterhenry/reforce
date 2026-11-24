#include "render/JigsawPipeline.hpp"

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

// The cut, as Metal source (the same file the engine compiles as C++).
#define JIGSAW_SHAPE_SOURCE(...) #__VA_ARGS__
constexpr const char* kShapeSource =
#include "engine/JigsawShape.inc"
    ;
#undef JIGSAW_SHAPE_SOURCE

constexpr const char* kHeader = R"MSL(
#include <metal_stdlib>
using namespace metal;
)MSL";

constexpr const char* kShaders = R"MSL(
struct JigU {
    float2 size;     // output pixels
    float unitPx;    // pixels per board unit (a cell's height)
    float seam;      // the cut on the landed part, 0..1
    float glow;      // completion flash over every seam
    float opacity;
    float frost, sat, dim;  // the empty table
    float focal;     // camera height above the board (board units)
    int cols; int rows; float cw; uint seed;
    int count; float pad0; float pad1; float pad2;
};
struct PieceU {
    float2 home; float2 pos;
    float angle; float z; float tiltX; float tiltY;
    float flash; float pad0;
    float2 ext;
    int col; int row; int index; float pad1;
};

struct FullOut { float4 position [[position]]; float2 uv; };
vertex FullOut jig_full_vs(uint vid [[vertex_id]]) {
    const float2 p = float2((vid << 1) & 2, vid & 2);
    FullOut o;
    o.position = float4(p * 2.0f - 1.0f, 0.0f, 1.0f);
    o.uv = float2(p.x, 1.0f - p.y);
    return o;
}
fragment float4 jig_down_fs(FullOut in [[stage_in]], texture2d<float> src [[texture(0)]],
                            constant float2& texel [[buffer(0)]]) {
    constexpr sampler smp(filter::linear, address::clamp_to_edge);
    const float2 d = texel * 0.5f;
    return 0.25f * (src.sample(smp, in.uv + float2(-d.x, -d.y)) + src.sample(smp, in.uv + float2(d.x, -d.y)) +
                    src.sample(smp, in.uv + float2(-d.x, d.y)) + src.sample(smp, in.uv + float2(d.x, d.y)));
}
struct BlurP { float2 dir; float sigma; float step; };
fragment float4 jig_blur_fs(FullOut in [[stage_in]], texture2d<float> src [[texture(0)]],
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

// The cut map: owner, nearest other piece (255: none), signed distance to the owner's outline in 1/256 px + 128 px.
fragment uint4 jig_cut_fs(FullOut in [[stage_in]], constant JigU& U [[buffer(0)]]) {
    JsGrid g;
    g.cols = U.cols; g.rows = U.rows; g.cw = U.cw; g.seed = U.seed;
    const JsHit h = js_owner(g, in.position.xy / U.unitPx);
    const float gpx = clamp(h.g * U.unitPx, -127.99f, 127.99f);
    const uint q = uint(clamp(round((gpx + 128.0f) * 256.0f), 0.0f, 65535.0f));
    return uint4(uint(max(h.index, 0)), h.index2 < 0 ? 255u : uint(h.index2), q >> 8u, q & 255u);
}
static float cut_distance(uint4 c) { return float(c.b * 256u + c.a) / 256.0f - 128.0f; }

// The cut's look on a piece's top: a bevel along the outline lit from the upper left, a fine dark seam line on the
// outline, the snap's flash. d: pixels inside the outline; n: outward unit normal on screen (y down). seam 0 leaves the
// frame exactly as it is.
static float3 jig_shade(float3 col, float d, float2 n, float seam, float flash, float unitPx) {
    const float bw = 0.035f * unitPx;
    const float b = saturate(1.0f - d / bw);
    const float slope = b * b * 2.2f;
    const float3 N = normalize(float3(n * slope, 1.0f));
    const float3 L = normalize(float3(-0.5f, -0.65f, 0.6f));
    const float diff = dot(N, L) - L.z;  // 0 on the flat top
    col = col * (1.0f + seam * 0.9f * diff) + seam * max(diff, 0.0f) * 0.3f;
    const float line = 1.0f - smoothstep(0.0025f * unitPx, 0.007f * unitPx, d);
    col *= 1.0f - seam * 0.65f * line;
    // A light rim just inside the seam (the cut edge catching light, brighter towards the light): the cut reads on
    // black too.
    const float rim = smoothstep(0.0045f * unitPx, 0.0075f * unitPx, d) *
                      (1.0f - smoothstep(0.0085f * unitPx, 0.014f * unitPx, d));
    const float lit = 0.5f + 0.5f * dot(n, normalize(L.xy));
    col += seam * rim * (0.05f + 0.2f * lit);
    col += flash * (1.0f - smoothstep(0.0f, bw * 1.2f, d)) * 0.85f;
    return col;
}

static float3 jig_table(float3 sharp, float3 blur, constant JigU& U) {
    float3 t = mix(sharp, blur, U.frost);
    const float lum = dot(t, float3(0.2126f, 0.7152f, 0.0722f));
    return mix(float3(lum), t, U.sat) * U.dim;
}

// A slot's look at a pixel `d` px inside it: home (state >= 0: the flash) or the empty table (recessed edges).
static float3 jig_look(float state, float3 src, float3 table, float d, float2 n, constant JigU& U) {
    if (state >= 0.0f) return jig_shade(src, d, n, U.seam, max(state, 0.45f * U.glow), U.unitPx);
    return table * mix(0.45f, 1.0f, smoothstep(0.0f, 0.05f * U.unitPx, max(d, 0.0f)));
}

fragment float4 jig_board_fs(FullOut in [[stage_in]], constant JigU& U [[buffer(0)]],
                             constant float* state [[buffer(1)]], texture2d<float> frame [[texture(0)]],
                             texture2d<float> blur [[texture(1)]], texture2d<uint> cut [[texture(2)]]) {
    constexpr sampler smp(filter::linear, address::clamp_to_edge);
    const uint2 pix = uint2(in.position.xy);
    const float4 src = frame.read(pix);
    const uint4 c = cut.read(pix);
    const float gpx = cut_distance(c);
    const float2 gr = float2(dfdx(gpx), dfdy(gpx));
    const float2 n = length(gr) > 1e-4f ? normalize(gr) : float2(0.0f);
    const float3 table = jig_table(src.rgb, blur.sample(smp, in.uv).rgb, U);
    const int o = min(int(c.r), U.count - 1);
    float3 col = jig_look(state[o], src.rgb, table, -gpx, n, U);
    if (c.g < 255u && -gpx < 1.0f) {
        // Anti-aliased against the piece across the outline.
        const int o2 = min(int(c.g), U.count - 1);
        const float3 other = jig_look(state[o2], src.rgb, table, 0.0f, -n, U);
        col = mix(other, col, saturate(0.5f - gpx));
    }
    return float4(mix(src.rgb, col, U.opacity), 1.0f);
}

// ---- the loose pieces
struct PieceOut { float4 position [[position]]; float2 q; };

// The piece's field at home pixel hp (px; < 0 inside), bilinear over the cut map; far outside reads 40.
static float piece_field(texture2d<uint> cut, float2 hp, int piece) {
    const float2 t = hp - 0.5f;
    const float2 f0 = floor(t);
    const float2 fr = t - f0;
    const int2 i0 = int2(f0);
    const int2 mx = int2(int(cut.get_width()) - 1, int(cut.get_height()) - 1);
    float v[4];
    for (int k = 0; k < 4; ++k) {
        const int2 at = clamp(i0 + int2(k & 1, k >> 1), int2(0), mx);
        const uint4 c = cut.read(uint2(at));
        const float g = cut_distance(c);
        v[k] = int(c.r) == piece ? g : (int(c.g) == piece ? -g : 40.0f);
    }
    // Beyond the frame (border pieces: the clamped taps would extend them): outside, by the distance to the frame.
    const float2 sz = float2(cut.get_width(), cut.get_height());
    const float out = max(max(-hp.x, hp.x - sz.x), max(-hp.y, hp.y - sz.y));
    return max(mix(mix(v[0], v[1], fr.x), mix(v[2], v[3], fr.x), fr.y), out);
}

static float2 rot(float2 v, float a) {
    const float c = cos(a), s = sin(a);
    return float2(c * v.x - s * v.y, s * v.x + c * v.y);
}

static float4 project(float3 P, constant JigU& U) {
    const float2 board = float2(float(U.cols) * U.cw, float(U.rows));
    const float2 center = 0.5f * board;
    const float w = max(0.2f, (U.focal - P.z) / U.focal);
    const float2 scr = center + (P.xy - center) / w;
    const float2 ndc = float2(scr.x / board.x * 2.0f - 1.0f, 1.0f - scr.y / board.y * 2.0f);
    return float4(ndc * w, 0.0f, w);
}

vertex PieceOut jig_piece_vs(uint vid [[vertex_id]], constant JigU& U [[buffer(0)]], constant PieceU& P [[buffer(1)]]) {
    const float2 corner = float2((vid & 1u) != 0u ? 1.0f : -1.0f, (vid & 2u) != 0u ? 1.0f : -1.0f);
    const float2 q = corner * P.ext;
    // Tilt (about x, then y), turn (about the view axis), lift towards the viewer.
    const float cx = cos(P.tiltX), sx = sin(P.tiltX), cy = cos(P.tiltY), sy = sin(P.tiltY);
    float3 v = float3(q.x, q.y * cx, q.y * sx);
    v = float3(v.x * cy + v.z * sy, v.y, -v.x * sy + v.z * cy);
    const float2 r = rot(v.xy, P.angle);
    PieceOut o;
    o.position = project(float3(P.pos + r, P.z + v.z), U);
    o.q = q;
    return o;
}

vertex PieceOut jig_shadow_vs(uint vid [[vertex_id]], constant JigU& U [[buffer(0)]], constant PieceU& P [[buffer(1)]]) {
    const float2 corner = float2((vid & 1u) != 0u ? 1.0f : -1.0f, (vid & 2u) != 0u ? 1.0f : -1.0f);
    const float2 q = corner * P.ext;
    // On the board, offset away from the light (upper left) by the lift, a little larger the higher it is.
    const float2 off = P.z * float2(0.3f, 0.42f);
    PieceOut o;
    o.position = project(float3(P.pos + rot(q * (1.0f + 0.12f * P.z), P.angle) + off, 0.0f), U);
    o.q = q;
    return o;
}

fragment float4 jig_piece_fs(PieceOut in [[stage_in]], constant JigU& U [[buffer(0)]], constant PieceU& P [[buffer(1)]],
                             texture2d<float> frame [[texture(0)]], texture2d<uint> cut [[texture(2)]]) {
    constexpr sampler smp(filter::linear, address::clamp_to_edge);
    const float2 hp = (P.home + in.q) * U.unitPx;
    const float f = piece_field(cut, hp, P.index);
    const float aa = max(fwidth(f), 1e-3f);
    const float cov = saturate(0.5f - f / aa);
    if (cov <= 0.0f) discard_fragment();
    const float2 gr = float2(dfdx(f), dfdy(f));
    const float2 n = length(gr) > 1e-5f ? normalize(gr) : float2(0.0f);
    float3 col = frame.sample(smp, hp / U.size).rgb;
    col = jig_shade(col, max(-f, 0.0f), n, 1.0f, P.flash, U.unitPx);
    const float a = cov * U.opacity;
    return float4(saturate(col) * a, a);
}

fragment float4 jig_shadow_fs(PieceOut in [[stage_in]], constant JigU& U [[buffer(0)]], constant PieceU& P [[buffer(1)]],
                              texture2d<uint> cut [[texture(2)]]) {
    const float2 hp = (P.home + in.q) * U.unitPx;
    const float f = piece_field(cut, hp, P.index);
    const float blur = (0.012f + 0.2f * P.z) * U.unitPx;
    const float a = (0.6f - 0.3f * saturate(P.z * 1.5f)) * smoothstep(-1.0f, 2.0f * blur, -f) * U.opacity;
    if (a <= 0.0f) discard_fragment();
    return float4(0.0f, 0.0f, 0.0f, a);
}
)MSL";

struct JigU {
    float size[2];
    float unitPx, seam, glow, opacity;
    float frost, sat, dim, focal;
    int32_t cols, rows;
    float cw;
    uint32_t seed;
    int32_t count;
    float pad0, pad1, pad2;
};
static_assert(sizeof(JigU) == 72, "JigU must match the MSL struct");
struct PieceU {
    float home[2], pos[2];
    float angle, z, tiltX, tiltY;
    float flash, pad0;
    float ext[2];
    int32_t col, row, index;
    float pad1;
};
static_assert(sizeof(PieceU) == 64, "PieceU must match the MSL struct");

}  // namespace

JigsawTable jigsawTable(int gap) {
    switch (gap) {
        case 1: return {0.0f, 0.0f, 0.5f};   // gray
        case 2: return {1.0f, 0.3f, 0.14f};  // dark
        case 3: return {0.3f, 1.0f, 0.7f};   // ghost
        default: return {1.0f, 0.5f, 0.45f}; // frosted
    }
}

JigsawPipeline::JigsawPipeline(MTL::Device* device, uint32_t width, uint32_t height)
    : device_(device->retain()), width_(std::max(1u, width)), height_(std::max(1u, height)) {
    const std::string src = std::string(kHeader) + kShapeSource + kShaders;
    MTL::Library* lib = compileLibrary(device_, src, "reforce.jigsaw");
    auto make = [&](const char* vsName, const char* fsName, MTL::PixelFormat fmt, bool blend, bool alpha,
                    const char* label) {
        MTL::Function* v = loadFunction(lib, vsName);
        MTL::Function* f = loadFunction(lib, fsName);
        MTL::RenderPipelineDescriptor* d = MTL::RenderPipelineDescriptor::alloc()->init();
        d->setLabel(NS::String::string(label, NS::UTF8StringEncoding));
        d->setVertexFunction(v);
        d->setFragmentFunction(f);
        auto* a = d->colorAttachments()->object(0);
        a->setPixelFormat(fmt);
        if (blend) {  // premultiplied over
            a->setBlendingEnabled(true);
            a->setRgbBlendOperation(MTL::BlendOperationAdd);
            a->setSourceRGBBlendFactor(MTL::BlendFactorOne);
            a->setDestinationRGBBlendFactor(MTL::BlendFactorOneMinusSourceAlpha);
            a->setAlphaBlendOperation(MTL::BlendOperationAdd);
            a->setSourceAlphaBlendFactor(MTL::BlendFactorOne);
            a->setDestinationAlphaBlendFactor(MTL::BlendFactorOneMinusSourceAlpha);
        }
        if (!alpha && fmt == MTL::PixelFormatBGRA8Unorm)
            a->setWriteMask(MTL::ColorWriteMaskRed | MTL::ColorWriteMaskGreen | MTL::ColorWriteMaskBlue);
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
    const auto bgra = MTL::PixelFormatBGRA8Unorm;
    downPso_ = make("jig_full_vs", "jig_down_fs", bgra, false, true, "jigsaw downsample");
    blurPso_ = make("jig_full_vs", "jig_blur_fs", bgra, false, true, "jigsaw blur");
    cutPso_ = make("jig_full_vs", "jig_cut_fs", MTL::PixelFormatRGBA8Uint, false, true, "jigsaw cut");
    for (int k = 0; k < 2; ++k) {
        boardPso_[k] = make("jig_full_vs", "jig_board_fs", bgra, false, k == 1, "jigsaw board");
        piecePso_[k] = make("jig_piece_vs", "jig_piece_fs", bgra, true, k == 1, "jigsaw pieces");
        shadowPso_[k] = make("jig_shadow_vs", "jig_shadow_fs", bgra, true, k == 1, "jigsaw shadows");
    }
    lib->release();

    auto tex = [&](MTL::PixelFormat fmt, uint32_t w, uint32_t h, MTL::TextureUsage usage, const char* label) {
        MTL::TextureDescriptor* td = MTL::TextureDescriptor::texture2DDescriptor(fmt, w, h, false);
        td->setUsage(usage);
        td->setStorageMode(MTL::StorageModePrivate);
        MTL::Texture* t = makeTexture(device_, td);
        if (!t) throw std::runtime_error(std::string(label) + " allocation failed");
        t->setLabel(NS::String::string(label, NS::UTF8StringEncoding));
        return t;
    };
    const auto rt = MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead;
    frame_ = tex(bgra, width_, height_, MTL::TextureUsageShaderRead, "jigsaw frame copy");
    const uint32_t hw = std::max(1u, width_ / 2), hh = std::max(1u, height_ / 2);
    halfSharp_ = tex(bgra, hw, hh, rt, "jigsaw half frame");
    halfBlur_ = tex(bgra, hw, hh, rt, "jigsaw blur");
    halfScratch_ = tex(bgra, hw, hh, rt, "jigsaw blur scratch");
    cut_[0] = tex(MTL::PixelFormatRGBA8Uint, width_, height_, rt, "jigsaw cut map");
    cut_[1] = tex(MTL::PixelFormatRGBA8Uint, width_, height_, rt, "jigsaw cut map 2");
}

JigsawPipeline::~JigsawPipeline() {
    for (auto* p : {downPso_, blurPso_, cutPso_, boardPso_[0], boardPso_[1], piecePso_[0], piecePso_[1], shadowPso_[0],
                    shadowPso_[1]})
        if (p) p->release();
    for (auto* t : {frame_, halfSharp_, halfBlur_, halfScratch_, cut_[0], cut_[1]})
        if (t) t->release();
    device_->release();
}

void JigsawPipeline::encode(MTL::CommandBuffer* cmd, MTL::Texture* output, const JigsawDraw& d, bool trackAlpha) {
    const engine::jigsaw::Frame& f = d.frame;
    if (!output || !(d.opacity > 0) || f.whole || f.count <= 0) return;
    if (output->width() != width_ || output->height() != height_) return;
    const engine::jigsaw::Grid& g = f.grid;
    const float W = float(width_), H = float(height_);
    auto pass = [&](MTL::Texture* target, MTL::LoadAction load, const char* label) {
        MTL::RenderPassDescriptor* rp = MTL::RenderPassDescriptor::renderPassDescriptor();
        auto* a = rp->colorAttachments()->object(0);
        a->setTexture(target);
        a->setLoadAction(load);
        a->setStoreAction(MTL::StoreActionStore);
        MTL::RenderCommandEncoder* enc = makeRenderEncoder(cmd, rp);
        enc->setLabel(NS::String::string(label, NS::UTF8StringEncoding));
        return enc;
    };
    JigU u{};
    u.size[0] = W;
    u.size[1] = H;
    u.unitPx = H / float(g.rows);
    u.seam = std::clamp(f.seam, 0.0f, 1.0f);
    u.glow = std::clamp(f.glow, 0.0f, 1.0f);
    u.opacity = std::clamp(d.opacity, 0.0f, 1.0f);
    u.frost = std::clamp(d.table.frost, 0.0f, 1.0f);
    u.sat = std::clamp(d.table.sat, 0.0f, 1.0f);
    u.dim = std::clamp(d.table.dim, 0.0f, 1.0f);
    u.focal = 2.2f * float(g.rows);
    u.cols = g.cols;
    u.rows = g.rows;
    u.cw = g.cw;
    u.seed = g.seed;
    u.count = f.count;

    // 1. The frame, and its half-size sharp and blurred copies (the table's frost).
    {
        MTL::BlitCommandEncoder* blit = cmd->blitCommandEncoder();
        blit->copyFromTexture(output, frame_);
        blit->endEncoding();
    }
    {
        MTL::RenderCommandEncoder* enc = pass(halfSharp_, MTL::LoadActionDontCare, "jigsaw downsample");
        enc->setRenderPipelineState(downPso_);
        const float texel[2] = {1.0f / W, 1.0f / H};
        enc->setFragmentTexture(frame_, 0);
        enc->setFragmentBytes(texel, sizeof(texel), 0);
        enc->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(3));
        enc->endEncoding();
    }
    const float hw = float(halfSharp_->width()), hh = float(halfSharp_->height());
    const float sigma = std::max(0.5f, std::clamp(d.frost, 0.0f, 60.0f) * (H / 1080.0f) * 0.5f);
    struct {
        float dir[2];
        float sigma, step;
    } bp{};
    for (int axis = 0; axis < 2; ++axis) {
        MTL::RenderCommandEncoder* enc = pass(axis == 0 ? halfScratch_ : halfBlur_, MTL::LoadActionDontCare,
                                              axis == 0 ? "jigsaw blur x" : "jigsaw blur y");
        enc->setRenderPipelineState(blurPso_);
        bp.dir[0] = axis == 0 ? 1.0f / hw : 0.0f;
        bp.dir[1] = axis == 0 ? 0.0f : 1.0f / hh;
        bp.sigma = sigma;
        bp.step = std::max(1.0f, sigma / 3.0f);
        enc->setFragmentTexture(axis == 0 ? halfSharp_ : halfScratch_, 0);
        enc->setFragmentBytes(&bp, sizeof(bp), 0);
        enc->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(3));
        enc->endEncoding();
    }
    // 2. The cut map, when the cut changed (two kept; the least recently used one is rebuilt).
    auto same = [&](int i) {
        const auto& c = cutGrid_[i];
        return cutValid_[i] && c.cols == g.cols && c.rows == g.rows && c.cw == g.cw && c.seed == g.seed;
    };
    int slot = same(0) ? 0 : same(1) ? 1 : -1;
    if (slot < 0) {
        slot = !cutValid_[0] ? 0 : !cutValid_[1] ? 1 : cutUsed_[0] <= cutUsed_[1] ? 0 : 1;
        MTL::RenderCommandEncoder* enc = pass(cut_[slot], MTL::LoadActionDontCare, "jigsaw cut");
        enc->setRenderPipelineState(cutPso_);
        enc->setFragmentBytes(&u, sizeof(u), 0);
        enc->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(3));
        enc->endEncoding();
        cutGrid_[slot] = g;
        cutValid_[slot] = true;
        ++cutBuilds_;
    }
    cutUsed_[slot] = ++uses_;
    lastCut_ = slot;
    MTL::Texture* cut = cut_[slot];
    // 3. The board, then 4. the loose pieces with their shadows, bottom to top.
    float state[engine::jigsaw::kMaxPieces];
    for (int p = 0; p < f.count; ++p) {
        const auto& pc = f.pieces[std::size_t(p)];
        state[p] = pc.landed ? std::clamp(pc.flash, 0.0f, 1.0f) : -1.0f;
    }
    const int k = trackAlpha && d.opaque ? 1 : 0;
    MTL::RenderCommandEncoder* enc = pass(output, MTL::LoadActionLoad, "jigsaw board");
    enc->setRenderPipelineState(boardPso_[k]);
    enc->setFragmentBytes(&u, sizeof(u), 0);
    enc->setFragmentBytes(state, sizeof(float) * std::size_t(f.count), 1);
    enc->setFragmentTexture(frame_, 0);
    enc->setFragmentTexture(halfBlur_, 1);
    enc->setFragmentTexture(cut, 2);
    enc->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(3));
    if (f.loose > 0) {
        enc->setVertexBytes(&u, sizeof(u), 0);
        for (int i = 0; i < f.loose; ++i) {
            const int p = f.order[std::size_t(i)];
            if (p < 0 || p >= f.count) continue;
            const auto& pc = f.pieces[std::size_t(p)];
            const auto h = engine::jigsaw::homeCenter(g, p);
            PieceU pu{};
            pu.home[0] = h.x;
            pu.home[1] = h.y;
            pu.pos[0] = pc.x;
            pu.pos[1] = pc.y;
            pu.angle = pc.angle;
            pu.z = std::clamp(pc.z, 0.0f, 2.0f * float(g.rows));
            pu.tiltX = pc.tiltX;
            pu.tiltY = pc.tiltY;
            pu.flash = std::clamp(pc.flash, 0.0f, 1.0f);
            // Half size of the quad: the cell, the jitter and the knobs.
            pu.ext[0] = 0.5f * g.cw + 0.42f * std::min(g.cw, 1.0f) + 0.1f * g.cw;
            pu.ext[1] = 0.5f + 0.42f * std::min(g.cw, 1.0f) + 0.1f;
            pu.col = p % g.cols;
            pu.row = p / g.cols;
            pu.index = p;
            enc->setVertexBytes(&pu, sizeof(pu), 1);
            enc->setFragmentBytes(&pu, sizeof(pu), 1);
            if (pu.z > 1e-4f) {
                enc->setRenderPipelineState(shadowPso_[k]);
                enc->drawPrimitives(MTL::PrimitiveTypeTriangleStrip, NS::UInteger(0), NS::UInteger(4));
            }
            enc->setRenderPipelineState(piecePso_[k]);
            enc->drawPrimitives(MTL::PrimitiveTypeTriangleStrip, NS::UInteger(0), NS::UInteger(4));
        }
    }
    enc->endEncoding();
}

}  // namespace reforce::render
