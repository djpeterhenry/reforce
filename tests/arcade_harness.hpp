#pragma once
// Test helpers for the arcade organics (PLAN 5Q): compile a config, step a run on a given beat with the beat clock on.
#include <doctest/doctest.h>

#include <memory>
#include <string>
#include <vector>

#include "engine/Organics.hpp"
#include "scene/SceneConfig.hpp"
#include "scene/SceneFile.hpp"
#include "scene/SceneType.hpp"

namespace arcade_test {

using namespace reforce::engine;

inline std::shared_ptr<const reforce::scene::CompiledConfig> compile(const std::string& text) {
    auto file = reforce::scene::parseSceneText(text, "test");
    auto cfg = std::make_shared<reforce::scene::CompiledConfig>(
        reforce::scene::compileConfig(file, reforce::scene::SceneType::Organic));
    for (const auto& d : cfg->diags.items()) {
        CAPTURE(d.message);
        CHECK(false);
    }
    return cfg;
}

// A live run (seeded, no fade: full strength from t = 0 for `life` seconds).
struct Run {
    reforce::scene::Environment env;
    std::shared_ptr<const reforce::scene::CompiledConfig> cfg;
    std::unique_ptr<OrganicRun> run;
    FrameMapping map = organicMapping(1920, 1080);
    const OrganicColors* colors = nullptr;  // the ColorMap's colors handed to the body (none by default)
    double t = 0;
    explicit Run(const std::string& text, double life = 600.0) : cfg(compile(text)) {
        run = std::make_unique<OrganicRun>(cfg, env, "arcade", 0.0, 0.0, life, nullptr);
    }
    // One frame at beat `beat` (beat clock on at `bpm`; bar = beat / 4), `dt` seconds after the last.
    // `drop` / `build` set SYNC_DROP / SYNC_BUILD.
    std::vector<LineSegment> at(double beat, double bpm = 120, double drop = 0, double build = 0, double dt = 1.0 / 60) {
        SpriteAudio a;
        a.sync.on = 1;
        a.sync.bpm = bpm;
        a.sync.beat = beat;
        a.sync.bar = beat / 4.0;
        a.sync.phrase = beat / 64.0;
        a.sync.drop = drop;
        a.sync.build = build;
        t += dt;
        std::vector<LineSegment> segs;
        run->step(t, a, map, LineStyle{}, segs, colors);
        return segs;
    }
    // Steps at 60 fps from beat b0 to b1 (at `bpm`), returning the last frame.
    std::vector<LineSegment> play(double b0, double b1, double bpm = 120) {
        std::vector<LineSegment> last;
        const double perFrame = bpm / 60.0 / 60.0;
        for (double b = b0; b <= b1 + 1e-9; b += perFrame) last = at(b, bpm);
        return last;
    }
};

inline bool allFinite(const std::vector<LineSegment>& segs) {
    for (const auto& s : segs)
        if (!std::isfinite(s.ax) || !std::isfinite(s.ay) || !std::isfinite(s.bx) || !std::isfinite(s.by) ||
            !std::isfinite(s.radius) || !(s.penA >= 0 && s.penA <= 1))
            return false;
    return true;
}

}  // namespace arcade_test
