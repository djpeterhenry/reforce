#include "engine/Solids.hpp"

#include <algorithm>
#include <cmath>

namespace reforce::engine {

std::optional<MeshKind> solidMesh(const scene::CompiledConfig& cfg, std::string* written) {
    std::string name;
    if (const scene::CompiledKey* k = cfg.find("MESH")) name = k->text;
    const auto b = name.find_first_not_of(" \t\r\n");
    const auto e = name.find_last_not_of(" \t\r\n");
    name = b == std::string::npos ? std::string() : name.substr(b, e - b + 1);
    if (written) *written = name;
    if (name.empty()) return MeshKind::Icosahedron;
    return parseMeshKind(name);
}

SolidRun::SolidRun(std::shared_ptr<const scene::CompiledConfig> cfg, const scene::Environment& env,
                   std::string name, MeshKind mesh, double startTime, double fade, double defaultHold,
                   std::shared_ptr<const TransitionFunction> fn)
    : cfg_(std::move(cfg)), name_(std::move(name)), mesh_(mesh), start_(startTime) {
    scene::Environment e = env;
    e.loadTime = startTime;
    inst_ = std::make_unique<scene::SolidInstance>(*cfg_, e);
    double hold = inst_->duration();
    if (!(std::isfinite(hold) && hold >= 0)) hold = defaultHold;
    env_.fade = std::max(0.0, fade);
    env_.hold = std::min(hold, 24.0 * 3600.0);
    env_.fn = std::move(fn);
}

SolidSpec SolidRun::evaluate(double t, const SpriteAudio& audio) {
    scene::SolidFrameInput in;
    in.t = t;
    in.startTime = start_;
    in.endTime = endTime();
    in.dt = in.endTime > start_ ? std::clamp((t - start_) / (in.endTime - start_), 0.0, 1.0) : 0.0;
    in.fader = env_(t - start_);
    in.bass = audio.bass;
    in.level = audio.level;
    in.energy = audio.energy;
    in.mid = audio.mid;
    in.treble = audio.treble;
    in.beat = audio.beat;
    in.tempo = audio.tempo;
    in.linkOn = audio.linkOn;
    in.linkBpm = audio.linkBpm;
    in.linkBeat = audio.linkBeat;
    in.linkPhase = audio.linkPhase;
    in.sync = audio.sync;
    const scene::SolidFrame f = inst_->evaluateFrame(in);
    auto finite = [](double v, double fb) { return std::isfinite(v) ? v : fb; };
    SolidSpec s;
    s.mesh = mesh_;
    s.pose.x = finite(f.x, 0.0);
    s.pose.y = finite(f.y, 0.0);
    s.pose.z = finite(f.z, 0.0);
    s.pose.scale = std::max(0.0, finite(f.scale, 0.5));
    s.pose.rotX = finite(f.rotX, 0.0);
    s.pose.rotY = finite(f.rotY, 0.0);
    s.pose.rotZ = finite(f.rotZ, 0.0);
    s.perspective = inst_->perspective();
    s.text = inst_->text();
    s.fill = std::clamp(finite(f.fill, 0.15), 0.0, 1.0);
    s.shade = std::clamp(finite(f.shade, 0.6), 0.0, 1.0);
    s.edge = std::clamp(finite(f.edge, 0.8), 0.0, 1.0);
    s.hasPen = f.hasPen;
    s.hasEPen = f.hasEPen;
    s.pen = std::clamp(finite(f.pen, 0.0), 0.0, 1.0);
    s.epen = std::clamp(finite(f.epen, 0.0), 0.0, 1.0);
    s.opacity = in.fader;
    s.flags = inst_->flags();
    s.colorKey = inst_->colorKey();
    s.over = inst_->over();
    s.moves = inst_->moves();
    s.turn = finite(f.turn, 2.0 * s.moves);
    s.seed = finite(f.seed, 0.0);
    s.shuffle = finite(f.shuffle, 1.0);
    s.glass = inst_->glass();
    s.frost = std::clamp(finite(f.frost, 12.0), 0.0, 60.0);
    s.tint = std::clamp(finite(f.tint, 0.35), 0.0, 1.0);
    s.bend = std::clamp(finite(f.bend, 1.0), 0.0, 4.0);
    return s;
}

}  // namespace reforce::engine
