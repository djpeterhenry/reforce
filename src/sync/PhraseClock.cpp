#include "sync/PhraseClock.hpp"

#include <algorithm>
#include <cmath>

namespace reforce::sync {

namespace {
double posMod(double a, double m) {
    const double r = std::fmod(a, m);
    return r < 0 ? r + m : r;
}
}  // namespace

void PhraseClock::setConfig(const PhraseConfig& cfg) {
    const bool lengthChanged = std::clamp(cfg.bars, 1, 64) != std::clamp(cfg_.bars, 1, 64);
    cfg_ = cfg;
    cfg_.bars = std::clamp(cfg_.bars, 1, 64);
    if (lengthChanged) {
        // Keep the manual / auto origin's position within a bar; the histogram is for the old length.
        hist_.assign(std::size_t(length()), 0.0);
        origin_ = posMod(origin_, double(length()));
        if (how_ == "auto") {
            how_ = "none";
            confidence_ = 0;
        }
    }
    if (!cfg_.autoAlign && how_ == "auto") {
        how_ = "none";
        origin_ = 0;
        confidence_ = 0;
    }
    place();
}

int PhraseClock::length() const {
    return std::max(1, int(std::lround(std::clamp(cfg_.bars, 1, 64) * std::max(1.0, quantum_))));
}

void PhraseClock::reset() {
    origin_ = 0;
    how_ = "none";
    deck_ = -1;
    confidence_ = 0;
    hist_.assign(std::size_t(length()), 0.0);
    recent_.clear();
    haveBeat_ = false;
    sum_ = 0;
    count_ = 0;
    place();
}

void PhraseClock::startNow() {
    if (source_ == BeatSource::None) return;
    const double unit = bars_ ? quantum_ : 1.0;
    origin_ = posMod(std::round(beat_ / unit) * unit, double(length()));
    how_ = "manual";
    deck_ = -1;
    confidence_ = 1;
    if (hist_.size() != std::size_t(length())) hist_.assign(std::size_t(length()), 0.0);
    hist_[std::size_t(std::lround(origin_)) % hist_.size()] += cfg_.manualVote;
    place();
}

void PhraseClock::startAt(double beat, int deck) {
    if (source_ == BeatSource::None || !std::isfinite(beat)) return;
    // Songs start on a downbeat: with Link, the nearest bar line (rkbx_link keeps Link's bars on the master deck's).
    const double unit = bars_ ? quantum_ : 1.0;
    origin_ = posMod(std::round(beat / unit) * unit, double(length()));
    how_ = "deck";
    deck_ = deck;
    confidence_ = 1;
    // The music's own evidence starts over from here (auto takes over again only without deck starts).
    hist_.assign(std::size_t(length()), 0.0);
    hist_[std::size_t(std::lround(origin_)) % hist_.size()] += cfg_.manualVote;
    place();
}

void PhraseClock::hint(double beat, double weight) {
    if (source_ == BeatSource::None || hist_.empty() || !(weight > 0) || !std::isfinite(beat)) return;
    const double unit = bars_ ? quantum_ : 1.0;
    vote((long long)std::llround(std::round(beat / unit) * unit), weight);
    pickAuto();
    place();
}

void PhraseClock::nudge(int bars) {
    origin_ = posMod(origin_ + double(bars) * quantum_, double(length()));
    if (how_ != "manual") {
        how_ = "manual";
        deck_ = -1;
        confidence_ = 1;
    }
    if (hist_.size() == std::size_t(length())) hist_[std::size_t(std::lround(origin_)) % hist_.size()] += cfg_.manualVote;
    place();
}

void PhraseClock::vote(long long beatIndex, double score) {
    const long long L = length();
    hist_[std::size_t(((beatIndex % L) + L) % L)] += score;
}

void PhraseClock::pickAuto() {
    if (!cfg_.autoAlign || hist_.empty() || how_ == "deck") return;
    const int L = int(hist_.size());
    const int step = bars_ ? std::max(1, int(std::lround(quantum_))) : 1;  // Link: phrases start on bar lines
    // Candidate score: the bin plus half its neighbors (a change smeared over two beats still counts).
    auto score = [&](int i) {
        return hist_[std::size_t(i)] + 0.5 * (hist_[std::size_t((i + 1) % L)] + hist_[std::size_t((i + L - 1) % L)]);
    };
    int best = -1;
    double bestS = 0;
    for (int i = 0; i < L; i += step)
        if (score(i) > bestS) {
            bestS = score(i);
            best = i;
        }
    if (best < 0) return;
    double second = 0;
    for (int i = 0; i < L; i += step) {
        const int d = std::min(std::abs(i - best), L - std::abs(i - best));
        if (d > 1) second = std::max(second, score(i));
    }
    if (bestS < cfg_.minPeak || bestS < cfg_.peakRatio * second) return;
    if (how_ == "manual" || how_ == "auto") {
        const int cur = int(std::lround(origin_)) % L;
        if (cur == best) {
            if (how_ == "auto") confidence_ = bestS / (bestS + second);
            return;
        }
        if (bestS < cfg_.switchRatio * score(cur)) return;
    }
    origin_ = double(best);
    how_ = "auto";
    confidence_ = bestS / (bestS + second);
}

const PhraseState& PhraseClock::update(const BeatClockState& clock, double loudness) {
    if (clock.source != source_) {
        // A different count: the origin and the loudness history mean nothing on it.
        source_ = clock.source;
        quantum_ = clock.quantum > 0 ? clock.quantum : 4.0;
        bars_ = clock.bars;
        beat_ = clock.beat;
        reset();
        return state_;
    }
    if (clock.quantum > 0 && clock.quantum != quantum_) {
        quantum_ = clock.quantum;
        hist_.assign(std::size_t(length()), 0.0);
    }
    bars_ = clock.bars;
    beat_ = clock.beat;
    if (source_ == BeatSource::None) {
        place();
        return state_;
    }
    if (hist_.size() != std::size_t(length())) hist_.assign(std::size_t(length()), 0.0);
    if (!std::isfinite(loudness) || loudness < 0) loudness = 0;

    // Per-beat loudness; score the boundary two beats back once its two "after" beats are in.
    const long long b = (long long)std::floor(beat_);
    if (!haveBeat_) {
        curBeat_ = b;
        haveBeat_ = true;
    }
    if (b != curBeat_) {
        if (b < curBeat_ || b - curBeat_ > 8) {  // jumped: start the history over
            recent_.clear();
        } else if (count_ > 0) {
            const double e = sum_ / count_;
            // Beats skipped inside one step (very fast tempo / long step) repeat the value.
            for (long long k = curBeat_; k < b; ++k) {
                recent_.push_back(e);
                recentLast_ = k;
                if (recent_.size() > 6) recent_.erase(recent_.begin());
                // Forget a little every beat.
                const double keep = std::exp(-1.0 / (std::max(0.5, cfg_.memoryPhrases) * length()));
                for (double& h : hist_) h *= keep;
                if (recent_.size() == 6) {
                    const double eps = 0.02;
                    const double before = (recent_[0] + recent_[1] + recent_[2] + recent_[3]) / 4.0;
                    const double after = (recent_[4] + recent_[5]) / 2.0;
                    const double s = std::abs(std::log((after + eps) / (before + eps)));
                    if (s >= cfg_.minScore) vote(recentLast_ - 1, s);  // the boundary: first of the two "after" beats
                }
            }
            pickAuto();
        }
        curBeat_ = b;
        sum_ = 0;
        count_ = 0;
    }
    sum_ += loudness;
    ++count_;
    place();
    return state_;
}

void PhraseClock::place() {
    state_.on = source_ != BeatSource::None;
    state_.bars = std::clamp(cfg_.bars, 1, 64);
    state_.quantum = quantum_;
    state_.origin = origin_;
    state_.how = how_;
    state_.deck = how_ == "deck" ? deck_ : -1;
    state_.aligned = how_ != "none";  // auto, manual, deck
    state_.confidence = confidence_;
    if (!state_.on) {
        state_.phrase = state_.bar = 0;
        state_.barInPhrase = 0;
        return;
    }
    state_.bar = (beat_ - origin_) / quantum_;
    state_.phrase = (beat_ - origin_) / double(length());
    state_.barInPhrase = std::clamp(int(std::floor(posMod(state_.bar, double(state_.bars)))), 0, state_.bars - 1);
}

}  // namespace reforce::sync
