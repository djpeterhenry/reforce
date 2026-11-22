#pragma once
// Deterministic offscreen rendering (no window): fixed dt, file audio (or
// silence), seeded RNG, slideshow on the simulation clock; writes selected
// frames as PNG.

#include <set>
#include <string>
#include <utility>
#include <vector>

#include "engine/Commands.hpp"

namespace MTL { class Device; }

namespace reforce::app {

class Director;
class AudioDriver;

struct OfflineOptions {
    int frames = 30;
    double fps = 60.0;
    std::string outDir;       // "" = do not write PNGs
    std::set<int> saveFrames; // 0-based frame indices; empty = last frame only
    int saveEvery = 0;        // also every Nth frame (0 = off)
    bool seedNoise = false;
    std::string prefix = "frame";
    // Commands pushed before the first step at t >= time (--command T=CMD).
    std::vector<std::pair<double, engine::Command>> commands;
};

// Renders into a BGRA8 target of the visualizer's size: exactly one
// simulation step per output frame (t = frame / fps), then colorize.
// Returns an exit code.
int runOffline(MTL::Device* device, unsigned width, unsigned height, const OfflineOptions& opts, Director& director,
               AudioDriver* audio);

}  // namespace reforce::app
