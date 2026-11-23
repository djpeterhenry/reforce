// Visualizer: the color video layer, video sprites, the camera backdrop, body tracking and beat-synced movies.
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

// ---------------------------------------------------------------- camera backdrop (docs/backdrop.md)

namespace {
constexpr double kBackdropFade = 0.4;  // seconds to fade the backdrop in or out
}

void Visualizer::setBackdrop(const std::optional<video::SourceSpec>& spec) {
    if (!spec) {
        if (bdWanted_) rlog("[reforce] backdrop off\n");
        bdWanted_.reset();
        bdPending_.reset();
        return;
    }
    if (opts_.syncBuilds && spec->kind != video::SourceKind::File) {
        videoError("backdrop: cameras and Syphon are not used in deterministic runs");
        return;
    }
    const std::string want = video::specToString(*spec);
    bdWanted_ = spec;
    if (bdSrc_ && video::specToString(bdSrc_->spec()) == want && !bdSrc_->failed()) {
        bdPending_.reset();  // already showing it (perhaps fading out): fade back in
        return;
    }
    if (bdPending_ && video::specToString(bdPending_->spec()) == want) return;
    rlog("[reforce] backdrop: %s\n", want.c_str());
    bdPending_ = openVideo(*spec);
    if (!bdPending_) bdWanted_.reset();
}

void Visualizer::stepBackdrop(double t) {
    const double dt = bdLastT_ < 0 ? 0 : std::clamp(t - bdLastT_, 0.0, 0.25);
    bdLastT_ = t;
    if (bdPending_) {
        if (bdPending_->failed()) {
            videoError("backdrop: " + bdPending_->label() + ": " + bdPending_->error());
            bdPending_.reset();
            if (!bdSrc_) bdWanted_.reset();
        } else if (bdPending_->ready() && (!bdSrc_ || bdLevel_ <= 0)) {
            // The old one has faded out (or there was none): the new one fades in.
            bdSrc_ = std::move(bdPending_);
            trackedSrc_ = nullptr;  // body tracking attaches to the new source (even at a reused address)
            bdStart_ = t;
            bdLevel_ = 0;
            bdFailReported_ = false;
        }
    }
    if (bdSrc_ && bdSrc_->failed() && !bdFailReported_) {
        videoError("backdrop: " + bdSrc_->label() + ": " + bdSrc_->error());
        bdFailReported_ = true;
    }
    const bool show = bdSrc_ && bdWanted_ && !bdPending_;
    bdLevel_ = show ? std::min(1.0, bdLevel_ + dt / kBackdropFade) : std::max(0.0, bdLevel_ - dt / kBackdropFade);
    if (!bdWanted_ && bdSrc_ && bdLevel_ <= 0) bdSrc_.reset();
}

void Visualizer::encodeBackdrop(MTL::CommandBuffer* cmd, MTL::Texture* frame) {
    MTL::Texture* tex = nullptr;
    engine::UvTransform uv;
    if (bdSrc_ && bdLevel_ > 0) {
        video::FramePtr f = backdropFrame();
        if (f && f->width() > 0 && f->height() > 0) {
            const double W = double(frame->width()), H = double(frame->height());
            uv = engine::fitTransform(f->width(), f->height(), W, H, bdFit_);
            const double sx = W / (double(f->width()) * std::max(1e-6, double(uv.scale[0])));
            const double sy = H / (double(f->height()) * std::max(1e-6, double(uv.scale[1])));
            tex = minified(cmd, bdSrc_.get(), f, std::min(sx, sy));
            bdHold_.push_back(std::move(f));
            holdFrames(cmd, bdHold_);
        }
    }
    render::BackdropBody body;
    const bool tracking = tracker_ != nullptr;
    if (tracking) {
        body.mask = maskCur_ >= 0 ? maskTex_[std::size_t(maskCur_)] : nullptr;
        body.front = float(body_.front);
        body.show = body_.show;
        body.joints = bodyJoints_;
    }
    pipeline_->encodeBackdrop(cmd, frame, tex, uv, bdMirror_, float(bdOpacity_ * bdLevel_),
                              float(keySimilarity_ / 1000.0), float(keySmoothness_ / 1000.0),
                              tracking ? &body : nullptr);
}

// ---------------------------------------------------------------- body tracking (docs/body.md)

bool Visualizer::backdropUv(engine::UvTransform& uv) const {
    if (!bdSrc_ || !bdSrc_->ready() || bdSrc_->width() <= 0 || bdSrc_->height() <= 0) return false;
    uv = engine::fitTransform(bdSrc_->width(), bdSrc_->height(), opts_.width, opts_.height, bdFit_);
    return true;
}

video::FramePtr Visualizer::backdropFrame() {
    video::FramePtr newest = bdSrc_->frameAt(std::max(0.0, bdLastT_ - bdStart_));
    // In step with the silhouette: the frame it was computed from, unless the tracker has fallen behind (starting,
    // stalled): then the newest.
    if (tracker_ && body_.sync && bdSyncFrame_ && newest && newest->index() >= bdSyncFrame_->index() &&
        newest->index() - bdSyncFrame_->index() <= 3)
        return bdSyncFrame_;
    return newest;
}

void Visualizer::resetBody() {
    pose_.reset();
    trails_.reset();
    poseSample_ = {};
    bodySeq_ = 0;
    maskCur_ = maskPrev_ = -1;
    bodyJoints_ = {};
    bdSyncFrame_ = nullptr;
}

void Visualizer::stepBody(double t) {
    const double dt = bodyLastT_ < 0 ? 0 : std::clamp(t - bodyLastT_, 0.0, 0.25);
    bodyLastT_ = t;
    // Backdrop frames per second (shown next to the tracker's rate).
    if (bdSrc_) {
        const std::uint64_t dec = bdSrc_->stats().decoded;
        if (bdCamT_ < 0 || t < bdCamT_) {
            bdCamT_ = t;
            bdCamCount_ = dec;
        } else if (t - bdCamT_ >= 1.0) {
            bdCamFps_ = double(dec - std::min(dec, bdCamCount_)) / (t - bdCamT_);
            bdCamT_ = t;
            bdCamCount_ = dec;
        }
    }
    if (!body_.track || !bdSrc_) {
        if (tracker_) {
            rlog("[reforce] body tracking stopped\n");
            if (bdSrc_ && trackedSrc_ == bdSrc_.get()) bdSrc_->setFrameListener(nullptr);
            tracker_.reset();
            trackedSrc_ = nullptr;
            resetBody();
        }
        return;
    }
    if (!tracker_) {
        tracker_ = std::make_shared<video::BodyTracker>();
        rlog("[reforce] body tracking on %s\n", bdSrc_->label().c_str());
    }
    tracker_->setQuality(body_.fast ? video::BodyQuality::Fast : video::BodyQuality::Balanced);
    tracker_->setPose(body_.trails > 0 || body_.show);  // the silhouette alone is about half the work
    if (trackedSrc_ != bdSrc_.get()) {  // a new backdrop source: forget the old one's body
        tracker_->reset();
        trackedSrc_ = bdSrc_.get();
        resetBody();
        // Live sources hand each frame to the tracker as it arrives (no wait for the next render step).
        std::weak_ptr<video::BodyTracker> weak = tracker_;
        const void* src = bdSrc_.get();
        trackerListens_ = bdSrc_->setFrameListener([weak, src](const video::FramePtr& f) {
            if (auto t = weak.lock()) t->offer(f, src);
        });
    }
    if (!trackerListens_)
        if (video::FramePtr f = bdSrc_->frameAt(std::max(0.0, bdLastT_ - bdStart_))) tracker_->offer(f, bdSrc_.get());
    const video::BodyResult r = tracker_->latest();
    if (r.seq != 0 && r.seq != bodySeq_) {
        bodySeq_ = r.seq;
        if (r.mask && r.maskW > 0 && r.maskH > 0) uploadBodyMask(*r.mask, r.maskW, r.maskH);
        bdSyncFrame_ = r.frame;  // the backdrop can show exactly this frame with its mask
        poseSample_.seq = r.seq;
        for (int j = 0; j < engine::kPoseJoints; ++j) {
            const auto& p = r.joints[std::size_t(j)];
            poseSample_.x[std::size_t(j)] = p.x;
            poseSample_.y[std::size_t(j)] = p.y;
            poseSample_.conf[std::size_t(j)] = r.person ? p.conf : 0.0f;
        }
    }
    pose_.step(poseSample_, dt);
    engine::UvTransform uv;
    if (!backdropUv(uv)) return;
    const double W = opts_.width, H = opts_.height;
    std::array<std::array<double, 2>, engine::kPoseJoints> scr{};
    for (int j = 0; j < engine::kPoseJoints; ++j) {
        const auto& J = pose_.joint(j);
        scr[std::size_t(j)] = engine::cameraToScreen(J.x, J.y, uv, bdMirror_);
        float* o = &bodyJoints_[std::size_t(j) * 4];
        o[0] = float(scr[std::size_t(j)][0]);
        o[1] = float(scr[std::size_t(j)][1]);
        o[2] = float(J.weight);
        o[3] = 0;
    }
    if (body_.trails > 0) {
        std::array<std::array<double, 3>, 2> hands{};
        int i = 0;
        for (int j : {engine::JLeftWrist, engine::JRightWrist}) {
            hands[std::size_t(i++)] = {scr[std::size_t(j)][0] * W, scr[std::size_t(j)][1] * H, pose_.joint(j).weight};
        }
        trails_.step(hands, dt, body_.trails, body_.trailWidth, W, H, segments_);
    } else {
        trails_.reset();
    }
}

void Visualizer::uploadBodyMask(const std::vector<std::uint8_t>& mask, int w, int h) {
    if (mask.size() < std::size_t(w) * std::size_t(h)) return;
    if (w != maskW_ || h != maskH_ || !maskTex_[0]) {
        releaseBodyMasks();
        MTL::TextureDescriptor* td = MTL::TextureDescriptor::texture2DDescriptor(
            MTL::PixelFormatR8Unorm, NS::UInteger(w), NS::UInteger(h), false);
        td->setUsage(MTL::TextureUsageShaderRead);
        td->setStorageMode(MTL::StorageModeShared);
        for (auto& tex : maskTex_) {
            perf::countRenderAlloc();  // once per mask size
            tex = pipeline_->device()->newTexture(td);
            if (!tex) {
                releaseBodyMasks();
                return;
            }
        }
        maskW_ = w;
        maskH_ = h;
        maskCur_ = maskPrev_ = -1;
    }
    const int next = (maskCur_ + 1) % int(maskTex_.size());
    maskTex_[std::size_t(next)]->replaceRegion(MTL::Region(0, 0, NS::UInteger(w), NS::UInteger(h)), 0, mask.data(),
                                               NS::UInteger(w));
    maskPrev_ = maskCur_;
    maskCur_ = next;
}

void Visualizer::releaseBodyMasks() {
    for (auto& tex : maskTex_)
        if (tex) {
            tex->release();
            tex = nullptr;
        }
    maskW_ = maskH_ = 0;
    maskCur_ = maskPrev_ = -1;
}

// ---------------------------------------------------------------- video (Phase 5F)

std::unique_ptr<video::VideoSource> Visualizer::openVideo(const video::SourceSpec& spec) {
    perf::tag(perf::TagVideoStart);
    video::OpenOptions o;
    o.deterministic = opts_.syncBuilds && spec.kind == video::SourceKind::File;
    auto src = video::openSource(pipeline_->device(), spec, o);
    if (!src) videoError("cannot open " + video::specToString(spec));
    return src;
}

MTL::Texture* Visualizer::minified(MTL::CommandBuffer* cmd, const void* slot, const video::FramePtr& f,
                                   double scale) {
    if (!f) return nullptr;
    if (!videoMips_) return f->texture();
    return videoMips_->prepare(cmd, slot, f->texture(), f, scale);
}

void Visualizer::videoError(const std::string& msg) {
    videoError_ = msg;
    rlog("[reforce] video: %s\n", msg.c_str());
}

void Visualizer::holdFrames(MTL::CommandBuffer* cmd, std::vector<video::FramePtr>& frames) {
    if (frames.empty()) return;
    // Command buffers retain the textures, but a CVMetalTexture's IOSurface must
    // not go back to the decoder's pool while the GPU still reads it.
    auto keep = std::make_shared<std::vector<video::FramePtr>>();
    keep->swap(frames);
    cmd->addCompletedHandler([keep](MTL::CommandBuffer*) { keep->clear(); });
}

bool Visualizer::movieLoops(const std::string& path, bool fallback) const {
    if (fallback || videoLoopRules_.empty()) return fallback;
    const std::string stem = std::filesystem::path(path).stem().string();
    return engine::isExcluded(videoLoopRules_, scene::SceneType::Sprite, engine::SceneEntry{stem, stem, path});
}

void Visualizer::applyMovieOptions(video::SourceSpec& spec, bool loopDefault) const {
    if (spec.kind != video::SourceKind::File) return;
    spec.loop = movieLoops(spec.id, loopDefault);
    const engine::MediaOptions o = engine::loadMediaOptions(spec.id);
    if (o.loop) spec.loop = *o.loop;
    if (o.rate) spec.rate = *o.rate;
    if (o.start) spec.offset = *o.start;
    if (o.end) spec.end = *o.end;
    spec.black = o.black.value_or(0.0);
    spec.white = o.white.value_or(1.0);
    spec.gamma = o.gamma.value_or(1.0);
    spec.colorKey = o.colorKey.value_or(true);
    // A frame-interpolated copy ("<stem>.rife4x.mov", same start and length) plays in its place.
    spec.id = engine::interpolatedMovie(spec.id).string();
    if (o.bpm) {
        // Beat-synced movies play from a downbeat (the first at or after Start) and loop whole bars, so every
        // pass keeps the phase and the jump back lands on "the 1".
        const double bar = double(o.beatsPerBar.value_or(4)) * 60.0 / *o.bpm;
        const double beatStart = o.beatStart.value_or(0.0);
        double from = beatStart + std::ceil((spec.offset - beatStart) / bar - 1e-9) * bar;
        if (from < 0) from += bar;
        if (!(spec.end > spec.offset) || from + bar <= spec.end) spec.offset = from;
        spec.loopUnit = bar;
    }
}

video::SourceSpec Visualizer::withVideoDefaults(video::SourceSpec spec) const {
    if (spec.kind == video::SourceKind::File) {
        spec.rate = videoParams_.rate;
        spec.offset = videoParams_.offset;
        applyMovieOptions(spec, videoParams_.loop);
    }
    if (spec.kind == video::SourceKind::Syphon) spec.allowFeedback = opts_.prefs.syphonAllowFeedback;
    return spec;
}

bool Visualizer::startVideoSprite(const video::SourceSpec& spec, const std::string& label, double t,
                                  const FadeSpec& fade, const std::filesystem::path& paramsFor) {
    auto params = makeSpriteParams(paramsFor);
    if (!params) return false;
    PendingSprite p;
    p.video = openVideo(spec);
    if (!p.video) return false;
    p.params = std::move(*params);
    p.path = paramsFor;
    p.label = label;
    p.fade = fade;
    p.requested = t;
    p.seed = opts_.seed + 0x1000 * ++loads_;
    spritePending_.push_back(std::move(p));
    adoptSprites(t);
    return true;
}

void Visualizer::stopVideoSprites(double t, double fade) {
    std::size_t n = 0;
    for (auto it = spritePending_.begin(); it != spritePending_.end();) {
        if (it->video) {
            it = spritePending_.erase(it);
            ++n;
        } else {
            ++it;
        }
    }
    for (auto& s : sprites_)
        if (s.video) {
            s.run->fadeOutFrom(t, fade);
            ++n;
        }
    perf::tag(perf::TagVideoStop);
    rlog("[reforce] %zu video sprite(s) stopped (fade %.2f s)\n", n, fade);
}

void Visualizer::playVideoLayer(const video::SourceSpec& spec, double t) {
    vlPending_ = openVideo(withVideoDefaults(spec));
    if (vlPending_) rlog("[reforce] video layer: opening %s\n", video::specToString(vlPending_->spec()).c_str());
    stepVideoLayer(t);  // deterministic runs adopt it now; live ones once the first frame is in
}

void Visualizer::stopVideoLayer(double t) {
    perf::tag(perf::TagVideoStop);
    vlPending_.reset();
    if (vlFading_) {
        // Finish the running fade first.
        if (vlB_.src) vlA_ = std::move(vlB_);
        else vlA_ = {};
        vlB_ = {};
        vlFading_ = false;
    }
    if (!vlA_.src) return;
    rlog("[reforce] video layer: \"%s\" fades out over %.2f s\n", vlA_.src->label().c_str(), videoParams_.crossfade);
    if (videoParams_.crossfade <= 0) {
        vlA_ = {};
        return;
    }
    vlFading_ = true;
    vlFadeStart_ = t;
    vlFadeDur_ = videoParams_.crossfade;
    vlWeight_ = 0;
}

std::string Visualizer::setVideoParam(const std::string& key, const std::string& value) {
    std::string k, v;
    for (char c : key) k.push_back(char(std::tolower(static_cast<unsigned char>(c))));
    for (char c : value) v.push_back(char(std::tolower(static_cast<unsigned char>(c))));
    if (k == "spritecolor" || k == "spritecolor") {
        if (v == "over" || v == "1" || v == "on" || v == "true" || v == "yes") movieColorOver_ = true;
        else if (v == "off" || v == "0" || v == "false" || v == "no") movieColorOver_ = false;
        else return "spritecolor: over | off";
        rlog("[reforce] full-color movie sprites %s\n", movieColorOver_ ? "on (over)" : "off");
        return {};
    }
    if (k == "syphonalpha") {
        if (v == "1" || v == "on" || v == "true" || v == "yes") syphonAlpha_ = true;
        else if (v == "0" || v == "off" || v == "false" || v == "no") syphonAlpha_ = false;
        else return "syphonalpha: on | off";
        rlog("[reforce] Syphon alpha %s\n", syphonAlpha_ ? "on" : "off");
        return {};
    }
    if (k == "lightspair") {  // room lights: pair with the panels (start | cancel)
        if (v != "start" && v != "cancel") return "lightspair: start | cancel";
        if (!lightsOut_) return "lights: not in this instance (offline, or --background without --lights)";
        if (v == "start") lightsOut_->startPairing();
        else lightsOut_->cancelPairing();
        return {};
    }
    if (k.rfind("lights", 0) == 0) {  // room lights (docs/lights.md)
        const bool was = lights_.enabled;
        if (auto err = engine::applyLightsSetting(lights_, k.substr(6), v); !err.empty()) return err;
        if (k == "lights" && was != lights_.enabled) rlog("[reforce] lights %s\n", lights_.enabled ? "on" : "off");
        if (!lightsOut_ && lights_.enabled) rlog("[reforce] lights: not in this instance (offline, or --background without --lights)\n");
        applyLightsSettings();
        return {};
    }
    if (k == "windowvisuals") {
        if (v == "1" || v == "on" || v == "true" || v == "yes") windowVisualsOnly_ = true;
        else if (v == "0" || v == "off" || v == "false" || v == "no") windowVisualsOnly_ = false;
        else return "windowvisuals: on | off";
        rlog("[reforce] window shows %s\n", windowVisualsOnly_ ? "the visuals only" : "everything");
        return {};
    }
    if (k == "keysimilarity" || k == "keysmoothness") {
        char* end = nullptr;
        const double x = std::strtod(v.c_str(), &end);
        if (v.empty() || end == v.c_str() || *end || !(x >= 1 && x <= 1000)) return k + ": 1..1000";
        (k == "keysimilarity" ? keySimilarity_ : keySmoothness_) = x;
        return {};
    }
    if (k.size() > 4 && k.rfind("body", 0) == 0) {  // body tracking (docs/body.md): body<key>
        const std::string err = engine::applyBodySetting(body_, k.substr(4), value);
        if (err.empty()) rlog("[reforce] body %s = %s\n", k.substr(4).c_str(), v.c_str());
        return err;
    }
    if (k == "backdropmirror") {
        if (v == "1" || v == "on" || v == "true" || v == "yes") bdMirror_ = true;
        else if (v == "0" || v == "off" || v == "false" || v == "no") bdMirror_ = false;
        else return "backdropmirror: on | off";
        return {};
    }
    if (k == "backdropfit") {
        auto f = engine::parseVideoFit(v);
        if (!f) return "backdropfit: cover | contain | stretch";
        bdFit_ = *f;
        return {};
    }
    if (k == "backdropopacity") {
        char* end = nullptr;
        const double x = std::strtod(v.c_str(), &end);
        if (v.empty() || end == v.c_str() || *end || !(x >= 0 && x <= 1)) return "backdropopacity: 0..1";
        bdOpacity_ = x;
        return {};
    }
    if (k == "beatoffset") {
        char* end = nullptr;
        const double ms = std::strtod(v.c_str(), &end);
        if (v.empty() || end == v.c_str() || *end || !std::isfinite(ms)) return "beatoffset: milliseconds";
        beatOffsetOverride_ = std::clamp(ms, -200.0, 500.0);
        rlog("[reforce] beat offset %+.0f ms\n", *beatOffsetOverride_);
        return {};
    }
    if (k == "beatsync") {
        if (v == "1" || v == "on" || v == "true" || v == "yes") beatSyncOverride_ = true;
        else if (v == "0" || v == "off" || v == "false" || v == "no") beatSyncOverride_ = false;
        else return "beatsync: on | off";
        rlog("[reforce] movie beat sync %s\n", *beatSyncOverride_ ? "on" : "off");
        return {};
    }
    std::string err = engine::applyVideoSetting(videoParams_, key, value);
    // "Loop files" also changes the movie playing on the layer now.
    if (err.empty() && key == "loop")
        for (LayerSlot* s : {&vlA_, &vlB_})
            if (s->src && s->src->spec().kind == video::SourceKind::File) {
                s->src->setLoop(videoParams_.loop);
                if (videoParams_.loop) s->endHandled = false;
            }
    return err;
}

void Visualizer::stepVideoLayer(double t) {
    if (vlPending_) {
        if (!vlPending_->ready() && !vlPending_->failed() && opts_.syncBuilds &&
            vlPending_->spec().kind == video::SourceKind::File)
            vlPending_->waitReady(20);
        if (vlPending_->failed()) {
            videoError("layer: " + vlPending_->label() + ": " + vlPending_->error());
            vlPending_.reset();
        } else if (vlPending_->ready()) {
            perf::tag(perf::TagVideoStart);
            if (vlFading_) {  // a change during a crossfade: the incoming one becomes the outgoing one
                if (vlB_.src) vlA_ = std::move(vlB_);
                else vlA_ = {};
                vlB_ = {};
            }
            vlB_.src = std::move(vlPending_);
            vlB_.start = t;
            vlB_.beatSync = makeBeatSync(*vlB_.src, "video layer \"" + vlB_.src->label() + "\"");
            vlB_.lastT = t;
            vlFading_ = true;
            vlFadeStart_ = t;
            vlFadeDur_ = videoParams_.crossfade;
            const auto st = vlB_.src->stats();
            rlog("[reforce] video layer: \"%s\" %dx%d%s starts, crossfade %.2f s (decode %.2f ms/frame)\n",
                 vlB_.src->label().c_str(), vlB_.src->width(), vlB_.src->height(),
                 vlA_.src ? (" from \"" + vlA_.src->label() + "\"").c_str() : "", vlFadeDur_, st.decodeMsAvg);
        }
    }
    if (vlFading_) {
        vlWeight_ = vlFadeDur_ > 0 ? std::clamp((t - vlFadeStart_) / vlFadeDur_, 0.0, 1.0) : 1.0;
        if (vlWeight_ >= 1.0) {
            if (!vlB_.src && vlA_.src) rlog("[reforce] video layer: \"%s\" stopped\n", vlA_.src->label().c_str());
            vlA_ = std::move(vlB_);
            vlB_ = {};
            vlFading_ = false;
            vlWeight_ = 1;
        }
    }
    // The shown source failed after it started (decode error, camera
    // unplugged, ...): report it once and fade the layer out rather than
    // freezing on its last frame. A failed outgoing source just fades.
    {
        LayerSlot& shown = vlFading_ ? vlB_ : vlA_;
        if (shown.src && shown.src->failed() && !shown.failReported) {
            shown.failReported = true;
            videoError("layer: " + shown.src->label() + ": " + shown.src->error());
            stopVideoLayer(t);
        }
    }
    // A play-once movie on the layer: fade out so the fade ends with the movie.
    {
        LayerSlot& shown = vlFading_ ? vlB_ : vlA_;
        if (shown.src && !shown.endHandled && shown.src->spec().kind == video::SourceKind::File &&
            !shown.src->spec().loop) {
            const auto& sp = shown.src->spec();
            const double len = video::playLength(sp, shown.src->duration());
            const double end = video::sectionEnd(sp, shown.src->duration());
            if (len > 0) {
                // Beat-synced: what is left of the media at the current rate.
                const double left = shown.beatSync && shown.beatSync->rate() > 1e-3
                                        ? (end - shown.beatSync->mediaTime()) / shown.beatSync->rate()
                                        : len - (t - shown.start);
                if (left <= videoParams_.crossfade) {
                    shown.endHandled = true;
                    rlog("[reforce] video layer: \"%s\" ends (plays once)\n", shown.src->label().c_str());
                    stopVideoLayer(t);
                }
            }
        }
    }
    const engine::BeatSample beat = beatSample();
    for (LayerSlot* s : {&vlA_, &vlB_}) {
        if (!s->src) {
            s->frame = nullptr;
            continue;
        }
        if (s->beatSync) {
            const double dt = s->lastT < 0 ? 0.0 : std::max(0.0, t - s->lastT);
            s->lastT = t;
            s->frame = s->src->frameAtMedia(s->beatSync->step(dt, beat, movieBeatSyncOn()));
        } else {
            s->frame = s->src->frameAt(t - s->start);
        }
    }
}

engine::BeatSample Visualizer::beatSample() const {
    const sync::BeatClockState& bc = beatClock_.state();
    engine::BeatSample b;
    b.confident = bc.confident;
    b.source = int(bc.source);
    b.bpm = bc.bpm;
    b.beat = bc.beat;
    b.bars = bc.bars;
    return b;
}

std::unique_ptr<engine::MovieBeatSync> Visualizer::makeBeatSync(const video::VideoSource& src,
                                                                const std::string& who) const {
    const video::SourceSpec& sp = src.spec();
    if (sp.kind != video::SourceKind::File) return nullptr;
    const engine::MediaOptions o = engine::loadMediaOptions(sp.id);
    if (!o.bpm) return nullptr;
    engine::ClipBeats cb;
    cb.bpm = *o.bpm;
    cb.beatStart = o.beatStart.value_or(0.0);
    cb.beatsPerBar = o.beatsPerBar.value_or(4);
    rlog("[reforce] %s: beat sync at %.2f BPM (%s, from %.2f s)%s\n", who.c_str(), cb.bpm,
         std::filesystem::path(sp.id).filename().string().c_str(), sp.offset,
         movieBeatSyncOn() ? "" : ", off (reforce.Video.BeatSync)");
    return std::make_unique<engine::MovieBeatSync>(cb, sp.offset, sp.rate);
}

void Visualizer::videoState(engine::VideoState& v, double t) const {
    (void)t;
    const engine::VideoLayerParams& p = videoParams_;
    v.layerActive = videoLayerActive();
    const LayerSlot& shown = vlFading_ && vlB_.src ? vlB_ : vlA_;
    v.layerSource = vlA_.src ? vlA_.src->label() : std::string();
    v.layerIncoming = vlFading_ && vlB_.src ? vlB_.src->label() : std::string();
    v.layerPending = vlPending_ ? vlPending_->label() : std::string();
    v.layerWaiting = vlPending_ ? vlPending_->waiting() : shown.src ? shown.src->waiting() : std::string();
    if (v.layerWaiting.empty())
        for (const auto& p : spritePending_)
            if (p.video && !(v.layerWaiting = p.video->waiting()).empty()) break;
    v.layerSpec = shown.src ? video::specToString(shown.src->spec()) : std::string();
    v.crossfade = vlFading_ ? vlWeight_ : -1.0;
    v.width = shown.src ? shown.src->width() : 0;
    v.height = shown.src ? shown.src->height() : 0;
    v.fps = shown.src ? shown.src->frameRate() : 0;
    v.duration = shown.src ? shown.src->duration() : 0;
    if (shown.src) {
        const video::SourceStats st = shown.src->stats();
        v.decodeMsAvg = st.decodeMsAvg;
        v.decodeMsMax = st.decodeMsMax;
        v.decoded = st.decoded;
        v.dropped = st.dropped;
    } else {
        v.decodeMsAvg = v.decodeMsMax = 0;
        v.decoded = v.dropped = 0;
    }
    v.blend = engine::videoBlendName(p.blend);
    v.fit = engine::videoFitName(p.fit);
    v.order = engine::videoOrderName(p.order);
    v.key = engine::videoKeyName(p.key);
    v.keyColor = engine::colorToHex(p.keyColor);
    v.opacity = p.opacity;
    v.threshold = p.lumaThreshold;
    v.tolerance = p.tolerance;
    v.softness = p.softness;
    v.crossfadeSeconds = p.crossfade;
    v.rate = p.rate;
    v.offset = p.offset;
    v.loop = p.loop;
    v.toSyphon = p.toSyphon;
    v.sprites.clear();
    for (const auto& s : sprites_)
        if (s.video)
            v.sprites.push_back(s.run->name() + (s.beatSync && s.beatSync->synced() ? " (beat-synced)" : "") +
                                (s.held ? " (looping until stopped)" : ""));
    v.spritesHeld = videoSpritesHeld();
    v.beatSync = movieBeatSyncOn();
    v.spriteColor = movieColorOver_ ? "over" : "off";
    v.syphonAlpha = syphonAlpha_;
    v.windowVisuals = windowVisualsOnly_;
    v.lightsAvailable = lightsOut_ != nullptr;
    v.lightsOn = lights_.enabled;
    v.lightsBrightness = lights_.brightness;
    v.lightsSaturation = lights_.saturation;
    v.lightsFloor = lights_.floor;
    v.lightsSmoothing = lights_.smoothing;
    if (lightsOut_) {
        const lights::LightsStatus ls = lightsOut_->status();
        v.lightsState = ls.state;
        v.lightsMessage = ls.message;
        v.lightsDevice = ls.device;
        v.lightsPanels = ls.panels;
        v.lightsHz = ls.sendHz;
        v.lightsPulsing = ls.pulsing;
        v.lightsSide = ls.sideLength;
        v.lightsLayout.clear();
        v.lightsColors.clear();
        static const char* hex = "0123456789abcdef";
        for (const lights::PanelView& p : lightsOut_->panels()) {
            v.lightsLayout.insert(v.lightsLayout.end(), {p.x, p.y, p.o});
            for (std::uint8_t b : {p.color.r, p.color.g, p.color.b}) {
                v.lightsColors.push_back(hex[b >> 4]);
                v.lightsColors.push_back(hex[b & 15]);
            }
        }
    }
    v.lightsPulse = lights_.pulse;
    v.lightsPulseTiming = lights_.pulseTimingMs;
    v.keySimilarity = keySimilarity_;
    v.keySmoothness = keySmoothness_;
    v.backdropOn = bdWanted_.has_value();
    v.backdropSpec = bdWanted_ ? video::specToString(*bdWanted_) : std::string();
    v.backdropSource = bdSrc_ ? bdSrc_->label() : std::string();
    v.backdropPending = bdPending_ ? bdPending_->label() : std::string();
    v.backdropWaiting = bdPending_ ? bdPending_->waiting() : bdSrc_ ? bdSrc_->waiting() : std::string();
    v.backdropLevel = bdLevel_;
    v.backdropMirror = bdMirror_;
    v.backdropFit = engine::videoFitName(bdFit_);
    v.backdropOpacity = bdOpacity_;
    v.backdropWidth = bdSrc_ && bdSrc_->ready() ? bdSrc_->width() : 0;
    v.backdropHeight = bdSrc_ && bdSrc_->ready() ? bdSrc_->height() : 0;
    v.bodyTrack = body_.track;
    v.bodyQuality = body_.fast ? "fast" : "balanced";
    v.bodyFront = body_.front;
    v.bodyBurn = engine::bodyBurnName(body_.burn);
    v.bodyBurnAmount = body_.burnAmount;
    v.bodyTrails = body_.trails;
    v.bodyTrailWidth = body_.trailWidth;
    v.bodyShow = body_.show;
    v.bodySync = body_.sync;
    v.bodyRunning = tracker_ != nullptr;
    v.bodyPresence = tracker_ ? pose_.presence() : 0.0;
    v.bodyJoints = 0;
    if (tracker_) {
        for (int j = 0; j < engine::kPoseJoints; ++j) v.bodyJoints += pose_.joint(j).weight > 0.5 ? 1 : 0;
        const video::BodyTrackerStats st = tracker_->stats();
        v.bodyFps = st.fps;
        v.bodyMs = st.msAvg;
        v.bodyMaskMs = st.maskMsAvg;
        v.bodyPoseMs = st.poseMsAvg;
        v.bodyCamFps = bdCamFps_;
        v.bodyError = st.error;
        v.bodyMask = maskCur_ >= 0;
    }
    const sync::BeatClockState& bc = beatClock_.state();
    v.beatSource = sync::beatSourceName(bc.source);
    v.beatConfident = bc.confident;
    v.beatBpm = bc.bpm;
    v.beatOffsetMs = beatOffsetMs();
    v.beatLock = bc.lock;
    v.beatSprites.clear();
    for (const LayerSlot* l : {&vlA_, &vlB_})
        if (l->beatSync) {
            char buf[160];
            std::snprintf(buf, sizeof buf, "layer %s: %s, rate %.3fx, x%.1f, phase %+.3f beat", l->src->label().c_str(),
                          l->beatSync->synced() ? "synced" : "free", l->beatSync->rate(), l->beatSync->ratio(),
                          l->beatSync->phaseError());
            v.beatSprites.push_back(buf);
        }
    for (const auto& s : sprites_)
        if (s.beatSync) {
            char buf[160];
            std::snprintf(buf, sizeof buf, "%s: %s, rate %.3fx, x%.1f, phase %+.3f beat", s.run->name().c_str(),
                          s.beatSync->synced() ? "synced" : "free", s.beatSync->rate(), s.beatSync->ratio(),
                          s.beatSync->phaseError());
            v.beatSprites.push_back(buf);
        }
    v.lastError = videoError_;
}

}  // namespace reforce::app
