#pragma once
// reforce extension (PLAN 5R.3, docs/model-design.md "Folders"): the model folder and its scene files.
//
// reforce.Model.Dir (default ~/Library/Application Support/reforce/Models) holds the user's own models (.glb) and
// the solid scene files that show them (Mesh="model", File="name.glb"). Model scenes are listed as Objects with ids
// "Model/..."; a scene whose File does not exist is left out (one log line), so nothing changes without models.

#include <filesystem>
#include <string>
#include <vector>

#include "engine/SceneLibrary.hpp"

namespace reforce::engine {

// The model folder: `setting` (reforce.Model.Dir, "~/" expanded), the default under Application Support when empty,
// or none (an empty path: no model scenes) for "none", which --no-settings runs use.
std::filesystem::path modelFolder(const std::string& setting);

// A scene's File resolved: absolute as written, else next to the scene file, else in the model folder.
std::filesystem::path resolveModelFile(const std::string& file, const std::filesystem::path& scenePath,
                                       const std::filesystem::path& modelDir);

// Solid scene files in `dir` (recursive) whose Mesh is "model" and whose File exists, ids "Model/<relative>".
// `skipped` (optional) gets "<scene>: <reason>" for model scenes left out.
std::vector<SceneEntry> listModelScenes(const std::filesystem::path& dir, std::vector<std::string>* skipped = nullptr);

}  // namespace reforce::engine
