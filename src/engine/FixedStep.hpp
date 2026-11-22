#pragma once
// Fixed simulation tick. G-Force's behavior is per frame (decay 1/255 per
// frame, B-var counters per frame; probe #4, #6) at Graphics.TargetFrameRate,
// so the simulation advances in steps of 1/rate whatever the display refresh
// (the window may run at 120 Hz). An accumulator collects real time; each
// display frame runs the whole steps it holds, at most `maxSteps` (catch-up
// cap: after a stall the extra time is dropped rather than replayed).

#include <algorithm>
#include <cmath>

namespace reforce::engine {

class FixedStepClock {
public:
    explicit FixedStepClock(double rate = 60.0, int maxSteps = 4)
        : step_(1.0 / (rate > 0 ? rate : 60.0)), maxSteps_(std::max(1, maxSteps)) {}

    double stepSeconds() const { return step_; }
    double rate() const { return 1.0 / step_; }

    // Adds `realDt` seconds of wall time; returns the number of simulation
    // steps to run now (0..maxSteps).
    int advance(double realDt) {
        if (!(realDt > 0)) return 0;
        acc_ += std::min(realDt, 0.5);  // ignore huge gaps (debugger, sleep)
        // Small tolerance so a 120 Hz display reliably yields a step every
        // second frame despite timestamp jitter / rounding.
        const double eps = step_ * 1e-3;
        int n = 0;
        while (acc_ + eps >= step_ && n < maxSteps_) {
            acc_ -= step_;
            ++n;
        }
        if (acc_ + eps >= step_) acc_ = 0;  // over the cap: drop the backlog
        if (acc_ < 0) acc_ = 0;
        steps_ += n;
        return n;
    }

    // Vsync lock (Phase 5D): frame callbacks come every `callbackPeriod`
    // seconds (display link pinned to the sim rate, every refresh of a 120 Hz
    // screen, or the fallback timer). When that period is a whole fraction or
    // multiple of the step (within 0.5 %), a callback adds exactly the nominal
    // time of the callback periods that elapsed (rounded), so jitter and a
    // slightly-off refresh (59.94 Hz) never shift the accumulator phase: 60 Hz
    // on 120 Hz is an exact 0,1,0,1 step pattern, 60 on 60 always 1. A real
    // stall (several periods) still adds its whole periods (catch-up). Other
    // periods: plain wall time.
    int advanceLocked(double realDt, double callbackPeriod) {
        return advance(lockedDt(realDt, callbackPeriod));
    }
    double lockedDt(double realDt, double callbackPeriod) const {
        if (!(callbackPeriod > 0) || !(realDt > 0)) return realDt;
        const double r = callbackPeriod / step_;  // 0.5 at 120 Hz, 1 at 60 Hz
        double nominal = 0;
        if (r <= 1) {
            const double k = std::round(1.0 / r);
            if (k >= 1 && std::abs(1.0 / r - k) < 0.005 * k) nominal = step_ / k;
        } else {
            const double m = std::round(r);
            if (std::abs(r - m) < 0.005 * m) nominal = step_ * m;
        }
        if (nominal <= 0) return realDt;
        const double n = std::round(realDt / nominal);
        if (n >= 1 && std::abs(realDt - n * nominal) < 0.25 * nominal) return n * nominal;
        return realDt;
    }

    long long totalSteps() const { return steps_; }
    double simTime() const { return double(steps_) * step_; }
    double backlog() const { return acc_; }  // seconds not yet simulated

private:
    double step_;
    int maxSteps_;
    double acc_ = 0;
    long long steps_ = 0;
};

}  // namespace reforce::engine
