// The fifteen solid (docs/3d-design.md "15 puzzle"): the sliding puzzle (engine/Fifteen) and the content scene's
// timing.
#include <doctest/doctest.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>

#include "engine/Fifteen.hpp"
#include "engine/Solids.hpp"
#include "scene/Evaluate.hpp"
#include "scene/SceneConfig.hpp"
#include "scene/SceneFile.hpp"
#include "scene/SceneType.hpp"

using namespace reforce::engine;
namespace fs = std::filesystem;

TEST_CASE("fifteen: scrambles are legal blank walks, seeded, never straight back, well scrambled") {
    std::array<int8_t, fifteen::kMaxMoves + 1> a{}, b{}, c{};
    CHECK(fifteen::scramble(7, 31, a) == 31);
    fifteen::scramble(7, 31, b);
    fifteen::scramble(8, 31, c);
    CHECK(a == b);
    CHECK(a != c);
    CHECK(a[0] == 15);
    for (int m = 1; m <= 31; ++m) {
        const int p = a[std::size_t(m - 1)], q = a[std::size_t(m)];
        CHECK(std::abs(p / 4 - q / 4) + std::abs(p % 4 - q % 4) == 1);  // one cell, on the board
        if (m >= 2) CHECK(q != a[std::size_t(m - 2)]);                   // never straight back
    }
    CHECK(fifteen::scramble(1, 999, a) == fifteen::kMaxMoves);
    // The pick of the most scrambled walk: tiles well away from home on average.
    int total = 0;
    for (uint64_t seed = 0; seed < 20; ++seed) total += fifteen::frameAt(seed, 31, 0).board.scrambledness();
    CHECK(total / 20 >= 20);
}

TEST_CASE("fifteen: a cycle starts scrambled, slides one tile per step and ends solved") {
    for (uint64_t seed : {0ull, 3ull, 99ull}) {
        const auto s0 = fifteen::frameAt(seed, 31, 0);
        CHECK(!s0.board.isSolved());
        CHECK(fifteen::frameAt(seed, 31, 31).board.isSolved());
        CHECK(fifteen::frameAt(seed, 31, 1e9).board.isSolved());
        for (int k = 0; k < 31; ++k) {
            const auto p = fifteen::frameAt(seed, 31, k), q = fifteen::frameAt(seed, 31, k + 1);
            int changed = 0;
            for (int i = 0; i < 16; ++i) changed += p.board.tile[std::size_t(i)] != q.board.tile[std::size_t(i)];
            CHECK(changed == 2);  // one tile and the blank swap places
            // Mid-slide: exactly one tile is between cells.
            const auto m = fifteen::frameAt(seed, 31, k + 0.5);
            CHECK(m.sliding);
            int between = 0;
            for (const auto& t : m.tiles) {
                const float fx = t.x + 1.5f, fy = 1.5f - t.y;
                between += std::fabs(fx - std::round(fx)) > 0.1f || std::fabs(fy - std::round(fy)) > 0.1f;
            }
            CHECK(between == 1);
        }
        // Every tile once.
        std::set<int> nums;
        for (const auto& t : s0.tiles) nums.insert(t.number);
        CHECK(nums.size() == 15);
        CHECK(*nums.begin() == 1);
        CHECK(*nums.rbegin() == 15);
        // The leap: shuffle 0 = the solved places (lifted 0 at the ends), 1 = the scramble.
        const auto l0 = fifteen::frameAt(seed, 31, 0, 0.0);
        for (const auto& t : l0.tiles) {
            CHECK(t.x == doctest::Approx(fifteen::cellX(t.number - 1)));
            CHECK(t.y == doctest::Approx(fifteen::cellY(t.number - 1)));
            CHECK(t.z == doctest::Approx(0.0));
        }
        float maxZ = 0;
        for (const auto& t : fifteen::frameAt(seed, 31, 0, 0.5).tiles) maxZ = std::max(maxZ, t.z);
        CHECK(maxZ > 0.5f);
    }
}

namespace {
std::string readAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
}  // namespace

TEST_CASE("fifteen: the content scene slides one tile per beat and is solved on every 8-bar line") {
    const fs::path f = fs::path(REFORCE_SOURCE_DIR) / "content/Packages/reforce.Solid.package/Fifteen Glass.txt";
    REQUIRE(fs::exists(f));
    auto file = reforce::scene::parseSceneText(readAll(f), "Fifteen Glass");
    auto cfg = std::make_shared<reforce::scene::CompiledConfig>(
        reforce::scene::compileConfig(file, reforce::scene::SceneType::Solid));
    REQUIRE(solidMesh(*cfg) == MeshKind::Fifteen);
    reforce::scene::Environment env;
    SolidRun run(cfg, env, "Fifteen Glass", MeshKind::Fifteen, 0.0, 0.0, 40.0, nullptr);
    auto at = [&](double beat) {
        SpriteAudio a;
        a.sync.on = 1;
        a.sync.bpm = 120;
        a.sync.beat = beat;
        a.sync.bar = beat / 4;
        a.sync.phrase = beat / 64;
        return run.evaluate(beat / 2, a);
    };
    for (int cycle = 0; cycle < 3; ++cycle) {
        const double b0 = 32.0 * cycle;
        const SolidSpec s0 = at(b0);
        CHECK(s0.moves == 31);
        CHECK(s0.glass);
        CHECK(!s0.colorKey);
        CHECK(s0.turn == doctest::Approx(0.0));
        CHECK(s0.shuffle == doctest::Approx(0.0));        // the leap starts from the solved board
        CHECK(at(b0 + 0.75).shuffle == doctest::Approx(1.0));
        CHECK(at(b0 + 1).turn == doctest::Approx(0.0));   // scrambled, waiting for the first slide
        for (int k = 2; k <= 31; ++k) CHECK(at(b0 + k).turn == doctest::Approx(double(k - 1)));  // one per beat
        const SolidSpec end = at(b0 + 31.999);
        CHECK(end.turn == doctest::Approx(31.0).epsilon(1e-3));
        CHECK(at(b0 + 10).seed == doctest::Approx(double(cycle)));
        // Solved when the last slide lands (just before the line), scrambled again after the leap.
        CHECK(fifteen::frameAt(uint64_t(end.seed), end.moves, 31.0).board.isSolved());
        const SolidSpec next = at(b0 + 32.8);
        CHECK(!fifteen::frameAt(uint64_t(next.seed), next.moves, next.turn, next.shuffle).board.isSolved());
    }
}
