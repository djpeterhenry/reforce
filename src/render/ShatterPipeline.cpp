#include "render/ShatterPipeline.hpp"

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>

#include "render/GpuPoison.hpp"
#include "render/ShaderCompiler.hpp"

namespace reforce::render {
namespace {

namespace sh = engine::shatter;

// The fracture, as Metal source (the same file the engine compiles as C++).
#define SHATTER_SHAPE_SOURCE(...) #__VA_ARGS__
constexpr const char* kShapeSource =
#include "engine/ShatterShape.inc"
    ;
#undef SHATTER_SHAPE_SOURCE

constexpr const char* kHeader = R"MSL(
#include <metal_stdlib>
using namespace metal;
#define SH_CONST constant

struct FullOut { float4 position [[position]]; float2 uv; };
vertex FullOut sh_full_vs(uint vid [[vertex_id]]) {
    const float2 p = float2((vid << 1) & 2, vid & 2);
    FullOut o;
    o.position = float4(p * 2.0f - 1.0f, 0.0f, 1.0f);
    o.uv = float2(p.x, 1.0f - p.y);
    return o;
}
)MSL";

// The crack map: shard slot, nearest crack element, position along it (1/65535), distance to it (1/32 px).
constexpr const char* kMapShaders = R"MSL(
fragment uint4 sh_map_fs(FullOut in [[stage_in]], constant ShPane& P [[buffer(0)]], constant float& H [[buffer(1)]]) {
    const ShHit h = sh_owner(P, in.position.x / H, in.position.y / H);
    return uint4(uint(h.slot), uint(h.elem), uint(clamp(h.param, 0.0f, 1.0f) * 65535.0f + 0.5f),
                 uint(clamp(h.dist * H * 32.0f, 0.0f, 65535.0f)));
}
)MSL";

constexpr const char* kDrawShaders = R"MSL(
struct PaneU {
    float2 size; float H; float depthPx;
    float offsetPx; float opacity; float sheen; float spec;
    float line; float sparkle; float beat; float build;
    int line16; int opaque; int count; float s;
    float thick; float pad0; float pad1; float pad2;
    float4 imp[6];  // x, y (px, pane), r0 (px), beats since it landed (< 0: not yet)
};
struct ShardI {
    float2 home; float2 pos; float4 box;
    float4 r0; float4 r1; float4 r2;
    float z; float flash; float alpha; int slot;
};

static uint gl_hash(uint x) {
    x ^= x >> 16u; x *= 0x7feb352du; x ^= x >> 15u; x *= 0x846ca68bu; x ^= x >> 16u;
    return x;
}
static float gl_rand(uint a, uint b) { return float(gl_hash(a * 0x9E3779B1u ^ b * 0x85EBCA77u ^ 0x2545F491u) >> 8u) / 16777216.0f; }

static float3 sampleFrame(texture2d<float> t, float2 uv, float2 dx, float2 dy) {
    constexpr sampler tri(filter::linear, mip_filter::linear, address::clamp_to_edge);
    return t.sample(tri, uv, gradient2d(dx, dy)).rgb;
}

// The view ray (straight in) through a front face of normal N into glass of index eta and out through a back face of
// normal B, on to the visuals depthPx behind: where it lands, px from straight behind. (N = B: a slab, no bend.)
static float2 bend(float3 N, float3 B, float eta, float depthPx) {
    const float3 V = float3(0.0f, 0.0f, -1.0f);
    const float3 t1 = refract(V, N, 1.0f / eta);
    const float3 t2 = refract(normalize(t1), B, eta);
    if (dot(t2, t2) < 1e-6f) return normalize(N.xy + 1e-6f) * depthPx;  // total internal reflection: far off
    return t2.xy / max(-t2.z, 0.1f) * depthPx;
}

// Three channels through the glass: a touch of dispersion.
static float3 through(texture2d<float> frame, float2 px, float2 size, float3 N, float3 B, float depthPx, float2 shift) {
    const float2 oR = bend(N, B, 1.50f, depthPx) + shift, oG = bend(N, B, 1.52f, depthPx) + shift;
    const float2 oB = bend(N, B, 1.545f, depthPx) + shift;
    const float2 uvG = (px + oG) / size;
    const float2 dx = dfdx(uvG), dy = dfdy(uvG);
    float3 c = float3(sampleFrame(frame, (px + oR) / size, dx, dy).r, sampleFrame(frame, uvG, dx, dy).g,
                      sampleFrame(frame, (px + oB) / size, dx, dy).b);
    // Rays that leave the frame see the dark room.
    const float beyond = max(max(-uvG.x, uvG.x - 1.0f), max(-uvG.y, uvG.y - 1.0f));
    return c * mix(0.06f, 1.0f, 1.0f - smoothstep(0.0f, 0.1f, beyond));
}

constant float3 kLight = float3(-0.42f, -0.55f, 0.72f);  // towards the soft light, upper left (y down)

// ---- the pane
fragment float4 sh_pane_fs(FullOut in [[stage_in]], constant PaneU& U [[buffer(0)]],
                           const device float2* elems [[buffer(1)]], const device float2* tilts [[buffer(2)]],
                           texture2d<float> frame [[texture(0)]], texture2d<uint> map [[texture(1)]]) {
    const float2 px = in.position.xy;
    const int2 mx = int2(U.size) - 1;
    const float4 orig = frame.read(uint2(clamp(int2(px), int2(0), mx)), 0);
    const float edge = U.size.y + U.offsetPx - px.y;  // px above the pane's bottom edge (sliding in)
    if (edge <= -0.5f) return orig;
    const float s = U.s;
    const float2 pp = float2(px.x, px.y - U.offsetPx);  // where this is on the pane
    const uint4 m = map.read(uint2(clamp(int2(pp), int2(0), mx)));
    const float param = float(m.b) / 65535.0f;
    const float dist = float(m.a) / 32.0f;
    const float2 st = elems[m.g];
    // How much of the nearest crack has grown past this point (a soft tip).
    float vis = 0.0f;
    if (abs(st.x) >= 0.999f) vis = 1.0f;
    else if (st.x > 0.0f) vis = saturate((st.x - param) * 30.0f + 0.5f);
    else if (st.x < 0.0f) vis = saturate((-st.x - (1.0f - param)) * 30.0f + 0.5f);
    vis *= U.line;
    // The surface: the chipped edges lean into the crack; the cracked-loose piece tilts a hair; the sliding pane's
    // bottom edge rounds down.
    const float2 gd = float2(dfdx(dist), dfdy(dist));
    const float2 n2 = length(gd) > 1e-4f ? normalize(gd) : float2(0.0f);
    const float bev = vis * (1.0f - smoothstep(0.0f, 3.5f * s, dist));
    float2 g = n2 * 0.55f * bev * bev + tilts[m.r];
    float rim = 0.0f;
    if (U.offsetPx < -0.5f) {
        const float e = saturate(1.0f - edge / (8.0f * s));
        g += float2(0.0f, 0.9f * e * e);
        rim = 1.0f - smoothstep(0.0f, 1.6f * s, edge);
    }
    const float3 N = normalize(float3(-g, 1.0f));
    float3 col = through(frame, px, U.size, N, float3(0.0f, 0.0f, 1.0f), U.depthPx, float2(0.0f));
    // Fresnel over what flat glass reflects (flat glass leaves the frame as it is), a dim room, the highlight.
    const float F = 0.96f * pow(1.0f - saturate(N.z), 5.0f);
    const float3 R = reflect(float3(0.0f, 0.0f, -1.0f), N);
    const float rl = saturate(dot(R, normalize(kLight)));
    const float hl = U.spec * (2.2f * pow(rl, 90.0f) + 0.25f * pow(rl, 12.0f)) * smoothstep(0.02f, 0.12f, length(g));
    col = col * (1.0f - F) + F * float3(0.12f, 0.13f, 0.15f) + hl;
    // The crack: a fine silvery line with a darker fringe, so it reads over black and white; its brightness wanders
    // along it (the fracture face turns towards the light and away).
    const float core = vis * (1.0f - smoothstep(0.35f * s, 1.15f * s, dist));
    const float fringe = vis * smoothstep(0.7f * s, 1.4f * s, dist) * (1.0f - smoothstep(1.4f * s, 3.2f * s, dist));
    const float wx = param * 6.0f;
    const float wf = fract(wx);
    const float wander = mix(gl_rand(m.g, uint(wx)), gl_rand(m.g, uint(wx) + 1u), wf * wf * (3.0f - 2.0f * wf));
    col *= 1.0f - 0.28f * fringe;
    col = mix(col, float3(0.88f, 0.92f, 0.95f), (0.42f + 0.32f * wander) * core);
    // Glints along it (a few cells twinkle); in a build more tick on the 16th lines; the flash as it lands.
    const float near = vis * (1.0f - smoothstep(0.0f, 2.6f * s, dist));
    const uint cell = uint(param * 40.0f);
    const float hsh = gl_rand(m.g, cell);
    float tw = pow(hsh, 14.0f) * (0.55f + 0.45f * sin(U.beat * 3.7f + hsh * 50.0f));
    if (U.build > 0.0f) {
        const float hb = gl_rand(m.g * 7u + 3u, cell ^ (uint(U.line16) * 0x9E37u));
        tw += U.build * step(0.9f, hb) * exp(-fract(U.beat * 4.0f) * 4.0f);
    }
    col += U.sparkle * 1.6f * tw * near * float3(1.0f, 0.97f, 0.9f);
    col += st.y * near * 0.85f * float3(0.95f, 0.97f, 1.0f);
    // The impacts: the crushed spot (white, mottled) and the flash as it lands.
    for (int i = 0; i < U.count; ++i) {
        const float4 ip = U.imp[i];
        if (ip.w < 0.0f) continue;
        const float r = length(pp - ip.xy);
        const float crush = 1.0f - smoothstep(0.25f * ip.z, 0.85f * ip.z, r);
        if (crush > 0.0f) {
            const float n = gl_rand(uint(pp.x * 0.5f), uint(pp.y * 0.5f) + uint(i) * 977u);
            col = mix(col, float3(0.8f, 0.84f, 0.88f) * (0.7f + 0.45f * n), 0.6f * crush * U.line);
        }
        const float fr = ip.z * (1.5f + 6.0f * ip.w);
        col += 1.3f * exp(-ip.w / 0.25f) * exp(-(r * r) / (fr * fr));
    }
    // The intact glass: a faint broad reflection (stronger as the pane settles); the sliding pane's edge.
    const float2 uv = px / U.size;
    const float band = dot(uv - 0.5f, normalize(float2(1.0f, 0.6f)));
    col += U.sheen * (0.012f + 0.03f * (1.0f - smoothstep(0.0f, 0.3f, abs(band + 0.12f)))) * float3(0.9f, 0.95f, 1.0f);
    col += 0.45f * rim * float3(0.85f, 1.0f, 0.95f);
    const float3 rgb = mix(orig.rgb, saturate(col), U.opacity);
    float a = orig.a;
    if (U.opaque != 0) a = max(a, U.opacity * saturate(core + near * 0.5f + rim));
    return float4(rgb, a);
}

// ---- the shards
struct ShardOut { float4 position [[position]]; float2 q; float3 nrm; uint iid [[flat]]; };

vertex ShardOut sh_shard_vs(uint vid [[vertex_id]], uint iid [[instance_id]], const device ShardI* S [[buffer(0)]],
                            constant PaneU& U [[buffer(1)]]) {
    const ShardI I = S[iid];
    const float2 c = float2((vid & 1u) != 0u ? I.box.z : I.box.x, (vid & 2u) != 0u ? I.box.w : I.box.y);
    const float3 l = float3(c, 0.0f);
    const float3 P = float3(I.pos, I.z) + float3(dot(I.r0.xyz, l), dot(I.r1.xyz, l), dot(I.r2.xyz, l));
    const float F = 2.2f * U.H;
    const float w = max(0.05f, (F - P.z) / F);
    const float2 center = 0.5f * U.size;
    const float2 scr = center + (P.xy - center) / w;
    ShardOut o;
    o.position = float4((scr.x / U.size.x * 2.0f - 1.0f) * w, (1.0f - scr.y / U.size.y * 2.0f) * w, 0.0f, w);
    o.q = c;
    o.nrm = float3(I.r0.z, I.r1.z, I.r2.z);
    o.iid = iid;
    return o;
}

// The shard's field at home pixel hp (px, > 0 inside: about the distance to its cracks), bilinear over the crack map.
static float shard_field(texture2d<uint> map, float2 hp, int slot) {
    const float2 t = hp - 0.5f;
    const float2 f0 = floor(t);
    const float2 fr = t - f0;
    const int2 i0 = int2(f0);
    const int2 mx = int2(int(map.get_width()) - 1, int(map.get_height()) - 1);
    float v[4];
    for (int k = 0; k < 4; ++k) {
        const uint4 c = map.read(uint2(clamp(i0 + int2(k & 1, k >> 1), int2(0), mx)));
        const float d = float(c.a) / 32.0f;
        v[k] = int(c.r) == slot ? d + 0.5f : -0.5f - min(d, 8.0f);
    }
    return mix(mix(v[0], v[1], fr.x), mix(v[2], v[3], fr.x), fr.y);
}

fragment float4 sh_shard_fs(ShardOut in [[stage_in]], const device ShardI* S [[buffer(0)]], constant PaneU& U [[buffer(1)]],
                            texture2d<float> frame [[texture(0)]], texture2d<uint> map [[texture(1)]]) {
    const ShardI I = S[in.iid];
    const float2 hp = I.home + in.q;
    const float f = shard_field(map, hp, I.slot);
    // Coverage: inside its cracks and the frame (the pane ends there: no bevel along the frame's edge).
    const float inFrame = min(min(hp.x, U.size.x - hp.x), min(hp.y, U.size.y - hp.y));
    const float fc = min(f, inFrame);
    const float aa = max(fwidth(fc), 1e-3f);
    const float cov = saturate(0.5f + fc / aa);
    if (cov <= 0.0f) discard_fragment();
    const float s = U.s;
    float3 N0 = normalize(in.nrm);
    if (N0.z < 0.0f) N0 = -N0;  // seen from behind: the same glass
    // The bevel: within a few px of the edge the surface rounds down (towards the outside: f falls).
    const float bw = (3.0f + 6.0f * U.thick) * s;
    const float b = 1.0f - smoothstep(0.0f, bw, f);
    const float2 gf = float2(dfdx(f), dfdy(f));
    const float2 n2 = length(gf) > 1e-5f ? normalize(gf) : float2(0.0f);
    const float3 N = normalize(N0 + float3(n2 * 0.9f * b * b, 0.0f));
    // The slab shifts what lies behind it sideways as it tilts (thickness T), the bevel bends and splits it.
    const float T = 0.018f * U.H * U.thick;
    const float ci = clamp(N0.z, 0.05f, 1.0f), si = sqrt(1.0f - ci * ci);
    const float st = si / 1.52f, ct = sqrt(1.0f - st * st);
    const float2 dir = length(N0.xy) > 1e-5f ? normalize(N0.xy) : float2(0.0f);
    const float2 shift = -dir * T * (si * ct - ci * st) / ct;
    const float2 px = in.position.xy;
    float3 col = through(frame, px, U.size, N, N0, U.depthPx, shift);
    // Fresnel: a reflection of the room (the frame, blurred and mirrored) that flashes as the shard turns edge-on. Only
    // what the turn adds over flat glass, as for the pane: at the moment of breaking the shards are the pane.
    const float3 R = reflect(float3(0.0f, 0.0f, -1.0f), N);
    const float F = 0.96f * pow(1.0f - saturate(N.z), 5.0f);
    constexpr sampler lin(filter::linear, mip_filter::linear, address::clamp_to_edge);
    const float3 env = frame.sample(lin, px / U.size + R.xy * 0.35f, level(4.5f)).rgb * 0.55f + float3(0.06f, 0.07f, 0.08f);
    const float rl = saturate(dot(R, normalize(kLight)));
    const float slope = length(N.xy) / max(N.z, 0.05f);
    const float hl = U.spec * (3.0f * pow(rl, 150.0f) + 0.35f * pow(rl, 14.0f)) * smoothstep(0.02f, 0.12f, slope);
    // The green of the glass: at the edges, and through more of it as the shard turns.
    const float3 tint = mix(float3(1.0f), float3(0.9f, 0.98f, 0.94f), saturate(b + 2.0f * (1.0f - N0.z)));
    col = col * tint * (1.0f - F) + F * env + hl;
    // The edges catch the light (and the shatter's flash).
    const float edge = 1.0f - smoothstep(0.0f, 1.6f * s, f);
    col += (0.16f + 0.6f * I.flash) * edge * float3(0.9f, 1.0f, 0.95f);
    const float a = cov * I.alpha * U.opacity;
    return float4(saturate(col) * a, a);
}
)MSL";

struct PaneU {
    float size[2];
    float H, depthPx;
    float offsetPx, opacity, sheen, spec;
    float line, sparkle, beat, build;
    int32_t line16, opaque, count;
    float s;
    float thick, pad0, pad1, pad2;
    float imp[6][4];
};
static_assert(sizeof(PaneU) == 176, "PaneU must match the MSL struct");

struct ShardI {
    float home[2], pos[2];
    float box[4];
    float r0[4], r1[4], r2[4];
    float z, flash, alpha;
    int32_t slot;
};
static_assert(sizeof(ShardI) == 96, "ShardI must match the MSL struct");

constexpr std::size_t kElemBytes = std::size_t(sh::kElemIds) * 8;
constexpr std::size_t kTiltBytes = std::size_t(sh::kSlots) * 8;
constexpr std::size_t align256(std::size_t n) { return (n + 255) & ~std::size_t(255); }
constexpr std::size_t kTiltOff = align256(kElemBytes);
constexpr std::size_t kInstOff = align256(kTiltOff + kTiltBytes);
constexpr std::size_t kRingBytes = align256(kInstOff + std::size_t(sh::kMaxShards) * sizeof(ShardI));

MTL::RenderPipelineState* makePso(MTL::Device* dev, MTL::Library* lib, const char* vs, const char* fs,
                                  MTL::PixelFormat fmt, bool blend, bool alpha, const char* label) {
    MTL::Function* v = loadFunction(lib, vs);
    MTL::Function* f = loadFunction(lib, fs);
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

ShatterPipeline::ShatterPipeline(MTL::Device* device, uint32_t width, uint32_t height)
    : device_(device->retain()), width_(std::max(1u, width)), height_(std::max(1u, height)) {
    const std::string head = std::string(kHeader) + kShapeSource;
    // (Fast math: the map still agrees with the engine's C++ away from the cracks themselves; tested.)
    MTL::Library* mapLib = compileLibrary(device_, head + kMapShaders, "reforce.shatter.map", true);
    mapPso_ = makePso(device_, mapLib, "sh_full_vs", "sh_map_fs", MTL::PixelFormatRGBA16Uint, false, true, "shatter map");
    mapLib->release();
    MTL::Library* lib = compileLibrary(device_, std::string(kHeader) + kDrawShaders, "reforce.shatter");
    const auto bgra = MTL::PixelFormatBGRA8Unorm;
    for (int k = 0; k < 2; ++k) {
        panePso_[k] = makePso(device_, lib, "sh_full_vs", "sh_pane_fs", bgra, false, k == 1, "shatter pane");
        shardPso_[k] = makePso(device_, lib, "sh_shard_vs", "sh_shard_fs", bgra, true, k == 1, "shatter shards");
    }
    lib->release();
    auto tex = [&](MTL::PixelFormat fmt, bool mips, const char* label) {
        MTL::TextureDescriptor* td = MTL::TextureDescriptor::texture2DDescriptor(fmt, width_, height_, mips);
        td->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
        td->setStorageMode(MTL::StorageModePrivate);
        MTL::Texture* t = makeTexture(device_, td);
        if (!t) throw std::runtime_error(std::string(label) + " allocation failed");
        t->setLabel(NS::String::string(label, NS::UTF8StringEncoding));
        return t;
    };
    copy_ = tex(bgra, true, "shatter frame copy");
    map_[0] = tex(MTL::PixelFormatRGBA16Uint, false, "shatter crack map");
    map_[1] = tex(MTL::PixelFormatRGBA16Uint, false, "shatter crack map 2");
    for (auto& b : ring_) {
        b = makeBuffer(device_, NS::UInteger(kRingBytes), MTL::ResourceStorageModeShared);
        if (!b) throw std::runtime_error("shatter state buffer allocation failed");
        b->setLabel(NS::String::string("shatter state", NS::UTF8StringEncoding));
    }
}

ShatterPipeline::~ShatterPipeline() {
    for (auto* p : {mapPso_, panePso_[0], panePso_[1], shardPso_[0], shardPso_[1]})
        if (p) p->release();
    for (auto* t : {copy_, map_[0], map_[1]})
        if (t) t->release();
    for (auto* b : ring_)
        if (b) b->release();
    device_->release();
}

MTL::Texture* ShatterPipeline::crackMap(uint64_t planKey) const {
    for (int i = 0; i < 2; ++i)
        if (mapKey_[i] == planKey && planKey != 0) return map_[i];
    return nullptr;
}

// The crack map of plan p (built when it is not resident; the least recently used one is replaced, never keepKey's).
int ShatterPipeline::mapFor(MTL::CommandBuffer* cmd, const sh::Plan& p, uint64_t keepKey) {
    for (int i = 0; i < 2; ++i)
        if (mapKey_[i] == p.key) {
            mapUse_[i] = ++uses_;
            return i;
        }
    int slot = mapKey_[0] == 0 ? 0 : mapKey_[1] == 0 ? 1 : mapUse_[0] <= mapUse_[1] ? 0 : 1;
    if (mapKey_[slot] == keepKey && keepKey != 0) slot = 1 - slot;
    MTL::RenderCommandEncoder* enc = pass(cmd, map_[slot], MTL::LoadActionDontCare, "shatter map");
    enc->setRenderPipelineState(mapPso_);
    enc->setFragmentBytes(&p.pane, sizeof(p.pane), 0);
    const float H = float(height_);
    enc->setFragmentBytes(&H, sizeof(H), 1);
    enc->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(3));
    enc->endEncoding();
    mapKey_[slot] = p.key;
    mapUse_[slot] = ++uses_;
    ++mapBuilds_;
    return slot;
}

void ShatterPipeline::encode(MTL::CommandBuffer* cmd, MTL::Texture* output, const sh::Frame& f, const sh::Look& look,
                             bool trackAlpha) {
    if (!output || f.empty() || !(look.opacity > 0)) return;
    if (output->width() != width_ || output->height() != height_) return;
    const float W = float(width_), H = float(height_);
    auto fin = [](float v, float lo, float hi, float fb) { return std::isfinite(v) ? std::clamp(v, lo, hi) : fb; };
    const bool pane = f.pane != nullptr && f.paneAlpha > 0;
    const bool shards = f.fallen != nullptr && f.loose > 0;
    if (!pane && !shards) return;
    // 1. The crack maps (the pane's and the falling shards').
    int paneMap = -1, shardMap = -1;
    if (pane) paneMap = mapFor(cmd, *f.pane, shards ? f.fallen->key : 0);
    if (shards) shardMap = mapFor(cmd, *f.fallen, pane ? f.pane->key : 0);
    // 2. The frame, with mips.
    {
        MTL::BlitCommandEncoder* blit = cmd->blitCommandEncoder();
        blit->copyFromTexture(output, 0, 0, copy_, 0, 0, 1, 1);
        blit->generateMipmaps(copy_);
        blit->endEncoding();
    }
    // The per-frame state.
    MTL::Buffer* buf = ring_[ringNext_];
    ringNext_ = (ringNext_ + 1) % 8;
    auto* base = static_cast<std::uint8_t*>(buf->contents());
    PaneU u{};
    u.size[0] = W;
    u.size[1] = H;
    u.H = H;
    u.s = H / 1080.0f;
    u.depthPx = fin(look.depth, 0.0f, 4.0f, 0.35f) * H;
    u.opacity = fin(look.opacity, 0.0f, 1.0f, 0.0f);
    u.spec = fin(look.spec, 0.0f, 8.0f, 1.0f);
    u.line = fin(look.line, 0.0f, 4.0f, 1.0f);
    u.sparkle = fin(look.sparkle, 0.0f, 8.0f, 1.0f);
    u.thick = fin(look.thick, 0.0f, 4.0f, 1.0f);
    u.beat = float(std::fmod(f.beat, 4096.0));
    u.build = fin(f.build, 0.0f, 1.0f, 0.0f);
    u.line16 = int32_t(f.line16 & 0x7fffffff);
    u.opaque = trackAlpha && look.opaque ? 1 : 0;
    const int k = trackAlpha && look.opaque ? 1 : 0;
    if (pane) {
        auto* el = reinterpret_cast<float*>(base);
        for (int i = 0; i < sh::kElemIds; ++i) {
            el[2 * i] = f.reveal[std::size_t(i)];
            el[2 * i + 1] = f.glow[std::size_t(i)];
        }
        std::memcpy(base + kTiltOff, f.tilt.data(), kTiltBytes);
        u.offsetPx = fin(f.offset, -1.0f, 0.0f, 0.0f) * H;
        u.sheen = fin(look.sheen, 0.0f, 8.0f, 1.0f) * fin(f.sheen, 0.0f, 4.0f, 1.0f);
        u.count = std::min(f.pane->pane.count, 6);
        for (int i = 0; i < u.count; ++i) {
            const sh::Impact& im = f.pane->pane.imp[i];
            u.imp[i][0] = im.cx * H;
            u.imp[i][1] = im.cy * H;
            u.imp[i][2] = im.r0 * H;
            u.imp[i][3] = fin(f.impactAge[std::size_t(i)], -1.0f, 1e6f, -1.0f);
        }
    }
    MTL::RenderCommandEncoder* enc = pass(cmd, output, MTL::LoadActionLoad, "shatter");
    if (pane) {
        PaneU pu = u;
        pu.opacity *= fin(f.paneAlpha, 0.0f, 1.0f, 1.0f);
        enc->setRenderPipelineState(panePso_[k]);
        enc->setFragmentBytes(&pu, sizeof(pu), 0);
        enc->setFragmentBuffer(buf, 0, 1);
        enc->setFragmentBuffer(buf, kTiltOff, 2);
        enc->setFragmentTexture(copy_, 0);
        enc->setFragmentTexture(map_[paneMap], 1);
        enc->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(3));
    }
    if (shards) {
        auto* inst = reinterpret_cast<ShardI*>(base + kInstOff);
        const sh::Plan& pl = *f.fallen;
        int n = 0;
        for (int i = 0; i < f.loose && n < sh::kMaxShards; ++i) {
            const sh::ShardPose& p = f.poses[std::size_t(i)];
            if (p.shard < 0 || p.shard >= pl.shardCount) continue;
            const sh::Shard& sd = pl.shards[std::size_t(p.shard)];
            ShardI& d = inst[n++];
            d.home[0] = sd.cx * H;
            d.home[1] = sd.cy * H;
            d.pos[0] = fin(p.x, -8.0f, 8.0f, 0.0f) * H;
            d.pos[1] = fin(p.y, -8.0f, 8.0f, 0.0f) * H;
            d.box[0] = sd.x0 * H;
            d.box[1] = sd.y0 * H;
            d.box[2] = sd.x1 * H;
            d.box[3] = sd.y1 * H;
            for (int c = 0; c < 3; ++c) {
                d.r0[c] = fin(p.rot[std::size_t(c)], -1.0f, 1.0f, c == 0 ? 1.0f : 0.0f);
                d.r1[c] = fin(p.rot[std::size_t(3 + c)], -1.0f, 1.0f, c == 1 ? 1.0f : 0.0f);
                d.r2[c] = fin(p.rot[std::size_t(6 + c)], -1.0f, 1.0f, c == 2 ? 1.0f : 0.0f);
            }
            d.r0[3] = d.r1[3] = d.r2[3] = 0.0f;
            d.z = fin(p.z, -8.0f, 2.0f, 0.0f) * H;
            d.flash = fin(p.flash, 0.0f, 1.0f, 0.0f);
            d.alpha = fin(p.alpha, 0.0f, 1.0f, 1.0f);
            d.slot = sd.slot;
        }
        if (n > 0) {
            enc->setRenderPipelineState(shardPso_[k]);
            enc->setVertexBuffer(buf, kInstOff, 0);
            enc->setVertexBytes(&u, sizeof(u), 1);
            enc->setFragmentBuffer(buf, kInstOff, 0);
            enc->setFragmentBytes(&u, sizeof(u), 1);
            enc->setFragmentTexture(copy_, 0);
            enc->setFragmentTexture(map_[shardMap], 1);
            enc->drawPrimitives(MTL::PrimitiveTypeTriangleStrip, NS::UInteger(0), NS::UInteger(4), NS::UInteger(n));
        }
    }
    enc->endEncoding();
}

}  // namespace reforce::render
