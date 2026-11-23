#include "engine/Transition.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

#include "expr/Compiler.hpp"

namespace reforce::engine {

const char* layerName(Layer l) {
    switch (l) {
        case Layer::WaveShape: return "WaveShape";
        case Layer::ColorMap: return "ColorMap";
        case Layer::FlowField: return "FlowField";
        case Layer::Union: return "FlowField Union";
        case Layer::Particle: return "Particle";
        case Layer::Sprite: return "Sprite";
        case Layer::Object: return "Object";
        case Layer::Dancer: return "Dancer";
    }
    return "?";
}

std::optional<Layer> parseLayer(const std::string& s) {
    std::string k;
    for (char c : s)
        if (!std::isspace(static_cast<unsigned char>(c)) && c != '_' && c != '-')
            k.push_back(char(std::tolower(static_cast<unsigned char>(c))));
    if (k == "waveshape" || k == "ws" || k == "wave") return Layer::WaveShape;
    if (k == "colormap" || k == "cm" || k == "color") return Layer::ColorMap;
    if (k == "flowfield" || k == "ff" || k == "flow") return Layer::FlowField;
    if (k == "flowfieldunion" || k == "union") return Layer::Union;
    if (k == "particle" || k == "particles") return Layer::Particle;
    if (k == "sprite" || k == "sprites") return Layer::Sprite;
    if (k == "object" || k == "objects") return Layer::Object;
    if (k == "dancer" || k == "dancers") return Layer::Dancer;
    return std::nullopt;
}

TransitionFunction::TransitionFunction(const std::string& source) : source_(source) {
    expr::BasicResolver names;
    expr::Diagnostics diags;
    prog_ = expr::compileExpression(source, names, diags);
    for (const auto& d : diags.items())
        if (d.severity == expr::Severity::Error) error_ += d.message + "; ";
    if (!prog_.defined || prog_.failed) {
        if (error_.empty()) error_ = prog_.failed ? "parse error" : "empty";
        prog_ = expr::compileExpression("t", names, diags);
    }
}

double TransitionFunction::operator()(double t) const {
    t = std::clamp(std::isfinite(t) ? t : 0.0, 0.0, 1.0);
    ctx_.set(expr::Var::T, t);
    const double w = expr::runScalar(prog_, ctx_);
    return std::isfinite(w) ? std::clamp(w, 0.0, 1.0) : t;
}

double TransitionFunction::inverse(double w) const {
    double lo = 0, hi = 1;
    for (int i = 0; i < 40; ++i) {
        const double mid = 0.5 * (lo + hi);
        ((*this)(mid) < w ? lo : hi) = mid;
    }
    return 0.5 * (lo + hi);
}

double TransitionClock::progress(double now) const {
    if (duration_ <= 0) return 1.0;
    return std::clamp((now - start_) / duration_, 0.0, 1.0);
}

double TransitionClock::weight(double now) const {
    if (manual_) return *manual_;
    const double p = progress(now);
    return fn_ ? (*fn_)(p) : p;
}

void TransitionClock::release(double now) {
    if (!manual_) return;
    const double w = *manual_;
    manual_.reset();
    const double p = fn_ ? fn_->inverse(w) : w;
    start_ = now - p * duration_;
}

double sampleWithDeviation(double base, double relDeviation, double normal01, double minimum) {
    if (!(base > 0)) return std::max(minimum, 0.0);
    double v = base * (1.0 + relDeviation * normal01);
    v = std::clamp(v, base * 0.25, base * 4.0);
    return std::max(v, minimum);
}

}  // namespace reforce::engine
