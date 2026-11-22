#pragma once
// Locating scene files in a G-Force style Packages tree.
//
//   <scenes>/Packages/G-Force.FlowField.package/...      (or <scenes> = Packages dir itself)
//
// A scene is named by its file stem ("Expand and Turn"), by a relative path
// inside its package ("Gold/Lust", "Gold/Lust.txt"), or by a filesystem path.
// Names are matched case-insensitively; the search is recursive.

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "scene/SceneType.hpp"

namespace reforce::engine {

// Package directory name for a type ("G-Force.FlowField.package").
std::string packageDirName(scene::SceneType type);

// <scenesDir>/Packages or <scenesDir> when it already holds the packages.
std::optional<std::filesystem::path> packagesRoot(const std::filesystem::path& scenesDir);

// Resolves `name` for `type`. ColorMaps match .txt and .png (not -icon.png);
// other types .txt. A name that is an existing file path is returned as is.
std::optional<std::filesystem::path> findScene(const std::filesystem::path& scenesDir, scene::SceneType type,
                                               const std::string& name);

// One config file in a package.
struct SceneEntry {
    std::string id;    // relative path inside the package, '/', no extension ("WaveShapes/Simple Vertical")
    std::string name;  // file stem ("Simple Vertical"): display, categories, parenthesized check
    std::filesystem::path path;
};

// Every config of `type`, sorted by id. WaveShape skips the Particles/ folder;
// Particle lists it plus WaveShapes tagged #waveshape-particle-also; Sprite
// lists images and movies (not -icon thumbnails); ColorMaps prefer .txt over a .png of
// the same id.
std::vector<SceneEntry> listScenes(const std::filesystem::path& scenesDir, scene::SceneType type);

// Images and movies (videosOnly: movies) anywhere under `dir`, ids prefixed
// with `idPrefix` ("External/Clip"): reforce.ExternalSpritesDir joins the
// Sprite catalog this way, reforce.VideoDir feeds the video source picker.
std::vector<SceneEntry> listMediaFolder(const std::filesystem::path& dir, const std::string& idPrefix,
                                        bool videosOnly);

// Scene files of `type` anywhere under a plain folder (no Packages tree), ids prefixed with `idPrefix`:
// reforce.Solid.Dir adds solids this way.
std::vector<SceneEntry> listSceneFolder(const std::filesystem::path& dir, scene::SceneType type,
                                        const std::string& idPrefix);

// Skip list (reforce.Exclude): scenes reforce never uses: not in the slideshow,
// the control panel's lists, or manual switches. The scene files stay in place
// (a scene sync would bring deleted files back). Entries are separated by ';'
// or newlines, each "Type:name" or just "name" (any type), where name is the
// id ("Space/Moon"), the file stem ("Moon") or either with an extension; type
// is WaveShape, Particle, FlowField, Union ("FlowField Union"), ColorMap,
// Sprite or Solid. Case-insensitive.
struct ExcludeRule {
    std::optional<scene::SceneType> type;  // nullopt = any type
    std::string name;                      // lower case, '/' separators, no extension
};
// Unknown type prefixes go to *errors and the entry is skipped.
std::vector<ExcludeRule> parseExcludeList(const std::string& text, std::vector<std::string>* errors = nullptr);
bool isExcluded(const std::vector<ExcludeRule>& rules, scene::SceneType type, const SceneEntry& e);
// Removes excluded entries; returns how many.
std::size_t removeExcluded(std::vector<SceneEntry>& entries, scene::SceneType type,
                           const std::vector<ExcludeRule>& rules);

// Id lists (reforce.Sprite.Hidden): exact scene ids ("Abstract/Rose", "External/images/3-a4"),
// separated by ';' or newlines. Unlike reforce.Exclude a bare stem matches nothing: two folders
// may both hold a "Rose". Matching ignores case and spaces at either end of an id.
std::vector<std::string> parseIdList(const std::string& text);  // trimmed of line breaks; duplicates dropped
std::string joinIdList(const std::vector<std::string>& ids);   // "a;b" (ids keep their own spaces)
bool idListHas(const std::vector<std::string>& ids, const std::string& id);

// First reference scene per type (docs/reference-scenes.md, #1 of each table).
const char* defaultReferenceScene(scene::SceneType type);

}  // namespace reforce::engine
