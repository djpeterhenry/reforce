// Measurements only (skipped by default): do dancers slide on the floor during break floor moves, and why? And how
// often do neighbors overlap a dancer on the floor? docs/dancers-design.md "Break floor work", "Mingle".
// Run: reforce_engine_tests -tc="floor slide: measure*" --no-skip
//
// Every floor-contact point (hand balls, elbows, knees, feet, head, hips) while it touches the floor (its lowest point
// within 1 cm = 0.01 units; a dancer is about 1 unit tall) is followed frame to frame in stage space. Its world
// position is p = S + k (B + R(yaw) L): S the dancer's spot (x_, z_), B the move's root travel (bodyX, bodyZ), yaw
// the body heading (the move's own plus the stage's facing), L the contact in the body frame. A frame's slip
//   p1 - p0 = (S1 - S0) + k (B1 - B0) + k (R1 - R0) L0 + k R1 (L1 - L0)
// splits exactly into (a) spot, (b) root travel, (c) heading, (d) pose.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "engine/DanceClips.hpp"
#include "engine/DanceMoves.hpp"
#include "engine/DancerRig.hpp"
#include "engine/Dancers.hpp"

using namespace reforce::engine;
using dance::V3;

namespace {

const std::string kMovesFile = std::string(REFORCE_SOURCE_DIR) + "/content/Moves/aist.moves";
const std::string kFloorFile = std::string(REFORCE_SOURCE_DIR) + "/content/Moves/aist_floor.moves";

struct FloorLoaded {
    FloorLoaded() {
        REQUIRE(dance::loadClips(kMovesFile).empty());
        REQUIRE(dance::loadClips(kFloorFile, true).empty());
    }
    ~FloorLoaded() {
        dance::setFloorWeights(0);
        dance::loadClips(kMovesFile);
    }
};

constexpr double kBpm = 124, kFps = 60;

// A track at 124 BPM: 64-bar cycles, 48 bars of groove, an 8-bar breakdown (build 0 -> 1), a drop on bar 56 (decaying
// over 2 beats), 8 more bars of groove.
dance::MusicState musicAt(double beat) {
    dance::MusicState g;
    g.beat = beat;
    g.bar = beat / 4;
    g.phrase = beat / 64;
    const double inCycle = std::fmod(beat, 256.0);
    g.bass = 0.6;
    if (inCycle >= 192 && inCycle < 224) {
        g.build = (inCycle - 192) / 32.0;
        g.bass = 0.15;
    }
    if (inCycle >= 224) g.drop = std::exp(-(inCycle - 224) / 2.0);
    return g;
}

// ---- contact points (body frame: the pose with bodyX / bodyZ / bodyYaw zeroed)
enum Contact { kHandL, kHandR, kElbowL, kElbowR, kKneeL, kKneeR, kFootL, kFootR, kHead, kHips, kContacts };
const char* kContactName[kContacts] = {"hand", "hand", "elbow", "elbow", "knee", "knee", "foot", "foot", "head", "hips"};
int contactKind(int c) { return c == kHandR ? kHandL : c == kElbowR ? kElbowL : c == kKneeR ? kKneeL : c == kFootR ? kFootL : c; }

dance::Frame rotatedQ(const dance::Frame& f, float x, float y, float z, float w) {
    const float n = std::sqrt(x * x + y * y + z * z + w * w);
    if (n < 1e-6f) return f;
    x /= n, y /= n, z /= n, w /= n;
    dance::Frame r = f;
    r.x = f.dir({1 - 2 * (y * y + z * z), 2 * (x * y + w * z), 2 * (x * z - w * y)});
    r.y = f.dir({2 * (x * y - w * z), 1 - 2 * (x * x + z * z), 2 * (y * z + w * x)});
    r.z = f.dir({2 * (x * z + w * y), 2 * (y * z - w * x), 1 - 2 * (x * x + y * y)});
    return r;
}
V3 hintedPole(V3 own, const dance::Frame& frame, V3 hint) {
    const float w = std::min(1.0f, dance::length(hint));
    if (w < 1e-4f) return own;
    return dance::normalize(own) * (1 - w) + frame.dir(dance::normalize(hint)) * w;
}

struct Contacts {
    V3 p[kContacts];    // body frame (character units)
    float low[kContacts];  // lowest point above the floor
};
Contacts contactsOf(const dance::Look& look, const dance::Skeleton& sk, dance::DancePose pose) {
    pose.bodyX = pose.bodyZ = pose.bodyYaw = 0;
    Contacts c;
    dance::HairCollider col[dance::kHairColliders];
    dance::hairColliders(look, sk, pose, col);
    for (int s = 0; s < 2; ++s) {
        const auto& hand = col[dance::kColArms + 3 * s + 2];
        c.p[kHandL + s] = hand.a;
        c.low[kHandL + s] = hand.a.y - 0.05f;
        const auto& fore = col[dance::kColArms + 3 * s + 1];
        c.p[kElbowL + s] = fore.a;
        c.low[kElbowL + s] = fore.a.y - 0.045f;
    }
    c.p[kHead] = col[dance::kColHead].a;
    c.low[kHead] = c.p[kHead].y - sk.headR;
    // The pelvis and knees as the rig places them.
    dance::Frame pelvis = dance::Frame{}.translated(V3{0, sk.hipY, 0} + pose.hip);
    if (pose.hipQx != 0 || pose.hipQy != 0 || pose.hipQz != 0)
        pelvis = rotatedQ(pelvis, pose.hipQx, pose.hipQy, pose.hipQz, pose.hipQw);
    pelvis = pelvis.rotatedY(pose.hipYaw).rotatedZ(pose.hipRoll).rotatedX(pose.hipPitch);
    c.p[kHips] = pelvis.o;
    c.low[kHips] = pelvis.o.y - 0.1f;
    const dance::Frame body{};
    for (int s = 0; s < 2; ++s) {
        const float sx = s == 0 ? 1.0f : -1.0f;
        const V3 hipJ = pelvis.point({sx * sk.hipHalf, -0.01f, 0});
        const V3 kpole = hintedPole(body.dir({sx * 0.3f, 0, 1}), body, s == 0 ? pose.kneeL : pose.kneeR);
        const V3 foot = s == 0 ? pose.footL : pose.footR;
        const dance::IkResult leg = dance::solveTwoBone(hipJ, foot + V3{0, sk.ankleY, 0}, sk.thigh, sk.shin, kpole);
        c.p[kKneeL + s] = leg.mid;
        c.low[kKneeL + s] = leg.mid.y - 0.05f;
        c.p[kFootL + s] = foot;
        c.low[kFootL + s] = foot.y;
    }
    return c;
}

// Horizontal (x, z) of a body-frame point turned by yaw (bodyOf: local x -> (cos, -sin), local z -> (sin, cos)).
struct H {
    float x = 0, z = 0;
};
H turn(V3 p, float yaw) {
    const float c = std::cos(yaw), s = std::sin(yaw);
    return {p.x * c + p.z * s, -p.x * s + p.z * c};
}
float len(H a) { return std::hypot(a.x, a.z); }
H operator-(H a, H b) { return {a.x - b.x, a.z - b.z}; }
H operator+(H a, H b) { return {a.x + b.x, a.z + b.z}; }
H operator*(H a, float k) { return {a.x * k, a.z * k}; }

// ---- statistics
struct Dist {
    std::vector<float> v;
    void add(float x) { v.push_back(x); }
    float mean() const {
        double s = 0;
        for (float x : v) s += x;
        return v.empty() ? 0.0f : float(s / double(v.size()));
    }
    float pct(double p) {
        if (v.empty()) return 0;
        std::vector<float> w = v;
        const std::size_t i = std::min(w.size() - 1, std::size_t(p * double(w.size())));
        std::nth_element(w.begin(), w.begin() + std::ptrdiff_t(i), w.end());
        return w[i];
    }
    float max() const { return v.empty() ? 0.0f : *std::max_element(v.begin(), v.end()); }
    float share(float above) const {
        if (v.empty()) return 0;
        std::size_t n = 0;
        for (float x : v) n += x > above;
        return float(n) / float(v.size());
    }
};
// Per-frame slip (units / frame) per cause.
enum Cause {
    kTotal, kSpot, kSpotKeepApart, kRoot, kHeading, kHeadingFacing, kPose, kIntrinsic, kRuntimePose, kRuntimeRoot,
    kNoSpot, kNoFacing, kNoRuntimePose, kRuntimeOnly, kSpotFacing, kCauses
};
const char* kCauseName[kCauses] = {"total", "(a) spot", "(a') of which keepApart", "(b) root travel", "(c) heading",
                                   "(c') of which stage facing", "(d) pose", "move alone (as retargeted)",
                                   "runtime pose layers", "runtime root layers", "if: spot still", "if: no stage turn",
                                   "if: no runtime pose layers", "if: the move alone perfect", "spot + stage turn only"};
struct Stats {
    Dist d[kCauses];
    long frames = 0;
};

// One contact episode (a contact on the floor, frame after frame): how far it slid and by what.
struct Episode {
    float dist[kCauses] = {};  // path length per cause
    H net[kCauses];            // net displacement per cause
    int frames = 0, held = 0;
    std::string move, role, name, contact;
    float spotSpeedMax = 0;
};

// ---- a crowd stepped as the Visualizer does (VisualizerDancers.cpp: mingle plan, avoidEachOther, step, keepApart)
struct Scenario {
    std::string name;
    int dancers = 5;
    int breakers = 2;            // break dancers (the rest: other styles)
    double seconds = 600;
    bool mingle = true;          // false: fixed spots, facing the audience (no crowd, no mingle)
    float footwork = 2, power = 1;
    bool tryAll = false;         // every dancer tries every floor move in turn (16 bars each), as `beatset dancetry`
    bool overlap = false;        // capsule overlap of neighbors with a floor dancer (Part 3)
    std::uint32_t seed = 1;
    const char* fixes = nullptr;  // as SLIDE_FIXES (nullptr: the environment's, else all)
};

struct Overlap {
    long frames = 0, overlapFrames = 0;  // floor-dancer frames sampled / with another dancer's body inside theirs
    float maxDepth = 0;
    double depthSum = 0;
    long pairsNear = 0;  // neighbor pairs within 1.2 units sampled
};

// Closest distance between segments p0-p1 and q0-q1, and the params there.
float segSeg(V3 p0, V3 p1, V3 q0, V3 q1, float& s, float& t) {
    const V3 d1 = p1 - p0, d2 = q1 - q0, r = p0 - q0;
    const float a = dance::dot(d1, d1), e = dance::dot(d2, d2), f = dance::dot(d2, r);
    if (a <= 1e-9f && e <= 1e-9f) {
        s = t = 0;
        return dance::length(p0 - q0);
    }
    if (a <= 1e-9f) {
        s = 0;
        t = std::clamp(f / e, 0.0f, 1.0f);
    } else {
        const float c = dance::dot(d1, r);
        if (e <= 1e-9f) {
            t = 0;
            s = std::clamp(-c / a, 0.0f, 1.0f);
        } else {
            const float b = dance::dot(d1, d2), den = a * e - b * b;
            s = den > 1e-9f ? std::clamp((b * f - c * e) / den, 0.0f, 1.0f) : 0.0f;
            t = (b * s + f) / e;
            if (t < 0) {
                t = 0;
                s = std::clamp(-c / a, 0.0f, 1.0f);
            } else if (t > 1) {
                t = 1;
                s = std::clamp((b - c) / a, 0.0f, 1.0f);
            }
        }
    }
    return dance::length((p0 + d1 * s) - (q0 + d2 * t));
}
bool bodyPart(const dance::BodyPart& p) {
    return p.material == dance::Material::Plain || p.material == dance::Material::Head;
}
// How deep the two bodies' capsules go into each other (> 0: overlapping), hair, hats, skirts and lenses left out.
float bodyOverlap(const dance::BodyOut& A, const dance::BodyOut& B) {
    float deepest = -1e9f;
    for (const auto& a : A.parts) {
        if (!bodyPart(a)) continue;
        const V3 a0 = a.frame.o, a1 = a.frame.point({0, a.len, 0});
        const float sa = std::min({1.0f, a.scaleX, a.scaleZ});
        for (const auto& b : B.parts) {
            if (!bodyPart(b)) continue;
            const V3 b0 = b.frame.o, b1 = b.frame.point({0, b.len, 0});
            const float sb = std::min({1.0f, b.scaleX, b.scaleZ});
            float s, t;
            const float d = segSeg(a0, a1, b0, b1, s, t);
            const float ra = (a.r0 + (a.r1 - a.r0) * s) * sa, rb = (b.r0 + (b.r1 - b.r0) * t) * sb;
            deepest = std::max(deepest, ra + rb - d);
        }
    }
    return deepest;
}

struct Result {
    std::map<std::string, Stats> cat;            // "floor hand", "standing feet in place", ...
    std::map<std::string, double> spotByRole;    // floor contacts: spot slip path length by the dancer's role
    std::map<std::string, double> spotByRoleT;   // ... and contact time (s)
    std::map<std::string, double> heldByRole, heldByRoleT;  // the same for contacts the move holds still
    std::vector<Episode> worst;
    std::map<std::string, Overlap> overlap;      // per floor move label
    Overlap standingOverlap;                     // standing dancers, for comparison
    long floorFrames = 0, floorFramesMoving = 0;  // dancer frames on the floor; with the spot moving > 0.05 units/s
    Dist pop;                                     // floor frames: the fastest hand, foot or head, units / frame
    std::string popWorst;
    dance::Choreographer::FloorCounts counts;     // floor moves started at once / after waiting / given up
    long waitFrames = 0;                          // dancer frames with a floor move waiting for room
    Dist floorSpot;                               // spot speed (units / s) in those frames
    std::map<std::string, Dist> floorSpotByRole;
};

void keepWorst(std::vector<Episode>& w, const Episode& e) {
    if (e.frames < 6 || e.held * 10 < e.frames * 7) return;  // mostly held still by the move
    w.push_back(e);
    std::sort(w.begin(), w.end(), [](const Episode& a, const Episode& b) { return len(a.net[kTotal]) > len(b.net[kTotal]); });
    if (w.size() > 12) w.resize(12);
}

std::string moveLabel(const dance::Choreographer& ch, double beat, int* tier) {
    *tier = 0;
    const dance::Routine* r = ch.routineAt(beat);
    if (!r || r->full < 0) return "procedural";
    const auto& m = dance::moveLibrary()[std::size_t(r->full)];
    if (m.clip < 0) return m.name;
    *tier = dance::clipInfo(m.clip).tier;
    return dance::clipInfo(m.clip).label;
}

// Which fixes run (SLIDE_FIXES: a comma list of hold, fade, pins, room; default all; "none": as before them).
bool fixOn(const Scenario& sc, const char* name) {
    const char* e = sc.fixes ? sc.fixes : std::getenv("SLIDE_FIXES");
    if (!e) return true;
    return std::string(",") .append(e).append(",").find(std::string(",") + name + ",") != std::string::npos;
}

Result runScenario(const Scenario& sc) {
    dance::setFloorWeights(sc.footwork);
    floorFixes.hold = fixOn(sc, "hold");
    floorFixes.fade = fixOn(sc, "fade");
    floorFixes.pins = fixOn(sc, "pins");
    const bool room = fixOn(sc, "room");
    const StageCamera cam = stageCamera(16.0 / 9.0, 0.42);
    const int n = sc.dancers;
    const std::size_t nn = std::size_t(n);
    std::vector<std::unique_ptr<DancerRun>> runs;
    std::vector<dance::Skeleton> sks;
    const dance::Style others[] = {dance::Style::House, dance::Style::Pop, dance::Style::Lock, dance::Style::HipHop,
                                   dance::Style::Disco, dance::Style::Stomp, dance::Style::Jazz};
    for (int k = 0; k < n; ++k) {
        const dance::Style st = k < sc.breakers ? dance::Style::Break : others[std::size_t(k) % 7];
        runs.push_back(std::make_unique<DancerRun>(generateDancer(st, sc.seed * 101 + std::uint32_t(k)),
                                                   sc.seed * 977 + 70 + std::uint32_t(k), 0.0, 0.0, 0.0));
        sks.push_back(dance::makeSkeleton(runs.back()->spec().look));
        runs.back()->setHairMotion(0);  // the hair doesn't move the body; faster
        runs.back()->choreographer().setMocap(1);
    }
    // Floor moves (for trying them all).
    std::vector<int> floorMoves;
    for (int i = 0; i < dance::clipCount(); ++i)
        if (dance::clipInfo(i).tier != dance::kStanding) floorMoves.push_back(dance::clipInfo(i).move);
    // Fixed spots (no mingle): a line across the front.
    const auto slots = stageSlots(n, cam);

    Mingle mingle;
    Result res;
    struct Prev {
        bool have = false;
        float sx = 0, sz = 0, k = 1, facing = 0;
        V3 B;
        float yaw = 0;
        Contacts L;
        V3 Bi;
        float yawi = 0;
        Contacts Li;
        bool floor = false;
        Episode ep[kContacts];
        bool in[kContacts] = {};
    };
    std::vector<Prev> prev(nn);
    std::FILE* dump = std::getenv("SLIDE_DUMP") ? std::fopen(std::getenv("SLIDE_DUMP"), "w") : nullptr;
    if (dump) std::fprintf(dump, "t,beat,dancer,role,move,ground,sx,sz,rx,rz,hlx,hlz,hll,hrx,hrz,hrl,flx,flz,fll,frx,frz,frl,klx,klz,kll,krx,krz,krl,hx,hz,hl\n");
    std::vector<float> preKeepX(nn), preKeepZ(nn);
    const double dt = 1.0 / kFps;
    const long frames = long(sc.seconds * kFps);
    for (long i = 0; i < frames; ++i) {
        const double t = double(i) * dt;
        const double beat = t * kBpm / 60.0;
        const dance::MusicState m = musicAt(beat);
        if (sc.tryAll) {
            const long slot = long(std::floor(beat / 64.0));  // 16 bars each
            const int mv = floorMoves[std::size_t(slot) % floorMoves.size()];
            for (auto& r : runs) r->choreographer().tryMove(mv);
        }
        std::vector<StageSpot> spots(nn);
        if (sc.mingle) {
            std::vector<MinglePerson> people;
            for (int k = 0; k < n; ++k) {
                const auto& r = *runs[std::size_t(k)];
                MinglePerson p{std::uint64_t(r.seed()), r.x(), r.z(), r.facing(), r.placed()};
                p.style = int(r.spec().style);
                p.bustMove = r.inBustMove(beat);
                if (room) r.floorInfo(p, beat);
                people.push_back(p);
            }
            spots = mingle.plan(people, cam, beat, dt, sc.seed ^ 0x3141u);
        } else {
            for (int k = 0; k < n; ++k) {
                spots[std::size_t(k)].x = slots[std::size_t(k)].first;
                spots[std::size_t(k)].z = slots[std::size_t(k)].second;
            }
        }
        std::vector<DancerRun*> crowd;
        for (auto& r : runs) crowd.push_back(r.get());
        avoidEachOther(crowd);
        std::vector<DanceDirection> dirs(nn);
        for (int k = 0; k < n; ++k) {
            auto& run = *runs[std::size_t(k)];
            const StageSpot& sp = spots[std::size_t(k)];
            run.setSlot(sp.x, sp.z);
            DanceDirection& dir = dirs[std::size_t(k)];
            if (sp.leader >= 0 && sp.leader != k) dir.leader = &runs[std::size_t(sp.leader)]->choreographer();
            dir.mirror = sp.mirror;
            dir.react = sp.react;
            dir.facing = sp.facing;
            dir.look = sp.look;
            if (room) dir.floorRoom = sp.floorRoom;
            run.step(t, dt, m, std::clamp(sp.effort, 0.3f, 1.0f), dir);
            preKeepX[std::size_t(k)] = run.x();
            preKeepZ[std::size_t(k)] = run.z();
        }
        keepApart(crowd);

        // Overlap (Part 3): every 3rd frame, floor dancers against everyone near.
        std::vector<dance::BodyOut> outs;
        if (sc.overlap && i % 3 == 0 && t > 5) {
            outs.resize(nn);
            for (int k = 0; k < n; ++k) runs[std::size_t(k)]->build(outs[std::size_t(k)]);
            for (int a = 0; a < n; ++a) {
                auto& A = *runs[std::size_t(a)];
                if (A.scale() < 0.95f) continue;
                const bool floorA = A.poseNow().ground >= 0.5f;
                int tier = 0;
                const int lead = spots[std::size_t(a)].leader;
                std::string label;
                if (floorA)
                    label = lead >= 0 && lead != a && !spots[std::size_t(a)].react
                                ? moveLabel(runs[std::size_t(lead)]->choreographer(), A.danceBeat(beat), &tier) + " (led)"
                                : moveLabel(A.choreographer(), A.danceBeat(beat), &tier);
                float deepest = -1e9f;
                for (int b = 0; b < n; ++b) {
                    if (b == a) continue;
                    const auto& B = *runs[std::size_t(b)];
                    if (std::hypot(B.x() - A.x(), B.z() - A.z()) > 1.4f) continue;
                    if (!floorA && B.poseNow().ground >= 0.5f) continue;  // (counted from the floor dancer's side)
                    deepest = std::max(deepest, bodyOverlap(outs[std::size_t(a)], outs[std::size_t(b)]));
                }
                {
                    Overlap& o = floorA ? res.overlap[label] : res.standingOverlap;
                    ++o.frames;
                    if (deepest > 0) {
                        ++o.overlapFrames;
                        o.depthSum += deepest;
                        o.maxDepth = std::max(o.maxDepth, deepest);
                    }
                }
            }
        }

        // Slip.
        for (int k = 0; k < n; ++k) {
            const auto& run = *runs[std::size_t(k)];
            Prev& pv = prev[std::size_t(k)];
            const dance::DancePose& P = run.poseNow();
            const float sx = run.x(), sz = run.z(), kk = run.scale();
            res.waitFrames += runs[std::size_t(k)]->choreographer().floorPending() >= 0;
            if (kk < 0.95f || run.leaving()) {
                pv.have = false;
                continue;
            }
            const dance::Look& look = run.spec().look;
            const Contacts L = contactsOf(look, sks[std::size_t(k)], P);
            // The move alone: the source's pose at this dancer's beat (own routine or the leader's), grounded, no
            // humanising, no blending filters, energy 1, no stage facing, no spot.
            const double db = run.danceBeat(beat);
            dance::MusicState mh = m;
            mh.beat = db;
            const DanceDirection& dir = dirs[std::size_t(k)];
            dance::DancePose Pi;
            int tier = 0;
            std::string label;
            if (dir.react) {
                Pi = dance::watchPose(db, run.seed(), run.spec().style, sks[std::size_t(k)], true);
                label = "watching";
            } else if (dir.leader) {
                Pi = dir.leader->pose(mh, sks[std::size_t(k)], 0, dir.mirror);
                label = moveLabel(*dir.leader, db, &tier) + " (led)";
            } else {
                Pi = runs[std::size_t(k)]->choreographer().pose(mh, sks[std::size_t(k)]);
                label = moveLabel(runs[std::size_t(k)]->choreographer(), db, &tier);
            }
            dance::groundPose(sks[std::size_t(k)], Pi);
            const Contacts Li = contactsOf(look, sks[std::size_t(k)], Pi);
            const bool onFloor = P.ground >= 0.5f;
            if (dump && k < sc.breakers) {
                std::fprintf(dump, "%.4f,%.4f,%d,%s,\"%s\",%.3f,%.4f,%.4f,%.4f,%.4f", t, beat, k,
                             spots[std::size_t(k)].role ? spots[std::size_t(k)].role : "", label.c_str(), P.ground, sx, sz,
                             sx + kk * P.bodyX, sz + kk * P.bodyZ);
                for (int c : {kHandL, kHandR, kFootL, kFootR, kKneeL, kKneeR, kHead}) {
                    const H w = H{sx, sz} + (H{P.bodyX, P.bodyZ} + turn(L.p[c], P.bodyYaw)) * kk;
                    std::fprintf(dump, ",%.4f,%.4f,%.4f", w.x, w.z, L.low[c]);
                }
                std::fprintf(dump, "\n");
            }
            const std::string role = spots[std::size_t(k)].role ? spots[std::size_t(k)].role : "";
            if (pv.have) {
                const H dS{sx - pv.sx, sz - pv.sz};
                const H dKeep{sx - preKeepX[std::size_t(k)], sz - preKeepZ[std::size_t(k)]};
                const H dB = H{P.bodyX - pv.B.x, P.bodyZ - pv.B.z} * kk;
                const float spotSpeed = len(dS) * float(kFps);
                if (onFloor && pv.floor) {  // pops: the fastest hand, foot or head this frame (world, 3D)
                    float fast = 0;
                    int fastC = 0;
                    for (int c : {kHandL, kHandR, kFootL, kFootR, kHead}) {
                        const H w1 = H{sx, sz} + (H{P.bodyX, P.bodyZ} + turn(L.p[c], P.bodyYaw)) * kk;
                        const H w0 = H{pv.sx, pv.sz} + (H{pv.B.x, pv.B.z} + turn(pv.L.p[c], pv.yaw)) * pv.k;
                        const float dy = (L.p[c].y - pv.L.p[c].y) * kk;
                        const float f = std::sqrt(len(w1 - w0) * len(w1 - w0) + dy * dy);
                        if (f > fast) fast = f, fastC = c;
                    }
                    if (fast > res.pop.max()) {
                        char b[300];
                        std::snprintf(b, sizeof b, "%.1f cm at t %.2f s, dancer %d, %s, %s, pins held %d, shift %.3f", 100 * fast, t,
                                      k, kContactName[fastC], label.c_str(), run.pinsHeld(), run.pinShift());
                        res.popWorst = b;
                    }
                    res.pop.add(fast);
                }
                if (onFloor) {
                    ++res.floorFrames;
                    if (spotSpeed > 0.05f) ++res.floorFramesMoving;
                    res.floorSpot.add(spotSpeed);
                    res.floorSpotByRole[role].add(spotSpeed);
                }
                for (int c = 0; c < kContacts; ++c) {
                    const bool upper = c != kFootL && c != kFootR;
                    if (!onFloor && upper) continue;  // standing: the feet only
                    const bool in = L.low[c] < 0.01f && pv.L.low[c] < 0.01f;
                    Episode& ep = pv.ep[c];
                    if (!in) {
                        if (pv.in[c] && onFloor) keepWorst(res.worst, ep);
                        pv.in[c] = false;
                        continue;
                    }
                    if (!pv.in[c]) {
                        ep = Episode{};
                        ep.move = label;
                        ep.role = role;
                        ep.name = std::string(dance::styleName(run.spec().style)) + " #" + std::to_string(k);
                        ep.contact = std::string(c % 2 == 0 || c >= kFootL + 1 ? "" : "") + kContactName[c] +
                                     (c <= kFootR ? (c % 2 == 0 ? " L" : " R") : "");
                    }
                    pv.in[c] = true;
                    // The four parts (exact).
                    const H h0 = turn(pv.L.p[c], pv.yaw), h1 = turn(pv.L.p[c], P.bodyYaw);
                    const H dHead = (h1 - h0) * kk;
                    const H dPose = (turn(L.p[c], P.bodyYaw) - h1) * kk;
                    const H total = dS + dB + dHead + dPose;
                    // The stage's facing alone (the same contact turned by the change of facing).
                    const float dFace = run.facing() - pv.facing;
                    const H dFacing = (turn(pv.L.p[c], pv.yaw + dFace) - h0) * kk;
                    // The move alone (as retargeted): its own slip; and what the runtime layers add to the pose / root.
                    H dIntr{};
                    const bool inI = Li.low[c] < 0.01f && pv.Li.low[c] < 0.01f;
                    if (inI)
                        dIntr = (H{Pi.bodyX, Pi.bodyZ} + turn(Li.p[c], Pi.bodyYaw)) -
                                (H{pv.Bi.x, pv.Bi.z} + turn(pv.Li.p[c], pv.yawi));
                    const H rtPose = (H{L.p[c].x - Li.p[c].x, L.p[c].z - Li.p[c].z}) -
                                     (H{pv.L.p[c].x - pv.Li.p[c].x, pv.L.p[c].z - pv.Li.p[c].z});
                    const H rtRoot = H{P.bodyX - Pi.bodyX, P.bodyZ - Pi.bodyZ} - H{pv.B.x - pv.Bi.x, pv.B.z - pv.Bi.z};
                    const H rtPoseW = (turn(L.p[c], P.bodyYaw) - turn(Li.p[c] + (pv.L.p[c] - pv.Li.p[c]), P.bodyYaw)) * kk;
                    const H parts[kCauses] = {total, dS, dKeep, dB, dHead, dFacing, dPose, dIntr, rtPose, rtRoot,
                                              total - dS, total - dFacing, total - rtPoseW, total - dIntr, dS + dFacing};
                    std::string key;
                    if (onFloor) {
                        key = std::string("floor ") + kContactName[contactKind(c)];
                    } else {
                        key = run.walking() > 0.02f ? "standing feet, walking (stepFeet)" : "standing feet, in place";
                    }
                    const bool held = inI && len(dIntr) < 0.001f;  // the move alone keeps it still (< 2.9 cm / beat)
                    const std::string all = onFloor ? "floor, all contacts" : "standing, all feet";
                    std::vector<std::string> keys = {key, all};
                    // Planted in the source (the pin weights of the move as danced).
                    const float pinW[kContacts] = {P.pinHandL, P.pinHandR, 0, 0, P.pinKneeL, P.pinKneeR,
                                                   P.pinFootL, P.pinFootR, P.pinHead, 0};
                    if (onFloor && pinW[c] >= 0.5f) keys.push_back("floor, all contacts [planted in the source]");
                    if (held) keys.push_back(all + " [held by the move]"), keys.push_back(key + " [held by the move]");
                    for (const std::string& kname : keys) {
                        Stats& st = res.cat[kname];
                        ++st.frames;
                        for (int q = 0; q < kCauses; ++q)
                            if ((q != kIntrinsic && q != kRuntimeOnly) || inI) st.d[q].add(len(parts[q]));
                    }
                    if (onFloor && held) {
                        res.heldByRole[role] += len(dS);
                        res.heldByRoleT[role] += dt;
                    }
                    if (onFloor) {
                        ep.held += held;
                        res.spotByRole[role] += len(dS);
                        res.spotByRoleT[role] += dt;
                        for (int q = 0; q < kCauses; ++q) {
                            ep.dist[q] += len(parts[q]);
                            ep.net[q] = ep.net[q] + parts[q];
                        }
                        ++ep.frames;
                        ep.spotSpeedMax = std::max(ep.spotSpeedMax, spotSpeed);
                    }
                }
            }
            pv.have = true;
            pv.sx = sx;
            pv.sz = sz;
            pv.k = kk;
            pv.facing = run.facing();
            pv.B = {P.bodyX, 0, P.bodyZ};
            pv.yaw = P.bodyYaw;
            pv.L = L;
            pv.Bi = {Pi.bodyX, 0, Pi.bodyZ};
            pv.yawi = Pi.bodyYaw;
            pv.Li = Li;
            pv.floor = onFloor;
        }
    }
    if (dump) std::fclose(dump);
    for (auto& r : runs) {
        const auto& c = r->choreographer().floorCounts();
        res.counts.now += c.now;
        res.counts.waited += c.waited;
        res.counts.forced += c.forced;
        for (int k = 0; k < 8; ++k) res.counts.delays[k] += c.delays[k];
        res.counts.bustDelays += c.bustDelays;
    }
    floorFixes = FloorFixes{};
    return res;
}

// Units / frame -> cm / beat at 124 BPM (1 unit = 100 cm; a dancer is about 1 unit tall) and units / s.
double cmPerBeat(double perFrame) { return perFrame * kFps * 60.0 / kBpm * 100.0; }

void report(const Scenario& sc, Result& r) {
    std::printf("\n==== %s: %d dancers (%d break), %.0f s, %s, footwork %.1f power %.1f%s\n", sc.name.c_str(), sc.dancers,
                sc.breakers, sc.seconds, sc.mingle ? "mingle" : "fixed spots", sc.footwork, sc.power,
                sc.tryAll ? ", every floor move tried in turn" : "");
    std::printf("dancer frames on the floor: %ld, with the spot moving over 0.05 units/s: %ld (%.1f %%)\n", r.floorFrames,
                r.floorFramesMoving, r.floorFrames ? 100.0 * double(r.floorFramesMoving) / double(r.floorFrames) : 0.0);
    std::printf("spot speed on the floor (units/s): mean %.3f p50 %.3f p90 %.3f max %.3f; > 0.05: %.1f %%, > 0.15: %.1f %%, > 0.3: %.1f %%\n",
                r.floorSpot.mean(), r.floorSpot.pct(0.5), r.floorSpot.pct(0.9), r.floorSpot.max(), 100 * r.floorSpot.share(0.05f),
                100 * r.floorSpot.share(0.15f), 100 * r.floorSpot.share(0.3f));
    std::printf("pops (floor frames, the fastest hand / foot / head a frame): p99 %.2f cm, p99.9 %.2f cm, max %.2f cm\n",
                100 * r.pop.pct(0.99), 100 * r.pop.pct(0.999), 100 * r.pop.max());
    std::printf("   worst: %s\n", r.popWorst.c_str());
    {
        int n = 0, sum = 0, mx = 0;
        for (int k = 0; k < 8; ++k) n += r.counts.delays[k], sum += k * r.counts.delays[k], mx = r.counts.delays[k] ? k : mx;
        int p90 = 0;
        for (int k = 0, run = 0; k < 8; ++k) {
            run += r.counts.delays[k];
            if (run * 10 >= n * 9) {
                p90 = k;
                break;
            }
        }
        std::printf("floor moves: %d started at once, %d after waiting for room, %d anyway (last resort); %d bust delays; "
                    "waiting %.1f %% of dancer frames; start delay in grid points: mean %.2f, p90 %d, max %d%s (of %d)\n",
                    r.counts.now, r.counts.waited, r.counts.forced, r.counts.bustDelays,
                    100.0 * double(r.waitFrames) / std::max(1.0, sc.seconds * kFps * sc.dancers),
                    n ? double(sum) / n : 0.0, p90, mx, mx == 7 ? "+" : "", n);
    }
    for (auto& [role, d] : r.floorSpotByRole)
        std::printf("   %-26s %5.1f %% of floor time, spot mean %.3f p90 %.3f u/s, > 0.15: %.1f %%\n", role.c_str(),
                    100.0 * double(d.v.size()) / double(std::max<std::size_t>(1, r.floorSpot.v.size())), d.mean(), d.pct(0.9),
                    100 * d.share(0.15f));
    for (auto& [name, st] : r.cat) {
        std::printf("-- %s (%ld contact frames, %.0f s)\n", name.c_str(), st.frames, double(st.frames) / kFps);
        std::printf("   %-28s %9s %9s %9s %9s | %9s %9s %8s %8s\n", "cause", "mean", "p90", "p99", "max", "mean u/s",
                    "p90 u/s", ">0.1cm/f", ">0.5cm/f");
        for (int q = 0; q < kCauses; ++q) {
            Dist& d = st.d[q];
            if (d.v.empty()) continue;
            std::printf("   %-28s %6.2f cm %6.2f cm %6.2f cm %6.1f cm | %9.3f %9.3f %7.1f%% %7.1f%%   (per beat)\n",
                        kCauseName[q], cmPerBeat(d.mean()), cmPerBeat(d.pct(0.9)), cmPerBeat(d.pct(0.99)),
                        cmPerBeat(d.max()), d.mean() * kFps, d.pct(0.9) * kFps, 100 * d.share(0.001f),
                        100 * d.share(0.005f));
        }
    }
    if (!r.spotByRole.empty()) {
        std::printf("-- floor contacts: spot slip by mingle role (path length, contact time, mean speed)\n");
        double tot = 0;
        for (auto& [k, v] : r.spotByRole) tot += v;
        for (auto& [k, v] : r.spotByRole)
            std::printf("   %-26s %7.2f units (%4.1f %%) over %7.1f s: %.3f units/s\n", k.empty() ? "(none)" : k.c_str(), v,
                        tot > 0 ? 100 * v / tot : 0.0, r.spotByRoleT[k], v / std::max(1e-9, r.spotByRoleT[k]));
    }
    if (!r.heldByRole.empty()) {
        std::printf("-- floor contacts held by the move: spot slip by mingle role\n");
        double tot = 0;
        for (auto& [k, v] : r.heldByRole) tot += v;
        for (auto& [k, v] : r.heldByRole)
            std::printf("   %-26s %7.2f units (%4.1f %%) over %7.1f s: %.3f units/s\n", k.empty() ? "(none)" : k.c_str(), v,
                        tot > 0 ? 100 * v / tot : 0.0, r.heldByRoleT[k], v / std::max(1e-9, r.heldByRoleT[k]));
    }
    if (!r.worst.empty()) {
        std::printf("-- worst contact episodes held still by the move (net slide while in contact)\n");
        for (const auto& e : r.worst) {
            std::printf("   %.1f cm over %.2f s (%d frames): %s, %s, %s [%s]; spot max %.2f u/s\n", 100 * len(e.net[kTotal]),
                        e.frames / kFps, e.frames, e.name.c_str(), e.contact.c_str(), e.move.c_str(), e.role.c_str(),
                        e.spotSpeedMax);
            std::printf("      net cm: spot %.1f (keepApart %.1f), root %.1f, heading %.1f (facing %.1f), pose %.1f; move alone %.1f\n",
                        100 * len(e.net[kSpot]), 100 * len(e.net[kSpotKeepApart]), 100 * len(e.net[kRoot]),
                        100 * len(e.net[kHeading]), 100 * len(e.net[kHeadingFacing]), 100 * len(e.net[kPose]),
                        100 * len(e.net[kIntrinsic]));
        }
    }
    if (!r.overlap.empty()) {
        std::printf("-- overlap: per floor move, frames with another dancer's body capsules inside the floor dancer's\n");
        long F = 0, O = 0;
        float M = 0;
        for (auto& [label, o] : r.overlap) {
            F += o.frames;
            O += o.overlapFrames;
            M = std::max(M, o.maxDepth);
            std::printf("   %-58s %5ld samples, overlap %5.1f %%, mean depth %4.1f cm, max %4.1f cm\n", label.c_str(),
                        o.frames, o.frames ? 100.0 * double(o.overlapFrames) / double(o.frames) : 0.0,
                        o.overlapFrames ? 100 * o.depthSum / double(o.overlapFrames) : 0.0, 100 * o.maxDepth);
        }
        std::printf("   ALL FLOOR: %ld samples, overlap %.1f %%, max %.1f cm\n", F, F ? 100.0 * double(O) / double(F) : 0.0,
                    100 * M);
        const Overlap& s = r.standingOverlap;
        std::printf("   standing dancers (any standing neighbor, comparison): %ld samples, overlap %.2f %%, mean %.1f cm, max %.1f cm\n",
                    s.frames, s.frames ? 100.0 * double(s.overlapFrames) / double(s.frames) : 0.0,
                    s.overlapFrames ? 100 * s.depthSum / double(s.overlapFrames) : 0.0, 100 * s.maxDepth);
    }
    std::fflush(stdout);
}

}  // namespace

TEST_CASE("floor slide: measure contact slip in mingle crowds" * doctest::skip()) {
    FloorLoaded f;
    const double secs = std::getenv("SLIDE_SECONDS") ? std::atof(std::getenv("SLIDE_SECONDS")) : 600;
    std::vector<Scenario> scs = {
        {"solo, fixed spot", 1, 1, secs, false, 2, 1, false, false, 1},
        {"solo, mingle", 1, 1, secs, true, 2, 1, false, false, 1},
        {"5 mingle", 5, 2, secs, true, 2, 1, false, true, 1},
        {"9 mingle", 9, 4, secs, true, 2, 1, false, true, 1},
        {"14 mingle", 14, 6, secs, true, 2, 1, false, true, 1},
    };
    const char* only = std::getenv("SLIDE_ONLY");
    for (auto& sc : scs) {
        if (only && sc.name.find(only) == std::string::npos) continue;
        if (std::getenv("SLIDE_SEED")) sc.seed = std::uint32_t(std::atoi(std::getenv("SLIDE_SEED")));
        Result r = runScenario(sc);
        report(sc, r);
    }
}

TEST_CASE("floor slide: measure every floor move tried (crowd and solo)" * doctest::skip()) {
    FloorLoaded f;
    int nFloor = 0;
    for (int i = 0; i < dance::clipCount(); ++i) nFloor += dance::clipInfo(i).tier != dance::kStanding;
    const double secs = nFloor * 64 * 60.0 / kBpm;  // 16 bars each
    std::vector<Scenario> scs = {
        {"tried, solo fixed spot", 1, 1, secs, false, 0, 0, true, false, 2},
        {"tried, 9 mingle", 9, 9, secs, true, 0, 0, true, true, 2},
        {"tried, 14 mingle", 14, 14, secs, true, 0, 0, true, true, 3},
    };
    const char* only = std::getenv("SLIDE_ONLY");
    for (auto& sc : scs) {
        if (only && sc.name.find(only) == std::string::npos) continue;
        Result r = runScenario(sc);
        report(sc, r);
    }
}

// Each floor move's reach (Part 3): how far the body (capsule ends plus radii, hair left out) gets from the spot,
// horizontally, over its loop; where (relative to the body heading); the root travel; how much it turns.
TEST_CASE("floor slide: measure the reach of every floor move" * doctest::skip()) {
    FloorLoaded f;
    dance::Look look;
    look.hairStyle = dance::HairStyle::Bald;
    const dance::Skeleton sk = dance::makeSkeleton(look);
    std::printf("\n%-58s %6s %6s %6s %6s %6s | reach by side of the heading: front back left right | turn\n", "move",
                "tier", "max", "p90", "mean", "root");
    auto measure = [&](int i) {
        const dance::ClipInfo& c = dance::clipInfo(i);
        Dist reach;
        float rootMax = 0, side[4] = {};  // front, back, left, right (body heading)
        float yawMin = 1e9f, yawMax = -1e9f;
        for (int k = 0; k < c.beats * 16; ++k) {
            dance::DancePose p = dance::restPose(sk);
            dance::clipPose(i, float(k) / 16.0f, sk, p);
            dance::groundPose(sk, p);
            yawMin = std::min(yawMin, p.bodyYaw);
            yawMax = std::max(yawMax, p.bodyYaw);
            rootMax = std::max(rootMax, std::hypot(p.bodyX, p.bodyZ));
            dance::BodyOut o;
            dance::buildBody(look, sk, p, dance::Frame{}, 1.0f, o);
            float r = 0;
            const float cy = std::cos(p.bodyYaw), sy = std::sin(p.bodyYaw);
            for (const auto& part : o.parts) {
                if (!bodyPart(part)) continue;
                for (int e = 0; e < 2; ++e) {
                    const V3 q = e ? part.frame.point({0, part.len, 0}) : part.frame.o;
                    const float rr = e ? part.r1 : part.r0;
                    const float d = std::hypot(q.x, q.z) + rr;
                    r = std::max(r, d);
                    // In the body heading's frame: forward = (sin, cos), left = (cos, -sin).
                    const float fwd = q.x * sy + q.z * cy, lft = q.x * cy - q.z * sy;
                    side[0] = std::max(side[0], fwd + rr);
                    side[1] = std::max(side[1], -fwd + rr);
                    side[2] = std::max(side[2], lft + rr);
                    side[3] = std::max(side[3], -lft + rr);
                }
            }
            reach.add(r);
        }
        std::printf("%-58s %6s %6.2f %6.2f %6.2f %6.2f | %5.2f %5.2f %5.2f %5.2f | %5.1f rad\n", c.label.c_str(),
                    dance::clipTierName(c.tier), reach.max(), reach.pct(0.9), reach.mean(), rootMax, side[0], side[1],
                    side[2], side[3], yawMax - yawMin);
        if (std::getenv("SLIDE_IDS")) std::printf("   id %s beats %d bpm %d\n", c.id.c_str(), c.beats, c.bpm);
    };
    for (int i = 0; i < dance::clipCount(); ++i)
        if (dance::clipInfo(i).tier != dance::kStanding) measure(i);
    std::printf("-- standing break moves for comparison\n");
    for (int i = 0; i < dance::clipCount(); ++i)
        if (dance::clipInfo(i).tier == dance::kStanding && dance::clipInfo(i).genre == "BR") measure(i);
    std::fflush(stdout);
}

// The move alone, as retargeted (no runtime layers): each floor move played at its source's frame rate (60 fps at its
// own tempo), grounded; floor contacts' horizontal speed in leg lengths per beat, to set against the source
// (scratch tools: the AIST++ keypoints' wrists, knees and ankles near the floor, the same units).
TEST_CASE("floor slide: measure each floor move's own contact slip" * doctest::skip()) {
    FloorLoaded f;
    dance::Look look;
    look.hairStyle = dance::HairStyle::Bald;
    const dance::Skeleton sk = dance::makeSkeleton(look);
    const float leg = sk.thigh + sk.shin;
    std::printf("\n%-45s %-5s %6s %6s %6s %6s %8s  (leg lengths / beat)\n", "move", "part", "frames", "mean", "p50", "p90",
                "held<0.1");
    Dist all[3];
    for (int i = 0; i < dance::clipCount(); ++i) {
        const dance::ClipInfo& c = dance::clipInfo(i);
        if (c.tier == dance::kStanding) continue;
        const double fpb = 3600.0 / c.bpm;
        const int n = int(std::lround(c.beats * fpb));
        Dist d[3];
        bool have = false;
        Contacts prevL;
        H prevW[kContacts];
        for (int fr = 0; fr <= n; ++fr) {
            dance::DancePose p = dance::restPose(sk);
            dance::clipPose(i, float(fr / fpb), sk, p);
            dance::groundPose(sk, p);
            const Contacts L = contactsOf(look, sk, p);
            H w[kContacts];
            for (int q = 0; q < kContacts; ++q) w[q] = H{p.bodyX, p.bodyZ} + turn(L.p[q], p.bodyYaw);
            if (have)
                for (int q : {kHandL, kHandR, kKneeL, kKneeR, kFootL, kFootR}) {
                    if (L.low[q] >= 0.01f || prevL.low[q] >= 0.01f) continue;
                    const int part = q <= kHandR ? 0 : q <= kKneeR ? 1 : 2;
                    if (q == kKneeL || q == kKneeR) {
                    }
                    d[part].add(len(w[q] - prevW[q]) / leg * float(fpb));
                }
            have = true;
            prevL = L;
            for (int q = 0; q < kContacts; ++q) prevW[q] = w[q];
        }
        const char* names[3] = {"hand", "knee", "foot"};
        const std::string id = c.id.substr(c.id.find(':') + 1);
        for (int q = 0; q < 3; ++q) {
            for (float v : d[q].v) all[q].add(v);
            if (d[q].v.size() > 10)
                std::printf("%-45s %-5s %6zu %6.2f %6.2f %6.2f %7.1f%%\n", id.c_str(), names[q], d[q].v.size(), d[q].mean(),
                            d[q].pct(0.5), d[q].pct(0.9), 100 * (1 - d[q].share(0.1f)));
        }
    }
    const char* names[3] = {"hand", "knee", "foot"};
    for (int q = 0; q < 3; ++q)
        std::printf("%-45s %-5s %6zu %6.2f %6.2f %6.2f %7.1f%%\n", "all floor moves", names[q], all[q].v.size(),
                    all[q].mean(), all[q].pct(0.5), all[q].pct(0.9), 100 * (1 - all[q].share(0.1f)));
    std::fflush(stdout);
}

// The fixes, checked (docs/dancers-design.md "Floor moves held still, room to go down"): five dancers (two break) for
// two and a half minutes, as before the fixes and with them.
TEST_CASE("floor slide: floor moves held still, given room, no new pops") {
    FloorLoaded f;
    Scenario off{"5 mingle, before", 5, 2, 150, true, 2, 1, false, true, 1, "none"};
    Scenario on{"5 mingle, held", 5, 2, 150, true, 2, 1, false, true, 1, "hold,fade,pins,room"};
    Result a = runScenario(off), b = runScenario(on);
    auto mean = [](Result& r, const char* cat) { return float(cmPerBeat(r.cat[cat].d[kTotal].mean())); };
    auto overlap = [](const Result& r) {
        long F = 0, O = 0;
        for (const auto& [label, o] : r.overlap) F += o.frames, O += o.overlapFrames;
        return F ? float(O) / float(F) : 0.0f;
    };
    const float heldA = mean(a, "floor, all contacts [held by the move]"), heldB = mean(b, "floor, all contacts [held by the move]");
    const float plantA = mean(a, "floor, all contacts [planted in the source]"),
                plantB = mean(b, "floor, all contacts [planted in the source]");
    const float allA = mean(a, "floor, all contacts"), allB = mean(b, "floor, all contacts");
    int maxDelay = 0;
    for (int k = 0; k < 8; ++k)
        if (b.counts.delays[k]) maxDelay = k;
    MESSAGE("floor contacts cm / beat, before -> held: all " << allA << " -> " << allB << ", held by the move " << heldA << " -> "
            << heldB << ", planted in the source " << plantA << " -> " << plantB << "; spot over 0.15 u/s "
            << 100 * a.floorSpot.share(0.15f) << " % -> " << 100 * b.floorSpot.share(0.15f) << " %; overlap "
            << 100 * overlap(a) << " % -> " << 100 * overlap(b) << " %; fastest hand / foot / head a frame p99.9 "
            << 100 * a.pop.pct(0.999) << " -> " << 100 * b.pop.pct(0.999) << " cm, max " << 100 * a.pop.max() << " -> "
            << 100 * b.pop.max() << " cm; floor moves " << b.counts.now << " at once, " << b.counts.waited
            << " after waiting, " << b.counts.forced << " anyway; start delay at most " << maxDelay << " grid points");
    REQUIRE(b.floorFrames > 2000);
    CHECK(heldB < 3.0f);               // as planted standing feet (about 2-3)
    CHECK(plantB < 6.0f);
    CHECK(plantB < 0.4f * plantA);
    CHECK(allB < 0.85f * allA);  // (all contacts: many sweep the floor in the source too)
    CHECK(b.floorSpot.share(0.15f) < 0.005f);  // no glide
    CHECK(overlap(b) < 0.03f);
    CHECK(b.pop.pct(0.999) < 1.15f * a.pop.pct(0.999));
    CHECK(b.counts.now + b.counts.waited > 5);
    CHECK(b.counts.forced == 0);  // (room always cleared: never the last resort)
    CHECK(maxDelay <= 2);
}
