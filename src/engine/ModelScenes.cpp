// reforce extension (PLAN 5R.3, docs/model-design.md): the model folder and its scene files. See ModelScenes.hpp.
#include "engine/ModelScenes.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>

#include "scene/SceneFile.hpp"
#include "scene/SceneType.hpp"

namespace reforce::engine {

namespace fs = std::filesystem;

fs::path modelFolder(const std::string& setting) {
    if (setting == "none") return {};  // off (settings-free runs: tests)
    const char* home = std::getenv("HOME");
    if (setting.empty())
        return home ? fs::path(home) / "Library" / "Application Support" / "reforce" / "Models" : fs::path();
    if (setting.rfind("~/", 0) == 0 && home) return fs::path(home) / setting.substr(2);
    return fs::path(setting);
}

fs::path resolveModelFile(const std::string& file, const fs::path& scenePath, const fs::path& modelDir) {
    if (file.empty()) return {};
    const fs::path f(file);
    std::error_code ec;
    if (f.is_absolute()) return f;
    if (!scenePath.empty()) {
        const fs::path beside = scenePath.parent_path() / f;
        if (fs::exists(beside, ec)) return beside;
    }
    return modelDir / f;
}

std::vector<SceneEntry> listModelScenes(const fs::path& dir, std::vector<std::string>* skipped) {
    std::vector<SceneEntry> out;
    for (SceneEntry& e : listSceneFolder(dir, scene::SceneType::Solid, "Model/")) {
        const auto file = scene::loadSceneFile(e.path);
        if (!file) continue;
        const scene::SceneEntry* mesh = file->find("Mesh");
        std::string m = mesh ? mesh->value : std::string();
        m.erase(std::remove_if(m.begin(), m.end(), [](char c) { return std::isspace(static_cast<unsigned char>(c)); }),
                m.end());
        std::transform(m.begin(), m.end(), m.begin(), [](char c) { return char(std::tolower(static_cast<unsigned char>(c))); });
        if (m != "model") {
            out.push_back(std::move(e));  // an ordinary solid kept in the model folder
            continue;
        }
        const scene::SceneEntry* f = file->find("File");
        const std::string name = f ? f->value : std::string();
        std::error_code ec;
        const fs::path model = resolveModelFile(name, e.path, dir);
        if (name.empty() || !fs::is_regular_file(model, ec)) {
            if (skipped) skipped->push_back(e.id + ": " + (name.empty() ? "no File" : "missing " + model.string()));
            continue;
        }
        out.push_back(std::move(e));
    }
    return out;
}

}  // namespace reforce::engine
