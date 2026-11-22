#include "engine/MovieBeatSync.hpp"

#include <algorithm>
#include <cmath>

namespace reforce::engine {

namespace {
// x wrapped into [-p/2, p/2)
double wrapCentered(double x, double p) { return x - p * std::floor(x / p + 0.5); }
}  // namespace

MovieBeatSync::MovieBeatSync(const ClipBeats& clip, double startMedia, double nativeRate, BeatSyncConfig cfg)
    : clip_(clip), cfg_(cfg), native_(std::max(0.0, nativeRate)), u_(startMedia) {
    clip_.beatsPerBar = std::max(1, clip_.beatsPerBar);
}

double MovieBeatSync::pickRatio(double musicBpm) const {
    static const double kRatios[] = {0.5, 1.0, 2.0};
    auto cost = [&](double r) { return std::abs(std::log(musicBpm * r / clip_.bpm)); };
    double best = r_;
    for (double r : kRatios)
        if (cost(r) < cost(best)) best = r;
    // Stay with the current octave unless the best is clearly better (no flapping around a boundary).
    if (synced_ && best != r_ && cost(r_) - cost(best) < cfg_.switchMargin) return r_;
    return best;
}

double MovieBeatSync::step(double dt, const BeatSample& c, bool enabled) {
    dt = std::max(0.0, dt);
    const bool sameSource = c.source == lastSource_;
    const double lastBeat = lastBeat_;
    lastBeat_ = c.beat;
    lastSource_ = c.source;
    bool sync = enabled && clip_.bpm > 0 && c.confident && c.bpm > 0;
    double r = r_;
    if (sync) {
        r = pickRatio(c.bpm);
        const double rate = c.bpm * r / clip_.bpm;
        if (rate < cfg_.minRate || rate > cfg_.maxRate) sync = false;
    }
    if (!sync) {
        synced_ = false;
        rate_ = native_;
        u_ += dt * native_;
        return u_;
    }
    const double spb = 60.0 / clip_.bpm;  // media seconds per movie beat
    // Music beats this step: the clock's own delta when it is the same count and sane, else from its tempo.
    // A jump in the count (a Link re-sync, a source switch) is a phase error to slew, not motion to play.
    const double expected = dt * c.bpm / 60.0;
    double db = c.beat - lastBeat;
    if (!sameSource || !synced_ || std::abs(db - expected) > 0.6 * expected + 0.01) db = expected;
    r_ = r;  // an octave switch keeps the media time continuous; the phase loop re-aligns at the new ratio
    double du = db * r * spb;
    // Phase: movie position vs music position, in movie beats (a bar with Link's bars, else one beat).
    const double period = c.bars ? double(clip_.beatsPerBar) : 1.0;
    const double moviePos = (u_ + du - clip_.beatStart) / spb;  // where this step lands
    const double musicPos = r * c.beat;
    const double err = wrapCentered(musicPos - moviePos, period);
    err_ = err;
    if (!everSynced_) {
        // First synced step: start in phase, jumping forward up to one period (never back).
        double fwd = err;
        if (fwd < 0) fwd += period;
        u_ += fwd * spb;
        everSynced_ = true;
        err_ = 0;
    } else {
        double corr = err * spb * std::min(1.0, dt / cfg_.slewSeconds);
        const double lim = cfg_.maxSlew * std::max(du, dt * spb * 0.25);
        corr = std::clamp(corr, -lim, lim);
        du = std::max(0.0, du + corr);
    }
    u_ += du;
    rate_ = dt > 0 ? du / dt : 0.0;
    synced_ = true;
    return u_;
}

}  // namespace reforce::engine
