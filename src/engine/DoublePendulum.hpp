#pragma once
// Organic kind `double` (PLAN 5O.10, docs/rhythm-design.md "Pendulum Double"): chaotic double pendulums released
// together on the phrase line. Num double pendulums start from one pose, each a tiny angle apart; for the first bars
// they swing as one, then chaos fans them out into a bloom of diverging arms and tip traces; over the last bar of the
// cycle they are gathered back to the start pose, which they reach exactly on the next cycle line, and are released
// again.
//
// The physics is the standard double pendulum (two point masses on massless rods), integrated with RK4 in fixed steps
// of 1/48 beat from the cycle line, so the state at a clock position does not depend on the frame rate and runs
// repeat. Time is in beats: g / L is chosen at load so the first swing out and back takes `Prd` beats (2), so the
// swing follows the tempo. The spread between the pendulums at release is chosen at load (from the growth of a tiny
// difference along the reference trajectory) so the fan opens about `Div` bars after the release.

#include <cstdint>
#include <memory>
#include <vector>

#include "engine/OrganicKinds.hpp"

namespace reforce::engine::pendulum2 {

constexpr int kStepsPerBeat = 48;
constexpr int kMaxNum = 32;
// Pendulum-steps (one RK4 step of one pendulum) integrated per frame at most: a jump far into a cycle catches up over
// a few frames (fast-forwarding) instead of stalling one.
constexpr int kMaxStepsPerFrame = 12000;

struct State {
    double t1 = 0, t2 = 0, w1 = 0, w2 = 0;  // angles from straight down (radians, + = counterclockwise), rad / beat
};

// Load-time keys (docs/scene-format.md 5.8), clamped.
struct Params {
    int num = 16;            // pendulums (1..32)
    int view = 0;            // 0: all from one pivot, 1: a row of pivots across Sprd
    double cycBars = 16;     // bars per cycle (release, diverge, gather), on the phrase-aligned bar clock
    double back = 4;         // beats of the gather at the end of each cycle
    double buildGather = 0;  // BldG: a build lengthens the gather by up to this x Back (0: always just Back; Peter
                             // 2026-11-26: gathered only in the last bar)
    double period = 2;       // beats of the first swing out and back from the start pose
    double th1 = 120, th2 = 150;  // start pose, degrees from straight down (+ = counterclockwise)
    double div = 6;          // bars from the release until the fan opens visibly
    double lenRatio = 1;     // lower arm / upper arm
    double massRatio = 1;    // lower bob / upper bob
    double kick = 0.25;      // rad / beat added to the upper arms on every beat line, along their motion
    double drop = 2.5;       // rad / beat a drop adds to the upper arms, along their motion
    double len = 0.92;       // reach of a pendulum (both arms), tree units
    double spread = 3.2;     // with View 1: the row's width
    double trail = 1.5;      // beats of tip trace (0 = none; at most 16)
    double width = 2;        // arm width (LWdt)
    double bobW = 7;         // tip bob diameter (LWdt; 0 = none)
    double jointW = 3;       // elbow dot diameter (LWdt; 0 = none)
    double traceW = 1.5;     // trace width (LWdt)
    double shade = 0.45;     // the pendulums' intensities spread over 1 - Shd .. 1
};
Params paramsFrom(const scene::OrganicInstance& in);

// The equations of motion for unit upper arm and bob: lower arm `r`, lower bob `mu`, gravity `g` (per beat^2), plus
// an angular acceleration `tau` on the upper arm (the kicks). Returns d/dt of the state.
State deriv(const State& s, double g, double r, double mu, double tau = 0);
// Total energy (kinetic + potential, the upper bob's mass 1, pivot at 0).
double energy(const State& s, double g, double r, double mu);
// One RK4 step of `h` beats; `tauAt(x, state)` is the angular acceleration added to the upper arm at x in [0, h] from
// the step's start, in that state.
template <class Tau>
State rk4(const State& s, double h, double g, double r, double mu, Tau&& tauAt) {
    auto add = [](const State& a, const State& d, double k) {
        return State{a.t1 + k * d.t1, a.t2 + k * d.t2, a.w1 + k * d.w1, a.w2 + k * d.w2};
    };
    const State k1 = deriv(s, g, r, mu, tauAt(0.0, s));
    const State s2 = add(s, k1, 0.5 * h);
    const State k2 = deriv(s2, g, r, mu, tauAt(0.5 * h, s2));
    const State s3 = add(s, k2, 0.5 * h);
    const State k3 = deriv(s3, g, r, mu, tauAt(0.5 * h, s3));
    const State s4 = add(s, k3, h);
    const State k4 = deriv(s4, g, r, mu, tauAt(h, s4));
    return {s.t1 + h / 6 * (k1.t1 + 2 * k2.t1 + 2 * k3.t1 + k4.t1),
            s.t2 + h / 6 * (k1.t2 + 2 * k2.t2 + 2 * k3.t2 + k4.t2),
            s.w1 + h / 6 * (k1.w1 + 2 * k2.w1 + 2 * k3.w1 + k4.w1),
            s.w2 + h / 6 * (k1.w2 + 2 * k2.w2 + 2 * k3.w2 + k4.w2)};
}

// One cycle's motion: every pendulum from the release (step 0, the cycle line) on, in whole steps.
class Cycle {
public:
    explicit Cycle(const Params& p);

    const Params& params() const { return p_; }
    double gravity() const { return g_; }        // per beat^2 (unit upper arm)
    double offset() const { return eps_; }      // radians between neighbors at the release
    double cycleBeats() const { return cycBeats_; }
    int substeps() const { return substeps_; }

    // The release pose of pendulum k (at rest).
    State pose(int k) const;
    // Starts the cycle again from the release; `gather` = beats of this cycle's gather (Back, longer in a build).
    void reset(double gather);
    // Integrates whole steps up to `step` (from the cycle line), at most `budget` pendulum-steps; returns the step
    // reached. Kicks on beat lines until the gather; a drop recorded with setDrop at its step.
    long advance(long step, long budget = -1);
    long step() const { return step_; }
    // A drop's impulse at step `at` of this cycle (it takes effect when the integration passes it; -1 = none).
    void setDrop(long at) { dropStep_ = at; }
    long dropStep() const { return dropStep_; }
    double gatherBeats() const { return gather_; }
    // Changes this cycle's gather while its steps have not reached the gather; false when too late.
    bool setGather(double gather);
    // Pendulum k as shown `frac` (0..1) of a step after the current step: the free motion, gathered back to the
    // release pose over the gather (exactly the pose on the cycle line).
    State shown(int k, double frac) const;
    // Pendulum k's free (integrated) state at the current step.
    const State& free(int k) const { return st_[std::size_t(k)]; }
    // Its tip relative to its pivot, in units of the reach (|tip| <= 1).
    void tip(const State& s, double& x, double& y) const;
    // Called with every whole step reached (after it), for the traces: shown state of each pendulum at that step.
    template <class F>
    long advanceWith(long step, long budget, F&& onStep) {
        long b = budget;
        while (step_ < step && (b < 0 || b >= p_.num)) {
            stepOnce();
            if (b >= 0) b -= p_.num;
            onStep();
        }
        return step_;
    }

private:
    void stepOnce();
    State integrate(State s, int k, double u0, double h) const;  // pendulum k from beat u0 over h beats
    double kickPulse(double u) const;  // the kick's strength at beat u of the cycle (rad / beat^2)
    double push(const State& s) const;  // the way a kick or drop pushes: +-1 along the upper arm's motion
    double torque(const State& s, double u, int k) const;  // added angular acceleration of pendulum k's upper arm
    double gatherWeight(double u) const;

    Params p_;
    double g_ = 1, r_ = 1, mu_ = 1, eps_ = 0, cycBeats_ = 64, range_ = 1;
    int substeps_ = 1;
    double gather_ = 4;
    std::vector<State> st_;
    std::vector<double> m1_, m2_;  // the gather's target turns (2 pi multiples), chosen as the gather starts
    long step_ = 0;
    long dropStep_ = -1;
    std::vector<double> eRef_;  // each pendulum's energy level held (the release's, a drop's after one)
    bool holding_ = false;      // the energy is held (kicks on, or after a drop)
    bool targets_ = false;   // gather targets chosen
    bool calibrating_ = false;
};

// Beats from the release until the first swing out and back (the upper arm's velocity changes sign twice), for unit
// g; NaN if it never does.
double firstSwing(double th1, double th2, double r, double mu);

std::unique_ptr<OrganicBody> makeDoublePendulum(scene::OrganicInstance& inst, std::uint64_t seed);

}  // namespace reforce::engine::pendulum2
