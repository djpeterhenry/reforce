#pragma once
// Scene engine placeholder. The parser, evaluator and per-type loaders
// (PLAN.md phase 3) will live in src/scene. See docs/scene-format.md.

#include <optional>
#include <string_view>

namespace reforce::scene {

enum class SceneType {
    WaveShape,
    Particle,
    FlowField,
    FlowFieldUnion,
    ColorMap,
    Sprite,
    Solid,    // reforce extension (PLAN 5L.4): 3D solids, reforce.Solid.package
    Organic,  // reforce extension (PLAN 5N): procedural growth (trees), reforce.Organic.package
    Dancer,   // reforce extension (PLAN 5P): 3D cartoon dancers, reforce.Dancer.package
};

std::string_view sceneTypeName(SceneType type);

// Maps a G-Force package directory name (e.g. "G-Force.FlowField.package") to
// its scene type. Particles live inside the WaveShape package, so this returns
// WaveShape for it; the Particles/ subfolder decides. "reforce.Solid.package" is Solid,
// "reforce.Organic.package" Organic.
std::optional<SceneType> sceneTypeFromPackage(std::string_view packageDirName);

}  // namespace reforce::scene
