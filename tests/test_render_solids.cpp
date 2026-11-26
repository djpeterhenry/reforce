// 3D solids phase 2 (PLAN 5L.2): render/SolidPipeline on the GPU, offscreen.
//   - the vertex layout (engine::MeshVertex = the MSL MeshVertex) and the drawn coverage vs the CPU hull area
//   - back-face culling keeps the front faces (same image as with culling off)
//   - the --solid-test spec parser
//   - cost of the solid pass + sprite composite at 1920x1080 with 3 solids, and no render-thread allocations.
// Phase 3 (PLAN 5L.3): the WaveShape tile (lines + mips), Text=0 / 1 / 2 on the faces, the cost of the tile passes
// at 1920x1080 and no render-thread allocations with a Text=0 solid live.
#include <doctest/doctest.h>

#include "PerfBudget.hpp"

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "engine/FlowFieldBuilder.hpp"
#include "engine/Perf.hpp"
#include "engine/Sprites.hpp"
#include "render/ScenePipeline.hpp"
#include "render/SolidPipeline.hpp"

using namespace reforce;

namespace {

MTL::Device* device() {
    static MTL::Device* d = MTL::CreateSystemDefaultDevice();
    return d;
}

// The solid's box in its target: (luminance, alpha) per pixel, 0..1.
struct Box {
    int w = 0, h = 0;
    std::vector<float> lum, alpha, share;  // share: the rubik's sticker share (b)
    std::vector<float> shade;              // a: coverage x the puzzles' shading
};

Box drawOne(render::ScenePipeline& scene, render::SolidPipeline& solids, const render::SolidDraw& d,
            render::SolidQuad* quad = nullptr) {
    Box b;
    MTL::CommandQueue* q = scene.device()->newCommandQueue();
    MTL::CommandBuffer* cmd = q->commandBuffer();
    const render::SolidQuad sq = solids.encode(cmd, 0, d);
    if (quad) *quad = sq;
    REQUIRE(sq.visible);
    b.w = int(sq.w);
    b.h = int(sq.h);
    MTL::Buffer* buf = scene.device()->newBuffer(NS::UInteger(b.w * b.h * 4), MTL::ResourceStorageModeShared);
    MTL::BlitCommandEncoder* blit = cmd->blitCommandEncoder();
    blit->copyFromTexture(solids.target(0), 0, 0, MTL::Origin(0, 0, 0), MTL::Size(NS::UInteger(b.w), NS::UInteger(b.h), 1),
                          buf, 0, NS::UInteger(b.w * 4), NS::UInteger(b.w * b.h * 4));
    blit->endEncoding();
    cmd->commit();
    cmd->waitUntilCompleted();
    const auto* p = static_cast<const std::uint8_t*>(buf->contents());
    for (int i = 0; i < b.w * b.h; ++i) {
        b.lum.push_back(p[i * 4] / 255.0f);
        b.alpha.push_back(p[i * 4 + 1] / 255.0f);
        b.share.push_back(p[i * 4 + 2] / 255.0f);
        b.shade.push_back(p[i * 4 + 3] / 255.0f);
    }
    buf->release();
    q->release();
    return b;
}

render::SolidDraw cubeDraw() {
    render::SolidDraw d;
    d.mesh = engine::MeshKind::Cube;
    d.pose.rotX = 0.5;
    d.pose.rotY = 0.7;
    d.pose.scale = 0.6;
    return d;
}

}  // namespace

TEST_CASE("solids: MeshVertex layout matches the MSL struct (packed_float3 pos, normal; packed_float2 uv; edge)") {
    static_assert(sizeof(engine::MeshVertex) == 36);
    static_assert(offsetof(engine::MeshVertex, pos) == 0);
    static_assert(offsetof(engine::MeshVertex, normal) == 12);
    static_assert(offsetof(engine::MeshVertex, uv) == 24);
    static_assert(offsetof(engine::MeshVertex, edge) == 32);
    CHECK(true);
}

TEST_CASE("solids: drawn coverage matches the CPU hull, faces shade as the reference, culling keeps front faces") {
    if (!device()) return;
    const int W = 640, H = 360;
    render::ScenePipeline scene(device(), W, H, {});
    for (int msaa : {4, 1}) {
        CAPTURE(msaa);
        render::SolidPipelineOptions o;
        o.maxLive = 1;
        o.msaa = msaa;
        render::SolidPipeline solids(scene, o);
        const render::SolidDraw d = cubeDraw();
        render::SolidQuad sq;
        const Box b = drawOne(scene, solids, d, &sq);
        const auto ref = render::solidReference(solids.mesh(d.mesh), d, W, H);
        // The quad covers the projected extent (plus at most a 2 px margin) and samples the box's sub-rectangle.
        CHECK(sq.cx - sq.w / 2 <= ref.x0);
        CHECK(sq.cx + sq.w / 2 >= ref.x1);
        CHECK(sq.cx - sq.w / 2 >= ref.x0 - 2.5f);
        CHECK(sq.u1 == doctest::Approx(sq.w / W));
        CHECK(sq.v1 == doctest::Approx(sq.h / H));
        // Coverage = the projected hull's area (a wrong vertex layout would draw garbage).
        double cov = 0;
        for (float a : b.alpha) cov += a;
        CHECK(cov == doctest::Approx(ref.area).epsilon(0.01));
        // Face centers: premultiplied luminance = the reference (alpha 1 inside).
        REQUIRE(ref.faces.size() == 3);
        for (const auto& f : ref.faces) {
            const int x = int(f.x - (sq.cx - sq.w / 2)), y = int(f.y - (sq.cy - sq.h / 2));
            const std::size_t i = std::size_t(y * b.w + x);
            CHECK(b.alpha[i] == doctest::Approx(1.0));
            CHECK(b.lum[i] == doctest::Approx(f.lum).epsilon(0.02));
        }
        // Back faces culled: the same picture as drawing both sides with the depth test.
        render::SolidPipelineOptions o2 = o;
        o2.cullBack = false;
        render::SolidPipeline both(scene, o2);
        const Box b2 = drawOne(scene, both, d);
        REQUIRE(b2.lum.size() == b.lum.size());
        float maxDiff = 0;
        for (std::size_t i = 0; i < b.lum.size(); ++i) maxDiff = std::max(maxDiff, std::fabs(b.lum[i] - b2.lum[i]));
        CHECK(maxDiff <= 2.0f / 255.0f);
    }
}

TEST_CASE("solids: the torus (non-convex) needs the depth test; off-screen solids encode nothing") {
    if (!device()) return;
    const int W = 320, H = 180;
    render::ScenePipeline scene(device(), W, H, {});
    render::SolidPipelineOptions o;
    o.maxLive = 1;
    render::SolidPipeline solids(scene, o);
    render::SolidDraw d;
    d.mesh = engine::MeshKind::Torus;
    d.pose.rotX = 0.9;
    d.pose.scale = 0.7;
    const Box b = drawOne(scene, solids, d);
    double cov = 0;
    for (float a : b.alpha) cov += a;
    CHECK(cov > 1000);
    // The hole in the middle stays empty.
    CHECK(b.alpha[std::size_t((b.h / 2) * b.w + b.w / 2)] < 0.05f);

    MTL::CommandQueue* q = device()->newCommandQueue();
    MTL::CommandBuffer* cmd = q->commandBuffer();
    d.pose.x = 5;  // far right, off screen
    CHECK_FALSE(solids.encode(cmd, 0, d).visible);
    CHECK_FALSE(solids.encode(cmd, 7, cubeDraw()).visible);  // no such slot
    cmd->commit();
    q->release();
}

TEST_CASE("solids: --solid-test spec parser") {
    render::SolidTestSpec s;
    std::string err;
    REQUIRE(render::parseSolidTest("Cube,rotx=.5,RotY=0.7,scale=.6,spiny=1.5,pen=1,epen=0,flag=4096,fill=.2", s, &err));
    CHECK(s.draw.mesh == engine::MeshKind::Cube);
    CHECK(s.draw.pose.rotX == doctest::Approx(0.5));
    CHECK(s.draw.pose.rotY == doctest::Approx(0.7));
    CHECK(s.draw.pose.scale == doctest::Approx(0.6));
    CHECK(s.spin[1] == doctest::Approx(1.5));
    CHECK(s.epen == doctest::Approx(0));
    CHECK(s.flags == 4096u);
    CHECK(s.draw.shading.fill == doctest::Approx(0.2f));
    REQUIRE(render::parseSolidTest("icosahedron:x=-.5:pers=0", s, &err));
    CHECK(s.draw.mesh == engine::MeshKind::Icosahedron);
    CHECK(s.draw.pose.x == doctest::Approx(-0.5));
    CHECK_FALSE(s.draw.perspective);
    CHECK(s.epen < 0);  // default: no EPen
    CHECK_FALSE(render::parseSolidTest("blob", s, &err));
    CHECK(err.find("unknown mesh") != std::string::npos);
    CHECK_FALSE(render::parseSolidTest("cube,spin=1", s, &err));
    CHECK_FALSE(render::parseSolidTest("cube,rotx=abc", s, &err));
    CHECK(s.draw.text == render::SolidText::Flat);  // hook default
    REQUIRE(render::parseSolidTest("cube,text=0", s, &err));
    CHECK(s.draw.text == render::SolidText::Wave);
    REQUIRE(render::parseSolidTest("cube,text=1", s, &err));
    CHECK(s.draw.text == render::SolidText::Frame);
    CHECK_FALSE(render::parseSolidTest("cube,text=3", s, &err));
}

TEST_CASE("solids: cost at 1920x1080 with 3 solids; no render-thread allocations") {
    if (!device()) return;
    const int W = 1920, H = 1080;
    render::ScenePipeline scene(device(), W, H, {});
    scene.setField(engine::identityFlowField(W, H));
    render::SolidPipeline solids(scene, {});
    MTL::CommandQueue* q = device()->newCommandQueue();
    render::SolidDraw d[3];
    const engine::MeshKind kinds[3] = {engine::MeshKind::Cube, engine::MeshKind::Icosahedron, engine::MeshKind::Torus};
    for (int i = 0; i < 3; ++i) {
        d[i].mesh = kinds[i];
        d[i].pose.x = -0.55 + 0.55 * i;
        d[i].pose.scale = 0.45;  // each about 30 % of the frame height across
    }
    // Per frame: (a) the step alone, (b) only the 3 solid passes, (c) solids + the step with their sprite quads
    // (a keyed-copy Pen stamp into the feedback and a half-way-lighten display draw each, like Pen + EPen sprites).
    double sumA = 0, sumB = 0, sumC = 0;
    int n = 0;
    std::uint64_t allocs0 = 0;
    const int frames = 120, warm = 20;
    for (int f = 0; f < frames; ++f) {
        if (f == warm) allocs0 = perf::renderAllocs().load();
        NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
        for (int i = 0; i < 3; ++i) {
            d[i].pose.rotX = 0.02 * f + i;
            d[i].pose.rotY = 0.03 * f;
        }
        auto run = [&](bool solidsOn, bool step) {
            MTL::CommandBuffer* cmd = q->commandBuffer();
            std::vector<render::SpriteQuad> fb, disp;
            if (solidsOn)
                for (int i = 0; i < 3; ++i) {
                    const render::SolidQuad sq = solids.encode(cmd, i, d[i]);
                    if (!sq.visible) continue;
                    render::SpriteQuad r;
                    r.texture = sq.texture;
                    r.cx = sq.cx; r.cy = sq.cy; r.w = sq.w; r.h = sq.h;
                    r.u1 = sq.u1; r.v1 = sq.v1;
                    r.pen = 1;
                    r.op = int(engine::SpriteOp::KeyedCopy);
                    fb.push_back(r);
                    r.op = int(engine::SpriteOp::Fetch);
                    r.blend = int(engine::SpriteBlend::Selective);
                    disp.push_back(r);
                }
            if (step)
                scene.encodeStep(cmd, nullptr, 0, render::FlowBlend{}, fb.data(), fb.size(), true, disp.data(),
                                 disp.size());
            cmd->commit();
            cmd->waitUntilCompleted();
            REQUIRE(cmd->status() != MTL::CommandBufferStatusError);
            return (cmd->GPUEndTime() - cmd->GPUStartTime()) * 1000.0;
        };
        const double a = run(false, true), b = run(true, false), c = run(true, true);
        if (f >= warm) {
            sumA += a;
            sumB += b;
            sumC += c;
            ++n;
        }
        pool->release();
    }
    const double a = sumA / n, b = sumB / n, c = sumC / n;
    MESSAGE("1920x1080, 3 solids (cube, icosahedron, torus; msaa " << solids.msaa() << "): step alone " << a
            << " ms, solid passes " << b << " ms (" << b / 3 << " ms/solid), step + solids composited " << c
            << " ms -> " << (c - a) / 3 << " ms GPU per solid");
    PERF_BUDGET((c - a) / 3 < 1.0);  // design target 0.3 ms; loose here (shared GPU)
    CHECK(perf::renderAllocs().load() == allocs0);
    q->release();
}

namespace {

// A WaveShape-like line set in tile pixels: `paths` horizontal sine waves of `steps` segments each.
std::vector<engine::LineSegment> tileWaves(int tile, int paths, int steps, float phase) {
    std::vector<engine::LineSegment> segs;
    for (int p = 0; p < paths; ++p) {
        const float y0 = tile * (p + 0.5f) / paths;
        auto pt = [&](int i, float& x, float& y) {
            x = tile * float(i) / steps;
            y = y0 + 0.3f * tile / paths * std::sin(6.2832f * (3.0f * i / steps) + phase + p);
        };
        for (int i = 0; i < steps; ++i) {
            engine::LineSegment g{};
            pt(i, g.ax, g.ay);
            pt(i + 1, g.bx, g.by);
            g.penA = g.penB = 1.0f;
            g.radius = 1.5f;
            g.path = float(p * steps);
            segs.push_back(g);
        }
    }
    return segs;
}

std::vector<std::uint8_t> readLevel(MTL::Device* dev, MTL::CommandQueue* q, MTL::Texture* t, int level) {
    const int w = std::max(1, int(t->width()) >> level), h = std::max(1, int(t->height()) >> level);
    MTL::Buffer* buf = dev->newBuffer(NS::UInteger(w * h), MTL::ResourceStorageModeShared);
    MTL::CommandBuffer* cmd = q->commandBuffer();
    MTL::BlitCommandEncoder* blit = cmd->blitCommandEncoder();
    blit->copyFromTexture(t, 0, NS::UInteger(level), MTL::Origin(0, 0, 0), MTL::Size(NS::UInteger(w), NS::UInteger(h), 1),
                          buf, 0, NS::UInteger(w), NS::UInteger(w * h));
    blit->endEncoding();
    cmd->commit();
    cmd->waitUntilCompleted();
    const auto* p = static_cast<const std::uint8_t*>(buf->contents());
    std::vector<std::uint8_t> out(p, p + w * h);
    buf->release();
    return out;
}

double mean(const std::vector<std::uint8_t>& v) {
    double s = 0;
    for (auto x : v) s += x;
    return v.empty() ? 0 : s / double(v.size()) / 255.0;
}

}  // namespace

TEST_CASE("solids: the WaveShape tile has lines and a full mip chain; Text=0 / 1 / 2 on the faces") {
    if (!device()) return;
    const int W = 640, H = 360;
    render::ScenePipeline scene(device(), W, H, {});
    render::SolidPipelineOptions o;
    o.maxLive = 1;
    o.tileSize = 256;
    render::SolidPipeline solids(scene, o);
    REQUIRE(solids.tileSize() == 256);
    REQUIRE(solids.waveTile()->mipmapLevelCount() == 9);
    MTL::CommandQueue* q = device()->newCommandQueue();

    // Before any tile was prepared, Text=0 reads 0: the faces are the Fill floor (shade 0, no edge).
    render::SolidDraw d = cubeDraw();
    d.shading.shade = 0;
    d.shading.edge = 0;
    d.shading.fill = 0.25f;
    d.text = render::SolidText::Wave;
    render::SolidQuad sq;
    Box b = drawOne(scene, solids, d, &sq);
    auto center = [&](const Box& bx) { return bx.lum[std::size_t((bx.h / 2) * bx.w + bx.w / 2)]; };
    CHECK(center(b) == doctest::Approx(0.25).epsilon(0.03));

    // The tile: lines at level 0; the mips average them (level 4 = 16x16 px of level 0 per texel).
    const auto segs = tileWaves(256, 6, 64, 0.0f);
    {
        MTL::CommandBuffer* cmd = q->commandBuffer();
        solids.encodeWaveTile(cmd, segs.data(), segs.size());
        cmd->commit();
        cmd->waitUntilCompleted();
    }
    const auto l0 = readLevel(device(), q, solids.waveTile(), 0);
    const auto l4 = readLevel(device(), q, solids.waveTile(), 4);
    int lit = 0;
    for (auto v : l0) lit += v > 128;
    CHECK(lit > 1000);
    CHECK(mean(l0) > 0.05);
    CHECK(mean(l4) == doctest::Approx(mean(l0)).epsilon(0.1));  // mips generated (not left black)
    std::uint8_t mx4 = 0;
    for (auto v : l4) mx4 = std::max(mx4, v);
    CHECK(mx4 < 200);  // minified lines are averaged, not point-sampled at full pen

    // Text=0 now: the face varies, never below the Fill floor; Text=2 is flat (1 before shading).
    b = drawOne(scene, solids, d);
    float lo = 1, hi = 0;
    for (std::size_t i = 0; i < b.lum.size(); ++i)
        if (b.alpha[i] > 0.99f) {
            lo = std::min(lo, b.lum[i]);
            hi = std::max(hi, b.lum[i]);
        }
    CHECK(lo >= 0.25f - 0.02f);
    CHECK(hi > 0.6f);
    d.text = render::SolidText::Flat;
    b = drawOne(scene, solids, d);
    CHECK(center(b) == doctest::Approx(1.0).epsilon(0.01));

    // Text=1: the previous intensity frame (cleared: 0) -> the floor.
    d.text = render::SolidText::Frame;
    {
        MTL::CommandBuffer* cmd = q->commandBuffer();
        solids.encodeFrameTile(cmd);
        cmd->commit();
        cmd->waitUntilCompleted();
    }
    REQUIRE(solids.frameTile()->mipmapLevelCount() == 9);
    b = drawOne(scene, solids, d);
    CHECK(center(b) == doctest::Approx(0.25).epsilon(0.03));
    q->release();
}

TEST_CASE("solids: cost of the WaveShape / frame tiles at 1920x1080; no render-thread allocations with Text=0") {
    if (!device()) return;
    const int W = 1920, H = 1080;
    render::ScenePipeline scene(device(), W, H, {});
    scene.setField(engine::identityFlowField(W, H));
    render::SolidPipeline solids(scene, {});
    MTL::CommandQueue* q = device()->newCommandQueue();
    render::SolidDraw d;
    d.mesh = engine::MeshKind::Icosahedron;
    d.pose.scale = 0.55;
    d.text = render::SolidText::Wave;
    // A busy WaveShape: 8 paths x 512 segments in the tile; the frame draws the same count.
    const int T = solids.tileSize();
    std::vector<engine::LineSegment> frameSegs = tileWaves(1080, 8, 512, 0.0f);
    double sumStep = 0, sumWave = 0, sumFrame = 0, sumAll = 0;
    int n = 0;
    std::uint64_t allocs0 = 0;
    const int frames = 120, warm = 20;
    for (int f = 0; f < frames; ++f) {
        if (f == warm) allocs0 = perf::renderAllocs().load();
        NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
        const auto tileSegs = tileWaves(T, 8, 512, 0.05f * f);
        d.pose.rotX = 0.02 * f;
        d.pose.rotY = 0.03 * f;
        auto run = [&](bool wave, bool frame, bool solid) {
            MTL::CommandBuffer* cmd = q->commandBuffer();
            std::vector<render::SpriteQuad> fb;
            if (wave) solids.encodeWaveTile(cmd, tileSegs.data(), tileSegs.size());
            if (frame) solids.encodeFrameTile(cmd);
            if (solid) {
                const render::SolidQuad sq = solids.encode(cmd, 0, d);
                if (sq.visible) {
                    render::SpriteQuad r;
                    r.texture = sq.texture;
                    r.cx = sq.cx; r.cy = sq.cy; r.w = sq.w; r.h = sq.h;
                    r.u1 = sq.u1; r.v1 = sq.v1;
                    r.pen = 1;
                    r.op = int(engine::SpriteOp::KeyedCopy);
                    fb.push_back(r);
                }
            }
            scene.encodeStep(cmd, frameSegs.data(), frameSegs.size(), render::FlowBlend{}, fb.data(), fb.size(), true,
                             nullptr, 0);
            cmd->commit();
            cmd->waitUntilCompleted();
            REQUIRE(cmd->status() != MTL::CommandBufferStatusError);
            return (cmd->GPUEndTime() - cmd->GPUStartTime()) * 1000.0;
        };
        const double a = run(false, false, false), w = run(true, false, false), fr = run(false, true, false),
                     all = run(true, false, true);
        if (f >= warm) {
            sumStep += a;
            sumWave += w;
            sumFrame += fr;
            sumAll += all;
            ++n;
        }
        pool->release();
    }
    const double a = sumStep / n, w = sumWave / n - a, fr = sumFrame / n - a, all = sumAll / n - a;
    MESSAGE("1920x1080, tile " << T << ": step alone " << a << " ms; + WaveShape tile (4096 segments + mips) " << w
            << " ms; + frame tile (crop + mips) " << fr << " ms; + WaveShape tile and a Text=0 icosahedron "
            << "composited " << all << " ms GPU");
    PERF_BUDGET(w < 0.5);  // design estimate ~0.02 ms; loose here (shared GPU)
    PERF_BUDGET(fr < 0.5);
    CHECK(perf::renderAllocs().load() == allocs0);
    q->release();
}

// ---------------------------------------------------------------- the rubik solid (docs/3d-design.md)

namespace {
// Pixels of the box (fully covered) per sticker level and on the plastic (lum ~0).
std::array<int, 7> rubikLevels(const Box& b, const std::array<float, 6>& st) {
    std::array<int, 7> n{};
    for (std::size_t i = 0; i < b.lum.size(); ++i) {
        if (b.alpha[i] < 0.99f) continue;
        if (b.lum[i] < 0.02f) ++n[6];
        for (int k = 0; k < 6; ++k)
            if (std::fabs(b.lum[i] - st[std::size_t(k)]) < 0.01f) ++n[std::size_t(k)];
    }
    return n;
}
render::SolidDraw rubikDraw(double turn) {
    render::SolidDraw d;
    d.mesh = engine::MeshKind::Rubik;
    d.pose.rotX = 0.5;
    d.pose.rotY = 0.6;
    d.pose.scale = 0.6;
    d.shading.shade = 0;
    d.stickers = {0.2f, 0.35f, 0.5f, 0.65f, 0.8f, 0.95f};
    d.rubik = engine::rubik::frameAt(3, 30, turn);
    return d;
}
}  // namespace

TEST_CASE("solids: rubik: solved shows one color from each opposite pair, scrambled shows all six; plastic between") {
    if (!device()) return;
    const int W = 640, H = 360;
    render::ScenePipeline scene(device(), W, H, {});
    render::SolidPipelineOptions o;
    o.maxLive = 1;
    render::SolidPipeline solids(scene, o);
    const render::SolidDraw solved = rubikDraw(0);
    const auto n = rubikLevels(drawOne(scene, solids, solved), solved.stickers);
    int shown = 0;
    for (int pair = 0; pair < 3; ++pair) {
        const int a = n[std::size_t(2 * pair)], b = n[std::size_t(2 * pair + 1)];
        CHECK((a > 500) != (b > 500));  // one face of each pair faces the camera
        // (anti-aliased sticker borders pass through the other levels, so a hidden color still gets a few pixels)
        CHECK(std::min(a, b) * 4 < std::max(a, b));
        shown += (a > 500) + (b > 500);
    }
    CHECK(shown == 3);
    CHECK(n[6] > 200);  // the plastic between the stickers
    const render::SolidDraw mixed = rubikDraw(30);
    const auto m = rubikLevels(drawOne(scene, solids, mixed), mixed.stickers);
    for (int k = 0; k < 6; ++k) CHECK(m[std::size_t(k)] > 100);
    // Mid-turn: the turning layer pokes out of the still cube's box, and the quad still holds all of it (no clipping
    // at the box edge: coverage stays inside the margin).
    const render::SolidDraw turning = rubikDraw(40.5);
    REQUIRE(turning.rubik.turning);
    const Box t = drawOne(scene, solids, turning);
    for (int x = 0; x < t.w; ++x) {
        CHECK(t.alpha[std::size_t(x)] < 0.5f);
        CHECK(t.alpha[std::size_t((t.h - 1) * t.w + x)] < 0.5f);
    }
    for (int y = 0; y < t.h; ++y) {
        CHECK(t.alpha[std::size_t(y * t.w)] < 0.5f);
        CHECK(t.alpha[std::size_t(y * t.w + t.w - 1)] < 0.5f);
    }
}

TEST_CASE("solids: ColorKey = 0 keeps the whole solid opaque in the tracked alpha, dark plastic included") {
    if (!device()) return;
    const int W = 320, H = 180;
    render::ScenePipeline scene(device(), W, H, {});
    render::SolidPipelineOptions o;
    o.maxLive = 1;
    render::SolidPipeline solids(scene, o);
    render::SolidDraw d = rubikDraw(0);
    d.stickers = {0.02f, 0.02f, 0.02f, 0.02f, 0.02f, 0.02f};  // everything dark: the key would drop it all
    MTL::CommandQueue* q = device()->newCommandQueue();
    MTL::TextureDescriptor* td =
        MTL::TextureDescriptor::texture2DDescriptor(MTL::PixelFormatBGRA8Unorm, NS::UInteger(W), NS::UInteger(H), false);
    td->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
    td->setStorageMode(MTL::StorageModeShared);
    MTL::Texture* out = device()->newTexture(td);
    engine::Palette pal{};
    engine::MapLut lut{};
    for (int k = 0; k < engine::kPaletteSize; ++k) lut[std::size_t(k)] = float(k) / 255.f;
    std::vector<std::uint8_t> px(std::size_t(W * H * 4));
    render::SolidQuad sq;
    auto run = [&](bool track, float opacity) {
        MTL::CommandBuffer* cmd = q->commandBuffer();
        sq = solids.encode(cmd, 0, d);
        render::SpriteQuad o2;
        o2.texture = sq.texture;
        o2.cx = sq.cx;
        o2.cy = sq.cy;
        o2.w = sq.w;
        o2.h = sq.h;
        o2.u0 = sq.u0;
        o2.v0 = sq.v0;
        o2.u1 = sq.u1;
        o2.v1 = sq.v1;
        o2.opacity = opacity;
        o2.opaque = true;
        scene.encodeColorize(cmd, pal, lut, out, nullptr, nullptr, 0, track, &o2, 1);
        cmd->commit();
        cmd->waitUntilCompleted();
        out->getBytes(px.data(), NS::UInteger(W * 4), MTL::Region(0, 0, NS::UInteger(W), NS::UInteger(H)), 0);
    };
    auto alpha = [&](int x, int y) { return int(px[std::size_t((y * W + x) * 4 + 3)]); };
    run(true, 1.0f);
    REQUIRE(sq.visible);
    CHECK(alpha(int(sq.cx), int(sq.cy)) == 255);  // the cube's middle
    CHECK(alpha(2, 2) == 0);                      // the visuals around it stay keyed
    run(true, 0.5f);                              // fading: by its opacity
    CHECK(std::abs(alpha(int(sq.cx), int(sq.cy)) - 128) <= 1);
    run(false, 1.0f);  // not tracked: alpha 1 everywhere, nothing extra drawn
    CHECK(alpha(2, 2) == 255);
    out->release();
    q->release();
}

TEST_CASE("movie sprites: ColorKey = 0 without Full color keeps the whole movie frame opaque in the tracked alpha") {
    if (!device()) return;
    const int W = 320, H = 180;
    render::ScenePipeline scene(device(), W, H, {});
    MTL::CommandQueue* q = device()->newCommandQueue();
    MTL::TextureDescriptor* td =
        MTL::TextureDescriptor::texture2DDescriptor(MTL::PixelFormatBGRA8Unorm, NS::UInteger(W), NS::UInteger(H), false);
    td->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
    td->setStorageMode(MTL::StorageModeShared);
    MTL::Texture* out = device()->newTexture(td);
    MTL::Texture* frame = device()->newTexture(td);  // an all-black movie frame: the key would drop all of it
    std::vector<std::uint8_t> black(std::size_t(W * H * 4), 0);
    frame->replaceRegion(MTL::Region(0, 0, NS::UInteger(W), NS::UInteger(H)), 0, black.data(), NS::UInteger(W * 4));
    const int handle = scene.addVideoSpriteTexture(false);
    scene.setSpriteFrame(handle, frame);
    engine::Palette pal{};
    engine::MapLut lut{};
    for (int k = 0; k < engine::kPaletteSize; ++k) lut[std::size_t(k)] = float(k) / 255.f;
    std::vector<std::uint8_t> px(std::size_t(W * H * 4));
    auto run = [&](float opacity) {
        render::SpriteQuad o;
        o.texture = handle;
        o.cx = 100;
        o.cy = 90;
        o.w = 80;
        o.h = 60;
        o.opacity = opacity;
        o.opaque = true;
        o.wholeRect = true;
        MTL::CommandBuffer* cmd = q->commandBuffer();
        scene.encodeColorize(cmd, pal, lut, out, nullptr, nullptr, 0, true, &o, 1);
        cmd->commit();
        cmd->waitUntilCompleted();
        out->getBytes(px.data(), NS::UInteger(W * 4), MTL::Region(0, 0, NS::UInteger(W), NS::UInteger(H)), 0);
    };
    auto alpha = [&](int x, int y) { return int(px[std::size_t((y * W + x) * 4 + 3)]); };
    run(1.0f);
    CHECK(alpha(100, 90) == 255);  // the black middle of the movie
    CHECK(alpha(65, 65) == 255);   // near its corner
    CHECK(alpha(150, 90) == 0);    // outside it: keyed like the visuals
    run(0.5f);                     // fading: by its opacity
    CHECK(std::abs(alpha(100, 90) - 128) <= 1);
    frame->release();
    out->release();
    q->release();
}

TEST_CASE("solids: rubik in color: sticker borders blend the sticker and plastic colors, not the ColorMap between") {
    if (!device()) return;
    const int W = 320, H = 180;
    render::ScenePipeline scene(device(), W, H, {});
    render::SolidPipelineOptions o;
    o.maxLive = 1;
    render::SolidPipeline solids(scene, o);
    render::SolidDraw d = rubikDraw(30);
    d.stickers = {0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f};
    // Black at 0, red around the stickers' .9 (.78 up: the sticker intensity recovered from a thin border share is
    // a few levels off, and picked levels sit on flat stretches of the map), bright green everywhere in between: any
    // brightness blend shows green.
    engine::Palette pal{};
    for (int k = 0; k < engine::kPaletteSize; ++k)
        pal[std::size_t(k)] = k == 0 ? engine::PaletteEntry{0, 0, 0, 1} : k >= 200 ? engine::PaletteEntry{1, 0, 0, 1}
                                                                                    : engine::PaletteEntry{0, 1, 0, 1};
    engine::MapLut lut{};
    for (int k = 0; k < engine::kPaletteSize; ++k) lut[std::size_t(k)] = float(k) / 255.f;
    MTL::CommandQueue* q = device()->newCommandQueue();
    MTL::TextureDescriptor* td =
        MTL::TextureDescriptor::texture2DDescriptor(MTL::PixelFormatBGRA8Unorm, NS::UInteger(W), NS::UInteger(H), false);
    td->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
    td->setStorageMode(MTL::StorageModeShared);
    MTL::Texture* out = device()->newTexture(td);
    MTL::CommandBuffer* cmd = q->commandBuffer();
    const render::SolidQuad sq = solids.encode(cmd, 0, d);
    REQUIRE(sq.visible);
    render::SpriteQuad c;
    c.texture = sq.texture;
    c.cx = sq.cx;
    c.cy = sq.cy;
    c.w = sq.w;
    c.h = sq.h;
    c.u0 = sq.u0;
    c.v0 = sq.v0;
    c.u1 = sq.u1;
    c.v1 = sq.v1;
    c.opacity = 1;
    scene.encodeColorize(cmd, pal, lut, out, nullptr, nullptr, 0, false, nullptr, 0, &c, 1);
    cmd->commit();
    cmd->waitUntilCompleted();
    std::vector<std::uint8_t> px(std::size_t(W * H * 4));
    out->getBytes(px.data(), NS::UInteger(W * 4), MTL::Region(0, 0, NS::UInteger(W), NS::UInteger(H)), 0);
    int red = 0, partial = 0, maxGreen = 0;
    for (int i = 0; i < W * H; ++i) {
        const int b = px[std::size_t(i * 4)], g = px[std::size_t(i * 4 + 1)], r = px[std::size_t(i * 4 + 2)];
        (void)b;
        maxGreen = std::max(maxGreen, g);
        red += r > 240;
        partial += r > 15 && r < 240;
    }
    CHECK(red > 2000);      // the stickers
    CHECK(partial > 100);   // anti-aliased borders: between black and red...
    CHECK(maxGreen <= 2);   // ...never through the ColorMap's green
    out->release();
    q->release();
}

TEST_CASE("solids: fifteen: the solved board shows the four row colors, numbers on the tiles, no tray") {
    if (!device()) return;
    const int W = 640, H = 360;
    render::ScenePipeline scene(device(), W, H, {});
    render::SolidPipelineOptions o;
    o.maxLive = 1;
    render::SolidPipeline solids(scene, o);
    render::SolidDraw d;
    d.mesh = engine::MeshKind::Fifteen;
    d.pose.scale = 0.8;
    d.shading.shade = 0;
    d.stickers = {0.2f, 0.35f, 0.5f, 0.65f, 0.8f, 0.95f};
    d.fifteen = engine::fifteen::frameAt(5, 31, 31);  // solved
    const Box b = drawOne(scene, solids, d);
    std::array<int, 6> n{};
    int digits = 0, empty = 0;
    for (std::size_t i = 0; i < b.lum.size(); ++i) {
        if (b.alpha[i] < 0.01f) ++empty;  // the gaps and the blank (no tray)
        if (b.alpha[i] < 0.99f) continue;
        if (b.share[i] < 0.02f) ++digits;  // inside a number's strokes: still the tile's intensity
        for (int k = 0; k < 6; ++k)
            if (std::fabs(b.lum[i] - d.stickers[std::size_t(k)]) < 0.01f) ++n[std::size_t(k)];
    }
    for (int r = 0; r < 4; ++r) CHECK(n[std::size_t(render::kFifteenRowColor[r])] > 2000);  // each row's color
    CHECK(n[3] + n[5] < 200);  // the colors no row uses
    CHECK(digits > 500);       // the numbers' strokes (b = 0)
    CHECK(empty > 3000);       // the blank cell at least (a tile's area), and the gaps
    // A tile's center: tile 6 (row 1, col 1) at board (-.5, .5); its "6" crosses the center (segment g).
    const engine::fifteen::Frame f = d.fifteen;
    bool found = false;
    for (const auto& t : f.tiles) found = found || (t.number == 6 && t.x == doctest::Approx(-0.5f) && t.y == doctest::Approx(0.5f));
    CHECK(found);
}

TEST_CASE("solids: fifteen as glass: clear buttons let the frame through, white numbers read on black and white") {
    if (!device()) return;
    const int W = 640, H = 360;
    render::ScenePipeline scene(device(), W, H, {});
    render::SolidPipelineOptions o;
    o.maxLive = 1;
    render::SolidPipeline solids(scene, o);
    MTL::CommandQueue* q = device()->newCommandQueue();
    MTL::TextureDescriptor* td =
        MTL::TextureDescriptor::texture2DDescriptor(MTL::PixelFormatBGRA8Unorm, NS::UInteger(W), NS::UInteger(H), false);
    td->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
    td->setStorageMode(MTL::StorageModeShared);
    MTL::Texture* out = device()->newTexture(td);
    render::GlassDraw g;
    g.slot = 0;
    g.pose.scale = 0.8;
    g.fifteen = engine::fifteen::frameAt(5, 31, 31);  // solved
    g.tintAmount = 0.15f;
    for (int r = 0; r < 4; ++r) {
        g.tint[std::size_t(r)] = {0.95f, 0.9f, 0.8f};
        g.numeral[std::size_t(r)] = {0.95f, 0.95f, 0.95f};
    }
    std::vector<std::uint8_t> px(std::size_t(W * H * 4));
    auto run = [&](std::uint8_t fill) {
        std::vector<std::uint8_t> init(std::size_t(W * H * 4), fill);
        for (std::size_t i = 3; i < init.size(); i += 4) init[i] = 255;
        out->replaceRegion(MTL::Region(0, 0, NS::UInteger(W), NS::UInteger(H)), 0, init.data(), NS::UInteger(W * 4));
        MTL::CommandBuffer* cmd = q->commandBuffer();
        solids.encodeGlass(cmd, out, g, false);
        cmd->commit();
        cmd->waitUntilCompleted();
        out->getBytes(px.data(), NS::UInteger(W * 4), MTL::Region(0, 0, NS::UInteger(W), NS::UInteger(H)), 0);
    };
    auto at = [&](int x, int y, int c) { return int(px[std::size_t((y * W + x) * 4 + c)]); };
    // Over black: the glass stays dark (clear), the numbers are white; the frame's corners untouched.
    run(0);
    int white = 0, darkGlass = 0;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            white += at(x, y, 1) > 200;
            darkGlass += at(x, y, 1) < 40;
        }
    CHECK(white > 1000);
    CHECK(darkGlass > W * H / 2);
    CHECK(at(2, 2, 1) == 0);
    // Over white: the numbers still read through their dark halo; most of the glass lets the white through.
    run(255);
    int halo = 0, bright = 0;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            halo += at(x, y, 1) < 160;
            bright += at(x, y, 1) > 200;
        }
    CHECK(halo > 1000);
    CHECK(bright > W * H / 2);
    CHECK(at(2, 2, 1) == 255);
    out->release();
    q->release();
}

namespace {

// A labyrinth mid-run (board .3, walls .6, ball .9), seen from the front, a little above.
render::SolidDraw labyrinthDraw(engine::labyrinth::Player& p, int grid, double turn, float tiltGain = 2.0f) {
    render::SolidDraw d;
    d.mesh = engine::MeshKind::Labyrinth;
    d.pose.scale = 0.85;
    d.pose.rotX = -0.5;
    d.shading.shade = 0.55f;
    d.stickers = {0.3f, 0.0f, 0.6f, 0.0f, 0.9f, 0.0f};
    d.floor = 0.55f;
    d.tiltGain = tiltGain;
    d.labyrinth = p.at(4, grid, 32, turn);
    return d;
}

}  // namespace

TEST_CASE("solids: labyrinth: a see-through board, solid walls, black holes and the ball; the board tilts") {
    if (!device()) return;
    const int W = 640, H = 360;
    render::ScenePipeline scene(device(), W, H, {});
    render::SolidPipelineOptions o;
    o.maxLive = 1;
    render::SolidPipeline solids(scene, o);
    engine::labyrinth::Player p(32);
    // Mid-roll of a segment: the board is tilted.
    p.at(4, 8, 32, 0);
    p.planAll();
    const auto& seg = p.route().segments[2];
    const double turn = seg.start + 0.6 * (seg.end - seg.start);
    const render::SolidDraw d = labyrinthDraw(p, 8, turn);
    REQUIRE(std::hypot(d.labyrinth.tiltX, d.labyrinth.tiltY) > 0.01f);
    const Box b = drawOne(scene, solids, d);
    int board = 0, walls = 0, ball = 0, holes = 0;
    for (std::size_t i = 0; i < b.lum.size(); ++i) {
        const float a = b.alpha[i], s = b.share[i];
        if (std::fabs(a - 0.55f) < 0.02f && std::fabs(s - 0.55f) < 0.02f && std::fabs(b.lum[i] / a - 0.3f) < 0.01f)
            ++board;  // the board's top: its intensity at the board's opacity (premultiplied)
        if (a < 0.99f) continue;
        if (s < 0.01f) ++holes;  // black (share 0)
        else if (std::fabs(b.lum[i] - 0.6f) < 0.01f) ++walls;
        else if (std::fabs(b.lum[i] - 0.9f) < 0.01f) ++ball;
    }
    MESSAGE("labyrinth at 640x360: board " << board << " px, walls " << walls << ", ball " << ball << ", holes "
                                          << holes);
    CHECK(board > 20000);
    CHECK(walls > 5000);
    CHECK(ball > 60);
    CHECK(holes > 100);
    // Drawn level (Tilt 0) the image differs: the tilt shows.
    const Box flat = drawOne(scene, solids, labyrinthDraw(p, 8, turn, 0.0f));
    int differ = 0;
    if (flat.w == b.w && flat.h == b.h) {
        for (std::size_t i = 0; i < b.lum.size(); ++i) differ += std::fabs(flat.lum[i] - b.lum[i]) > 0.05f;
    } else {
        differ = 1000000;
    }
    CHECK(differ > 500);
}

TEST_CASE("solids: labyrinth in color: edges mix their two colors, never the ColorMap's between; holes black") {
    if (!device()) return;
    const int W = 640, H = 360;
    render::ScenePipeline scene(device(), W, H, {});
    render::SolidPipelineOptions o;
    o.maxLive = 1;
    render::SolidPipeline solids(scene, o);
    engine::labyrinth::Player p(32);
    const render::SolidDraw d = labyrinthDraw(p, 8, 7.5);
    // Black at 0, red around the board's .3, blue around the walls' .6, yellow around the ball's .9, green between:
    // a brightness blend across an edge would show green.
    engine::Palette pal{};
    for (int k = 0; k < engine::kPaletteSize; ++k) {
        engine::PaletteEntry e{0, 1, 0, 1};
        if (k == 0) e = {0, 0, 0, 1};
        if (k >= 60 && k <= 95) e = {1, 0, 0, 1};
        if (k >= 140 && k <= 170) e = {0, 0, 1, 1};
        if (k >= 215 && k <= 245) e = {1, 1, 0, 1};
        pal[std::size_t(k)] = e;
    }
    engine::MapLut lut{};
    for (int k = 0; k < engine::kPaletteSize; ++k) lut[std::size_t(k)] = float(k) / 255.f;
    MTL::CommandQueue* q = device()->newCommandQueue();
    MTL::TextureDescriptor* td =
        MTL::TextureDescriptor::texture2DDescriptor(MTL::PixelFormatBGRA8Unorm, NS::UInteger(W), NS::UInteger(H), false);
    td->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
    td->setStorageMode(MTL::StorageModeShared);
    MTL::Texture* out = device()->newTexture(td);
    MTL::CommandBuffer* cmd = q->commandBuffer();
    const render::SolidQuad sq = solids.encode(cmd, 0, d);
    REQUIRE(sq.visible);
    render::SpriteQuad c;
    c.texture = sq.texture;
    c.cx = sq.cx;
    c.cy = sq.cy;
    c.w = sq.w;
    c.h = sq.h;
    c.u0 = sq.u0;
    c.v0 = sq.v0;
    c.u1 = sq.u1;
    c.v1 = sq.v1;
    c.opacity = 1;
    c.puzzle = 2;
    c.levels[0] = 0.3f;
    c.levels[1] = 0.6f;
    c.levels[2] = 0.9f;
    scene.encodeColorize(cmd, pal, lut, out, nullptr, nullptr, 0, false, nullptr, 0, &c, 1);
    cmd->commit();
    cmd->waitUntilCompleted();
    std::vector<std::uint8_t> px(std::size_t(W * H * 4));
    out->getBytes(px.data(), NS::UInteger(W * 4), MTL::Region(0, 0, NS::UInteger(W), NS::UInteger(H)), 0);
    int red = 0, blue = 0, yellow = 0, white = 0, greenish = 0;
    for (int i = 0; i < W * H; ++i) {
        const int b = px[std::size_t(i * 4)], g = px[std::size_t(i * 4 + 1)], r = px[std::size_t(i * 4 + 2)];
        red += r > 60 && g < 10 && b < 10;
        blue += b > 60 && r < 10 && g < 10;
        yellow += r > 120 && g > 120 && b < 10;
        white += r > 200 && g > 200 && b > 60;  // the ball's highlight: yellow towards white
        greenish += g > r + 24 && g > b + 24;
    }
    MESSAGE("labyrinth in color: board " << red << " px, walls " << blue << ", ball " << yellow << ", highlight "
                                        << white << ", green " << greenish);
    CHECK(red > 15000);
    CHECK(blue > 4000);
    CHECK(yellow > 40);
    CHECK(white > 0);
    CHECK(greenish == 0);
    out->release();
    q->release();
}

TEST_CASE("solids: labyrinth cost at 1920x1080 (12 x 12 maze, the run played); no render-thread allocations") {
    if (!device()) return;
    const int W = 1920, H = 1080;
    render::ScenePipeline scene(device(), W, H, {});
    render::SolidPipeline solids(scene, {});
    MTL::CommandQueue* q = device()->newCommandQueue();
    engine::labyrinth::Player p(32);
    double gpu = 0, cpuMax = 0;
    int n = 0;
    std::uint64_t allocs0 = 0;
    const int frames = 140, warm = 20;
    for (int f = 0; f < frames; ++f) {
        if (f == warm) allocs0 = perf::renderAllocs().load();
        NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
        const auto t0 = std::chrono::steady_clock::now();
        render::SolidDraw d = labyrinthDraw(p, 12, 2.0 + f * 0.2);  // a beat every 5 frames: planning as it goes
        d.pose.scale = 0.95;
        MTL::CommandBuffer* cmd = q->commandBuffer();
        const render::SolidQuad sq = solids.encode(cmd, 0, d);
        const double cpu = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        REQUIRE(sq.visible);
        cmd->commit();
        cmd->waitUntilCompleted();
        if (f >= warm) {
            gpu += (cmd->GPUEndTime() - cmd->GPUStartTime()) * 1000.0;
            cpuMax = std::max(cpuMax, cpu);
            ++n;
        }
        pool->release();
    }
    MESSAGE("labyrinth 12 x 12 at 1920x1080 (msaa " << solids.msaa() << "): solid pass " << gpu / n
                                                    << " ms GPU; CPU per frame (planning + encode) max " << cpuMax
                                                    << " ms");
    PERF_BUDGET(gpu / n < 1.0);
    PERF_BUDGET(cpuMax < 4.0);
    CHECK(perf::renderAllocs().load() == allocs0);
    q->release();
}

namespace {
render::SolidDraw knightDraw(double turn, int board, bool web = true) {
    render::SolidDraw d;
    d.mesh = engine::MeshKind::Knight;
    d.pose.rotX = -0.9;
    d.pose.scale = 0.95;
    d.shading.shade = 0.75f;
    if (web) {
        d.tour = engine::knight::makeTour(7, true);
        d.prevTour = engine::knight::makeTour(6, true);
    }
    d.knight = engine::knight::frameAt(d.tour, d.prevTour, turn);
    d.knightLook.board = board;
    return d;
}
int countNear(const Box& b, float level, float tol = 0.01f) {
    int n = 0;
    for (std::size_t i = 0; i < b.lum.size(); ++i) n += b.alpha[i] > 0.99f && std::fabs(b.lum[i] - level) < tol;
    return n;
}
}  // namespace

TEST_CASE("solids: knight: grid lines with the visuals between them, the web grows hop by hop, the knight is shaded") {
    if (!device()) return;
    const int W = 640, H = 360;
    render::ScenePipeline scene(device(), W, H, {});
    render::SolidPipelineOptions o;
    o.maxLive = 1;
    render::SolidPipeline solids(scene, o);
    // Grid lines only (Brd 0): most of the board is see-through; the lines are at their level.
    const Box lines = drawOne(scene, solids, knightDraw(0, 0, false));
    int clear = 0;
    for (float a : lines.alpha) clear += a < 0.01f;
    CHECK(clear > int(lines.alpha.size()) / 2);
    int lined = 0;  // thin anti-aliased lines: coverage, not exact levels
    for (std::size_t i = 0; i < lines.alpha.size(); ++i) lined += lines.alpha[i] > 0.5f && lines.lum[i] > 0.15f;
    CHECK(lined > 1500);
    // Brd 1 / 2: the light squares, then the dark ones too, are filled.
    const Box light = drawOne(scene, solids, knightDraw(0, 1, false));
    const Box both = drawOne(scene, solids, knightDraw(0, 2, false));
    CHECK(countNear(light, 0.16f) > 10000);
    CHECK(countNear(light, 0.07f) < 1500);  // only the knight's shading
    CHECK(countNear(both, 0.07f) > 10000);
    // The web: more lines lit after more hops; the newest are the brightest (1), the old ones dim towards .68.
    auto webPixels = [&](double turn) {
        const Box b = drawOne(scene, solids, knightDraw(turn, 0));
        int n = 0;
        for (std::size_t i = 0; i < b.lum.size(); ++i) n += b.alpha[i] > 0.99f && b.lum[i] > 0.6f;
        return n;
    };
    const int w8 = webPixels(8), w32 = webPixels(32), w64 = webPixels(64);
    CHECK(w32 > w8 + 1000);
    CHECK(w64 > w32 + 1000);
    // At turn 0 the previous tour's web is still whole (it unravels during the first hop); half way it is gone.
    CHECK(webPixels(0) > w32);
    CHECK(webPixels(0.8) < w8);
    // Deterministic.
    const Box a1 = drawOne(scene, solids, knightDraw(20.6, 1)), a2 = drawOne(scene, solids, knightDraw(20.6, 1));
    CHECK(a1.lum == a2.lum);
    CHECK(a1.alpha == a2.alpha);
    // The knight close up (the board follows it to the center): a solid, shaded body there.
    render::SolidDraw k = knightDraw(5, 0);
    k.pose.scale = 5.0;
    k.pose.rotX = -1.25;
    k.pose.y = -0.6;
    k.knightLook.follow = 1.0f;
    render::SolidQuad kq;
    const Box kb = drawOne(scene, solids, k, &kq);
    float lo = 1, hi = 0;
    int body = 0;
    for (int y = kb.h / 3; y < kb.h * 2 / 3; ++y)
        for (int x = kb.w / 3; x < kb.w * 2 / 3; ++x) {
            const std::size_t i = std::size_t(y * kb.w + x);
            if (kb.alpha[i] < 0.99f) continue;
            ++body;
            lo = std::min(lo, kb.lum[i]);
            hi = std::max(hi, kb.lum[i]);
        }
    CHECK(body > kb.w * kb.h / 9 / 3);
    CHECK(hi - lo > 0.3f);  // shading across the piece
    // Junk in the frame and look: nothing breaks, nothing NaN.
    render::SolidDraw j = knightDraw(20, 2);
    j.knight.x = std::nanf("");
    j.knight.heading = 1e30f;
    j.knight.squash = -4;
    j.knight.landed = 999;
    j.knightLook.flash = std::nanf("");
    j.knightLook.orbit = 1e30f;
    j.knightLook.follow = -3;
    const Box jb = drawOne(scene, solids, j);
    for (float v : jb.lum) REQUIRE(std::isfinite(v));
}

TEST_CASE("solids: knight: cost at 1920x1080 with the whole web; no render-thread allocations") {
    if (!device()) return;
    const int W = 1920, H = 1080;
    render::ScenePipeline scene(device(), W, H, {});
    render::SolidPipelineOptions o;
    o.maxLive = 1;
    render::SolidPipeline solids(scene, o);
    MTL::CommandQueue* q = device()->newCommandQueue();
    double sum = 0;
    int n = 0;
    std::uint64_t allocs0 = 0;
    for (int f = 0; f < 80; ++f) {
        if (f == 20) allocs0 = perf::renderAllocs().load();
        NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
        render::SolidDraw d = knightDraw(63.0 + 0.01 * (f % 90), 1);
        d.pose.scale = 1.3;
        d.pose.rotX = -0.98;
        d.knightLook.orbit = 0.02f * float(f);
        MTL::CommandBuffer* cmd = q->commandBuffer();
        REQUIRE(solids.encode(cmd, 0, d).visible);
        cmd->commit();
        cmd->waitUntilCompleted();
        REQUIRE(cmd->status() != MTL::CommandBufferStatusError);
        if (f >= 20) {
            sum += (cmd->GPUEndTime() - cmd->GPUStartTime()) * 1000.0;
            ++n;
        }
        pool->release();
    }
    MESSAGE("1920x1080 knight (board, 64-hop web, piece; msaa " << solids.msaa() << "): solid pass " << sum / n
            << " ms GPU");
    PERF_BUDGET(sum / n < 1.0);
    CHECK(perf::renderAllocs().load() == allocs0);
    q->release();
}

// ---------------------------------------------------------------- the soma solid (docs/3d-design.md "Soma cube")

namespace {
render::SolidDraw somaDraw(double turn, double seed = 3, double scale = 0.43) {
    render::SolidDraw d;
    d.mesh = engine::MeshKind::Soma;
    d.pose.rotX = 0.5;
    d.pose.rotY = 0.8;
    d.pose.y = 0.07;
    d.pose.scale = scale;  // content: .43 while the figure stands complete, .36 for the burst, ring and flights
    d.shading.shade = 0.8f;
    d.pieces = {0.2f, 0.32f, 0.44f, 0.56f, 0.68f, 0.8f, 0.92f};
    engine::soma::Player player({0});
    d.soma = player.frame(seed, turn, engine::soma::Timing{}, 0.0, 0.0, {d.pose.rotY, d.pose.rotX});
    return d;
}
}  // namespace

TEST_CASE("solids: soma: the complete cube shows the pieces in their colors, shaded and grooved; exploded fits its box") {
    if (!device()) return;
    const int W = 960, H = 540;
    render::ScenePipeline scene(device(), W, H, {});
    render::SolidPipelineOptions o;
    o.maxLive = 1;
    render::SolidPipeline solids(scene, o);
    const render::SolidDraw done = somaDraw(4.0);  // the hold: the cube complete
    render::SolidQuad cq;
    const Box b = drawOne(scene, solids, done, &cq);
    std::array<int, 7> n{};
    int covered = 0, other = 0, grooves = 0;
    for (std::size_t i = 0; i < b.lum.size(); ++i) {
        if (b.alpha[i] < 0.99f) continue;
        ++covered;
        CHECK(b.share[i] > 0.99f);  // all color, no plastic
        bool hit = false;
        for (int k = 0; k < 7; ++k)
            if (std::fabs(b.lum[i] - done.pieces[std::size_t(k)]) < 0.006f) ++n[std::size_t(k)], hit = true;
        other += !hit;
        grooves += b.shade[i] < 0.45f;
    }
    int shown = 0;
    for (int k = 0; k < 7; ++k) shown += n[std::size_t(k)] > 300;
    MESSAGE("soma cube: " << covered << " px covered, " << shown << " of 7 pieces in view, " << grooves << " groove px");
    CHECK(shown >= 5);           // from a 3/4 view most pieces show (one can hide inside or at the back)
    CHECK(other * 50 < covered);  // flat colors: the intensity is the piece's (shading goes to alpha)
    CHECK(grooves > 300);         // the grooves between pieces
    CHECK(grooves * 8 < covered);
    // Tops are lit brighter than the sides (shading, not hue).
    float hi = 0, lo = 1;
    for (std::size_t i = 0; i < b.lum.size(); ++i)
        if (b.alpha[i] >= 0.99f) hi = std::max(hi, b.shade[i]), lo = std::min(lo, b.shade[i]);
    CHECK(hi > 0.85f);
    CHECK(lo < 0.5f);
    // Exploded (hovering): wider than the cube, and all of it inside the quad.
    render::SolidQuad hq;
    const Box h = drawOne(scene, solids, somaDraw(14.0, 3, 0.36), &hq);
    CHECK(hq.w > 1.6f * cq.w);
    for (int x = 0; x < h.w; ++x) {
        CHECK(h.alpha[std::size_t(x)] < 0.5f);
        CHECK(h.alpha[std::size_t((h.h - 1) * h.w + x)] < 0.5f);
    }
    for (int y = 0; y < h.h; ++y) {
        CHECK(h.alpha[std::size_t(y * h.w)] < 0.5f);
        CHECK(h.alpha[std::size_t(y * h.w + h.w - 1)] < 0.5f);
    }
    // The ring (in the figure's own space, a little below it) never covers the front of the figure being built: over
    // the lower half of the complete cube's screen box (at the same scale) nothing hovers. (Its back half lies
    // behind the figure.)
    render::SolidQuad small;
    drawOne(scene, solids, somaDraw(4.0, 3, 0.36), &small);
    int front = 0;
    for (int y = 0; y < h.h; ++y)
        for (int x = 0; x < h.w; ++x) {
            const float fx = hq.cx - hq.w * 0.5f + float(x), fy = hq.cy - hq.h * 0.5f + float(y);
            const bool inLowerHalf = std::fabs(fx - small.cx) < 0.5f * small.w && fy > small.cy && fy < small.cy + 0.5f * small.h;
            if (inLowerHalf) front += h.alpha[std::size_t(y * h.w + x)] > 0.5f;
        }
    CHECK(front == 0);
}

TEST_CASE("solids: soma: cost at 1080p, no render-thread allocations") {
    if (!device()) return;
    const int W = 1920, H = 1080;
    render::ScenePipeline scene(device(), W, H, {});
    scene.setField(engine::identityFlowField(W, H));
    render::SolidPipeline solids(scene, {});
    MTL::CommandQueue* q = device()->newCommandQueue();
    engine::soma::Player player({0, 1, 2});
    double sumStep = 0, sumSoma = 0;
    int n = 0;
    std::uint64_t allocs0 = 0;
    const int frames = 100, warm = 20;
    for (int f = 0; f < frames; ++f) {
        if (f == warm) allocs0 = perf::renderAllocs().load();
        NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
        render::SolidDraw d;
        d.mesh = engine::MeshKind::Soma;
        d.pose.rotX = 0.5;
        d.pose.rotY = 0.02 * f;
        d.pose.scale = 0.44;
        d.shading.shade = 0.8f;
        d.soma = player.frame(f / 50, (f % 50) * 0.64, engine::soma::Timing{}, 0.0, 0.0, {d.pose.rotY, d.pose.rotX});
        auto run = [&](bool solid) {
            MTL::CommandBuffer* cmd = q->commandBuffer();
            std::vector<render::SpriteQuad> fb;
            if (solid) {
                const render::SolidQuad sq = solids.encode(cmd, 0, d);
                if (sq.visible) {
                    render::SpriteQuad r;
                    r.texture = sq.texture;
                    r.cx = sq.cx; r.cy = sq.cy; r.w = sq.w; r.h = sq.h;
                    r.u1 = sq.u1; r.v1 = sq.v1;
                    r.pen = 1;
                    r.op = int(engine::SpriteOp::KeyedCopy);
                    fb.push_back(r);
                }
            }
            scene.encodeStep(cmd, nullptr, 0, render::FlowBlend{}, fb.data(), fb.size(), true, nullptr, 0);
            cmd->commit();
            cmd->waitUntilCompleted();
            REQUIRE(cmd->status() != MTL::CommandBufferStatusError);
            return (cmd->GPUEndTime() - cmd->GPUStartTime()) * 1000.0;
        };
        const double a = run(false), s = run(true);
        if (f >= warm) {
            sumStep += a;
            sumSoma += s;
            ++n;
        }
        pool->release();
    }
    const double soma = sumSoma / n - sumStep / n;
    MESSAGE("1920x1080: the soma (27 cubes, MSAA, the hold to the build) drawn and composited " << soma << " ms GPU");
    PERF_BUDGET(soma < 1.0);
    CHECK(perf::renderAllocs().load() == allocs0);
    q->release();
}
