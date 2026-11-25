// Rain on glass (docs/3d-design.md "Rain on glass"): render/RainPipeline on the GPU, offscreen.
//   - a bead is a lens: over a frame bright above and dark below, it shows the bright half at its bottom and the
//     dark half at its top (upside down); the frame away from the drop is untouched
//   - the fields start cleared (a composite before any step reads no undefined memory), the window fog frosts and a
//     wipe clears it
//   - the floor's waves stay finite and bounded over a long run with impulses, and spread from where drops struck
//   - cost at 1920x1080 (window with a full pane of drops, floor with waves)
#include <doctest/doctest.h>

#include "PerfBudget.hpp"

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "engine/Rain.hpp"
#include "render/ImageFiles.hpp"
#include "render/RainPipeline.hpp"
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
    render::RainPipeline rain;
    MTL::CommandQueue* q;
    MTL::Texture* out;
    std::vector<std::uint8_t> px;
    static render::SolidPipelineOptions opts() {
        render::SolidPipelineOptions o;
        o.maxLive = 1;
        return o;
    }
    Rig(int w, int h)
        : W(w), H(h), scene(device(), uint32_t(w), uint32_t(h), {}), solids(scene, opts()), rain(solids),
          q(device()->newCommandQueue()), px(std::size_t(w * h * 4)) {
        MTL::TextureDescriptor* td = MTL::TextureDescriptor::texture2DDescriptor(
            MTL::PixelFormatBGRA8Unorm, NS::UInteger(W), NS::UInteger(H), false);
        td->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
        td->setStorageMode(MTL::StorageModeShared);
        out = device()->newTexture(td);
    }
    ~Rig() {
        out->release();
        q->release();
    }
    // The frame: bright (200) above `split` (a share of the height), dark (20) below.
    void fill(double split) {
        std::vector<std::uint8_t> init(std::size_t(W * H * 4));
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const std::uint8_t v = y < split * H ? 200 : 20;
                std::uint8_t* p = &init[std::size_t((y * W + x) * 4)];
                p[0] = p[1] = p[2] = v;
                p[3] = 255;
            }
        out->replaceRegion(MTL::Region(0, 0, NS::UInteger(W), NS::UInteger(H)), 0, init.data(), NS::UInteger(W * 4));
    }
    void read() {
        out->getBytes(px.data(), NS::UInteger(W * 4), MTL::Region(0, 0, NS::UInteger(W), NS::UInteger(H)), 0);
    }
    int at(int x, int y) const { return px[std::size_t((y * W + x) * 4 + 1)]; }
};

}  // namespace

TEST_CASE("rain render: a bead shows the visuals upside down; the frame elsewhere is untouched") {
    if (!device()) return;
    Rig r(640, 360);
    engine::rain::Settings st;
    st.slide = 1000;
    engine::rain::Sim sim(st, 640.0 / 360.0, 0.0);
    const double rad = 40.0 / 360.0;  // 40 px
    sim.place(0.5 * 640.0 / 360.0, 0.5, rad);
    engine::rain::Controls quiet;
    quiet.density = quiet.drizzle = quiet.cluster = 0;
    sim.step(0.0, quiet, {});
    render::RainLook look;
    look.fog = 0;
    look.depth = 0.5f;
    r.fill(0.5);
    MTL::CommandBuffer* cmd = r.q->commandBuffer();
    r.rain.encodeStep(cmd, sim, look, true);
    r.rain.encodeComposite(cmd, r.out, sim, look, false);
    cmd->commit();
    cmd->waitUntilCompleted();
    r.read();
    const int cx = 320, cy = 180;
    // Inside: the top of the bead shows the dark lower half, the bottom shows the bright upper half.
    double top = 0, bottom = 0;
    for (int dy = 8; dy <= 20; ++dy)
        for (int dx = -6; dx <= 6; ++dx) {
            top += r.at(cx + dx, cy - dy);
            bottom += r.at(cx + dx, cy + dy);
        }
    top /= 13 * 13;
    bottom /= 13 * 13;
    CHECK(top < 80);
    CHECK(bottom > 110);
    // Far from the drop the frame is as it was.
    CHECK(r.at(20, 20) == 200);
    CHECK(r.at(600, 340) == 20);
    CHECK(r.at(cx + 120, cy - 60) == 200);
    // Just outside the contact line, over the bright half: the halo darkens it a little.
    CHECK(r.at(cx, cy - 44) < 200);
    CHECK(r.at(cx, cy - 44) > 120);
}

TEST_CASE("rain render: fields start cleared; the window fog frosts and a running drop wipes it") {
    if (!device()) return;
    Rig r(640, 360);
    // A composite before any step: nothing undefined is read (poison mode checks the float targets), the frame
    // without drops or fog is untouched.
    {
        engine::rain::Settings st;
        st.mode = engine::rain::Mode::Floor;
        engine::rain::Sim sim(st, 640.0 / 360.0, 0.0);
        render::RainLook look;
        look.mode = engine::rain::Mode::Floor;
        look.wet = 1.0f;
        r.fill(0.5);
        MTL::CommandBuffer* cmd = r.q->commandBuffer();
        r.rain.encodeComposite(cmd, r.out, sim, look, false);
        cmd->commit();
        cmd->waitUntilCompleted();
        r.read();
        CHECK(r.at(100, 100) == 200);
        CHECK(r.at(100, 300) == 20);
    }
    // Window fog over a fine pattern (stripes): it softens them, and a wipe leaves them sharp in its streak.
    engine::rain::Settings st;
    st.slide = 1000;
    engine::rain::Sim sim(st, 640.0 / 360.0, 0.0);
    render::RainLook look;
    look.fog = 1.0f;
    look.frost = 12.0f;
    std::vector<std::uint8_t> init(std::size_t(640 * 360 * 4));
    for (int y = 0; y < 360; ++y)
        for (int x = 0; x < 640; ++x) {
            std::uint8_t* p = &init[std::size_t((y * 640 + x) * 4)];
            p[0] = p[1] = p[2] = (x / 4) % 2 ? 230 : 10;
            p[3] = 255;
        }
    engine::rain::Controls quiet;
    quiet.density = quiet.drizzle = quiet.cluster = 0;
    MTL::CommandBuffer* cmd = r.q->commandBuffer();
    sim.step(0.0, quiet, {});
    r.rain.encodeStep(cmd, sim, look, true);
    r.out->replaceRegion(MTL::Region(0, 0, 640, 360), 0, init.data(), 640 * 4);
    r.rain.encodeComposite(cmd, r.out, sim, look, false);
    cmd->commit();
    cmd->waitUntilCompleted();
    r.read();
    auto contrast = [&](int x0, int y) {
        int lo = 255, hi = 0;
        for (int x = x0; x < x0 + 16; ++x) {
            lo = std::min(lo, r.at(x, y));
            hi = std::max(hi, r.at(x, y));
        }
        return hi - lo;
    };
    CHECK(contrast(300, 180) < 60);  // frosted
    // A drop big enough to run, from the top: it slides down x = 1 frame height and wipes a streak.
    engine::rain::Settings run = st;
    run.slide = 5;
    engine::rain::Sim s2(run, 640.0 / 360.0, 0.0);
    s2.place(300.0 / 360.0 + 8.0 / 360.0, 0.05, 30.0 / 1080.0 * 3.0);
    s2.step(0.0, quiet, {});
    cmd = r.q->commandBuffer();
    r.rain.encodeStep(cmd, s2, look, true);
    double mx = 0, my = 0;  // where the drop was half way
    for (int i = 1; i <= 90; ++i) {
        s2.step(i / 60.0, quiet, {});
        r.rain.encodeStep(cmd, s2, look, false);
        if (i == 45 && !s2.drops().empty()) {
            mx = s2.drops().front().x;
            my = s2.drops().front().y;
        }
    }
    r.out->replaceRegion(MTL::Region(0, 0, 640, 360), 0, init.data(), 640 * 4);
    r.rain.encodeComposite(cmd, r.out, s2, look, false);
    cmd->commit();
    cmd->waitUntilCompleted();
    r.read();
    REQUIRE_FALSE(s2.drops().empty());
    const engine::rain::Drop& d = s2.drops().front();
    CHECK(d.y > 0.2);  // it ran
    // Where it passed half way: sharp stripes again (the streak); beside the path still frosted.
    REQUIRE(d.y > my + 0.1);
    const int sx = int(std::lround(mx * 360.0)), sy = int(std::lround(my * 360.0));
    int best = 0;
    for (int dy = -6; dy <= 6; dy += 3) best = std::max(best, contrast(std::max(0, sx - 8), sy + dy));
    CHECK(best > 120);
    CHECK(contrast(sx < 320 ? 560 : 40, 300) < 60);
}

TEST_CASE("rain render: the floor's waves stay finite and bounded, and spread from the strikes") {
    if (!device()) return;
    Rig r(480, 270);
    engine::rain::Settings st;
    st.mode = engine::rain::Mode::Floor;
    st.seed = 4;
    engine::rain::Sim sim(st, 480.0 / 270.0, 0.0);
    engine::rain::Controls c;
    c.density = 12;
    c.drizzle = 4;
    c.wet = 1.0;
    render::RainLook look;
    look.mode = engine::rain::Mode::Floor;
    look.wet = 1.0f;
    look.speed = 2.0f;   // the fastest waves (most substeps)
    look.decay = 30.0f;  // the slowest decay
    const int cols = r.rain.simCols(), rows = r.rain.simRows();
    std::vector<float> h(std::size_t(cols * rows));
    double peak = 0;
    int impulses = 0;
    for (int batch = 0; batch < 20; ++batch) {
        MTL::CommandBuffer* cmd = r.q->commandBuffer();
        for (int i = 0; i < 60; ++i) {
            const double t = (batch * 60 + i) / 30.0;  // 30 fps: big steps
            engine::rain::Clock k;
            k.on = true;
            k.beat = 2 * t;
            k.bar = k.beat / 4;
            k.phrase = k.bar / 16;
            sim.step(t, c, k);
            impulses += int(sim.impulses().size());
            r.rain.encodeStep(cmd, sim, look, batch == 0 && i == 0);
        }
        cmd->commit();
        cmd->waitUntilCompleted();
        // Read the wave heights back.
        MTL::Buffer* buf = device()->newBuffer(NS::UInteger(cols * rows * 4), MTL::ResourceStorageModeShared);
        cmd = r.q->commandBuffer();
        MTL::BlitCommandEncoder* blit = cmd->blitCommandEncoder();
        blit->copyFromTexture(r.rain.waveTexture(), 0, 0, MTL::Origin(0, 0, 0),
                              MTL::Size(NS::UInteger(cols), NS::UInteger(rows), 1), buf, 0, NS::UInteger(cols * 4),
                              NS::UInteger(cols * rows * 4));
        blit->endEncoding();
        cmd->commit();
        cmd->waitUntilCompleted();
        const float* p = static_cast<const float*>(buf->contents());
        double m = 0;
        bool finite = true;
        for (int i = 0; i < cols * rows; ++i) {
            finite = finite && std::isfinite(p[i]);
            m = std::max(m, double(std::fabs(p[i])));
        }
        buf->release();
        REQUIRE(finite);
        peak = std::max(peak, m);
    }
    CHECK(impulses > 100);
    CHECK(peak > 0.05);   // waves
    CHECK(peak < 200.0);  // bounded
}

TEST_CASE("rain render: the floor's puddles are drawn where the sim has them, after their edges crept") {
    if (!device()) return;
    Rig r(640, 360);
    engine::rain::Settings st;
    st.mode = engine::rain::Mode::Floor;
    st.seed = 9;
    engine::rain::Sim sim(st, 640.0 / 360.0, 0.0);
    engine::rain::Controls c;
    c.density = c.drizzle = c.cluster = 0;  // no rain: only the puddles
    c.wet = 0.5;
    for (int i = 1; i <= 25 * 60; ++i) sim.step(i / 60.0, c, {});
    REQUIRE(sim.puddleTime() > 24.0);
    render::RainLook look;
    look.mode = engine::rain::Mode::Floor;
    look.wet = 0.5f;
    r.fill(2.0);  // all 200
    MTL::CommandBuffer* cmd = r.q->commandBuffer();
    r.rain.encodeStep(cmd, sim, look, true);
    r.rain.encodeComposite(cmd, r.out, sim, look, false);
    cmd->commit();
    cmd->waitUntilCompleted();
    r.read();
    // Inside a puddle a little darker, outside untouched; compared away from the edges (the rim, the lens).
    const std::uint32_t seed32 = std::uint32_t(st.seed ^ (st.seed >> 32));
    const double thr = engine::rain::puddleThreshold(sim.puddleWet());
    int checked = 0, agree = 0;
    for (int y = 4; y < 360; y += 7)
        for (int x = 4; x < 640; x += 7) {
            const double n = engine::rain::puddleField(x / 360.0, y / 360.0, sim.puddleTime(), seed32);
            if (std::abs(n - thr) < 0.04) continue;
            ++checked;
            agree += (n > thr) == (r.at(x, y) < 190);
        }
    REQUIRE(checked > 2000);
    CHECK(agree >= checked * 0.99);
    // Not the puddles of t = 0: the edges moved.
    int moved = 0;
    for (int y = 4; y < 360; y += 7)
        for (int x = 4; x < 640; x += 7)
            moved += (engine::rain::puddle(x / 360.0, y / 360.0, 0.5, seed32, 0.0) > 0.5) != (r.at(x, y) < 190);
    CHECK(moved > 20);

    // REFORCE_RAIN_PREVIEW=<dir>: a rainy floor over stripes at a few moments, as PNGs, to look at.
    if (const char* dir = std::getenv("REFORCE_RAIN_PREVIEW")) {
        engine::rain::Sim rainy(st, 640.0 / 360.0, 0.0);
        engine::rain::Controls rc;
        rc.density = 3;
        rc.size = 9;
        rc.drizzle = 0.6;
        rc.wet = 0.65;
        look.wet = 0.65f;
        bool first = true;
        for (int i = 1; i <= 40 * 60; ++i) {
            const double t = i / 60.0;
            engine::rain::Clock k;
            k.on = true;
            k.beat = t * 2.0;
            k.bar = k.beat / 4;
            k.phrase = k.beat / 128;
            k.drop = t > 20 && t < 32 ? 1.0 : 0.0;
            rainy.step(t, rc, k);
            MTL::CommandBuffer* cb = r.q->commandBuffer();
            r.rain.encodeStep(cb, rainy, look, first);
            first = false;
            if (i % 300 == 0) {
                std::vector<std::uint8_t> init(std::size_t(r.W * r.H * 4));
                for (int y = 0; y < r.H; ++y)
                    for (int x = 0; x < r.W; ++x) {
                        std::uint8_t* q = &init[std::size_t((y * r.W + x) * 4)];
                        const bool on = ((x / 24) + (y / 24)) % 2 == 0;
                        q[0] = on ? 60 : 210;
                        q[1] = on ? 120 : 190;
                        q[2] = on ? 200 : 80;
                        q[3] = 255;
                    }
                r.out->replaceRegion(MTL::Region(0, 0, NS::UInteger(r.W), NS::UInteger(r.H)), 0, init.data(),
                                     NS::UInteger(r.W * 4));
                r.rain.encodeComposite(cb, r.out, rainy, look, false);
            }
            cb->commit();
            cb->waitUntilCompleted();
            if (i % 300 == 0) {
                r.read();
                render::writePngBgra8(std::string(dir) + "/rain_floor_" + std::to_string(i / 60) + "s.png",
                                      r.px.data(), r.W, r.H, std::size_t(r.W) * 4);
            }
        }
    }
}

TEST_CASE("rain render: cost at 1920x1080") {
    if (!device()) return;
    Rig r(1920, 1080);
    r.fill(0.5);
    auto measure = [&](engine::rain::Mode mode) {
        engine::rain::Settings st;
        st.mode = mode;
        st.seed = 8;
        engine::rain::Sim sim(st, 1920.0 / 1080.0, 0.0);
        engine::rain::Controls c;
        c.density = 8;
        c.drizzle = 2;
        render::RainLook look;
        look.mode = mode;
        double best = 1e9;
        bool first = true;
        for (int f = 1; f <= 60 * 30; ++f) {
            const double t = f / 60.0;
            engine::rain::Clock k;
            k.on = true;
            k.beat = 2 * t;
            k.bar = k.beat / 4;
            k.phrase = k.bar / 16;
            sim.step(t, c, k);
            if (f < 60 * 30 - 8 && f % 4 != 0) continue;
            MTL::CommandBuffer* cmd = r.q->commandBuffer();
            r.rain.encodeStep(cmd, sim, look, first);
            first = false;
            r.rain.encodeComposite(cmd, r.out, sim, look, false);
            cmd->commit();
            cmd->waitUntilCompleted();
            if (f >= 60 * 30 - 8) best = std::min(best, (cmd->GPUEndTime() - cmd->GPUStartTime()) * 1000.0);
        }
        MESSAGE(std::string(engine::rain::modeName(mode)) << ": " << sim.drops().size() << " drops, " << best << " ms GPU");
        return best;
    };
    const double w = measure(engine::rain::Mode::Window);
    const double f = measure(engine::rain::Mode::Floor);
    PERF_BUDGET(w < 3.0);  // measured ~0.6 ms (M-series); loose here (shared GPU)
    PERF_BUDGET(f < 3.0);
}
