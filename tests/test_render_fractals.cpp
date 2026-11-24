// Fractal Objects (PLAN 5L.14, docs/3d-design.md "Fractal zoom"): render/FractalPipeline on the GPU, offscreen.
//   - the seam: the frame at the end of a step (the camera at g o B) matches the frame at its start (B), every path
//   - the picture moves smoothly within a step (no pops: neighboring frames close), and isn't empty or full
//   - cost at 1920x1080 for every path; no render-thread allocations
// REFORCE_FRACTAL_PREVIEW=DIR also writes PNGs of every path through a step (for looking, never committed).
#include <doctest/doctest.h>

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "PerfBudget.hpp"
#include "engine/FlowFieldBuilder.hpp"
#include "engine/FractalZoom.hpp"
#include "engine/Perf.hpp"
#include "render/FractalPipeline.hpp"
#include "render/ImageFiles.hpp"
#include "render/ScenePipeline.hpp"
#include "render/SolidPipeline.hpp"

using namespace reforce;

namespace {

MTL::Device* device() {
    static MTL::Device* d = MTL::CreateSystemDefaultDevice();
    return d;
}

struct Rig {
    int W, H;
    render::ScenePipeline scene;
    render::SolidPipeline solids;
    render::FractalPipeline fractals;
    MTL::CommandQueue* q;
    Rig(int w, int h)
        : W(w), H(h), scene(device(), w, h, {}), solids(scene, opts()), fractals(solids), q(device()->newCommandQueue()) {}
    ~Rig() { q->release(); }
    static render::SolidPipelineOptions opts() {
        render::SolidPipelineOptions so;
        so.maxLive = 1;
        return so;
    }
    // Renders and reads back the sub-rectangle: premultiplied luminance (r) and alpha (g) per pixel.
    std::vector<std::uint8_t> render(const render::FractalDraw& d, double* gpuMs = nullptr) {
        MTL::CommandBuffer* cmd = q->commandBuffer();
        const render::SolidQuad sq = fractals.encode(cmd, d);
        cmd->commit();
        cmd->waitUntilCompleted();
        REQUIRE(sq.visible);
        REQUIRE(cmd->status() != MTL::CommandBufferStatusError);
        if (gpuMs) *gpuMs = (cmd->GPUEndTime() - cmd->GPUStartTime()) * 1000.0;
        const auto [w, h] = fractals.renderSize(d.res);
        std::vector<std::uint8_t> px(std::size_t(w * h * 4));
        MTL::Buffer* buf = device()->newBuffer(NS::UInteger(w * h * 4), MTL::ResourceStorageModeShared);
        cmd = q->commandBuffer();
        MTL::BlitCommandEncoder* blit = cmd->blitCommandEncoder();
        blit->copyFromTexture(solids.target(0), 0, 0, MTL::Origin(0, 0, 0), MTL::Size(NS::UInteger(w), NS::UInteger(h), 1),
                              buf, 0, NS::UInteger(w * 4), NS::UInteger(w * h * 4));
        blit->endEncoding();
        cmd->commit();
        cmd->waitUntilCompleted();
        std::memcpy(px.data(), buf->contents(), px.size());
        buf->release();
        return px;
    }
};

render::FractalDraw drawFor(const engine::FractalPath& p, double u, const engine::FractalSway& sway = {}) {
    render::FractalDraw d;
    d.kind = p.kind;
    d.g = p.g;
    d.cam = engine::fractalCamera(p, u, sway);
    d.slot = 0;
    d.fog = float(p.fog);
    return d;
}

// Mean absolute difference of the premultiplied luminance (0..255), and the share of pixels off by more than 24.
std::pair<double, double> compare(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
    double sum = 0;
    long off = 0;
    const std::size_t n = a.size() / 4;
    for (std::size_t i = 0; i < n; ++i) {
        const int d = std::abs(int(a[i * 4]) - int(b[i * 4]));
        sum += d;
        off += d > 24;
    }
    return {sum / double(n), double(off) / double(n)};
}

void writePreview(const std::string& path, const std::vector<std::uint8_t>& px, int w, int h) {
    std::vector<std::uint8_t> bgra(px.size());
    for (std::size_t i = 0; i < px.size() / 4; ++i) {
        const std::uint8_t l = px[i * 4];
        bgra[i * 4] = bgra[i * 4 + 1] = bgra[i * 4 + 2] = l;
        bgra[i * 4 + 3] = 255;
    }
    render::writePngBgra8(path, bgra.data(), w, h, std::size_t(w) * 4);
}

}  // namespace

TEST_CASE("fractals: the frame at the end of a zoom step matches the frame at its start, for every path") {
    if (!device()) return;
    Rig rig(640, 360);
    const char* preview = std::getenv("REFORCE_FRACTAL_PREVIEW");
    for (int i = 0; i < engine::fractalPathCount(); ++i) {
        const engine::FractalPath& p = engine::fractalPath(i);
        const std::string name = std::string(engine::fractalKindName(p.kind)) + " " + p.name;
        CAPTURE(name);
        engine::FractalSway sway;
        sway.orbit = 0.07;
        sway.lookX = 0.03;
        const auto start = rig.render(drawFor(p, 0.0, sway));
        const auto end = rig.render(drawFor(p, 1.0, sway));            // g o B: the same view
        const auto before = rig.render(drawFor(p, 1.0 - 1e-4, sway));  // a frame's motion before the reset
        const auto mid = rig.render(drawFor(p, 0.5, sway));
        CHECK(compare(start, rig.render(drawFor(p, 0.0, sway))).first == 0.0);  // the same frame twice
        const auto [w, h] = rig.fractals.renderSize(0.5f);
        // Coverage: the fractal fills a good part of the frame but not all of it in one flat value.
        long covered = 0, bright = 0;
        for (std::size_t k = 0; k < start.size() / 4; ++k) {
            covered += start[k * 4 + 1] > 128;
            bright += start[k * 4] > 100;
        }
        const double n = double(start.size() / 4);
        CHECK(covered > n * 0.05);
        CHECK(bright > n * 0.02);
        const auto [mEnd, offEnd] = compare(start, end);
        const auto [mBefore, offBefore] = compare(start, before);
        const auto [mMid, offMid] = compare(start, mid);
        MESSAGE(name << ": seam mean |diff| " << mEnd << " (" << offEnd * 100
                       << "% > 24), 1e-4 before " << mBefore << ", mid-step " << mMid);
        CHECK(mEnd < 0.6);
        CHECK(offEnd < 0.003);
        CHECK(mBefore < 1.5);
        CHECK(mMid > 4 * std::max(mEnd, 0.5));  // the picture does move within a step
        if (preview) {
            const std::string base = std::string(preview) + "/" + engine::fractalKindName(p.kind) + "-" + p.name;
            for (int k = 0; k < 8; ++k) {
                const double u = k / 8.0;
                writePreview(base + "-u" + std::to_string(k) + ".png", rig.render(drawFor(p, u, sway)), w, h);
            }
            writePreview(base + "-end.png", end, w, h);
        }
    }
}

TEST_CASE("fractals: cost at 1920x1080 through a whole step, every path; no render-thread allocations") {
    if (!device()) return;
    Rig rig(1920, 1080);
    rig.scene.setField(engine::identityFlowField(1920, 1080));
    double worst = 0;
    for (int i = 0; i < engine::fractalPathCount(); ++i) {
        const engine::FractalPath& p = engine::fractalPath(i);
        const std::string name = std::string(engine::fractalKindName(p.kind)) + " " + p.name;
        double sum = 0, peak = 0;
        int n = 0;
        std::uint64_t allocs0 = 0;
        const int frames = 40, warm = 4;
        for (int f = 0; f < frames; ++f) {
            if (f == warm) allocs0 = perf::renderAllocs().load();
            NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
            engine::FractalSway sway;
            sway.orbit = 0.1 * std::sin(f * 0.3);
            const render::FractalDraw d = drawFor(p, double(f % 20) / 20.0, sway);
            MTL::CommandBuffer* cmd = rig.q->commandBuffer();
            rig.fractals.encode(cmd, d);
            cmd->commit();
            cmd->waitUntilCompleted();
            REQUIRE(cmd->status() != MTL::CommandBufferStatusError);
            const double ms = (cmd->GPUEndTime() - cmd->GPUStartTime()) * 1000.0;
            if (f >= warm) {
                sum += ms;
                peak = std::max(peak, ms);
                ++n;
            }
            pool->release();
        }
        CHECK(perf::renderAllocs().load() == allocs0);
        MESSAGE(name << ": " << sum / n << " ms GPU mean, " << peak << " ms peak (1920x1080, Res .5, Detl 2.5)");
        worst = std::max(worst, sum / n);
    }
    PERF_BUDGET(worst < 4.0);
}

TEST_CASE("fractals: large previews (REFORCE_FRACTAL_PREVIEW only)") {
    const char* dir = std::getenv("REFORCE_FRACTAL_PREVIEW");
    if (!device() || !dir) return;
    Rig rig(1920, 1080);
    const auto [w, h] = rig.fractals.renderSize(0.5f);
    for (int i = 0; i < engine::fractalPathCount(); ++i) {
        const engine::FractalPath& p = engine::fractalPath(i);
        for (double u : {0.0, 0.5, -1.0}) {
            auto dd = drawFor(p, std::max(u, 0.0));
            if (u < 0) dd.detail = 1.0f;  // (1 px: the moire rings that made 2.5 the default)
            writePreview(std::string(dir) + "/large-" + engine::fractalKindName(p.kind) + "-" + p.name + "-" +
                             (u > 0 ? "half" : u < 0 ? "start-detl1" : "start") + ".png",
                         rig.render(dd), w, h);
        }
    }
}
