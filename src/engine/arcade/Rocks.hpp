#pragma once
// Arcade organic `rocks` (PLAN 5Q.5, docs/arcade-design.md): what a rocks body reports about its game, for the tests
// (hits on beat lines, splits, waves, the drop's shockwave, bounded counts) and the log.
#include <limits>
#include <vector>

#include "engine/OrganicKinds.hpp"

namespace reforce::engine::arcade {

struct RocksStats {
    int rocks = 0, large = 0, medium = 0, small = 0;  // alive now, by size
    int debris = 0, shots = 0;                        // fragments and sparks flying, shots in the air
    long long fired = 0, hits = 0, chips = 0, waves = 0, shattered = 0;
    // The latest events (up to kKeep each), in beats of the clock they happened on.
    static constexpr std::size_t kKeep = 256;
    std::vector<double> hitBeats;   // a shot split or burst a rock
    std::vector<int> hitSizes;      // the size of the rock it hit (0 large, 1 medium, 2 small), per hitBeats
    std::vector<double> fireBeats;  // a shot left the ship
    std::vector<double> waveBeats;  // fresh large rocks started drifting in
    double shockBeat = std::numeric_limits<double>::quiet_NaN();  // the latest drop's shockwave
    int fireStepTicks = 0;  // the fire grid now, in ticks (kTicksPerBeat / shots per beat)
    int lastStepTicks = 0;  // ticks the latest step advanced (bounded catch-up)
};

inline constexpr int kRocksTicksPerBeat = 48;  // simulation ticks per beat: divides every fire and hit grid

// A rocks body's stats; null for another kind.
const RocksStats* rocksStats(const OrganicBody& body);

}  // namespace reforce::engine::arcade
