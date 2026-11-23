// Visualizer: the G-Force layers: FlowField (built on workers, transitions), WaveShape (morphs), ColorMap, Particles.
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

void Visualizer::setBlendMode(Layer layer, int mode) {
    const int s = modeSlot(layer);
    if (s >= 0 && mode >= 0 && std::size_t(mode) < engine::blendModeNames(layer).size()) modes_[s] = mode;
}

int Visualizer::blendMode(Layer layer) const {
    const int s = modeSlot(layer);
    return s >= 0 ? modes_[s].load() : 0;
}

// ---------------------------------------------------------------- FlowField

Visualizer::FieldBuild Visualizer::startFieldBuild(std::shared_ptr<const scene::CompiledConfig> cfg,
                                                   const std::string& label, double t) {
    FieldBuild b;
    b.label = label;
    b.cancel = std::make_shared<std::atomic<bool>>(false);
    engine::FlowFieldBuildOptions bo = buildOptions();
    bo.cancel = b.cancel.get();
    scene::Environment env = environment(t);
    env.seed = opts_.seed + 0x1000 * ++loads_;
    // The cancel flag must outlive the worker: capture the shared_ptr. The
    // worker also uploads the grid into a pooled texture.
    auto cancel = b.cancel;
    auto pool = pipeline_->fieldPool();
    const std::size_t rowBytes = std::size_t(opts_.width) * 8;
    b.future = std::async(std::launch::async, [cfg = std::move(cfg), env, bo, cancel, pool, rowBytes]() {
        // Below the render thread's QoS (its helper threads inherit this).
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
        engine::FlowFieldGrid grid = engine::buildFlowField(*cfg, env, bo);
        GpuField f;
        f.buildSeconds = grid.buildSeconds;
        f.polar = grid.polar;
        f.float32 = env.flowFloat32;
        if (!cancel->load() && grid.srcXY.size() == std::size_t(bo.width) * std::size_t(bo.height) * 2)
            f.texture = pool->upload(grid.srcXY.data(), rowBytes, false);
        return f;
    });
    if (opts_.syncBuilds) {
        b.ready = std::make_shared<GpuField>(b.future.get());
    }
    return b;
}

void Visualizer::cancelBuild(FieldBuild& b) {
    if (b.cancel) b.cancel->store(true);
    if (b.future.valid()) retiredFields_.push_back(std::move(b.future));
    b = FieldBuild{};
}

void Visualizer::cancelPending() {
    if (!ffPending_) return;
    cancelBuild(ffPending_->field);
    if (ffPending_->maskCancel) ffPending_->maskCancel->store(true);
    if (ffPending_->mask.valid()) retiredMasks_.push_back(std::move(ffPending_->mask));
    ffPending_.reset();
}

void Visualizer::reapRetired() {
    auto ready = [](const auto& f) { return f.wait_for(std::chrono::seconds(0)) == std::future_status::ready; };
    std::erase_if(retiredFields_, ready);
    std::erase_if(retiredMasks_, ready);
}

void Visualizer::preloadFlowField(std::shared_ptr<const scene::CompiledConfig> cfg, const std::string& label,
                                  double t) {
    if (!cfg) return;
    if (preloadLabel_ == label && (preload_.ready || preload_.future.valid())) return;
    cancelBuild(preload_);
    preload_ = startFieldBuild(std::move(cfg), label, t);
    preloadLabel_ = label;
}

void Visualizer::switchFlowField(std::shared_ptr<const scene::CompiledConfig> cfg, const std::string& label,
                                 double t, const TransitionSpec& spec,
                                 std::shared_ptr<const scene::CompiledConfig> unionCfg, const std::string& unionLabel) {
    if (!cfg) return;
    perf::tag(perf::TagSwitchField);
    cancelPending();
    PendingField p;
    if (preloadLabel_ == label && (preload_.ready || preload_.future.valid())) {
        p.field = std::move(preload_);
        preload_ = FieldBuild{};
        preloadLabel_.clear();
    } else {
        p.field = startFieldBuild(std::move(cfg), label, t);
    }
    p.spec = spec;
    if (unionCfg) {
        p.hasMask = true;
        p.unionLabel = unionLabel;
        p.maskCancel = std::make_shared<std::atomic<bool>>(false);
        engine::FlowFieldBuildOptions bo = buildOptions();
        bo.cancel = p.maskCancel.get();
        scene::Environment env = environment(t);
        env.seed = opts_.seed + 0x1000 * ++loads_;
        auto cancel = p.maskCancel;
        auto pool = pipeline_->maskPool();
        const std::size_t rowBytes = std::size_t(opts_.width) * 4;
        p.mask = std::async(std::launch::async, [cfg = std::move(unionCfg), env, bo, cancel, pool, rowBytes]() {
            pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
            const engine::UnionMask m = engine::buildUnionMask(*cfg, env, bo);
            render::TextureHandle h;
            if (!cancel->load() && m.alpha.size() == std::size_t(bo.width) * std::size_t(bo.height))
                h = pool->upload(m.alpha.data(), rowBytes, false);
            return h;
        });
    }
    ffPending_ = std::move(p);
    if (opts_.syncBuilds) waitForField(t);
}

bool Visualizer::pendingReady(bool block) {
    if (!ffPending_) return false;
    FieldBuild& f = ffPending_->field;
    if (!f.ready) {
        if (!block && !futureReady(f.future)) return false;
        f.ready = std::make_shared<GpuField>(f.future.get());
    }
    if (ffPending_->hasMask && !block && !futureReady(ffPending_->mask)) return false;
    return true;
}

void Visualizer::waitForField(double t) {
    if (pendingReady(true)) beginFieldTransition(t);
}

void Visualizer::beginFieldTransition(double t) {
    perf::tag(perf::TagFieldBegin);
    PendingField p = std::move(*ffPending_);
    ffPending_.reset();
    std::shared_ptr<const GpuField> grid = p.field.ready;
    render::TextureHandle mask;
    if (p.hasMask) mask = p.mask.get();
    if (!grid->texture) {
        rlog("[reforce] FlowField \"%s\": build failed or canceled; ignored\n", p.field.label.c_str());
        return;
    }
    lastFieldBuild_ = grid->buildSeconds;
    rlog("[reforce] FlowField \"%s\" ready (built in %.0f ms%s)\n", p.field.label.c_str(),
                 grid->buildSeconds * 1000.0, grid->polar ? ", polar" : "");

    const bool cut = ffName_.empty() || p.spec.duration <= 0;
    ffHeldUnion_.clear();
    if (ffTrans_) {
        // Interruption: the in-progress blend becomes the new "from" (baked on
        // the GPU before the next step's flow pass).
        const double w = ffTrans_->clock.weight(t);
        perf::tag(perf::TagFieldBake);
        const auto t0 = Clock::now();
        ffCur_ = std::make_shared<GpuField>(
            GpuField{pipeline_->bakeField(float(w), ffTrans_->mode, ffTrans_->mask != nullptr), 0, false});
        rlog("[reforce] FlowField transition to \"%s\" interrupted at w=%.3f (GPU bake queued in %.2f ms)\n",
             ffTrans_->toName.c_str(), w, msSince(t0));
        ffName_ = ffName_ + " > " + ffTrans_->toName;
        ffTrans_.reset();
    }
    if (cut) {
        ffCur_ = grid;
        pipeline_->setField(grid->texture);
        pipeline_->setIncomingField(nullptr);
        pipeline_->setUnionMask(nullptr);
        ffName_ = p.field.label;
        ffPendingHold_.reset();
        return;
    }
    FieldTransition tr;
    tr.to = grid;
    tr.mask = mask;
    tr.clock = engine::TransitionClock(t, p.spec.duration, p.spec.fn);
    tr.mode = modes_[2];
    tr.toName = p.field.label;
    tr.unionName = p.hasMask ? p.unionLabel : std::string();
    if (ffPendingHold_) {
        tr.clock.hold(*ffPendingHold_);
        ffPendingHold_.reset();
    }
    pipeline_->setIncomingField(grid->texture);
    pipeline_->setUnionMask(mask);
    rlog("[reforce] FlowField \"%s\" -> \"%s\" over %.2f s (%s%s%s)\n", ffName_.c_str(),
                 tr.toName.c_str(), p.spec.duration, engine::blendModeName(Layer::FlowField, tr.mode).c_str(),
                 tr.unionName.empty() ? "" : ", union ", tr.unionName.c_str());
    ffTrans_ = std::move(tr);
}

void Visualizer::stepFlowField(double t) {
    if (!retiredFields_.empty() || !retiredMasks_.empty()) reapRetired();
    if (ffPending_ && pendingReady(false)) beginFieldTransition(t);
    if (ffTrans_ && ffTrans_->clock.done(t)) {
        perf::tag(perf::TagFieldEnd);
        if (ffTrans_->mask) {
            // A Union's blend stays: the field is the outgoing and incoming fields mixed by ALFA (weight 1 x ALFA)
            // until the next FlowField transition, which morphs from it (probe #45, runs 41-43; #18's "the new
            // field applies everywhere" was a decay artifact).
            ffCur_ = std::make_shared<GpuField>(GpuField{pipeline_->bakeField(1.0f, ffTrans_->mode, true), 0, false});
            ffHeldUnion_ = ffTrans_->unionName;
            ffName_ = ffTrans_->toName;
            rlog("[reforce] FlowField now \"%s\" blended with the previous field by union \"%s\"\n", ffName_.c_str(),
                 ffHeldUnion_.c_str());
            ffTrans_.reset();
            return;
        } else {
            ffCur_ = ffTrans_->to;
            pipeline_->promoteIncomingField();
            pipeline_->setUnionMask(nullptr);
            ffName_ = ffTrans_->toName;
        }
        rlog("[reforce] FlowField now \"%s\"\n", ffName_.c_str());
        ffTrans_.reset();
    }
}

bool Visualizer::loadFlowField(const std::filesystem::path& path, double t) {
    auto cfg = compileFile(path, scene::SceneType::FlowField);
    if (!cfg) return false;
    switchFlowField(std::move(cfg), path.stem().string(), t, {});
    return true;
}

bool Visualizer::loadFlowFieldText(const std::string& text, const std::string& label, double t) {
    switchFlowField(compileText(text, label, scene::SceneType::FlowField), label, t, {});
    return true;
}

// ---------------------------------------------------------------- WaveShape

void Visualizer::switchWaveShape(std::shared_ptr<const scene::CompiledConfig> cfg, const std::string& label,
                                 double t, const TransitionSpec& spec) {
    if (!cfg) return;
    perf::tag(perf::TagSwitchWave);
    auto node = std::make_unique<WaveNode>();
    scene::Environment env = environment(t);
    env.seed = opts_.seed + 0x1000 * ++loads_;
    env.loadTime = 0;  // config-local t (probe #37): A vars see t = 0, frames see t - load time
    node->inst = std::make_unique<scene::WaveShapeInstance>(*cfg, env);
    node->t0 = t;
    node->inst->setPool(evalPool_.get());
    node->pool = evalPool_.get();
    node->cfg = std::move(cfg);
    node->name = label;
    rlog("[reforce] WaveShape \"%s\": %d instance(s) x %zu path(s) x %d steps, %s\n", label.c_str(),
                 node->inst->numInstances(), node->inst->pathIndices().size(), node->inst->numSteps(),
                 node->inst->connected() ? "lines" : "dots");

    if (!wsCur_ || spec.duration <= 0) {
        wsCur_ = std::move(node);
        wsTo_.reset();
        wsName_ = label;
        return;
    }
    if (wsTo_) {
        // Interruption: keep the composite (both live, frozen weight) as "from".
        // Depth cap 2: an interrupted composite first snaps to its target.
        if (!wsCur_->leaf()) wsCur_ = std::move(wsCur_->b);
        auto comp = std::make_unique<WaveNode>();
        comp->pool = evalPool_.get();
        comp->w = wsClock_.weight(t);
        comp->mode = wsTransMode_;
        comp->cache = std::move(wsCache_);
        comp->a = std::move(wsCur_);
        comp->b = std::move(wsTo_);
        rlog("[reforce] WaveShape morph to \"%s\" interrupted at w=%.3f\n", comp->b->name.c_str(),
                     comp->w);
        wsCur_ = std::move(comp);
    }
    wsTo_ = std::move(node);
    wsClock_ = engine::TransitionClock(t, spec.duration, spec.fn);
    wsTransMode_ = modes_[0];
    wsCache_ = {};
    rlog("[reforce] WaveShape \"%s\" -> \"%s\" over %.2f s (%s)\n", wsCur_->label().c_str(),
                 label.c_str(), spec.duration, engine::blendModeName(Layer::WaveShape, wsTransMode_).c_str());
}

void Visualizer::stepWaveShape(double t, const audio::AudioFrame* audio) {
    auto t0 = Clock::now();
    segments_.clear();
    wsOutValid_ = false;
    if (wsTo_ && wsClock_.done(t)) {
        perf::tag(perf::TagMorphEnd);
        wsCur_ = std::move(wsTo_);
        wsName_ = wsCur_->name;
        rlog("[reforce] WaveShape now \"%s\"\n", wsName_.c_str());
    }
    if (!wsCur_) return;
    scene::WaveShapeFrameInput in;
    in.t = t;
    if (audio) {
        in.audio.wave = audio->wave.data();
        in.audio.waveCount = int(audio->wave.size());
        in.audio.fft = audio->fft.data();
        in.audio.fftCount = int(audio->fft.size());
        in.bass = audio->bass;
        in.level = audio->level;
        in.energy = audio->energy;
        in.mid = audio->mid;
        in.treble = audio->treble;
        in.beat = audio->beat;
        in.tempo = audio->tempo;
        in.linkOn = audio->linkOn ? 1.0 : 0.0;
        in.linkBpm = audio->linkBpm;
        in.linkBeat = audio->linkBeat;
        in.linkPhase = audio->linkPhase;
    }
    in.sync = syncInput();
    if (wsTo_) {
        // Both configs evaluated every frame: both stay audio-reactive.
        wsCur_->evaluate(in, wsFrom_);
        wsTo_->evaluate(in, wsToFrame_);
        engine::morphFrames(wsTransMode_, wsFrom_, wsToFrame_, wsClock_.weight(t), wsOut_, &wsCache_,
                            evalPool_.get());
    } else {
        wsCur_->evaluate(in, wsOut_);
    }
    {
        auto e = wsCur_->extents();
        if (wsTo_) {
            const auto eb = wsTo_->extents();
            const double w = wsClock_.weight(t);
            e = {e.first + (eb.first - e.first) * w, e.second + (eb.second - e.second) * w};
        }
        wsExtents_ = e;
    }
    timings_.waveEvalMs = msSince(t0);
    t0 = Clock::now();
    engine::FrameMapping map{double(opts_.width), double(opts_.height), 1.0, 1.0};
    engine::appendSegments(wsOut_, map, lineStyle_, segments_);
    wsOutValid_ = true;
    timings_.geometryMs = msSince(t0);
}

bool Visualizer::loadWaveShape(const std::filesystem::path& path, double t) {
    auto cfg = compileFile(path, scene::SceneType::WaveShape);
    if (!cfg) return false;
    switchWaveShape(std::move(cfg), path.stem().string(), t, {});
    return true;
}

bool Visualizer::loadWaveShapeText(const std::string& text, const std::string& label, double t) {
    switchWaveShape(compileText(text, label, scene::SceneType::WaveShape), label, t, {});
    return true;
}

// ---------------------------------------------------------------- ColorMap

void Visualizer::switchColorMapSource(std::unique_ptr<engine::ColorMapSource> src, const std::string& label, double t,
                                      const TransitionSpec& spec) {
    perf::tag(perf::TagSwitchColor);
    if (!cmCur_ || spec.duration <= 0) {
        cmCur_ = std::move(src);
        cmTo_.reset();
        cmName_ = label;
        cmCur_->update(t, palCur_);
        palette_ = palCur_;
        return;
    }
    if (cmTo_) {
        // Interruption: bake the blended palette as the new "from".
        rlog("[reforce] ColorMap morph to \"%s\" interrupted at w=%.3f\n", cmToName_.c_str(),
                     cmClock_.weight(t));
        cmCur_ = std::make_unique<engine::ColorMapSource>(palette_);
        palCur_ = palette_;
        cmCur_->update(t, palCur_);
        cmName_ = cmName_ + " > " + cmToName_;
    }
    cmTo_ = std::move(src);
    cmTo_->update(t, palTo_);
    cmToName_ = label;
    cmClock_ = engine::TransitionClock(t, spec.duration, spec.fn);
    cmTransMode_ = modes_[1];
    rlog("[reforce] ColorMap \"%s\" -> \"%s\" over %.2f s (%s)\n", cmName_.c_str(), label.c_str(),
                 spec.duration, engine::blendModeName(Layer::ColorMap, cmTransMode_).c_str());
}

void Visualizer::switchColorMap(std::shared_ptr<const scene::CompiledConfig> cfg, const std::string& label, double t,
                                const TransitionSpec& spec) {
    if (!cfg) return;
    scene::Environment env = environment(t);
    env.seed = opts_.seed + 0x1000 * ++loads_;
    auto src = std::make_unique<engine::ColorMapSource>(std::move(cfg), env);
    switchColorMapSource(std::move(src), label, t, spec);
}

void Visualizer::switchColorMapPalette(const engine::Palette& palette, const std::string& label, double t,
                                       const TransitionSpec& spec) {
    switchColorMapSource(std::make_unique<engine::ColorMapSource>(palette), label, t, spec);
}

bool Visualizer::switchColorMapFile(const std::filesystem::path& path, const std::string& label, double t,
                                    const TransitionSpec& spec) {
    if (lowerExt(path) == ".png") {
        const std::shared_ptr<const engine::Palette> pal = pngPalette(path);
        if (!pal) {
            rlog("[reforce] cannot decode %s\n", path.string().c_str());
            return false;
        }
        switchColorMapPalette(*pal, label, t, spec);
        return true;
    }
    auto cfg = compileFile(path, scene::SceneType::ColorMap);
    if (!cfg) return false;
    switchColorMap(std::move(cfg), label, t, spec);
    return true;
}

void Visualizer::stepColorMap(double t) {
    if (cmTo_ && cmClock_.done(t)) {
        perf::tag(perf::TagMorphEnd);
        cmCur_ = std::move(cmTo_);
        palCur_ = palTo_;
        cmName_ = cmToName_;
        rlog("[reforce] ColorMap now \"%s\"\n", cmName_.c_str());
    }
    if (cmCur_) cmCur_->update(t, palCur_);
    if (cmTo_) {
        cmTo_->update(t, palTo_);
        engine::blendPalette(cmTransMode_, palCur_, palTo_, cmClock_.weight(t), palette_);
    } else if (cmCur_) {
        palette_ = palCur_;
    }
}

bool Visualizer::loadColorMap(const std::filesystem::path& path, double t) {
    return switchColorMapFile(path, path.stem().string(), t, {});
}

bool Visualizer::loadColorMapText(const std::string& text, const std::string& label, double t) {
    switchColorMap(compileText(text, label, scene::SceneType::ColorMap), label, t, {});
    return true;
}

// ---------------------------------------------------------------- particles

void Visualizer::startParticle(std::shared_ptr<const scene::CompiledConfig> cfg, const std::string& label, double t,
                               const FadeSpec& fade) {
    if (!cfg) return;
    perf::tag(perf::TagParticleStart);
    scene::Environment env = environment(t);
    env.seed = opts_.seed + 0x1000 * ++loads_;
    auto p = std::make_unique<engine::ParticleInstance>(cfg, env, label, t, fade.fade, fade.defaultHold, fade.fn);
    const auto& e = p->envelope();
    rlog("[reforce] Particle \"%s\" starts: %d instance(s) x %zu path(s) x %d steps, fade %.2f s, "
                 "hold %.2f s%s\n", label.c_str(), p->instance().numInstances(), p->instance().pathIndices().size(),
                 p->instance().numSteps(), e.fade, e.hold, cfg->defines("PDUR") ? " (PDur)" : "");
    particles_.spawn(std::move(p));
}

void Visualizer::killParticles(double t, double fade) {
    if (particles_.alive().empty()) return;
    perf::tag(perf::TagKill);
    rlog("[reforce] %zu particle(s) killed%s\n", particles_.alive().size(),
                 fade > 0 ? ": fading out" : "");
    particles_.fadeOutAll(t, fade);
}

void Visualizer::stepParticles(double t, const audio::AudioFrame* audio) {
    if (particles_.alive().empty()) return;
    scene::WaveShapeFrameInput in;
    in.t = t;
    if (audio) {
        in.audio.wave = audio->wave.data();
        in.audio.waveCount = int(audio->wave.size());
        in.audio.fft = audio->fft.data();
        in.audio.fftCount = int(audio->fft.size());
        in.bass = audio->bass;
        in.level = audio->level;
        in.energy = audio->energy;
        in.mid = audio->mid;
        in.treble = audio->treble;
        in.beat = audio->beat;
        in.tempo = audio->tempo;
        in.linkOn = audio->linkOn ? 1.0 : 0.0;
        in.linkBpm = audio->linkBpm;
        in.linkBeat = audio->linkBeat;
        in.linkPhase = audio->linkPhase;
    }
    in.sync = syncInput();
    const std::size_t before = particles_.alive().size();
    engine::FrameMapping map{double(opts_.width), double(opts_.height), 1.0, 1.0};
    // Appended after the WaveShape's segments: drawn on top, overwrite (probe #15).
    particles_.step(in, map, lineStyle_, segments_);
    if (particles_.alive().size() < before)
        rlog("[reforce] %zu particle(s) ended\n", before - particles_.alive().size());
}

// GPU organics: simulated / drawn into their slots on `cmd`, composited after this step's sprites (like solids).
void Visualizer::stepFields(MTL::CommandBuffer* cmd) {
    if (!fields_) return;
    fieldFeedback_.clear();
    fieldDisplay_.clear();
    for (const auto& o : organics_) {
        const engine::OrganicField* f = o->field();
        if (!f || o->fieldSlot() < 0) continue;
        const render::SolidQuad sq = fields_->encode(cmd, o->fieldSlot(), *f);
        if (!sq.visible || !(f->pen > 0) || !(f->opacity > 0)) continue;
        engine::SpriteDrawSpec ds;
        ds.hasPen = true;
        ds.pen = f->pen;
        ds.opacity = f->opacity;
        ds.blend = engine::spriteBlendMode(f->flags);
        addSolidQuads(sq, ds, fieldFeedback_, fieldDisplay_);
    }
    if (engine::spriteOldestOnTop(spriteLayering_)) {
        feedbackQuads_.insert(feedbackQuads_.end(), fieldFeedback_.rbegin(), fieldFeedback_.rend());
        displayQuads_.insert(displayQuads_.end(), fieldDisplay_.rbegin(), fieldDisplay_.rend());
    } else {
        feedbackQuads_.insert(feedbackQuads_.end(), fieldFeedback_.begin(), fieldFeedback_.end());
        displayQuads_.insert(displayQuads_.end(), fieldDisplay_.begin(), fieldDisplay_.end());
    }
}

}  // namespace reforce::app
