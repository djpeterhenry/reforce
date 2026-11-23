// Visualizer: the frame (step, colorize), scene compiling and prefetch, Look, status, Syphon alpha and lights.
// The layers live in Visualizer<Area>.cpp; helpers shared between them in VisualizerInternal.hpp.
#include "app/Visualizer.hpp"

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <thread>

#include <pthread.h>

#include "audio/Analyzer.hpp"
#include "engine/DanceClips.hpp"
#include "engine/MediaOptions.hpp"
#include "engine/Log.hpp"
#include "engine/Perf.hpp"
#include "lights/LightsOutput.hpp"
#include "render/ImageFiles.hpp"
#include "render/LightsCapture.hpp"
#include "scene/SceneConfig.hpp"
#include "scene/SceneFile.hpp"

#include "app/VisualizerInternal.hpp"

namespace reforce::app {
using engine::Layer;
using namespace visdetail;

// Our own simple configs (scene-format §5), not G-Force content.
const char* const kBuiltinFlowField = R"CFG(Aspc=1 srcR=".985 * r" srcT="theta + .012")CFG";
const char* const kBuiltinWaveShape =
    R"CFG(Aspc=0 ConB=1 X0="2 * s - 1" Y0=".45 * mag( s )" Pen="1" LWdt="1.5")CFG";
const char* const kBuiltinColorMap = R"CFG(H=".62 + .25 * i" S="1 - .6 * i" V="i ^ .8")CFG";

// ---------------------------------------------------------------- setup

Visualizer::Visualizer(MTL::Device* device, const VisualizerOptions& opts)
    : opts_(opts),
      pipeline_(std::make_unique<render::ScenePipeline>(device, opts.width, opts.height, opts.pipeline)),
      mapFn_(opts.prefs.mapFunction),
      cache_(std::make_unique<ConfigCache>(opts.syncBuilds ? 0 : 2)),
      evalPool_(std::make_unique<scene::WorkerPool>(opts.evalThreads)) {
    particles_.setPool(evalPool_.get());
    {
        sync::PhraseConfig pc;
        pc.bars = opts.prefs.phraseBars;
        pc.autoAlign = opts.prefs.phraseAutoAlign;
        phrase_.setConfig(pc);
        deckStartsOn_ = opts.prefs.phraseDeckStarts;
        sync::DeckStartsConfig dc;
        dc.faderUp = opts.prefs.phraseFaderUp;
        deckStarts_.setConfig(dc);
        kickZoom_ = opts.prefs.kickZoom;
        kickRotateDeg_ = opts.prefs.kickRotate;
        kickDecayMs_ = opts.prefs.kickDecayMs;
        nowPlayingOn_ = opts.prefs.nowPlaying;
        nowPlayingSize_ = opts.prefs.nowPlayingSize;
        nowPlayingCorner_ = opts.prefs.nowPlayingCorner;
        npShadow_.strength = opts.prefs.nowPlayingShadow;
        npShadow_.from = opts.prefs.nowPlayingShadowFrom;
        sync::NowPlayingConfig nc;
        nc.switchSeconds = opts.prefs.nowPlayingSwitch;
        nowPlaying_.setConfig(nc);
    }
    {
        const render::ScenePipelineOptions& po = opts.pipeline;
        engine::LookValues cur = engine::lookFromSettings(opts.prefs);
        cur[engine::LookKey::ThinLines] = po.thinLines == render::ThinLines::GForce ? "gforce" : "smooth";
        cur[engine::LookKey::Intensity] = po.format == render::IntensityFormat::Half16 ? "hq" : "gforce";
        static const char* const kRounding[] = {"nearest", "floor", "point", "fixed16", "fixed256"};
        cur[engine::LookKey::FlowRounding] = kRounding[std::clamp(int(po.flowRounding), 0, 4)];
        cur[engine::LookKey::Antialias] = po.antialias ? "on" : "off";
        cur[engine::LookKey::LineEdges] = po.colorEdges ? "color" : "gforce";
        cur[engine::LookKey::Supersample] = opts.supersample > 1 ? "2x" : "1x";
        look_ = engine::LookControl(cur, engine::lookFromSettings(opts.prefs));
        opts_.prefs.flowPrecision = cur[engine::LookKey::FlowPrecision];
    }
    // Live A/B of reforce.Intensity: the other format's pipelines compile on a
    // worker now, so the first switch does not stall a frame.
    if (!opts.syncBuilds) pipeline_->prewarmOtherFormat();
    {
        render::SolidPipelineOptions so;
        // One target more than MaxLive: a replaced solid fades out under the one replacing it.
        so.maxLive = opts.prefs.objectMaxLive + 1;
        so.msaa = opts.prefs.solidMsaa;
        so.tileSize = opts.prefs.solidTileSize;
        try {
            solids_ = std::make_unique<render::SolidPipeline>(*pipeline_, so);
        } catch (const std::exception& e) {
            rlog("[reforce] 3D solids unavailable: %s\n", e.what());
        }
        if (solids_) {
            try {
                models_ = std::make_unique<render::ModelPipeline>(*solids_);
                modelLoader_ = std::make_unique<ModelLoader>(*models_, opts.prefs.modelMaxTexture, opts.prefs.modelCache);
                preloadModels();
            } catch (const std::exception& e) {
                rlog("[reforce] 3D models unavailable: %s\n", e.what());
                models_.reset();
            }
        }
        try {
            render::FieldPipelineOptions fo;
            fo.maxLive = std::max(1, opts.prefs.objectMaxLive) + 1;  // + 1: see the solids
            fields_ = std::make_unique<render::FieldPipeline>(*pipeline_, fo);
        } catch (const std::exception& e) {
            rlog("[reforce] ripples / Julia / Mandelbrot sets unavailable: %s\n", e.what());
        }
        try {
            text_ = std::make_unique<render::TextOverlay>(pipeline_->device());
        } catch (const std::exception& e) {
            rlog("[reforce] now playing text unavailable: %s\n", e.what());
        }
        try {
            tags_ = std::make_unique<render::TextOverlay>(pipeline_->device(), kMaxNameTags);
            bubbles_ = std::make_unique<render::TextOverlay>(pipeline_->device(), kMaxNameTags);
        } catch (const std::exception& e) {
            rlog("[reforce] dancer name tags unavailable: %s\n", e.what());
        }
        try {
            render::DancerPipelineOptions dop;
            dop.msaa = opts.prefs.dancerMsaa;
            dancers_ = std::make_unique<render::DancerPipeline>(pipeline_->device(), opts.width, opts.height, dop);
        } catch (const std::exception& e) {
            rlog("[reforce] dancers unavailable: %s\n", e.what());
        }
        danceFloor_ = opts.prefs.dancerFloor;
        guestsOn_ = opts.prefs.guestsEnabled;
        guestsUrl_ = opts.prefs.guestsUrl;
        guestsKey_ = opts.prefs.guestsKey;
        guestsMinutes_ = opts.prefs.guestsMinutes;
        setGuestsBlocked(opts.prefs.guestsBlocked);
        {
            // Every guest's latest dancer, to bring back by hand later (Dancers card: Past guests). Read even without
            // the network (a test copy can bring them on); written only when a guest checks in.
            const std::string settings = engine::defaultSettingsPath();
            guestBook_ = std::make_unique<web::GuestBook>(settings.substr(0, settings.find_last_of('/') + 1) + "Guests");
        }
        if (opts.network) {
            // Remembers which check-ins were handed over (next to settings.json): a restart never repeats them.
            const std::string settings = engine::defaultSettingsPath();
            guestFeed_ = std::make_unique<web::GuestFeed>(settings.substr(0, settings.find_last_of('/') + 1) +
                                                          "guests-seen.txt");
            guestFeed_->configure(guestsOn_, guestsUrl_, 5.0, guestsKey_);
        }
        danceRandom_ = opts.prefs.dancerRandomStyles;
        danceLighting_ = opts.prefs.dancerLighting;
        danceReflection_ = opts.prefs.dancerReflection;
        danceOutline_ = opts.prefs.dancerOutline;
        danceHeight_ = opts.prefs.dancerHeight;
        danceHuman_ = opts.prefs.dancerHuman;
        danceMocap_ = opts.prefs.dancerMocap;
        danceMaxLive_ = opts.prefs.dancerMaxLive;
        dancerTestsPending_ = opts.dancerTests;
        // Test hook (--solid-test): one target slot each, up to reforce.Object.MaxLive + 1.
        for (const auto& spec : opts.solidTests) {
            if (!solids_ || int(solidTests_.size()) >= solids_->maxLive()) {
                rlog("[reforce] solid test \"%s\" skipped: no free solid slot (reforce.Object.MaxLive + 1)\n",
                     spec.text.c_str());
                continue;
            }
            solidTests_.push_back({spec, int(solidTests_.size())});
        }
    }
    lineStyle_.scale = opts.prefs.lineWidthScale;
    lineStyle_.offset = opts.prefs.lineWidthOffset;
    lineStyle_.autoScale = opts.prefs.autoLineScale;
    lineStyle_.sizeMode = opts.prefs.lineScale == "frame" ? engine::LineScaleMode::Frame : engine::LineScaleMode::GForce;
    spriteLayering_ = engine::parseSpriteLayering(opts.prefs.spriteLayering).value_or(engine::SpriteLayering::GForce);
    spriteTrails_ = std::clamp(opts.prefs.spriteTrails, 0.0, 1.0);
    spriteIgnoreAlpha_ = opts.prefs.spriteAlpha == "ignore";
    movieColorOver_ = opts.prefs.movieSpriteColor == "over";
    syphonAlpha_ = opts.prefs.syphonAlpha;
    syphonPremultiplied_ = opts.prefs.syphonAlphaPremultiplied;
    windowVisualsOnly_ = opts.prefs.windowVisualsOnly;
    lights_ = opts.prefs.lights;
    if (!opts.lightsConfig.empty()) {
        lightsOut_ = std::make_shared<lights::LightsOutput>(opts.lightsConfig);
        applyLightsSettings();
    }
    keySimilarity_ = opts.prefs.syphonKeySimilarity;
    keySmoothness_ = opts.prefs.syphonKeySmoothness;
    bdMirror_ = opts.prefs.backdropMirror;
    bdFit_ = engine::parseVideoFit(opts.prefs.backdropFit).value_or(engine::VideoFit::Cover);
    bdOpacity_ = std::clamp(opts.prefs.backdropOpacity, 0.0, 1.0);
    body_ = opts.prefs.body;
    if (opts.prefs.movieSpriteColor != "over" && opts.prefs.movieSpriteColor != "off")
        rlog("[reforce] reforce.MovieSprite.Color \"%s\": expected off or over (using off)\n",
             opts.prefs.movieSpriteColor.c_str());
    setSpriteMix(false, opts.prefs.spriteMix);
    setSpriteMix(true, opts.prefs.movieSpriteMix);
    if (opts.prefs.spriteAlpha != "ignore" && opts.prefs.spriteAlpha != "black")
        rlog("[reforce] reforce.SpriteAlpha \"%s\": expected black or ignore (using black)\n", opts.prefs.spriteAlpha.c_str());
    if (!mapFn_.error().empty())
        rlog("[reforce] ColorMap.MapFunction \"%s\": %s (using i)\n", mapFn_.source().c_str(),
                     mapFn_.error().c_str());
    // Until a ColorMap loads: grayscale.
    for (int k = 0; k < engine::kPaletteSize; ++k) {
        const float v = float(k) / 255.f;
        palette_[std::size_t(k)] = {v, v, v, 1.f};
    }
    palCur_ = palette_;
    ffCur_ = std::make_shared<GpuField>(GpuField{pipeline_->currentField(), 0, false});
    videoParams_ = opts.prefs.videoLayer;
    {
        std::vector<std::string> errs;
        videoLoopRules_ = engine::parseExcludeList(opts.prefs.videoLoopList, &errs);
        for (const auto& e : errs) rlog("[reforce] reforce.Video.Loop: %s (ignored)\n", e.c_str());
    }
    try {
        videoMips_ = std::make_unique<render::VideoMips>(device);
    } catch (const std::exception& e) {
        rlog("[reforce] video mips unavailable (%s): minified video is sampled bilinearly\n", e.what());
    }
}

Visualizer::~Visualizer() {
    shutdownLights();
    releaseBodyMasks();
    cancelBuild(preload_);
    cancelPending();
    // std::async futures (incl. the retired ones) join in their destructors.
}

scene::Environment Visualizer::environment(double t) const {
    scene::Environment env;
    env.width = opts_.width;
    env.height = opts_.height;
    env.numFftBins = opts_.prefs.fftNumBins;
    env.numSampleBins = opts_.prefs.pcmNumBins;
    env.particleDuration = opts_.prefs.particle.duration > 0 ? opts_.prefs.particle.duration : 25.0;
    env.seed = opts_.seed;
    env.loadTime = t;
    env.flowFloat32 = opts_.prefs.flowPrecision == "float32";
    return env;
}

engine::FlowFieldBuildOptions Visualizer::buildOptions() const {
    engine::FlowFieldBuildOptions bo;
    bo.width = int(opts_.width);
    bo.height = int(opts_.height);
    // Leave two cores for the render and present threads (Phase 5D); the
    // result does not depend on the thread count.
    const int hw = int(std::thread::hardware_concurrency());
    bo.threads = opts_.fieldThreads > 0 ? opts_.fieldThreads : std::max(1, hw - 2);
    return bo;
}

std::shared_ptr<const scene::CompiledConfig> Visualizer::compileText(const std::string& text,
                                                                     const std::string& label, scene::SceneType type) const {
    scene::SceneFile file = scene::parseSceneText(text, label);
    auto cfg = std::make_shared<scene::CompiledConfig>(scene::compileConfig(file, type));
    const auto errors = cfg->diags.errorCount();
    if (errors) {
        rlog("[reforce] %s: %zu error(s), %zu warning(s)\n", label.c_str(), errors,
                     cfg->diags.warningCount());
        for (const auto& d : cfg->diags.items())
            if (d.severity == expr::Severity::Error)
                rlog("  line %d %s: %s\n", d.line, d.key.c_str(), d.message.c_str());
    }
    return cfg;
}

std::shared_ptr<const scene::CompiledConfig> Visualizer::compileFile(const std::filesystem::path& path,
                                                                     scene::SceneType type) {
    const std::string key = "cfg|" + std::to_string(int(type)) + "|" + path.string();
    std::function<std::shared_ptr<const scene::CompiledConfig>()> make =
        [this, path, type]() -> std::shared_ptr<const scene::CompiledConfig> {
        std::string text;
        if (!readFile(path, text)) {
            rlog("[reforce] cannot read %s\n", path.string().c_str());
            return nullptr;
        }
        return compileText(text, path.string(), type);
    };
    return cache_->get<scene::CompiledConfig>(key, make);
}

std::shared_ptr<const engine::Palette> Visualizer::pngPalette(const std::filesystem::path& path) {
    std::function<std::shared_ptr<const engine::Palette>()> make = [path]() -> std::shared_ptr<const engine::Palette> {
        render::ImageRgba8 img;
        if (!render::loadImageRgba8(path.string(), img) || img.width <= 0) return nullptr;
        // N x 1 strip; for taller images use the first row.
        return std::make_shared<engine::Palette>(engine::paletteFromStrip(img.rgba.data(), img.width));
    };
    return cache_->get<engine::Palette>("png|" + path.string(), make);
}

void Visualizer::prefetchConfig(const std::filesystem::path& path, scene::SceneType type, bool front) {
    if (!cache_->background()) return;
    if (type == scene::SceneType::Sprite) {
        cache_->prefetch("sprite|" + path.string(), [this, path]() -> ConfigCache::Value {
            auto p = makeSpriteParams(path);
            return p ? std::make_shared<const SpriteParams>(std::move(*p)) : nullptr;
        }, front);
        return;
    }
    if (type == scene::SceneType::ColorMap && lowerExt(path) == ".png") {
        cache_->prefetch("png|" + path.string(), [path]() -> ConfigCache::Value {
            render::ImageRgba8 img;
            if (!render::loadImageRgba8(path.string(), img) || img.width <= 0) return nullptr;
            return std::make_shared<const engine::Palette>(engine::paletteFromStrip(img.rgba.data(), img.width));
        }, front);
        return;
    }
    const std::string key = "cfg|" + std::to_string(int(type)) + "|" + path.string();
    cache_->prefetch(key, [this, path, type]() -> ConfigCache::Value {
        std::string text;
        if (!readFile(path, text)) {
            rlog("[reforce] cannot read %s\n", path.string().c_str());
            return nullptr;
        }
        return compileText(text, path.string(), type);
    }, front);
}

// ---------------------------------------------------------------- controls / status

bool Visualizer::setCrossfader(Layer layer, std::optional<double> w, double t) {
    engine::TransitionClock* clock = nullptr;
    if (layer == Layer::WaveShape && wsTo_) clock = &wsClock_;
    if (layer == Layer::ColorMap && cmTo_) clock = &cmClock_;
    if (layer == Layer::FlowField) {
        if (ffTrans_) clock = &ffTrans_->clock;
        else if (ffPending_) {
            ffPendingHold_ = w;
            return true;
        }
    }
    if (!clock) return false;
    if (w) clock->hold(*w);
    else clock->release(t);
    return true;
}

bool Visualizer::busy(Layer layer) const {
    switch (layer) {
        case Layer::WaveShape: return wsTo_ != nullptr;
        case Layer::ColorMap: return cmTo_ != nullptr;
        case Layer::FlowField: return ffTrans_.has_value() || ffPending_.has_value();
        default: return false;
    }
}

// ---------------------------------------------------------------- Look

void Visualizer::applyLook(unsigned changed, std::string& note) {
    using engine::LookKey;
    const engine::LookValues& v = look_.current();
    auto has = [&](LookKey k) { return (changed & (1u << int(k))) != 0; };
    if (has(LookKey::ThinLines))
        pipeline_->setThinLines(v[LookKey::ThinLines] == "gforce" ? render::ThinLines::GForce : render::ThinLines::Smooth);
    if (has(LookKey::FlowRounding)) {
        const std::string& r = v[LookKey::FlowRounding];
        pipeline_->setFlowRounding(r == "nearest"    ? render::FlowRounding::Nearest
                                   : r == "point"    ? render::FlowRounding::PointFloor
                                   : r == "floor"    ? render::FlowRounding::Floor
                                   : r == "fixed256" ? render::FlowRounding::Fixed256Floor
                                                     : render::FlowRounding::Fixed16Floor);
    }
    if (has(LookKey::Antialias)) pipeline_->setAntialias(v[LookKey::Antialias] == "on");
    if (has(LookKey::LineScale))
        lineStyle_.sizeMode = v[LookKey::LineScale] == "frame" ? engine::LineScaleMode::Frame : engine::LineScaleMode::GForce;
    if (has(LookKey::SpriteLayering))
        spriteLayering_ = engine::parseSpriteLayering(v[LookKey::SpriteLayering]).value_or(engine::SpriteLayering::GForce);
    if (has(LookKey::Intensity)) {
        perf::tag(perf::TagCommand);
        pipeline_->setIntensityFormat(v[LookKey::Intensity] == "hq" ? render::IntensityFormat::Half16
                                                                    : render::IntensityFormat::GForce8);
    }
    if (has(LookKey::LineEdges)) pipeline_->setColorEdges(v[LookKey::LineEdges] == "color");
    if (has(LookKey::Supersample))
        note = ": Supersample " + v[LookKey::Supersample] + " applies the next time reforce starts (Save as default to keep "
               "it; now running at " + std::string(opts_.supersample > 1 ? "2x" : "1x") + ")";
    if (has(LookKey::FlowPrecision)) {
        opts_.prefs.flowPrecision = v[LookKey::FlowPrecision];
        // A preloaded field was built with the old precision: drop it, so the
        // next switch builds with the new one.
        cancelBuild(preload_);
        preloadLabel_.clear();
        note = ": FlowPrecision applies from the next FlowField build";
    }
}

std::string Visualizer::handleLook(const engine::Command& c) {
    if (auto e = engine::validateLook(c); !e.empty()) return ": " + e;
    std::string note;
    unsigned changed = 0;
    if (c.text == "ab") {
        changed = look_.toggleAB();
        note = std::string(": slot ") + look_.state().slot;
    } else if (c.text == "preset") {
        changed = look_.preset(*engine::lookPreset(c.arg));
    } else if (c.text == "reset") {
        changed = look_.resetToSaved(c.arg.empty() || c.arg == "all" ? std::nullopt : engine::parseLookKey(c.arg));
    } else if (c.text == "save") {
        return ": saving goes through the web UI (POST /api/command on the running app) or --write-setting";
    } else if (c.text == "saved") {
        const auto eq = c.arg.find('=');
        look_.markSaved(*engine::parseLookKey(c.arg.substr(0, eq)), c.arg.substr(eq + 1));
        return {};
    } else {
        const std::string err = look_.set(*engine::parseLookKey(c.text), c.arg, &changed);
        if (!err.empty()) return ": " + err;
    }
    std::string applyNote;
    applyLook(changed, applyNote);
    return note + applyNote;
}

void Visualizer::lookState(engine::LookState& out) const {
    out = look_.state();
    const GpuField* shown = ffTrans_ && ffTrans_->to ? ffTrans_->to.get() : ffCur_.get();
    const bool f32 = shown && shown->buildSeconds > 0 ? shown->float32 : opts_.prefs.flowPrecision == "float32";
    out.fieldPrecision = f32 ? "float32" : "double";
}

LayerStatus Visualizer::status(Layer layer, double t) const {
    LayerStatus s;
    switch (layer) {
        case Layer::WaveShape:
            s.current = wsCur_ ? wsCur_->label() : std::string();
            if (wsTo_) {
                s.target = wsTo_->name;
                s.transitioning = true;
                s.progress = wsClock_.progress(t);
                s.weight = wsClock_.weight(t);
                s.crossfaderHeld = wsClock_.held();
                s.depth = 1 + (wsCur_ ? wsCur_->depth() : 0);
            }
            break;
        case Layer::ColorMap:
            s.current = cmName_;
            if (cmTo_) {
                s.target = cmToName_;
                s.transitioning = true;
                s.progress = cmClock_.progress(t);
                s.weight = cmClock_.weight(t);
                s.crossfaderHeld = cmClock_.held();
            }
            break;
        case Layer::FlowField:
            s.current = ffName_;
            if (ffTrans_) {
                s.target = ffTrans_->toName;
                s.transitioning = true;
                s.progress = ffTrans_->clock.progress(t);
                s.weight = ffTrans_->clock.weight(t);
                s.crossfaderHeld = ffTrans_->clock.held();
                s.unionName = ffTrans_->unionName;
            } else {
                s.unionName = ffHeldUnion_;  // a Union blend held after its transition (probe #45)
            }
            if (ffPending_) {
                s.pending = true;
                if (!ffTrans_) s.target = ffPending_->field.label;
            }
            break;
        case Layer::Particle:
        case Layer::Sprite:
        case Layer::Object: {
            const auto names = layer == Layer::Particle ? particleNames()
                               : layer == Layer::Sprite ? spriteNames()
                                                        : objectNames();
            for (const auto& n : names) s.current += (s.current.empty() ? "" : ", ") + n;
            break;
        }
        default: break;
    }
    return s;
}

// ---------------------------------------------------------------- frame

void Visualizer::seedNoise(MTL::CommandBuffer* cmd) { pipeline_->encodeNoise(cmd, uint32_t(opts_.seed)); }

void Visualizer::step(MTL::CommandBuffer* cmd, double t, const audio::AudioFrame* audio) {
    timings_ = {};
    stepBeatClock(t, audio);  // first: SYNC_* for every config this step, movie beat sync
    stepFlowField(t);
    stepWaveShape(t, audio);
    stepParticles(t, audio);
    stepOrganics(t, audio);
    stepDancers(t, audio);
    stepSprites(t, audio);
    stepFields(cmd);
    stepSolids(cmd, t, audio);
    stepBody(t);  // hand trails join the line segments
    timings_.segments = segments_.size();

    auto t0 = Clock::now();
    stepColorMap(t);
    timings_.colorMs = msSince(t0);

    t0 = Clock::now();
    render::FlowBlend fb;
    if (ffTrans_) {
        fb.active = true;
        fb.weight = float(ffTrans_->clock.weight(t));
        fb.mode = ffTrans_->mode;
        fb.useMask = ffTrans_->mask != nullptr;
    }
    // Minified video sprites: sample a prefiltered, mipmapped copy (no shimmer).
    for (auto& v : videoSpriteDraws_) {
        MTL::Texture* tex = minified(cmd, v.slot, v.frame, v.scale);
        if (tex != v.frame->texture()) pipeline_->setSpriteFrame(v.texture, tex);
    }
    videoSpriteDraws_.clear();  // the frames stay alive in stepFrames_
    if (videoMips_) videoMips_->collect();
    pipeline_->encodeStep(cmd, segments_.data(), segments_.size(), fb, feedbackQuads_.data(), feedbackQuads_.size(),
                          spriteLayering_ != engine::SpriteLayering::Melt, displayQuads_.data(), displayQuads_.size());
    if (tracker_ && body_.burn != engine::BodyBurn::Off && maskCur_ >= 0) {
        engine::UvTransform uv;
        if (backdropUv(uv))
            pipeline_->encodeBodyBurn(cmd, maskTex_[std::size_t(maskCur_)],
                                      maskPrev_ >= 0 ? maskTex_[std::size_t(maskPrev_)] : nullptr, uv, bdMirror_,
                                      int(body_.burn), float(body_.burnAmount));
    }
    holdFrames(cmd, stepFrames_);
    stepVideoLayer(t);
    stepBackdrop(t);
    timings_.encodeMs = msSince(t0);
}

void Visualizer::colorize(MTL::CommandBuffer* cmd, MTL::Texture* output, double t, bool withVideo, bool trackAlpha,
                          bool backdrop, MTL::Texture* visualsCopy, bool overlays) {
    if (!lutValid_ || mapFn_.timeDependent()) {
        mapFn_.evaluate(t, lut_);
        lutValid_ = true;
    }
    render::VideoLayerDraw draw;
    const render::VideoLayerDraw* layer = nullptr;
    if (withVideo && videoLayerActive()) {
        const video::FramePtr& fa = vlFading_ ? vlA_.frame : nullptr;
        const video::FramePtr& fb = vlFading_ ? vlB_.frame : vlA_.frame;
        auto fit = [&](const video::FramePtr& f) {
            return f ? engine::fitTransform(f->width(), f->height(), opts_.width, opts_.height, videoParams_.fit)
                     : engine::UvTransform{};
        };
        draw.uvA = fit(fa);
        draw.uvB = fit(fb);
        // Drawn output pixels per video pixel along the more minified axis.
        auto scale = [&](const video::FramePtr& f, const engine::UvTransform& uv) {
            if (!f || f->width() <= 0 || f->height() <= 0) return 1.0;
            const double sx = double(opts_.width) / (double(f->width()) * std::max(1e-6, double(uv.scale[0])));
            const double sy = double(opts_.height) / (double(f->height()) * std::max(1e-6, double(uv.scale[1])));
            return std::min(sx, sy);
        };
        draw.a = fa ? minified(cmd, vlA_.src.get(), fa, scale(fa, draw.uvA)) : nullptr;
        draw.b = fb ? minified(cmd, vlFading_ ? vlB_.src.get() : vlA_.src.get(), fb, scale(fb, draw.uvB)) : nullptr;
        draw.weight = vlFading_ ? float(vlWeight_) : 1.0f;
        draw.params = videoParams_;
        if (draw.a || draw.b) {
            layer = &draw;
            std::vector<video::FramePtr> keep;
            if (fa) keep.push_back(fa);
            if (fb) keep.push_back(fb);
            holdFrames(cmd, keep);
        }
    }
    if (!colorQuads_.empty() && !colorFrames_.empty()) {
        colorHold_ = colorFrames_;
        holdFrames(cmd, colorHold_);
    }
    pipeline_->encodeColorize(cmd, palette_, lut_, output, layer, colorQuads_.data(), colorQuads_.size(),
                              trackAlpha || backdrop, opaqueSolids_.data(), opaqueSolids_.size(),
                              colorSolids_.data(), colorSolids_.size());
    // Glass puzzles (Glas = 1) over the colorized frame, before anything else goes on top.
    if (solids_)
        for (const auto& g : glassDraws_) solids_->encodeGlass(cmd, output, g, trackAlpha || backdrop);
    // 3D models in color (look B) over the colorized frame, likewise.
    if (models_)
        for (const auto& d : modelColorDraws_) models_->encodeColor(cmd, output, d, trackAlpha || backdrop);
    // The visuals alone (reforce.Window.VisualsOnly): copied before anything goes on top. Its alpha may hold the
    // Syphon key; the window ignores alpha.
    if (visualsCopy) {
        MTL::BlitCommandEncoder* blit = cmd->blitCommandEncoder();
        blit->copyFromTexture(output, visualsCopy);
        blit->endEncoding();
    }
    // Room lights: the visuals alone, from the pass that has the color video layer (docs/lights.md).
    if (withVideo && lightsOut_ && lightsOut_->wantsFrames()) captureLights(cmd, output);
    if (!overlays) return;
    // Camera backdrop (docs/backdrop.md): under the visuals, before the dancers (the person in front of the visuals
    // stays behind the dancers); the frame is opaque from here on.
    if (backdrop) encodeBackdrop(cmd, output);
    // Dancers (PLAN 5P): in full color over the finished frame, lit by it.
    if (dancers_ && (stage_.bodyCount > 0 || stage_.floor > 0.001f) && stage_.opacity > 0.001f)
        dancers_->encode(cmd, output, stage_, trackAlpha && !backdrop);
    encodeNameTags(cmd, output);
    encodeNowPlaying(cmd, output);
}

void Visualizer::encodeSyphonAlpha(MTL::CommandBuffer* cmd, MTL::Texture* frame, MTL::Texture* output) {
    pipeline_->encodeSyphonAlpha(cmd, frame, output, float(keySimilarity_ / 1000.0), float(keySmoothness_ / 1000.0),
                                 syphonPremultiplied_);
}

// ---------------------------------------------------------------- room lights (docs/lights.md)

void Visualizer::applyLightsSettings() {
    if (!lightsOut_) return;
    lights::MapSettings m;
    m.brightness = lights_.brightness;
    m.saturation = lights_.saturation;
    m.floor = lights_.floor;
    m.release = lights_.smoothing;
    m.pulse = lights_.pulse;
    m.pulseLeadMs = lights_.pulseTimingMs;
    lightsOut_->setSettings(m);
    lightsOut_->setEnabled(lights_.enabled);
}

void Visualizer::shutdownLights() {
    if (lightsOut_) lightsOut_->shutdown();
}

void Visualizer::captureLights(MTL::CommandBuffer* cmd, MTL::Texture* frame) {
    // At most 30 captures a second (the panels get 20).
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (lightsLastCapture_ >= 0 && now - lightsLastCapture_ < 1.0 / 30.0) return;
    if (!lightsCap_) {
        std::weak_ptr<lights::LightsOutput> out = lightsOut_;
        perf::countRenderAlloc();  // once, when the lights first stream
        lightsCap_ = std::make_unique<render::LightsCapture>(
            pipeline_->device(), opts_.width, opts_.height, [out](const std::uint8_t* p, int w, int h, int row) {
                if (auto o = out.lock()) o->submit(p, w, h, row);
            });
    }
    if (lightsCap_->encode(cmd, frame)) lightsLastCapture_ = now;
}

}  // namespace reforce::app
