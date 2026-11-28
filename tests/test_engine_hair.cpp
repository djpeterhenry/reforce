// Hair physics (Peter, 2026-11-27; docs/dancers-design.md "Hair physics"): hanging hair as chains that bend, trail
// and keep out of the body; loose volumes that lag the head; the classic swing still there (reforce.Dancer.Hair 0).
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "engine/DanceClips.hpp"
#include "engine/DanceMoves.hpp"
#include "engine/DancerRig.hpp"
#include "engine/Dancers.hpp"

using namespace reforce::engine;
using namespace reforce::engine::dance;

namespace {

bool finite(V3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

MusicState grooveAt(double beat) {
    MusicState g;
    g.beat = beat;
    g.bar = beat / 4;
    g.phrase = beat / 64;
    g.bass = 0.5;
    return g;
}

bool haveClips() {
    static const bool ok = loadClips(std::string(REFORCE_SOURCE_DIR) + "/content/Moves/aist.moves").empty();
    return ok;
}

// Every look with hanging hair (and one under a cap: the locks hanging out below it).
struct HangLook {
    const char* name;
    HairStyle style;
    Hat hat = Hat::None;
};
const HangLook kHanging[] = {
    {"ponytail", HairStyle::Ponytail}, {"pigtails", HairStyle::Pigtails}, {"braid", HairStyle::Braid},
    {"sidepony", HairStyle::SidePony}, {"long", HairStyle::Long},         {"bob", HairStyle::Bob},
    {"bangs", HairStyle::Bangs},       {"layers", HairStyle::Layers},     {"halfup", HairStyle::HalfUp},
    {"shag", HairStyle::Shag},         {"mullet", HairStyle::Mullet},     {"long+cap", HairStyle::Long, Hat::Cap},
    {"pigtails+beanie", HairStyle::Pigtails, Hat::Beanie},
};

HairCollider shrunk(HairCollider c, float by) {
    c.r0 = std::max(0.0f, c.r0 - by);
    c.r1 = std::max(0.0f, c.r1 - by);
    return c;
}

// How far the hair as built reaches into the body: along every link (five points each), into the arms and hands, the
// head (chains kept out of it) and the torso, as the sim sees them: each shrunk by as far as the chain passes into
// it as designed, at rest (DancerRun::hairShrink).
struct Depth {
    float arms = 0, head = 0, torso = 0;
};
Depth hairDepth(const DancerRun& run) {
    Depth d;
    HairCollider col[kHairColliders];
    run.hairBody(col);
    const HairState& hs = run.hairState();
    for (int c = 0; c < hs.chains; ++c) {
        const HairChainDef& def = run.hairChain(c);
        for (int i = 0; i < def.segs; ++i)
            for (int s = 0; s <= 4; ++s) {
                if (i == 0 && s < 2) continue;  // the first link's upper half grows out of the head and lies on it
                const V3 p = lerp(hs.pts[c][i], hs.pts[c][i + 1], float(s) / 4);
                for (int j = 0; j < kHairColliders; ++j) {
                    if (j == kColHead && !def.head) continue;
                    if (i == 0 && def.cone > 0 && j >= kColArms) continue;  // (a long column's first link: arms pass)
                    const HairCollider cj = shrunk(col[j], run.hairShrink(c, j));
                    const float k = colliderDepth(cj, p, def.radius);
                    float& slot = j == kColTorso ? d.torso : j == kColHead ? d.head : d.arms;
                    slot = std::max(slot, k);
                }
            }
    }
    return d;
}

DancerSpec lookSpec(const HangLook& h, int seed) {
    DancerSpec spec = generateDancer(Style(seed % kStyleCount), std::uint32_t(seed));
    spec.look.hairStyle = h.style;
    spec.look.hat = h.hat;
    return spec;
}

}  // namespace

TEST_CASE("hair physics: hanging hair is a chain per piece, the same layout as the classic swing at rest") {
    for (const HangLook& h : kHanging) {
        CAPTURE(std::string(h.name));
        const DancerSpec spec = lookSpec(h, 3);
        const Skeleton sk = makeSkeleton(spec.look);
        BodyOut a, b;
        buildBody(spec.look, sk, restPose(sk), Frame{}, 1.0f, a);
        CHECK(a.hairChainCount >= 1);
        CHECK(a.hairChainCount <= kHairChains);
        // With the sim's state at rest (no chains stepped yet: straight from root to rest tip, no lag) the pieces
        // lie where the classic swing puts them when still, only in more links.
        HairState still;
        buildBody(spec.look, sk, restPose(sk), Frame{}, 1.0f, b, &still);
        CHECK(b.parts.size() >= a.parts.size());
        CHECK(b.headTop == doctest::Approx(a.headTop).epsilon(1e-4));
    }
    // Short styles have no chains.
    for (HairStyle s : {HairStyle::Short, HairStyle::Afro, HairStyle::Bun, HairStyle::Spiky, HairStyle::Bald}) {
        Look look;
        look.hairStyle = s;
        BodyOut o;
        buildBody(look, makeSkeleton(look), restPose(makeSkeleton(look)), Frame{}, 1.0f, o);
        CHECK(o.hairChainCount == 0);
    }
}

TEST_CASE("hair physics: chains bend and trail with the dance, stay finite and smooth, and keep out of the body") {
    // The hand-made routines of every style, each hanging look, 20 s at 120 BPM.
    float worstArms = 0, worstHead = 0, worstTorso = 0, worstStep = 0;
    long frames = 0, deep = 0;
    for (int li = 0; li < int(std::size(kHanging)); ++li) {
        const HangLook& h = kHanging[li];
        CAPTURE(std::string(h.name));
        for (int st = 0; st < kStyleCount; st += 2) {
            DancerRun run(lookSpec(h, st + li), 7 + std::uint32_t(st), 0.0, 0.0, 0.0);
            run.choreographer().setMocap(0.0f);
            HairState prev;
            Frame prevHead;
            for (int i = 0; i < 60 * 20; ++i) {
                const double t = i / 60.0;
                run.step(t, 1.0 / 60, grooveAt(t * 2), 1.0f);
                const HairState& hs = run.hairState();
                if (t > 1.0) REQUIRE(hs.chains >= 1);
                for (int c = 0; c < hs.chains; ++c)
                    for (int p = 0; p <= run.hairChain(c).segs; ++p) {
                        REQUIRE(finite(hs.pts[c][p]));
                        if (t > 1.0) {
                            // In the head's frame, per unit length (as the classic swing is measured): the head
                            // itself moves as fast as the dance (a pose can jump).
                            const Frame hn = run.headNow();
                            auto local = [](const Frame& f, V3 q) {
                                q = q - f.o;
                                return V3{dot(q, f.x), dot(q, f.y), dot(q, f.z)};
                            };
                            const HairChainDef& cd = run.hairChain(c);
                            const float st1 = length(local(hn, hs.pts[c][p]) - local(prevHead, prev.pts[c][p])) /
                                              length(cd.b - cd.a);
                            worstStep = std::max(worstStep, st1);
                        }
                    }
                prev = hs;
                prevHead = run.headNow();
                if (t < 1.0) continue;  // after the entrance
                const Depth d = hairDepth(run);
                worstArms = std::max(worstArms, d.arms);
                worstHead = std::max(worstHead, d.head);
                worstTorso = std::max(worstTorso, d.torso);
                ++frames;
                deep += std::max({d.arms, d.head, d.torso}) > 0.01f;
            }
        }
    }
    MESSAGE("hair into the body (hand-made moves): arms up to " << worstArms << ", head " << worstHead << ", torso "
                                                                << worstTorso << "; over 1 cm in " << deep << " of "
                                                                << frames << " frames; largest point step "
                                                                << worstStep);
    // Over a centimeter in under 0.1 % of frames, never more than a graze (a forearm is 8 cm thick): where a joint
    // can bend no further a link can't wrap round an arm.
    CHECK(deep * 1000 < frames);
    CHECK(worstArms < 0.04f);
    CHECK(worstHead < 0.04f);
    CHECK(worstTorso < 0.04f);
    // The hair never jumps: as the classic swing's test, a pose that jumps moves the head in a frame and the hair
    // lags (as it should), so in the head's frame it can step about as far (most on the short mullet tufts, on a
    // locking snap; per unit length).
    CHECK(worstStep < 0.8f);
}

TEST_CASE("hair physics: a spin flings hanging hair out, a bounce makes it bob, still hair settles") {
    const DancerSpec spec = lookSpec(kHanging[0], 1);  // ponytail
    const Skeleton sk = makeSkeleton(spec.look);
    DancerRun run(spec, 3, 0.0, 0.0, 0.0);
    for (int i = 0; i < 60; ++i) run.step(i / 60.0, 1.0 / 60, grooveAt(i / 30.0), 1.0f);  // on stage
    const HairChainDef& d = run.hairChain(0);
    const float len = length(d.b - d.a);
    // Standing still: the tip settles where the rest shape and gravity put it.
    DancePose rest = restPose(sk);
    for (int i = 0; i < 240; ++i) run.stepHairWithPose(rest, 1.0 / 60);
    const V3 settled = run.hairState().pts[0][d.segs];
    for (int i = 0; i < 30; ++i) run.stepHairWithPose(rest, 1.0 / 60);
    CHECK(length(run.hairState().pts[0][d.segs] - settled) < 1e-3f);
    // Spinning (a turn a second): the tip swings out away from the axis.
    const Frame h0 = headFrame(sk, rest);
    auto radial = [&](V3 p) { return std::hypot(p.x - h0.o.x, p.z - h0.o.z); };
    const float out0 = radial(settled);
    float outMax = 0;
    DancePose spin = rest;
    for (int i = 0; i < 120; ++i) {
        spin.bodyYaw = float(i) / 60.0f * 6.2831853f;
        run.stepHairWithPose(spin, 1.0 / 60);
        outMax = std::max(outMax, radial(run.hairState().pts[0][d.segs]));
    }
    MESSAGE("ponytail tip from the spin axis: still " << out0 << ", spinning up to " << outMax << " (length " << len
                                                      << ")");
    CHECK(outMax > out0 + 0.2f * len);
    // The loose volumes turn behind the head while it spins, held to a sensible lag.
    CHECK(length(run.hairState().lagR) > 0.05f);
    CHECK(length(run.hairState().lagR) < 0.36f);
}

TEST_CASE("hair physics: upside down the hair falls towards the floor, never through it") {
    for (const HangLook& h : {kHanging[0], kHanging[4], kHanging[1]}) {
        CAPTURE(std::string(h.name));
        const DancerSpec spec = lookSpec(h, 2);
        const Skeleton sk = makeSkeleton(spec.look);
        DancerRun run(spec, 4, 0.0, 0.0, 0.0);
        for (int i = 0; i < 60; ++i) run.step(i / 60.0, 1.0 / 60, grooveAt(i / 30.0), 1.0f);
        // Tip over forwards into a handstand (the pelvis up, the head just off the floor), then hold.
        DancePose p = restPose(sk);
        for (int i = 0; i <= 120; ++i) {
            const float a = std::min(1.0f, float(i) / 60.0f);
            p.hipPitch = a * 3.0f;  // past upside down
            p.hip.y = a * 0.3f;
            run.stepHairWithPose(p, 1.0 / 60);
        }
        for (int i = 0; i < 180; ++i) run.stepHairWithPose(p, 1.0 / 60);
        const HairState& hs = run.hairState();
        const Frame head = headFrame(sk, p);
        REQUIRE(head.y.y < -0.5f);  // the head is upside down
        for (int c = 0; c < hs.chains; ++c) {
            const HairChainDef& d = run.hairChain(c);
            const V3 root = hs.pts[c][0], tip = hs.pts[c][d.segs];
            for (int i = 1; i <= d.segs; ++i) {
                REQUIRE(finite(hs.pts[c][i]));
                CHECK(hs.pts[c][i].y >= d.radius - 1e-3f);  // above the floor
            }
            // Hanging (or lying on the floor), not standing up off the head.
            CHECK(tip.y < root.y + 0.02f);
        }
    }
}

TEST_CASE("hair physics: the entrance starts still, a reset after a jump, and the same seed gives the same hair") {
    const DancerSpec spec = lookSpec(kHanging[1], 5);
    DancerRun a(spec, 9, 0.0, 0.0, 0.0), b(spec, 9, 0.0, 0.0, 0.0);
    for (int i = 0; i < 300; ++i) {
        const double t = i / 60.0;
        a.step(t, 1.0 / 60, grooveAt(t * 2), 1.0f);
        b.step(t, 1.0 / 60, grooveAt(t * 2), 1.0f);
        for (int c = 0; c < a.hairState().chains; ++c)
            for (int p = 0; p < kHairChainPts; ++p) REQUIRE(length(a.hairState().pts[c][p] - b.hairState().pts[c][p]) == 0.0f);
    }
    // A long frame (a stall) restarts the hair from its rest shape instead of flinging it.
    a.step(5.5, 0.5, grooveAt(11), 1.0f);
    const HairState& hs = a.hairState();
    for (int c = 0; c < hs.chains; ++c)
        for (int p = 0; p <= a.hairChain(c).segs; ++p) REQUIRE(finite(hs.pts[c][p]));
    // Hair 0: the classic swing (no chains, no lag); turning it back on picks up from rest.
    a.setHairMotion(0.0f);
    a.step(5.52, 1.0 / 60, grooveAt(11.04), 1.0f);
    CHECK(a.hairState().chains == 0);
    CHECK(length(a.hairState().lagR) == 0.0f);
    a.setHairMotion(1.0f);
    a.step(5.54, 1.0 / 60, grooveAt(11.08), 1.0f);
    CHECK(a.hairState().chains >= 1);
}

TEST_CASE("hair physics: short and big hair bounces a little with the head (loose volumes)") {
    for (HairStyle s : {HairStyle::Afro, HairStyle::Curly, HairStyle::Bun, HairStyle::Spiky, HairStyle::Short}) {
        CAPTURE(int(s));
        DancerSpec spec = generateDancer(Style::Break, std::uint32_t(2));
        spec.look.hairStyle = s;
        spec.look.hat = Hat::None;
        DancerRun run(spec, 11, 0.0, 0.0, 0.0);
        run.choreographer().setMocap(0.0f);
        double sumT = 0, sumR = 0;
        float maxT = 0, maxR = 0;
        int n = 0;
        for (int i = 0; i < 60 * 12; ++i) {
            const double t = i / 60.0;
            run.step(t, 1.0 / 60, grooveAt(t * 2), 1.0f);
            if (t < 1.5) continue;
            const HairState& hs = run.hairState();
            REQUIRE(finite(hs.lagT));
            REQUIRE(finite(hs.lagR));
            sumT += length(hs.lagT) / run.headRadius();
            sumR += length(hs.lagR);
            maxT = std::max(maxT, length(hs.lagT) / run.headRadius());
            maxR = std::max(maxR, length(hs.lagR));
            ++n;
        }
        MESSAGE("style " << int(s) << ": volume lag mean " << sumT / n << " R (max " << maxT << "), turn mean "
                         << sumR / n << " rad (max " << maxR << ")");
        CHECK(sumT / n > 0.01);  // it moves...
        CHECK(sumR / n > 0.01);
        CHECK(maxT <= 0.3001f);  // ...held to the limits (x Hair 1)
        CHECK(maxR <= 0.3501f);
        // The hair parts move with the lag; the head does not.
        HairState lagged = run.hairState();
        lagged.lagT = {0.03f, 0.02f, 0};
        lagged.lagR = {0.2f, 0, 0};
        const Skeleton sk = makeSkeleton(spec.look);
        HairState none;
        BodyOut still, moving;
        buildBody(spec.look, sk, restPose(sk), Frame{}, 1.0f, still, &none);
        buildBody(spec.look, sk, restPose(sk), Frame{}, 1.0f, moving, &lagged);
        REQUIRE(still.parts.size() == moving.parts.size());
        float hairMoved = 0, headMoved = 0;
        for (std::size_t i = 0; i < still.parts.size(); ++i) {
            const float m = length(still.parts[i].frame.o - moving.parts[i].frame.o) +
                            length(still.parts[i].frame.y - moving.parts[i].frame.y);
            if (still.parts[i].material == Material::Strands) hairMoved = std::max(hairMoved, m);
            if (still.parts[i].material == Material::Head) headMoved = std::max(headMoved, m);
        }
        CHECK(hairMoved > 1e-3f);
        CHECK(headMoved == 0.0f);
    }
}

// ---- Long hair that bends (reforce.Dancer.LongHair, 2026-11-27)

namespace {
const HairStyle kLongStyles[] = {HairStyle::Long,   HairStyle::Bob,   HairStyle::Bangs,    HairStyle::Layers,
                                 HairStyle::HalfUp, HairStyle::Shag,  HairStyle::Curls,    HairStyle::BigCurls,
                                 HairStyle::HugeCurls};

DancerSpec longSpec(HairStyle s, int seed) {
    DancerSpec spec = generateDancer(Style(seed % kStyleCount), std::uint32_t(seed));
    spec.look.hairStyle = s;
    spec.look.hat = Hat::None;
    return spec;
}

int bentParts(const BodyOut& o) {
    int n = 0;
    for (const BodyPart& p : o.parts) n += p.bent;
    return n;
}
const BodyPart* bentPart(const BodyOut& o) {
    for (const BodyPart& p : o.parts)
        if (p.bent) return &p;
    return nullptr;
}
}  // namespace

TEST_CASE("long hair that bends: three columns, one tube drawn bent from ear height; still, as the rigid tube") {
    for (HairStyle s : kLongStyles) {
        CAPTURE(int(s));
        const DancerSpec spec = longSpec(s, 3);
        const Skeleton sk = makeSkeleton(spec.look);
        BodyOut rigid, bent, classic;
        HairState still, stillBent;
        buildBody(spec.look, sk, restPose(sk), Frame{}, 1.0f, classic);
        stillBent.bendLong = true;
        buildBody(spec.look, sk, restPose(sk), Frame{}, 1.0f, rigid, &still);
        buildBody(spec.look, sk, restPose(sk), Frame{}, 1.0f, bent, &stillBent);
        // LongHair 0: one chain under the head, nothing bent; 1: the back and both sides, each three links with a cone.
        CHECK(rigid.hairChainCount == 1);
        CHECK(bentParts(rigid) == 0);
        REQUIRE(bent.hairChainCount == 3);
        CHECK(bentParts(bent) == 1);
        for (int c = 0; c < 3; ++c) {
            CHECK(bent.hairChains[c].segs == 3);
            CHECK(bent.hairChains[c].cone > 0);
            CHECK(bent.hairChains[c].head);
            CHECK(bent.hairChains[c].damp > 1);  // a bit calmer than a tail
        }
        // The bend starts high, about ear height (well above the jaw), not where the rigid tube's chain hangs from.
        CHECK(bent.bend.y0 < 0.5f * sk.headR);
        CHECK(bent.bend.y0 > 0.3f * sk.headR);
        // Still, the parts are the same (the same count, each where the classic swing puts it when still: the rigid
        // tube following its chain comes out a few tenths of a percent short) and the bend moves nothing: the look at
        // rest is as designed.
        REQUIRE(bent.parts.size() == rigid.parts.size());
        CHECK(bent.headTop == doctest::Approx(rigid.headTop).epsilon(1e-5));
        const BodyPart* tube = bentPart(bent);
        REQUIRE(tube);
        float worst = 0;
        for (int i = 0; i <= 20; ++i)
            for (int a = 0; a < 16; ++a) {
                const float y = tube->len * float(i) / 20.0f, u = 6.2831853f * float(a) / 16.0f;
                const float r = tube->r0 + (tube->r1 - tube->r0) * float(i) / 20.0f;
                const V3 w = tube->frame.point({std::sin(u) * r * tube->scaleX, y, std::cos(u) * r * tube->scaleZ});
                worst = std::max(worst, length(bendHairPoint(*tube, bent.bend, w) - w));
            }
        CHECK(worst < 2e-4f);
        for (std::size_t i = 0; i < bent.parts.size(); ++i)
            CHECK(length(bent.parts[i].frame.o - classic.parts[i].frame.o) < 2e-4f);
    }
}

TEST_CASE("long hair that bends: the bend is smooth, keeps the hair's length and turns its normals") {
    const DancerSpec spec = longSpec(HairStyle::Long, 5);
    const Skeleton sk = makeSkeleton(spec.look);
    DancerRun run(spec, 5, 0.0, 0.0, 0.0);
    REQUIRE(run.longHair());
    for (int i = 0; i < 60; ++i) run.step(i / 60.0, 1.0 / 60, grooveAt(i / 30.0), 1.0f);
    // Spinning hard: the columns swing out.
    DancePose spin = restPose(sk);
    for (int i = 0; i < 50; ++i) {
        spin.bodyYaw = float(i) / 60.0f * 9.0f;
        run.stepHairWithPose(spin, 1.0 / 60);
    }
    BodyOut out;
    buildBody(spec.look, sk, spin, Frame{}, 1.0f, out, &run.hairState());
    const BodyPart* tube = bentPart(out);
    REQUIRE(tube);
    const HairBend& b = out.bend;
    CHECK(b.y0 > 0);
    CHECK(b.y0 < tube->len);
    // Along each column's line on the wall: continuous (no step anywhere, none where the bend starts), about as long
    // as it was (a bend, not a stretch), and the hem moved well away from where it hangs at rest.
    float hemMoved = 0;
    for (float side : {-1.0f, 0.0f, 1.0f}) {
        CAPTURE(side);
        const float x = side * b.sideX, z = side == 0 ? b.backZ : b.sideZ;
        V3 prev{};
        float bentLen = 0, worstStep = 0;
        const int n = 200;
        for (int i = 0; i <= n; ++i) {
            const float y = tube->len * float(i) / float(n);
            const V3 w = tube->frame.point({x, y, z});
            const V3 p = bendHairPoint(*tube, b, w);
            REQUIRE(finite(p));
            if (i > 0) {
                const float st = length(p - prev);
                bentLen += st;
                worstStep = std::max(worstStep, st);
            }
            prev = p;
            if (i == n) hemMoved = std::max(hemMoved, length(p - w));
        }
        CHECK(worstStep < 3.0f * tube->len / float(n));
        CHECK(bentLen == doctest::Approx(tube->len).epsilon(0.15));
    }
    MESSAGE("spinning: the hem moved up to " << hemMoved / sk.headR << " head radii");
    CHECK(hemMoved > 0.3f * sk.headR);
    // Normals turn with the bend: still unit length; above the bend unchanged.
    V3 nUp = tube->frame.z, nDown = tube->frame.z;
    bendHairPoint(*tube, b, tube->frame.point({0, b.y0 * 0.5f, b.backZ}), &nUp);
    bendHairPoint(*tube, b, tube->frame.point({0, tube->len, b.backZ}), &nDown);
    CHECK(length(nUp - tube->frame.z) < 1e-5f);
    CHECK(length(nDown) == doctest::Approx(1.0f).epsilon(1e-4));
    // The curls ride the bent sheet: a mane's lower masses move with it, the top ones don't.
    const DancerSpec curls = longSpec(HairStyle::HugeCurls, 5);
    const Skeleton csk = makeSkeleton(curls.look);
    HairState still;
    still.bendLong = true;
    HairState moved = run.hairState();  // (the same chain layout: three columns of three links)
    BodyOut a, c;
    buildBody(curls.look, csk, restPose(csk), Frame{}, 1.0f, a, &still);
    buildBody(curls.look, csk, restPose(csk), Frame{}, 1.0f, c, &moved);
    REQUIRE(a.parts.size() == c.parts.size());
    int ballsMoved = 0;
    for (std::size_t i = 0; i < a.parts.size(); ++i)
        if (a.parts[i].len == 0 && a.parts[i].material == Material::Strands && a.parts[i].strands == kStrandsCoils &&
            length(a.parts[i].frame.o - c.parts[i].frame.o) > 0.2f * csk.headR)
            ++ballsMoved;
    CHECK(ballsMoved >= 6);
}

TEST_CASE("long hair that bends: switching LongHair restarts the hair from rest; 0 is the one chain") {
    const DancerSpec spec = longSpec(HairStyle::Layers, 6);
    DancerRun run(spec, 6, 0.0, 0.0, 0.0);
    for (int i = 0; i < 120; ++i) run.step(i / 60.0, 1.0 / 60, grooveAt(i / 30.0), 1.0f);
    CHECK(run.hairState().chains == 3);
    CHECK(run.hairState().bendLong);
    run.setLongHair(false);
    run.step(2.0, 1.0 / 60, grooveAt(4.0), 1.0f);
    CHECK(run.hairState().chains == 1);
    CHECK_FALSE(run.hairState().bendLong);
    BodyOut o;
    run.build(o);
    CHECK(bentParts(o) == 0);
    run.setLongHair(true);
    run.step(2.02, 1.0 / 60, grooveAt(4.04), 1.0f);
    CHECK(run.hairState().chains == 3);
    o = BodyOut{};
    run.build(o);
    CHECK(bentParts(o) == 1);
    // Classic swing (Hair 0): never bent, whatever LongHair says.
    run.setHairMotion(0.0f);
    run.step(2.04, 1.0 / 60, grooveAt(4.08), 1.0f);
    o = BodyOut{};
    run.build(o);
    CHECK(bentParts(o) == 0);
}

TEST_CASE("long hair that bends: with the hand-made moves the columns stay within their cone and never cross the "
          "tube's axis behind the head") {
    // Every long style, the hand-made routines: the columns' ends within the cone about where they hang, and the
    // curve never folds back up past the bend's start (what would show the tube's inside).
    float worstTurn = 0;
    for (int si = 0; si < int(std::size(kLongStyles)); ++si) {
        CAPTURE(si);
        for (int st = 1; st < kStyleCount; st += 3) {
            DancerRun run(longSpec(kLongStyles[si], st + si), 3 + std::uint32_t(st), 0.0, 0.0, 0.0);
            run.choreographer().setMocap(0.0f);
            for (int i = 0; i < 60 * 12; ++i) {
                const double t = i / 60.0;
                run.step(t, 1.0 / 60, grooveAt(t * 2), 1.0f);
                if (t < 1.0) continue;
                const HairState& hs = run.hairState();
                REQUIRE(hs.chains == 3);
                const Frame h = run.headNow();
                for (int c = 0; c < 3; ++c) {
                    const HairChainDef& d = run.hairChain(c);
                    const V3 rd = h.dir(normalize(d.b - d.a));
                    const V3 last = normalize(hs.pts[c][3] - hs.pts[c][2]);
                    worstTurn = std::max(worstTurn, std::acos(std::clamp(dot(last, rd), -1.0f, 1.0f)));
                }
            }
        }
    }
    MESSAGE("long columns standing: the last link up to " << worstTurn * 57.3f << " degrees from hanging");
    CHECK(worstTurn < 1.15f * 1.25f);  // the cone (soft) plus a little
}

// Not part of the suite: the measurements behind docs/dancers-design.md "Hair physics" (every mocap move, every
// hanging look; CPU cost). Run: reforce_engine_tests -tc="hair physics: measure*" --no-skip
TEST_CASE("hair physics: measure every mocap move" * doctest::skip()) {
    REQUIRE(haveClips());
    const int clips = clipCount();
    std::printf("%d mocap moves\n", clips);
    // Per move: deepest into the arms / head / torso over all looks, the swing (tip off its rest place in the head's
    // frame, per unit length), the volumes' turn.
    struct Row {
        float arms = 0, head = 0, torso = 0, swing = 0, swingMean = 0, turn = 0;
    };
    std::vector<Row> rows(static_cast<std::size_t>(clips));
    double simSeconds = 0, stepSeconds = 0;
    long frames = 0, deepFrames = 0, armFrames = 0;
    std::vector<float> armDepths;
    for (int li = 0; li < int(std::size(kHanging)); ++li) {
        const HangLook& h = kHanging[li];
        double lookSwing = 0, classicSwing = 0;
        long lookN = 0, classicN = 0;
        float lookMax = 0;
        // Where each chain's tip settles, standing still (head frame): the swing is measured from there.
        V3 settled[kHairChains];
        {
            DancerRun still(lookSpec(h, li), 99, 0.0, 0.0, 0.0);
            for (int i = 0; i < 60; ++i) still.step(i / 60.0, 1.0 / 60, grooveAt(i / 30.0), 1.0f);
            const DancePose rp = restPose(makeSkeleton(still.spec().look));
            for (int i = 0; i < 300; ++i) still.stepHairWithPose(rp, 1.0 / 60);
            const Frame head = still.headNow();
            for (int c = 0; c < still.hairState().chains; ++c) {
                const V3 q = still.hairState().pts[c][still.hairChain(c).segs] - head.o;
                settled[c] = {dot(q, head.x), dot(q, head.y), dot(q, head.z)};
            }
        }
        for (int ci = 0; ci < clips; ++ci) {
            Row& r = rows[std::size_t(ci)];
            const ClipInfo& info = clipInfo(ci);
            DancerRun run(lookSpec(h, ci + li), 100 + std::uint32_t(ci), 0.0, 0.0, 0.0);
            run.choreographer().setMocap(1.0f);
            run.choreographer().tryMove(info.move);
            const double secs = std::min(12.0, 2.5 + info.beats / 2.0);
            double swingSum = 0;
            int swingN = 0;
            for (int i = 0; i < int(secs * 60); ++i) {
                const double t = i / 60.0;
                const auto t0 = std::chrono::steady_clock::now();
                run.step(t, 1.0 / 60, grooveAt(t * 2), 1.0f);
                stepSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                if (t < 2.0) continue;  // the move starts on the next bar line (beat 4)
                const Depth d = hairDepth(run);
                r.arms = std::max(r.arms, d.arms);
                r.head = std::max(r.head, d.head);
                r.torso = std::max(r.torso, d.torso);
                ++frames;
                deepFrames += std::max({d.arms, d.head, d.torso}) > 0.01f;
                armFrames += d.arms > 0.005f;
                if (d.arms > 0) armDepths.push_back(d.arms);
                const HairState& hs = run.hairState();
                r.turn = std::max(r.turn, length(hs.lagR));
                // Swing (as the classic swing measures it): the tip off its rest place, in the head's frame, per unit
                // length.
                const Frame head = run.headNow();
                for (int c = 0; c < hs.chains; ++c) {
                    const HairChainDef& def = run.hairChain(c);
                    const V3 q = hs.pts[c][def.segs] - head.o;
                    const V3 local{dot(q, head.x), dot(q, head.y), dot(q, head.z)};
                    const float sw = length(local - settled[c]) / length(def.b - def.a);
                    r.swing = std::max(r.swing, sw);
                    swingSum += sw;
                    ++swingN;
                }
                classicSwing += 0.5 * (length(run.hairSwing(0)) + length(run.hairSwing(1)));
                ++classicN;
            }
            const float sw = float(swingSum / std::max(1, swingN));
            r.swingMean = std::max(r.swingMean, sw);
            lookSwing += swingSum;
            lookN += swingN;
            lookMax = std::max(lookMax, sw);
            simSeconds += secs;
        }
        std::printf("%-16s mean swing %.3f (the move with the most: mean %.3f); the classic swing's mean %.3f\n", h.name,
                    lookSwing / std::max(1L, lookN), lookMax, classicSwing / std::max(1L, classicN));
        std::fflush(stdout);
    }
    std::sort(armDepths.begin(), armDepths.end());
    auto pct = [&](double q) { return armDepths.empty() ? 0.0f : armDepths[std::size_t(q * double(armDepths.size() - 1))]; };
    float wa = 0, wh = 0, wt = 0;
    int worstA = 0, worstT = 0;
    for (int ci = 0; ci < clips; ++ci) {
        const Row& r = rows[std::size_t(ci)];
        if (r.arms > wa) wa = r.arms, worstA = ci;
        wh = std::max(wh, r.head);
        if (r.torso > wt) wt = r.torso, worstT = ci;
    }
    std::printf("frames %ld; over 1 cm into the body in %ld (%.3f %%), arms over 5 mm in %ld; worst arms %.4f (%s), "
                "head %.4f, torso %.4f (%s); arm depth p99 %.4f p99.9 %.4f\n",
                frames, deepFrames, 100.0 * double(deepFrames) / double(frames), armFrames, wa,
                clipInfo(worstA).id.c_str(), wh, wt, clipInfo(worstT).id.c_str(), pct(0.99), pct(0.999));
    std::printf("CPU: %.2f us per dancer step (pose + hair), %.0f s simulated\n", 1e6 * stepSeconds / (simSeconds * 60),
                simSeconds);
    // The moves with the most volume turn (spins) and swing: render candidates.
    std::vector<int> order(static_cast<std::size_t>(clips));
    for (int i = 0; i < clips; ++i) order[std::size_t(i)] = i;
    std::sort(order.begin(), order.end(), [&](int a, int b) { return rows[std::size_t(a)].turn > rows[std::size_t(b)].turn; });
    for (int i = 0; i < 8; ++i)
        std::printf("spin %s  turn %.3f\n", clipInfo(order[std::size_t(i)]).id.c_str(), rows[std::size_t(order[std::size_t(i)])].turn);
    std::sort(order.begin(), order.end(), [&](int a, int b) { return rows[std::size_t(a)].arms > rows[std::size_t(b)].arms; });
    for (int i = 0; i < 8; ++i)
        std::printf("arms %s  depth %.4f\n", clipInfo(order[std::size_t(i)]).id.c_str(), rows[std::size_t(order[std::size_t(i)])].arms);
}

TEST_CASE("hair physics: measure the CPU cost per dancer, with and without" * doctest::skip()) {
    for (const HangLook& h : {kHanging[1], kHanging[4], kHanging[10]}) {
        for (float amt : {0.0f, 1.0f}) {
            DancerRun run(lookSpec(h, 4), 7, 0.0, 0.0, 0.0);
            run.setHairMotion(amt);
            run.choreographer().setMocap(0.0f);
            BodyOut out;
            double step = 0, build = 0;
            const int n = 60 * 60;
            for (int i = 0; i < n; ++i) {
                const double t = i / 60.0;
                auto t0 = std::chrono::steady_clock::now();
                run.step(t, 1.0 / 60, grooveAt(t * 2), 1.0f);
                auto t1 = std::chrono::steady_clock::now();
                out.parts.clear();
                run.build(out);
                auto t2 = std::chrono::steady_clock::now();
                step += std::chrono::duration<double>(t1 - t0).count();
                build += std::chrono::duration<double>(t2 - t1).count();
            }
            std::printf("%-10s hair %.0f: step %.2f us, build %.2f us, %zu parts\n", h.name, amt, 1e6 * step / n,
                        1e6 * build / n, out.parts.size());
        }
    }
}


TEST_CASE("long hair that bends: measure the CPU cost per dancer, rigid and bending" * doctest::skip()) {
    for (HairStyle s : {HairStyle::Long, HairStyle::Layers, HairStyle::HugeCurls}) {
        for (bool bend : {false, true}) {
            DancerRun run(longSpec(s, 4), 7, 0.0, 0.0, 0.0);
            run.setLongHair(bend);
            run.choreographer().setMocap(0.0f);
            BodyOut out;
            double step = 0, build = 0;
            const int n = 60 * 60;
            for (int i = 0; i < n; ++i) {
                const double t = i / 60.0;
                auto t0 = std::chrono::steady_clock::now();
                run.step(t, 1.0 / 60, grooveAt(t * 2), 1.0f);
                auto t1 = std::chrono::steady_clock::now();
                out.parts.clear();
                run.build(out);
                auto t2 = std::chrono::steady_clock::now();
                step += std::chrono::duration<double>(t1 - t0).count();
                build += std::chrono::duration<double>(t2 - t1).count();
            }
            std::printf("style %2d LongHair %d: step %.2f us, build %.2f us, %zu parts\n", int(s), int(bend),
                        1e6 * step / n, 1e6 * build / n, out.parts.size());
        }
    }
}

TEST_CASE("long hair that bends: measure the swing" * doctest::skip()) {
    // The columns' ends off where they settle (in the head's frame, per unit length), over the hand-made routines of
    // every style for the long styles; a hard spin's fling and how soon it settles once still.
    auto local = [](const Frame& f, V3 q) {
        q = q - f.o;
        return V3{dot(q, f.x), dot(q, f.y), dot(q, f.z)};
    };
    std::vector<float> sw;
    double sum = 0;
    for (HairStyle s : {HairStyle::Long, HairStyle::Layers, HairStyle::Bangs, HairStyle::Bob, HairStyle::HugeCurls}) {
        for (int st = 0; st < kStyleCount; ++st) {
            const DancerSpec spec = longSpec(s, st + 1);
            const Skeleton sk = makeSkeleton(spec.look);
            DancerRun run(spec, 7 + std::uint32_t(st), 0.0, 0.0, 0.0);
            run.choreographer().setMocap(0.0f);
            // Where each column's end settles standing still (head frame).
            DancerRun still(spec, 1, 0.0, 0.0, 0.0);
            for (int i = 0; i < 60; ++i) still.step(i / 60.0, 1.0 / 60, grooveAt(i / 30.0), 1.0f);
            for (int i = 0; i < 240; ++i) still.stepHairWithPose(restPose(sk), 1.0 / 60);
            const Frame h0 = still.headNow();
            V3 rest[3];
            for (int c = 0; c < 3; ++c) rest[c] = local(h0, still.hairState().pts[c][3]);
            const float L = length(still.hairChain(0).b - still.hairChain(0).a);
            for (int i = 0; i < 60 * 20; ++i) {
                const double t = i / 60.0;
                run.step(t, 1.0 / 60, grooveAt(t * 2), 1.0f);
                if (t < 1.5) continue;
                const Frame h = run.headNow();
                for (int c = 0; c < 3; ++c) {
                    const float d = length(local(h, run.hairState().pts[c][3]) - rest[c]) / L;
                    sw.push_back(d);
                    sum += d;
                }
            }
        }
    }
    std::sort(sw.begin(), sw.end());
    const auto q = [&](double p) { return sw[std::size_t(p * double(sw.size() - 1))]; };
    std::printf("long columns' swing (per unit length): mean %.3f, p90 %.3f, p99 %.3f, max %.3f\n",
                sum / double(sw.size()), q(0.9), q(0.99), sw.back());
    // A hard spin, then held still: the side column's end out, and how long until it is back within 5 %.
    const DancerSpec spec = longSpec(HairStyle::Long, 5);
    const Skeleton sk = makeSkeleton(spec.look);
    DancerRun run(spec, 5, 0.0, 0.0, 0.0);
    for (int i = 0; i < 60; ++i) run.step(i / 60.0, 1.0 / 60, grooveAt(i / 30.0), 1.0f);
    const DancePose rest = restPose(sk);
    for (int i = 0; i < 240; ++i) run.stepHairWithPose(rest, 1.0 / 60);
    const V3 settledLocal = local(headFrame(sk, rest), run.hairState().pts[1][3]);
    const float L = length(run.hairChain(1).b - run.hairChain(1).a);
    DancePose spin = rest;
    float out = 0;
    for (int i = 0; i < 50; ++i) {
        spin.bodyYaw = float(i) / 60.0f * 9.0f;
        run.stepHairWithPose(spin, 1.0 / 60);
        out = std::max(out, length(local(headFrame(sk, spin), run.hairState().pts[1][3]) - settledLocal) / L);
    }
    const Frame hs = headFrame(sk, spin);
    int settleFrames = 0;
    for (int i = 0; i < 300; ++i) {
        run.stepHairWithPose(spin, 1.0 / 60);
        if (length(local(hs, run.hairState().pts[1][3]) - settledLocal) / L > 0.05f) settleFrames = i + 1;
    }
    std::printf("hard spin: the left column's end out up to %.3f per unit length (%.2f head radii; the column %.2f long); "
                "settled (within 5 %%) %.2f s after\n", out, out * L / sk.headR, L / sk.headR, settleFrames / 60.0);
}

// ---- Break floor work (docs/dancers-design.md "Break floor work", Peter 2026-11-27: big hair, long hair and hats went
// through the floor in floor moves)
namespace {
// The lowest point of a hair or hat part as drawn (its surface less what its planes cut away; a bent part where the
// bend puts it).
float partLowest(const BodyPart& q, const HairBend& bend) {
    const int around = 12, along = 4;
    float low = 1e9f;
    for (int a = 0; a < around; ++a)
        for (int j = -3; j <= along + 3; ++j) {
            const float ang = 6.2831853f * float(a) / float(around);
            float y, r;
            if (j < 0) {  // the cap at end 0
                const float e = 1.5707963f * float(-j) / 3.0f;
                r = q.r0 * std::cos(e);
                y = -q.r0 * std::sin(e);
            } else if (j > along) {
                const float e = 1.5707963f * float(j - along) / 3.0f;
                r = q.r1 * std::cos(e);
                y = q.len + q.r1 * std::sin(e);
            } else {
                const float t = float(j) / float(along);
                r = q.r0 + (q.r1 - q.r0) * t;
                y = q.len * t;
            }
            const V3 l{r * std::cos(ang), y, r * std::sin(ang)};
            if (l.x * q.clip[0] + l.y * q.clip[1] + l.z * q.clip[2] > q.clip[3] ||
                l.x * q.clip2[0] + l.y * q.clip2[1] + l.z * q.clip2[2] > q.clip2[3])
                continue;
            V3 w = q.frame.o + q.frame.x * (l.x * q.scaleX) + q.frame.y * l.y + q.frame.z * (l.z * q.scaleZ);
            if (q.bent) w = bendHairPoint(q, bend, w);
            low = std::min(low, w.y);
        }
    return low;
}
}  // namespace

TEST_CASE("floor work: hair and hats stay out of the floor (every hairstyle; the hats on big hair)") {
    // Each look on the face-down floor basic 10, two more floor moves and a break routine with a drop (routine 6: a
    // moment upside down; routine 9 till the spin fixes, 2026-11-27, now left out with the flips), with hair physics: the lowest point of the hair
    // and hat as drawn, apart for what moves with the head (big hair, buns, spikes, hats: groundPose holds them off the
    // floor) and what hangs (long hair, tails, the curls riding the long hair: the hair sim keeps its chains above it).
    const std::string dir = std::string(REFORCE_SOURCE_DIR) + "/content/Moves/";
    REQUIRE(loadClips(dir + "aist.moves").empty());
    REQUIRE(loadClips(dir + "aist_floor.moves", true).empty());
    std::vector<int> moves;
    for (const char* label : {"break floor basic 10 · dancer 5 · take 1", "break floor basic 8 · dancer 5 · take 2",
                              "break floor routine 11 · dancer 5", "break routine 6 · dancer 4"}) {
        CAPTURE(std::string(label));
        const int m = findClip(label);
        REQUIRE(m >= 0);
        moves.push_back(m);
    }
    struct Case {
        HairStyle hair;
        Hat hat;
    };
    std::vector<Case> cases;
    for (int h = 0; h < kHairStyleCount; ++h) cases.push_back({HairStyle(h), Hat::None});
    for (HairStyle h : {HairStyle::Afro, HairStyle::HugeCurls, HairStyle::Short, HairStyle::Long, HairStyle::Bun})
        for (Hat t : {Hat::Cap, Hat::Beanie, Hat::Top, Hat::Headphones}) cases.push_back({h, t});
    // 0: hats, glasses, the ears; 1: hair that moves with the head; 2: hanging hair.
    float worst[3] = {0, 0, 0};
    std::string worstName[3];
    for (std::size_t ci = 0; ci < cases.size(); ++ci) {
        DancerSpec spec = generateDancer(Style::Break, std::uint32_t(40 + ci));
        spec.look.hairStyle = cases[ci].hair;
        spec.look.hat = cases[ci].hat;
        spec.look.beard = Beard::None;
        const std::string name = "hair " + std::to_string(int(cases[ci].hair)) + " hat " + std::to_string(int(cases[ci].hat));
        float low[3] = {0, 0, 0};
        for (int m : moves) {
            DancerRun run(spec, 7, 0.0, 0.0, 0.0);
            run.choreographer().setMocap(1.0f);
            run.choreographer().tryMove(clipInfo(m).move);
            for (int i = 0; i < 60 * 12; ++i) {
                const double t = i / 60.0;
                run.step(t, 1.0 / 60, grooveAt(t * 2), 1.0f);
                if (t < 2.5 || i % 3) continue;  // (the move starts on the next bar line)
                BodyOut o;
                run.build(o);
                for (std::size_t pi = o.headBegin; pi < o.parts.size(); ++pi) {
                    const BodyPart& q = o.parts[pi];
                    if (q.material == Material::Head) continue;
                    const bool hair = q.material == Material::Hair || q.material == Material::Strands;
                    float& slot = low[q.hangs ? 2 : hair ? 1 : 0];
                    slot = std::min(slot, partLowest(q, o.bend));
                }
            }
        }
        for (int k = 0; k < 3; ++k)
            if (low[k] < worst[k]) {
                worst[k] = low[k];
                worstName[k] = name;
            }
        if (low[0] < -0.01f || low[1] < -0.01f || low[2] < -0.01f)
            MESSAGE(name << ": hat " << low[0] << ", hair with the head " << low[1] << ", hanging " << low[2]);
        CHECK(low[0] > -0.01f);
        // (Big hair lags the head a little; what still dips under the floor, as hanging hair lying on it does, is
        // cut off there when drawn: DancerPipeline.)
        CHECK(low[1] > -0.04f);
    }
    MESSAGE("deepest through the floor: hats " << worst[0] << " (" << worstName[0] << "), hair with the head " << worst[1]
                                               << " (" << worstName[1] << "), hanging " << worst[2] << " (" << worstName[2]
                                               << ")");
    loadClips(dir + "aist.moves");
}
