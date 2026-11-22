// Phase 5D: frame-time instrumentation, vsync-locked fixed step, prepared-config
// cache, non-blocking state publish.

#include <doctest/doctest.h>

#include <atomic>
#include <cmath>
#include <thread>

#include "app/ConfigCache.hpp"
#include "engine/Commands.hpp"
#include "engine/FixedStep.hpp"
#include "engine/Perf.hpp"

using namespace reforce;
using perf::FrameRecord;
using perf::FrameStats;

TEST_CASE("perf: histogram percentiles and coarse bins") {
    perf::Histogram h;
    for (int i = 0; i < 99; ++i) h.add(1.05);
    h.add(30.0);
    CHECK(h.count() == 100);
    CHECK(h.percentile(0.5) == doctest::Approx(1.1));
    CHECK(h.percentile(0.99) == doctest::Approx(1.1));
    CHECK(h.percentile(1.0) == doctest::Approx(30.0));
    CHECK(h.max() == doctest::Approx(30.0));
    const auto c = h.coarse({1, 2, 16.67});
    REQUIRE(c.size() == 4);
    CHECK(c[0] == 0);
    CHECK(c[1] == 99);
    CHECK(c[3] == 1);
}

TEST_CASE("perf: cadence at 60 Hz sim on a 120 Hz display (every refresh)") {
    perf::Cadence c;
    c.budgetMs = c.periodMs = 1000.0 / 120.0;
    c.stepsPerVsync = 0.5;
    int held = -1;
    // Ideal: 0,1,0,1,... -> no hitch after the first step.
    std::uint8_t any = 0;
    for (int f = 0; f < 40; ++f) {
        const auto h = FrameStats::cadenceHitch(c, f % 2, 1, held);
        if (f > 2) any |= h;
        if (f == 1) held = 0;  // first step starts the tracking
    }
    CHECK(any == 0);
    // A sim frame shown 3 refreshes (0,0,1): repeat. Then 1,1: shown too briefly (skip).
    held = 0;
    CHECK(FrameStats::cadenceHitch(c, 0, 1, held) == 0);
    CHECK(FrameStats::cadenceHitch(c, 0, 1, held) == 0);
    CHECK(FrameStats::cadenceHitch(c, 1, 1, held) == perf::HitchRepeat);
    CHECK(FrameStats::cadenceHitch(c, 1, 1, held) == perf::HitchSkip);
    // Two steps in one refresh: skip.
    held = 1;
    CHECK((FrameStats::cadenceHitch(c, 2, 1, held) & perf::HitchSkip) != 0);
}

TEST_CASE("perf: frame stats detect over-budget frames and count causes") {
    FrameStats st(64);
    perf::Cadence c;
    c.budgetMs = c.periodMs = 1000.0 / 60.0;
    c.stepsPerVsync = 1.0;
    st.setCadence(c);
    for (int i = 0; i < 20; ++i) {
        FrameRecord r;
        r.intervalMs = 16.67;
        r.cpuMs = i == 10 ? 30.0 : 2.0;
        r.steps = 1;
        r.tags = i == 10 ? perf::TagFieldBake : 0;
        const auto idx = st.reserve();
        st.setGpu(idx, i == 15 ? 40.0 : 3.0);
        st.add(r);
    }
    std::vector<FrameRecord> hitches;
    CHECK(st.finalize(0, [&](const FrameRecord& r) { hitches.push_back(r); }) == 2);
    const perf::PerfSummary s = st.summary();
    CHECK(s.frames == 20);
    CHECK(s.hitches == 2);
    CHECK(s.byKind[0] == 1);  // cpu
    CHECK(s.byKind[1] == 1);  // gpu
    CHECK(s.byTag[4] == 1);   // field-bake
    CHECK(s.untagged == 1);
    CHECK(s.workMax == doctest::Approx(40.0));
    CHECK(!FrameStats::report(s).empty());
    REQUIRE(hitches.size() == 2);
    CHECK(FrameStats::describe(hitches[0]).find("field-bake") != std::string::npos);
    // GPU time not in yet: not finalized until it arrives (or too old).
    const auto idx = st.reserve();
    st.add(FrameRecord{});
    CHECK(st.finalize(8) == 0);
    CHECK(st.summary().frames == 20);
    st.setGpu(idx, 1.0);
    st.finalize(8);
    CHECK(st.summary().frames == 21);
    st.reset();
    CHECK(st.summary().frames == 0);
}

TEST_CASE("fixed tick: vsync lock gives an exact cadence despite jitter") {
    // 120 Hz callbacks with +-0.5 ms jitter: exactly 0,1,0,1,...
    engine::FixedStepClock c(60.0, 4);
    int prev = -1, bad = 0, steps = 0;
    for (int f = 0; f < 1200; ++f) {
        const double jitter = (f % 3 == 0 ? 0.0005 : (f % 3 == 1 ? -0.0005 : 0.0));
        const int n = c.advanceLocked(1.0 / 120.0 + jitter, 1.0 / 120.0);
        if (n > 1 || (prev >= 0 && n == prev)) ++bad;
        prev = n;
        steps += n;
    }
    CHECK(bad == 0);
    CHECK(steps == 600);
    // 60 Hz pinned link on a 59.94 Hz-ish clock: one step every callback.
    engine::FixedStepClock d(60.0, 4);
    for (int f = 0; f < 3000; ++f) CHECK(d.advanceLocked(1.0 / 59.94, 1.0 / 60.0) == 1);
    // A missed callback (two periods): two steps (sim keeps time).
    CHECK(d.advanceLocked(2.0 / 60.0, 1.0 / 60.0) == 2);
    // Period that is no whole fraction (50 Hz sim on 120 Hz): wall time.
    engine::FixedStepClock e(50.0, 4);
    CHECK(e.lockedDt(0.0085, 1.0 / 120.0) == doctest::Approx(0.0085));
}

TEST_CASE("state box: tryPublish swaps without waiting") {
    engine::StateBox box;
    engine::EngineState s;
    s.simSteps = 7;
    CHECK(box.tryPublish(s));
    CHECK(box.snapshot().simSteps == 7);
    CHECK(s.simSteps == 0);  // the previous state comes back for reuse
}

TEST_CASE("config cache: background prefetch, inline compute, dedup") {
    std::atomic<int> made{0};
    {
        app::ConfigCache cache(1);
        auto make = [&made](int v) {
            return [&made, v]() -> app::ConfigCache::Value {
                ++made;
                return std::make_shared<const int>(v);
            };
        };
        cache.prefetch("a", make(1));
        cache.prefetch("a", make(99));  // known: ignored
        bool here = true;
        auto a = std::static_pointer_cast<const int>(cache.get("a", make(2), &here));
        REQUIRE(a);
        // Either the worker made it (1) or the caller took it over while queued (2).
        CHECK((*a == 1 || *a == 2));
        CHECK(*a == (here ? 2 : 1));
        auto b = std::static_pointer_cast<const int>(cache.get("b", make(3), &here));
        CHECK(here);
        CHECK(*b == 3);
        CHECK(*std::static_pointer_cast<const int>(cache.get("b", make(4), &here)) == 3);
        CHECK(!here);
        CHECK(made == 2);
    }
    app::ConfigCache sync(0);  // no workers: prefetch is a no-op
    sync.prefetch("x", [] { return app::ConfigCache::Value(std::make_shared<const int>(1)); });
    CHECK(sync.size() == 0);
}
