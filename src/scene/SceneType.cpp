#include "scene/SceneType.hpp"

namespace reforce::scene {

std::string_view sceneTypeName(SceneType type) {
    switch (type) {
        case SceneType::WaveShape: return "WaveShape";
        case SceneType::Particle: return "Particle";
        case SceneType::FlowField: return "FlowField";
        case SceneType::FlowFieldUnion: return "FlowField Union";
        case SceneType::ColorMap: return "ColorMap";
        case SceneType::Sprite: return "Sprite";
        case SceneType::Solid: return "Solid";
        case SceneType::Organic: return "Organic";
        case SceneType::Dancer: return "Dancer";
    }
    return "?";
}

std::optional<SceneType> sceneTypeFromPackage(std::string_view dir) {
    if (dir == "G-Force.WaveShape.package") return SceneType::WaveShape;
    if (dir == "G-Force.FlowField.package") return SceneType::FlowField;
    if (dir == "G-Force.FlowField Union.package") return SceneType::FlowFieldUnion;
    if (dir == "G-Force.ColorMap.package") return SceneType::ColorMap;
    if (dir == "G-Force.Sprite.package") return SceneType::Sprite;
    if (dir == "reforce.Solid.package") return SceneType::Solid;
    if (dir == "reforce.Organic.package") return SceneType::Organic;
    if (dir == "reforce.Dancer.package") return SceneType::Dancer;
    return std::nullopt;
}

}  // namespace reforce::scene
