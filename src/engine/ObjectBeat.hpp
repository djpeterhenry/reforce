#pragma once
// reforce extension: which Objects follow the beat (the control panel's green note and its beat / no-beat filter).
//
// An Object follows the beat when its motion or growth is timed by the beat clock while it runs (120 BPM from its
// start without one). Its hold in bars (PBar) does not count: that only says when it ends. By kind:
//   - any Object whose expressions read BEAT, TEMPO, LINK_* or SYNC_* (scene::usesBeat)
//   - solids: model scenes with MBar, Move="cuts" or Step keyframes (docs/model-design.md)
//   - organics, by Kind:
//       mandel, phyllotaxis, automaton, life, dragon, double, invaders, hopper, cycles, rocks, bricks,
//         chase: always
//         (drops, dives, seeds, generations, game moves on the beat)
//       pendulums, julia: unless Phas sets the phase (else the cycle / the bands follow the bars / beats)
//       tree: when it uses s (beats since its growth began), SwyB, Vib, Puls, Leaf, Rwir or more than one strike
//       harmonograph, lsystem, attractor: expressions only

#include "scene/SceneConfig.hpp"
#include "scene/SceneType.hpp"

namespace reforce::engine {

bool objectFollowsBeat(scene::SceneType type, const scene::CompiledConfig& cfg);

}  // namespace reforce::engine
