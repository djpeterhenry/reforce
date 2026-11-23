#include "app/Director.hpp"

#include <algorithm>

#include "audio/Analyzer.hpp"
#include <cctype>
#include <cmath>
#include <cstdio>

#include "engine/Log.hpp"
#include "engine/Perf.hpp"
#include "sync/LinkClock.hpp"

namespace reforce::app {
using engine::Layer;
using scene::SceneType;
namespace fs = std::filesystem;

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Lower-case, surrounding spaces removed ("  dj peter henry" == "DJ Peter Henry").
std::string looseName(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return lower(s.substr(a, b - a));
}

const engine::LayerPrefs& layerPrefs(const engine::Settings& p, Layer l) {
    switch (l) {
        case Layer::WaveShape: return p.waveShape;
        case Layer::ColorMap: return p.colorMap;
        case Layer::FlowField: return p.flowField;
        case Layer::Union: return p.union_;
        case Layer::Particle: return p.particle;
        case Layer::Sprite: return p.sprite;
        case Layer::Object: return p.object;
        case Layer::Dancer: return p.dancer;
    }
    return p.waveShape;
}

}  // namespace

Director::Director(std::unique_ptr<Visualizer> vis, const engine::Settings& prefs, const DirectorOptions& opts)
    : vis_(std::move(vis)), prefs_(prefs), opts_(opts), rng_(opts.seed), spawnRng_(opts.seed * 7919 + 17) {
    // Objects hold two scene types (solids and organics, objectSolids_ says which); d.type is Organic for them.
    const Layer ls[8] = {Layer::WaveShape, Layer::ColorMap, Layer::FlowField, Layer::Union, Layer::Particle,
                         Layer::Sprite,    Layer::Object,   Layer::Dancer};
    const SceneType ts[8] = {SceneType::WaveShape, SceneType::ColorMap, SceneType::FlowField,
                             SceneType::FlowFieldUnion, SceneType::Particle, SceneType::Sprite,
                             SceneType::Organic,   SceneType::Dancer};
    for (int i = 0; i < 8; ++i) {
        LayerData& d = layers_[std::size_t(i)];
        d.layer = ls[i];
        d.type = ts[i];
        const engine::LayerPrefs& lp = layerPrefs(prefs, ls[i]);
        d.slideshow = engine::LayerSlideshow(lp);
        if (opts.slideshow) d.slideshow.setEnabled(*opts.slideshow && (ls[i] != Layer::Union || lp.slideshowOnStartup));
        d.fn = std::make_shared<engine::TransitionFunction>(lp.transitionFunction);
        if (!d.fn->error().empty())
            rlog("[reforce] %s.Transition.Function \"%s\": %s (using t)\n", engine::layerName(ls[i]),
                         lp.transitionFunction.c_str(), d.fn->error().c_str());
        if (!opts.scenesDir.empty() && ls[i] != Layer::Object) d.catalog = engine::listScenes(opts.scenesDir, ts[i]);
    }
    // Objects: solids (reforce's own content, then G-Force-style scenes/, then reforce.Solid.Dir as "Extra/...") and
    // organics (content, then scenes/); the first of an id wins.
    {
        LayerData& objects = layers_[6];
        const auto excludeRules = engine::parseExcludeList(prefs.exclude);
        auto add = [&](std::vector<engine::SceneEntry> more, SceneType type) {
            engine::removeExcluded(more, type, excludeRules);
            for (auto& e : more) {
                const bool dup = std::any_of(objects.catalog.begin(), objects.catalog.end(),
                                             [&](const engine::SceneEntry& c) { return lower(c.id) == lower(e.id); });
                if (dup) continue;
                if (type == SceneType::Solid) objectSolids_.insert(lower(e.id));
                objects.catalog.push_back(std::move(e));
            }
        };
        if (!opts.contentDir.empty()) add(engine::listScenes(opts.contentDir, SceneType::Solid), SceneType::Solid);
        if (!opts.scenesDir.empty()) add(engine::listScenes(opts.scenesDir, SceneType::Solid), SceneType::Solid);
        if (!prefs.solidDir.empty())
            add(engine::listSceneFolder(prefs.solidDir, SceneType::Solid, "Extra/"), SceneType::Solid);
        if (!opts.contentDir.empty()) add(engine::listScenes(opts.contentDir, SceneType::Organic), SceneType::Organic);
        if (!opts.scenesDir.empty()) add(engine::listScenes(opts.scenesDir, SceneType::Organic), SceneType::Organic);
        objectInterval_ = prefs.object.slideshowInterval;
        if (!objects.catalog.empty())
            rlog("[reforce] objects: %zu scene file(s) (%zu solids, %zu organics)\n", objects.catalog.size(),
                 objectSolids_.size(), objects.catalog.size() - objectSolids_.size());
    }
    // Dancers (PLAN 5P): reforce's own content, then G-Force-style scenes/.
    {
        LayerData& dancers = layers_[7];
        std::vector<engine::SceneEntry> fromScenes = std::move(dancers.catalog);
        dancers.catalog.clear();
        if (!opts.contentDir.empty()) dancers.catalog = engine::listScenes(opts.contentDir, SceneType::Dancer);
        for (auto& e : fromScenes) {
            const bool dup = std::any_of(dancers.catalog.begin(), dancers.catalog.end(),
                                         [&](const engine::SceneEntry& c) { return lower(c.id) == lower(e.id); });
            if (!dup) dancers.catalog.push_back(std::move(e));
        }
        dancerInterval_ = prefs.dancer.slideshowInterval;
        if (!dancers.catalog.empty()) rlog("[reforce] dancers: %zu scene file(s)\n", dancers.catalog.size());
    }
    movieTimer_ = engine::LayerSlideshow(prefs.sprite);
    movieTimer_.setInterval(prefs.movieSpriteInterval >= 0 ? prefs.movieSpriteInterval
                                                           : prefs.sprite.slideshowInterval);
    spriteHidden_ = engine::parseIdList(prefs.spriteHidden);
    // Skip list (reforce.Exclude): applied to every catalog, external sprites included.
    std::vector<std::string> excludeErrors;
    const auto excludeRules = engine::parseExcludeList(prefs.exclude, &excludeErrors);
    for (const auto& e : excludeErrors) rlog("[reforce] %s (ignored)\n", e.c_str());
    // Phase 5F: an external sprite folder (images and movies) joins the Sprite
    // catalog, like G-Force's ExternalSprites.Path; movies also feed the video
    // source picker together with reforce.VideoDir.
    LayerData& sprites = layers_[5];
    for (const auto& e : sprites.catalog)
        if (engine::isSpriteVideoFile(e.path)) videoFiles_.push_back({"Sprites/" + e.id, e.name, e.path});
    if (!prefs.externalSpritesDir.empty()) {
        auto ext = engine::listMediaFolder(prefs.externalSpritesDir, "External/", false);
        rlog("[reforce] external sprites (reforce.ExternalSpritesDir %s): %zu file(s)\n",
             prefs.externalSpritesDir.c_str(), ext.size());
        for (const auto& e : ext) {
            if (engine::isSpriteVideoFile(e.path)) videoFiles_.push_back(e);
            sprites.catalog.push_back(e);
        }
    }
    for (int i = 0; i < 8; ++i) {
        LayerData& d = layers_[std::size_t(i)];
        if (d.layer == Layer::Object) continue;  // excluded per scene type while listing
        if (const std::size_t n = engine::removeExcluded(d.catalog, d.type, excludeRules))
            rlog("[reforce] reforce.Exclude: %zu %s(s) skipped\n", n, std::string(scene::sceneTypeName(d.type)).c_str());
    }
    // Movie sprites skipped above stay out of the video source picker too.
    videoFiles_.erase(std::remove_if(videoFiles_.begin(), videoFiles_.end(),
                                     [&](const engine::SceneEntry& v) {
                                         engine::SceneEntry e = v;
                                         if (e.id.rfind("Sprites/", 0) == 0) e.id = e.id.substr(8);
                                         return engine::isExcluded(excludeRules, SceneType::Sprite, e);
                                     }),
                      videoFiles_.end());
    if (!prefs.videoDir.empty())
        for (auto& e : engine::listMediaFolder(prefs.videoDir, "Videos/", true)) {
            std::error_code ec;
            const bool dup = std::any_of(videoFiles_.begin(), videoFiles_.end(),
                                         [&](const engine::SceneEntry& v) { return fs::equivalent(v.path, e.path, ec); });
            if (!dup) videoFiles_.push_back(std::move(e));
        }
    for (const auto& err : prefs.settingErrors) rlog("[reforce] settings: %s (ignored)\n", err.c_str());
    // FlowField transitions with a Union use the FlowField curve; the Union
    // only shapes the per-pixel weight.
    themes_ = engine::ThemeLibrary::loadAll(opts.scenesDir, opts.contentDir);

    // Blend modes from settings (reforce.Blend.<Type>).
    const std::pair<Layer, std::string> modes[] = {{Layer::WaveShape, prefs.blendWaveShape},
                                                   {Layer::ColorMap, prefs.blendColorMap},
                                                   {Layer::FlowField, prefs.blendFlowField}};
    for (const auto& [l, name] : modes) {
        if (auto m = engine::findBlendMode(l, name)) vis_->setBlendMode(l, *m);
        else rlog("[reforce] reforce.Blend.%s: unknown mode \"%s\" (using gforce)\n",
                          engine::layerName(l), name.c_str());
    }
    applyTheme(prefs.startupTheme);
    // Phase 5D: compile every config of the catalog on a background worker
    // (sprite params, .png ColorMaps decoded), so switches find them ready.
    // The upcoming ones are moved to the front as the slideshow arms.
    for (LayerData& d : layers_)
        for (const auto& e : d.catalog) vis_->prefetchConfig(e.path, entryType(d, e));
}

double Director::startBars(const LayerData& d, const engine::SceneEntry& e) {
    if (d.layer != Layer::Object) return 0;
    const auto it = startBars_.find(e.id);
    if (it != startBars_.end()) return it->second;
    double bars = 0;
    if (auto cfg = vis_->compileFile(e.path, entryType(d, e)))
        if (const scene::CompiledKey* k = cfg->find("SBAR")) {
            char* end = nullptr;
            const double v = std::strtod(k->text.c_str(), &end);
            if (end != k->text.c_str() && v > 0 && v <= 1024) bars = v;
        }
    startBars_[e.id] = bars;
    return bars;
}

SceneType Director::entryType(const LayerData& d, const engine::SceneEntry& e) const {
    if (d.layer != Layer::Object) return d.type;
    return objectSolids_.count(lower(e.id)) ? SceneType::Solid : SceneType::Organic;
}

void Director::prefetchNext(LayerData& d, engine::SchedRng& rng) {
    // Peeks with copies: the real shuffle and RNG sequence stay as they are.
    engine::Playlist copy = d.playlist;
    engine::SchedRng r = rng;
    if (const engine::SceneEntry* e = resolve(d, copy.peek(r))) vis_->prefetchConfig(e->path, entryType(d, *e), true);
}

Director::LayerData* Director::data(Layer layer) {
    for (auto& d : layers_)
        if (d.layer == layer) return &d;
    return nullptr;
}
const Director::LayerData* Director::data(Layer layer) const {
    for (const auto& d : layers_)
        if (d.layer == layer) return &d;
    return nullptr;
}

std::size_t Director::catalogSize(Layer layer) const {
    const LayerData* d = data(layer);
    return d ? d->catalog.size() : 0;
}
std::size_t Director::playlistSize(Layer layer) const {
    const LayerData* d = data(layer);
    return d ? d->playlist.items().size() : 0;
}

void Director::log(const std::string& msg) {
    publishDue_ = true;
    rlog("[reforce] %s\n", msg.c_str());
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.2f ", lastT_);
    recent_.push_back(buf + msg);
    while (recent_.size() > 32) recent_.pop_front();
}

void Director::applyTheme(const std::string& name) {
    perf::tag(perf::TagTheme);
    const engine::Theme* theme = themes_.find(name);
    if (!theme && lower(name) != "(standard)" && !name.empty())
        log("theme \"" + name + "\" not found; using (Standard)");
    theme_ = theme ? theme->name : std::string("(Standard)");
    for (LayerData& d : layers_) {
        // Parenthesized names and hidden sprites (reforce.Sprite.Hidden) never come up.
        auto skip = [&](const engine::SceneEntry& e) {
            return engine::excludedFromSlideshow(e.name) || hiddenSprite(d, e);
        };
        std::vector<std::string> names;
        for (const auto& e : d.catalog)
            if (!skip(e)) names.push_back(e.name);
        std::vector<std::string> warnings;
        const std::vector<std::string> keepNames =
            d.layer == Layer::Union ? names
                                    : themes_.filter(theme,
                                                     d.layer == Layer::Object ? std::string("Object")
                                                                              : std::string(scene::sceneTypeName(d.type)),
                                                     names, &warnings);
        for (const auto& w : warnings) log("theme " + theme_ + ": " + w);
        std::vector<std::string> ids, movies;
        // Sprites: movies go to their own shuffle (own timer); images stay in the layer's.
        auto add = [&](const engine::SceneEntry& e) {
            (d.layer == Layer::Sprite && engine::isSpriteVideoFile(e.path) ? movies : ids).push_back(e.id);
        };
        for (const auto& e : d.catalog) {
            if (skip(e)) continue;
            if (std::find(keepNames.begin(), keepNames.end(), e.name) != keepNames.end()) add(e);
        }
        if (ids.empty() && movies.empty() && !d.catalog.empty()) {
            log(std::string("theme ") + theme_ + " admits no " + engine::layerName(d.layer) + "; using all");
            for (const auto& e : d.catalog)
                if (!skip(e)) add(e);
        }
        const std::string cur = d.playlist.current();
        d.playlist.setItems(std::move(ids));
        if (!cur.empty()) d.playlist.noteCurrent(cur);
        if (d.layer == Layer::Sprite) {
            const std::string mcur = moviePlaylist_.current();
            moviePlaylist_.setItems(std::move(movies));
            if (!mcur.empty()) moviePlaylist_.noteCurrent(mcur);
        }
    }
    // Objects are not in the default slideshow: a theme may start them (Object.Slideshow.Interval.Duration).
    LayerData& objects = layers_[6];
    const double objectEvery = theme && theme->objectInterval ? *theme->objectInterval : objectInterval_;
    if (objects.slideshow.interval() != objectEvery) {
        objects.slideshow.setInterval(objectEvery);
        objects.slideshow.disarm();  // re-armed by the next poll (a first object one interval from now)
    }
    LayerData& dancers = layers_[7];
    const double dancerEvery = theme && theme->dancerInterval ? *theme->dancerInterval : dancerInterval_;
    if (dancers.slideshow.interval() != dancerEvery) {
        dancers.slideshow.setInterval(dancerEvery);
        dancers.slideshow.disarm();
    }
    preloadId_.clear();
    spritePrefetchDue_ = true;
    if (!layers_[0].catalog.empty() || theme)
        log("theme " + theme_ + ": " + std::to_string(layers_[0].playlist.items().size()) + " WaveShapes, " +
            std::to_string(layers_[1].playlist.items().size()) + " ColorMaps, " +
            std::to_string(layers_[2].playlist.items().size()) + " FlowFields, " +
            std::to_string(layers_[3].playlist.items().size()) + " Unions, " +
            std::to_string(layers_[4].playlist.items().size()) + " Particles, " +
            std::to_string(layers_[5].playlist.items().size()) + " Sprites, " +
            std::to_string(moviePlaylist_.items().size()) + " movie sprites, " +
            std::to_string(layers_[6].playlist.items().size()) + " Objects, " +
            std::to_string(layers_[7].playlist.items().size()) + " Dancers in the slideshow" +
            (objectEvery > 0 ? " (an object every " + std::to_string(int(std::lround(objectEvery))) + " s)" : "") +
            (dancerEvery > 0 ? " (a dancer every " + std::to_string(int(std::lround(dancerEvery))) + " s)" : ""));
}

bool Director::hiddenSprite(const LayerData& d, const engine::SceneEntry& e) const {
    return d.layer == Layer::Sprite && engine::idListHas(spriteHidden_, e.id);
}

const engine::SceneEntry* Director::resolve(const LayerData& d, const std::string& name) const {
    if (name.empty()) return nullptr;
    std::string want = lower(name);
    std::replace(want.begin(), want.end(), '\\', '/');
    for (const char* ext : {".txt", ".png"})
        if (want.size() > 4 && want.compare(want.size() - 4, 4, ext) == 0) want.resize(want.size() - 4);
    const engine::SceneEntry* tail = nullptr;
    for (const auto& e : d.catalog) {
        const std::string id = lower(e.id);
        if (id == want) return &e;
        if (!tail && (lower(e.name) == want ||
                      (id.size() > want.size() && id.compare(id.size() - want.size(), want.size(), want) == 0 &&
                       id[id.size() - want.size() - 1] == '/')))
            tail = &e;
    }
    if (tail) return tail;
    // Names with stray spaces (e.g. a sprite file "  dj peter henry.png").
    const std::string loose = looseName(want);
    for (const auto& e : d.catalog) {
        const std::string id = lower(e.id);
        const auto slash = id.find_last_of('/');
        if (looseName(e.name) == loose || looseName(id) == loose ||
            (slash != std::string::npos && looseName(id.substr(0, slash + 1) + looseName(id.substr(slash + 1))) == loose))
            return &e;
    }
    std::error_code ec;
    for (const auto& e : d.catalog)
        if (fs::equivalent(e.path, fs::path(name), ec)) return &e;
    return nullptr;
}

FadeSpec Director::spawnFade(LayerData& d, bool quick) {
    // Fade and hold for this instance (probe #19): Transition.Duration and
    // Duration, each with its relative deviation. Manual starts fade in over
    // Transition.Duration.Quick (like manual switches of the other layers).
    const engine::LayerPrefs& lp = layerPrefs(prefs_, d.layer);
    FadeSpec fade;
    fade.fn = d.fn;
    fade.fade = d.slideshow.sampleTransitionDuration(spawnRng_);  // drawn always: keeps the RNG sequence
    if (quick) fade.fade = d.slideshow.quickDuration();
    const double base = lp.duration > 0 ? lp.duration
                        : d.layer == Layer::Particle ? 25.0
                        : d.layer == Layer::Object ? 40.0
                        : d.layer == Layer::Dancer ? 90.0
                                                     : 45.0;
    fade.defaultHold = engine::sampleWithDeviation(base, lp.durationDeviation, spawnRng_.normal(), 0.0);
    // Dancer.Duration 0 = forever (the Dancers card's ∞): a dancer stays until cleared or until MaxLive makes way.
    if (d.layer == Layer::Dancer && lp.duration == 0) fade.forever = true;
    return fade;
}

std::string Director::nextId(LayerData& d, engine::SchedRng& rng) {
    if (d.layer != Layer::Dancer) return d.playlist.next(rng);
    // The shuffle never repeats within a round, so one round's worth of draws meets every file.
    const std::size_t tries = d.playlist.items().size() + 1;
    for (std::size_t i = 0; i < tries; ++i) {
        const std::string id = d.playlist.next(rng);
        if (id.empty()) return id;
        const engine::SceneEntry* e = resolve(d, id);
        if (!e || vis_->dancerFileAllowed(e->path)) return id;
    }
    log("Dancer: no character allowed next (reforce.Dancer.RandomStyles is \"none\" and there are no named ones)");
    return "";
}

bool Director::spawn(LayerData& d, const engine::SceneEntry& e, double t, const char* why, bool quick) {
    const FadeSpec fade = spawnFade(d, quick);
    if (d.layer == Layer::Particle) {
        auto cfg = vis_->compileFile(e.path, SceneType::Particle);
        if (!cfg) return false;
        vis_->startParticle(std::move(cfg), e.name, t, fade);
    } else if (d.layer == Layer::Object) {
        const SceneType type = entryType(d, e);
        auto cfg = vis_->compileFile(e.path, type);
        if (!cfg) return false;
        if (type == SceneType::Solid ? !vis_->startSolid(std::move(cfg), e.name, t, fade)
                                     : !vis_->startOrganic(std::move(cfg), e.name, t, fade))
            return false;
    } else if (d.layer == Layer::Dancer) {
        auto cfg = vis_->compileFile(e.path, SceneType::Dancer);
        if (!cfg || !vis_->startDancer(std::move(cfg), e.name, t, fade)) return false;
    } else {
        if (!vis_->startSprite(e.path, e.name, t, fade)) return false;
    }
    d.playlist.noteCurrent(e.id);
    d.current = e.id;
    if (d.layer == Layer::Sprite) spritePrefetchDue_ = true;
    char buf[96];
    std::snprintf(buf, sizeof buf, " (%s, fade %.2f s)", why, fade.fade);
    log(std::string(engine::layerName(d.layer)) + " + \"" + e.id + "\"" + buf);
    return true;
}

bool Director::switchTo(LayerData& d, const engine::SceneEntry& e, bool quick, double t, const char* why,
                        double seconds) {
    if (engine::isSpawnLayer(d.layer)) return spawn(d, e, t, why, quick);
    TransitionSpec spec;
    spec.fn = d.fn;
    const bool beatLength = linkOn_ && prefs_.linkTransitionBeats > 0 && linkBpm_ > 0;
    spec.duration = seconds >= 0 ? seconds
                    : quick      ? d.slideshow.quickDuration()
                    : beatLength ? sync::beatsToSeconds(prefs_.linkTransitionBeats, linkBpm_)
                                 : d.slideshow.sampleTransitionDuration(rng_);
    switch (d.layer) {
        case Layer::WaveShape: {
            auto cfg = vis_->compileFile(e.path, SceneType::WaveShape);
            if (!cfg) return false;
            vis_->switchWaveShape(std::move(cfg), e.name, t, spec);
            break;
        }
        case Layer::ColorMap:
            if (!vis_->switchColorMapFile(e.path, e.name, t, spec)) return false;
            break;
        case Layer::FlowField: {
            std::shared_ptr<const scene::CompiledConfig> cfg;
            if (vis_->preloadLabel() != e.id) {
                cfg = vis_->compileFile(e.path, SceneType::FlowField);
                if (!cfg) return false;
            } else {
                cfg = std::make_shared<scene::CompiledConfig>();  // preloaded: config not rebuilt
            }
            std::shared_ptr<const scene::CompiledConfig> unionCfg;
            std::string unionName;
            LayerData& u = layers_[3];
            if (unionDue_ && spec.duration > 0) {
                const engine::SceneEntry* ue = forcedUnion_;
                if (!ue) {
                    const std::string id = u.playlist.next(rng_);
                    ue = resolve(u, id);
                }
                if (ue) {
                    unionCfg = vis_->compileFile(ue->path, SceneType::FlowFieldUnion);
                    unionName = ue->name;
                    u.playlist.noteCurrent(ue->id);
                    u.current = ue->id;
                }
                unionDue_ = false;
                forcedUnion_ = nullptr;
                unionBack_.clear();
                u.slideshow.disarm();  // re-armed from now
            }
            vis_->switchFlowField(std::move(cfg), e.id, t, spec, std::move(unionCfg), unionName);
            preloadId_.clear();
            break;
        }
        default: return false;
    }
    d.playlist.noteCurrent(e.id);
    d.current = e.id;
    d.slideshow.disarm();  // re-armed when the transition ends
    char buf[96];
    if (linkOn_)
        std::snprintf(buf, sizeof buf, " (%s, %.2f s, Link beat %.2f)", why, spec.duration, linkBeat_);
    else
        std::snprintf(buf, sizeof buf, " (%s, %.2f s)", why, spec.duration);
    log(std::string(engine::layerName(d.layer)) + " -> \"" + e.id + "\"" + buf);
    return true;
}

bool Director::switchToTarget(LayerData& d, engine::SwitchTarget target, const std::string& name, bool quick,
                              double t, double seconds) {
    if (d.layer == Layer::Union) {
        // A Union is used by the next FlowField transition. Prev/Next pick one
        // now (so the control panel can show it): Next draws the shuffle, Prev
        // steps back through the picks made by Next, then through the Unions used.
        const engine::SceneEntry* pick = nullptr;
        if (target == engine::SwitchTarget::Name) {
            pick = resolve(d, name);
            if (!pick) {
                log("Union \"" + name + "\" not found");
                return false;
            }
        } else if (target == engine::SwitchTarget::Previous) {
            while (!pick && !unionBack_.empty()) {
                pick = resolve(d, unionBack_.back());
                unionBack_.pop_back();
            }
            if (!pick) pick = resolve(d, d.playlist.previous());
        } else {
            pick = resolve(d, d.playlist.next(rng_));
        }
        if (!pick) {
            log("Union: none to pick");
            return false;
        }
        if (forcedUnion_ && forcedUnion_ != pick && target != engine::SwitchTarget::Previous) {
            unionBack_.push_back(forcedUnion_->id);
            if (unionBack_.size() > 64) unionBack_.erase(unionBack_.begin());
        }
        forcedUnion_ = pick;
        unionDue_ = true;
        log("next FlowField transition uses Union \"" + forcedUnion_->id + "\"");
        return true;
    }
    std::string id;
    switch (target) {
        case engine::SwitchTarget::Name: {
            const engine::SceneEntry* e = resolve(d, name);
            if (!e) {
                log(std::string(engine::layerName(d.layer)) + " \"" + name + "\" not found");
                return false;
            }
            if (hiddenSprite(d, *e)) {
                log("Sprite \"" + e->id + "\" is hidden (control panel: Image sprites, Review images)");
                return false;
            }
            return switchTo(d, *e, quick, t,
                            seconds >= 0 ? "manual, explicit duration"
                                         : (quick ? "manual" : "manual, slideshow duration"),
                            seconds);
        }
        case engine::SwitchTarget::Next: id = nextId(d, rng_); break;
        case engine::SwitchTarget::Previous: id = d.playlist.previous(); break;
    }
    const engine::SceneEntry* e = resolve(d, id);
    if (!e) {
        log(std::string(engine::layerName(d.layer)) + ": nothing to switch to");
        return false;
    }
    return switchTo(d, *e, quick, t, target == engine::SwitchTarget::Next ? "next" : "previous");
}

bool Director::start() {
    bool ok = true;
    const std::pair<Layer, std::string> starts[] = {
        {Layer::FlowField, opts_.flowField}, {Layer::WaveShape, opts_.waveShape}, {Layer::ColorMap, opts_.colorMap}};
    std::error_code ec;
    for (const auto& [layer, name] : starts) {
        LayerData& d = *data(layer);
        if (name == "builtin") {
            if (layer == Layer::FlowField) vis_->loadFlowFieldText(kBuiltinFlowField, "builtin flowfield");
            else if (layer == Layer::WaveShape) vis_->loadWaveShapeText(kBuiltinWaveShape, "builtin waveshape");
            else vis_->loadColorMapText(kBuiltinColorMap, "builtin colormap");
            continue;
        }
        const engine::SceneEntry* e = nullptr;
        std::optional<fs::path> path;
        std::string label;
        if (!name.empty()) {
            e = resolve(d, name);
            if (!e) {
                if (fs::is_regular_file(name, ec)) {
                    path = fs::path(name);
                    label = path->stem().string();
                } else {
                    rlog("[reforce] %s \"%s\" not found%s%s\n",
                                 std::string(scene::sceneTypeName(d.type)).c_str(), name.c_str(),
                                 opts_.scenesDir.empty() ? "" : " under ", opts_.scenesDir.string().c_str());
                    ok = false;
                    continue;
                }
            }
        } else if (d.slideshow.enabled() && !d.playlist.empty()) {
            e = resolve(d, d.playlist.next(rng_));
        } else if (!d.catalog.empty()) {
            e = resolve(d, engine::defaultReferenceScene(d.type));
        }
        if (e) {
            path = e->path;
            label = layer == Layer::FlowField ? e->id : e->name;
            d.playlist.noteCurrent(e->id);
            d.current = e->id;
        }
        bool loaded = false;
        if (path) {
            if (layer == Layer::FlowField) {
                auto cfg = vis_->compileFile(*path, SceneType::FlowField);
                if (cfg) {
                    vis_->switchFlowField(std::move(cfg), label, 0, {});
                    loaded = true;
                }
            } else if (layer == Layer::WaveShape) {
                auto cfg = vis_->compileFile(*path, SceneType::WaveShape);
                if (cfg) {
                    vis_->switchWaveShape(std::move(cfg), label, 0, {});
                    loaded = true;
                }
            } else {
                loaded = vis_->switchColorMapFile(*path, label, 0, {});
            }
            if (loaded) log(std::string(engine::layerName(layer)) + " starts with \"" + label + "\"");
        }
        if (!loaded) {
            if (layer == Layer::FlowField) vis_->loadFlowFieldText(kBuiltinFlowField, "builtin flowfield");
            else if (layer == Layer::WaveShape) vis_->loadWaveShapeText(kBuiltinWaveShape, "builtin waveshape");
            else vis_->loadColorMapText(kBuiltinColorMap, "builtin colormap");
        }
    }
    LayerData& u = layers_[3];
    if (u.slideshow.enabled() && !u.playlist.empty()) u.slideshow.arm(0, rng_);
    if (!prefs_.videoStartupSource.empty())
        commands_.push(engine::Command::videoStart(engine::VideoTarget::Layer, prefs_.videoStartupSource));
    if (!prefs_.backdropSource.empty()) commands_.push(engine::Command::videoSet("backdrop", prefs_.backdropSource));
    publish(0);
    return ok;
}

std::optional<video::SourceSpec> Director::resolveVideo(const std::string& text, std::string* error) const {
    std::string err;
    auto spec = video::parseSourceSpec(text, &err);
    if (!spec) {
        if (error) *error = err;
        return std::nullopt;
    }
    if (spec->kind != video::SourceKind::File) return spec;
    std::error_code ec;
    if (fs::is_regular_file(spec->id, ec)) return spec;
    // A name from the video library.
    const std::string want = looseName(spec->id);
    for (const auto& e : videoFiles_)
        if (looseName(e.id) == want || looseName(e.name) == want ||
            looseName(e.path.filename().string()) == want) {
            spec->id = e.path.string();
            return spec;
        }
    if (error) *error = "no video \"" + spec->id + "\" (not a file, not in the video library)";
    return std::nullopt;
}

void Director::handleVideo(const engine::Command& c, double t) {
    switch (c.type) {
        case engine::CommandType::VideoStart: {
            std::string err;
            auto spec = resolveVideo(c.text, &err);
            if (!spec) {
                log("video: " + err);
                return;
            }
            if (c.videoTarget == engine::VideoTarget::Layer) {
                vis_->playVideoLayer(*spec, t);
                return;
            }
            LayerData& d = layers_[5];
            FadeSpec fade = spawnFade(d, true);
            const std::string label = video::specLabel(*spec);
            if (spec->kind == video::SourceKind::File) {
                // A movie sprite, exactly like one from the Sprite package.
                if (!vis_->startSprite(spec->id, label, t, fade)) log("video sprite: cannot start " + label);
                return;
            }
            // Live sprite: the Sprite package's default params; shown until stopped.
            fade.defaultHold = 24.0 * 3600.0;
            fs::path paramsFor = label;
            if (auto root = engine::packagesRoot(opts_.scenesDir); root && !opts_.scenesDir.empty())
                paramsFor = *root / engine::packageDirName(SceneType::Sprite) / label;
            if (!vis_->startVideoSprite(*spec, label, t, fade, paramsFor)) log("video sprite: cannot start " + label);
            return;
        }
        case engine::CommandType::VideoStop:
            if (c.videoTarget == engine::VideoTarget::Layer) vis_->stopVideoLayer(t);
            else vis_->stopVideoSprites(t, layers_[5].slideshow.quickDuration());
            return;
        case engine::CommandType::VideoHold:
            if (vis_->holdVideoSprites() == 0) log("video: no movie sprite to loop");
            return;
        case engine::CommandType::VideoSet: {
            std::string key;
            for (char ch : c.text) key.push_back(char(std::tolower(static_cast<unsigned char>(ch))));
            if (key == "backdrop") {  // camera backdrop: a source spec (resolved like video starts) or off
                std::string v;
                for (char ch : c.arg) v.push_back(char(std::tolower(static_cast<unsigned char>(ch))));
                if (v.empty() || v == "off" || v == "0" || v == "none" || v == "false" || v == "no") {
                    vis_->setBackdrop(std::nullopt);
                    return;
                }
                std::string err;
                auto spec = resolveVideo(c.arg, &err);
                if (!spec) {
                    log("video: backdrop: " + err);
                    return;
                }
                vis_->setBackdrop(*spec);
                return;
            }
            const std::string err = vis_->setVideoParam(c.text, c.arg);
            if (!err.empty()) log("video: " + err);
            return;
        }
        default: return;
    }
}

void Director::handle(const engine::Command& c, double t) {
    perf::tag(perf::TagCommand);
    publishDue_ = true;
    if (c.type == engine::CommandType::VideoStart || c.type == engine::CommandType::VideoStop ||
        c.type == engine::CommandType::VideoSet || c.type == engine::CommandType::VideoHold) {
        log("command: " + engine::describe(c));
        handleVideo(c, t);
        return;
    }
    if (c.type == engine::CommandType::BeatSet) {  // rhythm (PLAN 5O)
        log("command: " + engine::describe(c));
        if (const std::string e = engine::validateBeatSetting(c.text, c.arg); !e.empty()) {
            log("rhythm: " + e);
            return;
        }
        if (c.text == "quantize") prefs_.linkQuantize = c.arg;
        else if (c.text == "quantizemanual") prefs_.linkQuantizeManual = c.arg;
        else if (c.text == "quantizespawn") prefs_.spawnQuantize = c.arg;
        else if (c.text == "dropswitch") prefs_.dropSwitch = c.arg == "on" || c.arg == "1";
        else if (const std::string e = vis_->setBeatParam(c.text, c.arg); !e.empty()) log("rhythm: " + e);
        return;
    }
    if (c.type == engine::CommandType::SetSpriteHidden) {
        spriteHidden_ = engine::parseIdList(c.text);
        log("hidden sprites: " + std::to_string(spriteHidden_.size()));
        applyTheme(theme_);  // the shuffles without them
        // Hidden ones on screen (or still decoding) fade out.
        const LayerData& d = layers_[5];
        std::vector<std::string> names;
        for (const auto& e : d.catalog)
            if (hiddenSprite(d, e)) names.push_back(e.name);
        if (!names.empty()) vis_->fadeOutSprites(t, d.slideshow.quickDuration(), names);
        return;
    }
    if (c.type == engine::CommandType::Look) {  // rendering-quality A/B (engine/Look.hpp)
        log("command: " + engine::describe(c) + vis_->handleLook(c));
        return;
    }
    std::vector<LayerData*> targets;
    if (c.allLayers) {
        for (auto& d : layers_) targets.push_back(&d);
    } else if (LayerData* d = data(c.layer)) {
        targets.push_back(d);
    } else if (c.type != engine::CommandType::SetTheme) {
        log(std::string(engine::layerName(c.layer)) + ": unknown layer; ignored " + engine::describe(c));
        return;
    }
    log("command: " + engine::describe(c));
    switch (c.type) {
        case engine::CommandType::Switch:
            for (LayerData* d : targets) {
                // Movie sprites card "Start next": the movie shuffle, not the image one.
                if (d->layer == Layer::Sprite && c.target == engine::SwitchTarget::Next && c.arg == "movie") {
                    const engine::SceneEntry* e = resolve(*d, moviePlaylist_.next(spawnRng_));
                    if (e) spawn(*d, *e, t, "next movie", c.quick);
                    else log("Sprite: no movie sprites in the slideshow");
                    continue;
                }
                switchToTarget(*d, c.target, c.text, c.quick, t, c.seconds);
            }
            break;
        case engine::CommandType::Hold:
        case engine::CommandType::ToggleHold: {
            bool on = c.flag;
            if (c.type == engine::CommandType::ToggleHold) {
                // Toggle-all: hold everything unless everything is already held.
                bool allHeld = true;
                for (LayerData* d : targets) allHeld = allHeld && d->slideshow.held();
                on = !allHeld;
            }
            for (LayerData* d : targets) {
                const bool was = d->slideshow.held();
                d->slideshow.setHeld(on);
                if (was && !on && d->slideshow.armed()) d->slideshow.arm(t, rng_);  // fresh interval
            }
            log(std::string(on ? "hold " : "unhold ") + (c.allLayers ? "all layers" : engine::layerName(c.layer)));
            break;
        }
        case engine::CommandType::SetSlideshowEnabled:
            for (LayerData* d : targets) {
                d->slideshow.setEnabled(c.flag);
                if (c.flag && d->slideshow.armed()) d->slideshow.arm(t, rng_);
            }
            break;
        case engine::CommandType::SetSlideshowInterval:
            for (LayerData* d : targets) {
                const bool spawner = engine::isSpawnLayer(d->layer);
                if (!(c.value > 0) && !(spawner && c.value == 0)) continue;  // 0 = none: spawn timers only
                if (d->layer == Layer::Object) objectInterval_ = c.value;
                if (d->layer == Layer::Dancer) dancerInterval_ = c.value;
                d->slideshow.setInterval(c.value);
                if (d->slideshow.armed()) d->slideshow.arm(t, spawner ? spawnRng_ : rng_);
            }
            break;
        case engine::CommandType::SetSpawnDuration:
            if (c.value > 0) {
                if (c.layer == Layer::Particle) prefs_.particle.duration = c.value;
                else if (c.layer == Layer::Sprite) prefs_.sprite.duration = c.value;
                else if (c.layer == Layer::Object) prefs_.object.duration = c.value;
                else if (c.layer == Layer::Dancer) prefs_.dancer.duration = c.value;
            } else if (c.value == 0 && c.layer == Layer::Dancer) {
                prefs_.dancer.duration = 0;  // forever
            }
            break;
        case engine::CommandType::SetMovieInterval:
            if (c.value >= 0) {
                movieTimer_.setInterval(c.value);
                if (movieTimer_.armed()) movieTimer_.arm(t, spawnRng_);
            }
            break;
        case engine::CommandType::SetSpriteMix: {
            const bool movie = c.text == "movie";
            vis_->setSpriteMix(movie, c.flag ? vis_->spriteMix(movie) + c.value : c.value);
            char buf[64];
            std::snprintf(buf, sizeof buf, "%s sprite mix %.2f", movie ? "movie" : "image", vis_->spriteMix(movie));
            log(buf);
            break;
        }
        case engine::CommandType::SetTransitionDuration:
            for (LayerData* d : targets) d->slideshow.setTransitionDuration(c.value);
            break;
        case engine::CommandType::SetBlendMode:
            for (LayerData* d : targets) {
                if (auto m = engine::findBlendMode(d->layer, c.text)) vis_->setBlendMode(d->layer, *m);
                else log(std::string(engine::layerName(d->layer)) + ": unknown blend mode \"" + c.text + "\"");
            }
            break;
        case engine::CommandType::SetTheme: applyTheme(c.text); break;
        case engine::CommandType::Kill:
            for (LayerData* d : targets) {
                // Fade out over Transition.Duration.Quick instead of vanishing.
                if (d->layer == Layer::Particle) vis_->killParticles(t, d->slideshow.quickDuration());
                else if (d->layer == Layer::Sprite)
                    vis_->killSprites(t, d->slideshow.quickDuration(),
                                      c.text == "images"   ? Visualizer::SpriteKind::Images
                                      : c.text == "movies" ? Visualizer::SpriteKind::Movies
                                                           : Visualizer::SpriteKind::All);
                else if (d->layer == Layer::Object) vis_->killObjects(t, d->slideshow.quickDuration());
                else if (d->layer == Layer::Dancer) vis_->killDancers(t, d->slideshow.quickDuration());
                else if (d->layer == Layer::Union) clearUnion(t);
                else log(std::string(engine::layerName(d->layer)) +
                         ": kill applies to particles, sprites, objects, dancers and unions");
            }
            break;
        case engine::CommandType::SetCrossfader:
            for (LayerData* d : targets) {
                if (engine::isSpawnLayer(d->layer)) continue;
                if (!vis_->setCrossfader(d->layer, c.value, t)) {
                    // Nothing morphing: start a move to the next config, held at the fader.
                    if (switchToTarget(*d, engine::SwitchTarget::Next, {}, false, t))
                        vis_->setCrossfader(d->layer, c.value, t);
                }
            }
            break;
        case engine::CommandType::ReleaseCrossfader:
            for (LayerData* d : targets) vis_->setCrossfader(d->layer, std::nullopt, t);
            break;
        case engine::CommandType::VideoStart:
        case engine::CommandType::VideoStop:
        case engine::CommandType::VideoSet:
        case engine::CommandType::VideoHold: break;  // handleVideo
        case engine::CommandType::Look: break;     // handled above
        case engine::CommandType::BeatSet: break;  // handled above
        case engine::CommandType::SetSpriteHidden: break;  // handled above
    }
}

void Director::preloadNextField(double t) {
    LayerData& d = layers_[2];
    if (!d.slideshow.running() || d.playlist.empty()) return;
    const std::string id = d.playlist.peek(rng_);
    if (id.empty() || id == preloadId_) return;
    const engine::SceneEntry* e = resolve(d, id);
    if (!e) return;
    auto cfg = vis_->compileFile(e->path, SceneType::FlowField);
    if (!cfg) return;
    vis_->preloadFlowField(std::move(cfg), e->id, t);
    preloadId_ = id;
}

void Director::prefetchNextSprite() {
    // Decode the next slideshow sprite ahead of time (like the FlowField
    // preload), so it can start on time. Peeks with copies: the real shuffle
    // and RNG sequence stay as they are (a reshuffle may still miss).
    LayerData& d = layers_[5];
    if (!spritePrefetchDue_ || d.playlist.empty() || !d.slideshow.running()) return;
    spritePrefetchDue_ = false;
    engine::Playlist copy = d.playlist;
    engine::SchedRng rng = spawnRng_;
    if (const engine::SceneEntry* e = resolve(d, copy.peek(rng))) vis_->prefetchSprite(e->path);
}

void Director::runSlideshow(double t) {
    for (int i = 0; i < 3; ++i) {
        LayerData& d = layers_[std::size_t(i)];
        if (d.playlist.empty()) continue;
        if (vis_->busy(d.layer)) {
            d.slideshow.disarm();
            continue;
        }
        if (!d.slideshow.armed()) {
            // Interval counts from the end of the previous change.
            d.slideshow.arm(t, rng_);
            if (d.layer == Layer::FlowField) preloadNextField(t);
            else prefetchNext(d, rng_);
        }
        // With a Link session, a due change waits for the next bar (or beat) line.
        if (d.slideshow.due(t) && linkAllows(prefs_.linkQuantize)) {
            const std::string id = d.playlist.next(rng_);
            if (const engine::SceneEntry* e = resolve(d, id)) switchTo(d, *e, false, t, "slideshow");
        }
    }
    // Particles / Sprites / Solids / Organics / Dancers: spawn on their own timers (spawn to spawn).
    // reforce.Slideshow.OneFeature: solids, organics and movie sprites take turns. While one is at full strength the
    // slideshow starts none of them (their timers still arm); when it begins fading out, the one due longest goes.
    const bool oneFeature = prefs_.slideshowOneFeature;
    const bool featureUp = oneFeature && vis_->featureActive(t);
    const engine::LayerSlideshow* firstDue = nullptr;
    if (oneFeature && !featureUp) {
        auto consider = [&](const engine::LayerSlideshow& s, bool hasItems) {
            if (hasItems && s.interval() > 0 && s.due(t) && (!firstDue || s.dueAt() < firstDue->dueAt())) firstDue = &s;
        };
        for (const LayerData& d : layers_)
            if (d.layer == Layer::Object) consider(d.slideshow, !d.playlist.empty());
        if (!vis_->videoSpritesHeld()) consider(movieTimer_, !moviePlaylist_.empty());
    }
    // May this feature timer be polled now? (An unarmed one may always arm.)
    auto featureMay = [&](const engine::LayerSlideshow& s) {
        return !oneFeature || !s.armed() || (!featureUp && &s == firstDue);
    };
    for (int i = 4; i < 8; ++i) {
        LayerData& d = layers_[std::size_t(i)];
        // A movie sprite looping until stopped keeps the screen: no new sprites meanwhile.
        if (d.layer == Layer::Sprite && vis_->videoSpritesHeld()) continue;
        // reforce.Quantize.Spawn: a due spawn waits for the next beat / bar / phrase line.
        const bool waits = d.slideshow.armed() && d.slideshow.due(t) && !linkAllows(prefs_.spawnQuantize);
        // Solids / organics: a due start also waits while MaxLive are at full strength (until one starts fading out).
        const bool feature = d.layer == Layer::Object;
        // An object with SBar starts from the slideshow only on its line (every SBar bars of the phrase clock); the
        // next one in the shuffle is peeked (copies), so the shuffle itself is unchanged.
        bool offLine = false;
        if (feature && phraseOn_ && d.slideshow.armed() && d.slideshow.due(t) && !d.playlist.empty()) {
            engine::Playlist copy = d.playlist;
            engine::SchedRng r = spawnRng_;
            if (const engine::SceneEntry* e = resolve(d, copy.peek(r)))
                if (const double sb = startBars(d, *e); sb > 0) offLine = !sync::crossedBoundary(barPrev_, barCur_, sb);
        }
        if (!d.playlist.empty() && !waits && !offLine && vis_->spawnRoom(d.layer, t) && (!feature || featureMay(d.slideshow)) &&
            engine::pollSpawn(d.slideshow, t, spawnRng_)) {
            const std::string id = nextId(d, spawnRng_);
            if (const engine::SceneEntry* e = resolve(d, id)) spawn(d, *e, t, "slideshow", false);
        }
        if (d.layer == Layer::Sprite && !moviePlaylist_.empty()) {
            movieTimer_.setEnabled(d.slideshow.enabled());
            movieTimer_.setHeld(d.slideshow.held());
            // A due movie waits while reforce.MovieSprite.MaxLive movies are at full strength.
            if (vis_->movieRoom(t) && featureMay(movieTimer_) && engine::pollSpawn(movieTimer_, t, spawnRng_)) {
                const std::string id = moviePlaylist_.next(spawnRng_);
                if (const engine::SceneEntry* e = resolve(d, id)) spawn(d, *e, t, "slideshow movie", false);
            }
        }
    }
    prefetchNextSprite();
    // Union timer: when it elapses, the next FlowField transition takes a Union.
    LayerData& u = layers_[3];
    if (!u.playlist.empty() && u.slideshow.running() && !unionDue_) {
        if (!u.slideshow.armed()) u.slideshow.arm(t, rng_);
        if (u.slideshow.due(t)) {
            unionDue_ = true;
            if (!forcedUnion_) forcedUnion_ = resolve(u, u.playlist.next(rng_));  // named now: the panel shows it
            log("Union interval elapsed: the next FlowField transition uses Union" +
                (forcedUnion_ ? " \"" + forcedUnion_->id + "\"" : std::string()));
        }
    }
}

void Director::clearUnion(double t) {
    // reforce extension (control panel): no Union pending, and none shaping the field.
    const bool wasDue = unionDue_;
    unionDue_ = false;
    forcedUnion_ = nullptr;
    unionBack_.clear();
    layers_[3].slideshow.disarm();  // the Union timer starts a fresh interval
    const std::string shaping = vis_->status(Layer::FlowField, t).unionName;
    if (!shaping.empty()) {
        // Mid-transition or held after it (probe #45): morph from that blend to the
        // plain FlowField over the quick duration (an ordinary transition, no Union).
        LayerData& ff = layers_[2];
        if (const engine::SceneEntry* e = resolve(ff, ff.current)) {
            log("Union \"" + shaping + "\" cleared: morphing to the plain FlowField");
            switchTo(ff, *e, true, t, "union cleared");
            return;
        }
    }
    log(wasDue ? "Union cleared: the next FlowField transition is a plain one" : "Union: nothing to clear");
}

void Director::updateLink(const audio::AudioFrame* audio) {
    const bool on = audio && audio->linkOn;
    if (on) {
        linkPrevBeat_ = linkPrevValid_ ? linkBeat_ : audio->linkBeat;
        linkBeat_ = audio->linkBeat;
        linkBpm_ = audio->linkBpm;
        linkQuantum_ = audio->linkQuantum;
    }
    linkPrevValid_ = on;
    if (on != linkOn_) log(on ? "Ableton Link session joined" : "Ableton Link session left");
    linkOn_ = on;
}

bool Director::linkAllows(const std::string& mode) const {
    // Phrase lines come from the phrase clock (any beat clock source); without one, nothing waits.
    if (mode == "phrase") return !phraseOn_ || sync::crossedBoundary(phrasePrev_, phraseCur_, 1.0);
    const double unit = mode == "bar" ? linkQuantum_ : mode == "beat" ? 1.0 : 0.0;
    if (!linkOn_ || unit <= 0) return true;
    return sync::crossedBoundary(linkPrevBeat_, linkBeat_, unit);
}

void Director::route(const engine::Command& c, double t) {
    if (c.type == engine::CommandType::Switch && !c.allLayers &&
        (c.layer == Layer::FlowField || c.layer == Layer::Union))
        deferred_.push_back(c);
    else
        handle(c, t);
}

void Director::step(MTL::CommandBuffer* cmd, double t, const audio::AudioFrame* audio) {
    lastT_ = t;
    updateLink(audio);
    {
        // The phrase clock as of the last step (PLAN 5O.1).
        const sync::PhraseState& ps = vis_->phraseState();
        phrasePrev_ = phraseOn_ && ps.on ? phraseCur_ : ps.phrase;
        phraseCur_ = ps.phrase;
        barPrev_ = phraseOn_ && ps.on ? barCur_ : ps.bar;
        barCur_ = ps.bar;
        phraseOn_ = ps.on;
    }
    // Drops (PLAN 5O.5): cut FlowField, WaveShape and ColorMap to their next configs.
    if (vis_->takeDrop() && prefs_.dropSwitch) {
        log("drop: switching FlowField, WaveShape and ColorMap");
        for (LayerData& d : layers_) {
            if (d.layer != Layer::FlowField && d.layer != Layer::WaveShape && d.layer != Layer::ColorMap) continue;
            if (d.playlist.empty()) continue;
            if (const engine::SceneEntry* e = resolve(d, d.playlist.next(rng_))) switchTo(d, *e, true, t, "drop", 0.4);
        }
    }
    std::vector<engine::Command> due;
    due.swap(deferred_);
    for (const engine::Command& c : due) handle(c, t);
    // Manual switches held for the beat/bar line (or released when the session ends).
    const bool manualNow = linkAllows(prefs_.linkQuantizeManual);
    if (!linkHeld_.empty() && manualNow) {
        std::vector<engine::Command> held;
        held.swap(linkHeld_);
        for (const engine::Command& c : held) route(c, t);
    }
    for (const engine::Command& c : commands_.drain()) {
        if (c.type == engine::CommandType::Switch && !manualNow) linkHeld_.push_back(c);
        else route(c, t);
    }
    runSlideshow(t);
    vis_->step(cmd, t, audio);
    audioState_.valid = audio != nullptr;
    if (audio) {
        // Peak-hold BEAT between publishes (every ~3rd step) so a pulse is never missed by the UI.
        const bool fresh = steps_ - lastPublish_ <= 1;
        audioState_.level = audio->level;
        audioState_.bass = audio->bass;
        audioState_.energy = audio->energy;
        audioState_.mid = audio->mid;
        audioState_.treble = audio->treble;
        audioState_.beat = fresh ? audio->beat : std::max(audioState_.beat, double(audio->beat));
        audioState_.tempo = audio->tempo;
        audioState_.linkEnabled = audio->linkEnabled;
        audioState_.linkPeers = audio->linkPeers;
        audioState_.linkListening = audio->linkListening;
        audioState_.linkJoining = audio->linkJoining;
        audioState_.linkOn = audio->linkOn;
        audioState_.linkBpm = audio->linkBpm;
        audioState_.linkBeat = audio->linkBeat;
        audioState_.linkPhase = audio->linkPhase;
        audioState_.linkQuantum = audio->linkQuantum;
    }
    ++steps_;
    publish(t);
}

void Director::colorize(MTL::CommandBuffer* cmd, MTL::Texture* output, bool withVideo, bool trackAlpha,
                        bool backdrop, MTL::Texture* visualsCopy, bool overlays) {
    vis_->colorize(cmd, output, lastT_, withVideo, trackAlpha, backdrop, visualsCopy, overlays);
}

void Director::publish(double t) {
    // Phase 5D: at most every 3rd step (~20 Hz; the web UI polls at 50 Hz but
    // shows nothing faster) unless something changed, filled in place, and
    // handed over without waiting for a reader.
    if (!publishDue_ && steps_ - lastPublish_ < 3 && steps_ > 0) return;
    engine::EngineState& s = scratch_;
    s.simTime = t;
    s.simSteps = steps_;
    s.simRate = opts_.simRate;
    s.audio = audioState_;
    s.theme = theme_;
    s.themes.resize(themes_.themes().size());
    for (std::size_t i = 0; i < s.themes.size(); ++i) s.themes[i] = themes_.themes()[i].name;
    s.layers.resize(layers_.size());
    for (std::size_t i = 0; i < layers_.size(); ++i) {
        const LayerData& d = layers_[i];
        engine::LayerState& ls = s.layers[i];
        ls.layer = d.layer;
        const LayerStatus st = d.layer == Layer::Union ? LayerStatus{} : vis_->status(d.layer, t);
        ls.current = d.layer == Layer::Union ? d.current : st.current;
        ls.target = d.layer == Layer::Union ? (forcedUnion_ ? forcedUnion_->id : std::string()) : st.target;
        ls.pending = st.pending;
        ls.transitioning = st.transitioning;
        ls.progress = st.progress;
        ls.weight = st.weight;
        ls.crossfaderHeld = st.crossfaderHeld;
        ls.morphDepth = st.depth;
        ls.slideshowEnabled = d.slideshow.enabled();
        ls.held = d.slideshow.held();
        ls.interval = d.slideshow.interval();
        ls.transitionDuration = d.slideshow.transitionDuration();
        ls.nextSwitchIn = d.slideshow.armed() && d.slideshow.running() ? std::max(0.0, d.slideshow.dueAt() - t) : -1;
        ls.blendModes = engine::blendModeNames(d.layer);
        ls.blendMode = ls.blendModes.empty() ? std::string() : engine::blendModeName(d.layer, vis_->blendMode(d.layer));
        ls.playlistSize = d.playlist.items().size();
        ls.unionName = st.unionName;
        ls.unionDue = d.layer == Layer::FlowField && unionDue_;
        if (engine::isSpawnLayer(d.layer)) ls.spawnDuration = layerPrefs(prefs_, d.layer).duration;
        if (d.layer == Layer::Sprite) {
            ls.movieInterval = movieTimer_.interval();
            ls.spriteMix = vis_->spriteMix(false);
            ls.movieSpriteMix = vis_->spriteMix(true);
            ls.moviePlaylistSize = moviePlaylist_.items().size();
            ls.nextMovieIn = movieTimer_.armed() && movieTimer_.running() && movieTimer_.interval() > 0
                                 ? std::max(0.0, movieTimer_.dueAt() - t) : -1;
        }
        if (d.layer == Layer::Particle) ls.alive = vis_->particleNames();
        else if (d.layer == Layer::Object) ls.alive = vis_->objectNames();
        else if (d.layer == Layer::Dancer) ls.alive = vis_->dancerNames();
        else if (d.layer == Layer::Sprite) {
            // The Sprite layer card shows image sprites; movies are in the video state.
            ls.alive = vis_->spriteNames(Visualizer::SpriteKind::Images);
            ls.current.clear();
            for (const auto& n : ls.alive) ls.current += (ls.current.empty() ? "" : ", ") + n;
        }
        else ls.alive.clear();
    }
    vis_->videoState(s.video, t);
    vis_->lookState(s.look);
    vis_->rhythmState(s.rhythm);
    s.rhythm.quantize = prefs_.linkQuantize;
    s.rhythm.quantizeManual = prefs_.linkQuantizeManual;
    s.rhythm.quantizeSpawn = prefs_.spawnQuantize;
    s.rhythm.dropSwitch = prefs_.dropSwitch;
    s.recentLog.assign(recent_.begin(), recent_.end());
    if (state_.tryPublish(s)) {
        publishDue_ = false;
        lastPublish_ = steps_;
    }
}

}  // namespace reforce::app
