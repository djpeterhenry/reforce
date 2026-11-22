// dancerSpec from a compiled scene file (engine/Dancers.hpp). Kept apart from Dancers.cpp so the web preview
// (web/dancer-preview) can build the dancers without the scene compiler.
#include "engine/Dancers.hpp"

#include "scene/SceneConfig.hpp"

namespace reforce::engine {

DancerSpec dancerSpec(const scene::CompiledConfig& cfg, const std::string& name) {
    return dancerSpecFromKeys(
        [&](const char* key) -> std::string {
            const scene::CompiledKey* k = cfg.find(key);
            return k ? k->text : std::string();
        },
        name);
}

}  // namespace reforce::engine
