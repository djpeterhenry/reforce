// The soma solid (docs/3d-design.md "Soma cube"): the pieces, the exact-cover solver (the standard solution counts),
// the figures, the assembly order, the round's timing and the content scenes.
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <string>
#include <memory>
#include <set>
#include <sstream>
#include <vector>

#include "engine/PieceColors.hpp"
#include "engine/Soma.hpp"
#include "engine/Solids.hpp"
#include "scene/Evaluate.hpp"
#include "scene/SceneConfig.hpp"
#include "scene/SceneFile.hpp"
#include "scene/SceneType.hpp"

using namespace reforce::engine;
using namespace reforce::engine::soma;
namespace fs = std::filesystem;

namespace {

using Shape = std::vector<std::array<int, 3>>;

Shape normalized(Shape s) {
    int mx = 99, my = 99, mz = 99;
    for (auto& c : s) mx = std::min(mx, c[0]), my = std::min(my, c[1]), mz = std::min(mz, c[2]);
    for (auto& c : s) c = {c[0] - mx, c[1] - my, c[2] - mz};
    std::sort(s.begin(), s.end());
    return s;
}
Shape shapeOf(int p) {
    Shape s;
    const Piece& pc = pieces()[std::size_t(p)];
    for (int i = 0; i < pc.count; ++i) s.push_back({pc.cells[std::size_t(i)].x, pc.cells[std::size_t(i)].y, pc.cells[std::size_t(i)].z});
    return s;
}
Shape rotated(const Shape& s, const Rot& r) {
    Shape o;
    for (const auto& c : s) {
        const Cell x = rotate(r, {int8_t(c[0]), int8_t(c[1]), int8_t(c[2])});
        o.push_back({x.x, x.y, x.z});
    }
    return normalized(o);
}
Shape mirrored(const Shape& s) {
    Shape o = s;
    for (auto& c : o) c[0] = -c[0];
    return normalized(o);
}
bool congruent(const Shape& a, const Shape& b) {
    const Shape nb = normalized(b);
    for (const Rot& r : rotations())
        if (rotated(a, r) == nb) return true;
    return false;
}

// The 48 symmetries of the 3 x 3 x 3 cube (signed permutation matrices); `proper` = a rotation.
struct Sym {
    std::array<int, 9> m{};
    bool proper = true;
};
std::vector<Sym> cubeSymmetries() {
    std::vector<Sym> out;
    static constexpr int kPerm[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
    for (const auto& perm : kPerm)
        for (int signs = 0; signs < 8; ++signs) {
            Sym s;
            for (int row = 0; row < 3; ++row) s.m[std::size_t(row * 3 + perm[row])] = (signs >> row) & 1 ? -1 : 1;
            const auto& m = s.m;
            const int det = m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) +
                            m[2] * (m[3] * m[7] - m[4] * m[6]);
            s.proper = det == 1;
            out.push_back(s);
        }
    return out;
}

using Grid = std::array<int8_t, 27>;
Grid cubeGrid(const Solution& s) {
    const auto l = labels(s);
    Grid g{};
    for (int x = 0; x < 3; ++x)
        for (int y = 0; y < 3; ++y)
            for (int z = 0; z < 3; ++z) g[std::size_t((x * 3 + y) * 3 + z)] = l[std::size_t((x * kGrid + y) * kGrid + z)];
    return g;
}
// The smallest image of `g` under the symmetries (with `reflections`, the improper ones too, which turn screw A into
// screw B and back).
Grid canonical(const Grid& g, const std::vector<Sym>& syms, bool reflections) {
    Grid best{};
    bool have = false;
    for (const Sym& s : syms) {
        if (!s.proper && !reflections) continue;
        Grid h{};
        for (int x = 0; x < 3; ++x)
            for (int y = 0; y < 3; ++y)
                for (int z = 0; z < 3; ++z) {
                    const int v[3] = {x - 1, y - 1, z - 1};
                    int w[3];
                    for (int r = 0; r < 3; ++r) w[r] = s.m[std::size_t(r * 3)] * v[0] + s.m[std::size_t(r * 3 + 1)] * v[1] + s.m[std::size_t(r * 3 + 2)] * v[2];
                    int8_t lab = g[std::size_t((x * 3 + y) * 3 + z)];
                    if (!s.proper && lab == kScrewA) lab = kScrewB;
                    else if (!s.proper && lab == kScrewB) lab = kScrewA;
                    h[std::size_t(((w[0] + 1) * 3 + (w[1] + 1)) * 3 + (w[2] + 1))] = lab;
                }
        if (!have || h < best) best = h, have = true;
    }
    return best;
}

bool coversExactly(const Figure& fig, const Solution& s) {
    const auto l = labels(s);
    int covered = 0;
    for (int8_t v : l) covered += v >= 0;
    if (covered != kCells) return false;
    for (int i = 0; i < fig.count; ++i)
        if (l[std::size_t((fig.cells[std::size_t(i)].x * kGrid + fig.cells[std::size_t(i)].y) * kGrid + fig.cells[std::size_t(i)].z)] < 0)
            return false;
    return true;
}

// Replays a plan's assembly: every piece rests on the floor or a placed piece, and its way in is clear.
bool assemblyHolds(const Plan& plan, std::string* why) {
    std::set<std::array<int, 3>> placed;
    for (int k = 0; k < kPieces; ++k) {
        const int p = plan.order[std::size_t(k)];
        Cell c[kMaxPieceCells];
        placedCells(p, plan.solution[std::size_t(p)], c);
        const int n = pieces()[std::size_t(p)].count;
        bool rests = false;
        for (int i = 0; i < n; ++i) rests = rests || c[i].y == 0 || placed.count({c[i].x, c[i].y - 1, c[i].z});
        if (!rests) {
            *why = std::string("piece ") + pieces()[std::size_t(p)].name + " floats";
            return false;
        }
        int dx = 0, dy = 0, dz = 0;
        switch (plan.approach[std::size_t(p)]) {
            case Approach::Above: dy = 1; break;
            case Approach::FromMinusX: dx = -1; break;
            case Approach::FromPlusX: dx = 1; break;
            case Approach::FromMinusZ: dz = -1; break;
            case Approach::FromPlusZ: dz = 1; break;
        }
        for (int i = 0; i < n; ++i)
            for (int s = 1; s < kGrid; ++s)
                if (placed.count({c[i].x + dx * s, c[i].y + dy * s, c[i].z + dz * s})) {
                    *why = std::string("piece ") + pieces()[std::size_t(p)].name + " is blocked";
                    return false;
                }
        for (int i = 0; i < n; ++i) placed.insert({c[i].x, c[i].y, c[i].z});
    }
    return true;
}

std::array<float, 3> cellWorld(const PieceFrame& pf, const Cell& c) {
    const auto& r = pf.rot;
    return {r[0] * c.x + r[1] * c.y + r[2] * c.z + pf.pos[0], r[3] * c.x + r[4] * c.y + r[5] * c.z + pf.pos[1],
            r[6] * c.x + r[7] * c.y + r[8] * c.z + pf.pos[2]};
}
// Where piece p's cubes are in the figure (stage coordinates) when it is in place.
std::vector<std::array<float, 3>> targetCells(const Plan& plan, int p) {
    Cell c[kMaxPieceCells];
    placedCells(p, plan.solution[std::size_t(p)], c);
    std::vector<std::array<float, 3>> out;
    for (int i = 0; i < pieces()[std::size_t(p)].count; ++i)
        out.push_back({c[i].x - plan.center[0], c[i].y - plan.center[1], c[i].z - plan.center[2]});
    return out;
}
float distToTarget(const Frame& f, const Plan& plan, int p) {
    const auto tc = targetCells(plan, p);
    float d = 0;
    for (int i = 0; i < pieces()[std::size_t(p)].count; ++i) {
        const auto w = cellWorld(f.pieces[std::size_t(p)], pieces()[std::size_t(p)].cells[std::size_t(i)]);
        for (int k = 0; k < 3; ++k) d = std::max(d, std::fabs(w[std::size_t(k)] - tc[std::size_t(i)][std::size_t(k)]));
    }
    return d;
}

std::string readAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}  // namespace

TEST_CASE("soma: the seven pieces") {
    int total = 0;
    for (const Piece& p : pieces()) total += p.count;
    CHECK(total == 27);
    CHECK(pieces()[0].count == 3);
    for (int p = 1; p < kPieces; ++p) CHECK(pieces()[std::size_t(p)].count == 4);
    // Connected (face to face), all different, and only the screws are chiral: A is B's mirror image.
    for (int p = 0; p < kPieces; ++p) {
        const Shape s = shapeOf(p);
        std::set<std::array<int, 3>> reach{s[0]};
        for (int round = 0; round < 4; ++round)
            for (const auto& a : s)
                for (const auto& b : reach)
                    if (std::abs(a[0] - b[0]) + std::abs(a[1] - b[1]) + std::abs(a[2] - b[2]) == 1) {
                        reach.insert(a);
                        break;
                    }
        CHECK(reach.size() == s.size());
        for (int q = p + 1; q < kPieces; ++q) CHECK(!congruent(s, shapeOf(q)));
        const bool chiral = !congruent(s, mirrored(s));
        CHECK(chiral == (p == kScrewA || p == kScrewB));
    }
    CHECK(congruent(shapeOf(kScrewA), mirrored(shapeOf(kScrewB))));
    // Planar pieces: V, L, T, Z lie in one layer; A, B, P do not.
    for (int p = 0; p < kPieces; ++p) {
        std::set<int> zs;
        for (const auto& c : shapeOf(p)) zs.insert(c[2]);
        CHECK((zs.size() == 1) == (p <= 3));
    }
}

TEST_CASE("soma: 24 distinct proper rotations, closed under composition") {
    std::set<Rot> all(rotations().begin(), rotations().end());
    CHECK(all.size() == 24);
    CHECK(rotations()[0] == Rot{1, 0, 0, 0, 1, 0, 0, 0, 1});
    for (const Rot& a : rotations())
        for (const Rot& b : rotations()) {
            Rot c{};
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j) {
                    int v = 0;
                    for (int k = 0; k < 3; ++k) v += a[std::size_t(i * 3 + k)] * b[std::size_t(k * 3 + j)];
                    c[std::size_t(i * 3 + j)] = int8_t(v);
                }
            CHECK(all.count(c) == 1);
        }
}

TEST_CASE("soma: the cube has 11,520 solutions, 480 up to rotation, 240 up to rotation and reflection") {
    // Convention: pieces turn by rotations only (mirror images are other pieces), the cube is fixed in space. Every
    // solution has 48 images under the cube's symmetries (24 rotations, 24 with a reflection, which swaps the screws
    // A and B), none of them the same, so 11,520 = 48 x 240 (Conway and Guy's 240 essentially different solutions).
    Solver solver;
    struct Ctx {
        std::vector<Sym> syms = cubeSymmetries();
        std::set<Grid> rot, all;
        bool valid = true;
    } ctx;
    const auto t0 = std::chrono::steady_clock::now();
    const long long n = solver.enumerate(figure(0), [](const Solution& s, void* c) {
        auto& x = *static_cast<Ctx*>(c);
        x.valid = x.valid && coversExactly(figure(0), s);
        const Grid g = cubeGrid(s);
        x.rot.insert(canonical(g, x.syms, false));
        x.all.insert(canonical(g, x.syms, true));
        return true;
    }, &ctx);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    MESSAGE("soma cube: " << n << " solutions (" << solver.rows() << " placements), enumerated in " << ms << " ms");
    CHECK(n == 11520);
    CHECK(ctx.valid);
    CHECK(ctx.rot.size() == 480);
    CHECK(ctx.all.size() == 240);
}

TEST_CASE("soma: every figure is solvable and assembles bottom-up, each piece resting and with a clear way in") {
    Solver solver;
    CHECK(figureCount() >= 6);
    std::set<std::string> names;
    for (int f = 0; f < figureCount(); ++f) {
        const Figure& fig = figure(f);
        CAPTURE(std::string(fig.name));
        names.insert(fig.name);
        CHECK(fig.count == kCells);
        std::set<std::array<int, 3>> cells;
        for (int i = 0; i < fig.count; ++i) cells.insert({fig.cells[std::size_t(i)].x, fig.cells[std::size_t(i)].y, fig.cells[std::size_t(i)].z});
        CHECK(cells.size() == std::size_t(kCells));
        // It stands on the floor.
        bool floor = false;
        for (const auto& c : cells) floor = floor || c[1] == 0;
        CHECK(floor);
        int above = 0;
        for (uint64_t seed = 0; seed < 12; ++seed) {
            Plan plan;
            REQUIRE(makePlan(solver, f, seed, plan));
            CHECK(plan.figure == f);
            CHECK(coversExactly(fig, plan.solution));
            CHECK(plan.resting);
            std::string why;
            CHECK_MESSAGE(assemblyHolds(plan, &why), why);
            std::set<int> order(plan.order.begin(), plan.order.end());
            CHECK(order.size() == std::size_t(kPieces));
            for (Approach a : plan.approach) above += a == Approach::Above;
        }
        MESSAGE(fig.name << ": " << above << " of " << 12 * kPieces << " pieces dropped in from above, the rest slide in");
    }
    CHECK(names.count("cube") == 1);
    CHECK(findFigure("PYRAMID") == findFigure("pyramid"));
    CHECK(findFigure("nope") == -1);
    std::string unknown;
    const auto figs = parseFigures("cube, Steps nope  tower", &unknown);
    CHECK(figs == std::vector<int>{0, findFigure("steps"), findFigure("tower")});
    CHECK(unknown == "nope");
    CHECK(parseFigures("all").size() == std::size_t(figureCount()));
    CHECK(parseFigures("").size() == 1);
}

TEST_CASE("soma: seeds give different cube solutions, the same seed the same one") {
    Solver solver;
    const auto syms = cubeSymmetries();
    std::set<Grid> distinct;
    Plan a, b;
    REQUIRE(makePlan(solver, 0, 5, a));
    REQUIRE(makePlan(solver, 0, 5, b));
    CHECK(cubeGrid(a.solution) == cubeGrid(b.solution));
    CHECK(a.order == b.order);
    for (uint64_t seed = 0; seed < 40; ++seed) {
        Plan p;
        REQUIRE(makePlan(solver, 0, seed * 7919, p));
        distinct.insert(canonical(cubeGrid(p.solution), syms, true));
    }
    MESSAGE("40 rounds of the cube: " << distinct.size() << " essentially different solutions");
    CHECK(distinct.size() >= 25);
}

TEST_CASE("soma: a round locks one piece per step on the beat, the last on the round's end") {
    Player player(parseFigures("all"));
    const Timing tm;  // 32 beats, hold 8, a lock every 2 beats: 20, 22, ..., 32
    CHECK(lockBeat(tm, 0) == doctest::Approx(20.0));
    CHECK(lockBeat(tm, 6) == doctest::Approx(32.0));
    for (int64_t round : {0, 1, 2, 5}) {
        CAPTURE(round);
        const Plan prev = player.plan(round - 1), cur = player.plan(round);
        // Holding the previous figure, complete.
        Frame h = frameAt(prev, cur, 0.0, tm);
        CHECK(h.phase == Phase::Hold);
        CHECK(h.figure == prev.figure);
        for (int p = 0; p < kPieces; ++p) {
            CHECK(h.pieces[std::size_t(p)].placed);
            CHECK(distToTarget(frameAt(prev, cur, 4.0, tm), prev, p) < 1e-4f);
        }
        CHECK(h.thump == doctest::Approx(1.0f));
        CHECK(frameAt(prev, cur, 8.5, tm).phase == Phase::Burst);
        CHECK(frameAt(prev, cur, 12.0, tm).phase == Phase::Hover);
        CHECK(frameAt(prev, cur, 12.0, tm).placed == 0);
        for (int k = 0; k < kPieces; ++k) {
            const int p = cur.order[std::size_t(k)];
            const double L = lockBeat(tm, k);
            const Frame before = frameAt(prev, cur, L - 0.02, tm), at = frameAt(prev, cur, L, tm);
            CHECK(before.placed == k);
            CHECK(at.placed == k + 1);
            CHECK(!before.pieces[std::size_t(p)].placed);
            CHECK(at.pieces[std::size_t(p)].placed);
            CHECK(distToTarget(before, cur, p) > 0.05f);  // still moving in, fast
            CHECK(distToTarget(before, cur, p) < 0.5f);
            CHECK(distToTarget(at, cur, p) < 0.1f);  // in place (the figure dips with the thump)
            CHECK(at.thump == doctest::Approx(1.0f));
            CHECK(at.pieces[std::size_t(p)].flash == doctest::Approx(1.0f));
            // The pieces placed earlier stay put.
            for (int j = 0; j < k; ++j) CHECK(distToTarget(frameAt(prev, cur, L - 0.5, tm), cur, cur.order[std::size_t(j)]) < 0.1f);
        }
        const Frame end = frameAt(prev, cur, 32.0, tm);
        CHECK(end.placed == kPieces);
        for (int p = 0; p < kPieces; ++p) CHECK(distToTarget(end, cur, p) < 0.1f);
        // Seamless across the line: the end of this round is the start of the next.
        const Frame next = frameAt(cur, player.plan(round + 1), 0.0, tm);
        for (int p = 0; p < kPieces; ++p)
            for (int k = 0; k < 3; ++k)
                CHECK(next.pieces[std::size_t(p)].pos[std::size_t(k)] == doctest::Approx(end.pieces[std::size_t(p)].pos[std::size_t(k)]).epsilon(1e-4));
        // Unplaced pieces never sit inside the finished figure's place before their turn (they hover away from it).
        const Frame hover = frameAt(prev, cur, 15.0, tm);
        for (int p = 0; p < kPieces; ++p) CHECK(distToTarget(hover, cur, p) > 1.5f);
    }
}

TEST_CASE("soma: timing keys are clamped so the build fits; junk input stays finite") {
    Timing t;
    t.len = 32;
    t.hold = 30;  // no room: the hold gives way
    t.each = 2;
    const Timing c = clampTiming(t);
    CHECK(c.each == doctest::Approx(2.0));
    CHECK(c.hold + c.burst <= lockBeat(c, 0) - flightBeats(c) + 1e-9);
    t.each = 50;  // too slow for the round: as slow as fits
    const Timing d = clampTiming(t);
    CHECK(lockBeat(d, 0) - flightBeats(d) >= d.hold + d.burst - 1e-9);
    CHECK(d.each > 2);
    const double nan = std::numeric_limits<double>::quiet_NaN(), inf = std::numeric_limits<double>::infinity();
    Timing j;
    j.len = nan;
    j.hold = inf;
    j.each = -inf;
    const Timing jc = clampTiming(j);
    CHECK(std::isfinite(jc.len));
    CHECK(std::isfinite(jc.hold));
    CHECK(jc.each >= 0.25);
    Player player(parseFigures("all"));
    for (double seed : {nan, inf, -inf, 1e300, -1e300, -3.0, 0.0, 7.9})
        for (double turn : {nan, inf, -inf, 1e300, -1e300, -5.0, 0.0, 13.3, 31.99, 32.0})
            for (double e : {nan, inf, 0.0, 1.0, 1e300}) {
                const Frame f = player.frame(seed, turn, j, e, e, {e, -e});
                for (const PieceFrame& pf : f.pieces) {
                    for (float v : pf.rot) CHECK(std::isfinite(v));
                    for (float v : pf.pos) {
                        CHECK(std::isfinite(v));
                        CHECK(std::fabs(v) < 40.0f);
                    }
                    CHECK(std::isfinite(pf.flash));
                }
            }
    // A clock jump re-solves at most two rounds; repeated frames of the same round don't solve again.
    Player p2(parseFigures("all"));
    p2.frame(10, 3, Timing{});
    const int s = p2.solves();
    CHECK(s == 2);
    for (int i = 0; i < 100; ++i) p2.frame(10, 3 + 0.2 * i, Timing{});
    CHECK(p2.solves() == s);
    p2.frame(11, 0, Timing{});
    CHECK(p2.solves() == s + 1);  // round 10 is cached as the previous one
    p2.frame(1e15, 0, Timing{});
    CHECK(p2.solves() == s + 3);
}

TEST_CASE("soma: frames are deterministic and independent of the order they are asked for") {
    Player a(parseFigures("all")), b(parseFigures("all"));
    std::vector<Frame> fa;
    for (int i = 0; i < 200; ++i) fa.push_back(a.frame(i / 40, (i % 40) * 0.8, Timing{}, 0.1 * (i % 3), 0.2));
    for (int i = 199; i >= 0; --i) {
        const Frame f = b.frame(i / 40, (i % 40) * 0.8, Timing{}, 0.1 * (i % 3), 0.2);
        for (int p = 0; p < kPieces; ++p) {
            CHECK(f.pieces[std::size_t(p)].pos == fa[std::size_t(i)].pieces[std::size_t(p)].pos);
            CHECK(f.pieces[std::size_t(p)].rot == fa[std::size_t(i)].pieces[std::size_t(p)].rot);
        }
    }
    // Rotation matrices stay rotations (orthonormal) along the flights.
    for (const Frame& f : fa)
        for (const PieceFrame& pf : f.pieces) {
            const auto& r = pf.rot;
            const float det = r[0] * (r[4] * r[8] - r[5] * r[7]) - r[1] * (r[3] * r[8] - r[5] * r[6]) +
                              r[2] * (r[3] * r[7] - r[4] * r[6]);
            CHECK(det == doctest::Approx(1.0f).epsilon(1e-3));
        }
}

TEST_CASE("soma: the explosion moves pieces out and the tension stays small") {
    Player player(parseFigures("cube"));
    const Frame calm = player.frame(3, 4.0, Timing{});
    const Frame boom = player.frame(3, 4.0, Timing{}, 1.0);
    // Expl moves each placed piece a little way along its burst path (checked: no two pieces meet), about a cell.
    float most = 0;
    for (int p = 0; p < kPieces; ++p) {
        const auto& a = calm.pieces[std::size_t(p)].pos;
        const auto& b = boom.pieces[std::size_t(p)].pos;
        const float d = std::hypot(b[0] - a[0], b[1] - a[1], b[2] - a[2]);
        CHECK(d < 2.0f);
        most = std::max(most, d);
    }
    CHECK(most > 0.5f);
    const Frame still = player.frame(3, 14.0, Timing{}), shaky = player.frame(3, 14.0, Timing{}, 0.0, 1.0);
    for (int p = 0; p < kPieces; ++p) {
        const auto& a = still.pieces[std::size_t(p)].pos;
        const auto& b = shaky.pieces[std::size_t(p)].pos;
        CHECK(std::hypot(b[0] - a[0], b[1] - a[1], b[2] - a[2]) < 1.0f);
    }
}

TEST_CASE("soma: cost of a new round's solve") {
    Solver solver;
    double worst = 0, total = 0;
    int n = 0;
    for (int f = 0; f < figureCount(); ++f)
        for (uint64_t seed = 0; seed < 10; ++seed) {
            Plan p;
            const auto t0 = std::chrono::steady_clock::now();
            CHECK(makePlan(solver, f, seed, p));
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            worst = std::max(worst, ms);
            total += ms;
            ++n;
        }
    MESSAGE("soma makePlan: mean " << total / n << " ms, worst " << worst << " ms (once per round, on the render thread)");
    double cubeWorst = 0, cubeTotal = 0;
    for (uint64_t seed = 0; seed < 40; ++seed) {
        Plan p;
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(makePlan(solver, 0, seed * 7919, p));
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        cubeWorst = std::max(cubeWorst, ms);
        cubeTotal += ms;
        CHECK(p.burst >= 0);  // a burst under which no two pieces meet
        for (Approach a : p.approach) CHECK(a == Approach::Above);
    }
    MESSAGE("soma makePlan, the cube: mean " << cubeTotal / 40 << " ms, worst " << cubeWorst << " ms");
    CHECK(total / n < 20.0);
}

TEST_CASE("soma: seven piece colors that read apart") {
    MapLut lut{};
    for (int k = 0; k < kPaletteSize; ++k) lut[std::size_t(k)] = float(k) / 255.0f;
    auto ramp = [](float r0, float g0, float b0, float r1, float g1, float b1) {
        Palette p{};
        for (int k = 0; k < kPaletteSize; ++k) {
            const float t = float(k) / 255.0f;
            p[std::size_t(k)] = {r0 + (r1 - r0) * t, g0 + (g1 - g0) * t, b0 + (b1 - b0) * t, 1.0f};
        }
        return p;
    };
    Palette rainbow{};
    for (int k = 0; k < kPaletteSize; ++k) {
        const float h = float(k) / 256.0f * 6.0f;
        const int i = int(h) % 6;
        const float f = h - std::floor(h);
        float r = 0, g = 0, b = 0;
        switch (i) {
            case 0: r = 1, g = f; break;
            case 1: r = 1 - f, g = 1; break;
            case 2: g = 1, b = f; break;
            case 3: g = 1 - f, b = 1; break;
            case 4: r = f, b = 1; break;
            default: r = 1, b = 1 - f; break;
        }
        rainbow[std::size_t(k)] = {r, g, b, 1.0f};
    }
    LabTable lab{};
    effectiveColors(rainbow, lut, lab);
    const PiecePick rp = pickPieces(lab);
    std::set<int> levels(rp.level.begin(), rp.level.end());
    CHECK(levels.size() == 7u);
    CHECK(rp.score > 12.0f);
    for (int k : rp.level) CHECK(lab[std::size_t(k)].l >= 30.0f);  // light enough to shade
    effectiveColors(ramp(0, 0, 0, 1, 1, 1), lut, lab);
    const PiecePick gp = pickPieces(lab);
    CHECK(std::set<int>(gp.level.begin(), gp.level.end()).size() == 7u);
    CHECK(gp.score > 3.0f);
    // The picker keeps its colors for an unchanged map and eases to new ones.
    PiecePicker pk(0.5);
    pk.update(rainbow, lut, 0.0);
    const auto a = pk.levels(0.0);
    pk.update(rainbow, lut, 1.0);
    CHECK(pk.levels(1.0) == a);
    CHECK(pk.switches() == 0);
    // A very different map: switch, easing over half a second.
    pk.update(ramp(0, 0, 0.2f, 1, 1, 0.8f), lut, 2.0);
    CHECK(pk.switches() == 1);
    CHECK(pk.levels(2.0) == a);
    const auto mid = pk.levels(2.25), end = pk.levels(9.0);
    CHECK(end != a);
    bool between = false;
    for (int i = 0; i < 7; ++i) between = between || (mid[std::size_t(i)] != a[std::size_t(i)] && mid[std::size_t(i)] != end[std::size_t(i)]);
    CHECK(between);
    // A map whose colors drift dark under the current set (an animated map): the picker moves away from them.
    Palette dim = rainbow;
    for (int k = 0; k < kPaletteSize; ++k)
        if (std::abs(k - int(std::lround(end[3] * 255.0f))) < 12) dim[std::size_t(k)] = {0.03f, 0.03f, 0.03f, 1.0f};
    pk.update(dim, lut, 10.0);
    const auto moved = pk.levels(20.0);
    LabTable dl{};
    effectiveColors(dim, lut, dl);
    for (float v : moved) CHECK(dl[std::size_t(std::lround(v * 255.0f))].l > 20.0f);
}

TEST_CASE("soma: the content scenes lock on the beat and complete on every 8-bar line") {
    for (const char* name : {"Soma Cube"}) {  // (Soma Figures removed 2026-11-23; Figs stays)
        CAPTURE(name);
        const fs::path f = fs::path(REFORCE_SOURCE_DIR) / "content/Packages/reforce.Solid.package" / (std::string(name) + ".txt");
        REQUIRE(fs::exists(f));
        auto file = reforce::scene::parseSceneText(readAll(f), name);
        auto cfg = std::make_shared<reforce::scene::CompiledConfig>(
            reforce::scene::compileConfig(file, reforce::scene::SceneType::Solid));
        CHECK(cfg->diags.empty());
        REQUIRE(solidMesh(*cfg) == MeshKind::Soma);
        reforce::scene::Environment env;
        SolidRun run(cfg, env, name, MeshKind::Soma, 0.0, 0.0, 40.0, nullptr);
        auto at = [&](double beat) {
            SpriteAudio a;
            a.sync.on = 1;
            a.sync.bpm = 120;
            a.sync.beat = beat;
            a.sync.bar = beat / 4;
            a.sync.phrase = beat / 64;
            return run.evaluate(beat / 2, a);
        };
        std::set<int> figs;
        for (int cycle = 0; cycle < 4; ++cycle) {
            const double b0 = 32.0 * cycle;
            CHECK(at(b0 + 31.99).soma.placed == 6);
            const SolidSpec line = at(b0 + 32.0);  // the line: complete
            CHECK(line.soma.placed == 0);           // (the next round holds it)
            CHECK(line.soma.phase == Phase::Hold);
            for (const PieceFrame& pf : line.soma.pieces) CHECK(pf.placed);
            CHECK(line.over);
            CHECK(!line.colorKey);
            // Locks on beats: the count of placed pieces changes only on whole beats.
            int last = at(b0 + 16.0).soma.placed;
            for (double b = b0 + 16.0; b < b0 + 32.0; b += 0.125) {
                const int n = at(b).soma.placed;
                if (n != last) CHECK(b == doctest::Approx(std::round(b)));
                last = n;
            }
            figs.insert(at(b0 + 20).soma.figure);
        }
        CHECK(figs == std::set<int>{0});
    }
}

// ---------------------------------------------------------------- pieces never pass through each other

namespace {

struct Obb {
    std::array<float, 3> c{};
    std::array<std::array<float, 3>, 3> ax{};  // unit axes (the piece's rotation columns)
};

// Separating-axis test of two unit cubes shrunk by `tol` on every side (so cubes that touch face to face, or sit
// within `tol` of each other, do not count): true when they interpenetrate deeper than that.
bool cubesOverlap(const Obb& a, const Obb& b, float tol) {
    const float h = 0.5f - tol;
    const std::array<float, 3> d{b.c[0] - a.c[0], b.c[1] - a.c[1], b.c[2] - a.c[2]};
    auto dot = [](const std::array<float, 3>& p, const std::array<float, 3>& q) {
        return p[0] * q[0] + p[1] * q[1] + p[2] * q[2];
    };
    auto separates = [&](const std::array<float, 3>& n) {
        const float len = std::sqrt(dot(n, n));
        if (len < 1e-5f) return false;
        float ra = 0, rb = 0;
        for (int k = 0; k < 3; ++k) {
            ra += h * std::fabs(dot(a.ax[std::size_t(k)], n));
            rb += h * std::fabs(dot(b.ax[std::size_t(k)], n));
        }
        return std::fabs(dot(d, n)) > ra + rb;
    };
    for (int k = 0; k < 3; ++k)
        if (separates(a.ax[std::size_t(k)]) || separates(b.ax[std::size_t(k)])) return false;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            const auto& u = a.ax[std::size_t(i)];
            const auto& v = b.ax[std::size_t(j)];
            if (separates({u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]}))
                return false;
        }
    return true;
}

std::vector<Obb> pieceBoxes(const PieceFrame& pf, int p) {
    std::vector<Obb> out;
    const Piece& pc = pieces()[std::size_t(p)];
    for (int i = 0; i < pc.count; ++i) {
        Obb o;
        o.c = cellWorld(pf, pc.cells[std::size_t(i)]);
        for (int k = 0; k < 3; ++k)
            o.ax[std::size_t(k)] = {pf.rot[std::size_t(k)], pf.rot[std::size_t(3 + k)], pf.rot[std::size_t(6 + k)]};
        out.push_back(o);
    }
    return out;
}

const char* stateName(PieceState s) {
    switch (s) {
        case PieceState::Placed: return "placed";
        case PieceState::Burst: return "burst";
        case PieceState::Hover: return "hover";
        case PieceState::Flight: return "flight";
        case PieceState::Approach: return "approach";
    }
    return "?";
}

struct OverlapStats {
    std::map<std::string, long> byPhase;  // "flight / placed": sampled frames x piece pairs that interpenetrate
    long frames = 0, total = 0;
    std::string first;  // the first one found (round, beat, pieces)
};

// Every `step` beats of rounds [r0, r1) of `player`, seen as the content scene sees it (its orbit at 120 BPM), every
// pair of pieces as their unit cubes: interpenetrating pairs are counted by what the two pieces are doing.
void sweepOverlaps(Player& player, int64_t r0, int64_t r1, double step, double explode, double tension,
                   OverlapStats& st) {
    const Timing tm;
    const int samples = int(std::lround(tm.len / step));
    for (int64_t r = r0; r < r1; ++r)
        for (int k = 0; k <= samples; ++k) {
            const double b = k * step;
            const double sec = (32.0 * double(r) + b) / 2.0;  // the scene's B0 at 120 BPM
            const double b4 = 0.16 * sec + 0.785;
            const View view{b4 + 0.2 * std::sin(4 * b4), 0.5 + 0.05 * std::sin(0.19 * sec)};
            const Frame f = player.frame(double(r), b, tm, explode, tension, view);
            ++st.frames;
            std::array<std::vector<Obb>, kPieces> boxes;
            for (int p = 0; p < kPieces; ++p) boxes[std::size_t(p)] = pieceBoxes(f.pieces[std::size_t(p)], p);
            for (int i = 0; i < kPieces; ++i)
                for (int j = i + 1; j < kPieces; ++j) {
                    bool hit = false;
                    for (const Obb& a : boxes[std::size_t(i)])
                        for (const Obb& c : boxes[std::size_t(j)]) {
                            const float dx = a.c[0] - c.c[0], dy = a.c[1] - c.c[1], dz = a.c[2] - c.c[2];
                            if (dx * dx + dy * dy + dz * dz > 3.0f) continue;  // farther apart than two circumradii
                            hit = hit || cubesOverlap(a, c, 0.01f);
                        }
                    if (!hit) continue;
                    std::string s1 = stateName(f.pieces[std::size_t(i)].state);
                    std::string s2 = stateName(f.pieces[std::size_t(j)].state);
                    if (s2 < s1) std::swap(s1, s2);
                    ++st.byPhase[s1 + " / " + s2];
                    ++st.total;
                    if (st.first.empty())
                        st.first = "round " + std::to_string(r) + " beat " + std::to_string(b) + ": " +
                                   pieces()[std::size_t(i)].name + " (" + stateName(f.pieces[std::size_t(i)].state) +
                                   ") and " + pieces()[std::size_t(j)].name + " (" +
                                   stateName(f.pieces[std::size_t(j)].state) + ")";
                }
        }
}

std::string describe(const OverlapStats& st) {
    std::string s = std::to_string(st.total) + " overlapping pairs in " + std::to_string(st.frames) + " frames";
    for (const auto& [k, n] : st.byPhase) s += "; " + k + ": " + std::to_string(n);
    if (!st.first.empty()) s += "; first: " + st.first;
    return s;
}

}  // namespace

TEST_CASE("soma: no two pieces ever pass through each other (every 1/32 beat of 48 rounds: burst, halo, flights)") {
    Player cube(parseFigures("cube"));
    OverlapStats st;
    sweepOverlaps(cube, 0, 48, 1.0 / 32.0, 0.0, 0.0, st);
    MESSAGE("cube, 48 rounds: " << describe(st));
    CHECK(st.total == 0);
    // Every figure (two rounds of each), and with a drop / a build-up going on.
    Player all(parseFigures("all"));
    OverlapStats sa;
    sweepOverlaps(all, 0, 2 * figureCount(), 1.0 / 32.0, 0.0, 0.0, sa);
    MESSAGE("all figures, " << 2 * figureCount() << " rounds: " << describe(sa));
    CHECK(sa.total == 0);
    OverlapStats se;
    for (double e : {0.3, 1.0})
        for (double tn : {0.0, 1.0}) sweepOverlaps(cube, 100, 106, 1.0 / 16.0, e, tn, se);
    MESSAGE("cube with Expl .3 / 1 and Tens 0 / 1, 6 rounds each: " << describe(se));
    CHECK(se.total == 0);
}
