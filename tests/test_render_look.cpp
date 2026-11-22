// Live "Look" switches on the GPU pipeline (engine/Look.hpp): the intensity
// format changes mid-run without losing the trail, and thin lines / AA / flow
// rounding take effect on the next step. Synthetic segments, offscreen.
#include <doctest/doctest.h>

#include <chrono>
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

double sum(const std::vector<float>& img) {
    double s = 0;
    for (float v : img) s += v;
    return s;
}

float maxDiff(const std::vector<float>& a, const std::vector<float>& b, float offset = 0) {
    float m = 0;
    for (std::size_t i = 0; i < a.size(); ++i) m = std::max(m, std::fabs(a[i] - offset - b[i]));
    return m;
}

std::vector<engine::LineSegment> shape() {
    std::vector<engine::LineSegment> segs;
    linetest::addPolyline(segs, {20.3f, 30.3f, 180.7f, 60.6f, 40.2f, 90.1f}, 1.0f, 2.0f);
    linetest::addDot(segs, 150.5f, 20.5f, 0.8f, 5.0f);
    return segs;
}

}  // namespace

TEST_CASE("look: switching the intensity format mid-run keeps the trail (gforce <-> hq)") {
    if (!device()) return;
    const int W = 200, H = 100;
    render::ScenePipelineOptions o;  // gforce (R8)
    Harness hs(device(), W, H, o, Field::Identity);
    hs.pipeline().setField(engine::identityFlowField(W, H));  // exact identity: the trail only decays
    hs.step(shape());
    hs.step({}, 3);
    const auto before = hs.read();
    REQUIRE(sum(before) > 1000);

    // -> hq: the next step flows the copied trail (one decay level per step).
    hs.pipeline().setIntensityFormat(render::IntensityFormat::Half16);
    CHECK(hs.pipeline().format() == render::IntensityFormat::Half16);
    hs.step({});
    const auto hq = hs.read();
    CHECK(sum(hq) > 0.9 * sum(before));
    CHECK(maxDiff(before, hq, 1.0f) <= 1.01f);  // max(v - 1, 0), half-float rounding

    // -> gforce again.
    hs.pipeline().setIntensityFormat(render::IntensityFormat::GForce8);
    hs.step({});
    const auto back = hs.read();
    CHECK(maxDiff(hq, back, 1.0f) <= 1.01f);

    // Two switches before one step (a fast A/B double click): chained copies.
    hs.pipeline().setIntensityFormat(render::IntensityFormat::Half16);
    hs.pipeline().setIntensityFormat(render::IntensityFormat::GForce8);
    hs.step({});
    const auto chained = hs.read();
    CHECK(maxDiff(back, chained, 1.0f) <= 1.01f);
    CHECK(sum(chained) > 0.8 * sum(before));

    // Lines still draw in the new format (pipelines rebound).
    hs.pipeline().setIntensityFormat(render::IntensityFormat::Half16);
    hs.step(shape());
    const auto drawn = hs.read();
    CHECK(sum(drawn) > sum(chained));
}

TEST_CASE("look: the other format's pipelines can be warmed ahead (no compile at the switch)") {
    if (!device()) return;
    render::ScenePipelineOptions o;
    Harness hs(device(), 64, 32, o, Field::Identity);
    hs.pipeline().prewarmOtherFormat();
    hs.step(shape());
    const auto t0 = std::chrono::steady_clock::now();
    hs.pipeline().setIntensityFormat(render::IntensityFormat::Half16);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    MESSAGE("setIntensityFormat after prewarm: " << ms << " ms");
    hs.step({});
    CHECK(sum(hs.read()) > 0);
}

TEST_CASE("look: thin lines and AA switch live") {
    if (!device()) return;
    const int W = 200, H = 100;
    render::ScenePipelineOptions o;
    std::vector<engine::LineSegment> segs;
    linetest::addPolyline(segs, {20.3f, 30.3f, 180.7f, 30.3f}, 1.0f, 0.28f);  // thin line (probe #32 case)
    Harness hs(device(), W, H, o, Field::Void);
    if (std::string(hs.pipeline().lineAaMethod()) != "path") return;
    hs.step(segs);
    const auto smooth = hs.read();
    hs.pipeline().setThinLines(render::ThinLines::GForce);
    CHECK(hs.pipeline().gforceLines());
    hs.step(segs);
    const auto gforce = hs.read();
    CHECK(gforce[30 * W + 100] == doctest::Approx(143).epsilon(0.01));  // one row at 255 x width
    CHECK(gforce[29 * W + 100] == 0.0f);
    CHECK(maxDiff(smooth, gforce) > 20);
    hs.pipeline().setAntialias(false);
    CHECK(std::string(hs.pipeline().lineAaMethod()) == "off");
    CHECK(!hs.pipeline().gforceLines());
    hs.step(segs);
    const auto off = hs.read();
    CHECK(maxDiff(off, gforce) > 20);
    hs.pipeline().setAntialias(true);
    CHECK(std::string(hs.pipeline().lineAaMethod()) == "path");
    hs.step(segs);
    CHECK(maxDiff(hs.read(), gforce) == 0.0f);
}

TEST_CASE("look: flow rounding switches live") {
    if (!device()) return;
    const int W = 160, H = 90;
    render::ScenePipelineOptions o;  // fixed16
    Harness a(device(), W, H, o, Field::ZoomRotate), b(device(), W, H, o, Field::ZoomRotate);
    a.step(shape());
    b.step(shape());
    a.step({}, 5);
    b.step({}, 5);
    CHECK(maxDiff(a.read(), b.read()) == 0.0f);
    b.pipeline().setFlowRounding(render::FlowRounding::Nearest);
    a.step({}, 20);
    b.step({}, 20);
    const auto fa = a.read(), fb = b.read();
    CHECK(maxDiff(fa, fb) > 0.0f);
    CHECK(sum(fb) > sum(fa));  // truncation darkens the trail; nearest keeps it brighter (PLAN F1)
}
