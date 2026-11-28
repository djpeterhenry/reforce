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
                        CHECK(r->end - r->start == doctest::Approx(double(c.beats)));  // once, whole (no repeat)
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
        MESSAGE(styleName(style) << ": routines " << int(100 * routine / total + 0.5) << " %");
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

TEST_CASE("dance clips: watching someone is a mocap move of the style (any energy): routines or basics") {
    // Peter, 2026-11-27: watchers dance full routines too; an 8-bar one plays once (no forced repeat), the next half
    // picked afresh; else a basic move per 4 bars; standing only.
    REQUIRE(haveClips());
    const Skeleton sk = makeSkeleton(Look{});
    for (int si = 0; si < kStyleCount; ++si) {
        const Style st = Style(si);
        CAPTURE(styleName(st));
        std::set<std::string> seen;
        int routineSlots = 0, basicSlots = 0;
        std::string prev;
        for (double beat = 0; beat < 64 * 30; beat += 16) {  // every 4-bar slot
            const std::string name = watchName(beat + 3, 7, st, true);
            CHECK(name.find(" (watching)") != std::string::npos);
            const std::string move = name.substr(0, name.find(" (watching)"));
            const int m = findClip(move);
            REQUIRE(m >= 0);
            const ClipInfo& c = clipInfo(m);
            CHECK(clipGenreStyle(c.genre) == st);
            CHECK(c.tier == kStanding);
            CHECK(!c.floorWork);
            CHECK(watchName(beat + 15.9, 7, st, true) == name);  // the same all 4 bars
            // A move plays from its own beat 0 on its 4-bar (basic), 8-bar (half) or 16-bar (whole) grid point, whole.
            const double len = c.beats;
            if (std::fmod(beat, len) != 0) CHECK(move == prev);
            (c.routine ? routineSlots : basicSlots) += 1;
            DancePose want = restPose(sk);
            clipPose(m, float(std::fmod(beat + 3, len)), sk, want);
            const DancePose got = watchPose(beat + 3, 7, st, sk, true);
            CHECK(length(got.handL - want.handL) < 1e-4f);
            seen.insert(move);
            prev = move;
        }
        MESSAGE(styleName(st) << ": " << routineSlots << " slots in routines, " << basicSlots << " in basics");
        CHECK(routineSlots > 0);
        CHECK(basicSlots > 0);
        CHECK(seen.size() >= 3);  // variety
    }
    // Without mocap: the hand-made reactions.
    CHECK(watchName(5, 7, Style::House, false).find(" (watching)") != std::string::npos);
    CHECK(findClip(watchName(5, 7, Style::House, false).substr(0, watchName(5, 7, Style::House, false).find(" ("))) < 0);
}

// ---- Break floor work (docs/dancers-design.md "Break floor work"): content/Moves/aist_floor.moves after the standing
// moves, the pelvis quaternion, groundPose, the tier weights.
namespace {

const std::string kFloorFile = std::string(REFORCE_SOURCE_DIR) + "/content/Moves/aist_floor.moves";

// The floor moves appended to the standing ones (as main.cpp loads them); afterwards the standing moves alone again and
// the weights off, as the other tests expect.
struct WithFloor {
    int standing = 0;
    WithFloor() {
        REQUIRE(haveClips());
        REQUIRE(loadClips(kMovesFile).empty());
        standing = clipCount();
        const std::string err = loadClips(kFloorFile, true);
        if (!err.empty()) std::fprintf(stderr, "loadClips: %s\n", err.c_str());
        REQUIRE(err.empty());
    }
    ~WithFloor() {
        setFloorWeights(kDefaultFloorWeight);
        loadClips(kMovesFile);
    }
};

// The lowest point of a built body (its parts' ends less their radii), leaving out the shoes (a standing foot's sole
// sits on the floor by design) and the hair (big hair may brush the floor on a headspin: groundPose holds the head).
float lowestPoint(const BodyOut& o) {
    float low = 1e9f;
    for (const BodyPart& part : o.parts) {
        if (part.material == Material::Hair || part.material == Material::Strands) continue;
        if (length(part.frame.o - o.feet[0]) < 0.09f || length(part.frame.o - o.feet[1]) < 0.09f) continue;
        const float s = std::max({part.scaleX, part.scaleZ, 1.0f});
        low = std::min({low, part.frame.o.y - part.r0 * s, part.frame.point({0, part.len, 0}).y - part.r1 * s});
    }
    return low;
}

}  // namespace

TEST_CASE("floor work: the floor moves load after the standing ones, tiered, break only") {
    WithFloor f;
    CHECK(f.standing == 543);  // aist.moves as it was
    int tiers[4] = {0, 0, 0, 0};
    std::set<std::string> ids, labels;
    for (int i = 0; i < clipCount(); ++i) {
        const ClipInfo& c = clipInfo(i);
        CHECK(ids.insert(c.id).second);
        CHECK(labels.insert(c.label).second);  // (beatset dancetry by label)
        CHECK(moveLibrary()[std::size_t(c.move)].clip == i);
        CHECK(c.floorWork == (i >= f.standing));
        if (i < f.standing) {
            CHECK(c.tier == kStanding);
            continue;
        }
        CAPTURE(c.label);
        CHECK(c.genre == "BR");
        REQUIRE((c.tier == kFloorTier || c.tier == kStanding));  // (no flips: Peter, 2026-11-27)
        ++tiers[c.tier];
        // Floor moves say so ("break floor basic 7 ..."); the mostly standing ones are plain break routines.
        CHECK((c.label.find("break floor ") == 0) == (c.tier == kFloorTier));
        CHECK((c.routine ? (c.beats == 64 || c.beats == 32) : c.beats == 16));
    }
    MESSAGE("from the floor file: " << tiers[kFloorTier] << " floor moves, " << tiers[kStanding]
                                    << " standing break routines with some floor work");
    // (Peter, 2026-11-27: the 16 floor basics and 6 routines; the routines mostly standing as standing break. Six
    // since the spin fixes: break routine 9 · dancer 5, its heading no longer whipping, is no longer bridged over as
    // a glitch and goes upside down long enough to count as flips.)
    CHECK(tiers[kFloorTier] == 22);
    CHECK(tiers[kStanding] == 6);
}

TEST_CASE("floor work: the pelvis quaternion mixes, mirrors and round-trips; standing poses are left alone") {
    const Skeleton sk = makeSkeleton(Look{});
    DancePose a = restPose(sk), b = a;
    // Upside down: half a turn about x, written with w < 0 (the same turn as with w > 0).
    b.hipQx = -1;
    b.hipQw = -1e-3f;
    b.ground = 1;
    const DancePose m = mix(a, b, 0.5f);
    // The short way round: a quarter turn, not through zero.
    const float n = std::sqrt(m.hipQx * m.hipQx + m.hipQw * m.hipQw);
    CHECK(n > 0.6f);
    CHECK(std::fabs(m.hipQx / n) == doctest::Approx(std::sqrt(0.5f)).epsilon(0.01));
    CHECK(m.ground == doctest::Approx(0.5f));
    DancePose c = a;
    c.hipQx = 0.1f;
    c.hipQy = 0.2f;
    c.hipQz = 0.3f;
    c.hipQw = std::sqrt(1 - 0.14f);
    const DancePose mc = mirrored(c);
    CHECK(mc.hipQx == doctest::Approx(0.1f));
    CHECK(mc.hipQy == doctest::Approx(-0.2f));
    CHECK(mc.hipQz == doctest::Approx(-0.3f));
    // Mirroring the pelvis turn mirrors the head: its x negated.
    CHECK(headFrame(sk, mc).o.x == doctest::Approx(-headFrame(sk, c).o.x).epsilon(1e-4));
    CHECK(headFrame(sk, mc).o.y == doctest::Approx(headFrame(sk, c).o.y).epsilon(1e-4));
    float fl[kPoseFloats];
    poseToFloats(c, fl);
    const DancePose r = poseFromFloats(fl);
    CHECK(r.hipQz == doctest::Approx(0.3f));
    CHECK(r.hipQw == doctest::Approx(c.hipQw));
    // A standing pose: groundPose changes nothing.
    DancePose s = restPose(sk);
    s.hip.y = -0.05f;
    s.handL = {0.3f, -0.4f, 0.2f};
    float before[kPoseFloats], after[kPoseFloats];
    poseToFloats(s, before);
    groundPose(sk, s);
    poseToFloats(s, after);
    for (int i = 0; i < kPoseFloats; ++i) CHECK(before[i] == after[i]);
}

TEST_CASE("floor work: groundPose puts an upside-down body on its head, feet in the air going up with it") {
    const Skeleton sk = makeSkeleton(Look{});
    const float leg = sk.thigh + sk.shin;
    // A headstand: the pelvis half a turn about x, the feet up.
    DancePose p = restPose(sk);
    p.hipQx = 1;
    p.hipQw = 0;
    p.footL = {sk.footHalf, 1.2f * leg, 0};
    p.footR = {-sk.footHalf, 1.2f * leg, 0};
    p.ground = 1;
    p.clear = 0;
    const float footBefore = p.footL.y;
    groundPose(sk, p);
    BodyOut o;
    buildBody(Look{}, sk, p, Frame{}, 1.0f, o);
    CHECK(o.headCenter.y < 0.5f);  // head down
    CHECK(std::fabs(o.headCenter.y - sk.headR) < 0.01f);  // on the floor, not through it
    CHECK(lowestPoint(o) > -0.02f);
    CHECK(p.footL.y > footBefore);  // the feet in the air went up with the body
    // Clear of the floor where the move says so (in the air): held at that height.
    DancePose q = restPose(sk);
    q.hipQx = 1;
    q.hipQw = 0;
    q.ground = 1;
    q.clear = 0.02f * leg;
    groundPose(sk, q);
    BodyOut o2;
    buildBody(Look{}, sk, q, Frame{}, 1.0f, o2);
    CHECK(std::fabs(o2.headCenter.y - sk.headR - 0.02f * leg) < 0.01f);
}

TEST_CASE("floor work: every floor move is finite, on the floor where it should be, never through it, continuous") {
    WithFloor f;
    Look look;
    look.hairStyle = HairStyle::Bald;
    // Per tier (0: the standing break moves, for comparison): moves, moves with a fast moment, the fastest.
    int moves[4] = {}, fast[4] = {};
    float fastest[4] = {};
    long touching = 0, touchingOk = 0, samples = 0;
    float deepest = 0;
    for (const Skeleton& sk : skeletons()) {
        const float leg = sk.thigh + sk.shin;
        for (int i = 0; i < clipCount(); ++i) {
            const ClipInfo& c = clipInfo(i);
            if (c.genre != "BR") continue;
            CAPTURE(c.label);
            BodyOut prev;
            float worst = 0;
            for (int k = 0; k <= c.beats * 16; ++k) {  // the loop and its wrap, 16 frames a beat
                DancePose p = restPose(sk);
                clipPose(i, float(k) / 16.0f, sk, p);
                REQUIRE(std::isfinite(p.hipQx + p.hipQy + p.hipQz + p.hipQw + p.clear));
                CHECK(p.ground == (c.floorWork ? 1 : 0));
                const float clear = p.clear;
                groundPose(sk, p);
                REQUIRE(finite(p.hip));
                BodyOut o;
                buildBody(look, sk, p, Frame{}, 1.0f, o);
                REQUIRE(finite(o.headCenter));
                if (c.floorWork) {
                    const float low = lowestPoint(o);
                    deepest = std::min(deepest, low);
                    CHECK(low > -0.03f);  // (the torso's flattened radius counted round: a little conservative)
                    ++samples;
                    if (clear < 0.01f * leg) {  // the dancer's upper body on the floor: ours too
                        ++touching;
                        if (low < 0.03f) ++touchingOk;
                    }
                }
                if (k > 0)
                    worst = std::max({worst, length(o.headCenter - prev.headCenter), length(o.hands[0] - prev.hands[0]),
                                      length(o.hands[1] - prev.hands[1]), length(o.feet[0] - prev.feet[0]),
                                      length(o.feet[1] - prev.feet[1])});
                prev = o;
            }
            // In a 16th of a beat (a body is 1 tall) nothing teleports.
            ++moves[c.tier];
            if (worst > 0.2f) ++fast[c.tier];
            fastest[c.tier] = std::max(fastest[c.tier], worst);
            CHECK(worst < 0.45f);
        }
    }
    MESSAGE(touchingOk << " of " << touching << " samples with the upper body on the floor touch it (of " << samples
                       << "); deepest " << deepest);
    for (int t = 0; t < 4; ++t)
        if (moves[t])
            MESSAGE(std::string(clipTierName(t)) << ": " << fast[t] << " of " << moves[t] << " moves with a hand, foot or the head "
                                    << "moving 0.2 in a 16th of a beat; fastest " << fastest[t]);
    CHECK(touchingOk > touching * 99 / 100);
    // Floor moves about as smooth as the standing break moves; flips are fast (kick-ups): reported only.
    CHECK(fast[kFloorTier] * moves[kStanding] <= 4 * fast[kStanding] * moves[kFloorTier] + moves[kStanding]);
}

TEST_CASE("floor work: a hand the source dancer had on the floor is planted on it, smoothly") {
    // Peter, 2026-11-27: "the hands don't quite touch the floor". Per sample where the source's wrist was on the floor
    // (its clearance under 2 % of a leg): the hand ball's lowest point, without planting (the body only held to its
    // lowest point) and with, per tier. Knees and the head as they come (not moved): reported.
    WithFloor f;
    std::vector<float> before[4], after[4], knees, heads;
    long samples = 0;
    float deepest = 0, popBefore = 0, popAfter = 0;
    for (const Skeleton& sk : skeletons()) {
        const float leg = sk.thigh + sk.shin;
        for (int i = f.standing; i < clipCount(); ++i) {
            const ClipInfo& c = clipInfo(i);
            CAPTURE(c.label);
            float last[2][2] = {};
            for (int k = 0; k <= c.beats * 16; ++k) {
                const float b = float(k) / 16.0f;
                DancePose p = restPose(sk);
                clipPose(i, b, sk, p);
                float con[5];
                REQUIRE(clipContacts(i, b, sk, con));
                DancePose q = p;
                q.plantL = q.plantR = 0;
                groundPose(sk, q);
                groundPose(sk, p);
                ++samples;
                for (int side = 0; side < 2; ++side) {
                    const float hb = handBallBottom(sk, q, side), ha = handBallBottom(sk, p, side);
                    deepest = std::min(deepest, ha);
                    if (con[side] < 0.02f * leg) {
                        CHECK((side == 0 ? p.plantL : p.plantR) > 0.99f);
                        before[c.tier].push_back(hb);
                        after[c.tier].push_back(ha);
                    }
                    // No pops: the hand ball's height from one 16th of a beat to the next.
                    if (k > 0) {
                        popBefore = std::max(popBefore, std::fabs(hb - last[side][0]));
                        popAfter = std::max(popAfter, std::fabs(ha - last[side][1]));
                    }
                    last[side][0] = hb;
                    last[side][1] = ha;
                }
                // Knees (the default look: the shins, parts 1 and 4, start at the knee) and the head.
                if (con[2] < 0.02f * leg || con[3] < 0.02f * leg || con[4] < 0.02f * leg) {
                    BodyOut o;
                    buildBody(Look{}, sk, p, Frame{}, 1.0f, o);
                    for (int side = 0; side < 2; ++side)
                        if (con[2 + side] < 0.02f * leg) {
                            const BodyPart& shin = o.parts[std::size_t(1 + 3 * side)];
                            knees.push_back(shin.frame.o.y - shin.r0);
                        }
                    if (con[4] < 0.02f * leg) heads.push_back(o.headCenter.y - sk.headR);
                }
            }
        }
    }
    auto pct = [](std::vector<float> v, float q) {
        std::sort(v.begin(), v.end());
        return v.empty() ? 0.0f : v[std::size_t(q * float(v.size() - 1))];
    };
    auto within = [](const std::vector<float>& v, float d) {
        long n = 0;
        for (float x : v) n += std::fabs(x) <= d;
        return v.empty() ? 1.0f : float(n) / float(v.size());
    };
    for (int t : {kFloorTier, kStanding}) {
        REQUIRE(!after[t].empty());
        MESSAGE(std::string(t == kStanding ? "standing break with floor work" : clipTierName(t)) << ": " << after[t].size() << " hand samples on the floor; the hand ball's bottom before "
                                << "median " << pct(before[t], 0.5f) << ", 90 % " << pct(before[t], 0.9f) << ", max "
                                << pct(before[t], 1.0f) << ", within 1 cm " << within(before[t], 0.01f) << "; planted median "
                                << pct(after[t], 0.5f) << ", 90 % " << pct(after[t], 0.9f) << ", max " << pct(after[t], 1.0f)
                                << ", within 1 cm " << within(after[t], 0.01f));
    }
    MESSAGE("of " << 2 * samples << " hand samples the deepest " << deepest << "; biggest change of a hand's height in a 16th "
                  << "of a beat " << popBefore << " before, " << popAfter << " planted");
    MESSAGE(knees.size() << " knee samples on the floor: the knee's bottom median " << pct(knees, 0.5f) << ", 90 % "
                         << pct(knees, 0.9f) << "; " << heads.size() << " head samples: the head ball's bottom median "
                         << pct(heads, 0.5f) << ", 90 % " << pct(heads, 0.9f));
    CHECK(within(after[kFloorTier], 0.01f) > 0.98f);  // resting on the floor: not floating, not sunk
    CHECK(within(after[kStanding], 0.01f) > 0.9f);
    CHECK(deepest > -0.005f);
    CHECK(popAfter < popBefore + 0.03f);  // no pops
}

TEST_CASE("floor work: the choreographer picks floor moves only by their weight; watching never") {
    WithFloor f;
    const Skeleton sk = makeSkeleton(Look{});
    auto picks = [&](float floor) {
        setFloorWeights(floor);
        int tiers[4] = {0, 0, 0, 0};
        for (std::uint32_t seed = 1; seed <= 4; ++seed) {
            ChoreoOptions o;
            o.style = Style::Break;
            o.seed = seed;
            o.mocap = 1;
            Choreographer ch(o);
            const Routine* last = nullptr;
            for (double beat = 0; beat < 1600; beat += 0.5) {
                ch.pose(at(beat), sk);
                const Routine* r = ch.routineAt(beat);
                if (r && r != last && r->full >= 0 && moveLibrary()[std::size_t(r->full)].clip >= 0)
                    ++tiers[clipInfo(moveLibrary()[std::size_t(r->full)].clip).tier];
                last = r;
            }
        }
        return std::vector<int>(tiers, tiers + 4);
    };
    const auto off = picks(0);
    CHECK(off[kFloorTier] == 0);
    const auto fw = picks(1);
    CHECK(fw[kFloorTier] > 0);
    const auto half = picks(0.5f);
    CHECK(half[kFloorTier] > 0);
    CHECK(half[kFloorTier] < fw[kFloorTier]);
    const auto def = picks(kDefaultFloorWeight);  // 1: a break move like any other
    MESSAGE("break picks at floor moves 1: " << fw[0] << " standing, " << fw[1] << " floor; at the default "
                                             << kDefaultFloorWeight << ": " << def[0] << " standing, " << def[1] << " floor");
    setFloorWeights(4);
    for (double beat = 0; beat < 16 * 60; beat += 16) {
        const std::string name = watchName(beat + 3, 7, Style::Break, true);
        const int m = findClip(name.substr(0, name.find(" (watching)")));
        REQUIRE(m >= 0);
        CHECK(clipInfo(m).tier == kStanding);
        CHECK(!clipInfo(m).floorWork);
    }
}

TEST_CASE("floor work: break dancers mix floor moves in by default, as ordinary break moves, whatever the music") {
    // Peter, 2026-11-27: break dancers (generated and guests) mix floor moves in by themselves, "into the normal break
    // cycle without special distribution weighting": the same share in every section of the music. Per music section held for a long time: the share of the
    // mocap picks (a basic move or routine starting) that are floor moves.
    WithFloor f;
    setFloorWeights(kDefaultFloorWeight);
    const Skeleton sk = makeSkeleton(Look{});
    struct Section {
        const char* name;
        double build, drop;
    };
    const Section sections[] = {{"groove", 0, 0}, {"breakdown", 0.3, 0}, {"build", 0.8, 0}, {"drop", 0, 1}};
    float share[4] = {};
    int floorRoutinesInARow = 0, routinesSeen = 0;
    for (int si = 0; si < 4; ++si) {
        int floor = 0, all = 0;
        for (std::uint32_t seed = 1; seed <= 6; ++seed) {
            ChoreoOptions o;
            o.style = Style::Break;
            o.seed = seed;
            o.mocap = 1;
            Choreographer ch(o);
            const Routine* last = nullptr;
            bool lastFloorRoutine = false;
            for (double beat = 0; beat < 2400; beat += 0.5) {
                MusicState g = at(beat);
                g.build = sections[si].build;
                // A drop: the decaying "drop" right after one (a hit every 64 beats, the picks in between see it).
                g.drop = sections[si].drop > 0 ? std::max(0.0, 1.0 - std::fmod(beat, 64.0) / 48.0) : 0.0;
                ch.pose(g, sk);
                const Routine* r = ch.routineAt(beat);
                if (r && r != last && r->full >= 0 && moveLibrary()[std::size_t(r->full)].clip >= 0) {
                    const MoveDef& m = moveLibrary()[std::size_t(r->full)];
                    const bool fl = clipInfo(m.clip).tier == kFloorTier;
                    ++all;
                    floor += fl;
                    if (m.beats >= 32) {
                        ++routinesSeen;
                        if (fl && lastFloorRoutine) ++floorRoutinesInARow;
                        lastFloorRoutine = fl;
                    } else {
                        lastFloorRoutine = false;  // (a basic move between two is fine)
                    }
                }
                last = r;
            }
        }
        share[si] = float(floor) / float(std::max(1, all));
        MESSAGE(std::string(sections[si].name) << ": " << floor << " floor moves of " << all << " picks (" << int(share[si] * 100 + 0.5f)
                                  << " %)");
    }
    // Floor moves are 22 of break's 56 moves, by count about 40 %; energy, tempo and basics-vs-routines spread it.
    CHECK(share[0] > 0.2f);
    CHECK(share[0] < 0.6f);
    for (int si = 1; si < 4; ++si) CHECK(std::fabs(share[si] - share[0]) < 0.15f);  // no say of the music
    CHECK(routinesSeen > 50);
    MESSAGE("floor routines straight after a floor routine: " << floorRoutinesInARow << " of " << routinesSeen);
    setFloorWeights(0);
    // (Other styles never: floor moves are break's.)
    ChoreoOptions o;
    o.style = Style::House;
    o.mocap = 1;
    Choreographer ch(o);
    for (double beat = 0; beat < 1200; beat += 0.5) {
        ch.pose(at(beat), sk);
        const Routine* r = ch.routineAt(beat);
        if (r && r->full >= 0 && moveLibrary()[std::size_t(r->full)].clip >= 0)
            CHECK(!clipInfo(moveLibrary()[std::size_t(r->full)].clip).floorWork);
    }
}

TEST_CASE("bust a move: two whole break routines with floor work from the next 8-bar point, then the own style") {
    // Peter, 2026-11-27 (a stream guest's "Bust a move"): any dancer, any style.
    WithFloor f;
    const Skeleton sk = makeSkeleton(Look{});
    auto breakRoutine = [](const Routine* r) {
        if (!r || r->full < 0) return false;
        const MoveDef& m = moveLibrary()[std::size_t(r->full)];
        return m.clip >= 0 && clipInfo(m.clip).floorWork && clipInfo(m.clip).genre == "BR" && m.beats == 64;
    };
    for (Style style : {Style::House, Style::Ballet, Style::Break}) {
        for (double ask : {37.0, 61.5, 96.0})
          for (double skew : {0.0, 0.13}) {  // (a dancer's own beat runs a little off the music's and drifts: humanBeat)
            CAPTURE(int(style));
            CAPTURE(ask);
            CAPTURE(skew);
            auto at = [&](double b) {
                MusicState g = ::at(b);
                g.beat += skew + (skew != 0 ? 0.05 * std::sin(b * 0.31) : 0.0);
                return g;
            };
            ChoreoOptions o;
            o.style = style;
            o.seed = 5;
            o.mocap = 1;
            Choreographer ch(o);
            double beat = 0;
            for (; beat < ask; beat += 0.25) ch.pose(at(beat), sk);
            ch.bustMove();
            CHECK(ch.bustMoveAt(beat + skew));  // asked: pending
            ch.pose(at(beat), sk);
            // From the next 8-bar point (at least a beat on), 32 bars.
            const double start = ch.bustMoveStart() - skew;
            CHECK(std::fabs(std::remainder(start, 32.0)) < 0.06);
            CHECK(start > beat + 0.9);
            CHECK(start <= beat + 33);
            CHECK(std::fabs(ch.bustMoveEnd() - skew - (start + 128)) < 1e-6);
            CHECK(!ch.bustMoveAt(start + skew - 0.5, false));
            Routine first, second;
            first.full = second.full = -1;
            for (; beat < start + 140; beat += 0.25) {
                ch.pose(at(beat), sk);
                const Routine* r = ch.routineAt(beat + skew);
                if (beat >= start + 0.5 && beat < start + 64) {
                    CHECK(breakRoutine(r));
                    if (r) first = *r;
                }
                if (beat >= start + 64.5 && beat < start + 128) {
                    CHECK(breakRoutine(r));
                    if (r) second = *r;
                }
                if (beat >= start + 128.5) CHECK(!ch.bustMoveAt(beat + skew, false));
            }
            REQUIRE(first.full >= 0);
            REQUIRE(second.full >= 0);
            CHECK(first.full != second.full);  // two different ones
            CHECK(std::fabs(first.start - skew - start) < 0.1);
            CHECK(std::fabs(second.start - skew - (start + 64)) < 0.1);
            // Afterwards the own style again (not a break routine, unless a break dancer picks one).
            if (style != Style::Break) {
                for (; beat < start + 400; beat += 0.5) {
                    ch.pose(at(beat), sk);
                    const Routine* r = ch.routineAt(beat + skew);
                    if (r && r->full >= 0 && moveLibrary()[std::size_t(r->full)].clip >= 0)
                        CHECK((moveLibrary()[std::size_t(r->full)].styles & styleBit(style)) != 0);
                }
            }
        }
    }
}

TEST_CASE("floor moves wait for room, never give up: the chosen move starts on its next 4- or 8-bar point with room") {
    // Peter, 2026-11-27 ("let them do the move they want to do, waiting if necessary, ... still reasonably on phrase";
    // docs/dancers-design.md "Floor moves held still, room to go down").
    WithFloor f;
    setFloorWeights(2);
    const Skeleton sk = makeSkeleton(Look{});
    auto floorMove = [](const Routine* r) {
        return r && r->full >= 0 && moveLibrary()[std::size_t(r->full)].clip >= 0 &&
               clipInfo(moveLibrary()[std::size_t(r->full)].clip).floorWork;
    };
    // Every floor move knows its reach (the room cleared for it): a body's width or more, at most about a body.
    for (int i = 0; i < clipCount(); ++i) {
        if (!clipInfo(i).floorWork) continue;
        const FloorReach& r = clipInfo(i).reach;
        CHECK(r.max > 0.35f);
        CHECK(r.max < 1.2f);
        for (float d : r.dir) CHECK(d <= r.max + 1e-4f);
    }
    CHECK(bustMoveReach().max > 0.5f);
    // Peter, 2026-11-27: never give up: the chosen move (as chosen) waits for room, however long, and starts on its grid.
    int started = 0, waited = 0, offGrid = 0, sameMove = 0, changed = 0;
    for (std::uint32_t seed = 1; seed <= 6; ++seed) {
        ChoreoOptions o;
        o.style = Style::Break;
        o.seed = seed;
        o.mocap = 1;
        Choreographer ch(o);
        // No room for 48 beats: no floor move starts; then room every other 32 beats.
        int pending = -1;
        for (double beat = 0; beat < 1200; beat += 0.25) {
            const bool room = beat >= 48 && std::fmod(beat, 64.0) >= 32;
            ch.setFloorRoom(room ? 10.0f : 0.0f);
            ch.pose(at(beat), sk);
            const Routine* r = ch.routineAt(beat);
            if (beat < 48 && floorMove(r) && r->start > 4) FAIL("a floor move started without room at beat " << beat);
            if (floorMove(r) && std::fabs(r->start - beat) < 0.125 && r->start > 4) {
                ++started;
                if (pending >= 0) {
                    ++waited;
                    sameMove += r->full == pending;  // the move it chose
                    const double grid = moveLibrary()[std::size_t(r->full)].beats >= 32 ? 32 : 16;
                    offGrid += std::fabs(std::remainder(r->start, grid)) > 1e-6;
                }
                pending = -1;
            }
            if (ch.floorPending() >= 0) {
                if (pending >= 0 && ch.floorPending() != pending) ++changed;  // (never: no giving up)
                pending = ch.floorPending();
            }
        }
        CHECK(ch.floorCounts().waited > 0);
        CHECK(ch.floorCounts().forced == 0);
    }
    MESSAGE("break dancers with room half the time: " << started << " floor moves started, " << waited
                                                      << " after waiting, all as chosen: " << sameMove);
    CHECK(waited > 5);
    CHECK(offGrid == 0);
    CHECK(changed == 0);
    CHECK(sameMove == waited);
    // Never room: the chosen move still starts, on its grid, after kFloorLastResort points (the last resort).
    {
        ChoreoOptions o;
        o.style = Style::Break;
        o.seed = 9;
        o.mocap = 1;
        Choreographer ch(o);
        int last = -1, changes = 0, offGridForced = 0;
        for (double beat = 0; beat < 2000; beat += 0.25) {
            ch.setFloorRoom(-1);
            ch.pose(at(beat), sk);
            if (ch.floorPending() >= 0 && last >= 0 && ch.floorPending() != last) ++changes;
            if (ch.floorPending() >= 0) last = ch.floorPending();
            const Routine* r = ch.routineAt(beat);
            if (floorMove(r) && std::fabs(r->start - beat) < 0.125 && r->start > 4) {
                CHECK(r->full == last);
                const double grid = moveLibrary()[std::size_t(r->full)].beats >= 32 ? 32 : 16;
                offGridForced += std::fabs(std::remainder(r->start, grid)) > 1e-6;
                last = -1;
            }
        }
        CHECK(ch.floorCounts().forced > 3);
        CHECK(ch.floorCounts().delays[7] == 0);
        CHECK(ch.floorCounts().delays[Choreographer::kFloorLastResort] == ch.floorCounts().forced);
        CHECK(changes == 0);
        CHECK(offGridForced == 0);
    }
    // A bust waits for room too (its 8-bar points), and starts anyway after kFloorLastResort of them.
    {
        ChoreoOptions o;
        o.style = Style::House;
        o.seed = 3;
        o.mocap = 1;
        Choreographer ch(o);
        double beat = 0;
        for (; beat < 40; beat += 0.25) {
            ch.setFloorRoom(0);
            ch.pose(at(beat), sk);
        }
        using Phase = Choreographer::BustPhase;
        double left = 0;
        CHECK(ch.bustPhase(beat, &left) == Phase::None);
        ch.bustMove();
        // Where it stands (the guest page, docs/guests.md "bust"): asked, then waiting for its point, then put off
        // for room, then on.
        CHECK(ch.bustPhase(beat, &left) == Phase::Waiting);
        CHECK(left == -1);
        ch.pose(at(beat), sk);
        const double asked = ch.bustMoveStart();
        CHECK(ch.bustPhase(beat, &left) == Phase::Waiting);
        CHECK(std::fabs(left - (asked - beat)) < 1e-9);
        bool sawRoom = false;
        for (; beat < asked + 32 * (Choreographer::kFloorLastResort + 1) + 8; beat += 0.25) {
            ch.setFloorRoom(0);
            ch.pose(at(beat), sk);
            if (beat > asked + 1 && ch.bustPhase(beat, &left) == Phase::Room) {
                sawRoom = true;
                CHECK(std::fabs(left - (ch.bustMoveStart() - beat)) < 1e-9);
            }
        }
        CHECK(sawRoom);
        CHECK(ch.floorCounts().bustDelays == Choreographer::kFloorLastResort);
        CHECK(std::fabs(ch.bustMoveStart() - asked - 32 * Choreographer::kFloorLastResort) < 1e-6);
        CHECK(floorMove(ch.routineAt(beat)));
        CHECK(ch.bustMoveAt(beat, false));
        CHECK(ch.bustPhase(beat, &left) == Phase::On);
        CHECK(std::fabs(left - (ch.bustMoveEnd() - beat)) < 1e-9);
        ch.bustMove();  // asked again while on: still on
        CHECK(ch.bustPhase(beat) == Phase::On);
        CHECK(ch.bustPhase(ch.bustMoveEnd() + 1) != Phase::On);
    }
    // No stage (infinite room, the default): exactly as before.
    {
        ChoreoOptions o;
        o.style = Style::Break;
        o.seed = 4;
        o.mocap = 1;
        Choreographer a(o), b(o);
        b.setFloorRoom(1e30f);
        for (double beat = 0; beat < 800; beat += 0.25) {
            a.pose(at(beat), sk);
            b.pose(at(beat), sk);
            const Routine* ra = a.routineAt(beat);
            const Routine* rb = b.routineAt(beat);
            REQUIRE((ra && rb));
            CHECK(ra->full == rb->full);
            CHECK(a.floorPending() == -1);
        }
    }
}
