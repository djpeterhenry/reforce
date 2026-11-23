#pragma once
// Per-layer transitions (architecture §5a): the weight curve
// (`<Type>.Transition.Function`, an expression in t = progress 0..1) and the
// timing of one morph, which the slideshow clock or a manual crossfader drives.

#include <memory>
#include <optional>
#include <string>

#include "expr/Program.hpp"
#include "expr/Vm.hpp"

namespace reforce::engine {

// Layers that switch configs (Particles/Sprites: M6).
// Object (Peter 2026-11-22): solids and organics as one layer ("Objects"), each scene file still drawn its own way
// (a 3D solid or an organic); the scene types stay SceneType::Solid / SceneType::Organic.
enum class Layer { WaveShape, ColorMap, FlowField, Union, Particle, Sprite, Object, Dancer };
inline constexpr int kLayerCount = 8;
// Layers whose configs are spawned instances with their own lifetimes (several alive at once), not switched.
inline constexpr bool isSpawnLayer(Layer l) {
    return l == Layer::Particle || l == Layer::Sprite || l == Layer::Object || l == Layer::Dancer;
}
inline constexpr Layer kMorphLayers[] = {Layer::WaveShape, Layer::ColorMap, Layer::FlowField};

const char* layerName(Layer l);                         // "WaveShape", ..., "FlowField Union"
std::optional<Layer> parseLayer(const std::string& s);  // case-insensitive; "union", "ws", "cm", "ff" too

// Compiled `Transition.Function`. Evaluates with t clamped to [0,1]; the result
// is clamped to [0,1] and NaN -> t. A function that fails to compile falls
// back to the identity (error() says why).
class TransitionFunction {
public:
    explicit TransitionFunction(const std::string& source = "t");
    double operator()(double t) const;
    // Inverse by bisection (assumes a monotone curve): the t with f(t) ~= w.
    double inverse(double w) const;
    const std::string& source() const { return source_; }
    const std::string& error() const { return error_; }

private:
    std::string source_, error_;
    expr::Program prog_;
    mutable expr::EvalContext ctx_;
};

// Timing of one morph. progress = elapsed / duration (0..1), weight = fn(progress),
// or the manual crossfader value while one is held.
class TransitionClock {
public:
    TransitionClock() = default;
    TransitionClock(double start, double duration, std::shared_ptr<const TransitionFunction> fn)
        : start_(start), duration_(duration), fn_(std::move(fn)) {}

    double progress(double now) const;
    double weight(double now) const;
    bool done(double now) const { return !manual_ && progress(now) >= 1.0; }
    double duration() const { return duration_; }

    // Manual crossfader: holds the weight at `w` (0..1). release() resumes the
    // timer from the progress whose curve value is the held weight.
    void hold(double w) { manual_ = w < 0 ? 0 : (w > 1 ? 1 : w); }
    void release(double now);
    bool held() const { return manual_.has_value(); }

private:
    double start_ = 0, duration_ = 0;
    std::shared_ptr<const TransitionFunction> fn_;
    std::optional<double> manual_;
};

// Sample of `base * (1 + deviation * N(0,1))` (G-Force's relative standard
// deviation), clamped to [base * 0.25, base * 4] and >= minimum.
double sampleWithDeviation(double base, double relDeviation, double normal01, double minimum = 0.05);

}  // namespace reforce::engine
