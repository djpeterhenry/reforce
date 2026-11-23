#include "web/Api.hpp"

#include "engine/DanceClips.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>

#include "engine/Blend.hpp"
#include "engine/MediaOptions.hpp"
#include "engine/SceneLibrary.hpp"
#include "engine/Scheduler.hpp"
#include "engine/Sprites.hpp"
#include "engine/Themes.hpp"
#include "engine/Body.hpp"
#include "engine/LightsSettings.hpp"
#include "engine/VideoComposite.hpp"
#include "scene/SceneConfig.hpp"
#include "scene/SceneFile.hpp"
#include "scene/SceneType.hpp"
#include "web/Json.hpp"

namespace reforce::web {
using engine::Command;
using engine::CommandType;
using engine::Layer;
namespace fs = std::filesystem;

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool ieq(const std::string& a, const std::string& b) { return lower(a) == lower(b); }

struct TypeInfo {
    Layer layer;
    scene::SceneType type;
};
constexpr TypeInfo kTypes[] = {
    {Layer::FlowField, scene::SceneType::FlowField},   {Layer::WaveShape, scene::SceneType::WaveShape},
    {Layer::ColorMap, scene::SceneType::ColorMap},     {Layer::Union, scene::SceneType::FlowFieldUnion},
    {Layer::Particle, scene::SceneType::Particle},     {Layer::Sprite, scene::SceneType::Sprite},
    {Layer::Object, scene::SceneType::Organic},  // solids and organics (see buildLibrary)
    {Layer::Dancer, scene::SceneType::Dancer},
};

// Category titles defined for a type (<scenes>/Categories/<Type>/*.txt).
std::vector<std::string> categoryTitles(const fs::path& scenesDir, const std::string& typeName) {
    std::vector<std::string> titles;
    std::error_code ec;
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(scenesDir / "Categories" / typeName, ec))
        if (e.is_regular_file(ec) && lower(e.path().extension().string()) == ".txt") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    for (const auto& f : files) {
        std::ifstream in(f, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        const auto kv = engine::parseKeyValueFile(ss.str());
        auto it = kv.find("categorytitle");
        if (it != kv.end() && !it->second.empty() &&
            std::find(titles.begin(), titles.end(), it->second) == titles.end())
            titles.push_back(it->second);
    }
    return titles;
}

void writeLayerState(json::Writer& w, const engine::LayerState& l, bool stable) {
    w.beginObject();
    w.field("key", layerKey(l.layer));
    w.field("name", engine::layerName(l.layer));
    w.field("current", l.current);
    w.field("target", l.target);
    w.field("pending", l.pending);
    w.field("transitioning", l.transitioning);
    if (!stable) {  // continuous during a transition: the live event carries them (liveToJson)
        w.field("progress", l.progress);
        w.field("weight", l.weight);
    }
    w.field("crossfaderHeld", l.crossfaderHeld);
    w.field("morphDepth", l.morphDepth);
    w.field("slideshowEnabled", l.slideshowEnabled);
    w.field("held", l.held);
    w.field("interval", l.interval);
    w.field("transitionDuration", l.transitionDuration);
    if (!stable) w.field("nextSwitchIn", l.nextSwitchIn);
    w.field("blendMode", l.blendMode);
    w.key("blendModes").stringArray(l.blendModes);
    w.field("playlistSize", l.playlistSize);
    w.field("unionName", l.unionName);
    w.field("unionDue", l.unionDue);
    w.key("alive").stringArray(l.alive);  // Particle / Sprite / Solid: live instances
    if (engine::isSpawnLayer(l.layer)) w.field("spawnDuration", l.spawnDuration);
    if (l.layer == Layer::Sprite) {
        w.field("movieInterval", l.movieInterval).field("moviePlaylistSize", static_cast<unsigned long long>(l.moviePlaylistSize));
        w.field("spriteMix", l.spriteMix).field("movieSpriteMix", l.movieSpriteMix);
        if (!stable) w.field("nextMovieIn", l.nextMovieIn);
    }
    w.endObject();
}

std::string stringField(const json::Value& o, std::initializer_list<const char*> keys, bool* present = nullptr,
                        bool* wrongType = nullptr) {
    for (const char* k : keys) {
        if (const json::Value* v = o.get(k)) {
            if (present) *present = true;
            if (v->isString()) return v->str;
            if (wrongType) *wrongType = true;
            return {};
        }
    }
    if (present) *present = false;
    return {};
}

// One command object -> Command (unvalidated). Returns "" or an error.
std::string commandFromObject(const json::Value& o, Command& out) {
    if (!o.isObject()) return "command must be a JSON object";
    bool present = false, wrongType = false;
    const std::string text = stringField(o, {"command"}, &present, &wrongType);
    if (wrongType) return "\"command\" must be a string";
    if (present) {
        if (text.size() > 1024) return "command text too long";
        std::string err;
        auto c = engine::parseCommand(text, &err);
        if (!c) return "command \"" + text + "\": " + err;
        out = *c;
        return {};
    }

    std::string action = lower(stringField(o, {"action"}, &present, &wrongType));
    if (!present || wrongType || action.empty()) return "missing \"action\" (string)";

    if (action == "theme") {
        const std::string name = stringField(o, {"theme", "name", "value"}, &present, &wrongType);
        if (!present || wrongType || name.empty()) return "theme needs \"theme\" (string)";
        out = Command::theme(name);
        return {};
    }

    if (action == "videohold") {
        out = Command::videoHold();
        return {};
    }
    if (action == "video" || action == "videostop") {
        std::string target = lower(stringField(o, {"target", "layer"}, &present, &wrongType));
        if (wrongType) return "\"target\" must be a string";
        if (!present || target.empty()) target = "layer";
        engine::VideoTarget t;
        if (target == "layer" || target == "colour" || target == "color") t = engine::VideoTarget::Layer;
        else if (target == "sprite") t = engine::VideoTarget::Sprite;
        else return "\"target\" must be \"layer\" or \"sprite\"";
        if (action == "videostop") {
            out = Command::videoStop(t);
            return {};
        }
        const std::string src = stringField(o, {"source"}, &present, &wrongType);
        if (!present || wrongType || src.empty()) return "video needs \"source\" (string)";
        out = Command::videoStart(t, src);
        return {};
    }
    if (action == "look") {
        // {"action":"look","key":K,"value":V} | {"action":"look","op":"ab"|"preset"|"reset"|"save", ...}
        std::string op = lower(stringField(o, {"op"}, &present, &wrongType));
        if (wrongType) return "\"op\" must be a string";
        const std::string key = stringField(o, {"key"}, &present, &wrongType);
        if (wrongType) return "\"key\" must be a string";
        std::string value;
        if (const json::Value* v = o.get("value")) {
            if (v->isBool()) value = v->b ? "on" : "off";
            else if (v->type == json::Value::Type::String) value = v->str;
            else if (v->isNumber()) value = v->num != 0 ? "1" : "0";
            else return "\"value\" must be a string or boolean";
        }
        if (op.empty()) {
            if (key.empty()) return "look needs \"key\" and \"value\", or \"op\"";
            auto k = engine::parseLookKey(key);
            if (!k) return "unknown look key \"" + key + "\"";
            out = Command::look(engine::lookInfo(*k).id, value);
        } else if (op == "preset") {
            out = Command::look(op, value.empty() ? key : value);
        } else if (op == "reset" || op == "save") {
            std::string k;
            if (!key.empty() && key != "all") {
                auto lk = engine::parseLookKey(key);
                if (!lk) return "unknown look key \"" + key + "\"";
                k = engine::lookInfo(*lk).id;
            }
            out = Command::look(op, k);
        } else if (op == "ab") {
            out = Command::look(op);
        } else {
            return "look \"op\" must be ab, preset, reset or save";
        }
        return {};
    }
    if (action == "spritemix") {
        // {"action":"spritemix","target":"sprite"|"movie","value":W} (PLAN 5K)
        const std::string target = lower(stringField(o, {"target", "kind"}, &present, &wrongType));
        if (wrongType) return "\"target\" must be a string";
        const bool movie = target == "movie" || target == "movies" || target == "video";
        if (!movie && target != "sprite" && target != "image" && target != "images")
            return "spritemix needs \"target\": \"sprite\" or \"movie\"";
        const json::Value* v = o.get("value");
        if (!v || !v->isNumber() || !std::isfinite(v->num) || v->num < 0 || v->num > 1)
            return "spritemix needs \"value\" (number 0..1)";
        out = Command::spriteMix(movie, v->num);
        return {};
    }
    if (action == "videoset") {
        const std::string key = stringField(o, {"key"}, &present, &wrongType);
        if (!present || wrongType || key.empty()) return "videoset needs \"key\" (string)";
        const json::Value* v = o.get("value");
        if (!v) return "videoset needs \"value\"";
        std::string value;
        if (v->isBool()) value = v->b ? "on" : "off";
        else if (v->isNumber()) {
            char buf[64];
            std::snprintf(buf, sizeof buf, "%.17g", v->num);
            value = buf;
        } else if (v->type == json::Value::Type::String) value = v->str;
        else return "\"value\" must be a string, number or boolean";
        out = Command::videoSet(key, value);
        return {};
    }
    if (action == "beatset") {  // rhythm (PLAN 5O): {"action":"beatset","key":K,"value":V}
        const std::string key = stringField(o, {"key"}, &present, &wrongType);
        if (!present || wrongType || key.empty()) return "beatset needs \"key\" (string)";
        std::string value;
        if (const json::Value* v = o.get("value")) {
            if (v->isBool()) value = v->b ? "on" : "off";
            else if (v->isNumber()) {
                char buf[64];
                std::snprintf(buf, sizeof buf, "%.17g", v->num);
                value = buf;
            } else if (v->type == json::Value::Type::String) value = v->str;
            else return "\"value\" must be a string, number or boolean";
        }
        out = Command::beatSet(key, value);
        if (auto e = engine::validateBeatSetting(out.text, out.arg); !e.empty()) return e;
        return {};
    }

    const std::string layerText = stringField(o, {"layer"}, &present, &wrongType);
    if (!present || wrongType || layerText.empty()) return "missing \"layer\" (string)";
    const bool all = ieq(layerText, "all");
    const auto layer = engine::parseLayer(layerText);
    if (!layer && !all) return "unknown layer \"" + layerText + "\"";
    const Layer l = layer.value_or(Layer::WaveShape);
    const bool allowsAll =
        action == "hold" || action == "unhold" || action == "togglehold" || action == "slideshow";
    if (all && !allowsAll) return "action \"" + action + "\" needs a single layer";

    auto number = [&](std::initializer_list<const char*> keys, double& v) -> std::string {
        for (const char* k : keys)
            if (const json::Value* x = o.get(k)) {
                if (!x->isNumber()) return std::string("\"") + k + "\" must be a number";
                v = x->num;
                return {};
            }
        return std::string("missing \"") + *keys.begin() + "\" (number)";
    };
    auto flag = [&](const char* k, bool def, bool& v) -> std::string {
        const json::Value* x = o.get(k);
        if (!x) {
            v = def;
            return {};
        }
        if (!x->isBool()) return std::string("\"") + k + "\" must be true or false";
        v = x->b;
        return {};
    };

    if (action == "switch") {
        const std::string cfg = stringField(o, {"config", "name"}, &present, &wrongType);
        if (!present || wrongType || cfg.empty()) return "switch needs \"config\" (string)";
        bool quick = true;
        if (auto e = flag("quick", true, quick); !e.empty()) return e;
        out = Command::switchTo(l, cfg, quick);
        return {};
    }
    if (action == "next" || action == "prev" || action == "previous") {
        bool quick = true;
        if (auto e = flag("quick", true, quick); !e.empty()) return e;
        out = action == "next" ? Command::next(l, quick) : Command::previous(l, quick);
        return {};
    }
    if (action == "hold" || action == "unhold") {
        bool on = action == "hold";
        if (action == "hold")
            if (auto e = flag("on", true, on); !e.empty()) return e;
        out = all ? Command::holdAll(on) : Command::hold(l, on);
        return {};
    }
    if (action == "togglehold") {
        out = all ? Command::toggleHoldAll() : Command::toggleHold(l);
        return {};
    }
    if (action == "slideshow") {
        bool on = true;
        if (!o.get("on")) return "slideshow needs \"on\" (true/false)";
        if (auto e = flag("on", true, on); !e.empty()) return e;
        out = all ? Command::slideshowAll(on) : Command::slideshow(l, on);
        return {};
    }
    if (action == "interval" || action == "duration" || action == "life" || action == "movieinterval") {
        double s = 0;
        if (auto e = number({"seconds", "value"}, s); !e.empty()) return e;
        if (action == "life" && !engine::isSpawnLayer(l))
            return "life applies to particle, sprite, object and dancer";
        if (action == "movieinterval" && l != Layer::Sprite) return "movieinterval applies to sprite";
        out = action == "interval"   ? Command::interval(l, s)
              : action == "duration" ? Command::transitionDuration(l, s)
              : action == "life"     ? Command::spawnDuration(l, s)
                                     : Command::movieInterval(s);
        return {};
    }
    if (action == "blend") {
        const std::string mode = stringField(o, {"mode", "value"}, &present, &wrongType);
        if (!present || wrongType || mode.empty()) return "blend needs \"mode\" (string)";
        out = Command::blendMode(l, mode);
        return {};
    }
    if (action == "fader" || action == "crossfader") {
        double w = 0;
        if (auto e = number({"weight", "value"}, w); !e.empty()) return e;
        out = Command::crossfader(l, w);
        return {};
    }
    if (action == "release") {
        out = Command::releaseCrossfader(l);
        return {};
    }
    return "unknown action \"" + action + "\"";
}

bool configKnown(const LibraryLayer& ll, const std::string& name) {
    std::string want = lower(name);
    std::replace(want.begin(), want.end(), '\\', '/');
    for (const char* ext : {".txt", ".png"})
        if (want.size() > 4 && want.compare(want.size() - 4, 4, ext) == 0) want.resize(want.size() - 4);
    for (const auto& e : ll.configs) {
        const std::string id = lower(e.id);
        if (id == want || lower(e.name) == want) return true;
        if (id.size() > want.size() && id.compare(id.size() - want.size(), want.size(), want) == 0 &&
            id[id.size() - want.size() - 1] == '/')
            return true;
    }
    return false;
}

}  // namespace

const char* layerKey(Layer l) {
    switch (l) {
        case Layer::WaveShape: return "waveshape";
        case Layer::ColorMap: return "colormap";
        case Layer::FlowField: return "flowfield";
        case Layer::Union: return "union";
        case Layer::Particle: return "particle";
        case Layer::Sprite: return "sprite";
        case Layer::Object: return "object";
        case Layer::Dancer: return "dancer";
    }
    return "?";
}

const LibraryLayer* Library::find(Layer l) const {
    for (const auto& ll : layers)
        if (ll.layer == l) return &ll;
    return nullptr;
}

Library buildLibrary(const fs::path& scenesDir, const std::string& exclude, const fs::path& externalSpritesDir,
                     const fs::path& contentDir, const fs::path& solidDir) {
    const auto excludeRules = engine::parseExcludeList(exclude);
    Library lib;
    if (scenesDir.empty() && contentDir.empty()) return lib;
    const engine::ThemeLibrary themes = engine::ThemeLibrary::loadAll(scenesDir, contentDir);
    for (const auto& t : themes.themes()) lib.themes.push_back(t.name);
    for (const TypeInfo& ti : kTypes) {
        LibraryLayer ll;
        ll.layer = ti.layer;
        const bool objects = ti.layer == Layer::Object;
        const std::string typeName = objects ? std::string("Object") : std::string(scene::sceneTypeName(ti.type));
        std::vector<engine::SceneEntry> entries;
        std::set<std::string> solidIds;  // Objects: which entries are solids
        if (objects) {
            // Like the Director: solids (content/, scenes/, reforce.Solid.Dir), then organics (content/, scenes/);
            // the first of an id wins; each list skips its own type's reforce.Exclude rules.
            auto add = [&](std::vector<engine::SceneEntry> more, scene::SceneType type) {
                engine::removeExcluded(more, type, excludeRules);
                for (auto& e : more)
                    if (std::none_of(entries.begin(), entries.end(),
                                     [&](const engine::SceneEntry& c) { return lower(c.id) == lower(e.id); })) {
                        if (type == scene::SceneType::Solid) solidIds.insert(lower(e.id));
                        entries.push_back(std::move(e));
                    }
            };
            const auto S = scene::SceneType::Solid, O = scene::SceneType::Organic;
            if (!contentDir.empty()) add(engine::listScenes(contentDir, S), S);
            if (!scenesDir.empty()) add(engine::listScenes(scenesDir, S), S);
            if (!solidDir.empty()) add(engine::listSceneFolder(solidDir, S, "Extra/"), S);
            if (!contentDir.empty()) add(engine::listScenes(contentDir, O), O);
            if (!scenesDir.empty()) add(engine::listScenes(scenesDir, O), O);
        } else if (ti.type == scene::SceneType::Dancer) {
            // Like the Director: content/, then scenes/; the first of an id wins.
            if (!contentDir.empty()) entries = engine::listScenes(contentDir, ti.type);
            if (!scenesDir.empty())
                for (auto& e : engine::listScenes(scenesDir, ti.type))
                    if (std::none_of(entries.begin(), entries.end(),
                                     [&](const engine::SceneEntry& c) { return lower(c.id) == lower(e.id); }))
                        entries.push_back(std::move(e));
        } else if (!scenesDir.empty()) {
            entries = engine::listScenes(scenesDir, ti.type);
        }
        if (ti.type == scene::SceneType::Sprite && !externalSpritesDir.empty())
            for (auto& e : engine::listMediaFolder(externalSpritesDir, "External/", false)) entries.push_back(std::move(e));
        if (!objects) engine::removeExcluded(entries, ti.type, excludeRules);
        for (const fs::path& root : {scenesDir, contentDir}) {
            if (root.empty()) continue;
            for (auto& c : categoryTitles(root, typeName))
                if (std::find(ll.categories.begin(), ll.categories.end(), c) == ll.categories.end())
                    ll.categories.push_back(std::move(c));
        }
        std::vector<const engine::Category*> cats;
        for (const auto& title : ll.categories) cats.push_back(themes.category(typeName, title));
        if (objects)
            for (const auto& title : ll.categories) {
                LibraryLayer::Group g{title, {}};
                for (const auto& t : themes.themes()) {
                    const auto it = t.categoryExpr.find("object");
                    if (it == t.categoryExpr.end()) continue;
                    std::string e = it->second;
                    e.erase(0, e.find_first_not_of(" \t"));
                    e.erase(e.find_last_not_of(" \t") + 1);
                    if (lower(e) == "+" + lower(title)) {
                        g.theme = t.name;
                        break;
                    }
                }
                ll.groups.push_back(std::move(g));
            }
        for (const auto& e : entries) {
            LibraryEntry le;
            le.id = e.id;
            le.name = e.name;
            le.parenthesized = engine::excludedFromSlideshow(e.name);
            le.path = e.path;
            le.movie = ti.type == scene::SceneType::Sprite && engine::isSpriteVideoFile(e.path);
            if (le.movie) {
                const engine::MediaOptions mo = engine::loadMediaOptions(e.path);
                if (mo.bpm) le.bpm = *mo.bpm;
                le.noColorKey = mo.colorKey == std::optional<bool>(false);
            }
            if (objects && solidIds.count(lower(e.id)))
                if (const auto file = scene::loadSceneFile(e.path))
                    le.beat = scene::usesBeat(scene::compileConfig(*file, scene::SceneType::Solid));
            for (std::size_t i = 0; i < cats.size(); ++i)
                if (cats[i] && cats[i]->matches(e.name)) le.categories.push_back(ll.categories[i]);
            ll.configs.push_back(std::move(le));
        }
        lib.layers.push_back(std::move(ll));
    }
    return lib;
}

std::vector<Favorite> parseFavorites(const std::string& text) {
    static const char* const kLayers[] = {"flowfield", "waveshape", "colormap", "union", "particle", "sprite", "object", "dancer"};
    std::vector<Favorite> out;
    std::string item;
    auto flush = [&] {
        const auto colon = item.find(':');
        if (colon != std::string::npos) {
            std::string layer = lower(item.substr(0, colon));
            layer.erase(0, layer.find_first_not_of(" \t\r\n"));
            layer.erase(layer.find_last_not_of(" \t\r\n") + 1);
            std::string id = item.substr(colon + 1);
            // Ids may start with spaces ("  dj peter henry"): only line breaks are trimmed.
            while (!id.empty() && (id.back() == '\r' || id.back() == '\n')) id.pop_back();
            const bool known = std::any_of(std::begin(kLayers), std::end(kLayers), [&](const char* l) { return layer == l; });
            Favorite f{layer, id};
            if (known && !id.empty() && std::find(out.begin(), out.end(), f) == out.end()) out.push_back(std::move(f));
        }
        item.clear();
    };
    for (char c : text) {
        if (c == ';' || c == '\n') flush();
        else item.push_back(c);
    }
    flush();
    return out;
}

std::string joinFavorites(const std::vector<Favorite>& favs) {
    std::string s;
    for (const auto& f : favs) s += (s.empty() ? "" : ";") + f.layer + ":" + f.id;
    return s;
}

std::string favoritesToJson(const std::vector<Favorite>& favs, bool canSave) {
    json::Writer w;
    w.beginObject().field("canSave", canSave).key("favorites").beginArray();
    for (const auto& f : favs) w.beginObject().field("layer", f.layer).field("id", f.id).endObject();
    w.endArray().endObject();
    return w.take();
}

std::string libraryToJson(const Library& lib) {
    json::Writer w;
    w.beginObject();
    w.key("themes").stringArray(lib.themes);
    // The dance styles (key, name for people) and the mocap moves (engine/DanceClips) for the Dancers card's move
    // browser, each with its style, in file order.
    w.key("danceStyles").beginArray();
    for (int i = 0; i < engine::dance::kStyleCount; ++i) {
        const auto st = engine::dance::Style(i);
        w.beginObject().field("key", std::string(engine::dance::styleName(st)))
            .field("name", std::string(engine::dance::styleLabel(st))).endObject();
    }
    w.endArray();
    w.key("danceMoves").beginArray();
    for (int i = 0; i < engine::dance::clipCount(); ++i) {
        const engine::dance::ClipInfo& c = engine::dance::clipInfo(i);
        const auto st = engine::dance::clipGenreStyle(c.genre);
        w.beginObject().field("id", c.id).field("label", c.label)
            .field("style", std::string(st ? engine::dance::styleName(*st) : "")).field("routine", c.routine)
            .field("bpm", c.bpm).endObject();
    }
    w.endArray();
    w.key("layers").beginArray();
    for (const auto& ll : lib.layers) {
        w.beginObject();
        w.field("key", layerKey(ll.layer));
        w.field("name", engine::layerName(ll.layer));
        w.key("categories").stringArray(ll.categories);
        if (!ll.groups.empty()) {
            w.key("groups").beginArray();
            for (const auto& g : ll.groups) w.beginObject().field("name", g.name).field("theme", g.theme).endObject();
            w.endArray();
        }
        w.key("blendModes").stringArray(engine::blendModeNames(ll.layer));
        w.key("configs").beginArray();
        for (const auto& e : ll.configs) {
            w.beginObject();
            w.field("id", e.id);
            w.field("name", e.name);
            if (e.parenthesized) w.field("parenthesized", true);
            if (e.movie) w.field("movie", true);
            if (e.bpm > 0) w.field("bpm", e.bpm);
            if (e.noColorKey) w.field("noColorKey", true);
            if (e.beat) w.field("beat", true);
            if (!e.categories.empty()) w.key("categories").stringArray(e.categories);
            w.endObject();
        }
        w.endArray();
        w.endObject();
    }
    w.endArray();
    w.endObject();
    return w.take();
}

namespace {
void writeAudio(json::Writer& w, const engine::AudioState& a) {
    w.key("audio").beginObject();
    w.field("level", a.level).field("bass", a.bass).field("energy", a.energy);
    w.field("mid", a.mid).field("treble", a.treble).field("beat", a.beat).field("tempo", a.tempo);
    if (a.linkEnabled) {
        w.key("link").beginObject();
        w.field("peers", a.linkPeers).field("on", a.linkOn).field("bpm", a.linkBpm);
        w.field("listening", a.linkListening).field("joining", a.linkJoining);
        w.field("beat", a.linkBeat).field("phase", a.linkPhase).field("quantum", a.linkQuantum);
        w.endObject();
    }
    w.endObject();
}
// Rhythm (PLAN 5O): the changing part (live event and full state) ...
void writeRhythmLive(json::Writer& w, const engine::RhythmState& r) {
    w.field("phrase", r.phrase).field("barInPhrase", r.barInPhrase).field("bpm", r.bpm);
    w.field("confidence", r.confidence).field("kick", r.kick);
    w.field("breakdown", r.breakdown).field("build", r.build).field("drop", r.drop).field("drops", r.drops);
    w.field("decks", r.decks).field("deckInputs", r.deckInputs).field("nowPlayingLine", r.nowPlayingLine);
    w.key("dancing").stringArray(r.dancing);
    w.key("danceRuns").beginArray();
    for (const auto& d : r.danceRuns)
        w.beginObject().field("id", static_cast<unsigned long long>(d.id)).field("name", d.name).field("guest", d.guest)
            .field("leaving", d.leaving).endObject();
    w.endArray();
    w.key("guestBook").beginArray();
    for (const auto& g : r.guestBook)
        w.beginObject().field("id", g.id).field("label", g.label).field("style", g.style).field("seen", g.seen).endObject();
    w.endArray();
    w.key("guestsBlocked").beginArray();
    for (const auto& [id, name] : r.guestsBlocked) w.beginObject().field("id", id).field("name", name).endObject();
    w.endArray();
    w.field("danceMode", r.danceMode);
}
}  // namespace

std::string liveToJson(const engine::EngineState& s) {
    json::Writer w;
    w.beginObject();
    w.field("simTime", s.simTime);
    w.field("fps", s.displayFps);
    if (s.audio.valid) writeAudio(w, s.audio);
    w.key("layers").beginArray();
    for (const auto& l : s.layers) {
        w.beginObject();
        w.field("key", layerKey(l.layer));
        w.field("progress", l.progress).field("weight", l.weight).field("nextSwitchIn", l.nextSwitchIn);
        if (l.layer == Layer::Sprite) w.field("nextMovieIn", l.nextMovieIn);
        w.endObject();
    }
    w.endArray();
    w.key("video").beginObject().field("crossfade", s.video.crossfade);
    if (s.video.lightsAvailable && s.video.lightsOn)  // room lights preview: the panels' colours as last sent
        w.key("lights").beginObject().field("colours", s.video.lightsColours).field("hz", s.video.lightsHz)
            .field("pulsing", s.video.lightsPulsing).endObject();
    w.endObject();
    w.key("rhythm").beginObject();
    writeRhythmLive(w, s.rhythm);
    w.endObject();
    w.endObject();
    return w.take();
}

std::string stateToJson(const engine::EngineState& s, bool stable) {
    json::Writer w;
    w.beginObject();
    if (!stable) {
        w.field("simTime", s.simTime);
        w.field("simSteps", static_cast<unsigned long long>(s.simSteps));
        w.field("fps", s.displayFps);
        if (s.perf.valid) {
            const engine::PerfState& p = s.perf;
            w.key("perf").beginObject();
            w.field("frames", static_cast<unsigned long long>(p.frames));
            w.field("hitches", static_cast<unsigned long long>(p.hitches));
            w.field("p50", p.workP50).field("p99", p.workP99).field("max", p.workMax);
            w.field("gpuP99", p.gpuP99).field("budget", p.budgetMs).field("displayHz", p.displayHz);
            w.field("renderAllocs", static_cast<unsigned long long>(p.renderAllocs));
            w.key("histEdges").beginArray();
            for (double e : p.histEdges) w.value(e);
            w.endArray();
            w.key("hist").beginArray();
            for (auto c : p.hist) w.value(static_cast<unsigned long long>(c));
            w.endArray();
            if (!p.lastHitch.empty()) w.field("lastHitch", p.lastHitch);
            w.endObject();
        }
        if (s.audio.valid) writeAudio(w, s.audio);
    }
    {
        // ... and the settings (stable: a change sends a full state).
        const engine::RhythmState& r = s.rhythm;
        w.key("rhythm").beginObject();
        if (!stable) writeRhythmLive(w, r);
        w.field("on", r.phraseOn).field("bars", r.bars).field("quantum", r.quantum).field("how", r.how);
        w.field("autoAlign", r.autoAlign).field("quantize", r.quantize).field("quantizeManual", r.quantizeManual);
        w.field("quantizeSpawn", r.quantizeSpawn).field("kickZoom", r.kickZoom).field("kickRotate", r.kickRotate);
        w.field("kickDecayMs", r.kickDecayMs).field("dropSwitch", r.dropSwitch);
        w.field("deck", r.deck).field("deckStarts", r.deckStarts);
        w.field("nowPlaying", r.nowPlaying).field("nowPlayingSize", r.nowPlayingSize);
        w.field("nowPlayingCorner", r.nowPlayingCorner).field("nowPlayingSwitch", r.nowPlayingSwitch);
        w.field("nowPlayingShadow", r.nowPlayingShadow).field("nowPlayingShadowFrom", r.nowPlayingShadowFrom);
        w.field("danceFloor", r.danceFloor).field("danceLighting", r.danceLighting);
        w.field("danceReflection", r.danceReflection);
        w.field("danceRandom", r.danceRandom);
        w.field("danceHidden", r.danceHidden);
        w.field("guests", r.guests).field("guestMinutes", r.guestMinutes).field("guestStatus", r.guestStatus)
            .field("guestArrivals", r.guestArrivals);
        w.field("danceOutline", r.danceOutline).field("danceHeight", r.danceHeight).field("danceHuman", r.danceHuman);
        w.field("danceMax", r.danceMax).field("danceMocap", r.danceMocap).field("danceTry", r.danceTry);
        w.endObject();
    }
    w.field("simRate", s.simRate);
    w.field("theme", s.theme);
    w.key("themes").stringArray(s.themes);
    bool anyOn = false, allOn = !s.layers.empty(), anyHeld = false, allHeld = !s.layers.empty();
    for (const auto& l : s.layers) {
        if (l.layer == Layer::Union) continue;
        anyOn = anyOn || l.slideshowEnabled;
        allOn = allOn && l.slideshowEnabled;
        anyHeld = anyHeld || l.held;
        allHeld = allHeld && l.held;
    }
    w.key("slideshow").beginObject();
    w.field("anyOn", anyOn).field("allOn", allOn).field("anyHeld", anyHeld).field("allHeld", allHeld);
    w.endObject();
    w.key("layers").beginArray();
    for (const auto& l : s.layers) writeLayerState(w, l, stable);
    w.endArray();
    {
        const engine::VideoState& v = s.video;
        w.key("video").beginObject();
        w.field("active", v.layerActive);
        w.field("source", v.layerSource).field("incoming", v.layerIncoming).field("pending", v.layerPending);
        w.field("spec", v.layerSpec);
        w.field("waiting", v.layerWaiting);
        w.field("blend", v.blend).field("fit", v.fit).field("order", v.order).field("key", v.key);
        w.field("keyColor", v.keyColor);
        w.field("opacity", v.opacity).field("threshold", v.threshold).field("tolerance", v.tolerance);
        w.field("softness", v.softness).field("crossfadeSeconds", v.crossfadeSeconds).field("rate", v.rate);
        w.field("offset", v.offset).field("loop", v.loop).field("syphon", v.toSyphon);
        w.key("sprites").stringArray(v.sprites);
        w.field("spritesHeld", v.spritesHeld);
        w.field("beatSync", v.beatSync).field("beatSource", v.beatSource).field("beatConfident", v.beatConfident);
        w.field("beatOffsetMs", v.beatOffsetMs);
        w.field("spriteColour", v.spriteColour);
        w.field("syphonAlpha", v.syphonAlpha).field("windowVisuals", v.windowVisuals).field("keySimilarity", v.keySimilarity);
        w.field("keySmoothness", v.keySmoothness);
        w.key("body").beginObject();
        w.field("track", v.bodyTrack).field("quality", v.bodyQuality).field("front", v.bodyFront);
        w.field("burn", v.bodyBurn).field("burnAmount", v.bodyBurnAmount).field("trails", v.bodyTrails);
        w.field("trailWidth", v.bodyTrailWidth).field("show", v.bodyShow).field("sync", v.bodySync);
        if (!stable) {
            w.field("running", v.bodyRunning).field("mask", v.bodyMask).field("presence", v.bodyPresence);
            w.field("fps", v.bodyFps).field("ms", v.bodyMs).field("joints", v.bodyJoints).field("error", v.bodyError);
            w.field("maskMs", v.bodyMaskMs).field("poseMs", v.bodyPoseMs).field("camFps", v.bodyCamFps);
        }
        w.endObject();
        w.key("lights").beginObject();
        w.field("available", v.lightsAvailable).field("on", v.lightsOn).field("brightness", v.lightsBrightness);
        w.field("saturation", v.lightsSaturation).field("floor", v.lightsFloor).field("smoothing", v.lightsSmoothing);
        w.field("state", v.lightsState).field("message", v.lightsMessage).field("device", v.lightsDevice);
        w.field("panels", v.lightsPanels);
        w.field("pulse", v.lightsPulse).field("pulseTiming", v.lightsPulseTiming).field("side", v.lightsSide);
        w.key("layout").beginArray();
        for (double x : v.lightsLayout) w.value(std::round(x * 10) / 10);
        w.endArray();
        if (!stable) w.field("hz", v.lightsHz).field("pulsing", v.lightsPulsing).field("colours", v.lightsColours);
        w.endObject();
        w.key("backdrop").beginObject();
        w.field("on", v.backdropOn).field("spec", v.backdropSpec).field("source", v.backdropSource);
        w.field("pending", v.backdropPending).field("waiting", v.backdropWaiting);
        w.field("mirror", v.backdropMirror).field("fit", v.backdropFit).field("opacity", v.backdropOpacity);
        if (!stable) {
            w.field("level", v.backdropLevel);
            w.field("width", v.backdropWidth).field("height", v.backdropHeight);
        }
        w.endObject();
        w.field("error", v.lastError);
        if (!stable) {
            w.field("beatBpm", v.beatBpm).field("beatLock", v.beatLock);
            w.key("beatSprites").stringArray(v.beatSprites);
            w.field("crossfade", v.crossfade);
            w.field("width", v.width).field("height", v.height).field("fps", v.fps).field("duration", v.duration);
            w.field("decodeMsAvg", v.decodeMsAvg).field("decodeMsMax", v.decodeMsMax);
            w.field("decoded", static_cast<unsigned long long>(v.decoded));
            w.field("dropped", static_cast<unsigned long long>(v.dropped));
        }
        w.endObject();
    }
    if (s.look.valid) {
        const engine::LookState& l = s.look;
        auto values = [&](const char* name, const engine::LookValues& v) {
            w.key(name).beginObject();
            for (const auto& k : engine::lookKeys()) w.field(k.id, v[k.key]);
            w.endObject();
        };
        w.key("look").beginObject();
        values("current", l.current);
        values("saved", l.saved);
        values("slotA", l.slotA);
        values("slotB", l.slotB);
        w.field("slot", l.slot ? std::string(1, l.slot) : std::string());
        w.field("fieldPrecision", l.fieldPrecision);
        w.key("differs").beginArray();
        for (const auto& k : engine::lookKeys())
            if (l.current[k.key] != l.saved[k.key]) w.value(k.id);
        w.endArray();
        w.endObject();
    }
    w.key("log").stringArray(s.recentLog);
    w.endObject();
    return w.take();
}

std::string videoSourcesToJson(const VideoSourceList& v) {
    json::Writer w;
    w.beginObject();
    w.key("files").beginArray();
    for (const auto& f : v.files) {
        w.beginObject();
        w.field("id", f.id).field("name", f.name).field("source", "file:" + f.path);
        w.endObject();
    }
    w.endArray();
    w.key("cameras").beginArray();
    for (const auto& c : v.cameras) {
        w.beginObject();
        w.field("id", c.id).field("name", c.name).field("source", "camera:" + c.id);
        w.endObject();
    }
    w.endArray();
    w.key("syphon").beginArray();
    for (const auto& x : v.syphon) {
        w.beginObject();
        w.field("name", x.name).field("app", x.app).field("display", x.display);
        w.field("source", "syphon:" + x.display).field("self", x.self);
        w.endObject();
    }
    w.endArray();
    static const char* const kCamera[] = {"ask", "granted", "denied", "waiting", "unavailable"};
    w.field("cameraPermission", v.cameraPermission >= 0 && v.cameraPermission <= 4 ? kCamera[v.cameraPermission] : "ask");
    w.endObject();
    return w.take();
}

std::string validateCommand(const Command& c, const Library* lib, int* status) {
    auto bad = [&](const std::string& m, int code = 400) {
        if (status) *status = code;
        return m;
    };
    const char* ln = engine::layerName(c.layer);
    switch (c.type) {
        case CommandType::Switch:
            if (c.target == engine::SwitchTarget::Name) {
                if (c.text.empty() || c.text.size() > 512) return bad("config name empty or too long");
                if (lib)
                    if (const LibraryLayer* ll = lib->find(c.layer); ll && !ll->configs.empty() &&
                                                                     !configKnown(*ll, c.text))
                        return bad(std::string(ln) + " \"" + c.text + "\" not found", 404);
            }
            break;
        case CommandType::SetSlideshowInterval:
            // Particles / Sprites / Solids / Organics: 0 = the slideshow starts none.
            if (engine::isSpawnLayer(c.layer) && !c.allLayers && c.value == 0) break;
            if (!std::isfinite(c.value) || c.value < 0.5 || c.value > 3600)
                return bad("interval must be 0.5..3600 seconds (0 = off for particles, sprites, solids, organics and "
                           "dancers)");
            break;
        case CommandType::SetMovieInterval:
            if (!std::isfinite(c.value) || (c.value != 0 && (c.value < 0.5 || c.value > 3600)))
                return bad("movie interval must be 0 (off) or 0.5..3600 seconds");
            break;
        case CommandType::SetSpriteMix:
            if (!std::isfinite(c.value) || (!c.flag && (c.value < 0 || c.value > 1)))
                return bad("sprite mix must be 0..1");
            break;
        case CommandType::SetSpawnDuration:
            if (c.layer == engine::Layer::Dancer && c.value == 0) break;  // dancers: 0 = forever
            if (!std::isfinite(c.value) || c.value < 0.5 || c.value > 3600)
                return bad(c.layer == engine::Layer::Dancer ? "duration must be 0.5..3600 seconds, or 0 (forever)"
                                                            : "duration must be 0.5..3600 seconds");
            break;
        case CommandType::SetTransitionDuration:
            if (!std::isfinite(c.value) || c.value < 0 || c.value > 600)
                return bad("transition duration must be 0..600 seconds");
            break;
        case CommandType::SetCrossfader:
            if (!std::isfinite(c.value) || c.value < 0 || c.value > 1) return bad("crossfader weight must be 0..1");
            break;
        case CommandType::SetBlendMode:
            if (engine::blendModeNames(c.layer).empty()) return bad(std::string(ln) + " has no blend modes");
            if (!engine::findBlendMode(c.layer, c.text))
                return bad(std::string(ln) + ": unknown blend mode \"" + c.text + "\"");
            break;
        case CommandType::SetTheme: {
            if (c.text.empty() || c.text.size() > 256) return bad("theme name empty or too long");
            if (lib && !ieq(c.text, "(Standard)")) {
                bool found = false;
                for (const auto& t : lib->themes)  // the listed name, or a G-Force theme's bare title
                    found = found || ieq(t, c.text) || ieq(t, engine::kGForceThemePrefix + c.text);
                if (!found) return bad("theme \"" + c.text + "\" not found", 404);
            }
            break;
        }
        case CommandType::VideoStart:
            if (c.text.empty() || c.text.size() > 4096) return bad("video source empty or too long");
            break;
        case CommandType::Look:
            if (auto e = engine::validateLook(c); !e.empty()) return bad(e);
            break;
        case CommandType::VideoSet: {
            // Keys the Visualizer handles itself (not colour layer settings).
            const std::string key = lower(c.text), val = lower(c.arg);
            const bool on = val == "1" || val == "on" || val == "true" || val == "yes";
            const bool off = val == "0" || val == "off" || val == "false" || val == "no";
            if (key == "beatsync") {
                if (!on && !off) return bad("video: beatsync: on | off");
                break;
            }
            if (key == "beatoffset") {
                char* end = nullptr;
                const double ms = std::strtod(val.c_str(), &end);
                if (val.empty() || *end || !(ms >= -200.0 && ms <= 500.0))
                    return bad("video: beatoffset: -200..500 ms");
                break;
            }
            if (key == "spritecolour" || key == "spritecolor") {
                if (!on && !off && val != "over") return bad("video: spritecolour: over | off");
                break;
            }
            if (key == "syphonalpha") {
                if (!on && !off) return bad("video: syphonalpha: on | off");
                break;
            }
            if (key == "windowvisuals") {
                if (!on && !off) return bad("video: windowvisuals: on | off");
                break;
            }
            if (key == "lightspair") {
                if (val != "start" && val != "cancel") return bad("video: lightspair: start | cancel");
                break;
            }
            if (key.rfind("lights", 0) == 0) {  // room lights (docs/lights.md)
                engine::LightsSettings scratch;
                if (auto e = engine::applyLightsSetting(scratch, key.substr(6), c.arg); !e.empty()) return bad("video: " + e);
                break;
            }
            if (key == "keysimilarity" || key == "keysmoothness") {
                char* end = nullptr;
                const double x = std::strtod(val.c_str(), &end);
                if (val.empty() || *end || !(x >= 1 && x <= 1000)) return bad("video: " + key + ": 1..1000");
                break;
            }
            if (key == "backdrop") {
                if (c.arg.empty() || c.arg.size() > 4096) return bad("video: backdrop: a source, or off");
                break;
            }
            if (key.size() > 4 && key.rfind("body", 0) == 0) {
                engine::BodySettings scratch;
                if (auto e = engine::applyBodySetting(scratch, key.substr(4), c.arg); !e.empty())
                    return bad("video: body " + e);
                break;
            }
            if (key == "backdropmirror") {
                if (!on && !off) return bad("video: backdropmirror: on | off");
                break;
            }
            if (key == "backdropfit") {
                if (!engine::parseVideoFit(val)) return bad("video: backdropfit: cover | contain | stretch");
                break;
            }
            if (key == "backdropopacity") {
                char* end = nullptr;
                const double x = std::strtod(val.c_str(), &end);
                if (val.empty() || *end || !(x >= 0 && x <= 1)) return bad("video: backdropopacity: 0..1");
                break;
            }
            engine::VideoLayerParams scratch;
            if (auto e = engine::applyVideoSetting(scratch, c.text, c.arg); !e.empty()) return bad("video: " + e);
            break;
        }
        default: break;
    }
    return {};
}

CommandParseResult parseCommandJson(std::string_view body, const Library* lib) {
    CommandParseResult r;
    auto fail = [&](std::string m, int code = 400) {
        r.commands.clear();
        r.status = code;
        r.error = std::move(m);
        return r;
    };
    if (body.size() > 64 * 1024) return fail("body too large", 413);
    std::string err;
    auto doc = json::parse(body, &err);
    if (!doc) return fail("invalid JSON: " + err);
    std::vector<const json::Value*> items;
    if (doc->isArray()) {
        if (doc->array.empty()) return fail("empty command list");
        if (doc->array.size() > 16) return fail("at most 16 commands per request");
        for (const auto& v : doc->array) items.push_back(&v);
    } else {
        items.push_back(&*doc);
    }
    for (const json::Value* v : items) {
        Command c;
        if (auto e = commandFromObject(*v, c); !e.empty()) return fail(e);
        int code = 400;
        if (auto e = validateCommand(c, lib, &code); !e.empty()) return fail(e, code);
        r.commands.push_back(std::move(c));
    }
    return r;
}

}  // namespace reforce::web
