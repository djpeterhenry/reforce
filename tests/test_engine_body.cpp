// Body tracking reactions (engine/Body.hpp, docs/body.md): settings, smoothing, mapping, hand trails.

#include <doctest/doctest.h>

#include <cmath>

#include "engine/Body.hpp"

using namespace reforce::engine;

TEST_CASE("body: settings parse, validate and save normalized") {
    BodySettings s;
    CHECK(applyBodySetting(s, "track", "on").empty());
    CHECK(s.track);
    CHECK(applyBodySetting(s, "Quality", "fast").empty());
    CHECK(s.fast);
    CHECK(applyBodySetting(s, "burn", "Outline").empty());
    CHECK(s.burn == BodyBurn::Outline);
    CHECK(applyBodySetting(s, "front", "0.75").empty());
    CHECK(s.front == doctest::Approx(0.75));
    CHECK(applyBodySetting(s, "trailwidth", "12").empty());
    CHECK(s.trailWidth == doctest::Approx(12));
    CHECK_FALSE(applyBodySetting(s, "front", "1.5").empty());
    CHECK_FALSE(applyBodySetting(s, "burn", "glow").empty());
    CHECK_FALSE(applyBodySetting(s, "quality", "accurate").empty());
    CHECK_FALSE(applyBodySetting(s, "trailwidth", "0").empty());
    CHECK_FALSE(applyBodySetting(s, "bogus", "1").empty());
    CHECK(s.front == doctest::Approx(0.75));  // a refused value leaves the setting alone

    CHECK(bodySettingKey("burnamount") == "reforce.Body.BurnAmount");
    CHECK(bodySettingKey("nope").empty());
    CHECK(bodySettingValue("track", "yes") == std::optional<std::string>("1"));
    CHECK(bodySettingValue("burn", "MOTION") == std::optional<std::string>("motion"));
    CHECK(bodySettingValue("trails", "0.5") == std::optional<std::string>("0.5"));
    CHECK(BodySettings{}.sync);  // in step with the camera by default
    CHECK(bodySettingKey("sync") == "reforce.Body.Sync");
    CHECK(bodySettingValue("sync", "off") == std::optional<std::string>("0"));
    CHECK_FALSE(bodySettingValue("trails", "-1"));
}

TEST_CASE("body: one-euro filter starts at the first sample, holds still input, follows a move") {
    OneEuro f;
    CHECK(f.filter(0.3, 1.0 / 60) == doctest::Approx(0.3));
    for (int i = 0; i < 30; ++i) f.filter(0.3, 1.0 / 60);
    CHECK(f.value() == doctest::Approx(0.3));
    // A jump: partway after one step (smoothed), close after half a second.
    const double first = f.filter(0.7, 1.0 / 60);
    CHECK(first > 0.3);
    CHECK(first < 0.7);
    for (int i = 0; i < 30; ++i) f.filter(0.7, 1.0 / 60);
    CHECK(f.value() == doctest::Approx(0.7).epsilon(0.01));
}

TEST_CASE("body: pose smoother fades joints in and out, drops stale poses, low confidence stays hidden") {
    PoseSmoother p;
    PoseSample s;
    s.seq = 1;
    s.x[JLeftWrist] = 0.2f;
    s.y[JLeftWrist] = 0.6f;
    s.conf[JLeftWrist] = 0.9f;
    s.x[JNose] = 0.5f;
    s.conf[JNose] = 0.1f;  // below kMinConf
    const double dt = 1.0 / 60;
    p.step(s, dt);
    CHECK(p.joint(JLeftWrist).x == doctest::Approx(0.2));  // starts where it is
    CHECK(p.joint(JLeftWrist).weight == doctest::Approx(dt / PoseSmoother::kFade));
    for (int i = 0; i < 20; ++i) p.step(s, dt);
    CHECK(p.joint(JLeftWrist).weight == doctest::Approx(1.0));
    CHECK(p.joint(JNose).weight == 0);
    CHECK(p.presence() == doctest::Approx(1.0));
    // No new pose for longer than kStale: fades out, holding its place.
    for (int i = 0; i < 60; ++i) p.step(s, dt);
    CHECK(p.joint(JLeftWrist).weight == 0);
    CHECK(p.joint(JLeftWrist).x == doctest::Approx(0.2));
    // Seen again somewhere else: appears there (no slide from the old place).
    s.seq = 2;
    s.x[JLeftWrist] = 0.8f;
    p.step(s, dt);
    CHECK(p.joint(JLeftWrist).x == doctest::Approx(0.8));
    p.reset();
    CHECK(p.presence() == 0);
}

TEST_CASE("body: camera -> screen mapping inverts the backdrop's sampling (fit and mirror)") {
    for (VideoFit fit : {VideoFit::Cover, VideoFit::Contain, VideoFit::Stretch})
        for (bool mirror : {false, true}) {
            const UvTransform uv = fitTransform(1920, 1440, 1920, 1080, fit);  // a 4:3 camera on a 16:9 frame
            for (double u : {0.1, 0.5, 0.83})
                for (double v : {0.2, 0.7}) {
                    // backdrop_fs: screen uv -> (mirror) -> camera s = uv * scale + offset
                    const double um = mirror ? 1 - u : u;
                    const double sx = um * uv.scale[0] + uv.offset[0], sy = v * uv.scale[1] + uv.offset[1];
                    const auto back = cameraToScreen(sx, sy, uv, mirror);
                    INFO("fit " << int(fit) << " mirror " << mirror);
                    CHECK(back[0] == doctest::Approx(u));
                    CHECK(back[1] == doctest::Approx(v));
                }
        }
}

TEST_CASE("body: hand trails draw from where each hand was to where it is") {
    HandTrails h;
    std::vector<LineSegment> out;
    const double W = 1920, H = 1080;
    h.step({{{100, 200, 1.0}, {0, 0, 0.0}}}, 1.0 / 60, 0.8, 10, W, H, out);
    REQUIRE(out.size() == 1);  // the right hand is not visible
    CHECK(out[0].ax == 100);
    CHECK(out[0].bx == 100);  // first step: a dot
    CHECK(out[0].penA == doctest::Approx(0.8));
    CHECK(out[0].radius == doctest::Approx(5));
    h.step({{{140, 210, 0.5}, {50, 60, 1.0}}}, 1.0 / 60, 0.8, 10, W, H, out);
    REQUIRE(out.size() == 3);
    CHECK(out[1].ax == 100);
    CHECK(out[1].bx == 140);
    CHECK(out[1].penB == doctest::Approx(0.4));  // brightness x visibility
    CHECK(out[1].path == 1);
    CHECK(out[2].ax == 50);
    // Lost, then back: starts again with a dot, no line across the gap.
    h.step({{{0, 0, 0.0}, {50, 60, 1.0}}}, 1.0 / 60, 0.8, 10, W, H, out);
    h.step({{{900, 900, 1.0}, {50, 60, 1.0}}}, 1.0 / 60, 0.8, 10, W, H, out);
    CHECK(out[out.size() - 2].ax == 900);
    CHECK(out[out.size() - 2].bx == 900);
    // Brightness 0: nothing.
    const std::size_t n = out.size();
    h.step({{{1, 1, 1.0}, {1, 1, 1.0}}}, 1.0 / 60, 0.0, 10, W, H, out);
    CHECK(out.size() == n);
}
