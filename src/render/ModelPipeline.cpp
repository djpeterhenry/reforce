// reforce extension (PLAN 5R.2 / 5R.4, docs/model-design.md): 3D model Objects on the GPU. See ModelPipeline.hpp.
#include "render/ModelPipeline.hpp"

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <stdexcept>

#include "render/ImageFiles.hpp"
#include "render/ShaderCompiler.hpp"

namespace reforce::render {
namespace {

constexpr const char* kModelShaders = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct MV { packed_float3 pos; packed_float3 normal; packed_float4 tangent; packed_float2 uv; };
struct ModelV { float4x4 mvp; float4x4 model; float4 n0; float4 n1; float4 n2; };
struct ModelOut { float4 position [[position]]; float3 wpos; float3 n; float4 t; float2 uv; };

vertex ModelOut model_vs(uint vid [[vertex_id]], const device MV* v [[buffer(0)]], constant ModelV& U [[buffer(1)]]) {
    const MV m = v[vid];
    ModelOut o;
    const float4 p = float4(float3(m.pos), 1.0f);
    o.position = U.mvp * p;
    o.wpos = (U.model * p).xyz;
    const float3 n = float3(m.normal);
    o.n = U.n0.xyz * n.x + U.n1.xyz * n.y + U.n2.xyz * n.z;
    const float4 t = float4(m.tangent);
    o.t = float4((U.model * float4(t.xyz, 0.0f)).xyz, t.w);
    o.uv = float2(m.uv);
    return o;
}

// flags: 1 base texture, 2 normal map, 4 emissive texture, 8 metallic-roughness texture, 16 alpha mask
struct ModelF {
    float4 light;      // world, towards the key light
    float4 fillDir;    // world, towards the fill (below the camera)
    float4 eye;        // world camera position
    float4 baseColor;  // linear factor
    float4 emissive;   // linear factor (rgb)
    float4 tint;       // rgb linear tint color, a = amount
    float fill; float shade; float glow; float exposure;
    float metallic; float roughness; float normalScale; float alphaCutoff;
    int flags; int pad0; int pad1; int pad2;
};

struct Surface { float3 n; float4 base; float3 emissive; float metallic; float roughness; float3 v; };

static Surface surface(ModelOut in, bool front, constant ModelF& F, texture2d<float> baseT, texture2d<float> normalT,
                       texture2d<float> emitT, texture2d<float> mrT, sampler smp) {
    Surface s;
    float3 n = normalize(in.n);
    if (!front) n = -n;
    if (F.flags & 2) {
        float3 tn = normalT.sample(smp, in.uv).xyz * 2.0f - 1.0f;
        tn.xy *= F.normalScale;
        const float3 t0 = in.t.xyz - n * dot(n, in.t.xyz);
        if (dot(t0, t0) > 1e-12f) {
            const float3 t = normalize(t0);
            const float3 b = cross(n, t) * (in.t.w < 0.0f ? -1.0f : 1.0f);
            n = normalize(t * tn.x + b * tn.y + n * max(tn.z, 1e-3f));
        }
    }
    s.n = n;
    s.base = F.baseColor * ((F.flags & 1) ? baseT.sample(smp, in.uv) : float4(1.0f));
    s.emissive = F.emissive.rgb * ((F.flags & 4) ? emitT.sample(smp, in.uv).rgb : float3(1.0f));
    const float2 mr = (F.flags & 8) ? mrT.sample(smp, in.uv).bg : float2(1.0f);
    s.metallic = saturate(F.metallic * mr.x);
    s.roughness = clamp(F.roughness * mr.y, 0.04f, 1.0f);
    s.v = normalize(F.eye.xyz - in.wpos);
    return s;
}

constant float3 kLuma = float3(0.2126f, 0.7152f, 0.0722f);

// Look A: luminance into the solid slot (r = intensity, g = 1: coverage after the resolve).
fragment float4 model_lum_fs(ModelOut in [[stage_in]], bool front [[front_facing]], constant ModelF& F [[buffer(0)]],
                             texture2d<float> baseT [[texture(0)]], texture2d<float> normalT [[texture(1)]],
                             texture2d<float> emitT [[texture(2)]], texture2d<float> mrT [[texture(3)]],
                             sampler smp [[sampler(0)]]) {
    const Surface s = surface(in, front, F, baseT, normalT, emitT, mrT, smp);
    if ((F.flags & 16) && s.base.a < F.alphaCutoff) discard_fragment();
    const float albedo = dot(pow(saturate(s.base.rgb), float3(1.0f / 2.2f)), kLuma);
    const float key = max(0.0f, dot(s.n, F.light.xyz));
    const float rim = pow(1.0f - saturate(dot(s.n, s.v)), 3.0f);
    float lum = F.fill + (1.0f - F.fill) * albedo * mix(1.0f, key + 0.25f * rim, F.shade);
    const float e = pow(saturate(dot(s.emissive, kLuma)), 1.0f / 2.2f);
    lum = max(lum, F.glow * e);
    return float4(saturate(lum), 1.0f, 0.0f, 0.0f);
}

static float3 aces(float3 x) {
    return saturate((x * (2.51f * x + 0.03f)) / (x * (2.43f * x + 0.59f) + 0.14f));
}

struct ColorOut { float4 color [[color(0)]]; float4 emit [[color(1)]]; };

// Look B: color (sRGB-encoded, alpha 1 = coverage after the resolve) and the emissive light for the bloom.
fragment ColorOut model_color_fs(ModelOut in [[stage_in]], bool front [[front_facing]],
                                   constant ModelF& F [[buffer(0)]], texture2d<float> baseT [[texture(0)]],
                                   texture2d<float> normalT [[texture(1)]], texture2d<float> emitT [[texture(2)]],
                                   texture2d<float> mrT [[texture(3)]], sampler smp [[sampler(0)]]) {
    const Surface s = surface(in, front, F, baseT, normalT, emitT, mrT, smp);
    if ((F.flags & 16) && s.base.a < F.alphaCutoff) discard_fragment();
    float3 albedo = s.base.rgb;
    if (F.tint.a > 0.0f) {
        const float l = dot(albedo, kLuma);
        const float3 tinted = F.tint.rgb * (l / max(dot(F.tint.rgb, kLuma), 0.05f));
        albedo = mix(albedo, tinted, F.tint.a);
    }
    const float3 n = s.n, v = s.v, l = F.light.xyz;
    const float ndl = max(0.0f, dot(n, l));
    const float3 keyC = float3(1.0f, 0.95f, 0.88f) * 2.4f;
    const float3 fillC = float3(0.45f, 0.55f, 0.75f) * 0.35f;
    const float3 rimC = float3(0.8f, 0.85f, 1.0f) * 0.5f;
    const float3 ambient = float3(0.06f);
    const float fillL = max(0.0f, dot(n, F.fillDir.xyz));
    const float3 diffuse = albedo * (1.0f - s.metallic) * (keyC * ndl + fillC * fillL + ambient);
    const float3 h = normalize(l + v);
    const float r4 = pow(s.roughness, 4.0f);
    const float shin = clamp(2.0f / max(r4, 1e-4f) - 2.0f, 1.0f, 2048.0f);
    const float3 f0 = mix(float3(0.04f), albedo, s.metallic);
    const float3 spec = f0 * pow(max(dot(n, h), 0.0f), shin) * ((shin + 8.0f) / 25.13f) * ndl * keyC;
    const float rim = pow(1.0f - saturate(dot(n, v)), 3.0f);
    const float3 emit = s.emissive * F.glow;
    const float3 lit = (diffuse + spec + rimC * rim * (0.3f + 0.7f * dot(albedo, kLuma))) + emit;
    ColorOut o;
    o.color = float4(pow(aces(lit * F.exposure), float3(1.0f / 2.2f)), 1.0f);
    o.emit = float4(pow(aces(emit * F.exposure), float3(1.0f / 2.2f)), 1.0f);
    return o;
}

struct FullOut { float4 position [[position]]; float2 uv; };
vertex FullOut model_full_vs(uint vid [[vertex_id]]) {
    const float2 p = float2((vid << 1) & 2, vid & 2);
    FullOut o;
    o.position = float4(p * 2.0f - 1.0f, 0.0f, 1.0f);
    o.uv = float2(p.x, 1.0f - p.y);
    return o;
}
fragment float4 model_down_fs(FullOut in [[stage_in]], texture2d<float> src [[texture(0)]],
                              constant float2& texel [[buffer(0)]]) {
    constexpr sampler smp(filter::linear, address::clamp_to_edge);
    const float2 d = texel * 0.5f;
    return 0.25f * (src.sample(smp, in.uv + float2(-d.x, -d.y)) + src.sample(smp, in.uv + float2(d.x, -d.y)) +
                    src.sample(smp, in.uv + float2(-d.x, d.y)) + src.sample(smp, in.uv + float2(d.x, d.y)));
}
struct BlurP { float2 dir; float sigma; float step; };
fragment float4 model_blur_fs(FullOut in [[stage_in]], texture2d<float> src [[texture(0)]],
                              constant BlurP& B [[buffer(0)]]) {
    constexpr sampler smp(filter::linear, address::clamp_to_edge);
    float4 acc = 0.0f;
    float wsum = 0.0f;
    for (int i = -6; i <= 6; ++i) {
        const float x = float(i) * B.step;
        const float w = exp(-0.5f * x * x / (B.sigma * B.sigma));
        acc += w * src.sample(smp, in.uv + B.dir * x);
        wsum += w;
    }
    return acc / wsum;
}
// The resolved model (premultiplied by coverage) plus the bloom, over the frame (blend One, OneMinusSourceAlpha).
struct CompP { float opacity; float bloom; float pad0; float pad1; };
fragment float4 model_composite_fs(FullOut in [[stage_in]], texture2d<float> src [[texture(0)]],
                                   texture2d<float> bloomT [[texture(1)]], constant CompP& P [[buffer(0)]]) {
    constexpr sampler smp(filter::linear, address::clamp_to_edge);
    const float4 c = src.sample(smp, in.uv);
    const float3 b = bloomT.sample(smp, in.uv).rgb * P.bloom;
    return float4((c.rgb + b) * P.opacity, c.a * P.opacity);
}
)MSL";

struct ModelV {
    float mvp[16], model[16];
    float n0[4], n1[4], n2[4];
};
static_assert(sizeof(ModelV) == 176, "ModelV must match the MSL struct");
struct ModelF {
    float light[4], fillDir[4], eye[4], baseColor[4], emissive[4], tint[4];
    float fill, shade, glow, exposure;
    float metallic, roughness, normalScale, alphaCutoff;
    int32_t flags, pad0, pad1, pad2;
};
static_assert(sizeof(ModelF) == 144, "ModelF must match the MSL struct");
static_assert(offsetof(engine::ModelVertex, normal) == 12 && offsetof(engine::ModelVertex, tangent) == 24 &&
              offsetof(engine::ModelVertex, uv) == 40);

using V3 = std::array<double, 3>;
V3 normalize3(const V3& v, const V3& fb) {
    const double l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    return l > 1e-12 ? V3{v[0] / l, v[1] / l, v[2] / l} : fb;
}

engine::Mat4 rotY(double a) {
    engine::Mat4 m = engine::Mat4::identity();
    const float c = float(std::cos(a)), s = float(std::sin(a));
    m.m[0] = c;
    m.m[2] = -s;
    m.m[8] = s;
    m.m[10] = c;
    return m;
}
engine::Mat4 rotZ(double a) {
    engine::Mat4 m = engine::Mat4::identity();
    const float c = float(std::cos(a)), s = float(std::sin(a));
    m.m[0] = c;
    m.m[1] = s;
    m.m[4] = -s;
    m.m[5] = c;
    return m;
}

// Normal matrix (inverse transpose of the upper 3x3) columns.
void normalColumns(const engine::Mat4& m, float n[3][4]) {
    const double a[3][3] = {{m.m[0], m.m[4], m.m[8]}, {m.m[1], m.m[5], m.m[9]}, {m.m[2], m.m[6], m.m[10]}};  // [row][col]
    const double det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) -
                       a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
                       a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
    const double id = std::fabs(det) > 1e-20 ? 1.0 / det : 0.0;
    // inverse[row][col] = cofactor[col][row] / det; normal matrix = transpose(inverse), so nm[row][col] = cof[row][col]/det
    double cof[3][3];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) {
            const int r1 = (r + 1) % 3, r2 = (r + 2) % 3, c1 = (c + 1) % 3, c2 = (c + 2) % 3;
            cof[r][c] = a[r1][c1] * a[r2][c2] - a[r1][c2] * a[r2][c1];
        }
    for (int c = 0; c < 3; ++c) {
        for (int r = 0; r < 3; ++r) n[c][r] = float(cof[r][c] * id);
        n[c][3] = 0;
    }
}

MTL::RenderPipelineState* makePso(MTL::Device* dev, MTL::Library* lib, const char* vs, const char* fs,
                                  const std::vector<MTL::PixelFormat>& colors, bool depth, int samples, int blend,
                                  const char* label) {
    MTL::Function* v = loadFunction(lib, vs);
    MTL::Function* f = loadFunction(lib, fs);
    MTL::RenderPipelineDescriptor* d = MTL::RenderPipelineDescriptor::alloc()->init();
    d->setLabel(NS::String::string(label, NS::UTF8StringEncoding));
    d->setVertexFunction(v);
    d->setFragmentFunction(f);
    for (std::size_t i = 0; i < colors.size(); ++i) {
        auto* a = d->colorAttachments()->object(NS::UInteger(i));
        a->setPixelFormat(colors[i]);
        if (blend && i == 0) {
            a->setBlendingEnabled(true);
            a->setRgbBlendOperation(MTL::BlendOperationAdd);
            a->setSourceRGBBlendFactor(MTL::BlendFactorOne);
            a->setDestinationRGBBlendFactor(MTL::BlendFactorOneMinusSourceAlpha);
            a->setAlphaBlendOperation(MTL::BlendOperationAdd);
            a->setSourceAlphaBlendFactor(MTL::BlendFactorOne);
            a->setDestinationAlphaBlendFactor(MTL::BlendFactorOneMinusSourceAlpha);
            if (blend == 1) a->setWriteMask(MTL::ColorWriteMaskRed | MTL::ColorWriteMaskGreen | MTL::ColorWriteMaskBlue);
        }
    }
    if (depth) d->setDepthAttachmentPixelFormat(MTL::PixelFormatDepth32Float);
    d->setRasterSampleCount(NS::UInteger(samples));
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

MTL::Texture* solidTexture(MTL::Device* dev, MTL::PixelFormat fmt, const std::uint8_t rgba[4]) {
    MTL::TextureDescriptor* td = MTL::TextureDescriptor::texture2DDescriptor(fmt, 1, 1, false);
    td->setUsage(MTL::TextureUsageShaderRead);
    td->setStorageMode(MTL::StorageModeShared);
    MTL::Texture* t = dev->newTexture(td);
    if (!t) throw std::runtime_error("model default texture allocation failed");
    t->replaceRegion(MTL::Region(0, 0, 1, 1), 0, rgba, 4);
    return t;
}

// Halves an RGBA8 image with a 2x2 box (odd edges: the last row / column repeats).
void halve(ImageRgba8& img) {
    const int w = std::max(1, img.width / 2), h = std::max(1, img.height / 2);
    std::vector<std::uint8_t> out(std::size_t(w) * std::size_t(h) * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < 4; ++c) {
                int sum = 0;
                for (int dy = 0; dy < 2; ++dy)
                    for (int dx = 0; dx < 2; ++dx) {
                        const int sx = std::min(img.width - 1, 2 * x + dx), sy = std::min(img.height - 1, 2 * y + dy);
                        sum += img.rgba[(std::size_t(sy) * std::size_t(img.width) + std::size_t(sx)) * 4 + std::size_t(c)];
                    }
                out[(std::size_t(y) * std::size_t(w) + std::size_t(x)) * 4 + std::size_t(c)] = std::uint8_t((sum + 2) / 4);
            }
    img.width = w;
    img.height = h;
    img.rgba = std::move(out);
}

}  // namespace

ModelGpu::~ModelGpu() {
    if (vertices_) vertices_->release();
    if (indices_) indices_->release();
    for (auto* t : textures_)
        if (t) t->release();
}

ModelPipeline::ModelPipeline(SolidPipeline& solids)
    : solids_(solids), width_(0), height_(0), msaa_(solids.msaa()) {
    if (solids.maxLive() < 1) throw std::runtime_error("no solid slots");
    MTL::Texture* t0 = solids.target(0);
    device_ = t0->device()->retain();
    width_ = uint32_t(t0->width());
    height_ = uint32_t(t0->height());
    uploadQueue_ = device_->newCommandQueue();
    if (!uploadQueue_) throw std::runtime_error("model upload queue failed");
    uploadQueue_->setLabel(NS::String::string("model upload", NS::UTF8StringEncoding));

    MTL::Library* lib = compileLibrary(device_, kModelShaders, "reforce.model");
    lumPso_ = makePso(device_, lib, "model_vs", "model_lum_fs", {MTL::PixelFormatRGBA8Unorm}, true, msaa_, 0,
                      "model luminance");
    colorPso_ = makePso(device_, lib, "model_vs", "model_color_fs",
                         {MTL::PixelFormatRGBA8Unorm, MTL::PixelFormatRGBA8Unorm}, true, msaa_, 0, "model color");
    downPso_ = makePso(device_, lib, "model_full_vs", "model_down_fs", {MTL::PixelFormatRGBA8Unorm}, false, 1, 0,
                       "model bloom downsample");
    blurPso_ = makePso(device_, lib, "model_full_vs", "model_blur_fs", {MTL::PixelFormatRGBA8Unorm}, false, 1, 0,
                       "model bloom blur");
    compositePso_ = makePso(device_, lib, "model_full_vs", "model_composite_fs", {MTL::PixelFormatBGRA8Unorm}, false,
                            1, 1, "model composite");
    compositeOpaquePso_ = makePso(device_, lib, "model_full_vs", "model_composite_fs", {MTL::PixelFormatBGRA8Unorm},
                                  false, 1, 2, "model composite (opaque alpha)");
    lib->release();

    MTL::DepthStencilDescriptor* dd = MTL::DepthStencilDescriptor::alloc()->init();
    dd->setDepthCompareFunction(MTL::CompareFunctionLess);
    dd->setDepthWriteEnabled(true);
    depth_ = device_->newDepthStencilState(dd);
    dd->release();

    MTL::SamplerDescriptor* sd = MTL::SamplerDescriptor::alloc()->init();
    sd->setMinFilter(MTL::SamplerMinMagFilterLinear);
    sd->setMagFilter(MTL::SamplerMinMagFilterLinear);
    sd->setMipFilter(MTL::SamplerMipFilterLinear);
    sd->setSAddressMode(MTL::SamplerAddressModeRepeat);
    sd->setTAddressMode(MTL::SamplerAddressModeRepeat);
    sd->setMaxAnisotropy(8);
    sampler_ = device_->newSamplerState(sd);
    sd->release();

    const std::uint8_t white[4] = {255, 255, 255, 255}, flat[4] = {128, 128, 255, 255}, black[4] = {0, 0, 0, 255};
    white_ = solidTexture(device_, MTL::PixelFormatRGBA8Unorm, white);
    flatNormal_ = solidTexture(device_, MTL::PixelFormatRGBA8Unorm, flat);
    black_ = solidTexture(device_, MTL::PixelFormatRGBA8Unorm, black);

    const bool memoryless = device_->supportsFamily(MTL::GPUFamilyApple7);
    auto tex = [&](NS::UInteger w, NS::UInteger h, bool msaa, bool scratch, const char* label) {
        MTL::TextureDescriptor* td = MTL::TextureDescriptor::texture2DDescriptor(MTL::PixelFormatRGBA8Unorm, w, h, false);
        td->setUsage(scratch ? MTL::TextureUsageRenderTarget : MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
        td->setStorageMode(scratch && memoryless ? MTL::StorageModeMemoryless : MTL::StorageModePrivate);
        if (msaa) {
            td->setTextureType(MTL::TextureType2DMultisample);
            td->setSampleCount(NS::UInteger(msaa_));
        }
        MTL::Texture* t = device_->newTexture(td);
        if (!t) throw std::runtime_error(std::string(label) + " allocation failed");
        t->setLabel(NS::String::string(label, NS::UTF8StringEncoding));
        return t;
    };
    if (msaa_ > 1) emitMsaa_ = tex(width_, height_, true, true, "model emissive MSAA");
    emit_ = tex(width_, height_, false, false, "model emissive");
    const NS::UInteger hw = std::max<NS::UInteger>(1, width_ / 2), hh = std::max<NS::UInteger>(1, height_ / 2);
    halfA_ = tex(hw, hh, false, false, "model bloom A");
    halfB_ = tex(hw, hh, false, false, "model bloom B");
}

ModelPipeline::~ModelPipeline() {
    for (auto* p : {lumPso_, colorPso_, downPso_, blurPso_, compositePso_, compositeOpaquePso_})
        if (p) p->release();
    for (auto* t : {white_, flatNormal_, black_, emitMsaa_, emit_, halfA_, halfB_})
        if (t) t->release();
    if (depth_) depth_->release();
    if (sampler_) sampler_->release();
    if (uploadQueue_) uploadQueue_->release();
    device_->release();
}

std::shared_ptr<const ModelGpu> ModelPipeline::upload(const engine::Model& model, int maxTexture,
                                                      std::string* error) const {
    auto fail = [&](const std::string& e) {
        if (error) *error = e;
        return std::shared_ptr<const ModelGpu>();
    };
    if (model.vertices.empty() || model.indices.empty()) return fail("empty model");
    NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
    std::shared_ptr<ModelGpu> g(new ModelGpu());
    g->vertices_ = device_->newBuffer(model.vertices.data(),
                                      NS::UInteger(model.vertices.size() * sizeof(engine::ModelVertex)),
                                      MTL::ResourceStorageModeShared);
    g->indices_ = device_->newBuffer(model.indices.data(), NS::UInteger(model.indices.size() * sizeof(std::uint32_t)),
                                     MTL::ResourceStorageModeShared);
    if (!g->vertices_ || !g->indices_) {
        pool->release();
        return fail("model buffer allocation failed");
    }
    g->triangles_ = model.triangleCount();
    g->bmin_ = model.boundsMin;
    g->bmax_ = model.boundsMax;
    g->warnings_ = model.warnings;
    maxTexture = std::clamp(maxTexture, 16, 16384);

    MTL::CommandBuffer* cmd = uploadQueue_->commandBuffer();
    MTL::BlitCommandEncoder* blit = cmd->blitCommandEncoder();
    for (const engine::ModelTexture& mt : model.textures) {
        ImageRgba8 img;
        const bool ok = !mt.bytes.empty() ? decodeImageRgba8(mt.bytes.data(), mt.bytes.size(), img, true)
                                          : loadImageRgba8(mt.path.string(), img, true);
        if (!ok || img.width <= 0 || img.height <= 0) {
            g->warnings_.push_back("texture could not be decoded: " + (mt.name.empty() ? mt.path.string() : mt.name));
            g->textures_.push_back(nullptr);
            continue;
        }
        while (img.width > maxTexture || img.height > maxTexture) halve(img);
        const int levels = 1 + int(std::floor(std::log2(double(std::max(img.width, img.height)))));
        MTL::TextureDescriptor* td = MTL::TextureDescriptor::texture2DDescriptor(
            mt.srgb ? MTL::PixelFormatRGBA8Unorm_sRGB : MTL::PixelFormatRGBA8Unorm, NS::UInteger(img.width),
            NS::UInteger(img.height), true);
        td->setMipmapLevelCount(NS::UInteger(levels));
        td->setUsage(MTL::TextureUsageShaderRead);
        td->setStorageMode(MTL::StorageModeShared);
        MTL::Texture* t = device_->newTexture(td);
        if (!t) {
            g->warnings_.push_back("texture allocation failed: " + mt.name);
            g->textures_.push_back(nullptr);
            continue;
        }
        t->replaceRegion(MTL::Region(0, 0, NS::UInteger(img.width), NS::UInteger(img.height)), 0, img.rgba.data(),
                         NS::UInteger(img.width) * 4);
        if (levels > 1) blit->generateMipmaps(t);
        g->textureBytes_ += std::size_t(img.width) * std::size_t(img.height) * 4 * 4 / 3;
        g->textures_.push_back(t);
    }
    blit->endEncoding();
    cmd->commit();
    cmd->waitUntilCompleted();

    for (const engine::ModelMaterial& m : model.materials) {
        ModelGpu::Material o;
        std::copy(m.baseColor.begin(), m.baseColor.end(), o.baseColor);
        std::copy(m.emissive.begin(), m.emissive.end(), o.emissive);
        o.metallic = m.metallic;
        o.roughness = m.roughness;
        o.normalScale = m.normalScale;
        o.alphaCutoff = m.alphaCutoff;
        o.mask = m.alpha == engine::ModelAlpha::Mask;
        o.doubleSided = m.doubleSided;
        auto valid = [&](int i) { return i >= 0 && i < int(g->textures_.size()) && g->textures_[std::size_t(i)] ? i : -1; };
        o.base = valid(m.baseColorTex);
        o.normal = valid(m.normalTex);
        o.emission = valid(m.emissiveTex);
        o.mr = valid(m.metallicRoughnessTex);
        g->materials_.push_back(o);
    }
    g->emissive_ = model.anyEmissive();
    for (const engine::ModelPrimitive& p : model.primitives) g->primitives_.push_back({p.firstIndex, p.indexCount, p.material});
    pool->release();
    return g;
}

engine::Mat4 ModelPipeline::modelMatrix(const ModelDraw& d) {
    return rotY(d.spin) * rotZ(d.bank);
}

engine::Mat4 ModelPipeline::viewProjection(const ModelDraw& d) const {
    return engine::shotProjection(d.shot, double(width_) / double(height_)) * engine::shotView(d.shot);
}

void ModelPipeline::drawModel(MTL::CommandBuffer* cmd, const ModelDraw& d, bool color, int px0, int py0, int bw,
                              int bh, const char* label) {
    const ModelGpu& g = *d.model;
    MTL::Texture* target = solids_.target(d.slot);
    MTL::RenderPassDescriptor* pass = MTL::RenderPassDescriptor::renderPassDescriptor();
    auto attach = [&](int i, MTL::Texture* msaaTex, MTL::Texture* resolve) {
        auto* a = pass->colorAttachments()->object(NS::UInteger(i));
        a->setLoadAction(MTL::LoadActionClear);
        a->setClearColor(MTL::ClearColor(0, 0, 0, 0));
        if (msaa_ > 1) {
            a->setTexture(msaaTex);
            a->setResolveTexture(resolve);
            a->setStoreAction(MTL::StoreActionMultisampleResolve);
        } else {
            a->setTexture(resolve);
            a->setStoreAction(MTL::StoreActionStore);
        }
    };
    attach(0, solids_.msaaColorTexture(), target);
    if (color) attach(1, emitMsaa_, emit_);
    auto* da = pass->depthAttachment();
    da->setTexture(solids_.depthTexture());
    da->setLoadAction(MTL::LoadActionClear);
    da->setClearDepth(1.0);
    da->setStoreAction(MTL::StoreActionDontCare);
    pass->setRenderTargetWidth(NS::UInteger(bw));
    pass->setRenderTargetHeight(NS::UInteger(bh));
    MTL::RenderCommandEncoder* enc = cmd->renderCommandEncoder(pass);
    enc->setLabel(NS::String::string(label, NS::UTF8StringEncoding));
    const int W = int(width_), H = int(height_);
    enc->setViewport(MTL::Viewport{0.0, 0.0, double(W), double(H), 0.0, 1.0});
    enc->setScissorRect(MTL::ScissorRect{0, 0, NS::UInteger(bw), NS::UInteger(bh)});
    enc->setRenderPipelineState(color ? colorPso_ : lumPso_);
    enc->setDepthStencilState(depth_);
    enc->setFrontFacingWinding(MTL::WindingCounterClockwise);

    // Shift clip space so the box's top-left pixel lands on the target's (0, 0) (as SolidPipeline::encode).
    engine::Mat4 adj = engine::Mat4::identity();
    adj.m[12] = -2.0f * float(px0) / float(W);
    adj.m[13] = 2.0f * float(py0) / float(H);
    const engine::Mat4 model = modelMatrix(d);
    const engine::Mat4 mvp = adj * viewProjection(d) * model;
    ModelV v{};
    std::copy(std::begin(mvp.m), std::end(mvp.m), v.mvp);
    std::copy(std::begin(model.m), std::end(model.m), v.model);
    float nm[3][4];
    normalColumns(model, nm);
    std::copy(nm[0], nm[0] + 4, v.n0);
    std::copy(nm[1], nm[1] + 4, v.n1);
    std::copy(nm[2], nm[2] + 4, v.n2);
    enc->setVertexBuffer(g.vertices_, 0, 0);
    enc->setVertexBytes(&v, sizeof(v), 1);
    enc->setFragmentSamplerState(sampler_, 0);

    // Camera-space light directions to world: the view's rows are the camera axes.
    const engine::Mat4 view = engine::shotView(d.shot);
    auto toWorld = [&](const V3& c) {
        const V3 n = normalize3(c, {0, 0, 1});
        V3 w{};
        for (int k = 0; k < 3; ++k)  // R^T * c: column k of R^T = row k of R
            w[std::size_t(k)] = view.m[k * 4 + 0] * n[0] + view.m[k * 4 + 1] * n[1] + view.m[k * 4 + 2] * n[2];
        return normalize3(w, {0, 1, 0});
    };
    const V3 light = toWorld({d.light[0], d.light[1], d.light[2]});
    const V3 fillDir = toWorld({0.2, -1.0, 0.35});

    ModelF base{};
    base.light[0] = float(light[0]);
    base.light[1] = float(light[1]);
    base.light[2] = float(light[2]);
    base.fillDir[0] = float(fillDir[0]);
    base.fillDir[1] = float(fillDir[1]);
    base.fillDir[2] = float(fillDir[2]);
    for (int k = 0; k < 3; ++k) base.eye[k] = float(d.shot.eye[std::size_t(k)]);
    // The tint color from sRGB to linear.
    for (int k = 0; k < 3; ++k) base.tint[k] = std::pow(std::clamp(d.tintColor[std::size_t(k)], 0.0f, 1.0f), 2.2f);
    base.tint[3] = std::clamp(d.tint, 0.0f, 1.0f);
    base.fill = d.fill;
    base.shade = d.shade;
    base.glow = d.glow;
    base.exposure = d.exposure;

    auto texOr = [&](int i, MTL::Texture* fb) { return i >= 0 ? g.textures_[std::size_t(i)] : fb; };
    int lastCull = -1;
    for (const auto& p : g.primitives_) {
        ModelF f = base;
        const ModelGpu::Material* m =
            p.material >= 0 && p.material < int(g.materials_.size()) ? &g.materials_[std::size_t(p.material)] : nullptr;
        if (m) {
            std::copy(m->baseColor, m->baseColor + 4, f.baseColor);
            std::copy(m->emissive, m->emissive + 3, f.emissive);
            f.metallic = m->metallic;
            f.roughness = m->roughness;
            f.normalScale = m->normalScale;
            f.alphaCutoff = m->alphaCutoff;
            f.flags = (m->base >= 0 ? 1 : 0) | (m->normal >= 0 ? 2 : 0) | (m->emission >= 0 ? 4 : 0) |
                      (m->mr >= 0 ? 8 : 0) | (m->mask ? 16 : 0);
        } else {
            f.baseColor[0] = f.baseColor[1] = f.baseColor[2] = f.baseColor[3] = 1.0f;
            f.metallic = 0.0f;
            f.roughness = 0.6f;
            f.normalScale = 1.0f;
        }
        const int cull = m && m->doubleSided ? 0 : 1;
        if (cull != lastCull) {
            enc->setCullMode(cull ? MTL::CullModeBack : MTL::CullModeNone);
            lastCull = cull;
        }
        enc->setFragmentBytes(&f, sizeof(f), 0);
        enc->setFragmentTexture(texOr(m ? m->base : -1, white_), 0);
        enc->setFragmentTexture(texOr(m ? m->normal : -1, flatNormal_), 1);
        enc->setFragmentTexture(texOr(m ? m->emission : -1, white_), 2);
        enc->setFragmentTexture(texOr(m ? m->mr : -1, white_), 3);
        enc->drawIndexedPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(p.indexCount), MTL::IndexTypeUInt32,
                                   g.indices_, NS::UInteger(p.firstIndex) * sizeof(std::uint32_t));
    }
    enc->endEncoding();
}

SolidQuad ModelPipeline::encodeLuminance(MTL::CommandBuffer* cmd, const ModelDraw& d) {
    SolidQuad q;
    if (!d.model || d.slot < 0 || d.slot >= solids_.maxLive()) return q;
    const ModelGpu& g = *d.model;
    // Screen box: the model's bounding box corners; when any corner is behind the near plane, the whole frame.
    const engine::Mat4 mvp = viewProjection(d) * modelMatrix(d);
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    int behind = 0;
    for (int k = 0; k < 8; ++k) {
        const auto c = engine::transform(mvp, {k & 1 ? g.bmax_[0] : g.bmin_[0], k & 2 ? g.bmax_[1] : g.bmin_[1],
                                               k & 4 ? g.bmax_[2] : g.bmin_[2], 1.0f});
        if (c[3] <= 0.02f) {
            ++behind;
            continue;
        }
        x0 = std::min(x0, c[0] / c[3]);
        x1 = std::max(x1, c[0] / c[3]);
        y0 = std::min(y0, c[1] / c[3]);
        y1 = std::max(y1, c[1] / c[3]);
    }
    if (behind == 8) return q;  // wholly behind the camera
    if (behind > 0) {           // straddling the near plane: the projected corners don't bound it
        x0 = y0 = -1;
        x1 = y1 = 1;
    }
    if (x1 < -1 || x0 > 1 || y1 < -1 || y0 > 1) return q;
    x0 = std::max(x0, -1.0f);
    y0 = std::max(y0, -1.0f);
    x1 = std::min(x1, 1.0f);
    y1 = std::min(y1, 1.0f);
    const int W = int(width_), H = int(height_);
    const int px0 = std::clamp(int(std::floor((x0 + 1.0f) * 0.5f * float(W))) - 1, 0, W);
    const int px1 = std::clamp(int(std::ceil((x1 + 1.0f) * 0.5f * float(W))) + 1, 0, W);
    const int py0 = std::clamp(int(std::floor((1.0f - y1) * 0.5f * float(H))) - 1, 0, H);
    const int py1 = std::clamp(int(std::ceil((1.0f - y0) * 0.5f * float(H))) + 1, 0, H);
    const int bw = px1 - px0, bh = py1 - py0;
    if (bw <= 0 || bh <= 0) return q;
    drawModel(cmd, d, false, px0, py0, bw, bh, "model luminance");
    q.visible = true;
    q.texture = solids_.spriteTexture(d.slot);
    q.cx = float(px0) + float(bw) * 0.5f;
    q.cy = float(py0) + float(bh) * 0.5f;
    q.w = float(bw);
    q.h = float(bh);
    q.u1 = float(bw) / float(W);
    q.v1 = float(bh) / float(H);
    return q;
}

void ModelPipeline::encodeColor(MTL::CommandBuffer* cmd, MTL::Texture* output, const ModelDraw& d, bool trackAlpha) {
    if (!output || !d.model || d.slot < 0 || d.slot >= solids_.maxLive() || !(d.opacity > 0)) return;
    drawModel(cmd, d, true, 0, 0, int(width_), int(height_), "model color");
    const bool bloom = d.model->emissive() && d.bloom > 0;
    auto pass = [&](MTL::Texture* target, MTL::LoadAction load, const char* label) {
        MTL::RenderPassDescriptor* rp = MTL::RenderPassDescriptor::renderPassDescriptor();
        auto* a = rp->colorAttachments()->object(0);
        a->setTexture(target);
        a->setLoadAction(load);
        a->setStoreAction(MTL::StoreActionStore);
        MTL::RenderCommandEncoder* enc = cmd->renderCommandEncoder(rp);
        enc->setLabel(NS::String::string(label, NS::UTF8StringEncoding));
        return enc;
    };
    if (bloom) {
        {
            MTL::RenderCommandEncoder* enc = pass(halfA_, MTL::LoadActionDontCare, "model bloom downsample");
            enc->setRenderPipelineState(downPso_);
            const float texel[2] = {1.0f / float(width_), 1.0f / float(height_)};
            enc->setFragmentTexture(emit_, 0);
            enc->setFragmentBytes(texel, sizeof(texel), 0);
            enc->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(3));
            enc->endEncoding();
        }
        const float hw = float(halfA_->width()), hh = float(halfA_->height());
        const float sigma = std::max(1.0f, 9.0f * float(height_) / 1080.0f);  // half-size texels
        struct {
            float dir[2];
            float sigma, step;
        } bp{};
        bp.sigma = sigma;
        bp.step = std::max(1.0f, sigma / 3.0f);
        for (int axis = 0; axis < 2; ++axis) {
            MTL::RenderCommandEncoder* enc =
                pass(axis == 0 ? halfB_ : halfA_, MTL::LoadActionDontCare, axis == 0 ? "model bloom x" : "model bloom y");
            enc->setRenderPipelineState(blurPso_);
            bp.dir[0] = axis == 0 ? 1.0f / hw : 0.0f;
            bp.dir[1] = axis == 0 ? 0.0f : 1.0f / hh;
            enc->setFragmentTexture(axis == 0 ? halfA_ : halfB_, 0);
            enc->setFragmentBytes(&bp, sizeof(bp), 0);
            enc->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(3));
            enc->endEncoding();
        }
    }
    MTL::RenderCommandEncoder* enc = pass(output, MTL::LoadActionLoad, "model composite");
    enc->setRenderPipelineState(trackAlpha && d.opaque ? compositeOpaquePso_ : compositePso_);
    enc->setFragmentTexture(solids_.target(d.slot), 0);
    enc->setFragmentTexture(bloom ? halfA_ : black_, 1);
    const float cp[4] = {std::clamp(d.opacity, 0.0f, 1.0f), bloom ? d.bloom : 0.0f, 0, 0};
    enc->setFragmentBytes(cp, sizeof(cp), 0);
    enc->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(3));
    enc->endEncoding();
}

}  // namespace reforce::render
