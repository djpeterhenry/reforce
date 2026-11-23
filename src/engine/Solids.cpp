#include "engine/Solids.hpp"

#include "engine/ModelFile.hpp"

#include <algorithm>
#include <cctype>
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
    if (mesh_ == MeshKind::Model) {
        model_.file = inst_->literal("FILE");
        const double look = inst_->loaded("LOOK", 0.0);
        model_.look = std::isfinite(look) && look >= 0.5 ? 1 : 0;
        if (const std::string n = inst_->literal("FRONT"); !n.empty()) {
            if (auto a = parseModelAxis(n)) model_.front = *a;
            else model_.error += "unknown Front \"" + n + "\"; ";
        }
        if (const std::string u = inst_->literal("UP"); !u.empty()) {
            if (auto a = parseModelAxis(u)) model_.up = *a;
            else model_.error += "unknown Up \"" + u + "\"; ";
        }
        if (std::string m = inst_->literal("MOVE"); !m.empty()) {
            std::transform(m.begin(), m.end(), m.begin(), [](char c) { return char(std::tolower(static_cast<unsigned char>(c))); });
            model_.move = m;
        }
        model_.freeCamera = model_.move == "free";
        // The scene's own keyframes (K1, K2, ...) win over the Move preset.
        model_.path = cameraPathFrom([&](const char* key) { return inst_->literal(key); }, &model_.error);
        model_.ownPath = !model_.path.empty();
        const double mdur = inst_->loaded("MDUR", 24.0);
        if (std::isfinite(mdur) && mdur > 0.1) model_.moveSeconds = mdur;
        model_.moveBars = inst_->loaded("MBAR", std::nan(""));
        model_.loop = inst_->loaded("LOOP", 1.0) != 0.0;
        model_.trails = inst_->defines("PEN");
    }
}

void SolidRun::setCameraPath(CameraPath p) {
    if (model_.ownPath) return;
    // The scene's own options win over the preset's.
    auto own = [&](const char* key) { return !inst_->literal(key).empty(); };
    const CameraPath scene = model_.path;
    if (own("EASE")) p.ease = scene.ease;
    if (own("STEP")) p.step = scene.step;
    if (own("RAND")) p.random = scene.random;
    if (own("PUSH")) p.push = scene.push;
    model_.path = std::move(p);
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
    if (mesh_ == MeshKind::Model) {
        const double age = std::max(0.0, t - start_);
        double p = age / model_.moveSeconds;
        p = model_.loop ? p - std::floor(p) : std::min(p, 1.0);
        CameraMoveParams cp;
        cp.dist = std::max(0.05, finite(inst_->frameKey("DIST", 1.0), 1.0));
        cp.height = finite(inst_->frameKey("HITE", 0.0), 0.0);
        cp.side = finite(inst_->frameKey("SIDE", 1.0), 1.0) < 0 ? -1.0 : 1.0;
        cp.roll = finite(inst_->frameKey("ROLL", 0.0), 0.0);
        cp.fov = finite(inst_->frameKey("FOV", 0.0), 0.0);
        cp.seed = s.seed;
        cp.cutBeats = std::max(0.25, finite(inst_->frameKey("CUTB", 4.0), 4.0));
        cp.beat = audio.sync.on > 0 ? audio.sync.beat : age * 2.0;  // no clock: 120 BPM from the start
        ModelShot shot = pathShot(model_.freeCamera ? CameraPath{} : model_.path, p, cp);
        const char* cam[3] = {"CAMX", "CAMY", "CAMZ"};
        const char* aim[3] = {"AIMX", "AIMY", "AIMZ"};
        for (int k = 0; k < 3; ++k) {
            const std::size_t j = std::size_t(k);
            // Free: CamX..AimZ are the camera; other moves: offsets added to the path.
            const double c = finite(inst_->frameKey(cam[k], model_.freeCamera ? shot.eye[j] : 0.0), 0.0);
            const double a = finite(inst_->frameKey(aim[k], model_.freeCamera ? shot.aim[j] : 0.0), 0.0);
            shot.eye[j] = model_.freeCamera ? c : shot.eye[j] + c;
            shot.aim[j] = model_.freeCamera ? a : shot.aim[j] + a;
        }
        s.shot = shot;
        s.spin = finite(inst_->frameKey("SPIN", 0.0), 0.0) * age;
        s.bank = finite(inst_->frameKey("BANK", 0.0), 0.0);
        s.glow = std::clamp(finite(inst_->frameKey("GLOW", 1.0), 1.0), 0.0, 20.0);
        s.bloom = std::clamp(finite(inst_->frameKey("BLOM", 0.6), 0.6), 0.0, 4.0);
        s.exposure = std::clamp(finite(inst_->frameKey("EXPO", 1.0), 1.0), 0.0, 16.0);
        s.tint = std::clamp(finite(inst_->frameKey("TINT", 0.0), 0.0), 0.0, 1.0);
        s.light = {finite(inst_->frameKey("LTX", -0.45), -0.45), finite(inst_->frameKey("LTY", 0.6), 0.6),
                   finite(inst_->frameKey("LTZ", 0.65), 0.65)};
    }
    return s;
}

}  // namespace reforce::engine
