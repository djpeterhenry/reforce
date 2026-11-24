// 3D model Objects (PLAN 5R.2 / 5R.4, docs/model-design.md): render/ModelPipeline on the GPU, offscreen, with the
// generated test model (tests/model_fixture.hpp) and a generated dense mesh for the cost.
//   - look A: coverage, the emissive stripe brighter than the hull, the screen box
//   - look B: the base color lands, the emissive stripe glows, bloom spills past the silhouette only when on
//   - cost at 1920x1080 with ~500k triangles; no render-thread allocations
#include <doctest/doctest.h>

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

#include <cmath>
#include <filesystem>
#include <cstdint>
#include <vector>

#include "PerfBudget.hpp"
#include "engine/CameraMoves.hpp"
#include "engine/FlowFieldBuilder.hpp"
#include "engine/ModelFile.hpp"
#include "engine/Perf.hpp"
#include "model_fixture.hpp"
#include "render/ModelPipeline.hpp"
#include "render/ScenePipeline.hpp"
#include "render/SolidPipeline.hpp"

using namespace reforce;

namespace {

MTL::Device* device() {
    static MTL::Device* d = MTL::CreateSystemDefaultDevice();
    return d;
}

engine::Model testModel(bool emissive = true) {
    testmodel::Options o;
    o.emissive = emissive;
    o.mirrorNode = false;
    o.textureSize = 32;
    const auto glb = testmodel::makeGlb(o);
    std::string error;
    auto m = engine::loadModelFromMemory(glb.bytes.data(), glb.bytes.size(), {}, &error);
    REQUIRE_MESSAGE(m.has_value(), error);
    return std::move(*m);
}

// The box seen from the front three-quarter, filling the middle of the frame.
render::ModelDraw draw(std::shared_ptr<const render::ModelGpu> g) {
    render::ModelDraw d;
    d.model = std::move(g);
    d.slot = 0;
    d.shot.eye = {1.6, 0.9, 1.9};
    d.shot.aim = {0, 0, 0};
    d.shot.fov = 40;
    d.light = {0.0f, 0.3f, 1.0f};  // from the camera: every visible face is lit
    return d;
}

std::vector<std::uint8_t> readTarget(MTL::Texture* t, int w, int h) {
    std::vector<std::uint8_t> px(std::size_t(w * h * 4));
    MTL::CommandQueue* q = device()->newCommandQueue();
    MTL::Buffer* buf = device()->newBuffer(NS::UInteger(w * h * 4), MTL::ResourceStorageModeShared);
    MTL::CommandBuffer* cmd = q->commandBuffer();
    MTL::BlitCommandEncoder* blit = cmd->blitCommandEncoder();
    blit->copyFromTexture(t, 0, 0, MTL::Origin(0, 0, 0), MTL::Size(NS::UInteger(w), NS::UInteger(h), 1), buf, 0,
                          NS::UInteger(w * 4), NS::UInteger(w * h * 4));
    blit->endEncoding();
    cmd->commit();
    cmd->waitUntilCompleted();
    std::memcpy(px.data(), buf->contents(), px.size());
    buf->release();
    q->release();
    return px;
}

// A dense closed mesh (a UV sphere) of about `tris` triangles, no textures: the cost test.
engine::Model denseModel(int tris) {
    engine::Model m;
    const int rings = int(std::sqrt(double(tris) / 2.0));
    const int segs = rings;
    for (int r = 0; r <= rings; ++r)
        for (int s = 0; s <= segs; ++s) {
            const double th = M_PI * r / rings, ph = 2 * M_PI * s / segs;
            engine::ModelVertex v{};
            const float n[3] = {float(std::sin(th) * std::cos(ph)), float(std::cos(th)), float(std::sin(th) * std::sin(ph))};
            for (int k = 0; k < 3; ++k) {
                v.pos[k] = n[k] * 0.98f;
                v.normal[k] = n[k];
            }
            v.tangent[0] = -float(std::sin(ph));
            v.tangent[2] = float(std::cos(ph));
            v.tangent[3] = 1;
            v.uv[0] = float(s) / float(segs);
            v.uv[1] = float(r) / float(rings);
            m.vertices.push_back(v);
        }
    for (int r = 0; r < rings; ++r)
        for (int s = 0; s < segs; ++s) {
            const std::uint32_t a = std::uint32_t(r * (segs + 1) + s), b = a + 1, c = a + std::uint32_t(segs + 1),
                                d = c + 1;
            m.indices.insert(m.indices.end(), {a, c, b, b, c, d});
        }
    engine::ModelMaterial mat;
    mat.baseColor = {0.6f, 0.6f, 0.65f, 1};
    mat.metallic = 0.3f;
    mat.roughness = 0.4f;
    mat.emissive = {0.2f, 0.4f, 1.0f};
    m.materials.push_back(mat);
    m.primitives.push_back({0, std::uint32_t(m.indices.size()), 0});
    m.boundsMin = {-1, -1, -1};
    m.boundsMax = {1, 1, 1};
    m.radius = 1;
    return m;
}

}  // namespace

TEST_CASE("models: look A draws the model as luminance; the emissive stripe is the brightest part") {
    if (!device()) return;
    const int W = 640, H = 360;
    render::ScenePipeline scene(device(), W, H, {});
    render::SolidPipelineOptions so;
    so.maxLive = 1;
    render::SolidPipeline solids(scene, so);
    render::ModelPipeline models(solids);
    std::string error;
    auto g = models.upload(testModel(), 2048, &error);
    REQUIRE_MESSAGE(g, error);
    CHECK(g->triangles() == 12);
    CHECK(g->emissive());

    MTL::CommandQueue* q = device()->newCommandQueue();
    MTL::CommandBuffer* cmd = q->commandBuffer();
    render::ModelDraw d = draw(g);
    d.glow = 1.0f;
    const render::SolidQuad sq = models.encodeLuminance(cmd, d);
    cmd->commit();
    cmd->waitUntilCompleted();
    REQUIRE(sq.visible);
    // The box covers the middle of the frame, not the corners.
    CHECK(sq.w > 80);
    CHECK(sq.w < W);
    const int bw = int(sq.w), bh = int(sq.h);
    const auto px = readTarget(solids.target(0), bw, bh);
    int covered = 0, stripe = 0, hull = 0;
    for (int i = 0; i < bw * bh; ++i) {
        const int lum = px[std::size_t(i) * 4], cov = px[std::size_t(i) * 4 + 1];
        if (cov < 250) continue;
        ++covered;
        if (lum > 245) ++stripe;  // the emissive stripe at Glow 1: full white
        else if (lum > 40 && lum < 200) ++hull;
    }
    CHECK(covered > bw * bh / 4);
    CHECK(stripe > covered / 20);
    CHECK(hull > covered / 3);
    // Glow 0: no stripe.
    d.glow = 0.0f;
    cmd = q->commandBuffer();
    models.encodeLuminance(cmd, d);
    cmd->commit();
    cmd->waitUntilCompleted();
    const auto px0 = readTarget(solids.target(0), bw, bh);
    int bright0 = 0;
    for (int i = 0; i < bw * bh; ++i) bright0 += px0[std::size_t(i) * 4 + 1] >= 250 && px0[std::size_t(i) * 4] > 245;
    CHECK(bright0 < stripe / 10);
    // Behind the camera: nothing.
    d.shot.eye = {0, 0, 4};
    d.shot.aim = {0, 0, 8};
    cmd = q->commandBuffer();
    CHECK_FALSE(models.encodeLuminance(cmd, d).visible);
    cmd->commit();
    cmd->waitUntilCompleted();
    q->release();
}

TEST_CASE("models: look B draws the base color over the frame; bloom spills past the silhouette only when on") {
    if (!device()) return;
    const int W = 640, H = 360;
    render::ScenePipeline scene(device(), W, H, {});
    render::SolidPipelineOptions so;
    so.maxLive = 1;
    render::SolidPipeline solids(scene, so);
    render::ModelPipeline models(solids);
    auto g = models.upload(testModel(), 2048);
    REQUIRE(g);
    MTL::CommandQueue* q = device()->newCommandQueue();
    MTL::TextureDescriptor* td =
        MTL::TextureDescriptor::texture2DDescriptor(MTL::PixelFormatBGRA8Unorm, NS::UInteger(W), NS::UInteger(H), false);
    td->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
    td->setStorageMode(MTL::StorageModeShared);
    MTL::Texture* out = device()->newTexture(td);
    std::vector<std::uint8_t> px(std::size_t(W * H * 4));
    auto run = [&](float bloom, float glow) {
        std::vector<std::uint8_t> init(std::size_t(W * H * 4), 0);
        for (std::size_t i = 3; i < init.size(); i += 4) init[i] = 255;
        out->replaceRegion(MTL::Region(0, 0, NS::UInteger(W), NS::UInteger(H)), 0, init.data(), NS::UInteger(W * 4));
        render::ModelDraw d = draw(g);
        d.bloom = bloom;
        d.glow = glow;
        MTL::CommandBuffer* cmd = q->commandBuffer();
        models.encodeColor(cmd, out, d, false);
        cmd->commit();
        cmd->waitUntilCompleted();
        REQUIRE(cmd->status() != MTL::CommandBufferStatusError);
        out->getBytes(px.data(), NS::UInteger(W * 4), MTL::Region(0, 0, NS::UInteger(W), NS::UInteger(H)), 0);
    };
    // Coverage (from the slot target) to tell inside from outside.
    run(0.0f, 1.0f);
    const auto cov = readTarget(solids.target(0), W, H);
    int orange = 0, white = 0, inside = 0;
    for (int i = 0; i < W * H; ++i) {
        if (cov[std::size_t(i) * 4 + 3] < 250) continue;
        ++inside;
        const int b = px[std::size_t(i) * 4], gch = px[std::size_t(i) * 4 + 1], r = px[std::size_t(i) * 4 + 2];
        if (r > gch + 20 && gch > b + 20) ++orange;  // the base color (200, 120, 40), lit
        if (r > 230 && gch > 230 && b > 200) ++white;  // the emissive stripe
    }
    CHECK(inside > W * H / 20);
    CHECK(orange > inside / 3);
    CHECK(white > inside / 30);
    auto spill = [&] {
        long sum = 0;
        for (int i = 0; i < W * H; ++i)
            if (cov[std::size_t(i) * 4 + 3] == 0) sum += px[std::size_t(i) * 4 + 1];
        return sum;
    };
    CHECK(spill() == 0);  // no bloom: outside the model the frame is untouched
    run(1.0f, 1.0f);
    CHECK(spill() > 1000);
    // No emissive material: the bloom pass is skipped.
    auto g2 = models.upload(testModel(false), 2048);
    REQUIRE(g2);
    CHECK_FALSE(g2->emissive());
    out->release();
    q->release();
}

TEST_CASE("models: cost at 1920x1080 with ~500k triangles, both looks; no render-thread allocations") {
    if (!device()) return;
    const int W = 1920, H = 1080;
    render::ScenePipeline scene(device(), W, H, {});
    scene.setField(engine::identityFlowField(W, H));
    render::SolidPipelineOptions so;
    so.maxLive = 2;
    render::SolidPipeline solids(scene, so);
    render::ModelPipeline models(solids);
    auto g = models.upload(denseModel(500000));
    const auto orbit = *engine::loadCameraPreset(std::filesystem::path(REFORCE_SOURCE_DIR) / "content" / "Camera" / "orbit.txt");
    REQUIRE(g);
    CHECK(g->triangles() > 450000);
    MTL::CommandQueue* q = device()->newCommandQueue();
    MTL::TextureDescriptor* td =
        MTL::TextureDescriptor::texture2DDescriptor(MTL::PixelFormatBGRA8Unorm, NS::UInteger(W), NS::UInteger(H), false);
    td->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
    td->setStorageMode(MTL::StorageModePrivate);
    MTL::Texture* out = device()->newTexture(td);
    double sumA = 0, sumB = 0;
    int n = 0;
    std::uint64_t allocs0 = 0;
    const int frames = 60, warm = 10;
    for (int f = 0; f < frames; ++f) {
        if (f == warm) allocs0 = perf::renderAllocs().load();
        NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
        render::ModelDraw d = draw(g);
        d.shot = engine::pathShot(orbit, f / double(frames), {});
        auto time = [&](bool color) {
            MTL::CommandBuffer* cmd = q->commandBuffer();
            if (color) models.encodeColor(cmd, out, d, false);
            else models.encodeLuminance(cmd, d);
            cmd->commit();
            cmd->waitUntilCompleted();
            REQUIRE(cmd->status() != MTL::CommandBufferStatusError);
            return (cmd->GPUEndTime() - cmd->GPUStartTime()) * 1000.0;
        };
        const double a = time(false), b = time(true);
        if (f >= warm) {
            sumA += a;
            sumB += b;
            ++n;
        }
        pool->release();
    }
    const double a = sumA / n, b = sumB / n;
    MESSAGE("model cost at 1920x1080, ~500k triangles: look A " << a << " ms, look B (bloom) " << b << " ms GPU");
    PERF_BUDGET(a < 4.0);
    PERF_BUDGET(b < 5.0);
    CHECK(perf::renderAllocs().load() == allocs0);
    out->release();
    q->release();
}
