#pragma once
// reforce extension (PLAN 5O.5): breakdowns, build-ups and drops, from the bass on the beat clock.
//
//   breakdown  the per-beat bass falls below `breakdownLevel` x its usual level (a slow average over full beats)
//              for `breakdownBeats` beats in a row
//   build      during a breakdown: 0 at its start, rising to 1 over `buildBeats` beats (the tension a scene can use)
//   drop       during a breakdown, the bass (averaged over about half a beat, updated every step, so no waiting for the
//              beat to end; single kicks inside a breakdown do not count) comes back `dropFraction` of the way from
//              the breakdown's own level to the usual one, after at least `minBreakdownBeats`: the drop fires at once,
//              and the detector rests `restBeats` beats
// BASS is the whole mix's low band (kick included) and fairly smooth: on a house track (tools: the demucs stems of
// "Lose Control") a drum breakdown measured ~55 % of the usual level and a bass-only gap barely moved it, hence
// the defaults.
//
// Outputs SYNC_BUILD (0..1) and SYNC_DROP (1 at a drop, decaying over one bar). Without a beat clock the beats are
// counted at 120 BPM. Single-threaded (the render thread, once per sim step), pure (unit-tested without audio).

#include "sync/BeatClock.hpp"

namespace reforce::sync {

struct DropConfig {
    double breakdownLevel = 0.65;  // per-beat bass below this x usual ...
    int breakdownBeats = 4;        // ... for this many beats: a breakdown
    double dropFraction = 0.6;     // the half-beat average back this far from the breakdown's level to the usual one
    int minBreakdownBeats = 8;     // a breakdown shorter than this cannot drop (the bass wobbling in a breakdown)
    double buildBeats = 32;        // build rises 0 -> 1 over this many beats of breakdown
    double usualBeats = 32;        // time constant of the usual bass level, beats (frozen during a breakdown)
    int restBeats = 8;             // no new breakdown this soon after a drop
    double minUsual = 0.05;        // below this the music is too quiet to judge
};

struct DropState {
    bool breakdown = false;
    double build = 0;          // 0..1
    double drop = 0;           // 1 at a drop, decaying over a bar
    int drops = 0;             // drops so far
    double lastDropBeat = -1;  // beat-clock beat of the last drop (-1: none)
    double usual = 0;          // the usual per-beat bass
};

class DropDetector {
public:
    explicit DropDetector(DropConfig cfg = {}) : cfg_(cfg) {}
    // One sim step of dt seconds: the beat clock's state after its update, and the bass now (BASS).
    const DropState& update(double dt, const BeatClockState& clock, double bass);
    const DropState& state() const { return state_; }
    // True on the step a drop fired.
    bool dropped() const { return fired_; }

private:
    DropConfig cfg_;
    DropState state_;
    BeatSource source_ = BeatSource::None;
    double beat_ = 0, ownBeat_ = 0, bpm_ = 120;
    long long curBeat_ = 0;
    bool have_ = false;
    double sum_ = 0;
    int count_ = 0;
    int quietBeats_ = 0;
    double breakdownStart_ = 0;
    double restUntil_ = -1e300;
    bool fired_ = false;
    double bdSum_ = 0;
    int bdCount_ = 0;  // per-beat bass during the breakdown
    double ema_ = 0;  // bass averaged over about half a beat
};

}  // namespace reforce::sync
