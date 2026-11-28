// Why did dancers sometimes spin super-rapidly? (Peter, 2026-11-27; fixed the same day: docs/dancers-design.md "Spin
// glitches fixed".) The measurements are skipped by default:
//   reforce_engine_tests -tc="spin: measure*" --no-skip
//   (SPIN_SECONDS, SPIN_ONLY, SPIN_SEED, SPIN_UNLOCKED, SPIN_DUMP, SPIN_EVENTS, SPIN_TRACE, SPIN_CHURN, SPIN_PASSIVE)
// the key results run as checks ("spin: no routine churn, ..."). Since the fixes the energy mix leaves the heading
// alone (part (e) is always 0), and each clip is turned to face the audience at load (faceAudience).
//
// The body's heading as drawn is pose.bodyYaw after DancerRun::step: the source's heading (the routine, crossfaded
// with the next one by the choreographer; a leader's mirrored; a watch move), plus inertial blending's offset at a
// change of source, plus the stage's facing (DancerRun::facing_), then the energy mix towards the rest pose
// (mix(restPose, pose, e) when the effort is under 1). Per frame its change splits exactly (each part a wrapped
// difference) into:
//   (a) the move alone       the routine playing, by itself (clipPose / the move function), mirrored as it is
//   (b) crossfade            the choreographer's blend into the next routine (Choreographer::pose, mix)
//   (c) inertial             DancerRun's inertial blending after a change of source (own / react / leader)
//   (d) stage facing         DancerRun::facing_ (the stage's turn, walking turned towards the destination)
//   (e) energy mix           mix(restPose, pose, e) on the whole heading (effort < 1: wandering, resting...)
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "engine/DanceClips.hpp"
#include "engine/DanceMoves.hpp"
#include "engine/DancerRig.hpp"
#include "engine/Dancers.hpp"

using namespace reforce::engine;

namespace {

const std::string kMovesFile = std::string(REFORCE_SOURCE_DIR) + "/content/Moves/aist.moves";
const std::string kFloorFile = std::string(REFORCE_SOURCE_DIR) + "/content/Moves/aist_floor.moves";

struct SpinLoaded {
    SpinLoaded() {
        REQUIRE(dance::loadClips(kMovesFile).empty());
        REQUIRE(dance::loadClips(kFloorFile, true).empty());
        dance::setFloorWeights(dance::kDefaultFloorWeight);
    }
    ~SpinLoaded() {
        dance::setFloorWeights(0);
        dance::loadClips(kMovesFile);
    }
};

constexpr double kBpm = 124, kFps = 60;
constexpr float kTau = 6.2831853f;
constexpr float kFast = 1.5f * kTau;  // rad/s: 1.5 turns a second

// 124 BPM, 64-bar cycles: 48 bars of groove, an 8-bar breakdown (build 0 -> 1), a drop on bar 56, 8 more bars.
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

float wrapf(float a) { return std::remainder(a, kTau); }

struct Dist {
    std::vector<float> v;
    void add(float x) { v.push_back(x); }
    float pct(double p) const {
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

enum Part { kMove, kXfade, kCut, kInert, kFacing, kEnergy, kParts };
const char* kPartName[kParts] = {"(a) move alone", "(b) crossfade", "(b') cut (no fade)", "(c) inertial",
                                 "(d) stage facing", "(e) energy mix"};

struct Event {
    double t = 0, beat = 0;
    int dancer = 0, frames = 0;
    std::string style, move, role, source, extra;
    float angle = 0, peak = 0, maxStep = 0;  // net turn (rad), peak speed (rad/s), biggest one-frame step (rad)
    float part[kParts] = {};                  // each part's net turn over the event
    int cause = 0;
};

struct Scenario {
    std::string name;
    int dancers = 5;
    double seconds = 600;
    std::uint32_t seed = 1;
    float human = 1;
    double bustEvery = 90;  // seconds: someone busts a move (a guest's button)
};

struct Result {
    Dist speed;                 // |total| rad/s, every dancer frame
    Dist partSpeed[kParts];
    long frames = 0, fastFrames = 0;
    long partFast[kParts] = {};  // fast frames whose biggest part is this one
    std::vector<Event> events;
    double dancerSeconds = 0;
    long rawTurnJumps = 0, switchAfterJump = 0;  // the source heading jumping by whole turns; a switch right after
    long switches = 0;
    long churnFrames = 0, churnBust = 0;  // frames where the routine changed again (a new one every frame)
    long churn3Frames = 0;                // the routine changed three or more frames running (a drop cutting in
                                          // right after a routine started on its 8-bar point is two)
    long bustFrames = 0, bustAway = 0;    // busting a move: frames, and those facing more than 90 degrees from the camera
    long awayFrames = 0;                  // everyone: facing more than 90 degrees from the camera
    // What if: the heading without some parts (an upper bound of what a fix removes): frames over 1.5 turns/s and
    // events (runs of them) that are visible (|turn| >= 1 rad or a one-frame snap > 0.5 rad).
    static constexpr int kWhatIfs = 5;
    long wiFast[kWhatIfs] = {}, wiEvents[kWhatIfs] = {}, wiVisible[kWhatIfs] = {};
};

std::string moveLabel(const dance::Routine* r) {
    if (!r) return "?";
    if (r->full < 0) {
        std::string s = r->legs >= 0 ? dance::moveLibrary()[std::size_t(r->legs)].name : "";
        if (r->arms >= 0) s += std::string(" + ") + dance::moveLibrary()[std::size_t(r->arms)].name;
        return s;
    }
    const auto& m = dance::moveLibrary()[std::size_t(r->full)];
    if (m.clip < 0) return m.name;
    return dance::clipInfo(m.clip).label;
}

// The routine `r` alone at `beat` (as Choreographer::evaluate plays it), its body heading.
float aloneYaw(const dance::Routine& r, double beat, const dance::Skeleton& sk, bool mirror) {
    dance::DancePose p = dance::restPose(sk);
    dance::MoveCtx c;
    c.amp = r.amp;
    c.variant = r.variant;
    c.shoulder = sk.shoulderHalf;
    c.foot = sk.footHalf;
    c.sk = &sk;
    const double local = (beat - (r.aligned ? r.origin : r.start)) * r.speed;
    auto phase = [&](int m) {
        const double len = double(dance::moveLibrary()[std::size_t(m)].beats);
        double ph = std::fmod(local, len);
        if (ph < 0) ph += len;
        return float(ph);
    };
    if (r.full >= 0) {
        dance::runMove(dance::moveLibrary()[std::size_t(r.full)], phase(r.full), c, p);
    } else {
        if (r.legs >= 0) dance::runMove(dance::moveLibrary()[std::size_t(r.legs)], phase(r.legs), c, p);
        if (r.arms >= 0) dance::runMove(dance::moveLibrary()[std::size_t(r.arms)], phase(r.arms), c, p);
    }
    return (r.mirror != mirror) ? -p.bodyYaw : p.bodyYaw;
}

Result run(const Scenario& sc, std::FILE* dump) {
    const StageCamera cam = stageCamera(16.0 / 9.0, 0.42);
    const int n = sc.dancers;
    const std::size_t nn = std::size_t(n);
    std::vector<std::unique_ptr<DancerRun>> runs;
    std::vector<dance::Skeleton> sks;
    for (int k = 0; k < n; ++k) {
        const auto st = dance::Style(k % dance::kStyleCount);  // every style, break included
        runs.push_back(std::make_unique<DancerRun>(generateDancer(st, sc.seed * 101 + std::uint32_t(k)),
                                                   sc.seed * 977 + 70 + std::uint32_t(k), 0.0, 0.0, 0.0));
        sks.push_back(dance::makeSkeleton(runs.back()->spec().look));
        runs.back()->setHairMotion(0);
        runs.back()->setHuman(sc.human);
        runs.back()->choreographer().setMocap(1);
    }
    Mingle mingle;
    Result res;
    struct Prev {
        bool have = false;
        float Y = 0, F = 0, M = 0, S = 0, A = 0, Ypre = 0;
        std::string key, id;
        double keyT = -1e9, faceT = -1e9;
        float spotFacing = 0;
        bool inEvent = false;
        int churn = 0;  // frames in a row the routine changed
        bool jumped = false;
        struct WI {
            bool in = false;
            int gap = 0;
            float angle = 0, step = 0;
        } wi[Result::kWhatIfs];
        int gap = 0;
        Event ev;
    };
    std::vector<Prev> prev(nn);
    const double dt = 1.0 / kFps;
    const long frames = long(sc.seconds * kFps);
    std::uint32_t bustRng = sc.seed * 0x9e3779b9u + 7;
    double nextBust = sc.bustEvery;
    for (long i = 0; i < frames; ++i) {
        const double t = double(i) * dt;
        const double beat = t * kBpm / 60.0;
        const dance::MusicState m = musicAt(beat);
        if (sc.bustEvery > 0 && t >= nextBust) {
            bustRng = bustRng * 1664525u + 1013904223u;
            runs[std::size_t((bustRng >> 8) % std::uint32_t(n))]->bustMove();
            nextBust += sc.bustEvery;
        }
        std::vector<MinglePerson> people;
        for (int k = 0; k < n; ++k) {
            const auto& r = *runs[std::size_t(k)];
            MinglePerson p{std::uint64_t(r.seed()), r.x(), r.z(), r.facing(), r.placed()};
            p.style = int(r.spec().style);
            p.bustMove = r.inBustMove(beat);
            r.floorInfo(p, beat);
            people.push_back(p);
        }
        const std::vector<StageSpot> spots = mingle.plan(people, cam, beat, dt, sc.seed ^ 0x3141u);
        std::vector<DancerRun*> crowd;
        for (auto& r : runs) crowd.push_back(r.get());
        avoidEachOther(crowd);
        std::vector<DanceDirection> dirs(nn);
        std::vector<float> energies(nn);
        for (int k = 0; k < n; ++k) {
            auto& rn = *runs[std::size_t(k)];
            const StageSpot& sp = spots[std::size_t(k)];
            rn.setSlot(sp.x, sp.z);
            DanceDirection& dir = dirs[std::size_t(k)];
            if (sp.leader >= 0 && sp.leader != k) dir.leader = &runs[std::size_t(sp.leader)]->choreographer();
            dir.mirror = sp.mirror;
            dir.react = sp.react;
            dir.facing = sp.facing;
            dir.look = sp.look;
            dir.floorRoom = sp.floorRoom;
            // (VisualizerDancers: 1 x the effort with the beat locked, 0.7 x unlocked: SPIN_UNLOCKED)
            energies[std::size_t(k)] = (std::getenv("SPIN_UNLOCKED") ? 0.7f : 1.0f) * std::clamp(sp.effort, 0.3f, 1.0f);
            rn.step(t, dt, m, energies[std::size_t(k)], dir);
        }
        keepApart(crowd);

        for (int k = 0; k < n; ++k) {
            auto& rn = *runs[std::size_t(k)];
            Prev& pv = prev[std::size_t(k)];
            const StageSpot& sp = spots[std::size_t(k)];
            const DanceDirection& dir = dirs[std::size_t(k)];
            if (rn.scale() < 0.95f || rn.leaving()) {
                pv.have = false;
                continue;
            }
            const dance::DancePose& P = rn.poseNow();
            const float e = energies[std::size_t(k)];
            const float Y = P.bodyYaw, F = rn.facing();
            // (The energy mix leaves the heading alone since the spin fixes, 2026-11-27: before them Y / e.)
            const float Ypre = Y;
            const float M = Ypre - F;  // the move's heading after inertial blending
            // The source as DancerRun::step takes it (the same calls; the choreographer is already advanced).
            const double db = rn.danceBeat(beat);
            dance::MusicState mh = m;
            mh.beat = db;
            const auto& sk = sks[std::size_t(k)];
            const bool free = !rn.choreographer().bustMoveAt(db + 2, false) && rn.startTime() + 1.0 < t;
            float S, A;
            std::string key, move, id;
            auto ident = [](const dance::Routine* r) {
                return r ? std::to_string(r->full) + "/" + std::to_string(r->legs) + "/" + std::to_string(r->arms) + "@" +
                               std::to_string(r->start)
                         : std::string("-");
            };
            static const bool passive = std::getenv("SPIN_PASSIVE") != nullptr;  // no extra calls (state check)
            if (passive) {
                S = A = Ypre - F;
                key = "own";
                move = rn.describe(beat);
                id = move;
            } else if (free && dir.react) {
                S = A = dance::watchPose(db, rn.seed(), rn.spec().style, sk, true).bodyYaw;
                key = "react";
                move = dance::watchName(db, rn.seed(), rn.spec().style, true);
                id = move;  // (a basic repeated, or a routine across its blocks: the same move, continuous)
            } else if (free && dir.leader) {
                S = dir.leader->pose(mh, sk, 0, dir.mirror).bodyYaw;
                const dance::Routine* r = dir.leader->routineAt(db);
                A = r ? aloneYaw(*r, db, sk, dir.mirror) : S;
                id = ident(r);
                key = "lead" + std::to_string(dir.leader->options().seed) + (dir.mirror ? "m" : "");
                move = moveLabel(r) + (dir.mirror ? " (led, mirrored)" : " (led)");
            } else {
                S = rn.choreographer().pose(mh, sk).bodyYaw;
                const dance::Routine* r = rn.choreographer().routineAt(db);
                A = r ? aloneYaw(*r, db, sk, false) : S;
                id = ident(r);
                key = "own";
                move = moveLabel(r) + (r && r->mirror ? " (mirrored)" : "");
                static const int trace = std::getenv("SPIN_TRACE") ? std::atoi(std::getenv("SPIN_TRACE")) : -1;
                if (trace == k && r && pv.have && id != pv.id)
                    std::printf("trace t %.3f beat %.3f db %.3f phrasePos(db) %.3f: %s start %.3f end %.3f origin %.3f "
                                "created %.3f bust %.2f-%.2f pending %d\n",
                                t, beat, db, std::fmod(db - (m.phrase - std::floor(m.phrase)) * 64 - (beat - 64 * std::floor(m.phrase)) + 64 * 4, 64.0),
                                move.c_str(), r->start, r->end, r->origin, r->created,
                                rn.choreographer().bustMoveStart(), rn.choreographer().bustMoveEnd(),
                                rn.choreographer().floorPending());
            }
            if (pv.have && key != pv.key) pv.keyT = t;
            if (pv.have && std::isfinite(sp.facing) && std::fabs(sp.facing - pv.spotFacing) > 0.2f) pv.faceT = t;
            if (pv.have) {
                const float tot = wrapf(Y - pv.Y);
                float d[kParts];
                d[kMove] = wrapf(A - pv.A);
                d[kXfade] = wrapf((S - A) - (pv.S - pv.A));
                d[kCut] = 0;
                // Whole turns in the source's heading (invisible by itself: a loop seam, a crossfade's end) and a
                // change of source on the very next frame (inertial blending takes the velocity: DancerRun outVel_).
                if (key != pv.key) {
                    ++res.switches;
                    res.switchAfterJump += pv.jumped;
                }
                pv.jumped = key == pv.key && std::fabs(S - pv.S) > 3.14159f;
                res.rawTurnJumps += pv.jumped;
                if (key == pv.key && id != pv.id) {
                    if (++pv.churn >= 3) ++res.churn3Frames;
                    if (pv.churn >= 2) {
                        ++res.churnFrames;
                        if (std::getenv("SPIN_CHURN"))
                            std::printf("churn t %.3f #%d %s %s: %s -> %s\n", t, k, key.c_str(), move.c_str(),
                                        pv.id.c_str(), id.c_str());
                        if (rn.choreographer().bustMoveAt(db + 8, false) || rn.choreographer().bustMoveAt(db - 8, false))
                            ++res.churnBust;
                    }
                } else {
                    pv.churn = 0;
                }
                if (key == pv.key && id != pv.id) {  // the routine (or watch move) changed this frame
                    d[kCut] = wrapf(d[kMove] + d[kXfade]);
                    d[kMove] = d[kXfade] = 0;
                }
                d[kFacing] = F - pv.F;
                d[kEnergy] = wrapf(tot - wrapf(Ypre - pv.Ypre));
                d[kInert] = wrapf(tot - d[kMove] - d[kXfade] - d[kCut] - d[kFacing] - d[kEnergy]);
                const float w = std::fabs(tot) / float(dt);
                res.speed.add(w);
                for (int c = 0; c < kParts; ++c) res.partSpeed[c].add(std::fabs(d[c]) / float(dt));
                ++res.frames;
                {
                    // What if: 0 as is; 1 no energy mix on the heading; 2 and no cuts (faded instead); 3 and the
                    // crossfade's one-frame flips gone (blended without the short-way flip); 4 and no inertial part.
                    float v[Result::kWhatIfs];
                    v[0] = tot;
                    v[1] = v[0] - d[kEnergy];
                    v[2] = v[1] - d[kCut];
                    v[3] = v[2] - (std::fabs(d[kXfade]) > 0.5f ? d[kXfade] : 0.0f);
                    v[4] = v[3] - d[kInert];
                    for (int q = 0; q < Result::kWhatIfs; ++q) {
                        auto& W = pv.wi[q];
                        const bool f = std::fabs(v[q]) / float(dt) > kFast;
                        res.wiFast[q] += f;
                        if (f && !W.in) {
                            W = {};
                            W.in = true;
                        }
                        if (W.in) {
                            W.angle += v[q];
                            W.step = std::max(W.step, std::fabs(v[q]));
                            W.gap = f ? 0 : W.gap + 1;
                            if (W.gap > 2) {
                                W.in = false;
                                ++res.wiEvents[q];
                                res.wiVisible[q] += std::fabs(W.angle) >= 1.0f || W.step > 0.5f;
                            }
                        }
                    }
                }
                const bool fast = w > kFast;
                if (fast) {
                    ++res.fastFrames;
                    int big = 0;
                    for (int c = 1; c < kParts; ++c)
                        if (std::fabs(d[c]) > std::fabs(d[big])) big = c;
                    ++res.partFast[big];
                }
                // Events: runs of fast frames (a frame or two below in between still counts).
                if (fast) {
                    if (!pv.inEvent) {
                        pv.inEvent = true;
                        pv.ev = Event{};
                        pv.ev.t = t;
                        pv.ev.beat = beat;
                        pv.ev.dancer = k;
                        pv.ev.style = dance::styleName(rn.spec().style);
                        pv.ev.move = move;
                        pv.ev.role = sp.role ? sp.role : "";
                        pv.ev.source = key;
                        char b[200];
                        std::snprintf(b, sizeof b, "effort %.2f, walking %.2f, ground %.2f, source changed %.2f s ago, "
                                      "stage facing changed %.2f s ago, %s",
                                      e, rn.walking(), P.ground, t - pv.keyT, t - pv.faceT,
                                      m.drop > 0.05 ? "drop" : m.build > 0 ? "breakdown" : "groove");
                        pv.ev.extra = b;
                    }
                    pv.gap = 0;
                }
                if (pv.inEvent) {
                    Event& ev = pv.ev;
                    ++ev.frames;
                    ev.angle += tot;
                    ev.peak = std::max(ev.peak, w);
                    ev.maxStep = std::max(ev.maxStep, std::fabs(tot));
                    for (int c = 0; c < kParts; ++c) ev.part[c] += d[c];
                    if (!fast && ++pv.gap > 2) {
                        pv.inEvent = false;
                        int big = 0;
                        for (int c = 1; c < kParts; ++c)
                            if (std::fabs(ev.part[c]) > std::fabs(ev.part[big])) big = c;
                        ev.cause = big;
                        res.events.push_back(ev);
                    }
                }
                if (dump)
                    std::fprintf(dump, "%.4f,%d,%s,%s,\"%s\",%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.3f,%.3f,%.3f\n", t, k,
                                 key.c_str(), sp.role ? sp.role : "", move.c_str(), Y, F, A, S, M, Ypre, e,
                                 rn.walking(), P.ground);
            }
            pv.have = true;
            pv.Y = Y;
            pv.F = F;
            pv.M = M;
            pv.S = S;
            pv.A = A;
            pv.Ypre = Ypre;
            pv.key = key;
            pv.id = id;
            if (std::isfinite(sp.facing)) pv.spotFacing = sp.facing;
            res.dancerSeconds += dt;
            {
                const bool away = std::fabs(wrapf(Y)) > kTau / 4;
                res.awayFrames += away;
                if (rn.choreographer().bustMoveAt(rn.danceBeat(beat), false)) {
                    ++res.bustFrames;
                    res.bustAway += away;
                }
            }
        }
    }
    return res;
}

void report(const Scenario& sc, Result& r) {
    std::printf("\n== %s: %d dancers, %.0f s, Human %.1f (%.0f dancer-minutes)\n", sc.name.c_str(), sc.dancers,
                sc.seconds, sc.human, r.dancerSeconds / 60);
    const float T = kTau;
    std::printf("   body heading as drawn, turns/s: p50 %.3f p90 %.3f p99 %.3f p99.9 %.3f max %.2f; frames over 1.5 "
                "turns/s %.3f %%\n",
                r.speed.pct(0.5) / T, r.speed.pct(0.9) / T, r.speed.pct(0.99) / T, r.speed.pct(0.999) / T,
                r.speed.max() / T, 100.0 * double(r.fastFrames) / double(std::max(1L, r.frames)));
    std::printf("   %-18s %8s %8s %8s %8s  %s\n", "part (turns/s)", "p99", "p99.9", "max", ">1.5 %", "fast frames led");
    for (int c = 0; c < kParts; ++c)
        std::printf("   %-18s %8.3f %8.3f %8.2f %8.3f  %5.1f %%\n", kPartName[c], r.partSpeed[c].pct(0.99) / T,
                    r.partSpeed[c].pct(0.999) / T, r.partSpeed[c].max() / T, 100 * r.partSpeed[c].share(kFast),
                    100.0 * double(r.partFast[c]) / double(std::max(1L, r.fastFrames)));
    const double mins = r.dancerSeconds / 60;
    std::map<int, int> byCause;
    std::map<int, double> angleByCause;
    int snaps = 0;
    for (const Event& e : r.events) {
        ++byCause[e.cause];
        angleByCause[e.cause] += std::fabs(e.angle);
        snaps += e.maxStep > 0.5f;
    }
    std::printf("   events (runs of frames over 1.5 turns/s): %zu = %.2f per dancer-minute; with a one-frame snap "
                "> 0.5 rad: %d\n",
                r.events.size(), double(r.events.size()) / mins, snaps);
    std::map<int, int> visByCause, snapByCause, floorByCause;
    for (const Event& e : r.events) {
        if (std::fabs(e.angle) >= 1.0f || e.maxStep > 0.5f) ++visByCause[e.cause];
        if (e.maxStep > 0.5f) ++snapByCause[e.cause];
        if (e.role == "on the floor" || e.role == "going down") ++floorByCause[e.cause];
    }
    std::printf("     %-20s %6s %8s %8s %8s %8s %9s\n", "led by", "events", "/d-min", "visible", "/d-min", "snaps", "floor");
    for (const auto& [c, cnt] : byCause)
        std::printf("     %-20s %6d %8.2f %8d %8.2f %8d %9d   mean |turn| %.2f rad\n", kPartName[c], cnt,
                    double(cnt) / mins, visByCause[c], double(visByCause[c]) / mins, snapByCause[c], floorByCause[c],
                    angleByCause[c] / cnt);
    // By source / role of the dancer, events with a net turn of at least 1 rad (a visible spin).
    std::map<std::string, int> big;
    for (const Event& e : r.events)
        if (std::fabs(e.angle) >= 1.0f || e.maxStep > 0.5f) ++big[std::string(kPartName[e.cause]) + " | " + e.role];
    std::printf("   visible ones (|turn| >= 1 rad or a snap), by cause | role:\n");
    for (const auto& [k, cnt] : big) std::printf("     %-60s %d\n", k.c_str(), cnt);
    // The worst: by peak speed with |turn| >= 1 rad, and the biggest snaps.
    {
        const char* names[Result::kWhatIfs] = {"as is", "- energy mix on the heading", "  - cuts", "  - crossfade flips",
                                               "  - inertial (not a fix: it hides switches)"};
        std::printf("   what if (parts removed, cumulative): frames > 1.5 t/s %%, events / d-min, visible / d-min\n");
        for (int q = 0; q < Result::kWhatIfs; ++q)
            std::printf("     %-30s %7.3f %%  %6.2f  %6.2f\n", names[q],
                        100.0 * double(r.wiFast[q]) / double(std::max(1L, r.frames)), double(r.wiEvents[q]) / mins,
                        double(r.wiVisible[q]) / mins);
    }
    std::printf("   source heading jumping whole turns: %.2f / dancer-min; source switches %.2f / dancer-min, %ld of them "
                "the frame after such a jump\n",
                double(r.rawTurnJumps) / mins, double(r.switches) / mins, r.switchAfterJump);
    std::printf("   routine churn (a different routine two or more frames running): %ld frames (%.1f s), near a bust %ld; "
                "three or more: %ld frames\n",
                r.churnFrames, double(r.churnFrames) / kFps, r.churnBust, r.churn3Frames);
    std::printf("   facing away (more than 90 degrees from the camera): busting a move %.1f %% of %.0f s, everyone %.1f %%\n",
                100.0 * double(r.bustAway) / double(std::max(1L, r.bustFrames)), double(r.bustFrames) / kFps,
                100.0 * double(r.awayFrames) / double(std::max(1L, r.frames)));
    for (int c = 0; c < kParts; ++c) {
        std::vector<Event> wc;
        for (const Event& e : r.events)
            if (e.cause == c) wc.push_back(e);
        std::sort(wc.begin(), wc.end(), [](const Event& a, const Event& b) {
            return std::fabs(a.angle) * a.peak > std::fabs(b.angle) * b.peak;
        });
        std::printf("   worst led by %s:\n", kPartName[c]);
        for (std::size_t i = 0; i < std::min<std::size_t>(5, wc.size()); ++i) {
            const Event& e = wc[i];
            std::printf("     t %7.2f #%d %-6s %-18s turn %+.2f in %.2f s pk %.2f t/s step %.2f | m %+.2f x %+.2f c %+.2f i %+.2f "
                        "f %+.2f e %+.2f | %s | %s\n",
                        e.t, e.dancer, e.style.c_str(), e.role.c_str(), e.angle, double(e.frames) / kFps, e.peak / T,
                        e.maxStep, e.part[0], e.part[1], e.part[2], e.part[3], e.part[4], e.part[5], e.move.c_str(),
                        e.extra.c_str());
        }
    }
    std::vector<Event> w = r.events;
    std::sort(w.begin(), w.end(), [](const Event& a, const Event& b) {
        return std::fabs(a.angle) * a.peak > std::fabs(b.angle) * b.peak;
    });
    std::printf("   worst (|turn| x peak):\n");
    for (std::size_t i = 0; i < std::min<std::size_t>(14, w.size()); ++i) {
        const Event& e = w[i];
        std::printf("     t %7.2f s (beat %.1f) #%d %-6s %-20s turn %+.2f rad in %.2f s, peak %.2f turns/s, max step %.2f "
                    "rad | parts: move %+.2f xfade %+.2f cut %+.2f inert %+.2f facing %+.2f energy %+.2f -> %s\n"
                    "         %s | %s | %s\n",
                    e.t, e.beat, e.dancer, e.style.c_str(), e.role.c_str(), e.angle, double(e.frames) / kFps,
                    e.peak / T, e.maxStep, e.part[0], e.part[1], e.part[2], e.part[3], e.part[4], e.part[5], kPartName[e.cause],
                    e.move.c_str(), e.source.c_str(), e.extra.c_str());
    }
    std::fflush(stdout);
}

}  // namespace

TEST_CASE("spin: measure body heading speed in mingle crowds" * doctest::skip()) {
    SpinLoaded loaded;
    const double secs = std::getenv("SPIN_SECONDS") ? std::atof(std::getenv("SPIN_SECONDS")) : 600;
    std::vector<Scenario> scs = {
        {"5 mingle", 5, secs, 1, 1, 90},
        {"9 mingle", 9, secs, 2, 1, 90},
        {"14 mingle", 14, secs, 3, 1, 90},
    };
    const char* only = std::getenv("SPIN_ONLY");
    std::FILE* dump = std::getenv("SPIN_DUMP") ? std::fopen(std::getenv("SPIN_DUMP"), "w") : nullptr;
    if (dump) std::fprintf(dump, "t,dancer,source,role,move,Y,F,A,S,M,Ypre,e,walk,ground\n");
    for (auto& sc : scs) {
        if (only && sc.name.find(only) == std::string::npos) continue;
        if (std::getenv("SPIN_SEED")) sc.seed = std::uint32_t(std::atoi(std::getenv("SPIN_SEED")));
        Result r = run(sc, dump);
        report(sc, r);
        if (const char* ev = std::getenv("SPIN_EVENTS")) {
            std::FILE* f = std::fopen(ev, "a");
            for (const Event& e : r.events)
                std::fprintf(f, "%s|%.3f|%d|%s|%s|%s|%s|%.3f|%.3f|%.3f|%d|%.3f|%.3f|%.3f|%.3f|%.3f|%.3f|%s|%s\n",
                             sc.name.c_str(), e.t, e.dancer, e.style.c_str(), e.role.c_str(), e.move.c_str(),
                             e.source.c_str(), e.angle, e.peak, e.maxStep, e.frames, e.part[0], e.part[1], e.part[2],
                             e.part[3], e.part[4], e.part[5], kPartName[e.cause], e.extra.c_str());
            std::fclose(f);
        }
    }
    if (dump) std::fclose(dump);
}

// Each clip's fixed turn to the audience (DanceClips faceAudience, Peter 2026-11-27): the share of samples facing more
// than 90 degrees from the camera before and after, per file.
TEST_CASE("spin: measure clips facing away" * doctest::skip()) {
    SpinLoaded loaded;
    int changed[2] = {}, total[2] = {};
    for (int i = 0; i < dance::clipCount(); ++i) {
        const dance::ClipInfo& ci = dance::clipInfo(i);
        const int f = ci.floorWork ? 1 : 0;
        ++total[f];
        const bool big = std::fabs(ci.yawOffset) > 0.35f && ci.awayBefore - ci.awayAfter > 0.05f;
        changed[f] += big;
        if (f || big || ci.beats >= 64)
            std::printf("  %-50s %3d beats: offset %+5.0f deg, away %4.0f %% -> %4.0f %%\n", ci.label.c_str(), ci.beats,
                        ci.yawOffset * 57.2958f, 100 * ci.awayBefore, 100 * ci.awayAfter);
    }
    std::printf("turned noticeably (over 20 degrees, 5 points less facing away): standing file %d of %d, floor file %d of "
                "%d\n",
                changed[0], total[0], changed[1], total[1]);
}

// The harness's key results as checks (spin glitches fixed, Peter 2026-11-27): a mingle crowd with busts, every style.
TEST_CASE("spin: no routine churn, no heading snaps, nothing fast but the moves' own spins; busts face the camera") {
    SpinLoaded loaded;
    for (const Scenario& sc : {Scenario{"9 mingle", 9, 200, 7, 1, 40}, Scenario{"14 mingle", 14, 120, 3, 1, 25}}) {
        CAPTURE(sc.name);
        Result r = run(sc, nullptr);
        CHECK(r.churn3Frames == 0);  // (a routine picked again and again: Bust a move's start and end, floor waits)
        int snaps = 0, fastNotMove = 0;
        float worst = 0;
        for (const Event& e : r.events) {
            // One-frame jumps of the heading (the energy mix, the crossfade's flip, cuts, watchers' changes).
            snaps += e.maxStep > 0.5f;
            // Over 3.5 turns a second led by anything but the move itself (inertial blending, crossfades: up to ~3.3).
            if (e.cause != kMove && e.peak > 3.5f * kTau) {
                ++fastNotMove;
                std::printf("  fast, led by %s: t %.2f #%d %.2f turns/s, %s\n", kPartName[e.cause], e.t, e.dancer,
                            e.peak / kTau, e.move.c_str());
            }
            if (e.cause != kMove) worst = std::max(worst, e.peak / kTau);
        }
        MESSAGE(sc.name << ": " << r.events.size() << " events over 1.5 turns/s, fastest not led by the move " << worst
                        << " turns/s; busting a move " << double(r.bustFrames) / kFps << " s, facing away "
                        << 100.0 * double(r.bustAway) / double(std::max(1L, r.bustFrames)) << " %");
        CHECK(snaps == 0);
        CHECK(fastNotMove == 0);
        REQUIRE(r.bustFrames > 0);
        CHECK(double(r.bustAway) < 0.25 * double(r.bustFrames));  // (about 60 % before each clip faced the audience)
    }
}
