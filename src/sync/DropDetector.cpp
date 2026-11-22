#include "sync/DropDetector.hpp"

#include <algorithm>
#include <cmath>

namespace reforce::sync {

const DropState& DropDetector::update(double dt, const BeatClockState& clock, double bass) {
    fired_ = false;
    if (!std::isfinite(bass) || bass < 0) bass = 0;
    if (!(dt >= 0) || !std::isfinite(dt)) dt = 0;
    // The beat count: the clock's when it has a source, else our own at 120 BPM. A new source restarts the
    // per-beat bookkeeping (the usual level carries over: it is the same music).
    if (clock.source != BeatSource::None) {
        if (clock.source != source_) have_ = false;
        beat_ = clock.beat;
        if (clock.bpm > 0) bpm_ = clock.bpm;
    } else {
        if (source_ != BeatSource::None) have_ = false;
        ownBeat_ += dt * 2.0;
        beat_ = ownBeat_;
        bpm_ = 120;
    }
    source_ = clock.source;
    // The drop pulse: down to 1/e over one bar.
    state_.drop *= std::exp(-dt / std::max(0.1, 4.0 * 60.0 / bpm_));

    const long long b = (long long)std::floor(beat_);
    if (!have_) {
        curBeat_ = b;
        have_ = true;
        sum_ = 0;
        count_ = 0;
    }
    if (b != curBeat_) {
        if (count_ > 0 && b > curBeat_ && b - curBeat_ <= 8) {
            const double e = sum_ / count_;
            const double usual = state_.usual;
            if (!state_.breakdown) {
                // The usual level follows full beats only (a quiet beat is not "usual").
                const double k = 1.0 - std::exp(-1.0 / std::max(1.0, cfg_.usualBeats));
                if (usual <= 0) state_.usual = e;
                else if (e >= cfg_.breakdownLevel * usual) state_.usual += k * (e - usual);
                const bool quiet = usual >= cfg_.minUsual && e < cfg_.breakdownLevel * usual;
                quietBeats_ = quiet ? quietBeats_ + 1 : 0;
                if (quietBeats_ >= cfg_.breakdownBeats && beat_ >= restUntil_) {
                    state_.breakdown = true;
                    breakdownStart_ = double(curBeat_ - cfg_.breakdownBeats + 1);
                    bdSum_ = 0;
                    bdCount_ = 0;
                }
            } else {
                bdSum_ += e;
                ++bdCount_;
            }
        }
        curBeat_ = b;
        sum_ = 0;
        count_ = 0;
    }
    sum_ += bass;
    ++count_;
    ema_ += (1.0 - std::exp(-2.0 * dt * bpm_ / 60.0)) * (bass - ema_);  // time constant: half a beat
    if (state_.breakdown) {
        state_.build = std::clamp((beat_ - breakdownStart_) / std::max(1.0, cfg_.buildBeats), 0.0, 1.0);
        const double level = bdCount_ > 0 ? bdSum_ / bdCount_ : cfg_.breakdownLevel * state_.usual;
        const double threshold = level + cfg_.dropFraction * (state_.usual - level);
        if (beat_ - breakdownStart_ >= cfg_.minBreakdownBeats && ema_ > threshold) {
            state_.breakdown = false;
            state_.build = 0;
            state_.drop = 1;
            ++state_.drops;
            state_.lastDropBeat = beat_;
            restUntil_ = beat_ + cfg_.restBeats;
            quietBeats_ = 0;
            fired_ = true;
        }
    } else {
        state_.build = 0;
    }
    return state_;
}

}  // namespace reforce::sync
