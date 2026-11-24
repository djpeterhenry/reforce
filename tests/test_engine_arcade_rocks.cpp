// Arcade organic `rocks` (PLAN 5Q.5, docs/arcade-design.md): hits on the beat lines, splits, waves on the phrase
// lines, the drop's shockwave, the breakdown's fire rate, determinism, the fallback clock, junk and bounds.
#include <doctest/doctest.h>

#include <algorithm>

#include <chrono>
#include <cmath>
#include <memory>
#include <vector>

#include "PerfBudget.hpp"
#include "arcade_harness.hpp"
#include "engine/OrganicKinds.hpp"
#include "engine/arcade/Arcade.hpp"
#include "engine/arcade/Rocks.hpp"

using namespace reforce::engine;

namespace {

// Drives a rocks body directly (so the test can read its stats), one frame at a time.
struct Driver {
    std::shared_ptr<const reforce::scene::CompiledConfig> cfg;
    reforce::scene::Environment env;
    std::unique_ptr<reforce::scene::OrganicInstance> inst;
    std::unique_ptr<OrganicBody> body;
    FrameMapping map = organicMapping(1920, 1080);
    double barShift = 0;  // bar = beat / 4 + barShift (the phrase clock's alignment)
    explicit Driver(const std::string& text, std::uint64_t seed = 1) : cfg(arcade_test::compile(text)) {
        inst = std::make_unique<reforce::scene::OrganicInstance>(*cfg, env);
        body = arcade::makeRocks(*inst, seed);
    }
    std::vector<LineSegment> at(double beat, double drop = 0, double build = 0, double dt = 1.0 / 60,
                                bool clock = true) {
        SpriteAudio a;
        a.sync.on = clock ? 1 : 0;
        a.sync.drop = drop;
        a.sync.build = build;
        OrganicStep st;
        st.beat = beat;
        st.bar = beat / 4.0 + barShift;
        st.clock = clock;
        st.pen = 1;
        st.dt = dt;
        st.audio = &a;
        std::vector<LineSegment> out;
        const LineStyle style;
        OrganicCanvas c(map, style, out, 0, 0, 1, 0);
        body->step(st, c);
        return out;
    }
    // 60 fps at 120 BPM (1/30 beat a frame) from b0 to b1; returns the last frame.
    std::vector<LineSegment> play(double b0, double b1, double build = 0, double perFrame = 1.0 / 30) {
        std::vector<LineSegment> last;
        const int n = int(std::lround((b1 - b0) / perFrame));
        for (int i = 0; i <= n; ++i) last = at(b0 + i * perFrame, 0, build, perFrame / 2);
        return last;
    }
    const arcade::RocksStats& stats() const { return *arcade::rocksStats(*body); }
};

bool onGrid(double beat, double step) {
    const double k = beat / step;
    return std::abs(k - std::round(k)) < 1e-9;
}

}  // namespace

TEST_CASE("arcade rocks: draws, finite, on the beat clock") {
    arcade_test::Run r("Kind=\"rocks\"\n");
    const auto segs = r.play(0, 8);
    CHECK(!segs.empty());
    CHECK(arcade_test::allFinite(segs));
    CHECK(r.run->summary().find("rocks per wave") != std::string::npos);
}

TEST_CASE("arcade rocks: shots leave on the 8th-note lines, hits land on the beat lines, one per line") {
    Driver d("Kind=\"rocks\"\n");
    d.play(0, 40);
    const auto& st = d.stats();
    REQUIRE(st.hits >= 20);
    CHECK(st.fired >= 70);
    CHECK(st.chips > 0);  // the shots between the hit lines connect too
    for (double b : st.fireBeats) CHECK(onGrid(b, 0.5));
    for (std::size_t i = 0; i < st.hitBeats.size(); ++i) {
        CAPTURE(st.hitBeats[i]);
        CHECK(onGrid(st.hitBeats[i], 1.0));
        if (i > 0) CHECK(st.hitBeats[i] > st.hitBeats[i - 1]);
    }

    // Hits = 2: on the half beats; Hits = .5: every other beat; Fire = 4: shots on 16ths.
    Driver h2("Kind=\"rocks\"\nHits=\"2\"\nFire=\"4\"\n");
    h2.play(0, 24);
    bool half = false;
    for (double b : h2.stats().hitBeats) {
        CHECK(onGrid(b, 0.5));
        half = half || !onGrid(b, 1.0);
    }
    CHECK(half);
    for (double b : h2.stats().fireBeats) CHECK(onGrid(b, 0.25));
    Driver h05("Kind=\"rocks\"\nHits=\".5\"\n");
    h05.play(0, 24);
    REQUIRE(h05.stats().hits >= 6);
    for (double b : h05.stats().hitBeats) CHECK(onGrid(b, 2.0));
}

TEST_CASE("arcade rocks: a hit splits large into two medium, medium into two small, small into debris") {
    Driver d("Kind=\"rocks\"\nNum=\"3\"\nWave=\"0\"\n");
    d.at(0);
    REQUIRE(d.stats().large == 3);
    CHECK(d.stats().medium == 0);
    bool seen[3] = {false, false, false};
    for (int i = 1; i <= 30 * 30; ++i) {
        const auto before = d.stats();
        d.at(i / 30.0);
        const auto& after = d.stats();
        if (after.hits == before.hits || after.waves != before.waves) continue;
        REQUIRE(after.hits == before.hits + 1);
        const int size = after.hitSizes.back();
        seen[size] = true;
        CAPTURE(size);
        if (size == 0) {
            CHECK(after.large == before.large - 1);
            CHECK(after.medium == before.medium + 2);
        } else if (size == 1) {
            CHECK(after.medium == before.medium - 1);
            CHECK(after.small == before.small + 2);
        } else {
            CHECK(after.small == before.small - 1);
            CHECK(after.rocks == before.rocks - 1);
        }
        CHECK(after.debris > before.debris);
    }
    CHECK(seen[0]);
    CHECK(seen[1]);
    CHECK(seen[2]);
}

TEST_CASE("arcade rocks: fresh rocks on the phrase lines (aligned to the phrase clock), and when the field is clear") {
    Driver d("Kind=\"rocks\"\nWave=\"16\"\n");
    d.barShift = 0.25;  // bar lines a beat early: the phrase line (bar 16) is beat 63
    d.play(0, 70);
    const auto& w = d.stats().waveBeats;
    REQUIRE(!w.empty());
    CHECK(w.front() == 0.0);  // the first wave is there from the start
    bool phrase = false;
    for (std::size_t i = 1; i < w.size(); ++i) {
        CAPTURE(w[i]);
        CHECK(onGrid(w[i] + 1, 4.0));  // on a bar line
        phrase = phrase || w[i] == 63.0;
    }
    CHECK(phrase);

    // One rock, cleared fast: the next wave comes on the next bar line, not the phrase.
    Driver c("Kind=\"rocks\"\nNum=\"1\"\nHits=\"4\"\nFire=\"4\"\nWave=\"0\"\n");
    c.play(0, 30);
    REQUIRE(c.stats().waves >= 2);
    for (double b : c.stats().waveBeats) CHECK(onGrid(b, 4.0));
}

TEST_CASE("arcade rocks: a drop sends a shockwave that shatters the field within about a beat") {
    Driver d("Kind=\"rocks\"\nNum=\"6\"\nWave=\"0\"\n");
    d.play(0, 7);
    REQUIRE(d.stats().rocks > 0);
    // SYNC_DROP rising at beat 8, then falling over a bar.
    const double frame = 1.0 / 30;
    for (int i = 0; i <= 45; ++i) {
        const double b = 8.0 + i * frame;
        d.at(b, std::exp(-(b - 8.0) / 4.0));
    }
    const auto& st = d.stats();
    CHECK(st.shockBeat == doctest::Approx(8.0));
    CHECK(st.rocks == 0);
    CHECK(st.shattered > 0);
    CHECK(st.debris > 30);
    // The ring is drawn: segments well away from the ship while it is out.
    Driver r("Kind=\"rocks\"\nNum=\"6\"\nWave=\"0\"\n");
    r.play(0, 7.9);
    r.at(8.0, 1.0);
    const auto segs = r.at(8.5, 0.9);
    int far = 0;
    for (const auto& s : segs)
        if (std::hypot(s.ax - 960, s.ay - 540) > 400) ++far;
    CHECK(far > 40);
    // Holding SYNC_DROP high does not fire again; the field refills on a later bar line.
    const double first = d.stats().shockBeat;
    for (int i = 0; i < 300; ++i) d.at(9.6 + i * frame, 0.7);
    CHECK(d.stats().shockBeat == first);
    CHECK(d.stats().rocks > 0);
}

TEST_CASE("arcade rocks: through a breakdown the fire rate doubles (8ths -> 16ths) and the rocks speed up") {
    Driver d("Kind=\"rocks\"\n");
    d.play(0, 8);
    CHECK(d.stats().fireStepTicks == arcade::kRocksTicksPerBeat / 2);
    const long long before = d.stats().fired;
    d.play(8 + 1.0 / 30, 16, 0.8);
    CHECK(d.stats().fireStepTicks == arcade::kRocksTicksPerBeat / 4);
    CHECK(d.stats().fired - before >= 30);  // 32 lines in 8 beats (none while a target is gone? all fire)
    bool sixteenth = false;
    for (double b : d.stats().fireBeats)
        if (b > 8.0) sixteenth = sixteenth || !onGrid(b, 0.5);
    CHECK(sixteenth);

    // Faster rocks: the same seed with and without the build, the outlines move further.
    auto centroid = [](const std::vector<LineSegment>& s) {
        double x = 0;
        for (const auto& q : s) x += q.ax;
        return s.empty() ? 0.0 : x / double(s.size());
    };
    Driver calm("Kind=\"rocks\"\nFire=\"1\"\nHits=\".125\"\n"), fast("Kind=\"rocks\"\nFire=\"1\"\nHits=\".125\"\n");
    const auto c0 = calm.at(0), f0 = fast.at(0);
    REQUIRE(centroid(c0) == centroid(f0));
    // Between the first ticks (no shot has arrived yet), only the drift differs.
    const auto c1 = calm.at(0.3), f1 = fast.at(0.3, 0, 1.0);
    CHECK(std::abs(centroid(f1) - centroid(f0)) > std::abs(centroid(c1) - centroid(c0)));
}

TEST_CASE("arcade rocks: deterministic per seed, the same game at any frame rate") {
    Driver a("Kind=\"rocks\"\n", 7), b("Kind=\"rocks\"\n", 7), c("Kind=\"rocks\"\n", 8);
    bool differ = false;
    for (int i = 0; i <= 900; ++i) {
        const double beat = i / 30.0, drop = i == 600 ? 1.0 : 0.0, build = i > 300 && i < 450 ? 0.7 : 0.0;
        const auto sa = a.at(beat, drop, build), sb = b.at(beat, drop, build), sc = c.at(beat, drop, build);
        REQUIRE(sa.size() == sb.size());
        for (std::size_t k = 0; k < sa.size(); ++k) {
            REQUIRE(sa[k].ax == sb[k].ax);
            REQUIRE(sa[k].ay == sb[k].ay);
            REQUIRE(sa[k].bx == sb[k].bx);
            REQUIRE(sa[k].penA == sb[k].penA);
        }
        differ = differ || sa.size() != sc.size() || (!sa.empty() && sa[0].ax != sc[0].ax);
    }
    CHECK(differ);

    // Through OrganicRun: two runs of the same config draw the same.
    arcade_test::Run r1("Kind=\"rocks\"\n"), r2("Kind=\"rocks\"\n");
    const auto s1 = r1.play(0, 12), s2 = r2.play(0, 12);
    REQUIRE(s1.size() == s2.size());
    for (std::size_t k = 0; k < s1.size(); ++k) CHECK(s1[k].ax == s2[k].ax);

    // 60 fps and 24 fps reach the same game state on the same beats.
    Driver f60("Kind=\"rocks\"\n", 3), f24("Kind=\"rocks\"\n", 3);
    f60.play(0, 30, 0, 1.0 / 30);
    f24.play(0, 30, 0, 1.0 / 12);
    CHECK(f60.stats().hitBeats == f24.stats().hitBeats);
    CHECK(f60.stats().fireBeats == f24.stats().fireBeats);
    CHECK(f60.stats().rocks == f24.stats().rocks);
}

TEST_CASE("arcade rocks: without a beat clock it runs on 120 BPM from the start") {
    // OrganicRun: SYNC_ON = 0, so the beat is 2 per second of age.
    auto cfg = arcade_test::compile("Kind=\"rocks\"\n");
    reforce::scene::Environment env;
    OrganicRun run(cfg, env, "rocks", 0.0, 0.0, 600.0, nullptr);
    SpriteAudio a;
    std::size_t most = 0;
    for (int i = 0; i < 600; ++i) {
        std::vector<LineSegment> segs;
        run.step(i / 60.0, a, organicMapping(1280, 720), LineStyle{}, segs);
        CHECK(arcade_test::allFinite(segs));
        most = std::max(most, segs.size());
    }
    CHECK(most > 50);
    // The body on that fallback clock: hits on its beat lines.
    Driver d("Kind=\"rocks\"\n");
    for (int i = 0; i < 1200; ++i) d.at(2.0 * i / 60.0, 0, 0, 1.0 / 60, false);
    REQUIRE(d.stats().hits >= 10);
    for (double b : d.stats().hitBeats) CHECK(onGrid(b, 1.0));
}

TEST_CASE("arcade rocks: junk keys and junk beats stay finite and bounded") {
    const char* configs[] = {
        "Kind=\"rocks\"\nNum=\"1e9\"\nFire=\"-5\"\nHits=\"0\"\nSpd=\"0/0\"\nWdth=\"-3\"\nSize=\"1e9\"\nWave=\"-1\"\nGlow=\"9\"\n",
        "Kind=\"rocks\"\nNum=\"-4\"\nFire=\"1e300\"\nHits=\"1e300\"\nSpd=\"1e300\"\nWdth=\"1e300\"\nSize=\"0\"\nWave=\"1e300\"\n",
        "Kind=\"rocks\"\nFire=\"3\"\nHits=\"3\"\nGlow=\"0\"\nX=\"1e300\"\nScal=\"0/0\"\nRot=\"1e300\"\nPen=\"-1\"\n",
    };
    for (const char* text : configs) {
        CAPTURE(text);
        Driver d(text);
        for (int i = 0; i < 400; ++i) {
            const auto segs = d.at(i / 30.0, i == 200 ? 1 : 0, i > 250 ? 1 : 0);
            REQUIRE(arcade_test::allFinite(segs));
            CHECK(segs.size() < 8000);
        }
        for (double junk : {std::nan(""), 1e300, -1e300, 1e15, -50.0, 3.0}) {
            const auto segs = d.at(junk);
            CHECK(arcade_test::allFinite(segs));
        }
        arcade_test::Run r(text);
        CHECK(arcade_test::allFinite(r.play(0, 6)));
    }
}

TEST_CASE("arcade rocks: segments, rocks, debris and shots stay bounded; a step is cheap") {
    Driver d("Kind=\"rocks\"\nNum=\"12\"\nFire=\"16\"\nHits=\"16\"\nSize=\"3\"\nWave=\"1\"\nSpd=\"1\"\n");
    std::size_t most = 0;
    int mostRocks = 0, mostDebris = 0, mostShots = 0;
    double total = 0, worst = 0;
    int frames = 0;
    for (int i = 0; i <= 30 * 160; ++i) {
        const double beat = i / 30.0;
        const double drop = std::fmod(beat, 16.0) < 0.5 ? 1.0 : 0.0;
        const double build = std::fmod(beat, 32.0) > 20 ? 1.0 : 0.0;
        const auto t0 = std::chrono::steady_clock::now();
        const auto segs = d.at(beat, drop, build);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        total += ms;
        worst = std::max(worst, ms);
        ++frames;
        most = std::max(most, segs.size());
        mostRocks = std::max(mostRocks, d.stats().rocks);
        mostDebris = std::max(mostDebris, d.stats().debris);
        mostShots = std::max(mostShots, d.stats().shots);
    }
    MESSAGE("rocks stress: most " << most << " segments, " << mostRocks << " rocks, " << mostDebris << " debris, "
                                  << mostShots << " shots; step " << total / frames << " ms avg, " << worst << " max");
    CHECK(most < 8000);
    CHECK(mostRocks <= 48);
    CHECK(mostDebris <= 640);
    CHECK(mostShots <= 32);
    PERF_BUDGET(total / frames < 0.5);
}

TEST_CASE("arcade rocks: a big jump in the beat catches up a bounded number of ticks, then plays on") {
    Driver d("Kind=\"rocks\"\n");
    d.play(0, 10);
    const auto segs = d.at(5000.0);
    CHECK(arcade_test::allFinite(segs));
    CHECK(d.stats().lastStepTicks <= 4 * arcade::kRocksTicksPerBeat);
    CHECK(d.stats().lastStepTicks > 0);
    const long long hits = d.stats().hits;
    d.play(5000.0 + 1.0 / 30, 5016);
    CHECK(d.stats().hits > hits + 8);
    for (double b : d.stats().hitBeats) CHECK(onGrid(b, 1.0));
    // And back again (a clock reset): it carries on from there.
    d.at(3.0);
    d.play(3.0 + 1.0 / 30, 12);
    CHECK(d.stats().hitBeats.back() < 13);
    for (double b : d.stats().hitBeats) CHECK(onGrid(b, 1.0));
}
