#pragma once
// Slideshow scheduling (scene-format §6, §7): per-layer shuffled playlists and
// interval timers. Pure logic driven by an explicit clock (the simulation time),
// so tests can run it with a fake clock.
//
//   Playlist        shuffled order over the eligible configs, reshuffled when
//                   exhausted, never the same config twice in a row; history
//                   for "previous".
//   LayerSlideshow  `<Type>.Slideshow.Interval.Duration` (+- Deviation, a
//                   relative standard deviation), counted from the END of one
//                   change to the start of the next; transition durations
//                   (+- Deviation) and the quick duration for manual switches.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "engine/Prefs.hpp"
#include "expr/Rng.hpp"

namespace reforce::engine {

// Deterministic random source for scheduling decisions.
class SchedRng {
public:
    explicit SchedRng(std::uint64_t seed = 1) : pcg_(seed ^ 0x5eed5c4edULL) {}
    double uniform() { return pcg_.uniform01Exclusive(); }
    std::size_t index(std::size_t n) { return n ? std::size_t(uniform() * double(n)) % n : 0; }
    double normal();  // N(0,1), Box-Muller

private:
    expr::Pcg32 pcg_;
    std::optional<double> spare_;
};

// Names in parentheses are only started by script/hand, never by the
// slideshow (scene-format §5.2, [VH 3.1]).
bool excludedFromSlideshow(const std::string& name);

class Playlist {
public:
    // Eligible names for the shuffle (callers filter parenthesized names and
    // themes first). Keeps `current` when it is still present.
    void setItems(std::vector<std::string> names);
    const std::vector<std::string>& items() const { return items_; }
    bool empty() const { return items_.empty(); }

    // Next config of the shuffle. Never returns `current()` unless it is the
    // only item. Returns "" when empty.
    std::string next(SchedRng& rng);
    // Peeks at what next() will return (the shuffle is drawn lazily, so this
    // fixes it).
    std::string peek(SchedRng& rng);
    // The config shown before the current one (history); without history the
    // alphabetically previous item.
    std::string previous();

    // Records a config that became current (any source: shuffle, manual, CLI).
    void noteCurrent(const std::string& name);
    const std::string& current() const { return current_; }

private:
    void reshuffle(SchedRng& rng);
    std::vector<std::string> items_, order_;
    std::size_t pos_ = 0;
    std::string current_;
    std::vector<std::string> history_;
};

class LayerSlideshow {
public:
    LayerSlideshow() = default;
    explicit LayerSlideshow(const LayerPrefs& prefs)
        : interval_(prefs.slideshowInterval), intervalDev_(prefs.slideshowDeviation),
          duration_(prefs.transitionDuration), durationDev_(prefs.transitionDeviation),
          quick_(prefs.transitionQuick), enabled_(prefs.slideshowOnStartup) {}

    bool enabled() const { return enabled_; }
    void setEnabled(bool on) { enabled_ = on; }
    bool held() const { return held_; }
    void setHeld(bool h) { held_ = h; }
    bool running() const { return enabled_ && !held_; }

    double interval() const { return interval_; }
    // 0 is kept (Particle/Sprite timers: the slideshow starts none); the Director
    // refuses 0 for the switching layers.
    void setInterval(double s) { interval_ = s >= 0 ? s : interval_; }
    double transitionDuration() const { return duration_; }
    void setTransitionDuration(double s) { duration_ = s >= 0 ? s : duration_; }
    double quickDuration() const { return quick_; }

    // Starts the interval countdown at `now` with a sampled interval.
    void arm(double now, SchedRng& rng);
    void disarm() { armed_ = false; }
    bool armed() const { return armed_; }
    double dueAt() const { return dueAt_; }
    // True when armed, running and the interval has elapsed.
    bool due(double now) const { return armed_ && running() && now >= dueAt_; }

    double sampleInterval(SchedRng& rng) const;
    double sampleTransitionDuration(SchedRng& rng) const;

private:
    double interval_ = 20, intervalDev_ = 0.1, duration_ = 10, durationDev_ = 0.02, quick_ = 0.9;
    bool enabled_ = true, held_ = false, armed_ = false;
    double dueAt_ = 0;
};

}  // namespace reforce::engine
