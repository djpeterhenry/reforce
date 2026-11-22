#pragma once
// ColorMap side of the colorize pass:
//   MapFunction  ColorMap.MapFunction pref (an expression in i and t) baked
//                into a 256-entry LUT applied to the intensity first;
//   ColorMapSource  text ColorMap (ColorMapInstance, re-evaluated every CCal
//                seconds) or a PNG strip (N x 1, left = i 0).

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "expr/Program.hpp"
#include "expr/Vm.hpp"
#include "scene/Evaluate.hpp"

namespace reforce::engine {

inline constexpr int kPaletteSize = 256;

struct PaletteEntry {  // float4 in MSL
    float r = 0, g = 0, b = 0, a = 1;
};
using Palette = std::array<PaletteEntry, kPaletteSize>;
using MapLut = std::array<float, kPaletteSize>;

class MapFunction {
public:
    // Compiles `expr` (variables: i, t). Empty or failing -> identity, with
    // the error text in error().
    explicit MapFunction(const std::string& expr = "i");
    // LUT[k] = clamp(f(k/255, t), 0, 1); NaN -> 0.
    void evaluate(double t, MapLut& out);
    bool timeDependent() const { return prog_.usesVar(expr::Var::T); }
    const std::string& error() const { return error_; }
    const std::string& source() const { return source_; }

private:
    std::string source_, error_;
    expr::Program prog_;
    expr::EvalContext ctx_;
};

// N x 1 RGBA8 strip (any height: the first row is used) -> 256 entries,
// sampled like a linearly filtered, edge-clamped texture (u = i, texel centers
// at (j + .5) / W; corpus sweep), so entry 0 = leftmost pixel, 255 = rightmost.
Palette paletteFromStrip(const std::uint8_t* rgba, int width);

class ColorMapSource {
public:
    // Text ColorMap. `cfg` must outlive this object (shared ownership).
    ColorMapSource(std::shared_ptr<const scene::CompiledConfig> cfg, const scene::Environment& env);
    // Fixed palette (PNG ColorMap).
    explicit ColorMapSource(const Palette& fixed);

    // Re-evaluates when CCal seconds have passed since the last evaluation
    // (always on the first call). Returns true when `out` changed. The
    // config's t is local: seconds since the first update() (its start).
    bool update(double t, Palette& out);

    double recomputeInterval() const;

private:
    std::shared_ptr<const scene::CompiledConfig> cfg_;
    std::unique_ptr<scene::ColorMapInstance> inst_;
    Palette fixed_{};
    double lastEval_ = -1e300;
    bool first_ = true;
    double start_ = 0.0;  // t of the first update(): the config's t = 0
};

}  // namespace reforce::engine
