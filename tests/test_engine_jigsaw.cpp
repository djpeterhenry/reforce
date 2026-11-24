// The jigsaw solid (docs/3d-design.md "Jigsaw"): the cut (engine/JigsawShape.inc), the solve plan and timing
// (engine/Jigsaw), and the content scenes' timing.
#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <set>
#include <sstream>

#include "engine/Jigsaw.hpp"
#include "engine/Solids.hpp"
#include "scene/Evaluate.hpp"
#include "scene/SceneConfig.hpp"
#include "scene/SceneFile.hpp"
#include "scene/SceneType.hpp"

using namespace reforce::engine;
namespace fs = std::filesystem;

TEST_CASE("jigsaw: the grid fits the frame's aspect; cells close to square") {
    const auto g = jigsaw::fitGrid(32, 16.0 / 9.0, 0);
    CHECK(g.cols == 8);
    CHECK(g.rows == 4);
    CHECK(g.cw == doctest::Approx(16.0 / 9.0 * 4 / 8));
    const auto mini = jigsaw::fitGrid(12, 16.0 / 9.0, 0);
    CHECK(mini.cols == 4);
    CHECK(mini.rows == 3);
    const auto tall = jigsaw::fitGrid(24, 9.0 / 16.0, 0);
    CHECK(tall.rows > tall.cols);
    for (int n : {-5, 0, 1, 2, 7, 50, 160, 1000, std::numeric_limits<int>::max()}) {
        for (double a : {0.0, 0.5, 1.0, 16.0 / 9.0, 4.0, 1e9, std::nan("")}) {
            const auto q = jigsaw::fitGrid(n, a, 3);
            CHECK(q.count() >= 2);
            CHECK(q.count() <= jigsaw::kMaxPieces);
            CHECK(q.cols <= jigsaw::kMaxCols);
            CHECK(q.rows <= jigsaw::kMaxRows);
            CHECK(std::isfinite(q.cw));
            CHECK(q.cw > 0.1f);
        }
    }
    CHECK(jigsaw::fitGrid(32, 16.0 / 9.0, 1).seed != jigsaw::fitGrid(32, 16.0 / 9.0, 2).seed);
}

namespace {
// Every sample point of the frame lies in exactly one piece (points closer than `eps` to an outline excepted).
void checkTiling(const jigsaw::Grid& g, int samplesX, int samplesY, int& checked, int& bad) {
    const float eps = 1e-3f;
    for (int sy = 0; sy < samplesY; ++sy) {
        for (int sx = 0; sx < samplesX; ++sx) {
            const jigsaw::Point p{(float(sx) + 0.5f) / float(samplesX) * g.width(),
                                  (float(sy) + 0.5f) / float(samplesY) * float(g.rows)};
            int inside = 0;
            float nearest = 1e9f;
            for (int r = 0; r < g.rows; ++r)
                for (int c = 0; c < g.cols; ++c) {
                    const float v = jigsaw::pieceField(g, c, r, p);
                    nearest = std::min(nearest, std::fabs(v));
                    inside += v < 0 ? 1 : 0;
                }
            if (nearest < eps) continue;
            ++checked;
            const auto o = jigsaw::owner(g, p);
            if (inside != 1 || o.index < 0 || jigsaw::pieceField(g, o.index % g.cols, o.index / g.cols, p) >= 0) ++bad;
        }
    }
}
}  // namespace

TEST_CASE("jigsaw: the pieces tile the frame exactly: every point in exactly one piece, the owner lookup agrees") {
    for (uint64_t seed : {0ull, 1ull, 7ull, 123456789ull}) {
        for (int pieces : {32, 12, 6, 96}) {
            const auto g = jigsaw::fitGrid(pieces, 16.0 / 9.0, seed);
            int checked = 0, bad = 0;
            checkTiling(g, 320, 180, checked, bad);
            CHECK(bad == 0);
            CHECK(checked > 320 * 180 * 9 / 10);
        }
    }
    // Portrait and square frames too.
    for (double aspect : {1.0, 9.0 / 16.0, 2.4}) {
        const auto g = jigsaw::fitGrid(24, aspect, 5);
        int checked = 0, bad = 0;
        checkTiling(g, 200, 200, checked, bad);
        CHECK(bad == 0);
    }
}

TEST_CASE("jigsaw: every interior edge has a knob; the tab of one piece is the blank of its neighbor") {
    for (uint64_t seed : {0ull, 9ull, 42ull}) {
        const auto g = jigsaw::fitGrid(32, 16.0 / 9.0, seed);
        int plus = 0, minus = 0;
        auto probe = [&](bool horizontal, int i, int j) {
            const int side = jigsaw::knobSide(g, horizontal, i, j);
            REQUIRE(side != 0);
            (side > 0 ? plus : minus)++;
            // The pieces on either side ("-" = above / left, "+" = below / right).
            const int mc = horizontal ? i : i - 1, mr = horizontal ? j - 1 : j;
            const int pc = i, pr = j;
            const int tabC = side > 0 ? mc : pc, tabR = side > 0 ? mr : pr;    // the piece with the tab
            // A point a fifth of a cell into the blank side, near the edge's middle: on a straight edge it would be
            // in the blank piece; the knob's head puts it in the tab piece.
            const auto a = jigsaw::corner(g, i, j);
            const auto b = horizontal ? jigsaw::corner(g, i + 1, j) : jigsaw::corner(g, i, j + 1);
            const float mx = 0.5f * (a.x + b.x), my = 0.5f * (a.y + b.y);
            const float S = std::min(g.cw, 1.0f);
            const float d = 0.2f * S * float(side);
            const jigsaw::Point head{horizontal ? mx : mx + d, horizontal ? my + d : my};
            // The head may sit a little off the middle: look for the tab piece's ownership along the edge.
            bool inTab = false;
            for (int k = -4; k <= 4 && !inTab; ++k) {
                const float o = float(k) * 0.02f * S;
                const jigsaw::Point q{horizontal ? head.x + o : head.x, horizontal ? head.y : head.y + o};
                const auto own = jigsaw::owner(g, q);
                inTab = own.index == tabR * g.cols + tabC;
            }
            CHECK(inTab);
            // And the blank piece does not own it (exactly one owner).
            const auto own = jigsaw::owner(g, head);
            CHECK(own.index >= 0);
        };
        for (int j = 1; j < g.rows; ++j)
            for (int i = 0; i < g.cols; ++i) probe(true, i, j);
        for (int i = 1; i < g.cols; ++i)
            for (int j = 0; j < g.rows; ++j) probe(false, i, j);
        CHECK(plus > 5);
        CHECK(minus > 5);
        // Border edges are straight: no knob.
        CHECK(jigsaw::knobSide(g, true, 3, 0) == 0);
        CHECK(jigsaw::knobSide(g, false, g.cols, 1) == 0);
    }
}

TEST_CASE("jigsaw: solve orders place every piece once; edges first puts the corners, then the border first") {
    for (int o = 0; o < 4; ++o) {
        jigsaw::Config c;
        c.order = jigsaw::Order(o);
        jigsaw::Plan plan;
        jigsaw::makePlan(plan, 11, c);
        REQUIRE(plan.count == 32);
        std::set<int> seen;
        for (int k = 0; k < plan.count; ++k) seen.insert(plan.sequence[std::size_t(k)]);
        CHECK(seen.size() == 32);
        if (c.order == jigsaw::Order::Edges) {
            const std::set<int> corners{0, 7, 24, 31};
            for (int k = 0; k < 4; ++k) CHECK(corners.count(plan.sequence[std::size_t(k)]) == 1);
            // 8 x 4: 20 border pieces, then the 12 inside.
            for (int k = 0; k < 20; ++k) {
                const int p = plan.sequence[std::size_t(k)];
                CHECK((p % 8 == 0 || p % 8 == 7 || p / 8 == 0 || p / 8 == 3));
            }
        }
        if (c.order == jigsaw::Order::Rows)
            for (int k = 0; k < 32; ++k) CHECK(plan.sequence[std::size_t(k)] == k);
        if (c.order == jigsaw::Order::Corner) {
            // Grows: every piece after the first touches one placed before it.
            for (int k = 1; k < 32; ++k) {
                const int p = plan.sequence[std::size_t(k)];
                bool touches = false;
                for (int m = 0; m < k; ++m) {
                    const int q = plan.sequence[std::size_t(m)];
                    touches = touches || std::abs(p % 8 - q % 8) + std::abs(p / 8 - q / 8) == 1;
                }
                CHECK(touches);
            }
        }
    }
}

TEST_CASE("jigsaw: landings snap on whole beats, in order, the last exactly on the phrase line") {
    for (int pieces : {32, 12, 6, 96, 2}) {
        for (double brk : {0.0, 4.0, 8.0}) {
            jigsaw::Config c;
            c.pieces = pieces;
            c.brk = brk;
            jigsaw::Plan plan;
            jigsaw::makePlan(plan, 3, c);
            const int n = plan.count;
            double last = -1, prev = -1;
            for (int k = 0; k < n; ++k) {
                const double L = plan.land[std::size_t(plan.sequence[std::size_t(k)])];
                CHECK(L == std::floor(L));          // on a beat
                CHECK(L >= brk + 2);                // after the burst
                CHECK(L <= 32);
                CHECK(L >= prev);                   // in order
                prev = L;
                last = std::max(last, L);
            }
            CHECK(last == 32);  // the last snap on the next line
            if (n <= 32 - brk - 1) {
                // Fewer pieces than beats: one per beat at most.
                std::set<double> beats;
                for (int p = 0; p < n; ++p) beats.insert(plan.land[std::size_t(p)]);
                CHECK(int(beats.size()) == n);
            }
        }
    }
    // 32 pieces from beat 6 to 32 (27 beats): the first five beats take two, then one per beat.
    CHECK(jigsaw::landingBeat(0, 32, 6, 32) == 6);
    CHECK(jigsaw::landingBeat(1, 32, 6, 32) == 6);
    CHECK(jigsaw::landingBeat(9, 32, 6, 32) == 10);
    CHECK(jigsaw::landingBeat(10, 32, 6, 32) == 11);
    CHECK(jigsaw::landingBeat(31, 32, 6, 32) == 32);
}

TEST_CASE("jigsaw: a cycle: whole, breaks apart on Brk, pieces land on their beats, complete on the line") {
    jigsaw::Config c;  // 32 pieces, 32 beats, break on beat 4
    jigsaw::Solver solver;
    const uint64_t seed = 5;
    jigsaw::Plan plan, prev;
    jigsaw::makePlan(plan, seed, c);
    jigsaw::makePlan(prev, seed - 1, c);
    auto at = [&](double turn, double kick = 0, double shake = 0) {
        jigsaw::Input in;
        in.turn = turn;
        in.kick = kick;
        in.shake = shake;
        return solver.frame(seed, c, in);
    };
    // Just after the line: all home, the seams flashing (the previous seed's cut), fading out by the break.
    {
        const auto f = at(0.0);
        CHECK(f.loose == 0);
        CHECK(f.glow == doctest::Approx(1.0));
        CHECK(f.seam == doctest::Approx(1.0));
        CHECK(f.grid.seed == prev.grid.seed);
        const auto g = at(3.0);
        CHECK(g.seam == doctest::Approx(0.0));
        CHECK(g.whole);
    }
    // The break: everything lifts and spreads, rotated, over other slots.
    {
        const auto f = at(4.5);
        CHECK(f.loose == 32);
        CHECK(f.grid.seed == plan.grid.seed);
        const auto s = at(5.5);
        int away = 0, turned = 0;
        for (int p = 0; p < 32; ++p) {
            const auto& pc = s.pieces[std::size_t(p)];
            const auto h = jigsaw::homeCenter(s.grid, p);
            away += std::hypot(pc.x - h.x, pc.y - h.y) > 0.4f ? 1 : 0;
            turned += std::fabs(pc.angle) > 0.05f ? 1 : 0;
            CHECK(pc.z > 0.0f);
            CHECK(!pc.landed);
        }
        CHECK(away >= 28);
        CHECK(turned >= 28);
        // Bottom to top.
        for (int i = 1; i < s.loose; ++i)
            CHECK(s.pieces[std::size_t(s.order[std::size_t(i - 1)])].z <= s.pieces[std::size_t(s.order[std::size_t(i)])].z);
    }
    // Each piece snaps on its beat: just before it is lifted and on its way, on it it is home (landed, flashing).
    for (int p = 0; p < 32; ++p) {
        const double L = plan.land[std::size_t(p)];
        const auto before = at(L - 0.02);
        const auto& b = before.pieces[std::size_t(p)];
        CHECK(!b.landed);
        const auto h = jigsaw::homeCenter(plan.grid, p);
        CHECK(std::hypot(b.x - h.x, b.y - h.y) < 0.05f);  // nearly there
        CHECK(b.z < 0.05f);
        const auto on = at(L);
        CHECK(on.pieces[std::size_t(p)].landed);
        CHECK(on.pieces[std::size_t(p)].flash == doctest::Approx(1.0));
        const auto mid = at(L - 0.5);
        CHECK(!mid.pieces[std::size_t(p)].landed);
        CHECK(mid.pieces[std::size_t(p)].z > 0.1f);  // in the air
    }
    // Complete on the line, nothing loose a moment before only the last ones.
    const auto end = at(32.0);
    CHECK(end.loose == 0);
    CHECK(end.glow == doctest::Approx(1.0));
    int lastOnes = 0;
    for (int p = 0; p < 32; ++p) lastOnes += plan.land[std::size_t(p)] == 32.0f ? 1 : 0;
    CHECK(at(31.99).loose == lastOnes);
    // The pile rests over slots that are still empty (except near the very end, when nothing else is left).
    for (double beat = 6.5; beat < 28; beat += 1.0) {
        const auto f = at(beat);
        for (int i = 0; i < f.loose; ++i) {
            const int p = f.order[std::size_t(i)];
            const auto& pc = f.pieces[std::size_t(p)];
            if (pc.z > 0.12f) continue;  // flying or hopping
            int over = -1;  // the slot whose home center is nearest
            float best = 1e9f;
            for (int q = 0; q < f.count; ++q) {
                const auto h = jigsaw::homeCenter(f.grid, q);
                const float d = std::hypot(h.x - pc.x, h.y - pc.y);
                if (d < best) {
                    best = d;
                    over = q;
                }
            }
            CHECK_MESSAGE(!f.pieces[std::size_t(over)].landed, "beat ", beat, " piece ", p, " over ", over);
        }
    }
}

TEST_CASE("jigsaw: deterministic; the cache gives the same frames; any jump of the clock shows the right state") {
    jigsaw::Config c;
    jigsaw::Solver a, b;
    jigsaw::Plan plan, prev;
    for (uint64_t seed : {1ull, 2ull, 3ull, 900ull}) {
        jigsaw::makePlan(plan, seed, c);
        jigsaw::makePlan(prev, seed - 1, c);
        for (double t = 0; t <= 32; t += 0.37) {
            jigsaw::Input in;
            in.turn = t;
            jigsaw::Frame ref;
            jigsaw::frameAt(plan, prev, in, ref);
            const auto& fa = a.frame(seed, c, in);
            const auto& fb = b.frame(seed, c, in);
            CHECK(fa.loose == ref.loose);
            CHECK(fb.loose == ref.loose);
            for (int p = 0; p < 32; ++p) {
                CHECK(fa.pieces[std::size_t(p)].x == ref.pieces[std::size_t(p)].x);
                CHECK(fa.pieces[std::size_t(p)].angle == ref.pieces[std::size_t(p)].angle);
                CHECK(fb.pieces[std::size_t(p)].z == ref.pieces[std::size_t(p)].z);
            }
        }
    }
    // Different seeds: different cuts and scrambles.
    jigsaw::Plan p1, p2;
    jigsaw::makePlan(p1, 1, c);
    jigsaw::makePlan(p2, 2, c);
    CHECK(p1.grid.seed != p2.grid.seed);
    CHECK(p1.spots[0][0].x != p2.spots[0][0].x);
}

TEST_CASE("jigsaw: junk and huge input stays finite and bounded") {
    jigsaw::Solver s;
    const double junk[] = {std::nan(""), INFINITY, -INFINITY, -1e300, 1e300, -5, 0, 1e-300, 31.999999, 33};
    for (double turn : junk)
        for (double kick : junk)
            for (double shake : {std::nan(""), 1e9, -3.0, 0.5}) {
                jigsaw::Config c;
                auto toInt = [](double v, double mod) { return std::isfinite(v) ? int(std::fmod(std::fabs(v), mod)) : 0; };
                c.pieces = toInt(turn, 400.0);
                c.cycle = kick;
                c.brk = turn;
                c.aspect = shake;
                c.order = jigsaw::Order(toInt(kick, 9.0) - 2);
                jigsaw::Input in;
                in.turn = turn;
                in.kick = kick;
                in.shake = shake;
                const uint64_t seed = std::isfinite(turn) ? uint64_t(std::fabs(std::fmod(turn, 1e15))) : 0;
                const auto& f = s.frame(seed, c, in);
                CHECK(f.count >= 2);
                CHECK(f.count <= jigsaw::kMaxPieces);
                CHECK(f.loose <= f.count);
                CHECK(std::isfinite(f.seam));
                CHECK(std::isfinite(f.glow));
                for (int p = 0; p < f.count; ++p) {
                    const auto& pc = f.pieces[std::size_t(p)];
                    CHECK(std::isfinite(pc.x));
                    CHECK(std::isfinite(pc.y));
                    CHECK(std::isfinite(pc.angle));
                    CHECK(std::isfinite(pc.z));
                    CHECK(std::fabs(pc.x) < 100);
                    CHECK(pc.z >= 0);
                    CHECK(pc.z < 5);
                }
            }
    // The largest grid stays bounded and fast enough to plan every cycle.
    jigsaw::Config big;
    big.pieces = 100000;
    big.cycle = 1e9;
    jigsaw::Plan plan;
    jigsaw::makePlan(plan, 77, big);
    CHECK(plan.count <= jigsaw::kMaxPieces);
}

namespace {
std::string readAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
}  // namespace

TEST_CASE("jigsaw: the content scenes break apart after the line and complete exactly on every 8-bar line") {
    struct Want {
        const char* name;
        int pieces, cols, rows;
        double brk;
    };
    for (const Want w : {Want{"Jigsaw", 32, 8, 4, 4.0}, Want{"Jigsaw Mini", 12, 4, 3, 8.0}}) {
        const fs::path f =
            fs::path(REFORCE_SOURCE_DIR) / "content/Packages/reforce.Solid.package" / (std::string(w.name) + ".txt");
        REQUIRE(fs::exists(f));
        auto file = reforce::scene::parseSceneText(readAll(f), w.name);
        auto cfg = std::make_shared<reforce::scene::CompiledConfig>(
            reforce::scene::compileConfig(file, reforce::scene::SceneType::Solid));
        REQUIRE(solidMesh(*cfg) == MeshKind::Jigsaw);
        reforce::scene::Environment env;
        SolidRun run(cfg, env, w.name, MeshKind::Jigsaw, 0.0, 0.0, 400.0, nullptr);
        jigsaw::Solver solver;
        auto at = [&](double beat, double drop = 0) {
            SpriteAudio a;
            a.sync.on = 1;
            a.sync.bpm = 120;
            a.sync.beat = beat;
            a.sync.bar = beat / 4;
            a.sync.phrase = beat / 64;
            a.sync.drop = drop;
            return run.evaluate(beat / 2, a);
        };
        auto frame = [&](const SolidSpec& s) {
            jigsaw::Config c = s.jigsaw;
            c.aspect = 16.0 / 9.0;
            return solver.frame(uint64_t(s.seed), c, s.jigsawIn);
        };
        for (int cycle = 1; cycle < 4; ++cycle) {
            const double b0 = 32.0 * cycle;
            const SolidSpec s0 = at(b0);
            CHECK(s0.jigsaw.pieces == w.pieces);
            CHECK(s0.jigsaw.brk == w.brk);
            CHECK(s0.jigsaw.cycle == 32);
            CHECK(s0.jigsawIn.turn == doctest::Approx(0.0));
            CHECK(s0.seed == doctest::Approx(double(cycle)));
            // On the line: complete (every piece home, the completion flash).
            const auto f0 = frame(s0);
            CHECK(f0.grid.cols == w.cols);
            CHECK(f0.grid.rows == w.rows);
            CHECK(f0.loose == 0);
            CHECK(f0.glow == doctest::Approx(1.0));
            // Whole just before the break, all loose just after it.
            CHECK(frame(at(b0 + w.brk - 0.01)).whole);
            CHECK(frame(at(b0 + w.brk + 0.5)).loose == w.pieces);
            // Each piece snaps on its beat; the last ones just before the next line.
            jigsaw::Plan plan;
            jigsaw::Config c = s0.jigsaw;
            c.aspect = 16.0 / 9.0;
            jigsaw::makePlan(plan, uint64_t(s0.seed), c);
            for (int k = 0; k < plan.count; ++k) {
                const int p = plan.sequence[std::size_t(k)];
                const double L = plan.land[std::size_t(p)];
                CHECK(L == std::floor(L));
                CHECK(!frame(at(b0 + L - 0.01)).pieces[std::size_t(p)].landed);
                if (L < 32) CHECK(frame(at(b0 + L)).pieces[std::size_t(p)].landed);
            }
            CHECK(frame(at(b0 + 31.99)).loose >= 1);
            CHECK(frame(at(b0 + 32.0)).loose == 0);
        }
        // A drop pops the picture apart (Kick from SYNC_DROP), even when it is whole.
        const SolidSpec d = at(32.0 * 5 + 1.0, 1.0);
        CHECK(d.jigsawIn.kick == doctest::Approx(1.0));
        CHECK(frame(d).loose == w.pieces);
    }
}
