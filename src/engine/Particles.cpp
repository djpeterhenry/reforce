#include "engine/Particles.hpp"

#include <algorithm>
#include <cmath>

namespace reforce::engine {

double FadeEnvelope::total() const {
    if (earlyOut >= 0.0) return earlyOut + (1.0 - earlyX) * outFade();
    return 2.0 * fade + hold;
}

double FadeEnvelope::outFade() const { return earlyFade >= 0.0 ? earlyFade : fade; }

double FadeEnvelope::operator()(double age) const {
    if (!(age >= 0.0) || age >= total()) return 0.0;
    auto curve = [&](double x) { return fn ? (*fn)(x) : std::clamp(x, 0.0, 1.0); };
    if (earlyOut >= 0.0 && age >= earlyOut) {
        const double of = outFade();
        if (!(of > 0)) return 0.0;
        const double x = earlyX + (age - earlyOut) / of;
        return earlyLinear ? std::clamp(1.0 - x, 0.0, 1.0) : 1.0 - curve(x);
    }
    if (age < fade) return fade > 0 ? curve(age / fade) : 1.0;
    if (age < fade + hold) return 1.0;
    const double tau = age - fade - hold;
    return fade > 0 ? 1.0 - curve(tau / fade) : 0.0;
}

void FadeEnvelope::fadeOutAt(double age, double newFade, bool linear) {
    if (age < 0.0) age = 0.0;
    if (age >= total()) return;  // already over
    const double want = newFade >= 0.0 ? newFade : fade;
    const double cur = (*this)(age);
    const bool fadingOut = (earlyOut >= 0.0 && age >= earlyOut) || (earlyOut < 0.0 && age >= fade + hold);
    // Already fading out at least as fast: keep it.
    if (fadingOut && want >= (earlyOut >= 0.0 ? outFade() : fade)) return;
    double x = 1.0;
    if (cur > 0.0) x = (fn && !linear) ? fn->inverse(1.0 - cur) : 1.0 - cur;
    earlyOut = age;
    earlyX = std::clamp(x, 0.0, 1.0);
    earlyFade = want;
    earlyLinear = linear;
}

bool pollSpawn(LayerSlideshow& slideshow, double now, SchedRng& rng) {
    if (!slideshow.running()) return false;
    if (!(slideshow.interval() > 0)) {  // interval 0: none from the slideshow
        slideshow.disarm();
        return false;
    }
    if (!slideshow.armed()) {
        slideshow.arm(now, rng);
        return false;
    }
    if (!slideshow.due(now)) return false;
    slideshow.arm(now, rng);
    return true;
}

ParticleInstance::ParticleInstance(std::shared_ptr<const scene::CompiledConfig> cfg, const scene::Environment& env,
                                   std::string name, double startTime, double fade, double defaultHold,
                                   std::shared_ptr<const TransitionFunction> fn)
    : cfg_(std::move(cfg)), name_(std::move(name)), start_(startTime) {
    scene::Environment e = env;
    e.particleDuration = defaultHold;  // used when the config has no PDur
    e.loadTime = startTime;
    inst_ = std::make_unique<scene::WaveShapeInstance>(*cfg_, e);
    double hold = inst_->duration();
    if (!std::isfinite(hold) || hold < 0) hold = defaultHold;
    hold = std::min(hold, 24.0 * 3600.0);
    env_.fade = std::max(0.0, fade);
    env_.hold = hold;
    env_.fn = std::move(fn);
}

double ParticleInstance::scriptEndTime() const { return start_ + 5.0 + 0.5 * (env_.fade + env_.hold); }

void ParticleInstance::evaluate(const scene::WaveShapeFrameInput& inBase, MorphFrame& out, scene::WorkerPool* pool) {
    scene::WaveShapeFrameInput in = inBase;
    const double f = fader(in.t);
    in.startTime = start_;
    in.endTime = scriptEndTime();
    const double life = env_.total();
    in.dt = life > 0 ? std::clamp((in.t - start_) / life, 0.0, 1.0) : 0.0;
    in.fader = f;
    inst_->setPool(pool);
    inst_->evaluateFrame(in, frame_);
    toMorphFrame(frame_, inst_->xExtent(), inst_->yExtent(), out);
    const float k = float(f);
    for (MorphPath& p : out)
        for (float& pen : p.pen) pen *= k;
}

ParticleInstance& ParticleSystem::spawn(std::unique_ptr<ParticleInstance> p) {
    while (alive_.size() >= kMaxAlive) alive_.erase(alive_.begin());
    alive_.push_back(std::move(p));
    return *alive_.back();
}

void ParticleSystem::step(const scene::WaveShapeFrameInput& in, const FrameMapping& map, const LineStyle& style,
                          std::vector<LineSegment>& out) {
    alive_.erase(std::remove_if(alive_.begin(), alive_.end(), [&](const auto& p) { return p->expired(in.t); }),
                 alive_.end());
    // Morph frames are normalized (extents divided out).
    const FrameMapping norm{map.width, map.height, 1.0, 1.0};
    for (auto& p : alive_) {
        p->evaluate(in, frame_, pool_);
        appendSegments(frame_, norm, style, out);
    }
}

void ParticleSystem::fadeOutAll(double t, double fade) {
    if (fade <= 0.0) {
        killAll();
        return;
    }
    for (auto& p : alive_) p->fadeOutFrom(t, fade);
}

std::vector<std::string> ParticleSystem::names() const {
    std::vector<std::string> n;
    for (const auto& p : alive_) n.push_back(p->name());
    return n;
}

}  // namespace reforce::engine
