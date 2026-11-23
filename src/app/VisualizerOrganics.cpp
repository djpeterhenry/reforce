// Visualizer: organics, procedural growth (PLAN 5N).
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

// ---------------------------------------------------------------- 3D solids (PLAN 5L)

bool Visualizer::startOrganic(std::shared_ptr<const scene::CompiledConfig> cfg, const std::string& label, double t,
                              const FadeSpec& fade) {
    if (!cfg) return false;
    std::string written;
    const auto kind = engine::organicKind(*cfg, &written);
    if (!kind) {
        rlog("[reforce] Organic \"%s\": unknown Kind \"%s\" (tree, pendulums, phyllotaxis, harmonograph, automaton, "
             "life, lsystem, attractor, ripples, julia)\n", label.c_str(), written.c_str());
        return false;
    }
    if (*kind == engine::OrganicKind::Field && !fields_) {
        rlog("[reforce] Organic \"%s\": ripples / Julia / Mandelbrot sets are unavailable\n", label.c_str());
        return false;
    }
    makeRoomForObject(t, fade.fade, label);
    const int cap = std::max(1, opts_.prefs.objectMaxLive);
    // At most MaxLive + 1 organics live in all (fading ones included): that is the field slot count.
    while (int(organics_.size()) > cap) {
        auto it = std::find_if(organics_.begin(), organics_.end(), [&](const auto& o) { return o->fadingOut(t); });
        if (it == organics_.end()) it = organics_.begin();
        rlog("[reforce] Organic \"%s\" removed for \"%s\" (reforce.Object.MaxLive %d)\n", (*it)->name().c_str(),
             label.c_str(), cap);
        organics_.erase(it);
    }
    perf::tag(perf::TagOrganicStart);
    scene::Environment env = environment(t);
    env.seed = opts_.seed + 0x1000 * ++loads_;
    auto run = std::make_unique<engine::OrganicRun>(std::move(cfg), env, label, t, fade.fade, fade.defaultHold,
                                                    fade.fn, opts_.prefs.organicMaxNodes);
    run->setHold(engine::holdForBars(run->bars(), fade.fade, syncInput(), phrase_.state().bars,
                                     phrase_.state().quantum));  // PBar
    if (run->kind() == engine::OrganicKind::Field) {
        // A free slot: there is one, since organics_ holds at most MaxLive now (MaxLive + 1 slots).
        for (int slot = 0; slot < fields_->maxLive() && run->fieldSlot() < 0; ++slot)
            if (std::none_of(organics_.begin(), organics_.end(), [&](const auto& o) { return o->fieldSlot() == slot; }))
                run->setFieldSlot(slot);
        if (run->fieldSlot() < 0) {
            rlog("[reforce] Organic \"%s\": no free field slot\n", label.c_str());
            return false;
        }
    }
    const auto& e = run->envelope();
    rlog("[reforce] Organic \"%s\" starts: %s, %s, fade %.2f s, hold %.2f s\n", label.c_str(),
         run->kindName().c_str(), run->summary().c_str(), e.fade, e.hold);
    organics_.push_back(std::move(run));
    return true;
}

void Visualizer::killOrganics(double t, double fade) {
    if (organics_.empty()) return;
    perf::tag(perf::TagKill);
    rlog("[reforce] %zu organic(s) killed%s\n", organics_.size(), fade > 0 ? ": fading out" : "");
    if (!(fade > 0)) {
        organics_.clear();
        return;
    }
    for (auto& o : organics_) o->fadeOutFrom(t, fade);
}

int Visualizer::activeObjects(double t) const {
    const auto s = std::count_if(liveSolids_.begin(), liveSolids_.end(), [&](const auto& x) { return !x.run->fadingOut(t); });
    const auto o = std::count_if(organics_.begin(), organics_.end(), [&](const auto& x) { return !x->fadingOut(t); });
    return int(s + o);
}

void Visualizer::makeRoomForObject(double t, double fade, const std::string& label) {
    // reforce.Object.MaxLive counts solids and organics at full strength together: the oldest of them (by start)
    // fades out under the new one, over its fade-in.
    const int cap = std::max(1, opts_.prefs.objectMaxLive);
    for (int guard = 0; guard < 64 && activeObjects(t) >= cap; ++guard) {
        engine::SolidRun* solid = nullptr;
        engine::OrganicRun* organic = nullptr;
        double oldest = 1e300;
        for (auto& s : liveSolids_)
            if (!s.run->fadingOut(t) && s.run->startTime() < oldest) {
                oldest = s.run->startTime();
                solid = s.run.get();
                organic = nullptr;
            }
        for (auto& o : organics_)
            if (!o->fadingOut(t) && o->startTime() < oldest) {
                oldest = o->startTime();
                organic = o.get();
                solid = nullptr;
            }
        if (!solid && !organic) break;
        const std::string& name = solid ? solid->name() : organic->name();
        rlog("[reforce] Object \"%s\" makes way for \"%s\": fading out over %.2f s (reforce.Object.MaxLive %d)\n",
             name.c_str(), label.c_str(), fade, cap);
        if (solid) solid->fadeOutFrom(t, fade);
        else organic->fadeOutFrom(t, fade);
    }
}

bool Visualizer::spawnRoom(engine::Layer layer, double t) const {
    if (layer == Layer::Object) return activeObjects(t) < std::max(1, opts_.prefs.objectMaxLive);
    return true;
}

void Visualizer::killObjects(double t, double fade) {
    killSolids(t, fade);
    killOrganics(t, fade);
}

std::vector<std::string> Visualizer::objectNames() const {
    std::vector<std::string> n = solidNames();
    for (auto& o : organicNames()) n.push_back(std::move(o));
    return n;
}

std::vector<std::string> Visualizer::organicNames() const {
    std::vector<std::string> n;
    for (const auto& o : organics_) n.push_back(o->name());
    return n;
}

void Visualizer::stepOrganics(double t, const audio::AudioFrame* audio) {
    for (auto it = organics_.begin(); it != organics_.end();) {
        if ((*it)->expired(t)) {
            rlog("[reforce] Organic \"%s\" ended (%s)\n", (*it)->name().c_str(), (*it)->summary().c_str());
            it = organics_.erase(it);
        } else {
            ++it;
        }
    }
    if (organics_.empty()) return;
    const engine::SpriteAudio a = spriteAudio(audio);
    const engine::FrameMapping map = engine::organicMapping(double(opts_.width), double(opts_.height));
    // After the WaveShape's and the particles' segments: drawn on top, overwrite (probe #15).
    for (auto& o : organics_) o->step(t, a, map, lineStyle_, segments_);
}

}  // namespace reforce::app
