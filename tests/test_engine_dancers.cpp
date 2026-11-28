#include <map>
// Dancers (PLAN 5P, docs/dancers-design.md): rig, IK, body parts, moves, styles, choreographer.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>
#include <set>
#include <string>

#include "engine/DanceMoves.hpp"
#include "engine/DancerRig.hpp"

using namespace reforce::engine::dance;

namespace {

bool finite(V3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

void checkOrthonormal(const Frame& f) {
    CHECK(near(length(f.x), 1));
    CHECK(near(length(f.y), 1));
    CHECK(near(length(f.z), 1));
    CHECK(near(dot(f.x, f.y), 0));
    CHECK(near(dot(f.y, f.z), 0));
    CHECK(near(dot(cross(f.x, f.y), f.z), 1));  // right-handed
}

MusicState grooveAt(double beat) {
    MusicState g;
    g.beat = beat;
    g.bar = beat / 4;
    g.phrase = beat / 64;
    return g;
}

}  // namespace

TEST_CASE("dancers: colors and names parse") {
    auto c = parseRgb("#ff8000");
    REQUIRE(c);
    CHECK(near(c->r, 1));
    CHECK(near(c->g, 128.0f / 255));
    CHECK(near(c->b, 0));
    CHECK(parseRgb("\"#0f0\"")->g == doctest::Approx(1));
    CHECK_FALSE(parseRgb("#12345"));
    CHECK_FALSE(parseRgb("zzzzzz"));
    CHECK(parseHairStyle("Afro") == HairStyle::Afro);
    CHECK(parseHairStyle("mullet") == HairStyle::Mullet);
    CHECK(parseHairStyle("High-Top") == HairStyle::Coils);  // cut 2026-11-21
    CHECK(parseHairStyle("quiff") == HairStyle::Quiff);
    CHECK(parseHairStyle("crew") == HairStyle::Short);  // merged 2026-11-21
    CHECK(parseHairStyle("waves") == HairStyle::Waves);
    CHECK(parseHairStyle("wavy") == HairStyle::Long);  // cut 2026-11-21: the closest left
    CHECK(parseHairStyle("messy") == HairStyle::Anime);
    CHECK(parseHairStyle("shaved") == HairStyle::Shaved);
    CHECK(parseHairStyle("half-up") == HairStyle::HalfUp);
    CHECK_FALSE(parseHairStyle("beehive"));
    CHECK(parseOutfit("t-shirt") == Outfit::Tee);
    CHECK(parseHat("TopHat") == Hat::Top);
    CHECK(parseStyle("Hip-Hop") == Style::HipHop);
    CHECK(parseStyle("shuffle") == Style::House);
    CHECK(parseStyle("krump") == Style::Stomp);
    CHECK(parseStyle("Waacking") == Style::Disco);
    CHECK(parseStyle("popping") == Style::Pop);
    CHECK(parseStyle("b-boy") == Style::Break);
    // The styles before 2026-11-21 (old scene files, settings and stream guests).
    CHECK(parseStyle("robot") == Style::Pop);
    CHECK(parseStyle("rave") == Style::House);
    CHECK(parseStyle("retro") == Style::Lock);
    CHECK(parseStyle("chill") == Style::Jazz);
    CHECK(parseStyle("street jazz") == Style::Jazz);
    CHECK(parseStyle("Ballet Jazz") == Style::Ballet);
    for (int i = 0; i < kStyleCount; ++i) CHECK(parseStyle(styleName(Style(i))) == Style(i));
    CHECK(std::string(styleLabel(Style::HipHop)) == "hip-hop");
    CHECK_FALSE(parseStyle("polka"));
}

TEST_CASE("dancers: frame rotations stay orthonormal and turn the documented way") {
    Frame f = Frame{}.rotatedY(0.7f).rotatedZ(-0.3f).rotatedX(1.1f);
    checkOrthonormal(f);
    // +yaw turns +z towards +x (the character turns to its left).
    const Frame y = Frame{}.rotatedY(1.5707963f);
    CHECK(near(y.z.x, 1));
    // +pitch tips +y towards +z (forward lean).
    const Frame x = Frame{}.rotatedX(0.3f);
    CHECK(x.y.z > 0);
    // +roll tips +y towards -x (lean to the character's right).
    const Frame z = Frame{}.rotatedZ(0.3f);
    CHECK(z.y.x < 0);
    checkOrthonormal(frameAlong({1, 2, 3}, {0.2f, -1, 0.1f}, {1, 0, 0}));
    checkOrthonormal(frameAlong({0, 0, 0}, {1, 0, 0}, {1, 0, 0}));  // hint along the axis
}

TEST_CASE("dancers: two-bone IK keeps bone lengths, reaches, clamps and bends towards the pole") {
    const V3 root{0, 1, 0};
    const float a = 0.3f, b = 0.25f;
    auto r = solveTwoBone(root, {0.2f, 0.7f, 0.1f}, a, b, {0, 0, 1});
    CHECK(r.reached);
    CHECK(near(length(r.mid - root), a, 1e-4f));
    CHECK(near(length(r.end - r.mid), b, 1e-4f));
    CHECK(near(length(r.end - V3{0.2f, 0.7f, 0.1f}), 0, 1e-4f));
    CHECK(r.mid.z > 0);  // towards the pole
    // Out of reach: pulled in along the direction, nearly straight.
    auto far = solveTwoBone(root, {0, -1, 0}, a, b, {0, 0, 1});
    CHECK_FALSE(far.reached);
    CHECK(near(length(far.end - root), 0.995f * (a + b), 1e-4f));
    CHECK(far.end.x == doctest::Approx(0).epsilon(1e-5));
    // Pole along the reach direction: still finite.
    auto deg = solveTwoBone(root, {0, 0.6f, 0}, a, b, {0, -1, 0});
    CHECK(finite(deg.mid));
}

TEST_CASE("dancers: rest pose stands on the floor, about one unit tall, facing the camera") {
    for (int hsi = 0; hsi < kHairStyleCount; ++hsi) {
        const HairStyle hs = HairStyle(hsi);
        Look look;
        look.hairStyle = hs;
        const Skeleton sk = makeSkeleton(look);
        BodyOut out;
        buildBody(look, sk, restPose(sk), Frame{}, 1.0f, out);
        REQUIRE(out.parts.size() > 15);
        float lo = 1e9f, hi = -1e9f;
        for (const auto& p : out.parts) {
            CHECK(finite(p.frame.o));
            checkOrthonormal(p.frame);
            const V3 e0 = p.frame.o, e1 = p.frame.point({0, p.len, 0});
            const float r = std::max(p.r0, p.r1) * std::max(1.0f, p.frame.y.y > 0.9f ? 1.0f : std::max(p.scaleX, p.scaleZ));
            lo = std::min(lo, std::min(e0.y - p.r0, e1.y - p.r1));
            hi = std::max(hi, std::max(e0.y, e1.y) + r);
        }
        CHECK(lo == doctest::Approx(0).epsilon(0.03));  // the soles touch the floor
        CHECK(hi > 0.9f);
        CHECK(hi < 1.35f);
        CHECK(out.feet[0].x > 0);  // the left foot on the character's left (+x)
        CHECK(out.headCenter.y > 0.7f);
    }
}

TEST_CASE("dancers: mirroring twice is the identity and swaps sides") {
    const Skeleton sk = makeSkeleton(Look{});
    DancePose p = restPose(sk);
    p.handL = {0.3f, 0.2f, 0.1f};
    p.hipYaw = 0.4f;
    p.footYawR = 0.2f;
    const DancePose m = mirrored(p);
    CHECK(m.handR.x == doctest::Approx(-0.3f));
    CHECK(m.hipYaw == doctest::Approx(-0.4f));
    CHECK(m.footYawL == doctest::Approx(0.2f));
    const DancePose mm = mirrored(m);
    CHECK(mm.handL.x == doctest::Approx(p.handL.x));
    CHECK(mm.hipYaw == doctest::Approx(p.hipYaw));
}

TEST_CASE("dancers: every move stays finite, reachable and above the floor, for every variant") {
    const Skeleton sk = makeSkeleton(Look{});
    std::set<std::string> names;
    for (const auto& m : moveLibrary()) {
        if (m.clip >= 0) continue;  // mocap moves: test_engine_danceclips
        CAPTURE(m.name);
        CHECK(names.insert(m.name).second);  // unique names
        CHECK(m.beats >= 1);
        for (int variant = 0; variant < 4; ++variant) {
            MoveCtx c;
            c.variant = variant;
            c.amp = 1.3f;
            c.sharp = 2;
            c.shoulder = sk.shoulderHalf;
            c.foot = sk.footHalf;
            for (int i = 0; i < 64; ++i) {
                const float b = float(m.beats) * float(i) / 64.0f;
                DancePose p = restPose(sk);
                m.fn(b, c, p);
                CHECK(finite(p.hip));
                CHECK(finite(p.handL));
                CHECK(finite(p.handR));
                CHECK(finite(p.footL));
                CHECK(finite(p.footR));
                CHECK(std::isfinite(p.bodyYaw));
                // Feet never go through the floor; hands stay near arm's reach of the shoulders.
                CHECK(p.footL.y >= -1e-4f);
                CHECK(p.footR.y >= -1e-4f);
                const float reach = sk.upperArm + sk.forearm;
                CHECK(length(p.handL - V3{sk.shoulderHalf, sk.shoulderY, 0}) < reach * 1.6f);
                CHECK(length(p.handR - V3{-sk.shoulderHalf, sk.shoulderY, 0}) < reach * 1.6f);
                BodyOut out;
                buildBody(Look{}, sk, p, Frame{}, 1.0f, out);
                for (const auto& part : out.parts) REQUIRE(finite(part.frame.o));
            }
        }
    }
    CHECK(moveLibrary().size() >= 55);
}

TEST_CASE("dancers: each style has a wide variety of routines") {
    for (int s = 0; s < kStyleCount; ++s) {
        CAPTURE(styleName(Style(s)));
        CHECK(routineCount(Style(s)) >= 30);
    }
}

TEST_CASE("dancers: the groove drops on the beat") {
    const Skeleton sk = makeSkeleton(Look{});
    const MoveDef& m = moveLibrary()[std::size_t(findMove("groove"))];
    MoveCtx c;
    DancePose on = restPose(sk), off = restPose(sk);
    m.fn(1.0f, c, on);
    m.fn(1.5f, c, off);
    CHECK(on.hip.y < off.hip.y - 0.02f);
}

TEST_CASE("dancers: the choreographer is deterministic, varied and changes on bar lines") {
    const Skeleton sk = makeSkeleton(Look{});
    for (int s = 0; s < kStyleCount; ++s) {
        ChoreoOptions o;
        o.style = Style(s);
        o.seed = 42;
        Choreographer a(o), b(o);
        std::set<std::string> seen;
        double lastStart = -1;
        for (int i = 0; i < 60 * 240; ++i) {  // 4 minutes at 120 BPM, 60 steps a second
            const double beat = double(i) / 30.0;
            const DancePose pa = a.pose(grooveAt(beat), sk);
            const DancePose pb = b.pose(grooveAt(beat), sk);
            REQUIRE(pa.handL.x == pb.handL.x);
            REQUIRE(pa.hip.y == pb.hip.y);
            const Routine* r = a.routineAt(beat);
            REQUIRE(r);
            if (r->start != lastStart) {
                lastStart = r->start;
                if (beat > 1) CHECK(std::fmod(r->start, 4.0) == doctest::Approx(0));  // bar lines
                seen.insert(a.describe(beat));
            }
        }
        CAPTURE(styleName(Style(s)));
        CHECK(seen.size() >= 12);  // many different routines in four minutes
    }
    ChoreoOptions o1, o2;
    o1.seed = 1;
    o2.seed = 2;
    Choreographer c1(o1), c2(o2);
    int differ = 0;
    for (int i = 0; i < 2000; ++i) {
        const double beat = i * 0.05;
        if (c1.pose(grooveAt(beat), sk).handL.y != c2.pose(grooveAt(beat), sk).handL.y) ++differ;
    }
    CHECK(differ > 1000);  // different seeds dance differently
}

TEST_CASE("dancers: a drop fires a jump; a delayed pose is a lagged copy") {
    const Skeleton sk = makeSkeleton(Look{});
    ChoreoOptions o;
    o.style = Style::House;
    Choreographer c(o);
    for (int i = 0; i < 300; ++i) c.pose(grooveAt(i * 0.05), sk);
    MusicState g = grooveAt(16.0);
    g.drop = 1;
    c.pose(g, sk);
    const Routine* r = c.routineAt(16.0);
    REQUIRE(r);
    CHECK(r->full == findMove("jump"));

    Choreographer lead(o);
    DancePose lagged{};
    for (int i = 0; i <= 2000; ++i) {
        const double beat = i * 0.05;
        const DancePose p = lead.pose(grooveAt(beat), sk, 1.0);
        if (i == 2000) lagged = p;
    }
    Choreographer ref(o);
    DancePose direct{};
    for (int i = 0; i <= 1980; ++i) direct = ref.pose(grooveAt(i * 0.05), sk);
    CHECK(lagged.handL.x == doctest::Approx(direct.handL.x).epsilon(1e-4));
    CHECK(lagged.hip.y == doctest::Approx(direct.hip.y).epsilon(1e-4));
}

#include <filesystem>

#include "engine/Dancers.hpp"
#include "scene/SceneConfig.hpp"
#include "scene/SceneFile.hpp"

TEST_CASE("dancers: every content character loads without warnings") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::path(REFORCE_SOURCE_DIR) / "content/Packages/reforce.Dancer.package";
    int n = 0, randoms = 0;
    std::set<int> styles;
    for (const auto& e : fs::directory_iterator(dir)) {
        if (e.path().extension() != ".txt") continue;
        const auto file = reforce::scene::loadSceneFile(e.path());
        REQUIRE(file);
        const auto cfg = reforce::scene::compileConfig(*file, reforce::scene::SceneType::Dancer);
        const auto spec = reforce::engine::dancerSpec(cfg, e.path().stem().string());
        CAPTURE(e.path().filename().string());
        for (const auto& w : spec.warnings) MESSAGE(w);
        CHECK(spec.warnings.empty());
        CHECK(cfg.diags.items().empty());
        styles.insert(int(spec.style));
        if (spec.random) ++randoms;
        ++n;
    }
    CHECK(n == kStyleCount);                           // no named characters since 2026-11-21 (Peter)
    CHECK(styles.size() == std::size_t(kStyleCount));  // every style has a generator ("Random <Style>")
    CHECK(randoms == kStyleCount);
}

TEST_CASE("dancers: Name sets the name tag (a stream guest)") {
    const auto file = reforce::scene::parseSceneText("Vers=100\nStyle=\"disco\"\nName=\"Sam O'Neil\"\n");
    const auto cfg = reforce::scene::compileConfig(file, reforce::scene::SceneType::Dancer);
    const auto spec = reforce::engine::dancerSpec(cfg, "sam");
    CHECK(spec.warnings.empty());
    CHECK(spec.tag == "Sam O'Neil");
    CHECK(spec.name == "sam");
    const auto plain = reforce::scene::parseSceneText("Vers=100\nStyle=\"pop\"\n");
    CHECK(reforce::engine::dancerSpec(reforce::scene::compileConfig(plain, reforce::scene::SceneType::Dancer), "x")
              .tag.empty());
}

TEST_CASE("dancers: stage camera puts the feet near the bottom and the heads below the middle") {
    const auto cam = reforce::engine::stageCamera(16.0 / 9.0, 0.42);
    const V3 feet = reforce::engine::projectStage(cam, {0, 0, 0});
    const V3 head = reforce::engine::projectStage(cam, {0, 1, 0});
    CHECK(feet.y == doctest::Approx(0.92).epsilon(0.02));
    CHECK(feet.y - head.y == doctest::Approx(0.42).epsilon(0.05));
    const auto slots = reforce::engine::stageSlots(5, cam);
    REQUIRE(slots.size() == 5);
    for (const auto& s : slots) {
        const V3 p = reforce::engine::projectStage(cam, {s.first, 0, s.second});
        CHECK(p.x > 0.05f);
        CHECK(p.x < 0.95f);
    }
}

TEST_CASE("dancers: a run says hello, dances, and leaves after its hold") {
    using namespace reforce::engine;
    DancerRun run(generateDancer(Style::Disco, std::uint32_t(0)), 7, 0.0, 0.0, 10.0);
    run.setSlot(0, 0);
    double t = 0;
    for (; t < 30 && !run.expired(t); t += 1.0 / 60) {
        MusicState m = grooveAt(t * 2);
        run.step(t, 1.0 / 60, m, 1.0f);
        BodyOut out;
        run.build(out);
        for (const auto& p : out.parts) REQUIRE(finite(p.frame.o));
    }
    CHECK(run.leaving());
    CHECK(t > 10.0);
    CHECK(t < 13.0);  // the goodbye takes a couple of seconds
}

TEST_CASE("dancers: a directed dancer turns smoothly and blends into reacting") {
    using namespace reforce::engine;
    DancerRun run(generateDancer(Style::HipHop, std::uint32_t(1)), 3, 0.0, 0.0, 0.0);
    run.setSlot(0, 0);
    DanceDirection dir;
    dir.facing = 1.0f;
    float prev = 0;
    double t = 0;
    for (int i = 0; i < 600; ++i, t += 1.0 / 60) {
        MusicState m = grooveAt(t * 2);
        dir.react = i >= 300;
        run.step(t, 1.0 / 60, m, 1.0f, dir);
        if (t > 2) {
            CHECK(std::fabs(run.facing() - prev) < 0.1f);  // no snapping
        }
        prev = run.facing();
    }
    CHECK(run.facing() == doctest::Approx(1.0f).epsilon(0.02));
    CHECK(run.describe(t * 2).find("watching") != std::string::npos);
}

TEST_CASE("dancers: every move is continuous, also where its cycle wraps") {
    const Skeleton sk = makeSkeleton(Look{});
    for (const auto& m : moveLibrary()) {
        if (m.clip >= 0) continue;  // mocap moves: test_engine_danceclips
        CAPTURE(m.name);
        for (int v = 0; v < 4; ++v) {
            MoveCtx c;
            c.variant = v;
            c.shoulder = sk.shoulderHalf;
            c.foot = sk.footHalf;
            DancePose prev;
            bool have = false;
            float worst = 0;
            for (int i = 0; i <= m.beats * 2 * 30; ++i) {  // two cycles, 30 frames a beat (60 fps at 120 BPM)
                const float b = std::fmod(float(i) / 30.0f, float(m.beats));
                DancePose p = restPose(sk);
                m.fn(b, c, p);
                if (have) {
                    worst = std::max({worst, length(p.handL - prev.handL), length(p.handR - prev.handR),
                                      length(p.hip - prev.hip) * 2, length(p.footL - prev.footL),
                                      length(p.footR - prev.footR),
                                      std::fabs(std::remainder(p.bodyYaw - prev.bodyYaw, 6.2831853f)) * 0.3f});
                }
                prev = p;
                have = true;
            }
            CHECK(worst < 0.14f);  // the robot's snaps are the fastest (0.13)
        }
    }
}

TEST_CASE("dancers: routine changes are about as smooth as the moves themselves") {
    const Skeleton sk = makeSkeleton(Look{});
    std::vector<float> atChange, inside;
    for (int s = 0; s < kStyleCount; ++s)
        for (std::uint32_t seed = 1; seed <= 3; ++seed) {
            ChoreoOptions o;
            o.style = Style(s);
            o.seed = seed;
            Choreographer c(o);
            DancePose p1, p2;
            int have = 0;
            for (int i = 0; i < 60 * 120; ++i) {
                const double beat = i / 30.0;
                const DancePose p = c.pose(grooveAt(beat), sk);
                const Routine* r = c.routineAt(beat);
                const bool nearChange = r && r->start > 0 && std::fabs(beat - r->start) < 0.75;
                if (have >= 2) {
                    auto acc = [](V3 a, V3 b, V3 d) { return length(a - b * 2.0f + d); };
                    const float a = std::max({acc(p.handL, p1.handL, p2.handL), acc(p.handR, p1.handR, p2.handR),
                                              acc(p.footL, p1.footL, p2.footL), acc(p.footR, p1.footR, p2.footR)});
                    (nearChange ? atChange : inside).push_back(a);
                }
                p2 = p1;
                p1 = p;
                ++have;
            }
        }
    auto median = [](std::vector<float> v) {
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    };
    MESSAGE("acceleration median: at changes " << median(atChange) << ", inside " << median(inside));
    CHECK(median(atChange) < 1.5f * median(inside));
}

TEST_CASE("dancers: walking plants the feet and steps on the 8ths") {
    using namespace reforce::engine;
    DancerRun run(generateDancer(Style::Pop, std::uint32_t(0)), 11, 0.0, 0.0, 0.0);
    run.setSlot(-1.0f, 0);
    double t = 0;
    for (; t < 2; t += 1.0 / 60) run.step(t, 1.0 / 60, grooveAt(t * 2), 1.0f);  // arrive, settle
    run.setSlot(1.0f, -0.3f);  // walk over
    std::pair<float, float> last[2];
    bool lastSwing[2] = {true, true};
    int plantedFrames = 0, slidFrames = 0;
    const int steps0 = run.stepsTaken();
    for (int i = 0; i < 240; ++i, t += 1.0 / 60) {
        run.step(t, 1.0 / 60, grooveAt(t * 2), 1.0f);
        reforce::engine::dance::BodyOut out;
        run.build(out);
        for (int f = 0; f < 2; ++f) {
            const auto spot = run.footSpot(f);
            if (run.walking() > 0.95f && !run.footSwinging(f) && !lastSwing[f]) {
                ++plantedFrames;
                const float moved = std::hypot(spot.first - last[f].first, spot.second - last[f].second);
                if (moved > 0.002f) ++slidFrames;
                // The drawn ankle stands on the planted spot.
                CHECK(std::hypot(out.feet[f].x - spot.first, out.feet[f].z - spot.second) < 0.05f);
            }
            last[f] = spot;
            lastSwing[f] = run.footSwinging(f);
        }
    }
    CHECK(run.stepsTaken() - steps0 >= 6);  // ~2 m at walking pace: several steps
    CHECK(plantedFrames > 100);
    CHECK(slidFrames == 0);  // a planted foot does not slide
}

TEST_CASE("dancers: humanising makes a crew doing the same routine differ a little, smoothly; Human 0 is exact") {
    using namespace reforce::engine;
    auto crew = [](float human, float* meanDiff, float* maxJump, float* maxBeatOff) {
        Choreographer leader([] {
            ChoreoOptions o;
            o.style = Style::Pop;
            o.seed = 5;
            return o;
        }());
        DancerRun a(generateDancer(Style::Pop, std::uint32_t(0)), 21, 0.0, 0.0, 0.0), b(generateDancer(Style::Pop, std::uint32_t(0)), 22, 0.0, 0.0, 0.0);
        a.setHuman(human);
        b.setHuman(human);
        // Hair motion off: this look (a long curly mane) hangs its curls on the hair chain, whose contacts turn the
        // bodies' float-rounding difference (~1e-7) into ~1e-4, which is invisible but not "identical".
        a.setHairMotion(0);
        b.setHairMotion(0);
        a.setSlot(0, 0);
        b.setSlot(0, 0);
        DanceDirection dir;
        dir.leader = &leader;
        dir.facing = 0;
        double sum = 0;
        int n = 0;
        V3 prev{};
        *maxJump = 0;
        *maxBeatOff = 0;
        double lastBeat = -1e9;
        for (int i = 0; i < 60 * 40; ++i) {
            const double t = i / 60.0;
            const MusicState m = grooveAt(t * 2);
            a.step(t, 1.0 / 60, m, 1.0f, dir);
            b.step(t, 1.0 / 60, m, 1.0f, dir);
            const double hb = a.humanBeat(m.beat);
            *maxBeatOff = std::max(*maxBeatOff, float(std::fabs(hb - m.beat)));
            CHECK(hb >= lastBeat);  // the dancer's own clock never runs backwards
            lastBeat = hb;
            if (t < 3) continue;  // past the hello
            BodyOut oa, ob;
            a.build(oa);
            b.build(ob);
            // Compare the bodies by the mean of their part origins (same look: same parts list).
            V3 ha{}, hb3{};
            for (const auto& p : oa.parts) ha = ha + p.frame.o;
            for (const auto& p : ob.parts) hb3 = hb3 + p.frame.o;
            const V3 d = (ha - hb3) * (1.0f / float(oa.parts.size()));
            sum += length(d);
            ++n;
            if (n > 1) *maxJump = std::max(*maxJump, length(ha * (1.0f / float(oa.parts.size())) - prev));
            prev = ha * (1.0f / float(oa.parts.size()));
            // Still a valid body: every part finite, nothing under the floor.
            for (const auto& p : oa.parts) {
                REQUIRE(std::isfinite(p.frame.o.x + p.frame.o.y + p.frame.o.z));
                CHECK(p.frame.o.y > -0.05f);
            }
        }
        *meanDiff = float(sum / n);
    };
    float diff0, jump0, off0, diff1, jump1, off1;
    crew(0.0f, &diff0, &jump0, &off0);
    crew(1.0f, &diff1, &jump1, &off1);
    MESSAGE("human 0: diff " << diff0 << " jump " << jump0 << "; human 1: diff " << diff1 << " jump " << jump1
                             << ", beat offset up to " << off1);
    CHECK(diff0 < 1e-5f);    // machine-exact: identical
    CHECK(off0 == 0.0f);
    CHECK(diff1 > 0.003f);   // human: visibly different...
    CHECK(diff1 < 0.05f);    // ...but the same routine
    CHECK(off1 < 0.07f);     // within ~30 ms at 128 BPM
    CHECK(jump1 < jump0 * 1.3f + 1e-3f);  // and no jerkier than the exact version
}

TEST_CASE("dancers: arms never pass through the head, in any style's routines") {
    using namespace reforce::engine;
    using namespace reforce::engine::dance;
    int posedFrames = 0, frames = 0, deepFrames = 0;
    float worstPosed = 0, worst = 0, worstExtraStep = 0;
    for (int st = 0; st < kStyleCount; ++st)
        for (int who = 0; who < 3; ++who) {
            const DancerSpec spec = generateDancer(Style(st), std::uint32_t(who));
            const Skeleton sk = makeSkeleton(spec.look);
            ChoreoOptions o;
            o.style = Style(st);
            o.seed = std::uint32_t(100 + st * 7 + who);
            Choreographer c(o);
            V3 last[2] = {}, lastPosed[2] = {};
            const int per = 28;  // samples a beat: about 60 fps at 128 BPM
            for (int i = 0; i < 160 * per; ++i) {  // 160 beats
                const double beat = i / double(per);
                const DancePose p = c.pose(grooveAt(beat), sk);
                BodyOut out;
                buildBody(spec.look, sk, p, Frame{}, 1.0f, out);
                ++frames;
                posedFrames += out.armInHeadPosed > 0.005f;
                worstPosed = std::max(worstPosed, out.armInHeadPosed);
                worst = std::max(worst, out.armInHead);
                deepFrames += out.armInHead > 0.005f;
                // Smooth: a hand kept out of the head moves no more from one sample to the next than the free arm
                // would (plus a little), so it slides around the head instead of jumping.
                if (i > 0)
                    for (int h = 0; h < 2; ++h) {
                        const float step = length(out.hands[h] - last[h]);
                        const float stepPosed = length(out.handsPosed[h] - lastPosed[h]);
                        worstExtraStep = std::max(worstExtraStep, step - stepPosed);
                    }
                for (int h = 0; h < 2; ++h) {
                    last[h] = out.hands[h];
                    lastPosed[h] = out.handsPosed[h];
                }
            }
        }
    MESSAGE("arms into the head as posed: " << posedFrames << " of " << frames << " frames, up to " << worstPosed
                                            << "; as built: up to " << worst << " (" << deepFrames
                                            << " frames over 5 mm); hand step at most " << worstExtraStep
                                            << " more than as posed");
    CHECK(posedFrames > frames / 10);    // the moves as designed do reach into the head (what this is about)
    CHECK(deepFrames * 10000 < frames);  // kept out: over 5 mm in under 0.01 % of frames...
    // ...and never more than a graze (a forearm is 8 cm thick). Since the styles became the dance genres
    // (2026-11-21) the sampled routines include one crossfade (groove + swing, a break dancer) where the swing-away
    // leaves 5 cm for two frames; hand-made moves only play with reforce.Dancer.Mocap below 1.
    CHECK(worst < 0.055f);
    CHECK(worstExtraStep < 0.18f);       // going around the head adds at most a few cm to a frame's hand step
}

TEST_CASE("dancers: the dance clock: own feel within each beat, hits on the dancer's beat, never backwards") {
    using namespace reforce::engine;
    DancerRun a(generateDancer(Style::Pop, std::uint32_t(0)), 21, 0.0, 0.0, 0.0);
    a.setHuman(0.0f);
    for (double b = 0; b < 8; b += 0.37) CHECK(a.danceBeat(b) == b);  // Human 0: the music's beat exactly
    // Human 1 and 2, several dancers: monotonic; within the beat at most 0.4 / 2 pi beats from the hits' clock.
    for (std::uint32_t seed : {21u, 22u, 23u, 24u, 25u})
        for (float human : {1.0f, 2.0f}) {
            DancerRun d(generateDancer(Style::Pop, std::uint32_t(seed % 3)), seed, 0.0, 0.0, 0.0);
            d.setHuman(human);
            double last = -1e9, maxOff = 0;
            for (int i = 0; i < 64 * 200; ++i) {
                const double b = i / 200.0;
                const double db = d.danceBeat(b);
                REQUIRE(db > last);
                last = db;
                maxOff = std::max(maxOff, std::fabs(db - d.humanBeat(b)));
            }
            CHECK(maxOff <= 0.4 / 6.283185307179586 + 1e-9);
        }
    // Dancers differ in feel: across seeds the within-beat shift goes both ways.
    int punchy = 0, laidBack = 0;
    for (std::uint32_t seed = 1; seed <= 40; ++seed) {
        DancerRun d(generateDancer(Style::Pop, std::uint32_t(0)), seed, 0.0, 0.0, 0.0);
        d.setHuman(1.0f);
        // A quarter of the way into a beat of the dancer's own clock: punchy dancers are already further along.
        double b = 0;
        while (d.humanBeat(b) < 16.25) b += 0.001;
        const double off = d.danceBeat(b) - d.humanBeat(b);
        punchy += off > 0.003;
        laidBack += off < -0.003;
    }
    CHECK(punchy >= 5);
    CHECK(laidBack >= 5);
}

namespace {
// Steps a crowd as the Visualizer does: steer, step, keep apart. Returns the closest any two came before keepApart
// (what steering alone achieved) and after it; counts frames where keepApart had to push more than 4 cm.
struct CrowdStats {
    float closestSteered = 1e9f, closest = 1e9f;
    int shoves = 0, frames = 0;
};
void stepCrowd(std::vector<std::unique_ptr<reforce::engine::DancerRun>>& runs, double t, CrowdStats& st) {
    using namespace reforce::engine;
    std::vector<DancerRun*> crowd;
    for (auto& r : runs) crowd.push_back(r.get());
    avoidEachOther(crowd);
    for (auto& r : runs) r->step(t, 1.0 / 60, grooveAt(t * 2), 1.0f);
    float pre = 1e9f;
    for (std::size_t i = 0; i < runs.size(); ++i)
        for (std::size_t j = i + 1; j < runs.size(); ++j)
            pre = std::min(pre, std::hypot(runs[i]->x() - runs[j]->x(), runs[i]->z() - runs[j]->z()));
    keepApart(crowd);
    float post = 1e9f;
    for (std::size_t i = 0; i < runs.size(); ++i)
        for (std::size_t j = i + 1; j < runs.size(); ++j)
            post = std::min(post, std::hypot(runs[i]->x() - runs[j]->x(), runs[i]->z() - runs[j]->z()));
    st.closestSteered = std::min(st.closestSteered, pre);
    st.closest = std::min(st.closest, post);
    if (pre < kMinApart - 0.04f) ++st.shoves;
    ++st.frames;
}
}  // namespace

TEST_CASE("dancers: walking dancers steer round each other (head-on swap, a four-way crossing)") {
    using namespace reforce::engine;
    struct Case {
        std::string name;
        std::vector<std::pair<float, float>> from, to;
    };
    const std::vector<Case> cases = {
        {"head-on swap", {{-0.9f, -0.2f}, {0.9f, -0.2f}}, {{0.9f, -0.2f}, {-0.9f, -0.2f}}},
        {"four-way crossing", {{-0.8f, -0.6f}, {0.8f, -0.6f}, {-0.8f, 0.2f}, {0.8f, 0.2f}},
         {{0.8f, 0.2f}, {-0.8f, 0.2f}, {0.8f, -0.6f}, {-0.8f, -0.6f}}},
        {"through a standing dancer", {{-1.0f, -0.2f}, {0.0f, -0.2f}}, {{1.0f, -0.2f}, {0.0f, -0.2f}}},
    };
    for (const Case& c : cases) {
        std::vector<std::unique_ptr<DancerRun>> runs;
        for (std::size_t k = 0; k < c.from.size(); ++k) {
            runs.push_back(std::make_unique<DancerRun>(generateDancer(Style::Pop, std::uint32_t(k)), 40 + std::uint32_t(k), 0.0,
                                                       0.0, 0.0));
            runs.back()->setSlot(c.from[k].first, c.from[k].second);
        }
        CrowdStats st;
        double t = 0;
        for (; t < 1; t += 1.0 / 60) stepCrowd(runs, t, st);
        st = {};
        for (std::size_t k = 0; k < runs.size(); ++k) runs[k]->setSlot(c.to[k].first, c.to[k].second);
        for (; t < 9; t += 1.0 / 60) stepCrowd(runs, t, st);
        MESSAGE(c.name << ": closest " << st.closestSteered << " by steering, " << st.closest << " in the end, "
                       << st.shoves << " shoves");
        CHECK(st.closest > kMinApart - 0.005f);   // never overlapping
        CHECK(st.closestSteered > 0.3f);          // steering does the work...
        CHECK(st.shoves < st.frames / 50);        // ...the backstop hardly ever
        for (std::size_t k = 0; k < runs.size(); ++k)  // and everyone gets there
            CHECK(std::hypot(runs[k]->x() - c.to[k].first, runs[k]->z() - c.to[k].second) < 0.06f);
    }
}

TEST_CASE("dancers: a mingling crowd never overlaps") {
    using namespace reforce::engine;
    const StageCamera cam = stageCamera(16.0 / 9.0, 0.42);
    for (int n : {3, 6, 9, 12}) {
        std::vector<std::unique_ptr<DancerRun>> runs;
        for (int k = 0; k < n; ++k)
            runs.push_back(std::make_unique<DancerRun>(generateDancer(Style(k % kStyleCount), std::uint32_t(k)),
                                                       std::uint32_t(70 + k), 0.0, 0.0, 0.0));
        CrowdStats st;
        Mingle mingle;
        for (int i = 0; i < 60 * 180; ++i) {  // 3 minutes at 120 BPM
            const double t = i / 60.0;
            std::vector<MinglePerson> people;
            for (int k = 0; k < n; ++k) {
                const auto& r = *runs[std::size_t(k)];
                people.push_back({std::uint64_t(k + 1), r.x(), r.z(), r.facing(), r.placed()});
            }
            const auto spots = mingle.plan(people, cam, t * 2, 1.0 / 60, 9);
            for (int k = 0; k < n; ++k) runs[std::size_t(k)]->setSlot(spots[std::size_t(k)].x, spots[std::size_t(k)].z);
            stepCrowd(runs, t, st);
        }
        MESSAGE(n << " mingling: closest " << st.closestSteered << " by steering, " << st.closest << " in the end, "
                  << st.shoves << " shoves in " << st.frames << " frames");
        CHECK(st.closest > kMinApart - 0.01f);
        CHECK(st.shoves < st.frames / 50);
    }
}

TEST_CASE("dancers: generated characters vary, follow their style's wardrobe, and always build a sound body") {
    using namespace reforce::engine;
    // Same seed, same character.
    const DancerSpec a = generateDancer(Style::Disco, 77), b = generateDancer(Style::Disco, 77);
    CHECK(a.name == b.name);
    CHECK(a.look.top.r == b.look.top.r);
    CHECK(a.look.glasses == b.look.glasses);
    const int N = 300;
    std::set<std::string> names;
    std::set<int> glassesSeen, fabricsSeen, patternsSeen, eyesSeen, beardsSeen, outfitsSeen, hairSeen;
    int bearded = 0, dressed = 0;
    for (int si = 0; si < kStyleCount; ++si) {
        const Style st = Style(si);
        int sticks = 0, shades = 0, metallic = 0, neon = 0;
        float minH = 9, maxH = 0;
        for (int k = 0; k < N; ++k) {
            const DancerSpec d = generateDancer(st, std::uint32_t(k * 7919 + si));
            const Look& L = d.look;
            CHECK(d.style == st);
            names.insert(d.name);
            glassesSeen.insert(int(L.glasses));
            hairSeen.insert(int(L.hairStyle));
            fabricsSeen.insert(int(L.topFabric));
            patternsSeen.insert(int(L.topPattern));
            eyesSeen.insert(int(L.eyeShape));
            beardsSeen.insert(int(L.beard));
            outfitsSeen.insert(int(L.outfit));
            sticks += L.glowSticks;
            shades += L.glasses != Glasses::None;
            metallic += L.topFabric == Fabric::Shiny || L.topFabric == Fabric::Sequins;
            neon += L.topFabric == Fabric::Neon;
            // Feminine or masculine (2026-11-21): beards never with lashes, lipstick, a dress or a skirt.
            const bool femWear = L.lashes || L.lips.has_value() || L.outfit == Outfit::Dress ||
                                 L.outfit == Outfit::Skirt || L.outfit == Outfit::LongDress;
            CHECK_FALSE((femWear && L.beard != Beard::None));
            CHECK_FALSE((d.feminine && L.beard != Beard::None));
            CHECK_FALSE((!d.feminine && femWear));
            // Masculine only (Peter, 2026-11-21).
            for (HairStyle h : {HairStyle::Bald, HairStyle::Balding, HairStyle::ManBun, HairStyle::SlickBack,
                                HairStyle::Buzz, HairStyle::Quiff, HairStyle::Undercut, HairStyle::Shaved,
                                HairStyle::Mullet, HairStyle::Bowl, HairStyle::Pompadour})
                CHECK_FALSE((d.feminine && L.hairStyle == h));
            // Feminine only (Peter, 2026-11-21; the long curls 2026-11-27).
            for (HairStyle h : {HairStyle::Bob, HairStyle::Layers, HairStyle::HalfUp, HairStyle::Braid, HairStyle::Bangs,
                                HairStyle::Pixie, HairStyle::Pigtails, HairStyle::Bun, HairStyle::SidePony,
                                HairStyle::SpaceBuns, HairStyle::Curls, HairStyle::BigCurls, HairStyle::HugeCurls})
                CHECK_FALSE((!d.feminine && L.hairStyle == h));
            bearded += L.beard != Beard::None;
            dressed += L.outfit == Outfit::Dress || L.outfit == Outfit::Skirt || L.outfit == Outfit::LongDress;
            // Tall hair never pokes through a hat.
            if (L.hat == Hat::Cap || L.hat == Hat::Beanie || L.hat == Hat::Top)
                CHECK((L.hairStyle != HairStyle::Mohawk && L.hairStyle != HairStyle::Spiky &&
                       L.hairStyle != HairStyle::Afro && L.hairStyle != HairStyle::Bun &&
                       L.hairStyle != HairStyle::Curly && L.hairStyle != HairStyle::Pompadour &&
                       L.hairStyle != HairStyle::Undercut));
            if (k % 10) continue;
            // A sound body at rest and mid-move: finite, on the floor, about one unit tall.
            const Skeleton sk = makeSkeleton(L);
            Choreographer ch([&] {
                ChoreoOptions o;
                o.style = st;
                o.seed = std::uint32_t(k + 1);
                return o;
            }());
            for (double beat : {0.0, 9.3, 21.7}) {
                BodyOut out;
                const DancePose pose = beat == 0 ? restPose(sk) : ch.pose(grooveAt(beat), sk);
                buildBody(L, sk, pose, Frame{}, 1.0f, out);
                float top = 0;
                for (const BodyPart& p : out.parts) {
                    REQUIRE(std::isfinite(p.frame.o.x + p.frame.o.y + p.frame.o.z + p.r0 + p.r1 + p.len));
                    CHECK(p.frame.o.y > -0.06f);
                    top = std::max(top, p.frame.o.y + p.r0);
                }
                if (beat == 0) {
                    minH = std::min(minH, out.headCenter.y);
                    maxH = std::max(maxH, out.headCenter.y);
                }
                CHECK(out.parts.size() < 90);  // the instance budget: about 40-60 parts a dancer
            }
        }
        MESSAGE(std::string(styleName(st)) << ": glow sticks " << sticks << ", sunglasses " << shades << ", metallic " << metallic
                              << ", neon " << neon << " of " << N << "; head height " << minH << " .. " << maxH);
        if (st == Style::House) {  // now and then in rave gear
            CHECK(sticks > N / 8);  // about 1 in 5 (Peter, 2026-11-21)
            CHECK(sticks < N * 3 / 10);
            CHECK(neon > N / 10);
        } else {
            CHECK(sticks == 0);  // house dancers only
        }
        if (st == Style::Disco) CHECK(metallic > N / 3);
        CHECK(minH > 0.6f);
        CHECK(maxH < 1.15f);
    }
    // Across everyone: every sunglasses shape, fabric, print, eye shape, beard, outfit and hairstyle turns up.
    CHECK(glassesSeen.size() == std::size_t(Glasses::Count));
    CHECK(fabricsSeen.size() == std::size_t(Fabric::Count));
    CHECK(patternsSeen.size() == std::size_t(Pattern::Count));
    CHECK(eyesSeen.size() == std::size_t(EyeShape::Count));
    CHECK(beardsSeen.size() == std::size_t(Beard::Count));
    CHECK(outfitsSeen.size() == std::size_t(kOutfitCount));
    CHECK(hairSeen.size() == std::size_t(kHairStyleCount));
    MESSAGE("bearded " << bearded << ", in a dress or skirt " << dressed << " of " << N * kStyleCount);
    CHECK(bearded > N * kStyleCount / 10);  // masculine ones (half) often have one
    CHECK(dressed > N * kStyleCount / 12);
    CHECK(names.size() > 60);
}

TEST_CASE("dancers: look keys parse; unknown values warn") {
    CHECK(parsePattern("plaid") == Pattern::Plaid);
    CHECK(parseFabric("Sequins") == Fabric::Sequins);
    CHECK(parseGlasses("cateye") == Glasses::CatEye);
    CHECK(parseEyeShape("sleepy") == EyeShape::Sleepy);
    CHECK(parseBrows("arched") == Brows::Arched);
    CHECK(parseMouth("smirk") == Mouth::Smirk);
    CHECK(parseBeard("goatee") == Beard::Goatee);
    CHECK(parseBeard("handlebar") == Beard::Handlebar);
    CHECK(parseBeard("long") == Beard::Long);
    CHECK(parseBeard("short") == Beard::Short);
    CHECK(parseBeard("chinstrap") == Beard::Short);  // cut 2026-11-21
    CHECK_FALSE(parseGlasses("monocle"));
    CHECK_FALSE(parseFabric("tweed"));
}

TEST_CASE("dancers: generated skin tones are 40 % light, 40 % medium, 20 % dark, all natural") {
    using namespace reforce::engine;
    // The generator's ten tones by group (engine/Dancers.cpp generateDancer).
    const std::vector<std::string> light = {"#ffe0c8", "#f6d2b5", "#f0c4a0"}, medium = {"#e3ad86", "#d19a72", "#b9805a"},
                                   dark = {"#9c6644", "#7d4f33", "#633a24", "#4a2a1a"};
    auto inGroup = [](const std::vector<std::string>& g, Rgb c) {
        for (const auto& h : g) {
            const Rgb t = *parseRgb(h);
            if (std::fabs(t.r - c.r) + std::fabs(t.g - c.g) + std::fabs(t.b - c.b) < 1e-4f) return true;
        }
        return false;
    };
    const int N = 9000;
    int l = 0, m = 0, dk = 0;
    for (int k = 0; k < N; ++k) {
        const Rgb s = generateDancer(Style(k % kStyleCount), std::uint32_t(k * 2654435761u + 17)).look.skin;
        if (inGroup(light, s)) ++l;
        else if (inGroup(medium, s)) ++m;
        else if (inGroup(dark, s)) ++dk;
    }
    MESSAGE("light " << 100.0 * l / N << " %, medium " << 100.0 * m / N << " %, dark " << 100.0 * dk / N << " %");
    CHECK(l + m + dk == N);  // no other tones (no fantasy tints)
    CHECK(std::abs(l - N * 4 / 10) < N / 50);
    CHECK(std::abs(m - N * 4 / 10) < N / 50);
    CHECK(std::abs(dk - N * 2 / 10) < N / 50);
}

namespace {
// Mingle test floor: people walk to their spots (at most 0.75 units/s) and turn to their facing.
struct MingleSim {
    reforce::engine::Mingle mingle;
    std::vector<reforce::engine::MinglePerson> people;
    std::vector<std::string> roles;
    std::vector<reforce::engine::StageSpot> spots;
    std::uint32_t seed = 11;
    void step(const reforce::engine::StageCamera& cam, double beat, double dt) {
        spots = mingle.plan(people, cam, beat, dt, seed);
        roles.assign(people.size(), "");
        for (std::size_t i = 0; i < people.size(); ++i) {
            auto& p = people[i];
            const auto& s = spots[i];
            if (!p.placed) {
                p.x = s.x;
                p.z = s.z;
                p.placed = true;
            }
            const float dx = s.x - p.x, dz = s.z - p.z, d = std::hypot(dx, dz), stepLen = 0.75f * float(dt);
            if (d > stepLen) {
                p.x += dx / d * stepLen;
                p.z += dz / d * stepLen;
            } else {
                p.x = s.x;
                p.z = s.z;
            }
            const float f = std::isfinite(s.facing) ? s.facing : 0.0f;
            p.facing += (f - p.facing) * float(std::min(1.0, dt * 5));
            roles[i] = s.role;
        }
    }
};
}  // namespace

TEST_CASE("dancers: mingle: partners copy each other only within a dance style (else face to face)") {
    // (Peter, 2026-11-27.) Two crowds of six: all one style, and every other one a different style.
    using namespace reforce::engine;
    const auto cam = stageCamera(16.0 / 9.0, 0.42);
    for (int mixed = 0; mixed < 2; ++mixed) {
        MingleSim sim;
        for (int i = 0; i < 6; ++i) {
            sim.people.push_back({std::uint64_t(100 + i), 0, 0, 0, false});
            sim.people.back().style = mixed ? i % 2 : 3;
        }
        const double dt = 1.0 / 60, bpm = 124;
        long led = 0, ledAcross = 0, facing = 0;
        for (int f = 0; f < 60 * 600; ++f) {
            sim.step(cam, f * dt * bpm / 60, dt);
            for (std::size_t i = 0; i < sim.people.size(); ++i) {
                const auto& s = sim.spots[i];
                if (s.leader >= 0 && s.leader != int(i)) {
                    ++led;
                    ledAcross += sim.people[std::size_t(s.leader)].style != sim.people[i].style;
                }
                facing += sim.roles[i] == "facing";
            }
        }
        CHECK(ledAcross == 0);
        CHECK(facing > 0);
        if (!mixed) CHECK(led > 0);  // (the same style: partners as before)
    }
}

TEST_CASE("dancers: mingle: everyone on their own clock; pairs and groups form and break up") {
    using namespace reforce::engine;
    const auto cam = stageCamera(16.0 / 9.0, 0.42);
    MingleSim sim;
    for (int i = 0; i < 6; ++i) sim.people.push_back({std::uint64_t(100 + i), 0, 0, 0, false});
    const double dt = 1.0 / 60, bpm = 124;
    int framesIn[5] = {}, changes = 0, onBar = 0, maxSameBeat = 0, frames = 0;
    long lastBeat = -1;
    int sameBeat = 0, beatsWithChanges = 0, busyBeats = 0, crowdedBeats = 0;
    unsigned whoChanged = 0;  // this beat
    std::vector<std::string> prev(6);
    std::set<std::string> seenRoles;
    for (double t = 0; t < 600; t += dt, ++frames) {
        const double beat = t * bpm / 60;
        sim.step(cam, beat, dt);
        const long b = long(std::floor(beat));
        if (b != lastBeat) {
            sameBeat = __builtin_popcount(whoChanged);
            whoChanged = 0;
            maxSameBeat = std::max(maxSameBeat, sameBeat);
            if (sameBeat >= 5) ++crowdedBeats;
            if (sameBeat > 0) ++beatsWithChanges;
            if (sameBeat >= 4) ++busyBeats;
            sameBeat = 0;
            lastBeat = b;
        }
        for (std::size_t i = 0; i < 6; ++i) {
            const auto& s = sim.spots[i];
            REQUIRE(std::isfinite(s.x));
            REQUIRE(std::isfinite(s.z));
            CHECK(s.z >= kFloorZMin - 1e-4f);
            CHECK(s.z <= kFloorZMax + 1e-4f);
            CHECK(s.other < 6);
            CHECK(s.leader < 6);
            CHECK(s.other != int(i));
            CHECK(std::fabs(s.look) <= 0.86f);
            const int gs = sim.mingle.groupSize(sim.people[i].id);
            CHECK(gs <= 4);
            ++framesIn[gs];
            seenRoles.insert(sim.roles[i]);
            // Changes between activities (not the turns inside a group).
            const std::string now = gs > 0 ? std::string("group") : sim.roles[i];
            if (t > 0 && now != prev[i]) {
                ++changes;
                whoChanged |= 1u << i;
                const double inBar = std::fmod(beat, 4.0);
                if (inBar < 0.2 || inBar > 3.95) ++onBar;
            }
            prev[i] = now;
        }
        // Partners face each other.
        for (std::size_t i = 0; i < 6; ++i)
            if (sim.roles[i] == "with" && sim.spots[i].leader >= 0) {
                const auto& a = sim.spots[i];
                const auto& l = sim.spots[std::size_t(a.leader)];
                CHECK(a.mirror);
                CHECK((a.facing > 0) == (l.x > a.x));
            }
    }
    const int total = frames * 6;
    MESSAGE("alone " << framesIn[0] * 100 / total << "%, pairs " << framesIn[2] * 100 / total << "%, threes "
                     << framesIn[3] * 100 / total << "%, fours " << framesIn[4] * 100 / total << "%; " << changes
                     << " changes, " << onBar << " on a bar line, at most " << maxSameBeat << " in one beat, " << busyBeats << " of "
                     << beatsWithChanges << " beats with 4+");
    for (const char* r : {"dancing", "wandering", "going over to", "with", "facing", "in a circle with"})
        CHECK_MESSAGE(seenRoles.count(r), r);
    CHECK(framesIn[2] > total / 10);               // pairs happen a lot
    CHECK(framesIn[3] + framesIn[4] > total / 40);  // and bigger groups
    CHECK(framesIn[0] > total / 5);                // but people are on their own too
    CHECK(changes > 200);                          // ~ every few bars each
    CHECK(onBar * 2 < changes);                    // not on the bar lines
    // Never a crew-wide switch: 4+ of 6 changing in one beat is a rare coincidence (separate pairs forming,
    // a pair breaking up), not a formation change.
    CHECK(busyBeats * 25 < beatsWithChanges);
    // 5+ of 6 at once: a show ending (its watchers move on with it) on the beat others' own timers run out. Once in
    // the 10 minutes is a coincidence; more would be a crew-wide switch.
    CHECK(crowdedBeats <= 1);
}

TEST_CASE("dancers: mingle: people come and go, the clock may jump") {
    using namespace reforce::engine;
    const auto cam = stageCamera(16.0 / 9.0, 0.42);
    MingleSim sim;
    for (int i = 0; i < 5; ++i) sim.people.push_back({std::uint64_t(7 + i), 0, 0, 0, false});
    double beat = 0;
    for (int f = 0; f < 60 * 240; ++f) {
        const double dt = 1.0 / 60;
        beat += dt * 2;
        if (f == 60 * 60) beat -= 300;  // a new track: the beat clock starts over
        if (f == 60 * 90) sim.people.erase(sim.people.begin() + 2);
        if (f == 60 * 100) sim.people.erase(sim.people.begin());
        if (f == 60 * 120) sim.people.push_back({999, 0, 0, 0, false});
        sim.step(cam, beat, dt);
        for (std::size_t i = 0; i < sim.people.size(); ++i) {
            CHECK(sim.spots[i].other < int(sim.people.size()));
            CHECK(sim.spots[i].leader < int(sim.people.size()));
        }
    }
    // After the clock jumped back, people still change what they do.
    std::set<std::string> late;
    for (int f = 0; f < 60 * 60; ++f) {
        beat += 2.0 / 60;
        sim.step(cam, beat, 1.0 / 60);
        for (const auto& r : sim.roles) late.insert(r);
    }
    CHECK(late.size() >= 3);
}

TEST_CASE("dancers: mingle: one personality for everyone, whatever their seed (Peter, 2026-11-27)") {
    using namespace reforce::engine;
    const auto cam = stageCamera(16.0 / 9.0, 0.42);
    // Two floors seeded differently, dancers with different ids (a guest among them): the same personality.
    MingleSim a, b;
    a.seed = 11;
    b.seed = 977;
    a.people = {{1, 0, 0, 0, false}, {2, 0, 0, 0, false}};
    b.people = {{123456789, 0, 0, 0, false}, {42, 0, 0, 0, false}};
    b.people[1].vibe = MingleVibe::Crew;  // a guest's vibe is their choice, not a personality
    for (int f = 0; f < 120; ++f) {
        a.step(cam, f * 2.0 / 60, 1.0 / 60);
        b.step(cam, f * 2.0 / 60, 1.0 / 60);
    }
    const Mingle::Traits want = Mingle::kPersonality;
    for (const auto* s : {&a, &b})
        for (const auto& p : s->people) {
            const Mingle::Traits t = s->mingle.traits(p.id);
            CHECK(t.sociable == want.sociable);
            CHECK(t.restless == want.restless);
            CHECK(t.showy == want.showy);
        }
    // Middle of the road.
    for (const float v : {want.sociable, want.restless, want.showy}) {
        CHECK(v > 0.3f);
        CHECK(v < 0.7f);
    }
    CHECK(a.mingle.traits(999).sociable == 0);  // not on the floor
}

// How a mingling crowd spends its time (docs/dancers-design.md "Mingle only, one personality"): 24 runs of 10 minutes
// at 124 BPM (walkers as in MingleSim) for 6 and 12 dancers.
TEST_CASE("dancers: mingle: measure the shares of time alone, in pairs and groups, and activities" * doctest::skip()) {
    using namespace reforce::engine;
    const auto cam = stageCamera(16.0 / 9.0, 0.42);
    for (const int n : {6, 12}) {
        double in[7] = {}, frames = 0, changes = 0;
        std::map<std::string, double> roleTime;
        for (std::uint32_t run = 0; run < 24; ++run) {
            MingleSim sim;
            sim.seed = 11 + run * 7919u;
            for (int i = 0; i < n; ++i) sim.people.push_back({std::uint64_t(100 * (run + 1) + i), 0, 0, 0, false});
            std::vector<std::string> prev(static_cast<std::size_t>(n));
            const double dt = 1.0 / 60, bpm = 124;
            for (double t = 0; t < 600; t += dt) {
                sim.step(cam, t * bpm / 60, dt);
                for (std::size_t i = 0; i < std::size_t(n); ++i) {
                    const int gs = std::min(6, sim.mingle.groupSize(sim.people[i].id));
                    in[gs] += 1;
                    frames += 1;
                    roleTime[gs > 0 ? std::string("(in a group)") : sim.roles[i]] += 1;
                    const std::string now = gs > 0 ? std::string("group") : sim.roles[i];
                    if (t > 0 && now != prev[i]) changes += 1;
                    prev[i] = now;
                }
            }
        }
        char buf[256];
        std::snprintf(buf, sizeof buf, "%d dancers: alone %.1f %%, pairs %.1f %%, threes %.1f %%, fours %.1f %%; %.2f changes of activity a dancer-minute",
                      n, 100 * in[0] / frames, 100 * in[2] / frames, 100 * in[3] / frames, 100 * in[4] / frames,
                      changes / (frames / 3600));
        std::string line = buf;
        for (const auto& [r, f] : roleTime) {
            std::snprintf(buf, sizeof buf, "; %s %.1f %%", r.c_str(), 100 * f / frames);
            line += buf;
        }
        MESSAGE(line);
    }
}

TEST_CASE("dancers: hanging hair swings with the dance, smoothly and within bounds") {
    for (int si = 0; si < kStyleCount; ++si) {
        reforce::engine::DancerSpec spec = reforce::engine::generateDancer(Style(si), std::uint32_t(si));
        spec.look.hairStyle = HairStyle::Pigtails;
        reforce::engine::DancerRun run(spec, 5 + si, 0.0, 0.0, 0.0);
        double sum = 0, sumDiff = 0;
        float maxSw = 0, maxStep = 0;
        int n = 0;
        V3 prev[2] = {};
        for (int i = 0; i < 60 * 20; ++i) {
            const double t = i / 60.0;
            run.step(t, 1.0 / 60, grooveAt(t * 2), 1.0f);
            if (t < 1.5) {  // after the entrance
                prev[0] = run.hairSwing(0);
                prev[1] = run.hairSwing(1);
                continue;
            }
            for (int s = 0; s < 2; ++s) {
                const V3 sw = run.hairSwing(s);
                REQUIRE(finite(sw));
                maxSw = std::max(maxSw, length(sw));
                maxStep = std::max(maxStep, length(sw - prev[s]));
                sum += length(sw);
                prev[s] = sw;
            }
            sumDiff += length(run.hairSwing(0) - run.hairSwing(1));
            ++n;
        }
        const double mean = sum / (2 * n);
        MESSAGE("style " << si << ": mean swing " << mean << ", max " << maxSw
                         << ", max step " << maxStep << ", sides differ " << sumDiff / n);
        CHECK(mean > 0.04);         // it moves
        CHECK(maxSw <= 0.9001f);    // held to a sensible swing
        // The swing is measured from where the head is now: a robot snap moves the head in a frame and the hair's
        // end lags (as it should), so the swing can step by as much as the snap; the hair itself never jumps.
        CHECK(maxStep < 0.45f);
        CHECK(sumDiff / n > 0.01);  // the two sides don't swing as one
    }
}

TEST_CASE("dancers: random styles setting (reforce.Dancer.RandomStyles)") {
    using reforce::engine::normalizeRandomStyles;
    using reforce::engine::randomStyleAllowed;
    CHECK(normalizeRandomStyles("all") == "all");
    CHECK(normalizeRandomStyles("") == "none");
    CHECK(normalizeRandomStyles("none") == "none");
    CHECK(normalizeRandomStyles("Stomp, hip-hop,disco") == "disco,stomp,hiphop");  // style order, canonical names
    CHECK(normalizeRandomStyles("ballet,jazz,hiphop,stomp,disco,break,lock,pop,house") == "all");
    CHECK_FALSE(normalizeRandomStyles("disco,polka"));
    CHECK(normalizeRandomStyles("latin,stomp") == "stomp");  // the removed latin style is ignored in saved settings
    CHECK(normalizeRandomStyles("rave,retro,chill") == "house,lock,jazz");  // the styles before 2026-11-21
    CHECK(randomStyleAllowed("all", Style::Jazz));
    CHECK_FALSE(randomStyleAllowed("none", Style::Jazz));
    CHECK(randomStyleAllowed("disco,stomp", Style::Stomp));
    CHECK_FALSE(randomStyleAllowed("disco,stomp", Style::Lock));
    CHECK_FALSE(randomStyleAllowed("pop", Style::HipHop));
}

TEST_CASE("dancers: hair color: natural (platinum and silver included) for most styles, dyed only for house and pop") {
    const char* natural[] = {"#120c0a", "#1c1210", "#2a1a12", "#4a2c1a", "#6b3f22", "#8a4a20",
                             "#b5652a", "#c9a24f", "#e2c77a", "#8a8a8a", "#c0c0c8", "#e8e8f0"};
    auto isNatural = [&](Rgb c) {
        for (const char* h : natural) {
            const Rgb n = *parseRgb(h);
            if (std::fabs(n.r - c.r) < 1e-3f && std::fabs(n.g - c.g) < 1e-3f && std::fabs(n.b - c.b) < 1e-3f) return true;
        }
        return false;
    };
    for (int si = 0; si < kStyleCount; ++si) {
        const Style st = Style(si);
        int dyed = 0, platinum = 0;
        for (int k = 0; k < 600; ++k) {
            const Look L = reforce::engine::generateDancer(st, std::uint32_t(k * 104729 + si)).look;
            dyed += !isNatural(L.hair);
            platinum += L.hair.r > 0.7f && std::fabs(L.hair.r - L.hair.b) < 0.05f;
        }
        const bool mayDye = st == Style::House || st == Style::Pop;  // (house's rave gear, pop's robot look)
        if (mayDye) CHECK(dyed > 60);
        else CHECK(dyed == 0);
        CHECK(platinum > 10);  // platinum and silver turn up as natural colors in every style
    }
}

namespace {
// Points on a built part's surface (world), skipping what its clip plane cuts away.
template <class Fn>
void surfacePoints(const BodyPart& p, Fn&& fn) {
    for (int i = 0; i < 24; ++i) {
        const float a = 6.2831853f * float(i) / 24.0f, ca = std::cos(a), sa = std::sin(a);
        auto emit = [&](V3 q) {
            if (q.x * p.clip[0] + q.y * p.clip[1] + q.z * p.clip[2] > p.clip[3]) return;
            if (q.x * p.clip2[0] + q.y * p.clip2[1] + q.z * p.clip2[2] > p.clip2[3]) return;  // (hair only)
            fn(p.frame.o + p.frame.x * (q.x * p.scaleX) + p.frame.y * q.y + p.frame.z * (q.z * p.scaleZ));
        };
        for (int j = 0; j <= 8; ++j) {
            const float t = float(j) / 8.0f, r = p.r0 + (p.r1 - p.r0) * t;
            emit({r * ca, p.len * t, r * sa});
        }
        for (int k = 1; k <= 6; ++k) {
            const float e = 1.5707963f * float(k) / 6.0f;
            emit({p.r1 * std::cos(e) * ca, p.len + p.r1 * std::sin(e), p.r1 * std::cos(e) * sa});
            emit({p.r0 * std::cos(e) * ca, -p.r0 * std::sin(e), p.r0 * std::cos(e) * sa});
        }
    }
}
// How far (world units) the hair pokes out of the hat where the hat covers the head, judged against the hat as
// built: a cap / beanie's dome (above its cut, the hair must be inside it), a top hat's crown (above the brim).
float hairOutsideHat(const Look& look, const BodyOut& out, float margin) {
    auto same = [](Rgb a, Rgb b) { return std::fabs(a.r - b.r) + std::fabs(a.g - b.g) + std::fabs(a.b - b.b) < 1e-4f; };
    const BodyPart *dome = nullptr, *brim = nullptr, *crown = nullptr;
    for (const auto& p : out.parts) {
        if (same(p.color0, look.accent) && p.clip[3] < 1e8f && (!dome || p.r0 > dome->r0)) dome = &p;
        if (same(p.color0, look.accent) && p.scaleX > 8) brim = &p;
        if (p.len > 0 && same(p.color1, look.accent) && !same(p.color0, look.accent)) crown = &p;
    }
    float worst = 0;
    for (const auto& p : out.parts) {
        if (!same(p.color0, look.hair)) continue;
        surfacePoints(p, [&](V3 x) {
            if (look.hat == Hat::Top) {
                REQUIRE(brim);
                REQUIRE(crown);
                if (dot(x - brim->frame.o, brim->frame.y) <= margin) return;  // below the brim
                const V3 d = x - crown->frame.o;
                const float along = dot(d, crown->frame.y);
                const V3 radial = d - crown->frame.y * along;
                worst = std::max(worst, length(radial) - crown->r1);
            } else {
                REQUIRE(dome);
                const V3 d = x - dome->frame.o;
                const V3 q{dot(d, dome->frame.x) / dome->scaleX, dot(d, dome->frame.y), dot(d, dome->frame.z) / dome->scaleZ};
                if (q.x * dome->clip[0] + q.y * dome->clip[1] + q.z * dome->clip[2] > dome->clip[3] - margin) return;
                worst = std::max(worst, length(q) - dome->r0);
            }
        });
    }
    return worst;
}
}  // namespace

TEST_CASE("dancers: no hair pokes out of a hat, every hairstyle under every hat, at rest and dancing") {
    for (Hat hat : {Hat::Cap, Hat::Beanie, Hat::Top}) {
        for (int hsi = 0; hsi < kHairStyleCount; ++hsi) {
            reforce::engine::DancerSpec spec = reforce::engine::generateDancer(Style::Pop, std::uint32_t(0));
            spec.look.hairStyle = HairStyle(hsi);
            spec.look.hat = hat;
            spec.look.hair = {0.91f, 0.12f, 0.33f};  // colors no other part has
            spec.look.accent = {0.13f, 0.47f, 0.79f};
            const float R = makeSkeleton(spec.look).headR;
            CAPTURE(int(hat));
            CAPTURE(hsi);
            {
                const Skeleton sk = makeSkeleton(spec.look);
                BodyOut out;
                buildBody(spec.look, sk, restPose(sk), Frame{}, 1.0f, out);
                CHECK(hairOutsideHat(spec.look, out, 0.03f * R) < 0.01f * R);
            }
            reforce::engine::DancerRun run(spec, 3, 0.0, 0.0, 0.0);
            float worst = 0;
            for (int i = 0; i < 60 * 5; ++i) {
                const double t = i / 60.0;
                run.step(t, 1.0 / 60, grooveAt(t * 2), 1.0f);
                if (t < 1 || i % 6) continue;
                BodyOut out;
                run.build(out);
                worst = std::max(worst, hairOutsideHat(spec.look, out, 0.03f * R));
            }
            CHECK(worst < 0.015f * R);
        }
    }
}

TEST_CASE("dancers: mingle: busting a move, the group makes room and watches in an arc; the floor dancer stays put") {
    // Peter, 2026-11-27 (a guest's "Bust a move"; docs/dancers-design.md "Floor moves held still, room to go down"):
    // the others in their group make room and watch them, dancing; others nearby just clear the move's reach.
    using namespace reforce::engine;
    const auto cam = stageCamera(16.0 / 9.0, 0.42);
    const double dt = 1.0 / 60, bpm = 124;
    MingleSim sim;
    for (int i = 0; i < 7; ++i) sim.people.push_back({std::uint64_t(100 + i), 0, 0, 0, false});
    sim.people[0].with = 101;
    sim.people[3].with = 100;  // someone who wants to dance with them
    double t = 0;
    for (; t < 60; t += dt) sim.step(cam, t * bpm / 60, dt);
    REQUIRE(sim.mingle.together(100, 101));
    const int size = sim.mingle.groupSize(100);
    std::vector<std::uint64_t> group;
    for (std::uint64_t o = 101; o <= 106; ++o)
        if (sim.mingle.together(100, o)) group.push_back(o);
    dance::FloorReach reach;  // a routine's: 0.75 to the front, 0.45 behind
    for (int d = 0; d < 8; ++d) reach.dir[d] = 0.6f + 0.15f * std::cos(float(d) * 0.785398f);
    reach.max = 0.75f;
    sim.people[0].bustMove = true;
    sim.people[0].floor = 1;
    sim.people[0].reach = reach;
    sim.step(cam, t * bpm / 60, dt);
    const float x0 = sim.people[0].x, z0 = sim.people[0].z;
    int frames = 0, inside = 0, notFacing = 0, notWatching = 0, grew = 0;
    float moved = 0;
    for (const double start = t, end = t + 60; t < end; t += dt) {
        if (t > start + 4) sim.people[0].floor = 2;  // down from the next 8-bar point
        sim.step(cam, t * bpm / 60, dt);
        moved = std::max(moved, std::hypot(sim.people[0].x - x0, sim.people[0].z - z0));
        grew += sim.mingle.groupSize(100) > size;
        if (t < start + 6) continue;  // (time to step back)
        ++frames;
        for (std::size_t k = 1; k < sim.people.size(); ++k) {
            const auto& o = sim.people[k];
            const float dx = o.x - x0, dz = o.z - z0;
            inside += std::hypot(dx, dz) < floorNeed(reach, sim.spots[0].facing, dx, dz, kFloorKeep);
        }
        for (const auto id : group) {
            const std::size_t k = std::size_t(id - 100);
            const auto& o = sim.people[k];
            notFacing += std::fabs(std::remainder(o.facing - std::atan2(x0 - o.x, z0 - o.z), 6.2831853f)) > 0.35f;
            notWatching += !sim.spots[k].react || sim.spots[k].other != 0;
        }
    }
    MESSAGE("busting a move in a group of " << size << ": moved " << moved << ", neighbors inside the reach " << inside
                                            << " of " << frames * 6 << ", group not facing " << notFacing << ", not watching "
                                            << notWatching);
    CHECK(moved < 1e-4f);
    CHECK(inside * 100 < frames * 6);  // (someone passing behind, now and then)
    CHECK(grew == 0);
    CHECK(notFacing < frames * int(group.size()) / 20);
    CHECK(notWatching == 0);
    CHECK(sim.mingle.together(100, 101));  // still their group
    // Done: their own social life again; the room goes.
    sim.people[0].bustMove = false;
    sim.people[0].floor = 0;
    int together = 0;
    for (const double end = t + 120; t < end; t += dt) {
        sim.step(cam, t * bpm / 60, dt);
        together += sim.mingle.together(100, 101);
    }
    CHECK(together > 0);
}

TEST_CASE("dancers: mingle: a partner doesn't mirror a floor move: they make room and watch (kFollowerMirrorsFloor)") {
    // Peter, 2026-11-27 (still deciding; kFollowerMirrorsFloor flips it back to copying).
    using namespace reforce::engine;
    const auto cam = stageCamera(16.0 / 9.0, 0.42);
    const double dt = 1.0 / 60, bpm = 124;
    dance::FloorReach reach;
    for (float& d : reach.dir) d = 0.6f;
    reach.max = 0.6f;
    int checked = 0;
    for (std::uint64_t base : {100u, 200u, 300u, 400u}) {
        MingleSim sim;
        for (int i = 0; i < 4; ++i) sim.people.push_back({base + std::uint64_t(i), 0, 0, 0, false});
        int leader = -1, follower = -1;
        double t = 0;
        for (; t < 300 && leader < 0; t += dt) {
            sim.step(cam, t * bpm / 60, dt);
            for (std::size_t k = 0; k < sim.spots.size(); ++k)
                if (sim.spots[k].leader >= 0 && sim.spots[k].mirror) {
                    leader = sim.spots[k].leader;
                    follower = int(k);
                }
        }
        if (leader < 0) continue;
        auto& L = sim.people[std::size_t(leader)];
        L.bustMove = true;
        L.floor = 2;  // (on the floor)
        L.reach = reach;
        for (const double end = t + 10; t < end; t += dt) sim.step(cam, t * bpm / 60, dt);
        const auto& f = sim.spots[std::size_t(follower)];
        CHECK(f.leader == (kFollowerMirrorsFloor ? leader : -1));
        if (!kFollowerMirrorsFloor) {
            CHECK(f.react);
            CHECK(f.other == leader);
            const auto& fp = sim.people[std::size_t(follower)];
            CHECK(std::hypot(fp.x - L.x, fp.z - L.z) > reach.max + kFloorKeep - 0.01f);
        }
        ++checked;
    }
    CHECK(checked > 0);
}

TEST_CASE("dancers: mingle: stream guests' social settings (Solo, Crew, dancing with someone)") {
    using namespace reforce::engine;
    const auto cam = stageCamera(16.0 / 9.0, 0.42);
    const double dt = 1.0 / 60, bpm = 124;
    auto crowd = [](MingleSim& sim, int n) {
        for (int i = 0; i < n; ++i) sim.people.push_back({std::uint64_t(100 + i), 0, 0, 0, false});
    };
    auto idx = [](std::uint64_t id) { return std::size_t(id - 100); };

    SUBCASE("someone picked: they walk over, stay together, and follow when the other moves on") {
        MingleSim sim;
        crowd(sim, 8);
        sim.people[idx(101)].with = 105;
        int frames = 0, together = 0;
        for (double t = 0; t < 300; t += dt, ++frames) {
            sim.step(cam, t * bpm / 60, dt);
            if (t > 30) together += sim.mingle.together(101, 105);
        }
        const int counted = int((300 - 30) / dt);
        CHECK(together > counted * 7 / 10);  // most of the time (walking back over after the other moves on)
    }
    SUBCASE("picking each other works too") {
        MingleSim sim;
        crowd(sim, 6);
        sim.people[idx(100)].with = 104;
        sim.people[idx(104)].with = 100;
        int together = 0, frames = 0;
        for (double t = 0; t < 200; t += dt) {
            sim.step(cam, t * bpm / 60, dt);
            if (t > 30) ++frames, together += sim.mingle.together(100, 104);
        }
        CHECK(together > frames * 8 / 10);
    }
    SUBCASE("Solo: never in a group; someone who picked them dances near them, watching") {
        MingleSim sim;
        crowd(sim, 8);
        sim.people[idx(103)].vibe = MingleVibe::Solo;
        sim.people[idx(102)].with = 103;
        int grouped = 0, watching = 0, frames = 0;
        float far = 0;
        for (double t = 0; t < 300; t += dt) {
            sim.step(cam, t * bpm / 60, dt);
            grouped += sim.mingle.groupSize(103) > 0;
            if (t > 30) {
                ++frames;
                watching += sim.mingle.watching(102) == 103;
                const auto& a = sim.people[idx(102)];
                const auto& b = sim.people[idx(103)];
                far = std::max(far, std::hypot(a.x - b.x, a.z - b.z));
            }
        }
        CHECK(grouped == 0);
        CHECK(watching == frames);
        CHECK(far < 1.6f);  // close by (they follow a wander at walking speed)
    }
    SUBCASE("switching to Solo leaves the group") {
        MingleSim sim;
        crowd(sim, 4);
        sim.people[idx(100)].with = 101;
        double t = 0;
        for (; t < 60; t += dt) sim.step(cam, t * bpm / 60, dt);
        REQUIRE(sim.mingle.together(100, 101));
        sim.people[idx(100)].with = 0;
        sim.people[idx(100)].vibe = MingleVibe::Solo;
        sim.step(cam, t * bpm / 60, dt);
        CHECK(sim.mingle.groupSize(100) == 0);
    }
    SUBCASE("Crew: in a group most of the time; Solo dancers never") {
        MingleSim sim;
        crowd(sim, 10);
        sim.people[idx(100)].vibe = MingleVibe::Crew;
        sim.people[idx(101)].vibe = MingleVibe::Crew;
        int crew = 0, frames = 0, solo = 0;
        for (int i = 2; i < 5; ++i) sim.people[std::size_t(i)].vibe = MingleVibe::Solo;
        for (double t = 0; t < 400; t += dt) {
            sim.step(cam, t * bpm / 60, dt);
            for (int i = 2; i < 5; ++i) solo += sim.mingle.groupSize(std::uint64_t(100 + i)) > 0;
            if (t > 40) ++frames, crew += sim.mingle.groupSize(100) > 0;
        }
        CHECK(solo == 0);
        CHECK(crew > frames * 7 / 10);
    }
    SUBCASE("the one picked goes away: back to mingling") {
        MingleSim sim;
        crowd(sim, 5);
        sim.people[idx(100)].with = 104;
        double t = 0;
        for (; t < 60; t += dt) sim.step(cam, t * bpm / 60, dt);
        sim.people.pop_back();  // 104 leaves
        for (; t < 120; t += dt) sim.step(cam, t * bpm / 60, dt);
        for (const auto& s : sim.spots) {
            CHECK(std::isfinite(s.x));
            CHECK(s.other < int(sim.people.size()));
        }
    }
    CHECK(parseMingleVibe("solo") == MingleVibe::Solo);
    CHECK(parseMingleVibe("crew") == MingleVibe::Crew);
    CHECK(parseMingleVibe("") == MingleVibe::Mingle);
    CHECK(!parseMingleVibe("party"));
}

TEST_CASE("dancers: a new look in place keeps the dancer going; a new style dances that style") {
    using namespace reforce::engine;
    DancerSpec spec = generateDancer(Style::House, 7u);
    spec.guestId = "g1";
    DancerRun run(spec, 5, 0.0, 0.0, 60.0);
    for (int i = 0; i < 120; ++i) run.step(i / 60.0, 1.0 / 60, grooveAt(i / 30.0), 1.0f);
    DancerSpec next = generateDancer(Style::Break, 9u);
    next.name = "Sam";
    run.restyle(next);
    CHECK(run.spec().guestId == "g1");
    CHECK(run.name() == "Sam");
    CHECK(run.choreographer().options().style == Style::Break);
    CHECK(run.seed() == 5u);
    for (int i = 120; i < 240; ++i) run.step(i / 60.0, 1.0 / 60, grooveAt(i / 30.0), 1.0f);
    BodyOut out;
    run.build(out);
    CHECK(!out.parts.empty());
    // A check-in again: the full stay from now.
    run.stayFor(50.0, 60.0);
    CHECK(!run.leaving());
    run.step(100.0, 1.0 / 60, grooveAt(200), 1.0f);
    CHECK(!run.leaving());
    run.step(111.0, 1.0 / 60, grooveAt(222), 1.0f);
    CHECK(run.leaving());
}

namespace {
// A mingling crowd stepped as the Visualizer does (direction from the spots), for the crowd measurements below.
struct CrowdProbe {
    int jitters = 0, moving = 0, limbHits = 0, bodyFrames = 0, stuck = 0, turnFlips = 0;
    float reach = 0;  // furthest a part end gets from its own dancer's axis, above the knees
    std::vector<int> jitterBy;
    std::map<std::string, int> jitterRole;  // role (and whether its spot is within kMinApart of another's spot)
    int closeSpots = 0;  // frames with two spots closer than kMinApart
};
CrowdProbe probeCrowd(int n, const std::function<void(std::vector<reforce::engine::MinglePerson>&)>& social,
                      double seconds, float coreR, std::uint32_t seed = 0) {
    using namespace reforce::engine;
    const StageCamera cam = stageCamera(16.0 / 9.0, 0.42);
    std::vector<std::unique_ptr<DancerRun>> runs;
    for (int k = 0; k < n; ++k)
        runs.push_back(std::make_unique<DancerRun>(generateDancer(Style(k % kStyleCount), std::uint32_t(k) + seed * 31),
                                                   std::uint32_t(70 + k) + seed * 977, 0.0, 0.0, 0.0));
    Mingle mingle;
    CrowdProbe pr;
    pr.jitterBy.assign(std::size_t(n), 0);
    const std::size_t nn = std::size_t(n);
    std::vector<float> px(nn), pz(nn), vx(nn), vz(nn), pf(nn), df(nn);
    std::vector<int> stuckFor(nn);
    for (int i = 0; i < int(seconds * 60); ++i) {
        const double t = i / 60.0;
        std::vector<MinglePerson> people;
        for (int k = 0; k < n; ++k) {
            const auto& r = *runs[std::size_t(k)];
            MinglePerson p{std::uint64_t(k + 1), r.x(), r.z(), r.facing(), r.placed()};
            p.bustMove = r.inBustMove(t * 2);
            p.style = int(r.spec().style);
            r.floorInfo(p, t * 2);  // (as the Visualizer: floor moves get their room)
            people.push_back(p);
        }
        social(people);
        const auto spots = mingle.plan(people, cam, t * 2, 1.0 / 60, 9 + seed);
        std::vector<DancerRun*> crowd;
        for (auto& r : runs) crowd.push_back(r.get());
        avoidEachOther(crowd);
        for (int k = 0; k < n; ++k) {
            auto& run = *runs[std::size_t(k)];
            const auto& sp = spots[std::size_t(k)];
            run.setSlot(sp.x, sp.z);
            DanceDirection dir;
            if (sp.leader >= 0 && sp.leader != k) dir.leader = &runs[std::size_t(sp.leader)]->choreographer();
            dir.mirror = sp.mirror;
            dir.react = sp.react;
            dir.facing = sp.facing;
            dir.look = sp.look;
            dir.floorRoom = sp.floorRoom;
            run.step(t, 1.0 / 60, grooveAt(t * 2), std::clamp(sp.effort, 0.3f, 1.0f), dir);
        }
        keepApart(crowd);
        {
            bool close = false;
            for (int a = 0; a < n; ++a)
                for (int b = a + 1; b < n; ++b)
                    close |= std::hypot(spots[std::size_t(a)].x - spots[std::size_t(b)].x,
                                        spots[std::size_t(a)].z - spots[std::size_t(b)].z) < kMinApart;
            pr.closeSpots += close;
        }
        // Jitter: the step reverses (both steps big enough to see).
        for (int k = 0; k < n; ++k) {
            const auto& r = *runs[std::size_t(k)];
            const float dx = r.x() - px[std::size_t(k)], dz = r.z() - pz[std::size_t(k)];
            const float a = std::hypot(dx, dz), b = std::hypot(vx[std::size_t(k)], vz[std::size_t(k)]);
            if (i > 1 && a > 0.0003f && b > 0.0003f) {
                ++pr.moving;
                if (dx * vx[std::size_t(k)] + dz * vz[std::size_t(k)] < -0.5f * a * b) {
                    ++pr.jitters;
                    float near = 9;
                    for (int o = 0; o < n; ++o)
                        if (o != k) near = std::min(near, std::hypot(spots[std::size_t(o)].x - spots[std::size_t(k)].x,
                                                                     spots[std::size_t(o)].z - spots[std::size_t(k)].z));
                    ++pr.jitterRole[std::string(spots[std::size_t(k)].role) + (near < kMinApart ? " (spot taken)" : "")];
                    ++pr.jitterBy[std::size_t(k)];
                }
            }
            // Stuck: a spot well away, hardly moving, for over a second.
            const auto& sp = spots[std::size_t(k)];
            if (std::hypot(sp.x - r.x(), sp.z - r.z()) > 0.15f && a < 0.0012f) {
                if (++stuckFor[std::size_t(k)] == 60) ++pr.stuck;
            } else {
                stuckFor[std::size_t(k)] = 0;
            }
            // The body turning back and forth (a turn reversing within a frame, both noticeable).
            const float f = r.facing(), d = std::remainder(f - pf[std::size_t(k)], 6.2831853f);
            if (i > 1 && std::fabs(d) > 0.004f && std::fabs(df[std::size_t(k)]) > 0.004f && d * df[std::size_t(k)] < 0)
                ++pr.turnFlips;
            pf[std::size_t(k)] = f;
            df[std::size_t(k)] = d;
            px[std::size_t(k)] = r.x();
            pz[std::size_t(k)] = r.z();
            vx[std::size_t(k)] = dx;
            vz[std::size_t(k)] = dz;
        }
        // Limbs in someone else's body: any part end inside another dancer's core (a cylinder round their axis, from
        // the knees to the shoulders).
        if (t > 5 && i % 6 == 0) {
            std::vector<dance::BodyOut> outs(nn);
            for (int k = 0; k < n; ++k) runs[std::size_t(k)]->build(outs[std::size_t(k)]);
            ++pr.bodyFrames;
            for (int a = 0; a < n; ++a) {
                const auto& A = outs[std::size_t(a)];
                for (const auto& p : A.parts)
                    for (const dance::V3 e : {p.frame.o, p.frame.point({0, p.len, 0})}) {
                        if (e.y > 0.3f * A.headTop)
                            pr.reach = std::max(pr.reach, std::hypot(e.x - A.rootFloor.x, e.z - A.rootFloor.z));
                        for (int b = 0; b < n; ++b) {
                            if (b == a) continue;
                            const auto& B = outs[std::size_t(b)];
                            if (e.y < 0.3f * B.headTop || e.y > 0.75f * B.headTop) continue;
                            if (std::hypot(e.x - B.rootFloor.x, e.z - B.rootFloor.z) < coreR) {
                                ++pr.limbHits;
                                goto nextPart;
                            }
                        }
                    nextPart:;
                    }
            }
        }
    }
    return pr;
}
}  // namespace

TEST_CASE("dancers: a mingling crowd: no jitter, arms mostly clear of each other (also with guests' social settings)") {
    using namespace reforce::engine;
    // Peter, 2026-11-27: dancers jittered against each other (spots handed out closer than dancers may stand: walk
    // there, pushed back, walk there...) and arms went into neighbors (reach about 0.48; pairs stood 0.52 apart).
    // Before the fix: 0.2 % of moving steps reversing and limb ends in a neighbor 420 times in 1749 frames.
    struct Case {
        std::string name;
        std::function<void(std::vector<MinglePerson>&)> social;
        std::uint32_t seed;
    };
    const std::vector<Case> cases = {
        {"8 mingling", [](auto&) {}, 0},
        {"8 mingling (other seed)", [](auto&) {}, 3},
        {"five pick one", [](auto& ps) { for (int k = 1; k < 6; ++k) ps[std::size_t(k)].with = 1; }, 0},
        {"two near a Solo one", [](auto& ps) {
             ps[0].vibe = MingleVibe::Solo;
             ps[1].with = 1;
             ps[2].with = 1;
         }, 0},
        {"three crew", [](auto& ps) { for (int k = 0; k < 3; ++k) ps[std::size_t(k)].vibe = MingleVibe::Crew; }, 0},
    };
    // Each case over four seeds (2026-11-27, one personality): reversing steps are heavy-tailed. Most runs have a few
    // (8 mingling: median 14 in 3 minutes), but now and then two walkers stall against each other for a few seconds
    // (e.g. head-on at the back of the floor, where keeping right pushes one into the back edge) and one run gets
    // 50-130; over 64 seeds 0.40 per thousand before the change (4 runs over 1 per thousand), 0.43-0.51 after (5-8).
    for (const auto& c : cases) {
        CrowdProbe p;
        for (const std::uint32_t s : {c.seed, c.seed + 10, c.seed + 20, c.seed + 30}) {
            const CrowdProbe q = probeCrowd(8, c.social, 180, 0.1f, s);
            p.jitters += q.jitters;
            p.moving += q.moving;
            p.limbHits += q.limbHits;
            p.bodyFrames += q.bodyFrames;
            p.stuck += q.stuck;
            CHECK(q.stuck < 15);
        }
        MESSAGE(c.name << ": " << p.jitters << " reversing of " << p.moving << " moving steps, " << p.stuck
                       << " stuck, limb ends in a neighbor " << p.limbHits << " in " << p.bodyFrames << " frames");
        CHECK(p.jitters * 1000 < p.moving);
        CHECK(p.limbHits * 8 < p.bodyFrames);
    }
}

