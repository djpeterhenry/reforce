// The glass Tower of Hanoi (docs/3d-design.md "Tower of Hanoi"): render/HanoiPipeline on the GPU, offscreen.
//   - the glass refracts: a peg is a rod lens, showing the visuals behind it flipped left to right; the frame away from
//     the tower is untouched
//   - the discs read over white and over black (darker rims, highlights), each tinted
//   - opacity 0 draws nothing
//   - cost at 1920x1080 (6 discs, a move in flight)
// REFORCE_HANOI_PREVIEW=<dir> writes a few frames over a colorful test picture as PNGs, to look at.
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

#include "engine/Hanoi.hpp"
#include "render/HanoiPipeline.hpp"
#include "render/ImageFiles.hpp"
#include "render/ScenePipeline.hpp"
#include "render/SolidPipeline.hpp"

using namespace reforce;
namespace hn = reforce::engine::hanoi;

namespace {

MTL::Device* device() {
    static MTL::Device* d = MTL::CreateSystemDefaultDevice();
    return d;
}

struct Rig {
    int W, H;
    render::ScenePipeline scene;
    render::SolidPipeline solids;
    render::HanoiPipeline hanoi;
    MTL::CommandQueue* q;
    MTL::Texture* out;
    std::vector<std::uint8_t> px, init;
    static render::SolidPipelineOptions opts() {
        render::SolidPipelineOptions o;
        o.maxLive = 1;
        return o;
    }
    Rig(int w, int h)
        : W(w), H(h), scene(device(), uint32_t(w), uint32_t(h), {}), solids(scene, opts()), hanoi(solids),
          q(device()->newCommandQueue()), px(std::size_t(w * h * 4)), init(std::size_t(w * h * 4)) {
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
    void fill(F color) {
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const auto c = color(x, y);
                std::uint8_t* p = &init[std::size_t((y * W + x) * 4)];
                p[0] = c[2];
                p[1] = c[1];
                p[2] = c[0];
                p[3] = 255;
            }
        out->replaceRegion(MTL::Region(0, 0, NS::UInteger(W), NS::UInteger(H)), 0, init.data(), NS::UInteger(W * 4));
    }
    double draw(const render::HanoiDraw& d) {
        out->replaceRegion(MTL::Region(0, 0, NS::UInteger(W), NS::UInteger(H)), 0, init.data(), NS::UInteger(W * 4));
        MTL::CommandBuffer* cmd = q->commandBuffer();
        hanoi.encode(cmd, out, d, false);
        cmd->commit();
        cmd->waitUntilCompleted();
        out->getBytes(px.data(), NS::UInteger(W * 4), MTL::Region(0, 0, NS::UInteger(W), NS::UInteger(H)), 0);
        return (cmd->GPUEndTime() - cmd->GPUStartTime()) * 1000.0;
    }
    int at(int x, int y, int c = 1) const { return px[std::size_t((y * W + x) * 4 + c)]; }
    int brightest(int x, int y) const { return std::max({at(x, y, 0), at(x, y, 1), at(x, y, 2)}); }
    int darkest(int x, int y) const { return std::min({at(x, y, 0), at(x, y, 1), at(x, y, 2)}); }
    int was(int x, int y, int c = 1) const { return init[std::size_t((y * W + x) * 4 + c)]; }
    // A tower point (tower units) on the screen (px, y down).
    std::array<double, 2> project(const render::HanoiDraw& d, double x, double y, double z) const {
        const double aspect = double(W) / H;
        const engine::Mat4 m =
            engine::solidViewProjection(aspect, 40.0, d.perspective) * render::hanoiModel(d.pose, d.frame.discs, aspect);
        const auto c = engine::transform(m, {float(x), float(y), float(z), 1.0f});
        return {(c[0] / c[3] * 0.5 + 0.5) * W, (0.5 - c[1] / c[3] * 0.5) * H};
    }
};

render::HanoiDraw tower(int discs, double turn, double seed = 0) {
    hn::Settings s;
    s.discs = discs;
    s.beats = hn::moveCount(discs) + 1;
    hn::Plan plan(s);
    render::HanoiDraw d;
    d.slot = 0;
    d.pose.scale = 0.9;
    d.pose.rotX = 0.32;
    d.frame = plan.frameAt(seed, turn);
    const float tints[8][3] = {{0.95f, 0.45f, 0.35f}, {0.95f, 0.75f, 0.3f}, {0.6f, 0.9f, 0.4f}, {0.35f, 0.85f, 0.85f},
                               {0.4f, 0.55f, 0.95f}, {0.75f, 0.45f, 0.95f}, {0.9f, 0.5f, 0.75f}, {0.8f, 0.8f, 0.8f}};
    for (int i = 0; i < 8; ++i)
        for (int k = 0; k < 3; ++k) d.tint[std::size_t(i)][std::size_t(k)] = tints[i][k];
    return d;
}

}  // namespace

TEST_CASE("hanoi render: a glass peg is a rod lens (the visuals behind it flipped); the frame away from it untouched") {
    if (!device()) return;
    Rig r(640, 360);
    // Bright left of the middle, dark right of it: the middle peg (empty at the start of run 0) stands on the line.
    r.fill([&](int x, int) { const std::uint8_t v = x < 320 ? 200 : 20; return std::array<std::uint8_t, 3>{v, v, v}; });
    render::HanoiDraw d = tower(6, 0.0);
    d.pose.rotX = 0.0;  // seen from the front: the peg's axis is vertical on the screen
    r.draw(d);
    const double yMid = 0.5 * (hn::pegHeight(6) + hn::kThick * 0.0);
    const auto c = r.project(d, hn::pegX(1), yMid + 0.25, 0);
    const auto e = r.project(d, hn::pegX(1) + hn::kPegR, yMid + 0.25, 0);
    const double rp = e[0] - c[0];
    REQUIRE(rp > 3.0);
    const int cy = int(std::lround(c[1]));
    const int left = int(std::lround(c[0] - 0.5 * rp)), right = int(std::lround(c[0] + 0.5 * rp));
    MESSAGE("peg radius " << rp << " px; left " << r.at(left, cy) << ", right " << r.at(right, cy));
    // Without the glass: left bright, right dark. Through the rod: flipped.
    CHECK(r.was(left, cy) == 200);
    CHECK(r.was(right, cy) == 20);
    CHECK(r.at(left, cy) < r.at(right, cy) - 40);
    // Away from the tower the frame is as it was.
    for (auto [x, y] : {std::array<int, 2>{5, 5}, {634, 5}, {5, 354}, {634, 354}, {320, 6}})
        CHECK(r.at(x, y) == r.was(x, y));
    // The tower changed the frame where it stands.
    int changed = 0;
    for (int y = 0; y < 360; y += 3)
        for (int x = 0; x < 640; x += 3) changed += std::abs(r.at(x, y) - r.was(x, y)) > 12;
    CHECK(changed > 300);
}

TEST_CASE("hanoi render: the discs read over white and over black, tinted in their colors; opacity 0 draws nothing") {
    if (!device()) return;
    Rig r(640, 360);
    render::HanoiDraw d = tower(6, 0.0);
    // The bottom disc's front middle (seen from a little above): a point on its flat top, near its front edge.
    const auto p = r.project(d, hn::pegX(0), hn::kThick * 1.0, 0.75);
    const int px = int(std::lround(p[0])), py = int(std::lround(p[1]));
    for (int v : {255, 0}) {
        r.fill([&](int, int) { const std::uint8_t c = std::uint8_t(v); return std::array<std::uint8_t, 3>{c, c, c}; });
        r.draw(d);
        if (const char* dir = std::getenv("REFORCE_HANOI_PREVIEW"))
            render::writePngBgra8(std::string(dir) + (v ? "/hanoi_white.png" : "/hanoi_black.png"), r.px.data(), r.W,
                                  r.H, std::size_t(r.W * 4));
        // Over the whole tower: rims darker than white (over white), highlights and rims brighter than black.
        int marked = 0;
        for (int y = 0; y < 360; y += 2)
            for (int x = 0; x < 640; x += 2)
                marked += v == 255 ? r.darkest(x, y) < 200 : r.brightest(x, y) > 50;
        MESSAGE((v ? "over white: " : "over black: ") << marked << " pixels clearly marked");
        CHECK(marked > 200);
        if (v == 255) {
            // The bottom (largest) disc is tinted in its color (violet-blue: blue above red above green).
            const int b = r.at(px, py, 0), g = r.at(px, py, 1), rr = r.at(px, py, 2);
            MESSAGE("bottom disc over white: r " << rr << " g " << g << " b " << b);
            CHECK(b > g + 8);
        }
    }
    d.opacity = 0;
    r.draw(d);
    for (int y = 0; y < 360; y += 5)
        for (int x = 0; x < 640; x += 5) REQUIRE(r.at(x, y) == r.was(x, y));
}

TEST_CASE("hanoi render: cost at 1920x1080 (6 discs, a move in flight)") {
    if (!device()) return;
    Rig r(1920, 1080);
    r.fill([&](int x, int y) {
        return std::array<std::uint8_t, 3>{std::uint8_t(x * 255 / 1920), std::uint8_t(y * 255 / 1080),
                                           std::uint8_t((x / 40 + y / 40) % 2 ? 220 : 40)};
    });
    double best = 1e9;
    for (int i = 0; i < 40; ++i) {
        render::HanoiDraw d = tower(6, 20.5 + 0.01 * i);
        const double ms = r.draw(d);
        if (i >= 4) best = std::min(best, ms);
    }
    MESSAGE("hanoi 1080p: " << best << " ms GPU");
    PERF_BUDGET(best < 3.0);

    // REFORCE_HANOI_PREVIEW=<dir>: the tower over a colorful picture at a few moments, as PNGs.
    if (const char* dir = std::getenv("REFORCE_HANOI_PREVIEW")) {
        r.fill([&](int x, int y) {
            const double u = x / 1920.0, v = y / 1080.0;
            const std::uint8_t a = std::uint8_t(127 + 120 * std::sin(12 * u + 3 * std::sin(7 * v)));
            const std::uint8_t b = std::uint8_t(127 + 120 * std::sin(9 * v + 2 * u));
            const std::uint8_t c = (x / 60 + y / 60) % 2 ? 230 : 25;
            return std::array<std::uint8_t, 3>{a, b, c};
        });
        int k = 0;
        for (double turn : {0.0, 0.5, 4.3, 20.5, 31.6, 63.8}) {
            render::HanoiDraw d = tower(6, turn);
            r.draw(d);
            render::writePngBgra8(std::string(dir) + "/hanoi_" + std::to_string(k++) + ".png", r.px.data(), r.W, r.H,
                                  std::size_t(r.W * 4));
        }
        // The content's one-glass look (Mono): Hanoi Tower.
        struct Look { const char* name; float frost, tint, bend, depth; };
        for (const Look& l : {Look{"tower", 16, 0.45f, 2.0f, 0.85f}}) {
            render::HanoiDraw d = tower(6, 20.5);
            const std::array<float, 3> glass{0.95f, 0.7f, 0.35f};
            for (auto& t : d.tint) t = glass;
            d.clear = glass;
            d.oneGlass = true;
            d.frost = l.frost;
            d.tintAmount = l.tint;
            d.bend = l.bend;
            d.depth = l.depth;
            r.draw(d);
            render::writePngBgra8(std::string(dir) + "/hanoi_look_" + l.name + ".png", r.px.data(), r.W, r.H,
                                  std::size_t(r.W * 4));
        }
    }
}
