#include "engine/Palette.hpp"

#include <algorithm>
#include <cmath>

#include "expr/Compiler.hpp"

namespace reforce::engine {

MapFunction::MapFunction(const std::string& expr) : source_(expr) {
    expr::BasicResolver names;
    expr::Diagnostics diags;
    prog_ = expr::compileExpression(expr, names, diags);
    for (const auto& d : diags.items())
        if (d.severity == expr::Severity::Error) error_ += d.message + "; ";
    if (!prog_.defined || prog_.failed) {
        if (error_.empty() && prog_.failed) error_ = "parse error";
        prog_ = expr::compileExpression("i", names, diags);
    }
}

void MapFunction::evaluate(double t, MapLut& out) {
    ctx_.set(expr::Var::T, t);
    for (int k = 0; k < kPaletteSize; ++k) {
        ctx_.set(expr::Var::I, double(k) / double(kPaletteSize - 1));
        const double v = expr::runScalar(prog_, ctx_);
        out[std::size_t(k)] = std::isfinite(v) ? float(std::clamp(v, 0.0, 1.0)) : 0.f;
    }
}

Palette paletteFromStrip(const std::uint8_t* rgba, int width) {
    Palette p{};
    if (!rgba || width <= 0) return p;
    for (int k = 0; k < kPaletteSize; ++k) {
        // G-Force looks the strip up like a linearly filtered texture: u = i in [0, 1] across the whole
        // strip, texel centers at (j + .5) / W, clamped at the ends. The corpus sweep (docs/sweep.md) fits
        // this on all 212 PNG maps; endpoint-to-endpoint resampling (u * (W - 1)) failed 8 with sharp steps.
        const double x = std::clamp(double(k) / double(kPaletteSize - 1) * double(width) - 0.5, 0.0, double(width - 1));
        const int x0 = std::clamp(int(std::floor(x)), 0, width - 1);
        const int x1 = std::min(x0 + 1, width - 1);
        const double f = x - x0;
        auto ch = [&](int c) {
            return float(((1.0 - f) * rgba[x0 * 4 + c] + f * rgba[x1 * 4 + c]) / 255.0);
        };
        p[std::size_t(k)] = {ch(0), ch(1), ch(2), 1.f};
    }
    return p;
}

namespace {
// A ColorMap's clock starts with it (see update()): its load tiers see t = 0 ("Return to Aqua": A3="t").
scene::Environment localClock(scene::Environment env) {
    env.loadTime = 0.0;
    return env;
}
}  // namespace

ColorMapSource::ColorMapSource(std::shared_ptr<const scene::CompiledConfig> cfg, const scene::Environment& env)
    : cfg_(std::move(cfg)), inst_(std::make_unique<scene::ColorMapInstance>(*cfg_, localClock(env))) {}

ColorMapSource::ColorMapSource(const Palette& fixed) : fixed_(fixed) {}

double ColorMapSource::recomputeInterval() const { return inst_ ? inst_->recomputeInterval() : 0.0; }

bool ColorMapSource::update(double t, Palette& out) {
    if (!inst_) {
        if (!first_) return false;
        first_ = false;
        out = fixed_;
        return true;
    }
    if (first_) start_ = t;
    if (!first_ && t - lastEval_ < inst_->recomputeInterval()) return false;
    first_ = false;
    lastEval_ = t;
    std::array<scene::Rgb, scene::ColorMapInstance::kEntries> rgb;
    // A ColorMap's t counts from when it started (corpus sweep: all 25 t-dependent maps match G-Force
    // at t = 0.7 s for a shot 0.7 s after the switch, none at the global clock).
    inst_->evaluate(t - start_, rgb);
    for (int k = 0; k < kPaletteSize; ++k) {
        auto c = [](float v) { return std::isfinite(v) ? std::clamp(v, 0.f, 1.f) : 0.f; };
        const scene::Rgb& e = rgb[std::size_t(k)];
        out[std::size_t(k)] = {c(e.r), c(e.g), c(e.b), 1.f};
    }
    return true;
}

}  // namespace reforce::engine
