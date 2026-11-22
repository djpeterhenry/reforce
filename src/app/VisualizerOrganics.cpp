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
        rlog("[reforce] Organic \"%s\": ripples / Julia sets are unavailable\n", label.c_str());
        return false;
    }
    const int maxLive = std::max(1, opts_.prefs.organicMaxLive);
    while (int(organics_.size()) >= maxLive) {
        rlog("[reforce] Organic \"%s\" makes way for \"%s\" (reforce.Organic.MaxLive %d)\n",
             organics_.front()->name().c_str(), label.c_str(), maxLive);
        organics_.erase(organics_.begin());
    }
    perf::tag(perf::TagOrganicStart);
    scene::Environment env = environment(t);
    env.seed = opts_.seed + 0x1000 * ++loads_;
    auto run = std::make_unique<engine::OrganicRun>(std::move(cfg), env, label, t, fade.fade, fade.defaultHold,
                                                    fade.fn, opts_.prefs.organicMaxNodes);
    if (run->kind() == engine::OrganicKind::Field) {
        // A free slot: there is one, since organics_ holds fewer than MaxLive (= the slot count) now.
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
