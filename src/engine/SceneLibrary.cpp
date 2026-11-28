#include "engine/SceneLibrary.hpp"

#include "engine/Sprites.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <system_error>
#include <vector>

namespace reforce::engine {
namespace fs = std::filesystem;
using scene::SceneType;

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool endsWith(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool acceptedFile(const fs::path& p, SceneType type) {
    if (type == SceneType::Sprite) return isSpriteFile(p);  // images and movies; a sibling .txt holds params
    const std::string ext = lower(p.extension().string());
    if (ext == ".txt") return p.filename().string()[0] != '_';  // _default_*, _package.py helpers
    if (type == SceneType::ColorMap && ext == ".png") return !endsWith(lower(p.stem().string()), "-icon");
    return false;
}

// `#waveshape-particle-also` anywhere in the file (scene-format §5.2).
bool taggedParticleAlso(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    std::stringstream ss;
    ss << in.rdbuf();
    return lower(ss.str()).find("#waveshape-particle-also") != std::string::npos;
}

}  // namespace

std::string packageDirName(SceneType type) {
    switch (type) {
        case SceneType::WaveShape:
        case SceneType::Particle: return "G-Force.WaveShape.package";
        case SceneType::FlowField: return "G-Force.FlowField.package";
        case SceneType::FlowFieldUnion: return "G-Force.FlowField Union.package";
        case SceneType::ColorMap: return "G-Force.ColorMap.package";
        case SceneType::Sprite: return "G-Force.Sprite.package";
        case SceneType::Solid: return "reforce.Solid.package";
        case SceneType::Organic: return "reforce.Organic.package";
        case SceneType::Dancer: return "reforce.Dancer.package";
    }
    return {};
}

std::optional<fs::path> packagesRoot(const fs::path& scenesDir) {
    std::error_code ec;
    if (fs::is_directory(scenesDir / "Packages", ec)) return scenesDir / "Packages";
    if (fs::is_directory(scenesDir / packageDirName(SceneType::FlowField), ec)) return scenesDir;
    return std::nullopt;
}

std::optional<fs::path> findScene(const fs::path& scenesDir, SceneType type, const std::string& name) {
    std::error_code ec;
    if (name.empty()) return std::nullopt;
    if (fs::is_regular_file(name, ec)) return fs::path(name);
    const auto root = packagesRoot(scenesDir);
    if (!root) return std::nullopt;
    const fs::path pkg = *root / packageDirName(type);
    if (!fs::is_directory(pkg, ec)) return std::nullopt;

    // Wanted relative path (without extension unless given), lower-case, '/'.
    std::string want = lower(name);
    std::replace(want.begin(), want.end(), '\\', '/');
    const bool hasExt = endsWith(want, ".txt") || endsWith(want, ".png");

    std::vector<fs::path> hits;
    for (auto it = fs::recursive_directory_iterator(pkg, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec) || !acceptedFile(it->path(), type)) continue;
        std::string rel = lower(fs::relative(it->path(), pkg, ec).generic_string());
        if (!hasExt) rel = rel.substr(0, rel.size() - it->path().extension().string().size());
        // Match the whole relative path or its tail at a '/' boundary.
        if (rel == want || (endsWith(rel, want) && rel[rel.size() - want.size() - 1] == '/')) hits.push_back(it->path());
    }
    if (hits.empty()) return std::nullopt;
    // Prefer .txt over .png, then the shortest (least nested) path, then lexical.
    std::sort(hits.begin(), hits.end(), [](const fs::path& a, const fs::path& b) {
        const bool at = lower(a.extension().string()) == ".txt", bt = lower(b.extension().string()) == ".txt";
        if (at != bt) return at;
        const auto as = a.string().size(), bs = b.string().size();
        if (as != bs) return as < bs;
        return a < b;
    });
    return hits.front();
}

std::vector<SceneEntry> listScenes(const fs::path& scenesDir, SceneType type) {
    std::vector<SceneEntry> out;
    std::error_code ec;
    const auto root = packagesRoot(scenesDir);
    if (!root) return out;
    const fs::path pkg = *root / packageDirName(type);
    if (!fs::is_directory(pkg, ec)) return out;
    for (auto it = fs::recursive_directory_iterator(pkg, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec) || !acceptedFile(it->path(), type)) continue;
        std::string rel = fs::relative(it->path(), pkg, ec).generic_string();
        rel = rel.substr(0, rel.size() - it->path().extension().string().size());
        const bool inParticles = lower(rel).rfind("particles/", 0) == 0;
        if (type == SceneType::WaveShape && inParticles) continue;
        // Particles: the Particles/ folder plus WaveShapes tagged #waveshape-particle-also.
        if (type == SceneType::Particle && !inParticles && !taggedParticleAlso(it->path())) continue;
        out.push_back({rel, it->path().stem().string(), it->path()});
    }
    std::sort(out.begin(), out.end(), [](const SceneEntry& a, const SceneEntry& b) {
        const std::string la = lower(a.id), lb = lower(b.id);
        if (la != lb) return la < lb;
        return lower(a.path.extension().string()) == ".txt" && lower(b.path.extension().string()) != ".txt";
    });
    out.erase(std::unique(out.begin(), out.end(),
                          [](const SceneEntry& a, const SceneEntry& b) { return lower(a.id) == lower(b.id); }),
              out.end());
    return out;
}

std::vector<SceneEntry> listMediaFolder(const fs::path& dir, const std::string& idPrefix, bool videosOnly) {
    std::vector<SceneEntry> out;
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(dir, ec)) return out;
    for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        const fs::path& p = it->path();
        // A folder holding a file named .reforce-ignore is left out with everything in it: work folders kept next
        // to the sprites (frames, source clips) that aren't sprites (Peter 2026-11-27).
        if (it->is_directory(ec)) {
            if (fs::exists(p / kMediaIgnoreFile, ec)) it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(ec) || !(videosOnly ? isSpriteVideoFile(p) : isSpriteFile(p))) continue;
        std::string rel = fs::relative(p, dir, ec).generic_string();
        rel = rel.substr(0, rel.size() - p.extension().string().size());
        out.push_back({idPrefix + rel, p.stem().string(), p});
    }
    std::sort(out.begin(), out.end(), [](const SceneEntry& a, const SceneEntry& b) { return lower(a.id) < lower(b.id); });
    return out;
}

std::vector<SceneEntry> listSceneFolder(const fs::path& dir, SceneType type, const std::string& idPrefix) {
    std::vector<SceneEntry> out;
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(dir, ec)) return out;
    for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        const fs::path& p = it->path();
        if (!it->is_regular_file(ec) || !acceptedFile(p, type)) continue;
        std::string rel = fs::relative(p, dir, ec).generic_string();
        rel = rel.substr(0, rel.size() - p.extension().string().size());
        out.push_back({idPrefix + rel, p.stem().string(), p});
    }
    std::sort(out.begin(), out.end(), [](const SceneEntry& a, const SceneEntry& b) { return lower(a.id) < lower(b.id); });
    return out;
}

const char* defaultReferenceScene(SceneType type) {
    switch (type) {
        case SceneType::WaveShape: return "WaveShapes/Simple Vertical";
        case SceneType::Particle: return "Particles/Raindrops";
        case SceneType::FlowField: return "Expand and Turn";
        case SceneType::FlowFieldUnion: return "Horizontal Strip";
        case SceneType::ColorMap: return "ColorWheel";
        case SceneType::Sprite: return "Space/Comet";
        case SceneType::Solid: return "";    // no solid at startup
        case SceneType::Organic: return "";  // no organic at startup
        case SceneType::Dancer: return "";   // no dancer at startup
    }
    return "";
}

namespace {

std::string excludeKey(std::string s) {
    for (char& c : s) {
        c = char(std::tolower(static_cast<unsigned char>(c)));
        if (c == '\\') c = '/';
    }
    const auto b = s.find_first_not_of(" \t\r");
    const auto e = s.find_last_not_of(" \t\r");
    s = b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
    const auto dot = s.find_last_of('.');
    const auto slash = s.find_last_of('/');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash) && s.size() - dot <= 5) {
        const std::string ext = s.substr(dot);
        static const char* const kExt[] = {".txt", ".png", ".jpg", ".jpeg", ".gif", ".bmp", ".tif", ".tiff",
                                           ".mov", ".mp4", ".m4v", ".mpg", ".mpeg", ".avi", ".qt", ".3gp"};
        for (const char* x : kExt)
            if (ext == x) return s.substr(0, dot);
    }
    return s;
}

}  // namespace

std::vector<ExcludeRule> parseExcludeList(const std::string& text, std::vector<std::string>* errors) {
    std::vector<ExcludeRule> out;
    std::string item;
    auto flush = [&] {
        std::string raw = item;
        item.clear();
        if (raw.find_first_not_of(" \t\r") == std::string::npos) return;
        ExcludeRule r;
        const auto colon = raw.find(':');
        if (colon != std::string::npos) {
            const std::string t = excludeKey(raw.substr(0, colon));
            if (t == "object" || t == "objects") {  // Objects: solids and organics
                const std::string name = excludeKey(raw.substr(colon + 1));
                if (!name.empty())
                    for (auto ty : {scene::SceneType::Solid, scene::SceneType::Organic}) {
                        ExcludeRule o;
                        o.type = ty;
                        o.name = name;
                        out.push_back(std::move(o));
                    }
                return;
            }
            static const std::pair<const char*, scene::SceneType> kTypes[] = {
                {"waveshape", scene::SceneType::WaveShape}, {"particle", scene::SceneType::Particle},
                {"flowfield", scene::SceneType::FlowField}, {"union", scene::SceneType::FlowFieldUnion},
                {"flowfield union", scene::SceneType::FlowFieldUnion}, {"colormap", scene::SceneType::ColorMap},
                {"sprite", scene::SceneType::Sprite}, {"solid", scene::SceneType::Solid},
                {"organic", scene::SceneType::Organic}, {"dancer", scene::SceneType::Dancer}};
            for (const auto& [n, ty] : kTypes)
                if (t == n) r.type = ty;
            if (!r.type) {
                if (errors) errors->push_back("reforce.Exclude: unknown type in \"" + raw + "\"");
                return;
            }
            raw = raw.substr(colon + 1);
        }
        r.name = excludeKey(raw);
        if (!r.name.empty()) out.push_back(std::move(r));
    };
    for (char c : text) {
        if (c == ';' || c == '\n') flush();
        else item.push_back(c);
    }
    flush();
    return out;
}

bool isExcluded(const std::vector<ExcludeRule>& rules, scene::SceneType type, const SceneEntry& e) {
    if (rules.empty()) return false;
    const std::string id = excludeKey(e.id), name = excludeKey(e.name);
    for (const auto& r : rules)
        if ((!r.type || *r.type == type) && (r.name == id || r.name == name)) return true;
    return false;
}

namespace {
std::string idKey(std::string s) {
    std::replace(s.begin(), s.end(), '\\', '/');
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    s = s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
    return lower(std::move(s));
}
}  // namespace

std::vector<std::string> parseIdList(const std::string& text) {
    std::vector<std::string> out;
    std::string item;
    auto flush = [&] {
        std::string id = std::move(item);
        item.clear();
        while (!id.empty() && (id.back() == '\r' || id.back() == '\n')) id.pop_back();
        if (!idKey(id).empty() && !idListHas(out, id)) out.push_back(std::move(id));
    };
    for (char c : text) {
        if (c == ';' || c == '\n') flush();
        else item.push_back(c);
    }
    flush();
    return out;
}

std::string joinIdList(const std::vector<std::string>& ids) {
    std::string s;
    for (const auto& id : ids) s += (s.empty() ? "" : ";") + id;
    return s;
}

bool idListHas(const std::vector<std::string>& ids, const std::string& id) {
    const std::string k = idKey(id);
    return !k.empty() && std::any_of(ids.begin(), ids.end(), [&](const std::string& x) { return idKey(x) == k; });
}

std::size_t removeExcluded(std::vector<SceneEntry>& entries, scene::SceneType type,
                           const std::vector<ExcludeRule>& rules) {
    const auto before = entries.size();
    entries.erase(std::remove_if(entries.begin(), entries.end(),
                                 [&](const SceneEntry& e) { return isExcluded(rules, type, e); }),
                  entries.end());
    return before - entries.size();
}

}  // namespace reforce::engine
