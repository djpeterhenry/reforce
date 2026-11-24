// The rubik solid (docs/3d-design.md "Rubik's cube"): the puzzle (engine/Rubik), the sticker colors per ColorMap
// (engine/StickerColors) and the content scene's timing.
#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>

#include "engine/Rubik.hpp"
#include "engine/Solids.hpp"
#include "engine/StickerColors.hpp"
#include "scene/Evaluate.hpp"
#include "scene/SceneConfig.hpp"
#include "scene/SceneFile.hpp"
#include "scene/SceneType.hpp"

using namespace reforce::engine;
namespace fs = std::filesystem;

TEST_CASE("rubik: solved cube, moves, inverses") {
    rubik::State s = rubik::State::solved();
    CHECK(s.isSolved());
    int stickers = 0;
    for (const auto& c : s.cubies)
        for (int d = 0; d < 6; ++d) stickers += c.face[std::size_t(d)] >= 0;
    CHECK(stickers == 54);
    // Four quarter turns of a face, or a turn and its inverse, give the solved cube back; one turn does not.
    for (int axis = 0; axis < 3; ++axis)
        for (int layer : {-1, 1}) {
            rubik::State t = rubik::State::solved();
            const rubik::Move m{int8_t(axis), int8_t(layer), 1};
            t.apply(m);
            CHECK(!t.isSolved());
            t.apply(rubik::inverse(m));
            CHECK(t.isSolved());
            for (int k = 0; k < 4; ++k) t.apply(m);
            CHECK(t.isSolved());
            rubik::State h = rubik::State::solved();
            h.apply({int8_t(axis), int8_t(layer), 2});
            h.apply({int8_t(axis), int8_t(layer), -2});
            CHECK(h.isSolved());
        }
    // A quarter turn of the +y layer about y moves the +z face's top row onto +x (right-handed: +z -> +x).
    rubik::State u = rubik::State::solved();
    u.apply({1, 1, 1});
    CHECK(u.colorAt(1, 1, 0, 0) == 4);   // +x side of the top layer now shows the old +z color
    CHECK(u.colorAt(1, 1, 1, 2) == 2);   // the top stays the top color
    CHECK(u.colorAt(1, -1, 0, 0) == 0);  // the bottom layer did not move
}

TEST_CASE("rubik: scrambles are seeded and never repeat an axis") {
    std::array<rubik::Move, rubik::kMaxMoves> a{}, b{}, c{};
    CHECK(rubik::scramble(7, 30, a.data()) == 30);
    rubik::scramble(7, 30, b.data());
    rubik::scramble(8, 30, c.data());
    bool same = true, differ = false;
    for (int k = 0; k < 30; ++k) {
        same = same && a[std::size_t(k)].axis == b[std::size_t(k)].axis && a[std::size_t(k)].layer == b[std::size_t(k)].layer &&
               a[std::size_t(k)].turns == b[std::size_t(k)].turns;
        differ = differ || a[std::size_t(k)].axis != c[std::size_t(k)].axis || a[std::size_t(k)].layer != c[std::size_t(k)].layer ||
                 a[std::size_t(k)].turns != c[std::size_t(k)].turns;
        if (k > 0) CHECK(a[std::size_t(k)].axis != a[std::size_t(k - 1)].axis);
        CHECK((a[std::size_t(k)].layer == 1 || a[std::size_t(k)].layer == -1));
        CHECK((a[std::size_t(k)].turns == 1 || a[std::size_t(k)].turns == -1 || a[std::size_t(k)].turns == 2));
    }
    CHECK(same);
    CHECK(differ);
    CHECK(rubik::scramble(1, 99, a.data()) == rubik::kMaxMoves);
}

TEST_CASE("rubik: a cycle is solved at both ends and scrambled in between") {
    for (uint64_t seed : {0ull, 1ull, 42ull, 123456789ull}) {
        CHECK(rubik::frameAt(seed, 30, 0).state.isSolved());
        CHECK(rubik::frameAt(seed, 30, 60).state.isSolved());
        CHECK(rubik::frameAt(seed, 30, 1e9).state.isSolved());  // clamped
        CHECK(rubik::frameAt(seed, 30, -5).state.isSolved());
        CHECK(!rubik::frameAt(seed, 30, 30).state.isSolved());
        // Mirror points of the cycle show the same cube (the solve undoes the scramble).
        for (int k = 0; k <= 30; ++k) {
            const auto p = rubik::frameAt(seed, 30, k).state, q = rubik::frameAt(seed, 30, 60 - k).state;
            bool eq = true;
            for (int x = -1; x <= 1; ++x)
                for (int y = -1; y <= 1; ++y)
                    for (int z = -1; z <= 1; ++z)
                        for (int d = 0; d < 6; ++d) eq = eq && p.colorAt(x, y, z, d) == q.colorAt(x, y, z, d);
            CHECK(eq);
        }
        // Mid-turn: the state before the turn and the angle so far.
        const auto f = rubik::frameAt(seed, 30, 40.5);
        CHECK(f.turning);
        CHECK(f.index == 40);
        CHECK(std::fabs(f.angle) == doctest::Approx(std::fabs(f.move.turns) * std::acos(-1.0) / 4));
        CHECK(!rubik::frameAt(seed, 30, 41).turning);
    }
    CHECK(!rubik::frameAt(1, 30, std::nan("")).turning);
}

namespace {

Palette rampPalette(float r0, float g0, float b0, float r1, float g1, float b1) {
    Palette p{};
    for (int k = 0; k < kPaletteSize; ++k) {
        const float f = float(k) / 255.0f;
        p[std::size_t(k)] = {r0 + (r1 - r0) * f, g0 + (g1 - g0) * f, b0 + (b1 - b0) * f, 1.0f};
    }
    return p;
}
MapLut identityLut() {
    MapLut l{};
    for (int k = 0; k < kPaletteSize; ++k) l[std::size_t(k)] = float(k) / 255.0f;
    return l;
}
Palette hsvRainbow() {
    Palette p{};
    for (int k = 0; k < kPaletteSize; ++k) {
        const float h = float(k) / 256.0f * 6.0f, v = std::min(1.0f, float(k) / 40.0f);
        const int i = int(h);
        const float f = h - float(i);
        float r = 0, g = 0, b = 0;
        switch (i % 6) {
            case 0: r = 1, g = f; break;
            case 1: r = 1 - f, g = 1; break;
            case 2: g = 1, b = f; break;
            case 3: g = 1 - f, b = 1; break;
            case 4: r = f, b = 1; break;
            default: r = 1, b = 1 - f; break;
        }
        p[std::size_t(k)] = {r * v, g * v, b * v, 1.0f};
    }
    return p;
}

}  // namespace

TEST_CASE("stickers: six distinct colors per ColorMap") {
    const MapLut lut = identityLut();
    LabTable lab{};
    SUBCASE("gray: spread over the lightness, clear of the black plastic") {
        effectiveColors(rampPalette(0, 0, 0, 1, 1, 1), lut, lab);
        const StickerPick p = pickStickers(lab);
        std::set<int> levels(p.level.begin(), p.level.end());
        CHECK(levels.size() == 6);
        CHECK(p.score > 10.0f);  // 100 L* over 7 colors, net of the wobble
        for (int l : p.level) CHECK(l >= 16);
    }
    SUBCASE("hue wheel: far better than evenly spaced levels") {
        effectiveColors(hsvRainbow(), lut, lab);
        const StickerPick p = pickStickers(lab);
        CHECK(p.score > 25.0f);
        CHECK(p.score >= stickerScore(lab, {77, 112, 148, 184, 219, 255}));
    }
    SUBCASE("the closest pair goes on opposite faces") {
        effectiveColors(rampPalette(0, 0, 0, 1, 1, 1), lut, lab);
        const StickerPick p = pickStickers(lab);
        float closest = 1e9f;
        int ci = -1, cj = -1;
        for (int i = 0; i < 6; ++i)
            for (int j = i + 1; j < 6; ++j) {
                const float d = deltaE(lab[std::size_t(p.level[std::size_t(i)])], lab[std::size_t(p.level[std::size_t(j)])]);
                if (d < closest) closest = d, ci = i, cj = j;
            }
        CHECK(ci / 2 == cj / 2);
    }
    SUBCASE("the MapFunction LUT is applied first") {
        MapLut inv{};
        for (int k = 0; k < kPaletteSize; ++k) inv[std::size_t(k)] = 1.0f - float(k) / 255.0f;
        effectiveColors(rampPalette(0, 0, 0, 1, 1, 1), inv, lab);
        CHECK(lab[0].l == doctest::Approx(100.0f).epsilon(0.01));
        CHECK(lab[255].l == doctest::Approx(0.0f).epsilon(0.01));
    }
}

TEST_CASE("stickers: the picker keeps its colors and eases to new ones") {
    StickerPicker pk(0.5);
    const MapLut lut = identityLut();
    const Palette gray = rampPalette(0, 0, 0, 1, 1, 1);
    pk.update(gray, lut, 0.0);
    const auto a = pk.levels(0.0);
    pk.update(gray, lut, 1.0);  // unchanged map: same levels, no switch
    CHECK(pk.levels(1.0) == a);
    CHECK(pk.switches() == 0);
    // A slightly different map: the current set still scores about as well, so it is kept.
    pk.update(rampPalette(0, 0, 0, 0.98f, 0.98f, 0.98f), lut, 2.0);
    CHECK(pk.switches() == 0);
    // A very different map: switch, easing over half a second.
    pk.update(hsvRainbow(), lut, 3.0);
    CHECK(pk.switches() == 1);
    const auto b0 = pk.levels(3.0), b1 = pk.levels(3.25), b2 = pk.levels(3.5), b3 = pk.levels(9.0);
    CHECK(b0 == pk.levels(3.0));
    CHECK(b2 == b3);
    bool between = false;
    for (int i = 0; i < 6; ++i) between = between || (b1[std::size_t(i)] != b0[std::size_t(i)] && b1[std::size_t(i)] != b2[std::size_t(i)]);
    CHECK(between);
}

namespace {
std::string readAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
}  // namespace

TEST_CASE("rubik: the content scene is solved on every 8-bar line") {
    const fs::path f = fs::path(REFORCE_SOURCE_DIR) / "content/Packages/reforce.Solid.package/Rubik Cube.txt";
    REQUIRE(fs::exists(f));
    auto file = reforce::scene::parseSceneText(readAll(f), "Rubik Cube");
    auto cfg = std::make_shared<reforce::scene::CompiledConfig>(
        reforce::scene::compileConfig(file, reforce::scene::SceneType::Solid));
    REQUIRE(solidMesh(*cfg) == MeshKind::Rubik);
    reforce::scene::Environment env;
    SolidRun run(cfg, env, "Rubik Cube", MeshKind::Rubik, 0.0, 0.0, 40.0, nullptr);
    // Locked clock at 120 BPM: beat b at t = b / 2, bars of 4 beats from beat 0.
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
        CHECK(s0.moves == 30);
        CHECK(!s0.colorKey);
        CHECK(s0.turn == doctest::Approx(0.0));
        CHECK(rubik::frameAt(uint64_t(s0.seed), s0.moves, s0.turn).state.isSolved());
        CHECK(at(b0 + 2).turn == doctest::Approx(30.0));                 // scrambled after two beats
        CHECK(at(b0 + 1).turn == doctest::Approx(7.5));                  // speeding up
        for (int k = 3; k <= 31; ++k) CHECK(at(b0 + k).turn == doctest::Approx(30.0 + (k - 2)));  // one per beat
        CHECK(at(b0 + 31.999).turn == doctest::Approx(60.0).epsilon(1e-3));
        const SolidSpec s1 = at(b0 + 31.999);
        CHECK(rubik::frameAt(uint64_t(s1.seed), s1.moves, s1.turn).state.isSolved() == (s1.turn >= 60.0));
        CHECK(at(b0 + 10).seed == doctest::Approx(double(cycle)));
    }
}

TEST_CASE("stickers: cost of a pick") {
    LabTable lab{};
    effectiveColors(hsvRainbow(), identityLut(), lab);
    const auto t0 = std::chrono::steady_clock::now();
    float s = 0;
    for (int k = 0; k < 20; ++k) s += pickStickers(lab).score;
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 20;
    MESSAGE("pickStickers: " << ms << " ms (runs at most every 0.1 s, only while the ColorMap changes)");
    CHECK(s > 0);
}

TEST_CASE("fifteen tiles: light rows with dark numbers, dark rows only on a map without light colors; no flips") {
    const MapLut lut = identityLut();
    LabTable lab{};
    effectiveColors(rampPalette(0, 0, 0, 1, 1, 1), lut, lab);
    const TilePick l = pickTiles(lab, true);
    for (int k : l.level) CHECK(lab[std::size_t(k)].l >= 45.0f);
    CHECK(l.score > 8.0f);
    const TilePick d = pickTiles(lab, false);
    for (int k : d.level) {
        CHECK(lab[std::size_t(k)].l >= 15.0f);
        CHECK(lab[std::size_t(k)].l <= 40.0f);
    }
    TilePicker pk(0.5);
    pk.update(rampPalette(0, 0, 0, 1, 1, 1), lut, 0.0);
    CHECK(pk.light());
    CHECK(pk.numeral(0.0) == doctest::Approx(0.0f));  // dark numbers
    // A map that is dark everywhere: dark tiles, light numbers, faded in over half a second.
    pk.update(rampPalette(0, 0, 0, 0.3f, 0.3f, 0.3f), lut, 1.0);
    CHECK(!pk.light());
    CHECK(pk.numeral(1.25) > 0.1f);
    CHECK(pk.numeral(1.25) < 0.9f);
    CHECK(pk.numeral(2.0) == doctest::Approx(1.0f));
    // Small changes keep the choice and the colors.
    const auto before = pk.levels(5.0);
    const int sw = pk.switches();
    pk.update(rampPalette(0, 0, 0, 0.31f, 0.31f, 0.31f), lut, 5.0);
    CHECK(pk.switches() == sw);
    CHECK(pk.levels(5.0) == before);
}


namespace {
// Black to white over the lower half, then white: .6 and 1 (the hopper's default tops) are both white.
Palette flatTopPalette() {
    Palette p{};
    for (int k = 0; k < kPaletteSize; ++k) {
        const float v = std::min(1.0f, float(k) / 127.0f);
        p[std::size_t(k)] = {v, v, v, 1.0f};
    }
    return p;
}
}  // namespace

TEST_CASE("hopper tops: picked so the states read where the fixed levels look the same") {
    const MapLut lut = identityLut();
    LabTable lab{};
    effectiveColors(flatTopPalette(), lut, lab);
    TopSpec sp;  // prefer .6 / 1, seams .04, sides .22 / .4
    sp.prefer = {153, 204, 255};
    const float fixed = topScore(lab, sp, {153, 255, 255});
    const TopPick p = pickTops(lab, sp);
    CHECK(fixed < 5.0f);
    CHECK(p.score > 20.0f);
    CHECK(p.level[0] < p.level[1]);
    CHECK(lab[std::size_t(p.level[1])].l >= lab[std::size_t(p.level[0])].l);  // bright looks at least as light
    // Three states.
    sp.n = 3;
    const TopPick p3 = pickTops(lab, sp);
    CHECK(p3.level[0] < p3.level[1]);
    CHECK(p3.level[1] < p3.level[2]);
    CHECK(p3.score > 8.0f);
    // Never worse than the preferred levels, on any of these maps.
    for (const Palette& pal : {flatTopPalette(), rampPalette(0, 0, 0, 1, 1, 1), hsvRainbow(), rampPalette(0.2f, 0, 0.4f, 1, 0.8f, 0)}) {
        effectiveColors(pal, lut, lab);
        for (int n : {2, 3}) {
            sp.n = n;
            const std::array<int, 3> pref = n == 2 ? std::array<int, 3>{153, 255, 255} : sp.prefer;
            CHECK(pickTops(lab, sp).score >= topScore(lab, sp, pref) - 1e-3f);
        }
    }
}

TEST_CASE("hopper tops: the picker keeps its levels on small changes and eases to new ones") {
    const MapLut lut = identityLut();
    LabTable gray{}, flat{};
    effectiveColors(rampPalette(0, 0, 0, 1, 1, 1), lut, gray);
    effectiveColors(flatTopPalette(), lut, flat);
    TopSpec sp;
    TopPicker pk(0.5);
    CHECK(!pk.have());
    pk.update(gray, 1, sp, 0.0);
    REQUIRE(pk.have());
    const auto a = pk.levels(0.0);
    pk.update(gray, 1, sp, 0.5);  // same version: nothing to do
    CHECK(pk.switches() == 0);
    pk.update(flat, 2, sp, 1.0);
    CHECK(pk.switches() == 1);
    const auto mid = pk.levels(1.25), b = pk.levels(2.0);
    CHECK(a[0] != b[0]);
    CHECK(((mid[0] - a[0]) * (b[0] - mid[0]) > 0));  // on the way
    // Within the minimum interval a new version waits for a later call.
    pk.update(gray, 3, sp, 1.05);
    CHECK(pk.switches() == 1);
    pk.update(gray, 3, sp, 3.0);
    CHECK(pk.levels(10.0)[0] * 255 == doctest::Approx(double(pickTops(gray, sp).level[0])).epsilon(0.01));
}
