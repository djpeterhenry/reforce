// The knight solid (docs/3d-design.md "Knight's tour"): tours (engine/Knight), the hop timing, the piece mesh and the
// content scenes' timing on the beat clock.
#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <sstream>

#include "engine/Knight.hpp"
#include "engine/Solids.hpp"
#include "scene/Evaluate.hpp"
#include "scene/SceneConfig.hpp"
#include "scene/SceneFile.hpp"
#include "scene/SceneType.hpp"

using namespace reforce::engine;
namespace fs = std::filesystem;
namespace kt = reforce::engine::knight;

TEST_CASE("knight: every tour is a closed tour from home; symmetric ones really are; seeded and varied") {
    const auto t0 = std::chrono::steady_clock::now();
    for (bool sym : {false, true}) {
        std::set<kt::Tour> distinct;
        for (uint64_t seed = 0; seed < 300; ++seed) {
            const kt::Tour t = kt::makeTour(seed, sym);
            REQUIRE(kt::isClosedTour(t));
            CHECK(t[0] == kt::kHome);
            if (sym) CHECK(kt::isSymmetric(t));
            distinct.insert(t);
        }
        CHECK(distinct.size() >= 290);  // a new web every cycle
        CHECK(kt::makeTour(42, sym) == kt::makeTour(42, sym));
    }
    // Other homes, huge seeds.
    for (int home : {0, 27, 63}) {
        const kt::Tour t = kt::makeTour(9, true, home);
        CHECK(kt::isClosedTour(t));
        CHECK(t[0] == home);
        CHECK(kt::isSymmetric(t));
    }
    CHECK(kt::isClosedTour(kt::makeTour(~0ull, false)));
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    MESSAGE("600 tours in " << ms << " ms");
    CHECK(ms < 3000.0);
    // The checks themselves.
    kt::Tour bad = kt::makeTour(1, false);
    std::swap(bad[10], bad[11]);
    CHECK(!kt::isClosedTour(bad));
    kt::Tour dup = kt::makeTour(1, false);
    dup[5] = dup[6];
    CHECK(!kt::isClosedTour(dup));
    CHECK(!kt::isSymmetric(kt::makeTour(3, false)));
    CHECK(kt::isKnightMove(6, 21));   // g1 - f3
    CHECK(!kt::isKnightMove(6, 14));  // g1 - g2
    CHECK(!kt::isKnightMove(7, 8));   // h1 - a2 is not a wrap-around move
    CHECK(!kt::isKnightMove(-1, 6));
}

TEST_CASE("knight: the cache returns the same tours") {
    kt::TourCache c;
    for (uint64_t s = 0; s < 10; ++s) {
        CHECK(c.get(s, true) == kt::makeTour(s, true));
        CHECK(c.get(s, false) == kt::makeTour(s, false));
    }
    CHECK(c.get(3, true) == kt::makeTour(3, true));
}

TEST_CASE("knight: hops land exactly on whole turns, the 64th back home; the flight is continuous") {
    const kt::Tour t = kt::makeTour(5, true), prev = kt::makeTour(4, true);
    for (int k = 0; k <= kt::kSquares; ++k) {
        const kt::Frame f = kt::frameAt(t, prev, double(k));
        const int s = t[std::size_t(k % kt::kSquares)];
        CHECK(f.landed == k);
        CHECK(f.x == doctest::Approx(kt::squareX(s)));
        CHECK(f.y == doctest::Approx(kt::squareY(s)));
        CHECK(f.z == doctest::Approx(0.0));
        CHECK(f.squash == doctest::Approx(1.0));
        CHECK(f.grow == doctest::Approx(0.0));
        if (k > 0) CHECK(f.heading == doctest::Approx(kt::hopHeading(t, k)));
    }
    // At turn 0 the knight faces where the previous tour's last hop left it.
    CHECK(kt::frameAt(t, prev, 0.0).heading == doctest::Approx(kt::hopHeading(prev, 64)));
    // Continuity: small steps of turn move the knight a little; it is in the air mid-flight.
    kt::Frame last = kt::frameAt(t, prev, 0.0);
    float maxStep = 0, maxZ = 0;
    for (int i = 1; i <= 64 * 200; ++i) {
        const kt::Frame f = kt::frameAt(t, prev, i / 200.0);
        maxStep = std::max(maxStep, std::hypot(f.x - last.x, f.y - last.y, f.z - last.z));
        const float dh = std::fabs(std::remainder(f.heading - last.heading, 6.2831853f));
        CHECK(dh < 0.2f);
        maxZ = std::max(maxZ, f.z);
        last = f;
    }
    CHECK(maxStep < 0.1f);
    CHECK(maxZ > 0.6f);
    const kt::Frame mid = kt::frameAt(t, prev, 10.75);
    CHECK(mid.z > 0.5f);
    CHECK(mid.grow > 0.3f);
    CHECK(mid.grow < 0.7f);
    // Junk input: clamped, finite.
    for (double turn : {std::numeric_limits<double>::quiet_NaN(), -1e300, 1e300, std::numeric_limits<double>::infinity(),
                        -5.0, 64.5}) {
        const kt::Frame f = kt::frameAt(t, prev, turn, turn);
        CHECK(std::isfinite(f.x));
        CHECK(std::isfinite(f.y));
        CHECK(std::isfinite(f.z));
        CHECK(std::isfinite(f.heading));
        CHECK(f.landed >= 0);
        CHECK(f.landed <= 64);
    }
    // A build lifts the knight between hops.
    CHECK(kt::frameAt(t, prev, 3.1, 1.0).z > kt::frameAt(t, prev, 3.1, 0.0).z + 0.2f);
}

TEST_CASE("knight: the piece mesh is closed, outward, smooth and the right size") {
    const auto t0 = std::chrono::steady_clock::now();
    const Mesh m = makeMesh(MeshKind::Knight);
    MESSAGE("knight piece built in "
            << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() << " ms");
    CHECK(m.kind == MeshKind::Knight);
    REQUIRE(m.vertices.size() % 3 == 0);
    const std::size_t tris = m.vertices.size() / 3;
    MESSAGE("knight piece: " << tris << " triangles");
    CHECK(tris > 8000);
    CHECK(tris < 80000);
    // Weld by position (surface nets share each vertex exactly), then count edge uses.
    std::map<std::array<float, 3>, int> ids;
    auto id = [&](const MeshVertex& v) {
        const std::array<float, 3> p{v.pos[0], v.pos[1], v.pos[2]};
        auto [it, added] = ids.try_emplace(p, int(ids.size()));
        return it->second;
    };
    std::map<std::pair<int, int>, int> directed;
    double volume = 0;
    float zmin = 1e9f, zmax = -1e9f, rmax = 0;
    int agree = 0;
    for (std::size_t i = 0; i < tris; ++i) {
        const MeshVertex* v = &m.vertices[3 * i];
        const int a = id(v[0]), b = id(v[1]), c = id(v[2]);
        ++directed[{a, b}];
        ++directed[{b, c}];
        ++directed[{c, a}];
        const float* p = v[0].pos;
        const float* q = v[1].pos;
        const float* r = v[2].pos;
        volume += (p[0] * (q[1] * r[2] - q[2] * r[1]) - p[1] * (q[0] * r[2] - q[2] * r[0]) +
                   p[2] * (q[0] * r[1] - q[1] * r[0])) / 6.0;
        const float e1[3] = {q[0] - p[0], q[1] - p[1], q[2] - p[2]}, e2[3] = {r[0] - p[0], r[1] - p[1], r[2] - p[2]};
        const float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        float dn = 0;
        for (int k = 0; k < 3; ++k) dn += n[k] * (v[0].normal[k] + v[1].normal[k] + v[2].normal[k]);
        agree += dn > 0;
        for (int k = 0; k < 3; ++k) {
            zmin = std::min(zmin, v[k].pos[2]);
            zmax = std::max(zmax, v[k].pos[2]);
            rmax = std::max(rmax, std::hypot(v[k].pos[0], v[k].pos[1]));
            CHECK(std::fabs(std::hypot(v[k].normal[0], v[k].normal[1], v[k].normal[2]) - 1.0f) < 1e-3f);
        }
    }
    // Every directed edge once and its reverse once: closed and consistently wound.
    int open = 0;
    for (const auto& [e, n] : directed) {
        const auto rev = directed.find({e.second, e.first});
        open += n != 1 || rev == directed.end() || rev->second != 1;
    }
    MESSAGE("open / non-manifold edges: " << open << " of " << directed.size());
    CHECK(open <= int(directed.size() / 1000));
    CHECK(volume > 0.05);  // outward (counter-clockwise from outside)
    CHECK(agree >= int(tris * 99 / 100));
    CHECK(std::fabs(zmin) < 0.01f);
    CHECK(zmax == doctest::Approx(kt::kPieceHeight).epsilon(0.03));
    CHECK(rmax < 0.6f);
    // The flat parts.
    CHECK(kt::makeBoard().vertices.size() == 64 * 6);
    CHECK(kt::makeDisc(24, 0.0).vertices.size() == 24 * 3);
    CHECK(kt::makeDisc(24, 0.5).vertices.size() == 24 * 6);
}

namespace {
std::string readAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::unique_ptr<SolidRun> loadScene(const char* name) {
    const fs::path f = fs::path(REFORCE_SOURCE_DIR) / "content/Packages/reforce.Solid.package" / (std::string(name) + ".txt");
    REQUIRE(fs::exists(f));
    auto file = reforce::scene::parseSceneText(readAll(f), name);
    auto cfg = std::make_shared<reforce::scene::CompiledConfig>(
        reforce::scene::compileConfig(file, reforce::scene::SceneType::Solid));
    REQUIRE(solidMesh(*cfg) == MeshKind::Knight);
    reforce::scene::Environment env;
    return std::make_unique<SolidRun>(cfg, env, name, MeshKind::Knight, 0.0, 0.0, 40.0, nullptr);
}
}  // namespace

TEST_CASE("knight: the content scenes hop once per beat, landing on the beats, the tour closing on the phrase line") {
    for (const char* name : {"Knight's Tour", "Knight's Web"}) {
        CAPTURE(name);
        auto run = loadScene(name);
        auto at = [&](double beat, double drop = 0, double build = 0) {
            SpriteAudio a;
            a.sync.on = 1;
            a.sync.bpm = 124;
            a.sync.beat = beat;
            a.sync.bar = beat / 4;
            a.sync.phrase = beat / 64;
            a.sync.drop = drop;
            a.sync.build = build;
            return run->evaluate(beat * 60.0 / 124.0, a);
        };
        for (int phrase = 0; phrase < 3; ++phrase) {
            const double b0 = 64.0 * phrase + 1024.0;  // well into the set
            for (int k = 0; k < 64; ++k) {
                const SolidSpec s = at(b0 + k);
                CHECK(s.turn == doctest::Approx(double(k)));  // hop k landed exactly on beat k of the phrase
                CHECK(s.seed == doctest::Approx(16.0 + phrase));
            }
            // Just before the phrase line the 64th hop is about to land home; on the line a new tour starts.
            const SolidSpec end = at(b0 + 63.9999);
            CHECK(end.turn == doctest::Approx(64.0).epsilon(1e-3));
            const kt::Tour t = kt::makeTour(uint64_t(end.seed), end.symmetric);
            const kt::Frame f = kt::frameAt(t, t, end.turn);
            CHECK(f.x == doctest::Approx(kt::squareX(kt::kHome)).epsilon(0.01));
            CHECK(f.y == doctest::Approx(kt::squareY(kt::kHome)).epsilon(0.01));
        }
        // Junk clock values stay finite and clamped.
        for (double beat : {1e300, -1e300, std::numeric_limits<double>::quiet_NaN()}) {
            const SolidSpec s = at(beat, beat, beat);
            CHECK(std::isfinite(s.turn));
            CHECK(std::isfinite(s.seed));
            CHECK(std::isfinite(s.pose.x));
            CHECK(std::isfinite(s.pose.rotX));
            CHECK(s.flash >= 0.0);
            CHECK(s.flash <= 1.0);
        }
        // Without the clock: 120 BPM from the start.
        SpriteAudio none;
        CHECK(run->evaluate(5.0 * 0.5, none).turn == doctest::Approx(5.0));
    }
}

TEST_CASE("knight: the content scene ends on a phrase line of the live clock (tempo changes, clock jumps)") {
    // As the Visualizer starts a solid: the PBar hold, then followBars (engine::BarHold).
    struct Live {
        std::unique_ptr<SolidRun> run;
        double t = 0, bar = 0;
        explicit Live(double fade) {
            const fs::path f = fs::path(REFORCE_SOURCE_DIR) / "content/Packages/reforce.Solid.package/Knight's Tour.txt";
            auto file = reforce::scene::parseSceneText(readAll(f), "Knight's Tour");
            auto cfg = std::make_shared<reforce::scene::CompiledConfig>(
                reforce::scene::compileConfig(file, reforce::scene::SceneType::Solid));
            reforce::scene::Environment env;
            run = std::make_unique<SolidRun>(cfg, env, "Knight's Tour", MeshKind::Knight, 0.0, fade, 40.0, nullptr);
            REQUIRE(run->bars() == doctest::Approx(64));
            reforce::scene::SyncInput s;
            s.on = 1, s.bpm = 125, s.bar = 0, s.beat = 0;
            run->setHold(holdForBars(run->bars(), fade, s, 16, 4.0));
            run->followBars(holdLineBars(run->bars(), 16));
        }
        SolidSpec frame(double bpm) {
            SpriteAudio a;
            a.sync.on = 1, a.sync.bpm = bpm, a.sync.bar = bar, a.sync.beat = 4 * bar, a.sync.phrase = bar / 16;
            const SolidSpec s = run->evaluate(t, a);
            t += 1.0 / 60, bar += bpm / 240.0 / 60;
            return s;
        }
        double untilEnd(double bpm, double upTo) {
            while (bar < upTo) {
                frame(bpm);
                if (run->fadingOut(t)) return bar;
            }
            return -1;
        }
    };
    const double frameBar = 140.0 / 240 / 60;
    {  // steady: four tours, the last hop home on bar 64
        Live r(1.0);
        CHECK(std::abs(r.untilEnd(125, 80) - 64) < 1.5 * frameBar);
    }
    {  // the DJ pushes the tempo mid-scene: fixed seconds would end ~1.3 bars late, mid-tour
        Live r(1.0);
        while (r.bar < 20) r.frame(125);
        CHECK(std::abs(r.untilEnd(128, 80) - 64) < 1.5 * frameBar);
    }
    {  // a deck start moves the phrase lines: the end moves to the nearest phrase line of the new clock
        Live r(1.0);
        while (r.bar < 20) r.frame(125);
        r.bar += 9.0;  // 29 + 44 left = 73 -> the line at 80
        CHECK(std::abs(r.untilEnd(125, 100) - 80) < 1.5 * frameBar);
    }
}
