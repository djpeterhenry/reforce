// reforce extension: which Objects follow the beat. See ObjectBeat.hpp.
#include "engine/ObjectBeat.hpp"

#include <algorithm>
#include <cctype>
#include <string>

#include "engine/Organics.hpp"
#include "engine/Solids.hpp"

namespace reforce::engine {

namespace {

std::string literal(const scene::CompiledConfig& cfg, const char* key) {
    const scene::CompiledKey* k = cfg.find(key);
    std::string s = k ? k->text : std::string();
    s.erase(std::remove_if(s.begin(), s.end(), [](char c) { return std::isspace(static_cast<unsigned char>(c)); }),
            s.end());
    std::transform(s.begin(), s.end(), s.begin(), [](char c) { return char(std::tolower(static_cast<unsigned char>(c))); });
    return s;
}

bool usesS(const scene::CompiledConfig& cfg) {
    return std::any_of(cfg.keys.begin(), cfg.keys.end(),
                       [](const scene::CompiledKey& k) { return k.prog.defined && k.prog.usesVar(expr::Var::S); });
}

}  // namespace

bool objectFollowsBeat(scene::SceneType type, const scene::CompiledConfig& cfg) {
    if (scene::usesBeat(cfg)) return true;
    if (type == scene::SceneType::Solid) {
        const auto mesh = solidMesh(cfg);
        // Fractal zooms step on the bar lines (unless a scene's own Zoom expression drives them, which usesBeat sees).
        if (mesh == MeshKind::Fractal) return !cfg.defines("ZOOM");
        if (mesh == MeshKind::Rain) return true;     // drops land on the beat grid
        if (mesh == MeshKind::Bubbles) return true;  // blown in on bar lines, popped on beat lines
        if (mesh == MeshKind::Shatter) return true;  // cracks land on the beats, the pane shatters on the line
        if (mesh != MeshKind::Model) return false;
        // Moves timed in bars, beat cuts (the cuts preset, or Step keyframes in the scene).
        return cfg.defines("MBAR") || literal(cfg, "MOVE") == "cuts" || (literal(cfg, "STEP") != "" && literal(cfg, "STEP") != "0");
    }
    if (type != scene::SceneType::Organic) return false;
    const std::string kind = literal(cfg, "KIND");
    static const char* kAlways[] = {"mandel", "mandelbrot", "phyllotaxis", "automaton", "life", "dragon",
                                    "double", "invaders", "hopper", "cycles", "rocks", "bricks", "chase"};
    for (const char* k : kAlways)
        if (kind == k) return true;
    if (kind == "pendulums" || kind == "pendulum" || kind == "julia") return !cfg.defines("PHAS");
    if (kind.empty() || kind == "tree") {
        if (usesS(cfg)) return true;
        for (const char* k : {"SWYB", "VIB", "PULS", "LEAF", "RWIR"})
            if (cfg.defines(k)) return true;
        if (const scene::CompiledKey* k = cfg.find("STRK"))
            if (k->prog.defined && k->text.find_first_not_of(" \t\"1") != std::string::npos) return true;  // Strk > 1
        return false;
    }
    return false;  // harmonograph, lsystem, attractor: expressions only
}

}  // namespace reforce::engine
