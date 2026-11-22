// Visualizer: image and movie sprites: decoding, prefetch, the slideshow, Mix and levels.
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

// ---------------------------------------------------------------- sprites

std::optional<Visualizer::SpriteParams> Visualizer::spriteParams(const std::filesystem::path& image) {
    std::function<std::shared_ptr<const SpriteParams>()> make = [this, &image]() -> std::shared_ptr<const SpriteParams> {
        auto p = makeSpriteParams(image);
        return p ? std::make_shared<const SpriteParams>(std::move(*p)) : nullptr;
    };
    const auto p = cache_->get<SpriteParams>("sprite|" + image.string(), make);
    if (!p) return std::nullopt;
    return *p;
}

std::optional<Visualizer::SpriteParams> Visualizer::makeSpriteParams(const std::filesystem::path& image) const {
    const engine::SpriteName sn = engine::parseSpriteName(image.stem().string());
    const engine::SpriteParamsText params = engine::spriteParamsFor(image);
    SpriteParams p;
    p.cfg = compileText(params.text, params.origin, scene::SceneType::Sprite);
    if (!p.cfg) return std::nullopt;
    p.origin = params.origin;
    p.nameInvert = sn.invert;
    // Inversion (name '~', Invt, Flag bit) is baked into the decoded luminance.
    const engine::SpriteRun probe(p.cfg, environment(0), {}, sn.invert, 0, 0, 0, nullptr);
    p.invert = probe.invert();
    return p;
}

Visualizer::DecodedSprite::~DecodedSprite() {
    if (texture) texture->release();
}

Visualizer::SpriteImage Visualizer::decodeSprite(const std::filesystem::path& image, bool invert) {
    const std::string key = image.string() + (invert ? "\n~" : "\n");
    if (prefetch_.valid() && key == prefetchKey_) {
        SpriteImage f = std::move(prefetch_);
        prefetch_ = {};
        prefetchKey_.clear();
        return f;
    }
    // A detached worker with a promise: dropping the future (kill) never blocks
    // the render thread the way a std::async future's destructor would.
    std::promise<std::shared_ptr<const DecodedSprite>> promise;
    SpriteImage future = promise.get_future().share();
    // The worker also creates and fills the Metal texture (Phase 5D: no
    // resource creation or upload on the render thread).
    MTL::Device* device = pipeline_->device()->retain();
    std::thread([promise = std::move(promise), path = image.string(), invert, device,
                 ignoreAlpha = spriteIgnoreAlpha_]() mutable {
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
        const auto t0 = Clock::now();
        auto out = std::make_shared<DecodedSprite>();
        render::ImageRgba8 img;
        if (render::loadImageRgba8(path, img, ignoreAlpha) && img.width > 0 && img.height > 0) {
            out->width = img.width;
            out->height = img.height;
            out->alpha = engine::rgbaHasAlpha(img.rgba.data(), img.width, img.height);
            const engine::LumaImage la =
                engine::toLumaAlpha(img.rgba.data(), img.width, img.height, invert, out->alpha);
            out->hasAlpha = la.hasAlpha;
            out->texture = render::ScenePipeline::createSpriteTexture(device, la.la.data(), la.width, la.height);
            out->ok = out->texture != nullptr;
        }
        out->decodeMs = msSince(t0);
        device->release();
        promise.set_value(std::move(out));
    }).detach();
    return future;
}

void Visualizer::prefetchSprite(const std::filesystem::path& image) {
    if (opts_.syncBuilds) return;  // deterministic runs decode in place
    auto params = spriteParams(image);
    if (!params) return;
    if (video::isVideoFile(image)) {
        // Movie sprite: open and preroll the next one (first frames decoded).
        const std::string key = "video\n" + image.string();
        if (key == prefetchKey_) return;
        prefetch_ = {};
        video::SourceSpec spec;
        spec.id = image.string();
        applyMovieOptions(spec, opts_.prefs.videoSpriteLoop);
        prefetchVideo_ = openVideo(spec);
        prefetchKey_ = key;
        return;
    }
    const std::string key = image.string() + (params->invert ? "\n~" : "\n");
    if (key == prefetchKey_) return;
    prefetch_ = {};
    prefetchVideo_.reset();
    prefetch_ = decodeSprite(image, params->invert);
    prefetchKey_ = key;
}

bool Visualizer::startSprite(const std::filesystem::path& image, const std::string& label, double t,
                             const FadeSpec& fade) {
    auto params = spriteParams(image);
    if (!params) return false;
    PendingSprite p;
    if (video::isVideoFile(image)) {
        const std::string key = "video\n" + image.string();
        if (prefetchVideo_ && key == prefetchKey_) {
            p.video = std::move(prefetchVideo_);
            prefetchKey_.clear();
        } else {
            video::SourceSpec spec;
            spec.id = image.string();
            applyMovieOptions(spec, opts_.prefs.videoSpriteLoop);
            p.video = openVideo(spec);
        }
        if (!p.video) return false;
    } else {
        p.image = decodeSprite(image, params->invert);
    }
    p.params = std::move(*params);
    p.path = image;
    p.label = label;
    p.fade = fade;
    p.requested = t;
    p.seed = opts_.seed + 0x1000 * ++loads_;
    spritePending_.push_back(std::move(p));
    if (opts_.syncBuilds) adoptSprites(t);  // deterministic: starts now, as requested
    return true;
}

void Visualizer::adoptSprites(double t) {
    // In request order, so later sprites still draw on top. A live source that
    // is not delivering yet (camera permission, Syphon server not up) waits
    // without holding up the sprites behind it.
    std::vector<PendingSprite> waiting;
    std::size_t n = 0;
    for (; n < spritePending_.size(); ++n) {
        PendingSprite& p = spritePending_[n];
        std::shared_ptr<const DecodedSprite> img;
        int srcW = 0, srcH = 0;
        double prepMs = 0;
        bool alpha = false;
        if (p.video) {
            if (!p.video->ready() && !p.video->failed() && opts_.syncBuilds &&
                p.video->spec().kind == video::SourceKind::File)
                p.video->waitReady(20);
            if (p.video->failed()) {
                videoError("sprite \"" + p.label + "\": " + p.video->error());
                continue;
            }
            if (!p.video->ready()) {
                if (p.video->spec().kind == video::SourceKind::File && !opts_.syncBuilds) break;  // prerolling
                waiting.push_back(std::move(p));
                continue;
            }
            srcW = p.video->width();
            srcH = p.video->height();
            prepMs = (t - p.requested) * 1000.0;
        } else {
            if (opts_.syncBuilds) p.image.wait();
            else if (p.image.wait_for(std::chrono::seconds(0)) != std::future_status::ready) break;
            img = p.image.get();
            if (!img || !img->ok) {
                rlog("[reforce] cannot decode sprite image %s\n", p.path.string().c_str());
                continue;
            }
            srcW = img->width;
            srcH = img->height;
            prepMs = img->decodeMs;
            alpha = img->alpha;
        }
        perf::tag(perf::TagSpriteStart | (p.video ? perf::TagVideoStart : 0u));
        scene::Environment env = environment(t);
        env.seed = p.seed;
        auto run = std::make_unique<engine::SpriteRun>(p.params.cfg, env, p.label, p.params.nameInvert, t,
                                                       p.fade.fade, p.fade.defaultHold, p.fade.fn);
        const int tex = p.video ? pipeline_->addVideoSpriteTexture(p.params.invert, videoLevels(p.video->spec()))
                                : pipeline_->addSpriteTexture(img->texture, img->hasAlpha);
        if (tex < 0) continue;
        if (p.video)
            if (const render::VideoLevels lv = videoLevels(p.video->spec()); !lv.identity())
                rlog("[reforce] Sprite \"%s\": levels black %.3f, white %.3f, gamma %.3f\n", p.label.c_str(), lv.black,
                     lv.white, lv.gamma);
        run->setImageSize(srcW, srcH, double(opts_.width), double(opts_.height));
        // A movie that plays once: the sprite lasts the movie and fades out as it ends.
        if (p.video && p.video->spec().kind == video::SourceKind::File && !p.video->spec().loop) {
            const double len = video::playLength(p.video->spec(), p.video->duration());
            if (len > 0) run->fitLifetime(len);
        }
        // A new EPen-only ("background") sprite fades out the running ones (probe #44);
        // with reforce.SpriteReplace = own only those of its kind (image vs video).
        if (!run->hasPen()) {
            const bool anyKind = opts_.prefs.spriteReplace == "any";
            for (auto& s : sprites_)
                if (!s.held && !s.run->hasPen() && (anyKind || bool(s.video) == bool(p.video)))
                    s.run->fadeOutFrom(t, engine::kSpriteReplaceFade, true);
        }
        const auto& e = run->envelope();
        char wait[48] = "";
        if (t > p.requested) std::snprintf(wait, sizeof wait, ", %.0f ms after the request", (t - p.requested) * 1000.0);
        const std::string kind =
            p.video ? std::string(" (") + video::sourceKindName(p.video->spec().kind) + " video)" : std::string();
        rlog(
                     "[reforce] Sprite \"%s\" starts: %dx%d%s%s -> %.0fx%.0f px, flags %u (%s), %s, %s%s, fade %.2f s, "
                     "hold %.2f s, params %s (%s in %.1f ms%s)\n",
                     p.label.c_str(), srcW, srcH, alpha ? " (alpha)" : "", kind.c_str(), run->drawSize().w,
                     run->drawSize().h, run->flags(), engine::describeSpriteFlags(run->flags()).c_str(),
                     engine::spriteBlendName(engine::spriteBlendMode(run->flags())),
                     run->hasPen() ? "Pen" : "EPen only", run->invert() ? ", inverted" : "", e.fade, e.hold,
                     p.params.origin == "builtin"
                         ? "builtin"
                         : std::filesystem::path(p.params.origin).filename().string().c_str(),
                     p.video ? "first frame" : "decoded", prepMs, wait);
        const bool held = p.held && p.video;
        if (held) {
            p.video->setLoop(true);
            run->holdForever();
        }
        std::unique_ptr<engine::MovieBeatSync> beatSync;
        if (p.video) beatSync = makeBeatSync(*p.video, "Sprite \"" + p.label + "\"");
        sprites_.push_back({std::move(run), tex, std::move(p.video), false, held, std::move(beatSync)});
    }
    std::vector<PendingSprite> rest;
    for (std::size_t i = n; i < spritePending_.size(); ++i) rest.push_back(std::move(spritePending_[i]));
    for (auto& w : waiting) rest.push_back(std::move(w));
    spritePending_ = std::move(rest);
}

void Visualizer::killSprites(double t, double fade, SpriteKind kind) {
    if (kind != SpriteKind::All) {
        const bool movies = kind == SpriteKind::Movies;
        std::size_t n = 0;
        for (auto it = spritePending_.begin(); it != spritePending_.end();)
            if (bool(it->video) == movies) it = spritePending_.erase(it);
            else ++it;
        for (auto& s : sprites_)
            if (bool(s.video) == movies) {
                s.run->fadeOutFrom(t, fade);
                ++n;
            }
        if (n) perf::tag(perf::TagKill);
        rlog("[reforce] %zu %s sprite(s) killed: fading out over %.2f s\n", n, movies ? "video" : "image", fade);
        return;
    }
    perf::tag(perf::TagKill);
    if (!spritePending_.empty())
        rlog("[reforce] %zu sprite(s) still decoding dropped\n", spritePending_.size());
    spritePending_.clear();
    if (sprites_.empty()) return;
    if (fade <= 0) {
        rlog("[reforce] %zu sprite(s) killed\n", sprites_.size());
        for (auto& s : sprites_) {
            if (s.video) perf::tag(perf::TagVideoStop);
            pipeline_->removeSpriteTexture(s.texture);
        }
        sprites_.clear();
        return;
    }
    rlog("[reforce] %zu sprite(s) killed: fading out over %.2f s\n", sprites_.size(), fade);
    for (auto& s : sprites_) s.run->fadeOutFrom(t, fade);
}

render::VideoLevels Visualizer::videoLevels(const video::SourceSpec& spec) const {
    // The movie's own levels (.reforce.txt; defaults for live sources), its Gamma times reforce.MovieSprite.Gamma.
    const double global = std::isfinite(opts_.prefs.movieSpriteGamma) && opts_.prefs.movieSpriteGamma > 0
                              ? std::min(opts_.prefs.movieSpriteGamma, 10.0) : 1.0;
    render::VideoLevels l;
    if (spec.white > spec.black) {
        l.black = float(std::clamp(spec.black, 0.0, 1.0));
        l.white = float(std::clamp(spec.white, 0.0, 1.0));
    }
    const double g = (spec.gamma > 0 ? spec.gamma : 1.0) * global;
    l.gamma = float(std::clamp(g, 0.01, 100.0));
    return l;
}

void Visualizer::setSpriteMix(bool movie, double mix) {
    const double m = std::isfinite(mix) ? std::clamp(mix, 0.0, 1.0) : 0.0;
    (movie ? movieSpriteMix_ : spriteMix_) = m;
}

int Visualizer::holdVideoSprites() {
    int n = 0;
    for (auto& p : spritePending_)
        if (p.video) {
            p.held = true;
            ++n;
        }
    for (auto& s : sprites_)
        if (s.video && !s.videoFailed) {
            s.held = true;
            s.video->setLoop(true);
            s.run->holdForever();
            ++n;
        }
    rlog("[reforce] %d video sprite(s) loop until stopped\n", n);
    return n;
}

bool Visualizer::videoSpritesHeld() const {
    for (const auto& s : sprites_)
        if (s.held) return true;
    for (const auto& p : spritePending_)
        if (p.held) return true;
    return false;
}

std::size_t Visualizer::fadeOutSprites(double t, double fade, const std::vector<std::string>& names) {
    auto named = [&](const std::string& n) { return std::find(names.begin(), names.end(), n) != names.end(); };
    std::size_t n = 0;
    for (auto it = spritePending_.begin(); it != spritePending_.end();)
        if (named(it->label)) {
            it = spritePending_.erase(it);
            ++n;
        } else {
            ++it;
        }
    for (auto& s : sprites_)
        if (named(s.run->name())) {
            s.run->fadeOutFrom(t, fade);
            ++n;
        }
    if (n) {
        perf::tag(perf::TagKill);
        rlog("[reforce] %zu hidden sprite(s): fading out over %.2f s\n", n, fade);
    }
    return n;
}

std::vector<std::string> Visualizer::spriteNames(SpriteKind kind) const {
    std::vector<std::string> n;
    for (const auto& s : sprites_)
        if (kind == SpriteKind::All || bool(s.video) == (kind == SpriteKind::Movies)) n.push_back(s.run->name());
    return n;
}

engine::SpriteAudio Visualizer::spriteAudio(const audio::AudioFrame* audio) const {
    engine::SpriteAudio a;
    if (audio) {
        a.bass = audio->bass;
        a.level = audio->level;
        a.energy = audio->energy;
        a.mid = audio->mid;
        a.treble = audio->treble;
        a.beat = audio->beat;
        a.tempo = audio->tempo;
        a.linkOn = audio->linkOn ? 1.0 : 0.0;
        a.linkBpm = audio->linkBpm;
        a.linkBeat = audio->linkBeat;
        a.linkPhase = audio->linkPhase;
    }
    a.sync = syncInput();
    return a;
}

void Visualizer::stepSprites(double t, const audio::AudioFrame* audio) {
    feedbackQuads_.clear();
    displayQuads_.clear();
    colourQuads_.clear();
    colourFrames_.clear();
    adoptSprites(t);
    for (auto it = sprites_.begin(); it != sprites_.end();) {
        if (it->run->expired(t)) {
            rlog("[reforce] Sprite \"%s\" ended\n", it->run->name().c_str());
            if (it->video) perf::tag(perf::TagVideoStop);
            pipeline_->removeSpriteTexture(it->texture);
            it = sprites_.erase(it);
        } else {
            ++it;
        }
    }
    const engine::SpriteAudio a = spriteAudio(audio);
    videoSpriteDraws_.clear();
    const engine::BeatSample beat = beatSample();
    const double beatDt = beatClockDt_;
    for (auto& s : sprites_) {
        const engine::SpriteDrawSpec d = s.run->evaluate(t, double(opts_.width), double(opts_.height), a);
        if (s.video) {
            if (s.video->failed() && !s.videoFailed) {
                // Decode error, camera unplugged, ...: fade out instead of freezing.
                s.videoFailed = true;
                videoError("sprite \"" + s.run->name() + "\": " + s.video->error());
                s.run->fadeOutFrom(t, 0.5);
            }
            // Movie: the frame at the sprite's age (looping); live: the newest
            // (null while a live input is gone: the sprite draws nothing).
            video::FramePtr f;
            if (s.beatSync) {
                const double u = s.beatSync->step(beatDt, beat, movieBeatSyncOn());
                f = s.video->frameAtMedia(u);
                // Plays once: the movie ends when the media reaches the section end at the current rate, so
                // the lifetime follows the rate (slower: longer) and the fade-out still ends with the movie.
                const auto& sp = s.video->spec();
                if (sp.kind == video::SourceKind::File && !sp.loop && !s.held && s.beatSync->rate() > 1e-3) {
                    const double end = video::sectionEnd(sp, s.video->duration());
                    if (end > 0) s.run->fitLifetime((t - s.run->startTime()) + std::max(0.0, end - u) / s.beatSync->rate());
                }
            } else {
                f = s.video->frameAt(t - s.run->startTime());
            }
            pipeline_->setSpriteFrame(s.texture, f ? f->texture() : nullptr);
            if (f && d.opacity > 0.0 && f->width() > 0 && f->height() > 0) {
                const double scale = std::min(d.w / f->width(), d.h / f->height());
                videoSpriteDraws_.push_back({s.texture, s.video.get(), f, scale});
            }
            if (f && movieColourOver_ && d.opacity > 0.0) colourFrames_.push_back(f);
            if (f) stepFrames_.push_back(std::move(f));
        }
        if (d.opacity <= 0.0) continue;
        render::SpriteQuad q;
        q.texture = s.texture;
        q.cx = float(d.cx);
        q.cy = float(d.cy);
        q.w = float(d.w);
        q.h = float(d.h);
        q.opacity = float(d.opacity);
        if (s.video && movieColourOver_) {
            // Full colour (PLAN 5K): not in the intensity buffer; alpha-over after colorize. The Mix scales the
            // opacity from half (Mix 0, like the half-way lighten) to the full envelope (Mix 1).
            q.opacity = float(d.opacity * (0.5 + 0.5 * movieSpriteMix_));
            q.opaque = !s.video->spec().colorKey;  // ColorKey = 0: opaque in the Syphon alpha
            colourQuads_.push_back(q);
            continue;
        }
        const engine::SpriteLayerPlan plan =
            engine::spriteLayerPlan(d, spriteLayering_, spriteTrails_, s.video ? movieSpriteMix_ : spriteMix_);
        auto quad = [&](const engine::SpriteLayerDraw& w) {
            render::SpriteQuad r = q;
            r.pen = float(w.pen);
            r.blend = int(w.blend);
            r.op = int(w.op);
            r.mix = float(w.mix);
            return r;
        };
        for (int i = 0; i < plan.feedbackCount; ++i) feedbackQuads_.push_back(quad(plan.feedback[i]));
        if (plan.hasDisplay) displayQuads_.push_back(quad(plan.display));
    }
    // sprites_ is in start order; G-Force draws the oldest last, on top (probe #42).
    if (engine::spriteOldestOnTop(spriteLayering_)) {
        std::reverse(feedbackQuads_.begin(), feedbackQuads_.end());
        std::reverse(displayQuads_.begin(), displayQuads_.end());
        std::reverse(colourQuads_.begin(), colourQuads_.end());
    }
}

}  // namespace reforce::app
