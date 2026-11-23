#pragma once
// Arcade organics (PLAN 5Q, docs/arcade-design.md): game-inspired Organic kinds drawn as line art into the intensity
// buffer (so they trail in the FlowField and take the ColorMap), stepping on the beat clock. One file per game:
//
//   invaders   a formation of aliens marching one step per beat, a cannon below shooting them down (Invaders.cpp)
//   hopper     a character hopping one cube per beat over an isometric pyramid, flipping the tops (Hopper.cpp)
//   cycles     light cycles on a grid, turning only on beat lines, walls behind them (Cycles.cpp)
//   rocks      vector rocks drifting and splitting on the beat, a ship turning and firing (Rocks.cpp)
//   bricks     a ball bouncing on the beat lines, clearing a wall of bricks by the phrase line (Bricks.cpp)
//   chase      the light-cycle game in 3D, a camera chasing one cycle across the grid (Chase.cpp)
//
// Our own designs: inspired by the games' mechanics, never their art, characters or names.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>

#include "engine/OrganicKinds.hpp"

namespace reforce::engine::arcade {

std::unique_ptr<OrganicBody> makeInvaders(scene::OrganicInstance& inst, std::uint64_t seed);
std::unique_ptr<OrganicBody> makeHopper(scene::OrganicInstance& inst, std::uint64_t seed);
std::unique_ptr<OrganicBody> makeCycles(scene::OrganicInstance& inst, std::uint64_t seed);
std::unique_ptr<OrganicBody> makeRocks(scene::OrganicInstance& inst, std::uint64_t seed);
std::unique_ptr<OrganicBody> makeBricks(scene::OrganicInstance& inst, std::uint64_t seed);
std::unique_ptr<OrganicBody> makeChase(scene::OrganicInstance& inst, std::uint64_t seed);

// ---- small shared helpers

inline double finiteOr(double v, double fb) { return std::isfinite(v) ? v : fb; }
// A load-time number key (upper-case name), clamped.
inline double keyNum(const scene::OrganicInstance& in, const char* key, double fb, double lo, double hi) {
    return std::clamp(finiteOr(in.loaded(key, fb), fb), lo, hi);
}
inline int keyCount(const scene::OrganicInstance& in, const char* key, double fb, int lo, int hi) {
    return int(std::lround(keyNum(in, key, fb, lo, hi)));
}
// A per-frame key (upper-case name), finite.
inline double frameNum(scene::OrganicInstance& in, const char* key, double fb) {
    return finiteOr(in.frameKey(key, fb), fb);
}
// SYNC_DROP (1 at a drop, falling to 1/e over a bar) and SYNC_BUILD (0..1 through a breakdown); 0 without audio.
inline double dropOf(const OrganicStep& s) { return s.audio ? std::clamp(finiteOr(s.audio->sync.drop, 0), 0.0, 1.0) : 0; }
inline double buildOf(const OrganicStep& s) {
    return s.audio ? std::clamp(finiteOr(s.audio->sync.build, 0), 0.0, 1.0) : 0;
}
// Onset strength (BEAT), 0 without audio.
inline double onsetOf(const OrganicStep& s) { return s.audio ? std::max(0.0, finiteOr(s.audio->beat, 0)) : 0; }

}  // namespace reforce::engine::arcade
