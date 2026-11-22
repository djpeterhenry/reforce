// GPU line pass (Phase 5E): per-path antialiasing keeps G-Force's overwrite
// semantics (probe #15) and removes the double blend at joints; thin lines
// keep intensity proportional to width. Synthetic segments, offscreen.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <string>

#include "line_harness.hpp"

using namespace reforce;
using linetest::Field;
using linetest::Harness;

namespace {

MTL::Device* device() {
    static MTL::Device* d = MTL::CreateSystemDefaultDevice();
    return d;
}

render::ScenePipelineOptions opts(bool aa, render::LineAa m = render::LineAa::PerPath) {
    render::ScenePipelineOptions o;
    o.antialias = aa;
    o.lineAa = m;
    return o;
}

std::vector<float> render1(const render::ScenePipelineOptions& o, const std::vector<engine::LineSegment>& segs, int w,
                           int h, std::string* method = nullptr) {
    Harness hs(device(), w, h, o, Field::Void);
    if (method) *method = hs.pipeline().lineAaMethod();
    hs.step(segs);
    return hs.read();
}

float at(const std::vector<float>& img, int w, int x, int y) { return img[std::size_t(y) * w + x]; }

}  // namespace

TEST_CASE("line AA: a polyline's own segments do not double-blend (joints, dense chains)") {
    if (!device()) return;
    const int W = 256, H = 64;
    // The same straight line as one segment and as a chain of 0.5-px segments (a dense curve's case).
    std::vector<engine::LineSegment> one, chain;
    linetest::addPolyline(one, {20.3f, 30.4f, 220.3f, 36.4f}, 0.8f, 1.5f);
    std::vector<float> xy;
    for (int i = 0; i <= 400; ++i) {
        xy.push_back(20.3f + 0.5f * i);
        xy.push_back(30.4f + 6.f * i / 400.f);
    }
    linetest::addPolyline(chain, xy, 0.8f, 1.5f);
    std::string method;
    const auto a = render1(opts(true), one, W, H, &method);
    const auto b = render1(opts(true), chain, W, H);
    if (method != "path") {
        MESSAGE("per-path AA unavailable on this GPU (" << method << "); skipped");
        return;
    }
    float maxDiff = 0, maxVal = 0;
    for (int y = 0; y < H; ++y)
        for (int x = 30; x < 210; ++x) {
            maxDiff = std::max(maxDiff, std::abs(at(a, W, x, y) - at(b, W, x, y)));
            maxVal = std::max(maxVal, at(b, W, x, y));
        }
    CHECK(maxDiff <= 1.0f);
    CHECK(maxVal <= 0.8f * 255 + 1);
    // The legacy per-segment blend fattens the chain's AA fringe (the bug this fixes).
    const auto c = render1(opts(true, render::LineAa::Analytic), chain, W, H);
    float legacyDiff = 0;
    for (int y = 0; y < H; ++y)
        for (int x = 30; x < 210; ++x) legacyDiff = std::max(legacyDiff, std::abs(at(a, W, x, y) - at(c, W, x, y)));
    CHECK(legacyDiff > 20.0f);

    // A sharp joint (150 deg turn) of a Pen .3 path over a Pen .8 path: every pixel stays between the two
    // pens, and the joint's fringe is no darker than the straight run's fringe at the same distance.
    std::vector<engine::LineSegment> v;
    linetest::addPolyline(v, {0, 32, float(W), 32}, 0.8f, 40.f);
    linetest::addPolyline(v, {60.4f, 30.3f, 160.4f, 30.3f, 60.4f + 100.f * std::cos(0.26f), 30.3f + 100.f * std::sin(0.26f)},
                          0.3f, 1.5f);
    const auto img = render1(opts(true), v, W, H);
    float lo = 255, hi = 0;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            lo = std::min(lo, at(img, W, x, y));
            hi = std::max(hi, at(img, W, x, y));
        }
    CHECK(lo >= 0.3f * 255 - 1);
    CHECK(hi <= 0.8f * 255 + 1);
    // Just past the joint's tip only the round cap's fringe covers pixel (161, 30): coverage from the distance
    // to the joint, applied once (legacy: once per segment, 79 instead of 92).
    const float d = std::hypot(161.5f - 160.4f, 30.5f - 30.3f), cov = 2.0f - d;
    CHECK(at(img, W, 161, 30) == doctest::Approx(204.f + (77.f - 204.f) * cov).epsilon(0.012));
}

TEST_CASE("line AA: thin lines keep intensity proportional to width") {
    if (!device()) return;
    const int W = 200, H = 120;
    const float widths[] = {0.25f, 0.5f, 0.75f, 1.0f, 2.0f};
    std::vector<engine::LineSegment> segs;
    for (int k = 0; k < 5; ++k) {
        const float y = 12.f + 22.f * k + 0.3f;
        linetest::addPolyline(segs, {10, y, 190, y + 9.f}, 1.0f, 0.5f * widths[k]);
    }
    std::string method;
    const auto img = render1(opts(true), segs, W, H, &method);
    if (method != "path") return;
    for (int k = 0; k < 5; ++k) {
        double sum = 0;
        int n = 0;
        for (int x = 30; x < 170; ++x, ++n)
            for (int y = 12 + 22 * k - 4; y <= 12 + 22 * k + 14; ++y) sum += at(img, W, x, y) / 255.0;
        const double energy = sum / n;  // px of full intensity per column
        CAPTURE(widths[k]);
        CHECK(energy == doctest::Approx(widths[k] * std::sqrt(1.0 + (9.0 / 180) * (9.0 / 180))).epsilon(0.06));
    }
    // Without AA a thin line is still drawn 1 px wide at full Pen (unchanged).
    const auto off = render1(opts(false), segs, W, H);
    float peak = 0;
    for (int x = 30; x < 170; ++x)
        for (int y = 6; y <= 20; ++y) peak = std::max(peak, at(off, W, x, y));
    CHECK(peak == 255.0f);
}

TEST_CASE("line AA: overwrite order across paths and Pen 0 erasing are unchanged (probe #15)") {
    if (!device()) return;
    const int W = 200, H = 200;
    for (int mode = 0; mode < 3; ++mode) {
        const bool aa = mode > 0;
        const auto m = mode == 2 ? render::LineAa::Analytic : render::LineAa::PerPath;
        CAPTURE(mode);
        // Pen .8 vertical, then Pen .3 horizontal: the crossing is .3 (77 levels), and the reverse order gives 204.
        std::vector<engine::LineSegment> segs;
        linetest::addPolyline(segs, {100, 10, 100, 190}, 0.8f, 3.f);
        linetest::addPolyline(segs, {10, 100, 190, 100}, 0.3f, 3.f);
        auto img = render1(opts(aa, m), segs, W, H);
        CHECK(at(img, W, 100, 100) == doctest::Approx(77).epsilon(0.015));
        CHECK(at(img, W, 100, 50) == doctest::Approx(204).epsilon(0.01));
        std::swap(segs[0], segs[1]);
        std::swap(segs[0].path, segs[1].path);
        img = render1(opts(aa, m), segs, W, H);
        CHECK(at(img, W, 100, 100) == doctest::Approx(204).epsilon(0.01));
        // Pen 0 erases: a 17-px-wide Pen 0 path across a Pen .8 path cuts a gap of ~17 px.
        segs.clear();
        linetest::addPolyline(segs, {10, 100, 190, 100}, 0.8f, 6.f);
        linetest::addPolyline(segs, {120.3f, 10, 120.3f, 190}, 0.0f, 8.5f);
        img = render1(opts(aa, m), segs, W, H);
        CHECK(at(img, W, 120, 100) == 0.0f);
        double gap = 0;
        for (int x = 90; x < 150; ++x) gap += 1.0 - at(img, W, x, 100) / 204.0;
        CHECK(gap == doctest::Approx(17.0).epsilon(aa ? 0.03 : 0.07));
    }
}

TEST_CASE("line AA: ThinLines gforce reproduces G-Force's rasterization (probe #32)") {
    if (!device()) return;
    const int W = 200, H = 100;
    render::ScenePipelineOptions o = opts(true);
    o.thinLines = render::ThinLines::GForce;
    std::string method;
    // A thin horizontal line at a sub-pixel y: snapped into ONE row at 255 x width (width 0.56 px -> 143).
    // Pixel positions are reforce's continuous mapping (x + 1) W/2 + 0.5; G-Force truncates toward the center.
    std::vector<engine::LineSegment> segs;
    // Center (100, 50); column = 100 + trunc(p - 0.5 - 100), row likewise: x 20.3 -> 20, 180.7 -> 180, y 30.3 -> 30.
    linetest::addPolyline(segs, {20.3f, 30.3f, 180.7f, 30.3f}, 1.0f, 0.28f);
    // Thin dots: a 1-px column of height = width (w 0.28 -> 71), and w 1.41 -> 52/255/52 in one column.
    linetest::addDot(segs, 60.9f, 70.6f, 1.0f, 0.14f);
    linetest::addDot(segs, 120.2f, 70.6f, 1.0f, 0.705f);
    // A large dot is a disc of column spans: column dx has half-height sqrt(r^2 - dx^2), no AA across columns.
    linetest::addDot(segs, 160.5f, 70.5f, 1.0f, 4.257f);
    Harness hs(device(), W, H, o, Field::Void);
    method = hs.pipeline().lineAaMethod();
    if (method != "path") return;
    CHECK(hs.pipeline().gforceLines());
    hs.step(segs);
    const auto img = hs.read();
    for (int x = 22; x < 179; x += 7) {
        CAPTURE(x);
        CHECK(at(img, W, x, 30) == doctest::Approx(143).epsilon(0.01));
        CHECK(at(img, W, x, 29) == 0.0f);
        CHECK(at(img, W, x, 31) == 0.0f);
    }
    CHECK(at(img, W, 19, 30) == 0.0f);  // ends at the snapped endpoint columns (20 and 180)
    CHECK(at(img, W, 20, 30) > 0.0f);
    CHECK(at(img, W, 180, 30) > 0.0f);
    CHECK(at(img, W, 181, 30) == 0.0f);
    // y 70.6 -> row 50 + trunc(20.1) = 70; x 60.9 -> column 100 + trunc(-39.6) = 61; 120.2 -> 119.
    CHECK(at(img, W, 61, 70) == doctest::Approx(71).epsilon(0.02));
    CHECK(at(img, W, 60, 70) + at(img, W, 62, 70) + at(img, W, 61, 69) + at(img, W, 61, 71) == 0.0f);
    CHECK(at(img, W, 119, 70) == 255.0f);
    CHECK(at(img, W, 119, 69) == doctest::Approx(52).epsilon(0.03));
    CHECK(at(img, W, 119, 71) == doctest::Approx(52).epsilon(0.03));
    CHECK(at(img, W, 118, 70) + at(img, W, 120, 70) == 0.0f);
    // r = 4.257 dot at column 160, row 70: column +4 spans 1.457 -> rows 69..71 (row 69: 0.957), column +5 empty.
    CHECK(at(img, W, 164, 70) == 255.0f);
    CHECK(at(img, W, 164, 69) == doctest::Approx(244).epsilon(0.02));
    CHECK(at(img, W, 164, 68) == 0.0f);
    CHECK(at(img, W, 165, 70) == 0.0f);
    CHECK(at(img, W, 160, 66) == doctest::Approx(193).epsilon(0.02));  // top: 4.257 - 3.5 = 0.757
}

TEST_CASE("line AA: ThinLines smooth draws small dots round with G-Force's energy") {
    if (!device()) return;
    const int W = 200, H = 40;
    const float radii[] = {0.07f, 0.14f, 0.35f, 0.7f, 1.0f};
    std::vector<engine::LineSegment> segs;
    for (int k = 0; k < 5; ++k) linetest::addDot(segs, 20.3f + 35.f * k, 20.4f, 1.0f, radii[k]);
    Harness hs(device(), W, H, opts(true), Field::Void);
    if (std::string(hs.pipeline().lineAaMethod()) != "path") return;
    CHECK(!hs.pipeline().gforceLines());
    hs.step(segs);
    const auto img = hs.read();
    for (int k = 0; k < 5; ++k) {
        double e = 0;
        for (int y = 14; y < 27; ++y)
            for (int x = 14 + 35 * k; x < 27 + 35 * k; ++x) e += at(img, W, x, y) / 255.0;
        CAPTURE(radii[k]);
        CHECK(e == doctest::Approx(2.0 * radii[k]).epsilon(0.12));  // G-Force: 2r up to r = 1
    }
}
