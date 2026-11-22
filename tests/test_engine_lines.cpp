// Line width model, frame -> pixel mapping and segment building.
#include <doctest/doctest.h>

#include <cmath>
#include <limits>

#include "engine/LineGeometry.hpp"

using namespace reforce;
using doctest::Approx;

TEST_CASE("lines: width = LWdt x s(W, H), G-Force's frame-size law (probe #38, run 24)") {
    engine::LineStyle s;
    // G-Force 5.9.7, area-based width per LWdt (run 24, LWdt 2/5/10 lines): 18 frame sizes, e.g.
    CHECK(engine::lineWidthPixels(1, s, 1980, 1260) == Approx(2.8357).epsilon(0.0005));
    CHECK(engine::lineWidthPixels(10, s, 1980, 860) == Approx(24.882).epsilon(0.0005));
    CHECK(engine::lineWidthPixels(10, s, 3200, 1260) == Approx(34.569).epsilon(0.0005));
    CHECK(engine::lineWidthPixels(10, s, 1000, 1860) == Approx(25.486).epsilon(0.0005));  // portrait: short side W
    CHECK(engine::lineWidthPixels(10, s, 600, 400) == Approx(13.855).epsilon(0.001));
    CHECK(engine::lineWidthPixels(10, s, 2800, 1844) == Approx(37.596).epsilon(0.0005));  // 5.16 MP: full size
    // Above ~5.2 MP G-Force renders at half size and doubles the pixels: 3456x2234 full screen = 2 x s(1728, 1117).
    CHECK(engine::lineWidthPixels(10, s, 3456, 2234) == Approx(51.647).epsilon(0.0005));
    CHECK(engine::lineWidthPixels(10, s, 3416, 1544) == Approx(45.467).epsilon(0.0005));
    // ... and the offset is in those frame pixels too (LineWidth.Scale 2, Offset 3: LWdt 2 -> 14.341 / 26.643 px).
    s.scale = 2;
    s.offset = 3;
    CHECK(engine::lineWidthPixels(2, s, 1980, 1260) == Approx(14.341).epsilon(0.001));
    CHECK(engine::lineWidthPixels(2, s, 3456, 2234) == Approx(26.643).epsilon(0.001));
    // reforce.LineScale = frame: the law on the render size itself (no step at the half-size limit).
    s.scale = 1;
    s.offset = 0;
    s.sizeMode = engine::LineScaleMode::Frame;
    CHECK(engine::lineWidthPixels(1, s, 3456, 2234) == Approx(0.73304 + 8.6742e-4 * 2234 + 5.0969e-4 * 3456));
    CHECK(engine::lineWidthPixels(1, s, 1980, 1260) == Approx(2.8357).epsilon(0.0005));
    s.sizeMode = engine::LineScaleMode::GForce;
    s.autoScale = false;
    s.scale = 2;
    s.offset = 1;
    CHECK(engine::lineWidthPixels(3, s, 1920, 1080) == Approx(7.0));
    CHECK(engine::lineWidthPixels(0, s, 1920, 1080) == Approx(1.0));
    // Below 1 px the true width is kept (the GPU draws a 1-px footprint, dimmed by the width under AA;
    // Phase 5E); negative widths are 0.
    CHECK(engine::lineWidthPixels(0.2, s, 1920, 1080) == Approx(1.4));
    s.offset = 0;
    CHECK(engine::lineWidthPixels(0.2, s, 1920, 1080) == Approx(0.4));
    CHECK(engine::lineWidthPixels(-5, s, 1920, 1080) == Approx(0.0));
    s.minWidthPx = 1;
    CHECK(engine::lineWidthPixels(0.2, s, 1920, 1080) == Approx(1.0));
}

TEST_CASE("lines: frame coordinates map to pixels (+y up, edges at +-extent)") {
    // G-Force draws frame x through the center of pixel column (x + 1) * W / 2 (corpus sweep): +0.5 px.
    engine::FrameMapping m{1920, 1080, 1, 1};
    CHECK(m.toPixelX(0) == Approx(960.5));
    CHECK(m.toPixelY(0) == Approx(540.5));
    CHECK(m.toPixelX(1) == Approx(1920.5));
    CHECK(m.toPixelY(1) == Approx(0.5));     // top
    CHECK(m.toPixelY(-1) == Approx(1080.5)); // bottom
    // Aspc=1: x extent 16/9, so x = 1 lands 9/16 of the way to the right edge.
    engine::FrameMapping a{1920, 1080, 1920.0 / 1080.0, 1};
    CHECK(a.toPixelX(1) == Approx(960 + 540 + 0.5));
    CHECK(a.toPixelY(0.5) == Approx(270.5));
}

TEST_CASE("lines: polylines vs dots, order, pen clamp, non-finite points") {
    scene::WaveShapeFrame f;
    f.connected = true;
    scene::WavePath p0;
    p0.x = {-1, 0, 1};
    p0.y = {0, 0, 0};
    p0.pen = {2, .5f, -1};
    p0.width = {1, 1, 3};
    scene::WavePath p1 = p0;
    p1.y = {0.5f, std::numeric_limits<float>::quiet_NaN(), 0.5f};
    f.paths = {p0, p1};
    engine::FrameMapping m{420, 420, 1, 1};
    engine::LineStyle s;
    std::vector<engine::LineSegment> out;
    engine::appendSegments(f, m, s, out);
    REQUIRE(out.size() == 2);  // path 1 is broken at its NaN point on both sides
    CHECK(out[0].ax == Approx(0.5));  // + half a pixel: FrameMapping::kPixelCenter
    CHECK(out[0].bx == Approx(210.5));
    CHECK(out[0].penA == Approx(1));     // clamped
    CHECK(out[1].penB == Approx(0));     // clamped
    // Mean of the radii for LWdt 1 and LWdt 3 at 420x420.
    const double s420 = engine::autoLineScale(420, 420);
    CHECK(out[1].radius == Approx(0.5 * (0.5 * 1 * s420 + 0.5 * 3 * s420)));
    // Path ids (per-path AA): the index of the path's first segment.
    CHECK(out[0].path == 0);  // both segments are path 0's (path 1 has none)
    CHECK(out[1].path == 0);
    f.connected = false;
    out.clear();
    engine::appendSegments(f, m, s, out);
    // Dots: x = +1 maps to px = W (off the pixel grid) and is not drawn (G-Force, parity run 1 shot 7);
    // x = -1 (px 0) is. Path 0: x = -1, 0; path 1: x = -1 (its middle point is NaN).
    REQUIRE(out.size() == 3);
    CHECK(out[0].ax == out[0].bx);
    CHECK(out[0].ax == Approx(0.5));
    CHECK(out[1].ax == Approx(210.5));
    CHECK(out[2].ay == Approx(105.5));  // second path drawn after the first
    CHECK(out[0].path == 0);  // dots of one path share its id
    CHECK(out[1].path == 0);
    CHECK(out[2].path == 2);
}

TEST_CASE("lines: dots centered off the pixel grid are culled, polylines are not") {
    scene::WaveShapeFrame f;
    f.connected = false;
    scene::WavePath p;
    p.x = {1, -1, 0, 0, 1.01f};
    p.y = {0, 0, 1, -1, 0};
    p.pen = {1, 1, 1, 1, 1};
    p.width = {10, 10, 10, 10, 10};
    f.paths = {p};
    engine::FrameMapping m{1980, 1260, 1, 1};
    engine::LineStyle s;
    std::vector<engine::LineSegment> out;
    engine::appendSegments(f, m, s, out);
    REQUIRE(out.size() == 2);  // x = -1 (px 0) and y = +1 (py 0); x = +1, y = -1 and beyond are off the grid
    CHECK(out[0].ax == Approx(0.5));
    CHECK(out[1].ay == Approx(0.5));
    f.connected = true;  // a polyline from x = 1 to x = -1 still reaches both edges
    out.clear();
    engine::appendSegments(f, m, s, out);
    CHECK(out.size() == 4);
}
