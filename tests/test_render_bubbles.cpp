// Bubbles (docs/3d-design.md "Bubbles"): render/BubblePipeline on the GPU, offscreen.
//   - the film reflectance (CPU copy of the shader's): black when thinner than a quarter wave, the Newton series of
//     colors as it thickens, white-ish when thick, a mirror at grazing incidence
//   - a bubble is nearly transparent in the middle and bright at the rim; the frame away from it is untouched;
//     interference colors are there (over black too: the room and the window light it)
//   - a pop: the hole around the tear shows the frame as it was, the rest of the film is still there
//   - a cluster's shared wall is drawn as a bright line between the two
//   - junk looks draw sanely; nothing is drawn (or copied) without bubbles
//   - cost at 1920x1080 with Max bubbles and a spray; no render-thread allocations
#include <doctest/doctest.h>

#include "PerfBudget.hpp"

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "engine/Bubbles.hpp"
#include "engine/Perf.hpp"
#include "render/BubblePipeline.hpp"
#include "render/ImageFiles.hpp"
#include "render/ScenePipeline.hpp"
#include "render/SolidPipeline.hpp"

using namespace reforce;
namespace bub = reforce::engine::bubbles;

namespace {

MTL::Device* device() {
    static MTL::Device* d = MTL::CreateSystemDefaultDevice();
    return d;
}

struct Rig {
    int W, H;
    render::ScenePipeline scene;
    render::SolidPipeline solids;
    render::BubblePipeline bubbles;
    MTL::CommandQueue* q;
    MTL::Texture* out;
    std::vector<std::uint8_t> px, base;
    static render::SolidPipelineOptions opts() {
        render::SolidPipelineOptions o;
        o.maxLive = 1;
        return o;
    }
    Rig(int w, int h)
        : W(w), H(h), scene(device(), uint32_t(w), uint32_t(h), {}), solids(scene, opts()), bubbles(solids),
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
    template <class F>
    void fill(F f) {
        base.assign(std::size_t(W * H * 4), 0);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                std::uint8_t* p = &base[std::size_t((y * W + x) * 4)];
                f(x, y, p);
                p[3] = 255;
            }
        out->replaceRegion(MTL::Region(0, 0, NS::UInteger(W), NS::UInteger(H)), 0, base.data(), NS::UInteger(W * 4));
    }
    void fillGray(std::uint8_t v) {
        fill([&](int, int, std::uint8_t* p) { p[0] = p[1] = p[2] = v; });
    }
    void draw(const bub::Sim& sim, const render::BubbleLook& look) {
        MTL::CommandBuffer* cmd = q->commandBuffer();
        bubbles.encodeComposite(cmd, out, sim, look, false);
        cmd->commit();
        cmd->waitUntilCompleted();
        REQUIRE(cmd->status() != MTL::CommandBufferStatusError);
        out->getBytes(px.data(), NS::UInteger(W * 4), MTL::Region(0, 0, NS::UInteger(W), NS::UInteger(H)), 0);
    }
    // BGRA
    int b(int x, int y) const { return px[std::size_t((y * W + x) * 4 + 0)]; }
    int g(int x, int y) const { return px[std::size_t((y * W + x) * 4 + 1)]; }
    int r(int x, int y) const { return px[std::size_t((y * W + x) * 4 + 2)]; }
    int diff(int x, int y) const {
        int m = 0;
        for (int c = 0; c < 3; ++c)
            m = std::max(m, std::abs(int(px[std::size_t((y * W + x) * 4 + c)]) - int(base[std::size_t((y * W + x) * 4 + c)])));
        return m;
    }
    void save(const char* name) {
        if (const char* dir = std::getenv("REFORCE_BUBBLES_PREVIEW"))
            render::writePngBgra8(std::string(dir) + "/" + name + ".png", px.data(), W, H, std::size_t(W) * 4);
    }
};

bub::Controls quiet() {
    bub::Controls c;
    c.blow = 0;
    c.pop = 0;
    c.air = 0;
    c.rise = 0;
    c.life = 4096;
    return c;
}

// A still bubble (no air, no wobble) of radius r (frame heights) at (x, y).
bub::Sim still(double aspect, double x, double y, double r, std::uint64_t seed = 1) {
    bub::Settings st;
    st.seed = seed;
    st.cluster = 0;
    bub::Sim s(st, aspect, 0.0);
    s.step(0.0, quiet(), {});
    s.place(x, y, r);
    s.step(1.0 / 120.0, quiet(), {});
    return s;
}

}  // namespace

TEST_CASE("bubbles render: the film's reflectance follows the interference series") {
    double rgb[3];
    // Thinner than a quarter wave: the black film (no reflection).
    render::filmReflectance(5, 1.0, rgb);
    CHECK(rgb[0] + rgb[1] + rgb[2] < 0.002);
    // A few hundred nm: strongly colored (channels far apart), and the color changes with thickness.
    double maxChroma = 0;
    int hueFlips = 0;
    int last = -1;
    for (double nm = 150; nm <= 900; nm += 10) {
        render::filmReflectance(nm, 1.0, rgb);
        const double mx = std::max({rgb[0], rgb[1], rgb[2]}), mn = std::min({rgb[0], rgb[1], rgb[2]});
        maxChroma = std::max(maxChroma, (mx - mn) / std::max(mx, 1e-9));
        const int top = rgb[0] >= rgb[1] && rgb[0] >= rgb[2] ? 0 : rgb[1] >= rgb[2] ? 1 : 2;
        if (top != last) ++hueFlips;
        last = top;
        CHECK(rgb[0] <= 0.2);  // at normal incidence a soap film reflects at most ~ 15 %
    }
    CHECK(maxChroma > 0.6);
    CHECK(hueFlips >= 6);
    // Thick films wash out to white (all channels about equal).
    double avg[3] = {0, 0, 0};
    for (double nm = 6000; nm < 7000; nm += 7) {
        render::filmReflectance(nm, 1.0, rgb);
        for (int c = 0; c < 3; ++c) avg[c] += rgb[c];
    }
    CHECK(std::fabs(avg[0] - avg[1]) < 0.25 * avg[1]);
    CHECK(std::fabs(avg[2] - avg[1]) < 0.25 * avg[1]);
    // Grazing: a mirror.
    render::filmReflectance(500, 0.001, rgb);
    CHECK(rgb[1] > 0.8);
    render::filmReflectance(std::nan(""), std::nan(""), rgb);
    CHECK(std::isfinite(rgb[0]));
}

TEST_CASE("bubbles render: clear in the middle, bright at the rim, colored, the frame elsewhere untouched") {
    if (!device()) return;
    Rig r(640, 360);
    const double A = 640.0 / 360.0;
    const bub::Sim sim = still(A, 0.5 * A, 0.5, 100.0 / 360.0);  // 100 px
    render::BubbleLook look;
    look.tint = 0;
    const int cx = 320, cy = 180;
    // A dark frame with a bright ring around the bubble (115..220 px from its center): the rim mirrors the ring
    // (its reflected rays run outwards to the visuals behind), the middle looks straight through to the dark.
    r.fill([&](int x, int y, std::uint8_t* p) {
        const double d = std::hypot(x - cx, y - cy);
        p[0] = p[1] = p[2] = d > 115 && d < 220 ? 230 : 25;
    });
    r.draw(sim, look);
    r.save("bubble_ring");
    double mid = 0;
    for (int dy = -8; dy <= 8; dy += 4)
        for (int dx = -8; dx <= 8; dx += 4) mid += r.diff(cx + dx, cy + dy);
    mid /= 25;
    CHECK(mid < 20);
    int brightRim = 0;
    for (int i = 0; i < 16; ++i) {
        const double a = 2 * 3.141592653589793 * i / 16;
        const int x = cx + int(std::lround(std::cos(a) * 97)), y = cy + int(std::lround(std::sin(a) * 97));
        brightRim += (r.r(x, y) + r.g(x, y) + r.b(x, y)) / 3 > 25 + 60;
    }
    CHECK(brightRim >= 14);
    // Over a mid gray: interference colors, strongly colored pixels of different hues inside the bubble.
    r.fillGray(90);
    r.draw(sim, look);
    r.save("bubble_gray");
    mid = 0;
    for (int dy = -8; dy <= 8; dy += 4)
        for (int dx = -8; dx <= 8; dx += 4) mid += r.diff(cx + dx, cy + dy);
    CHECK(mid / 25 < 20);
    int reddish = 0, greenish = 0, bluish = 0;
    for (int y = cy - 98; y <= cy + 98; y += 2)
        for (int x = cx - 98; x <= cx + 98; x += 2) {
            if (std::hypot(x - cx, y - cy) > 96) continue;
            const int R = r.r(x, y), G = r.g(x, y), B = r.b(x, y);
            const int mx = std::max({R, G, B}), mn = std::min({R, G, B});
            if (mx - mn < 18) continue;
            reddish += R == mx;
            greenish += G == mx;
            bluish += B == mx;
        }
    MESSAGE("colored pixels: red " << reddish << ", green " << greenish << ", blue " << bluish);
    CHECK(reddish > 20);
    CHECK(greenish > 20);
    CHECK(bluish > 20);
    // Away from it the frame is exactly as it was.
    CHECK(r.diff(20, 20) == 0);
    CHECK(r.diff(620, 340) == 0);
    CHECK(r.diff(cx + 110, cy) == 0);
    // Over black the bubble still shows (the room's light and the window on the film), the middle still clear.
    r.fillGray(0);
    r.draw(sim, look);
    r.save("bubble_black");
    int lit = 0;
    for (int y = cy - 98; y <= cy + 98; y += 3)
        for (int x = cx - 98; x <= cx + 98; x += 3)
            if (std::hypot(x - cx, y - cy) < 96) lit += r.r(x, y) + r.g(x, y) + r.b(x, y) > 40;
    CHECK(lit > 200);
    CHECK(r.diff(cx, cy) < 40);
}

TEST_CASE("bubbles render: a pop opens a hole around the tear; a cluster's wall is a bright line") {
    if (!device()) return;
    Rig r(640, 360);
    const double A = 640.0 / 360.0;
    render::BubbleLook look;
    look.tint = 0;
    const int cx = 320, cy = 180;
    auto ring = [&]() {
        r.fill([&](int x, int y, std::uint8_t* p) {
            const double d = std::hypot(x - cx, y - cy);
            p[0] = p[1] = p[2] = d > 115 && d < 220 ? 230 : 25;
        });
    };
    // A bubble planned to pop on the next beat line, drawn half way through its pop, and the same bubble intact.
    bub::Settings st;
    st.seed = 4;
    bub::Sim s(st, A, 0.0), whole(st, A, 0.0);
    s.step(0.0, quiet(), {});
    whole.step(0.0, quiet(), {});
    const std::uint32_t id = s.place(0.5 * A, 0.5, 100.0 / 360.0);
    whole.place(0.5 * A, 0.5, 100.0 / 360.0);
    s.planPop(id);
    double t = 0;
    while (s.tally().popped == 0 && t < 2) {
        t += 1.0 / 120.0;
        s.step(t, quiet(), {});
        whole.step(t, quiet(), {});
    }
    REQUIRE(s.tally().popped == 1);
    while (!s.bubbles().empty() && s.bubbles()[0].pop < 0.4 * bub::kPopSeconds) {  // to about half way (by area)
        t += 1.0 / 120.0;
        s.step(t, quiet(), {});
        whole.step(t, quiet(), {});
    }
    REQUIRE(s.bubbles().size() == 1);
    const bub::Bubble& b = s.bubbles()[0];
    REQUIRE(b.pop > 0.01);
    REQUIRE(b.pop < bub::kPopSeconds);
    ring();
    r.draw(whole, look);
    std::vector<int> intact(std::size_t(r.W * r.H));
    for (int y = 0; y < r.H; ++y)
        for (int x = 0; x < r.W; ++x) intact[std::size_t(y * r.W + x)] = r.diff(x, y);
    ring();
    r.draw(s, look);
    r.save("bubble_pop");
    // Where the tear began (the screen point over it) both films are gone: the frame as it was.
    const int tx = cx + int(std::lround(b.tear[0] * 100)), ty = cy + int(std::lround(b.tear[1] * 100));
    CHECK(intact[std::size_t(ty * r.W + tx)] > 8);
    CHECK(r.diff(tx, ty) < 6);
    // Some of the film is still there (the far side), some is gone.
    int was = 0, still = 0;
    for (int y = cy - 100; y <= cy + 100; ++y)
        for (int x = cx - 100; x <= cx + 100; ++x) {
            if (std::hypot(x - cx, y - cy) > 99) continue;
            was += intact[std::size_t(y * r.W + x)] > 8;
            still += r.diff(x, y) > 8;
        }
    MESSAGE("film pixels: intact " << was << ", half way through the pop " << still);
    CHECK(still > was / 8);
    CHECK(still < was * 9 / 10);
    // It ends within kPopSeconds, leaving the spray.
    for (int i = 0; i < 12; ++i) {
        t += 1.0 / 120.0;
        s.step(t, quiet(), {});
    }
    CHECK(s.bubbles().empty());
    CHECK_FALSE(s.droplets().empty());

    // Two bubbles stuck together: a bright wall where they meet.
    bub::Settings cs;
    cs.cluster = 1;
    bub::Sim c(cs, A, 0.0);
    c.step(0.0, quiet(), {});
    c.place(0.5 * A - 0.12, 0.5, 0.2);
    c.place(0.5 * A + 0.12, 0.5, 0.2);
    for (int i = 1; i <= 600; ++i) c.step(i / 120.0, quiet(), {});
    REQUIRE(c.tally().bonds == 1);
    REQUIRE(c.bubbles().size() == 2);
    const bub::Bubble& p = c.bubbles()[0];
    const bub::Bubble& q = c.bubbles()[1];
    // The wall: the plane where the spheres meet (equal radii: half way), the circle's half length.
    const double wx = 0.5 * (p.x + q.x) * 360.0, wy = 0.5 * (p.y + q.y) * 360.0;
    r.fillGray(40);
    r.draw(c, look);
    r.save("bubble_wall");
    int wallBright = 0;
    for (int dy = -40; dy <= 40; dy += 8) wallBright += r.r(int(wx), int(wy) + dy) + r.g(int(wx), int(wy) + dy) > 2 * 40 + 60;
    CHECK(wallBright >= 7);
    // Beside the wall, inside a bubble: clear.
    CHECK(r.diff(int(p.x * 360.0) - 10, int(p.y * 360.0)) < 25);
}

TEST_CASE("bubbles render: junk looks draw sanely; nothing without bubbles") {
    if (!device()) return;
    Rig r(320, 180);
    const double A = 320.0 / 180.0;
    const double nan = std::nan("");
    render::BubbleLook look;
    look.film = float(nan);
    look.iris = float(1e30);
    look.swirl = -5.0f;
    look.drain = float(nan);
    look.bend = float(1e30);
    look.spec = float(nan);
    look.rim = float(nan);
    look.depth = float(-1e30);
    look.tint = float(nan);
    look.tintColor[0] = float(nan);
    const bub::Sim sim = still(A, 0.5 * A, 0.5, 0.3);
    r.fillGray(90);
    r.draw(sim, look);
    CHECK(r.diff(5, 5) == 0);
    int changed = 0;
    for (int y = 40; y < 140; y += 5)
        for (int x = 110; x < 210; x += 5) changed += r.diff(x, y) > 3;
    CHECK(changed > 10);
    // A bubble with junk in its own state is still drawn finitely (no NaN outline swallowing the frame).
    // Without bubbles: untouched.
    bub::Settings st;
    bub::Sim none(st, A, 0.0);
    none.step(0.0, quiet(), {});
    r.fillGray(77);
    r.draw(none, render::BubbleLook{});
    CHECK(r.diff(160, 90) == 0);
}

TEST_CASE("bubbles render: cost at 1920x1080; no render-thread allocations") {
    if (!device()) return;
    Rig r(1920, 1080);
    r.fill([](int x, int y, std::uint8_t* p) {
        p[0] = std::uint8_t(40 + (x * 7) % 160);
        p[1] = std::uint8_t(60 + (y * 3) % 150);
        p[2] = std::uint8_t(((x / 64 + y / 64) % 2) ? 200 : 30);
    });
    // Bubbles Drift's crowd (40 of 55 px) and a few giants (7 of 150 px, Src air), popping and spraying.
    auto measure = [&](const char* name, bub::Source src, int maxBubbles, double blow, double size, double pop) {
        bub::Settings st;
        st.seed = 8;
        st.source = src;
        st.maxBubbles = maxBubbles;
        bub::Sim sim(st, 1920.0 / 1080.0, 0.0);
        bub::Controls c;
        c.blow = blow;
        c.size = size;
        c.pop = pop;
        render::BubbleLook look;
        double best = 1e9, sum = 0;
        int n = 0;
        std::uint64_t allocs0 = 0;
        std::size_t most = 0, spray = 0;
        const int frames = 60 * 24, warm = frames - 60;
        for (int f = 1; f <= frames; ++f) {
            const double t = f / 60.0;
            bub::Clock k;
            k.on = true;
            k.beat = 2 * t;
            k.bar = k.beat / 4;
            k.phrase = k.bar / 16;
            sim.step(t, c, k);
            most = std::max(most, sim.bubbles().size());
            spray = std::max(spray, sim.droplets().size());
            if (f < warm) continue;
            if (f == warm) allocs0 = perf::renderAllocs().load();
            NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
            MTL::CommandBuffer* cmd = r.q->commandBuffer();
            r.bubbles.encodeComposite(cmd, r.out, sim, look, false);
            cmd->commit();
            cmd->waitUntilCompleted();
            const double ms = (cmd->GPUEndTime() - cmd->GPUStartTime()) * 1000.0;
            best = std::min(best, ms);
            sum += ms;
            ++n;
            pool->release();
        }
        r.out->getBytes(r.px.data(), NS::UInteger(r.W * 4), MTL::Region(0, 0, NS::UInteger(r.W), NS::UInteger(r.H)), 0);
        r.save((std::string("bubbles_1080p_") + name).c_str());
        MESSAGE("bubbles 1920x1080, " << std::string(name) << ": " << sim.bubbles().size() << " bubbles (most " << most
                                      << "), spray up to " << spray << ", " << best << " ms GPU best, " << sum / n
                                      << " mean");
        CHECK(int(most) >= maxBubbles * 3 / 4);
        CHECK(perf::renderAllocs().load() == allocs0);
        return sum / n;
    };
    const double drift = measure("drift", bub::Source::Wand, 40, 24, 55, 1.5);
    const double giant = measure("giant", bub::Source::Air, 7, 4, 150, 0.25);
    PERF_BUDGET(drift < 3.0);  // measured ~0.5 ms (M-series); loose here (shared GPU)
    PERF_BUDGET(giant < 3.0);
}

TEST_CASE("bubbles render: preview frames (REFORCE_BUBBLES_PREVIEW)") {
    const char* dir = std::getenv("REFORCE_BUBBLES_PREVIEW");
    if (!device() || !dir) return;
    Rig r(960, 540);
    // One bubble through its planned pop: the film draining black over the beat, then the tear.
    {
        bub::Settings st;
        st.seed = 6;
        bub::Sim s(st, 960.0 / 540.0, 0.0);
        s.step(0.0, quiet(), {});
        const std::uint32_t id = s.place(0.9, 0.5, 0.3);
        double t = 0;
        for (int i = 0; i < 250; ++i) s.step(t += 1.0 / 120.0, quiet(), {});  // a little past a beat line
        s.planPop(id);
        int shot = 0;
        while (!s.bubbles().empty() && t < 10) {
            s.step(t += 1.0 / 120.0, quiet(), {});
            if (s.bubbles().empty()) break;
            const bub::Bubble& b = s.bubbles()[0];
            const bool show = (b.pop < 0 && (shot == 0 || (shot == 1 && b.doom > 0.5) || (shot == 2 && b.doom > 0.95))) ||
                              (b.pop >= 0 && shot >= 3);
            if (!show) continue;
            r.fill([&](int x, int y, std::uint8_t* p) {
                p[0] = std::uint8_t(60 + 40 * ((x / 40 + y / 40) % 2));
                p[1] = std::uint8_t(30 + y / 6);
                p[2] = std::uint8_t(40 + x / 8);
            });
            r.draw(s, render::BubbleLook{});
            r.save((std::string("seq_pop_") + std::to_string(shot++)).c_str());
        }
    }
    for (bub::Source src : {bub::Source::Wand, bub::Source::Edge, bub::Source::Air}) {
        bub::Settings st;
        st.seed = 3;
        st.source = src;
        st.maxBubbles = src == bub::Source::Air ? 8 : 40;
        bub::Sim sim(st, 960.0 / 540.0, 0.0);
        bub::Controls c;
        c.blow = src == bub::Source::Air ? 1.5 : src == bub::Source::Edge ? 12 : 6;
        c.size = src == bub::Source::Air ? 150 : src == bub::Source::Edge ? 40 : 55;
        c.pop = src == bub::Source::Air ? 0.25 : src == bub::Source::Edge ? 2.5 : 1;
        render::BubbleLook look;
        for (int f = 1; f <= 60 * 24; ++f) {
            const double t = f / 60.0;
            bub::Clock k;
            k.on = true;
            k.beat = 2 * t;
            k.bar = k.beat / 4;
            k.phrase = k.bar / 16;
            sim.step(t, c, k);
            // (Also a short burst from the giants, 1/6 s apart, to see the film flow.)
            const bool burst = src == bub::Source::Air && f >= 960 && f <= 1000 && f % 10 == 0;
            if (f % 240 != 0 && f != 60 * 12 + 31 && !burst) continue;
            r.fill([&](int x, int y, std::uint8_t* p) {
                const double u = x / 960.0, v = y / 540.0;
                p[2] = std::uint8_t(30 + 120 * (0.5 + 0.5 * std::sin(u * 9 + t)));
                p[1] = std::uint8_t(20 + 60 * (0.5 + 0.5 * std::sin(v * 7 - t * 0.7)));
                p[0] = std::uint8_t(60 + 150 * (0.5 + 0.5 * std::sin((u + v) * 5 + t * 0.4)));
            });
            r.draw(sim, look);
            r.save((std::string("preview_") + bub::sourceName(src) + "_" + std::to_string(f)).c_str());
        }
    }
}
