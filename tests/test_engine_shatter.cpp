// Shatter (docs/3d-design.md "Shatter"): engine/Shatter's fracture, schedule and flight, and the shatter solid's keys.
//   - the shards tile the pane exactly: every point has one shard, which is in the table and whose quad covers it;
//     for one to six impacts, dense and sparse webs; the count stays bounded
//   - cracks land on beat lines (every beat from the impact to the line adds or extends cracks), spokes grow outwards
//     after the piece below them, rings after a spoke reached them
//   - the pane shatters exactly on the line; at that moment every shard is loose and at home (the pane exactly); the
//     shards then fall and leave; the fresh pane arrives 2 beats later
//   - a drop explodes the pane on the next 16th line and a fresh pane follows; a drop just after a gentle shatter
//     blows it apart; a build makes the cracks due next creep (never back) on the 16ths
//   - determinism, the same frames at 30 / 60 / 144 fps, clock jumps re-base, junk inputs stay finite and bounded
//   - the solid's keys and the content scenes
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "engine/ObjectBeat.hpp"
#include "engine/Shatter.hpp"
#include "engine/Solids.hpp"
#include "scene/SceneConfig.hpp"
#include "scene/SceneFile.hpp"
#include "scene/SceneType.hpp"

using namespace reforce::engine;
using namespace reforce::engine::shatter;

namespace {

constexpr double kAspect = 16.0 / 9.0;

Clock clockAt(double beat, double build = 0, double drop = 0) {
    Clock c;
    c.on = true;
    c.bar = beat / 4.0;
    c.build = build;
    c.drop = drop;
    return c;
}

std::unique_ptr<Plan> plan(uint64_t seed, const Config& c, double length = 62, double first = 2, double bar = 2) {
    auto p = std::make_unique<Plan>();
    makePlan(*p, seed, c, kAspect, length, first, bar);
    return p;
}

// Every point of a grid over the pane: one shard in the table, its quad covers the point.
void checkTiling(const Plan& p, int cols, int rows) {
    int missing = 0, outside = 0, bad = 0;
    std::set<int> seen;
    for (int y = 0; y < rows; ++y)
        for (int x = 0; x < cols; ++x) {
            const float px = float((x + 0.5) / cols * p.aspect), py = float((y + 0.5) / rows);
            const Hit h = owner(p.pane, px, py);
            if (h.slot < 0 || h.slot >= kSlots || h.elem < 0 || h.elem >= kElemIds || !std::isfinite(h.dist) ||
                !(h.param >= 0.0f && h.param <= 1.0f)) {
                ++bad;
                continue;
            }
            const int s = p.slotShard[std::size_t(h.slot)];
            if (s < 0) {
                ++missing;
                continue;
            }
            seen.insert(s);
            const Shard& sd = p.shards[std::size_t(s)];
            if (sd.slot != h.slot) ++bad;
            if (px - sd.cx < sd.x0 || px - sd.cx > sd.x1 || py - sd.cy < sd.y0 || py - sd.cy > sd.y1) ++outside;
        }
    CHECK(bad == 0);
    CHECK(missing == 0);
    CHECK(outside == 0);
    CHECK(int(seen.size()) > p.shardCount / 2);  // (tiny shards may fall between grid points)
}

// Steps a player along the clock from beat `from` to `to` (in steps of `step`; at most 16 beats a step, or it re-bases).
void advance(Player& p, double from, double to, double step = 0.25, double build = 0) {
    for (double b = from; b < to; b += step) p.step(0.0, clockAt(b, build), Controls{});
}

// Runs a player from 0 to `seconds` at `fps` with the 120 BPM clock (or none); returns it.
std::unique_ptr<Player> run(const Config& c, double seconds, double fps, bool withClock, uint64_t seed = 9,
                            double build = 0) {
    auto p = std::make_unique<Player>(seed, c, kAspect, 0.0);
    const int n = int(std::lround(seconds * fps));
    for (int i = 0; i <= n; ++i) {
        const double t = double(i) / fps;
        p->step(t, withClock ? clockAt(2.0 * t, build) : Clock{}, Controls{});
    }
    return p;
}

bool sameFrame(const Frame& a, const Frame& b) {
    if ((a.pane == nullptr) != (b.pane == nullptr) || a.loose != b.loose) return false;
    // (The crack and tilt arrays mean something only while there is a pane.)
    if (a.pane && (a.pane->key != b.pane->key || a.offset != b.offset)) return false;
    if (a.pane && (a.reveal != b.reveal || a.glow != b.glow || a.tilt != b.tilt)) return false;
    for (int i = 0; i < a.loose; ++i) {
        const ShardPose &x = a.poses[std::size_t(i)], &y = b.poses[std::size_t(i)];
        if (x.shard != y.shard || x.x != y.x || x.y != y.y || x.z != y.z || x.rot != y.rot) return false;
    }
    return true;
}

}  // namespace

TEST_CASE("shatter: the shards tile the pane exactly, for one to six impacts; the count stays bounded") {
    for (int impacts = 1; impacts <= kMaxImpacts; ++impacts)
        for (uint64_t seed : {1ull, 7ull, 23ull}) {
            CAPTURE(impacts);
            CAPTURE(seed);
            Config c;
            c.impacts = impacts;
            c.spokes = impacts == 1 ? 9 : 7;
            const auto p = plan(seed, c);
            CHECK(p->shardCount > 20 * std::min(impacts, 3));
            CHECK(p->shardCount <= kMaxShards);
            CHECK(p->pane.count == impacts);
            checkTiling(*p, 320, 180);
            // Every shard's slot is unique and maps back to it.
            for (int s = 0; s < p->shardCount; ++s)
                CHECK(p->slotShard[std::size_t(p->shards[std::size_t(s)].slot)] == s);
        }
    // The densest web there is, six times: thinned to stay under the bound, still a tiling.
    Config d;
    d.impacts = 6;
    d.spokes = 12;
    d.rings = 1;
    d.fork = 1;
    d.size = 0.3;
    const auto p = plan(3, d);
    CHECK(p->shardCount <= kMaxShards);
    CHECK(p->shardCount > 200);
    checkTiling(*p, 320, 180);
    // A sparse one, and other aspects.
    Config sp;
    sp.spokes = 5;
    sp.rings = 0;
    sp.fork = 0;
    sp.size = 3;
    checkTiling(*plan(4, sp), 200, 120);
    for (double aspect : {1.0, 2.4}) {
        Plan q;
        makePlan(q, 5, Config{}, aspect, 62, 2, 2);
        CHECK(q.aspect == aspect);
        checkTiling(q, int(160 * aspect), 160);
    }
}

TEST_CASE("shatter: the web's structure: spokes and rings around the impact, the crack distance is right") {
    const auto p = plan(11, Config{});
    const Impact& m = p->pane.imp[0];
    CHECK(m.K >= 7);
    CHECK(m.K <= 11);
    CHECK(m.nb >= 4);
    // Going out from the impact along a ray, the band never decreases and steps one at a time.
    for (int a = 0; a < 24; ++a) {
        const double ang = 0.26 * a + 0.1;
        int band = 0;
        for (double r = 0.002; r < 2.5; r += 0.002) {
            const double x = m.cx + r * std::cos(ang), y = m.cy + r * std::sin(ang);
            if (x < 0 || y < 0 || x > kAspect || y > 1) break;
            const Hit h = owner(p->pane, float(x), float(y));
            CHECK(h.band >= band);
            CHECK(h.band <= band + 1);
            band = h.band;
        }
    }
    // The distance to the nearest crack is about the distance to where the shard changes.
    int checked = 0, close = 0;
    for (int y = 0; y < 90; ++y)
        for (int x = 0; x < 160; ++x) {
            const float px = float((x + 0.5) / 160 * kAspect), py = float((y + 0.5) / 90);
            const Hit h = owner(p->pane, px, py);
            if (h.dist > 0.02f || h.dist < 0.002f) continue;
            // Somewhere within 1.6 x the distance (+ a hair) the shard changes.
            bool change = false;
            const float r = h.dist * 1.6f + 0.002f;
            for (int k = 0; k < 32 && !change; ++k) {
                const float a = 0.19635f * float(k);
                change = owner(p->pane, px + r * std::cos(a), py + r * std::sin(a)).slot != h.slot;
            }
            ++checked;
            close += change ? 1 : 0;
        }
    CHECK(checked > 1000);
    CHECK(close > checked * 97 / 100);
}

TEST_CASE("shatter: cracks land on beat lines, spokes grow outwards, rings after their spokes") {
    for (int impacts : {1, 3}) {
        Config c;
        c.impacts = impacts;
        const auto p = plan(5, c, 62, 2, 2);
        CAPTURE(impacts);
        CHECK(p->land[0] == doctest::Approx(2.0));
        for (int i = 1; i < impacts; ++i) {
            CHECK(p->land[std::size_t(i)] > p->land[std::size_t(i - 1)] - 1e-3);
            // Later impacts on bar lines (pane beat 2 is a bar line here).
            CHECK(std::fmod(double(p->land[std::size_t(i)]) - 2.0, 4.0) == doctest::Approx(0.0));
        }
        std::set<int> beats;
        int shown = 0;
        for (int q = 0; q < p->elemCount; ++q) {
            const int e = p->elemList[std::size_t(q)];
            const Element& el = p->elems[std::size_t(e)];
            if (!el.shown()) continue;
            ++shown;
            const double b = std::floor(el.start);
            // Each starts on its beat line, or continues the crack below it within that beat.
            CHECK(el.start - b < 0.7);
            CHECK(el.start >= 2.0 - 1e-6);
            CHECK(el.start <= 61.0 + 0.7);
            if (el.start == b) beats.insert(int(b));
            // Spokes grow outwards; rings from one end.
            if ((e & 1) == 0 && e < 2 * kSlots) CHECK(el.dir == 1);
        }
        CHECK(shown == p->shown);
        CHECK(shown > p->elemCount * 6 / 10);
        CHECK(shown < p->elemCount);  // Crk .8: some only show as the pane breaks
        // Every beat from the first impact to the beat before the line has a crack landing exactly on it.
        for (int b = 2; b <= 61; ++b) CHECK(beats.count(b) == 1);
    }
    // Through the player, on the live clock: an element is still dark just before its beat line, growing just after.
    Config c;
    Player pl(5, c, kAspect, 0.0);
    pl.step(0.0, clockAt(64.0), Controls{});  // a line: the pane arrives at 66, its first impact on 68
    const Frame& f0 = pl.step(0.01, clockAt(66.5), Controls{});
    REQUIRE(f0.pane != nullptr);
    const Plan& p = *f0.pane;
    int tested = 0;
    for (int q = 0; q < p.elemCount && tested < 24; q += 5) {
        const int e = p.elemList[std::size_t(q)];
        const Element& el = p.elems[std::size_t(e)];
        if (!el.shown() || el.start != std::floor(el.start)) continue;
        const double line = 66.0 + el.start;  // the pane arrives on beat 66 (a whole beat)
        Player a(5, c, kAspect, 0.0);
        advance(a, 64.0, line - 0.02, 0.5);
        CHECK(a.step(0.01, clockAt(line - 0.02), Controls{}).reveal[std::size_t(e)] == 0.0f);
        CHECK(std::fabs(a.step(0.02, clockAt(line + 0.02), Controls{}).reveal[std::size_t(e)]) > 0.0f);
        CHECK(std::fabs(a.step(0.03, clockAt(line + 0.5), Controls{}).reveal[std::size_t(e)]) == doctest::Approx(1.0));
        ++tested;
    }
    CHECK(tested > 10);
}

TEST_CASE("shatter: the pane shatters exactly on the line, every shard at home; they fall and leave; a fresh pane") {
    Config c;
    c.cycle = 32;
    Player pl(21, c, kAspect, 0.0);
    for (int k = 0; k < 3; ++k) {
        const double line = 32.0 * (k + 1);
        advance(pl, k == 0 ? 0.0 : line - 28.0, line - 0.01);
        // Just before: the cracked pane, no shards.
        const Frame& b = pl.step(0.0, clockAt(line - 0.01), Controls{});
        REQUIRE(b.pane != nullptr);
        CHECK(b.loose == 0);
        CHECK(pl.paneEnd() == doctest::Approx(line));
        const uint64_t key = b.pane->key;
        int cracked = 0;
        for (int q = 0; q < b.pane->elemCount; ++q) cracked += b.reveal[std::size_t(b.pane->elemList[std::size_t(q)])] != 0;
        CHECK(cracked > b.pane->shown * 9 / 10);
        // On the line: no pane; every shard loose, at home, flat but for its hair of tilt.
        const Frame& f = pl.step(0.0, clockAt(line), Controls{});
        CHECK(f.pane == nullptr);
        REQUIRE(f.fallen != nullptr);
        CHECK(f.fallen->key == key);
        CHECK(f.fallBeat == doctest::Approx(0.0));
        const Plan& p = *f.fallen;
        CHECK(f.loose == p.shardCount);
        std::vector<int> count(std::size_t(p.shardCount), 0);
        for (int i = 0; i < f.loose; ++i) {
            const ShardPose& s = f.poses[std::size_t(i)];
            const Shard& sd = p.shards[std::size_t(s.shard)];
            ++count[std::size_t(s.shard)];
            CHECK(s.x == doctest::Approx(sd.cx).epsilon(1e-6));
            CHECK(s.y == doctest::Approx(sd.cy).epsilon(1e-6));
            CHECK(s.z == doctest::Approx(0.0));
            CHECK(s.rot[8] > 0.999f);   // facing the camera (but for the hair of tilt it had in the pane)
            CHECK(s.alpha == 1.0f);
        }
        for (int n : count) CHECK(n == 1);  // each shard exactly once: no gaps, no overlaps
        // They fall: a beat later lower down; within 3 beats all gone (gentle collapse).
        const Frame& g = pl.step(0.0, clockAt(line + 1.0), Controls{});
        double meanY = 0;
        for (int i = 0; i < g.loose; ++i) meanY += g.poses[std::size_t(i)].y;
        CHECK(g.loose > 0);
        CHECK(meanY / std::max(1, g.loose) > 0.5);
        CHECK(pl.step(0.0, clockAt(line + 1.9), Controls{}).pane == nullptr);  // the next one not yet arrived
        const Frame& h = pl.step(0.0, clockAt(line + 3.0), Controls{});
        CHECK(h.loose == 0);
        REQUIRE(h.pane != nullptr);  // the fresh pane, sliding in
        CHECK(h.pane->key != key);
        CHECK(h.offset < 0.0f);
        CHECK(pl.paneStart() == doctest::Approx(line + 2.0));
        CHECK(pl.step(0.0, clockAt(line + 4.0), Controls{}).offset == 0.0f);
        // Its first impact on beat Hit of the cycle.
        CHECK(pl.step(0.0, clockAt(line + 3.99), Controls{}).impactAge[0] < 0.0f);
        CHECK(pl.step(0.0, clockAt(line + 4.01), Controls{}).impactAge[0] >= 0.0f);
    }
    CHECK(pl.shatters() == 3);
    CHECK(pl.dropShatters() == 0);
}

TEST_CASE("shatter: a drop explodes the pane on the next 16th line; a drop just after a gentle shatter blows it apart") {
    Config c;
    Player pl(4, c, kAspect, 0.0);
    advance(pl, 0.0, 20.0, 0.1);
    // The drop at 20.1: the pane breaks on 20.25, explosively.
    CHECK(pl.step(0.0, clockAt(20.1, 0, 1.0), Controls{}).pane != nullptr);  // (still whole until the line)
    const Frame& f = pl.step(0.0, clockAt(20.26, 0, 0.95), Controls{});
    CHECK(pl.dropShatters() == 1);
    REQUIRE(f.fallen != nullptr);
    CHECK(f.big);
    CHECK(f.fallBeat == doctest::Approx(0.01).epsilon(1e-3));
    CHECK(f.loose == f.fallen->shardCount);
    // Thrown towards the camera.
    const Frame& g = pl.step(0.0, clockAt(20.75, 0, 0.8), Controls{});
    double z = 0;
    for (int i = 0; i < g.loose; ++i) z = std::max(z, double(g.poses[std::size_t(i)].z));
    CHECK(z > 0.1);
    // A fresh pane 3 beats after the line (on beat 24), shattering on the next line (64).
    CHECK(pl.step(0.0, clockAt(23.9, 0, 0.5), Controls{}).pane == nullptr);
    REQUIRE(pl.step(0.0, clockAt(24.2, 0, 0.4), Controls{}).pane != nullptr);
    CHECK(pl.paneStart() == doctest::Approx(24.0));
    CHECK(pl.paneEnd() == doctest::Approx(64.0));
    // Its own cracks: its first impact two beats after it arrives.
    CHECK(pl.step(0.0, clockAt(26.05, 0, 0.3), Controls{}).impactAge[0] >= 0.0f);
    // The line shatters it gently; a drop half a beat later blows the shards apart.
    advance(pl, 26.1, 64.0);
    Player calm(4, c, kAspect, 0.0);
    advance(calm, 0.0, 64.0);
    calm.step(0.0, clockAt(64.0), Controls{});
    calm.step(0.0, clockAt(64.5), Controls{});
    pl.step(0.0, clockAt(64.0), Controls{});
    pl.step(0.0, clockAt(64.5, 0, 1.0), Controls{});
    const Frame& u = pl.step(0.0, clockAt(65.0, 0, 0.9), Controls{});
    const Frame& v = calm.step(0.0, clockAt(65.0), Controls{});
    CHECK(u.loose > 0);
    double spreadU = 0, spreadV = 0;
    for (int i = 0; i < u.loose; ++i) spreadU = std::max(spreadU, double(u.poses[std::size_t(i)].z));
    for (int i = 0; i < v.loose; ++i) spreadV = std::max(spreadV, double(v.poses[std::size_t(i)].z));
    CHECK(spreadU > spreadV + 0.1);
}

TEST_CASE("shatter: a build makes the cracks due next creep ahead on the 16ths, never back") {
    Config c;
    Player pl(8, c, kAspect, 0.0);
    advance(pl, 0.0, 10.0, 0.125);
    const Frame& f = pl.step(0.0, clockAt(10.0), Controls{});
    REQUIRE(f.pane != nullptr);
    const Plan& p = *f.pane;
    // Elements due in beats 11..14 (pane beats 9..12: all within 4 beats of the 16th line at 10.25).
    std::vector<int> due;
    for (int q = 0; q < p.elemCount; ++q) {
        const int e = p.elemList[std::size_t(q)];
        const Element& el = p.elems[std::size_t(e)];
        if (el.shown() && el.start > 9.0 && el.start < 12.0) due.push_back(e);
    }
    REQUIRE(due.size() > 3);
    for (int e : due) CHECK(f.reveal[std::size_t(e)] == 0.0f);
    const Frame& g = pl.step(0.0, clockAt(10.3, 1.0), Controls{});
    int crept = 0;
    std::vector<float> was;
    for (int e : due) {
        const float r = std::fabs(g.reveal[std::size_t(e)]);
        CHECK(r <= 0.4f + 1e-6f);
        crept += r > 0.0f;
        was.push_back(r);
    }
    CHECK(crept == int(due.size()));
    // The build falls: they hold.
    const Frame& h = pl.step(0.0, clockAt(10.5, 0.0), Controls{});
    for (std::size_t i = 0; i < due.size(); ++i) CHECK(std::fabs(h.reveal[std::size_t(due[i])]) >= was[i]);
    CHECK(h.build == 0.0f);
}

TEST_CASE("shatter: deterministic, and the same at any frame rate") {
    Config c;
    c.cycle = 16;
    c.impacts = 2;
    for (bool withClock : {false, true}) {
        CAPTURE(withClock);
        // Beat 14 (the cracked pane), 17 and 33 (shards falling a beat after the lines at 16 and 32).
        for (double seconds : {7.0, 8.5, 16.5}) {
            const auto a = run(c, seconds, 60, withClock);
            const auto b = run(c, seconds, 60, withClock);
            CHECK(sameFrame(a->frame(), b->frame()));
            const auto c30 = run(c, seconds, 30, withClock);
            const auto c144 = run(c, seconds, 144, withClock);
            CHECK(sameFrame(a->frame(), c30->frame()));
            CHECK(sameFrame(a->frame(), c144->frame()));
            const auto other = run(c, seconds, 60, withClock, 10);
            CHECK_FALSE(sameFrame(a->frame(), other->frame()));
        }
    }
}

TEST_CASE("shatter: clock jumps re-base to the pane due there") {
    Config c;
    Player pl(3, c, kAspect, 0.0);
    advance(pl, 0.0, 30.0, 0.2);
    CHECK(pl.rebases() == 0);
    // Back to beat 1: the pane of that cycle, arriving on beat 2.
    pl.step(0.0, clockAt(1.0), Controls{});
    CHECK(pl.rebases() == 1);
    CHECK(pl.paneStart() == doctest::Approx(2.0));
    CHECK(pl.paneEnd() == doctest::Approx(64.0));
    // Far ahead, mid-cycle: a pane arriving on the next beat line, shattering on a line at least 12 beats on.
    const Frame& f = pl.step(0.0, clockAt(1000.4), Controls{});
    CHECK(pl.rebases() == 2);
    CHECK(f.loose == 0);
    CHECK(pl.paneStart() == doctest::Approx(1001.0));
    CHECK(pl.paneEnd() == doctest::Approx(1024.0));
    // Near the end of a cycle: the pane runs to the line after.
    pl.step(0.0, clockAt(2040.0), Controls{});
    CHECK(pl.paneStart() == doctest::Approx(2040.0));
    CHECK(pl.paneEnd() == doctest::Approx(2112.0));
    CHECK(pl.rebases() == 3);
    // The clock turning off: 120 BPM from the start.
    const Frame& g = pl.step(5.0, Clock{}, Controls{});
    CHECK(g.beat == doctest::Approx(10.0));
    CHECK(pl.rebases() == 4);
    // Small steps back (a phrase re-alignment nudging the clock by less than a 16th) do not re-base.
    pl.step(5.1, Clock{}, Controls{});
    pl.step(5.05, Clock{}, Controls{});
    CHECK(pl.rebases() == 4);
}

TEST_CASE("shatter: junk inputs stay finite and bounded") {
    const double nan = std::numeric_limits<double>::quiet_NaN(), inf = std::numeric_limits<double>::infinity();
    Config c;
    c.impacts = 99;
    c.cycle = nan;
    c.hit = -inf;
    c.spokes = 1e300;
    c.rings = nan;
    c.fork = -5;
    c.cracked = inf;
    c.size = 0;
    c.arrive = 7;
    const Config k = clampConfig(c);
    CHECK(k.impacts == kMaxImpacts);
    CHECK(k.cycle == 64);
    CHECK(k.hit >= 4);
    CHECK(k.spokes == 12);
    CHECK(k.cracked == doctest::Approx(0.8));  // (inf is junk: the default)
    Player pl(1, c, nan, nan);
    const double junk[] = {nan, inf, -inf, 1e300, -1e300, 0.0, 3.0};
    int i = 0;
    for (double a : junk)
        for (double b : junk) {
            Clock clk;
            clk.on = (i % 2) == 0;
            clk.bar = a;
            clk.build = b;
            clk.drop = a;
            Controls ctl;
            ctl.burst = b;
            ctl.gravity = a;
            ctl.tilt = b;
            const Frame& f = pl.step(i % 3 == 0 ? nan : double(i) * 0.37, clk, ctl);
            ++i;
            CHECK(std::isfinite(f.beat));
            CHECK(f.loose >= 0);
            CHECK(f.loose <= kMaxShards);
            CHECK(std::isfinite(f.offset));
            for (int q = 0; q < f.loose; ++q) {
                const ShardPose& s = f.poses[std::size_t(q)];
                CHECK(std::isfinite(s.x));
                CHECK(std::isfinite(s.y));
                CHECK(std::isfinite(s.z));
                for (float r : s.rot) CHECK(std::isfinite(r));
            }
            if (f.pane)
                for (int q = 0; q < f.pane->elemCount; ++q) {
                    const int e = f.pane->elemList[std::size_t(q)];
                    CHECK(std::isfinite(f.reveal[std::size_t(e)]));
                    CHECK(std::fabs(f.reveal[std::size_t(e)]) <= 1.0f);
                }
        }
    // Run a long while at 120 BPM after the junk: panes keep coming and going, plans are reused.
    for (int s = 0; s < 6000; ++s) pl.step(100.0 + s / 30.0, clockAt(400.0 + s / 15.0), Controls{});
    CHECK(pl.shatters() >= 5);
    CHECK(pl.planBuilds() < 40);  // (one per pane, and the junk's re-bases: not one per frame)
}

TEST_CASE("shatter: the solid's keys and the content scenes") {
    namespace fs = std::filesystem;
    const fs::path pkg = fs::path(REFORCE_SOURCE_DIR) / "content/Packages/reforce.Solid.package";
    struct Want {
        const char* name;
        int impacts;
        double cycle, burst;
    };
    for (const Want w : {Want{"Shatter Drop", 1, 64, 1.0}, Want{"Shatter Hail", 5, 32, 1.0}}) {
        CAPTURE(w.name);
        std::ifstream in(pkg / (std::string(w.name) + ".txt"));
        REQUIRE(in);
        std::stringstream ss;
        ss << in.rdbuf();
        auto file = reforce::scene::parseSceneText(ss.str(), w.name);
        auto cfg = std::make_shared<reforce::scene::CompiledConfig>(
            reforce::scene::compileConfig(file, reforce::scene::SceneType::Solid));
        REQUIRE(solidMesh(*cfg) == MeshKind::Shatter);
        CHECK(objectFollowsBeat(reforce::scene::SceneType::Solid, *cfg));
        reforce::scene::Environment env;
        env.seed = 77;
        SolidRun r(cfg, env, w.name, MeshKind::Shatter, 0.0, 0.0, 400.0, nullptr);
        r.setAspect(kAspect);
        CHECK(r.shatterConfig().impacts == w.impacts);
        CHECK(r.shatterConfig().cycle == w.cycle);
        int shattered = 0;
        bool sawPane = false;
        for (int i = 0; i <= 2 * int(w.cycle) * 8; ++i) {  // two cycles in 8ths
            SpriteAudio a;
            a.sync.on = 1;
            a.sync.beat = i / 8.0;
            a.sync.bar = a.sync.beat / 4;
            a.sync.phrase = a.sync.bar / 16;
            const SolidSpec sp = r.evaluate(a.sync.beat / 2, a);
            REQUIRE(sp.mesh == MeshKind::Shatter);
            REQUIRE(sp.shatter);
            CHECK(std::isfinite(sp.shatterLook.depth));
            const Frame& f = sp.shatter->frame();
            sawPane = sawPane || f.pane != nullptr;
            // On each line it shatters.
            if (i > 0 && i % (int(w.cycle) * 8) == 0) {
                CHECK(f.loose > 0);
                CHECK(f.pane == nullptr);
                ++shattered;
            }
        }
        CHECK(sawPane);
        CHECK(shattered == 2);
        CHECK(r.shatterPlayer()->shatters() == 2);
        CHECK(r.shatterPlayer()->aspect() == doctest::Approx(kAspect));
    }
    // Defaults and per-frame keys.
    auto compile = [](const std::string& text) {
        auto file = reforce::scene::parseSceneText(text, "test");
        return std::make_shared<reforce::scene::CompiledConfig>(
            reforce::scene::compileConfig(file, reforce::scene::SceneType::Solid));
    };
    reforce::scene::Environment env;
    env.seed = 5;
    {
        SolidRun r(compile("Mesh=\"shatter\"\n"), env, "s", MeshKind::Shatter, 0.0, 1.0, 40.0, nullptr);
        CHECK(r.shatterConfig().impacts == 1);
        CHECK(r.shatterConfig().cycle == 64);
        CHECK(r.shatterSeed() == 5);
        const SolidSpec sp = r.evaluate(0.5);
        CHECK(sp.shatterLook.depth == doctest::Approx(0.35));
        CHECK(sp.shatterLook.line == doctest::Approx(1.0));
    }
    {
        SolidRun r(compile("Mesh=\"shatter\"\nImp=\"3\"\nCycl=\"32\"\nSeed=\"12\"\nSize=\".5 + .5\"\nArrv=\"1\"\n"
                           "Dpth=\".1 + .1\"\nLine=\"9\"\nGls=\"0\"\n"),
                   env, "t", MeshKind::Shatter, 0.0, 1.0, 40.0, nullptr);
        CHECK(r.shatterConfig().impacts == 3);
        CHECK(r.shatterConfig().cycle == 32);
        CHECK(r.shatterConfig().size == doctest::Approx(1.0));
        CHECK(r.shatterConfig().arrive == 1);
        CHECK(r.shatterSeed() == 12);
        const SolidSpec sp = r.evaluate(0.5);
        CHECK(sp.shatterLook.depth == doctest::Approx(0.2));
        CHECK(sp.shatterLook.line == doctest::Approx(4.0));  // clamped
        CHECK(sp.shatterLook.sheen == doctest::Approx(0.0));
    }
}
