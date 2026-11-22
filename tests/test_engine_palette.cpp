// MapFunction LUT, PNG strip resampling, ColorMap recompute interval.
#include <doctest/doctest.h>

#include <cmath>

#include "engine/Palette.hpp"
#include "scene/SceneConfig.hpp"

using namespace reforce;
using doctest::Approx;

TEST_CASE("palette: MapFunction LUT") {
    engine::MapLut lut;
    engine::MapFunction f("i^2.1");
    CHECK(f.error().empty());
    CHECK(!f.timeDependent());
    f.evaluate(0, lut);
    CHECK(lut[0] == Approx(0));
    CHECK(lut[255] == Approx(1));
    CHECK(lut[128] == Approx(std::pow(128.0 / 255.0, 2.1)));

    engine::MapFunction g("wrap( i + .1 * t )");
    CHECK(g.timeDependent());
    g.evaluate(2, lut);
    CHECK(lut[0] == Approx(0.2));

    engine::MapFunction clampTest("2 * i - .5");
    clampTest.evaluate(0, lut);
    CHECK(lut[0] == 0.f);
    CHECK(lut[255] == 1.f);

    engine::MapFunction bad("i +* (");
    CHECK(!bad.error().empty());
    bad.evaluate(0, lut);
    CHECK(lut[51] == Approx(51.0 / 255.0));  // identity fallback
}

TEST_CASE("palette: PNG strip is resampled left (i=0) to right (i=1)") {
    const std::uint8_t strip[3 * 4] = {0, 0, 0, 255, 255, 0, 0, 255, 255, 255, 255, 255};
    const auto p = engine::paletteFromStrip(strip, 3);
    CHECK(p[0].r == 0.f);
    CHECK(p[255].g == Approx(1.f));
    CHECK(p[128].r == Approx(1.f).epsilon(0.01));  // middle pixel
    CHECK(p[64].r == Approx(64.0 / 255.0 * 3.0 - 0.5).epsilon(0.01));  // texture lookup: texel centers at (j+.5)/W
    CHECK(p[20].r == 0.f);                                               // clamped to the first texel
}

TEST_CASE("palette: text ColorMap re-evaluates every CCal seconds") {
    auto cfg = std::make_shared<scene::CompiledConfig>(scene::compileConfig(
        scene::parseSceneText("CCal=.5 R=\"i\" G=\"t\" B=\"2\""), scene::SceneType::ColorMap));
    engine::ColorMapSource src(cfg, {});
    engine::Palette p;
    CHECK(src.update(0, p));
    CHECK(p[255].r == Approx(1));
    CHECK(p[10].b == Approx(1));  // clamped
    CHECK(!src.update(0.3, p));
    CHECK(src.update(0.6, p));
    CHECK(p[0].g == Approx(0.6));
}

TEST_CASE("palette: a ColorMap's t starts at 0 when it starts (corpus sweep)") {
    auto cfg = std::make_shared<scene::CompiledConfig>(scene::compileConfig(
        scene::parseSceneText("R=\"i\" G=\"t / 10\" B=\"0\""), scene::SceneType::ColorMap));
    engine::ColorMapSource src(cfg, {});
    engine::Palette p;
    CHECK(src.update(100.0, p));
    CHECK(p[0].g == Approx(0.0));
    CHECK(src.update(105.0, p));
    CHECK(p[0].g == Approx(0.5));
}
