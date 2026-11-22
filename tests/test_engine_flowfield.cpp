// FlowField grid builds (engine/FlowFieldBuilder) on configs written for these tests.
#include <doctest/doctest.h>

#include <cmath>

#include "engine/FlowFieldBuilder.hpp"
#include "scene/SceneConfig.hpp"

using namespace reforce;
using doctest::Approx;

namespace {

scene::CompiledConfig ff(std::string_view text) {
    return scene::compileConfig(scene::parseSceneText(text), scene::SceneType::FlowField);
}

engine::FlowFieldGrid build(std::string_view text, int w, int h, int threads = 3, std::uint64_t seed = 7) {
    auto cfg = ff(text);
    scene::Environment env;
    env.seed = seed;
    engine::FlowFieldBuildOptions o;
    o.width = w;
    o.height = h;
    o.threads = threads;
    return engine::buildFlowField(cfg, env, o);
}

const float* at(const engine::FlowFieldGrid& g, int col, int row) {
    return g.srcXY.data() + (std::size_t(row) * std::size_t(g.width) + std::size_t(col)) * 2;
}

}  // namespace

TEST_CASE("flowfield: identity config reproduces the pixel grid (edge pixels on the frame edges)") {
    const auto g = build("srcX=\"x\" srcY=\"y\"", 16, 9);
    const auto id = engine::identityFlowField(16, 9);
    REQUIRE(g.srcXY.size() == id.srcXY.size());
    for (std::size_t i = 0; i < g.srcXY.size(); ++i) CHECK(g.srcXY[i] == Approx(id.srcXY[i]).epsilon(1e-6));
    // Row 0 is the top (+y up); column 0 the left edge.
    CHECK(at(g, 0, 0)[0] == Approx(-1.0));
    CHECK(at(g, 0, 0)[1] == Approx(1.0));
    CHECK(at(g, 15, 8)[0] == Approx(1.0));
    CHECK(at(g, 15, 8)[1] == Approx(-1.0));
    CHECK(at(g, 3, 0)[0] == Approx(-1.0 + 2.0 * 3 / 15));
}

TEST_CASE("flowfield: Aspc=1 evaluates in 1:1 coordinates but stores edge-normalized sources") {
    // 17x10 with Aspc=1: square pixel steps 2/(H-1), so x spans +-(W-1)/(H-1) = +-16/9.
    // Identity is still identity once normalized.
    const auto g = build("Aspc=1 srcX=\"x\" srcY=\"y\"", 17, 10);
    CHECK(g.xExtent == Approx(16.0 / 9.0));
    CHECK(g.yExtent == Approx(1.0));
    const auto id = engine::identityFlowField(17, 10);
    for (std::size_t i = 0; i < g.srcXY.size(); ++i) CHECK(g.srcXY[i] == Approx(id.srcXY[i]).epsilon(1e-6));
    // A source of x = 1 (1:1 units) lands at 9/16 of the half-width.
    const auto g2 = build("Aspc=1 srcX=\"1\" srcY=\"0\"", 17, 10);
    CHECK(at(g2, 3, 4)[0] == Approx(9.0 / 16.0));
}

TEST_CASE("flowfield: Aspc=1 grid has equal x and y pixel steps (sweep #36, 'Nebula')") {
    // 1980x1260: X_EXTENTS = 1979/1259, not 1980/1260; portrait swaps the axes.
    const auto g = build("Aspc=1 srcX=\"X_EXTENTS\" srcY=\"y\"", 1980, 1260, 1);
    CHECK(g.xExtent == Approx(1979.0 / 1259.0).epsilon(1e-12));
    const auto p = build("Aspc=1 srcX=\"x\" srcY=\"y\"", 9, 17, 1);
    CHECK(p.xExtent == Approx(1.0));
    CHECK(p.yExtent == Approx(2.0));
    // Aspc=0 is unchanged: the frame corners are (+-1, +-1).
    CHECK(build("srcX=\"x\" srcY=\"y\"", 16, 9).xExtent == Approx(1.0));
}

TEST_CASE("flowfield: reforce.FlowPrecision = float32 evaluates in single precision") {
    // A 1/wrap field amplifies the last bits of r: float32 and double differ somewhere, but only slightly.
    auto cfg = ff("Aspc=1 srcR=\"r - .001 / wrap( ( theta + .1 ) * 231.9212 )\" srcT=\"theta\"");
    scene::Environment env;
    engine::FlowFieldBuildOptions o;
    o.width = 64;
    o.height = 40;
    o.threads = 1;
    const auto d = engine::buildFlowField(cfg, env, o);
    env.flowFloat32 = true;
    const auto f = engine::buildFlowField(cfg, env, o);
    int diff = 0;
    for (std::size_t i = 0; i < d.srcXY.size(); ++i) diff += f.srcXY[i] != d.srcXY[i];
    CHECK(diff > 0);
    CHECK(diff < int(d.srcXY.size()));
}

TEST_CASE("flowfield: zoom halves the source coordinates") {
    const auto g = build("srcX=\".5 * x\" srcY=\".5 * y\"", 10, 10);
    const auto id = engine::identityFlowField(10, 10);
    for (std::size_t i = 0; i < g.srcXY.size(); ++i) CHECK(g.srcXY[i] == Approx(0.5 * id.srcXY[i]).epsilon(1e-6));
}

TEST_CASE("flowfield: polar srcR/srcT") {
    // Rotation by +90 degrees: source of (x, y) is (-y, x).
    const auto g = build("Aspc=1 srcR=\"r\" srcT=\"theta + PI / 2\"", 8, 8);
    CHECK(g.polar);
    const auto id = engine::identityFlowField(8, 8);
    for (int r = 0; r < 8; ++r)
        for (int c = 0; c < 8; ++c) {
            const float* s = at(g, c, r);
            const float* d = at(id, c, r);
            CHECK(s[0] == Approx(-d[1]).epsilon(1e-5));
            CHECK(s[1] == Approx(d[0]).epsilon(1e-5));
        }
    // Radial zoom keeps the angle.
    const auto z = build("Aspc=1 srcR=\".5 * r\" srcT=\"theta\"", 8, 8);
    for (std::size_t i = 0; i < z.srcXY.size(); ++i) CHECK(z.srcXY[i] == Approx(0.5 * id.srcXY[i]).epsilon(1e-5));
}

TEST_CASE("flowfield: non-finite sources become the out-of-frame sentinel") {
    const auto g = build("srcX=\"x / ( y - y )\" srcY=\"y\"", 4, 4);
    for (std::size_t i = 0; i < g.srcXY.size(); i += 2) CHECK(g.srcXY[i] == engine::kOutOfFrame);
}

TEST_CASE("flowfield: result is independent of the thread count; rnd per pixel (D) vs per row (C)") {
    const char* txt = "C0=\"rnd( 1 )\" D0=\"rnd( 1 )\" srcX=\"x + .01 * D0\" srcY=\"y + .01 * C0\"";
    const auto a = build(txt, 32, 16, 1);
    const auto b = build(txt, 32, 16, 5);
    CHECK(a.srcXY == b.srcXY);
    const auto id = engine::identityFlowField(32, 16);
    for (int r = 0; r < 16; ++r) {
        const double dy0 = at(a, 0, r)[1] - at(id, 0, r)[1];
        bool xVaries = false;
        for (int c = 1; c < 32; ++c) {
            CHECK(at(a, c, r)[1] - at(id, c, r)[1] == Approx(dy0).epsilon(1e-4));  // one value per row
            xVaries |= std::abs((at(a, c, r)[0] - at(id, c, r)[0]) - (at(a, 0, r)[0] - at(id, 0, r)[0])) > 1e-5;
        }
        CHECK(xVaries);  // per pixel
    }
}
