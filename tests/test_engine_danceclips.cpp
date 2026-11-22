// Mocap dance moves (docs/dancers-design.md "Mocap moves"): the moves file from tools/aist_import, clip poses,
// bend hints, and the choreographer's mocap share and move browser.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <set>
#include <string>

#include "engine/DanceClips.hpp"
#include "engine/DanceMoves.hpp"
#include "engine/DancerRig.hpp"

using namespace reforce::engine::dance;

namespace {

const std::string kMovesFile = std::string(REFORCE_SOURCE_DIR) + "/content/Moves/aist.moves";

// Loads the committed moves file once (the library is process-wide).
bool haveClips() {
    static const bool ok = [] {
        const std::string err = loadClips(kMovesFile);
        if (!err.empty()) std::fprintf(stderr, "loadClips: %s\n", err.c_str());
        return err.empty();
    }();
    return ok;
}

bool finite(V3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

MusicState at(double beat) {
    MusicState g;
    g.beat = beat;
    g.bar = beat / 4;
    g.phrase = beat / 64;
    g.bass = 0.5;
    return g;
}

// Characters at the ends of the body sliders.
std::vector<Skeleton> skeletons() {
    std::vector<Skeleton> v;
    Look a;
    v.push_back(makeSkeleton(a));
    Look b;
    b.tall = 0.85f;
    b.legs = 0.8f;
    b.build = 1.25f;
    b.head = 1.3f;
    v.push_back(makeSkeleton(b));
    Look c;
    c.tall = 1.15f;
    c.legs = 1.2f;
    c.build = 0.8f;
    c.shoulders = 1.2f;
    v.push_back(makeSkeleton(c));
    return v;
}

}  // namespace

TEST_CASE("dance clips: the moves file loads, every genre has basic moves and routines") {
    REQUIRE(haveClips());
    CHECK(clipCount() >= 300);
    std::set<std::string> ids;
    for (std::string_view g : clipGenres()) {
        int basic = 0, routine = 0;
        for (int i = 0; i < clipCount(); ++i)
            if (clipInfo(i).genre == g) (clipInfo(i).routine ? routine : basic)++;
        CAPTURE(std::string(g));
        CHECK(basic >= 10);
        CHECK(routine >= 5);
        REQUIRE(clipGenreStyle(g));
        CHECK(clipGenreStyles(g) == styleBit(*clipGenreStyle(g)));
    }
    for (int i = 0; i < clipCount(); ++i) {
        const ClipInfo& c = clipInfo(i);
        CHECK(ids.insert(c.id).second);
        // Basic moves are their 4 bars; routines 16 (a phrase) or, where only half was clean, 8.
        CHECK((c.routine ? (c.beats == 64 || c.beats == 32) : c.beats == 16));
        CHECK(c.bpm >= 80);
        CHECK(c.bpm <= 135);
        REQUIRE(c.move >= 0);
        const MoveDef& m = moveLibrary()[std::size_t(c.move)];
        CHECK(m.clip == i);
        CHECK(m.part == Part::Full);
        CHECK(m.styles == clipGenreStyles(c.genre));
        CHECK(findClip(c.id) == i);
    }
    // Loading again replaces the moves (no duplicates in the library).
    const std::size_t n = moveLibrary().size();
    REQUIRE(loadClips(kMovesFile).empty());
    CHECK(moveLibrary().size() == n);
}

TEST_CASE("dance clips: a broken file is refused and changes nothing") {
    REQUIRE(haveClips());
    const std::size_t n = moveLibrary().size();
    const std::string bad = std::string(REFORCE_SOURCE_DIR) + "/build/test-bad.moves";
    {
        std::ifstream in(kMovesFile, std::ios::binary);
        std::string head(4000, '\0');
        in.read(head.data(), std::streamsize(head.size()));
        std::ofstream out(bad, std::ios::binary);
        out.write(head.data(), std::streamsize(head.size()));  // cut off mid-move
    }
    CHECK_FALSE(loadClips(bad).empty());
    CHECK_FALSE(loadClips(std::string(REFORCE_SOURCE_DIR) + "/README.md").empty());
    CHECK_FALSE(loadClips("/nonexistent/x.moves").empty());
    CHECK(moveLibrary().size() == n);
    CHECK(clipCount() > 0);
    std::remove(bad.c_str());
}

TEST_CASE("dance clips: every move is finite, above the floor, within reach and continuous round its loop") {
    REQUIRE(haveClips());
    int jumpy = 0, moves = 0;
    long samples = 0, farHands = 0;
    for (const Skeleton& sk : skeletons()) {
        const float arm = sk.upperArm + sk.forearm, leg = sk.thigh + sk.shin;
        for (int i = 0; i < clipCount(); ++i) {
            CAPTURE(clipInfo(i).id);
            DancePose prev;
            float worst = 0;
            const int beats = clipInfo(i).beats;
            for (int k = 0; k <= beats * 30; ++k) {  // the loop and its wrap, 30 frames a beat
                DancePose p = restPose(sk);
                clipPose(i, float(k) / 30.0f, sk, p);
                REQUIRE(finite(p.hip));
                REQUIRE(finite(p.handL));
                REQUIRE(finite(p.handR));
                REQUIRE(finite(p.footL));
                REQUIRE(finite(p.footR));
                REQUIRE(std::isfinite(p.bodyYaw));
                CHECK(p.footL.y >= 0);
                CHECK(p.footR.y >= 0);
                // Hands within about arm's reach of their shoulder (straight arms, as the dancer had them, read a
                // little over 1 where the tracking stretched them; the IK stops them at full stretch).
                const V3 shL{sk.shoulderHalf, sk.shoulderY, 0}, shR{-sk.shoulderHalf, sk.shoulderY, 0};
                const float reach = std::max(length(p.handL - shL), length(p.handR - shR)) / arm;
                CHECK(reach < 1.35f);
                ++samples;
                if (reach > 1.1f) ++farHands;
                CHECK(std::fabs(p.hip.y) < 0.8f * leg);
                CHECK(length(p.elbowL) <= 1.001f);
                CHECK(length(p.kneeR) <= 1.001f);
                if (k > 0) {
                    float yaw = std::remainder(p.bodyYaw - prev.bodyYaw, 6.2831853f);
                    worst = std::max({worst, length(p.handL - prev.handL) / arm, length(p.handR - prev.handR) / arm,
                                      length(p.footL - prev.footL) / leg, length(p.footR - prev.footR) / leg,
                                      length(p.hip - prev.hip) / leg, std::fabs(yaw)});
                }
                prev = p;
            }
            // At 30 frames a beat nothing jumps further than a fast real dancer moves in one frame.
            ++moves;
            if (worst > 0.4f) ++jumpy;
            CHECK(worst < 0.8f);
        }
    }
    MESSAGE("hands out of reach in " << 100.0 * double(farHands) / double(samples) << " % of samples; " << jumpy
                                     << " of " << moves << " moves with a fast jump");
    CHECK(farHands < samples / 6);  // straight arms (as the dancers had them) are common
    CHECK(jumpy <= moves / 12);  // a few very fast moments (16-bar routines have many chances)
}

TEST_CASE("dance clips: moves fit each skeleton (hands and feet scale with the limbs)") {
    REQUIRE(haveClips());
    const auto sks = skeletons();
    const Skeleton& small = sks[1];
    const Skeleton& tall = sks[2];
    int clip = findClip(clipInfo(0).id);
    REQUIRE(clip >= 0);
    DancePose a = restPose(small), b = restPose(tall);
    clipPose(clip, 2.5f, small, a);
    clipPose(clip, 2.5f, tall, b);
    // The same angles; lengths in proportion to the legs.
    CHECK(a.hipYaw == doctest::Approx(b.hipYaw));
    CHECK(a.chestPitch == doctest::Approx(b.chestPitch));
    const float ls = small.thigh + small.shin, lt = tall.thigh + tall.shin;
    CHECK(a.hip.y / ls == doctest::Approx(b.hip.y / lt).epsilon(1e-3));
    CHECK(a.footL.z / ls == doctest::Approx(b.footL.z / lt).epsilon(1e-3));
}

TEST_CASE("dance clips: bend hints mix, mirror and steer the elbows and knees") {
    const Skeleton sk = makeSkeleton(Look{});
    DancePose p = restPose(sk);
    p.handL = {sk.shoulderHalf + 0.05f, sk.shoulderY - 0.12f, 0.12f};  // a bent arm
    p.footL = {sk.footHalf, 0, 0};
    p.hip.y = -0.06f;  // knees bent
    DancePose q = p;
    q.elbowL = {0, 1, 0};  // elbow up
    q.kneeL = {1, 0, 0};   // knee out to the character's left
    BodyOut a, b;
    buildBody(Look{}, sk, p, Frame{}, 1.0f, a);
    buildBody(Look{}, sk, q, Frame{}, 1.0f, b);
    // Hands and feet stay where the pose puts them; the elbow and knee move the hinted way.
    CHECK(length(a.hands[0] - b.hands[0]) < 1e-3f);
    CHECK(length(a.feet[0] - b.feet[0]) < 1e-3f);
    // The left upper arm's and thigh's far ends: the elbow higher, the knee further out to the left.
    auto farEnd = [](const BodyOut& o, float len, bool left) {
        for (const BodyPart& part : o.parts)
            if (std::fabs(part.len - len) < 2e-3f && (part.frame.o.x > 0) == left) return part.frame.point({0, part.len, 0});
        return V3{NAN, NAN, NAN};
    };
    const V3 elbowA = farEnd(a, sk.upperArm, true), elbowB = farEnd(b, sk.upperArm, true);
    const V3 kneeA = farEnd(a, sk.thigh, true), kneeB = farEnd(b, sk.thigh, true);
    REQUIRE(finite(elbowA));
    REQUIRE(finite(kneeA));
    CHECK(elbowB.y > elbowA.y + 0.02f);
    CHECK(kneeB.x > kneeA.x + 0.01f);
    const DancePose m = mirrored(q);
    CHECK(m.elbowR.y == doctest::Approx(1));
    CHECK(m.kneeR.x == doctest::Approx(-1));
    CHECK(length(m.elbowL) == doctest::Approx(0));
    const DancePose half = mix(p, q, 0.5f);
    CHECK(half.elbowL.y == doctest::Approx(0.5f));
    // Round trip through the pose floats (inertial blending).
    float f[kPoseFloats];
    poseToFloats(q, f);
    const DancePose r = poseFromFloats(f);
    CHECK(r.kneeL.x == doctest::Approx(1));
    CHECK(r.elbowL.y == doctest::Approx(1));
}

TEST_CASE("dance clips: the choreographer mixes in mocap moves by the Mocap share") {
    REQUIRE(haveClips());
    const Skeleton sk = makeSkeleton(Look{});
    int routines = 0, near = 0, mocapPicks = 0;
    auto count = [&](float share, Style style) {
        ChoreoOptions o;
        o.style = style;
        o.seed = 7;
        o.mocap = share;
        Choreographer ch(o);
        int mocap = 0, total = 0;
        const Routine* last = nullptr;
        for (double beat = 0; beat < 2000; beat += 0.25) {
            ch.pose(at(beat), sk);
            const Routine* r = ch.routineAt(beat);
            if (r && r != last) {
                ++total;
                if (r->full >= 0 && moveLibrary()[std::size_t(r->full)].clip >= 0) {
                    ++mocap;
                    const ClipInfo& c = clipInfo(moveLibrary()[std::size_t(r->full)].clip);
                    // Only moves of this style's genres (phrases are 64 beats from 0 here).
                    CHECK((moveLibrary()[std::size_t(r->full)].styles & styleBit(style)) != 0);
                    REQUIRE(r->aligned);
                    if (c.beats >= 32) {
                        // A routine starts on an 8-bar point of the phrase and plays whole from its bar 1 (unless a
                        // drop cuts in, none here).
                        ++routines;
                        CHECK(std::fmod(r->start, 32.0) == doctest::Approx(0));
                        CHECK(r->origin == r->start);
                        CHECK(r->end - r->start == doctest::Approx(64.0));  // 16 bars (an 8-bar half twice)
                    } else {
                        // A basic move: on the phrase's 4-bar grid, at least a bar, changing on the phrase line.
                        CHECK(std::fmod(r->origin, 16.0) == doctest::Approx(0));
                        CHECK(r->origin <= r->start);
                        CHECK(r->end - r->start >= 4 - 1e-9);
                        CHECK(std::floor(r->end / 64 - 1e-9) <= std::floor(r->start / 64));
                    }
                    ++mocapPicks;
                    if (c.bpm >= 118 && c.bpm <= 132) ++near;
                }
                last = r;
            }
        }
        return std::pair<int, int>{mocap, total};
    };
    CHECK(count(0, Style::House).first == 0);
    const auto [some, n] = count(0.5f, Style::House);
    CHECK(some > n / 4);
    CHECK(some < n * 3 / 4);
    const auto [all, n1] = count(1, Style::HipHop);
    CHECK(all == n1);
    CHECK(routines > 0);
    // Moves recorded at 120-130 BPM come up more than their share of the library.
    int inRange = 0;
    for (int i = 0; i < clipCount(); ++i)
        if (clipInfo(i).bpm >= 118 && clipInfo(i).bpm <= 132) ++inRange;
    CHECK(double(near) / mocapPicks > double(inRange) / clipCount());
}

TEST_CASE("dance clips: trying a move loops it from the next bar line, and Stop gives the choreography back") {
    REQUIRE(haveClips());
    const Skeleton sk = makeSkeleton(Look{});
    ChoreoOptions o;
    o.style = Style::Disco;
    Choreographer ch(o);
    for (double beat = 0; beat < 10.5; beat += 0.25) ch.pose(at(beat), sk);
    const int move = clipInfo(5).move;
    ch.tryMove(move);
    for (double beat = 10.5; beat < 40; beat += 0.25) ch.pose(at(beat), sk);
    const Routine* r = ch.routineAt(39);
    REQUIRE(r);
    CHECK(r->full == move);
    CHECK(r->start == doctest::Approx(12));  // the next bar line after beat 10.5
    CHECK(ch.routineAt(11.5)->full != move);
    // From its own start on that bar line.
    DancePose want = restPose(sk), got = ch.pose(at(44), sk);
    clipPose(5, float(std::fmod(44.0 - 12.0, double(clipInfo(5).beats))), sk, want);
    CHECK(got.hip.y == doctest::Approx(want.hip.y).epsilon(1e-3));
    ch.tryMove(-1);
    for (double beat = 40; beat < 60; beat += 0.25) ch.pose(at(beat), sk);
    CHECK(ch.routineAt(59)->full != move);
}

TEST_CASE("dance clips: at Mocap share 1 every routine is a mocap move, and a drop starts a high-energy one") {
    REQUIRE(haveClips());
    const Skeleton sk = makeSkeleton(Look{});
    for (int si = 0; si < kStyleCount; ++si) {
        const Style style = Style(si);
        CAPTURE(styleName(style));
        ChoreoOptions o;
        o.style = style;
        o.seed = 11;
        o.mocap = 1;
        Choreographer ch(o);
        int drops = 0;
        for (double beat = 0; beat < 1200; beat += 0.25) {
            MusicState g = at(beat);
            const bool drop = std::fmod(beat, 256.0) == 128.0;  // a drop on a phrase line now and then
            if (drop) g.drop = 1;
            ch.pose(g, sk);
            const Routine* r = ch.routineAt(beat);
            REQUIRE(r);
            REQUIRE(r->full >= 0);
            const MoveDef& m = moveLibrary()[std::size_t(r->full)];
            CHECK(m.clip >= 0);  // never a hand-made move (no jump on the drop either)
            if (drop) {
                ++drops;
                CHECK(r->start == doctest::Approx(beat));
                CHECK((m.energy & Hype) != 0);
            }
        }
        CHECK(drops >= 4);
    }
}

TEST_CASE("dance clips: routines take about 60 % of the dancing, basic moves the rest") {
    REQUIRE(haveClips());
    const Skeleton sk = makeSkeleton(Look{});
    for (Style style : {Style::House, Style::HipHop, Style::Jazz}) {
        CAPTURE(styleName(style));
        double routine = 0, total = 0;
        for (std::uint32_t seed = 1; seed <= 6; ++seed) {
            ChoreoOptions o;
            o.style = style;
            o.seed = seed;
            o.mocap = 1;
            Choreographer ch(o);
            for (double beat = 0; beat < 2048; beat += 0.5) {
                ch.pose(at(beat), sk);
                const Routine* r = ch.routineAt(beat);
                total += 1;
                if (clipInfo(moveLibrary()[std::size_t(r->full)].clip).routine) routine += 1;
            }
        }
        CHECK(routine / total > 0.5);
        CHECK(routine / total < 0.7);
    }
}

TEST_CASE("dance clips: every dance style has basic moves and routines of its own") {
    REQUIRE(haveClips());
    for (int si = 0; si < kStyleCount; ++si) {
        CAPTURE(styleName(Style(si)));
        int basic = 0, routine = 0;
        for (int i = 0; i < clipCount(); ++i)
            if (clipGenreStyle(clipInfo(i).genre) == Style(si)) (clipInfo(i).routine ? routine : basic)++;
        CHECK(basic >= 20);
        CHECK(routine >= 5);
    }
}

TEST_CASE("dance clips: loading from memory (the web preview) gives the same moves as from the file") {
    REQUIRE(haveClips());
    const int n = clipCount();
    const std::string first = clipInfo(0).id;
    std::ifstream in(kMovesFile, std::ios::binary);
    const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    REQUIRE(loadClipsFromMemory(bytes.data(), bytes.size()).empty());
    CHECK(clipCount() == n);
    CHECK(clipInfo(0).id == first);
    CHECK_FALSE(loadClipsFromMemory(bytes.data(), 100).empty());  // cut short: refused, nothing changed
    CHECK_FALSE(loadClipsFromMemory(nullptr, 0).empty());
    CHECK(clipCount() == n);
}

TEST_CASE("dance clips: watching someone is a calm mocap basic move of the style, a new one every 4 bars") {
    REQUIRE(haveClips());
    const Skeleton sk = makeSkeleton(Look{});
    for (int si = 0; si < kStyleCount; ++si) {
        const Style st = Style(si);
        CAPTURE(styleName(st));
        std::set<std::string> seen;
        for (double beat = 0; beat < 16 * 40; beat += 16) {
            const std::string name = watchName(beat + 3, 7, st, true);
            CHECK(name.find(" (watching)") != std::string::npos);
            const std::string move = name.substr(0, name.find(" (watching)"));
            const int m = findClip(move);
            REQUIRE(m >= 0);
            CHECK_FALSE(clipInfo(m).routine);
            CHECK(clipGenreStyle(clipInfo(m).genre) == st);
            CHECK(watchName(beat + 15.9, 7, st, true) == name);  // the same move all 4 bars
            // The pose is that move at the beat's place in the 4 bars.
            DancePose want = restPose(sk);
            clipPose(m, 3.0f, sk, want);
            const DancePose got = watchPose(beat + 3, 7, st, sk, true);
            CHECK(length(got.handL - want.handL) < 1e-4f);
            seen.insert(move);
        }
        CHECK(seen.size() >= 3);  // variety
    }
    // Without mocap: the hand-made reactions.
    CHECK(watchName(5, 7, Style::House, false).find(" (watching)") != std::string::npos);
    CHECK(findClip(watchName(5, 7, Style::House, false).substr(0, watchName(5, 7, Style::House, false).find(" ("))) < 0);
}
