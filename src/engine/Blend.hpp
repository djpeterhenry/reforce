#pragma once
// Blend-mode registry and the CPU side of each layer's morph (architecture §5a).
// Mode 0 of every layer is `gforce`: the behavior measured on G-Force 5.9.7
// (docs/probe-results.md #9-#12, #18). Alternatives are further entries, so
// adding an experiment is one more name plus one more branch.
//
//   ColorMap   gforce     linear RGB lerp of the two palettes (probe #9)
//              oklab      lerp in OKLab (perceptual, keeps vividness)
//              hsv-short  HSV with the hue along the shortest arc
//   FlowField  gforce     linear lerp of source coordinates (probe #10); with a
//                         Union the per-pixel weight is w * clamp(ALFA, 0, 1) (#18)
//              polar      complex ratio src/p: log-magnitude lerp + shortest-arc angle
//              crossfade  sample the previous frame through both fields, mix the values
//   WaveShape  gforce     paths matched by flattened index (ID x X#/Y# pair),
//                         resampled by s index to a common count, per-index lerp of
//                         position/pen/width; unmatched paths keep full pen and are
//                         dilated about the origin by 1 + 15.5 u^4.16 (#11, #12)
//              aligned    gforce + cyclic-shift alignment of closed loops
//              crossfade  both shapes drawn, pens scaled by (1-w) and w

#include <array>
#include <optional>
#include <string>
#include <vector>

#include "engine/FlowFieldBuilder.hpp"
#include "engine/Palette.hpp"
#include "engine/Transition.hpp"
#include "scene/Evaluate.hpp"

namespace reforce::engine {

// ---------------------------------------------------------------- registry

namespace blend {
enum ColorMode : int { ColorGForce = 0, ColorOklab = 1, ColorHsvShort = 2 };
enum FieldMode : int { FieldGForce = 0, FieldPolar = 1, FieldCrossfade = 2 };
enum WaveMode : int { WaveGForce = 0, WaveAligned = 1, WaveCrossfade = 2 };
}  // namespace blend

// Names for a morphing layer (WaveShape, ColorMap, FlowField); empty otherwise.
const std::vector<std::string>& blendModeNames(Layer layer);
// Index of `name` (case-insensitive) for `layer`; "rgb"/"linear" alias gforce
// where that is what gforce is.
std::optional<int> findBlendMode(Layer layer, const std::string& name);
std::string blendModeName(Layer layer, int mode);

// ---------------------------------------------------------------- ColorMap

PaletteEntry blendColor(int mode, const PaletteEntry& a, const PaletteEntry& b, double w);
void blendPalette(int mode, const Palette& a, const Palette& b, double w, Palette& out);

// ---------------------------------------------------------------- FlowField

// Per-pixel weight with an optional Union ALFA value (probe #18 model).
inline double unionWeight(double w, double alfa) {
    const double a = alfa != alfa ? 0.0 : (alfa < 0 ? 0.0 : (alfa > 1 ? 1.0 : alfa));
    return w * a;
}

// Blended source point for destination `p` (normalized frame coords, +-1 edges),
// fields a and b (normalized, kOutOfFrame sentinel allowed), weight w.
// `aspect` = width / height (the polar mode works in square units).
// Crossfade is not a point blend; here it falls back to linear (used for baking).
std::array<float, 2> blendFieldPoint(int mode, float px, float py, float ax, float ay, float bx, float by, double w,
                                     double aspect);

// Bakes a blend into a single field (interruptions: the in-progress blend
// becomes the next transition's "from"). `mask` may be null.
FlowFieldGrid bakeFieldBlend(int mode, const FlowFieldGrid& a, const FlowFieldGrid& b, const UnionMask* mask,
                             double w);

// ---------------------------------------------------------------- WaveShape

// One drawable path in normalized frame coordinates (+-1 at the frame edges).
struct MorphPath {
    std::vector<float> x, y, pen, width;
    bool connected = true;
    std::size_t size() const { return x.size(); }
};
using MorphFrame = std::vector<MorphPath>;

// Flattens a WaveShape frame (paths in ID-major, X#/Y# order) into normalized
// coordinates (divides by the config extents).
void toMorphFrame(const scene::WaveShapeFrame& frame, double xExtent, double yExtent, MorphFrame& out);

// Unmatched-path dilation (probe #12): f(u) = 1 + 15.5 u^4.16.
double dilationFactor(double u);

// Linear resample of a path to `n` points by s index.
MorphPath resamplePath(const MorphPath& p, std::size_t n);

// Per-transition cache for the aligned mode (cyclic shifts of closed loops).
struct AlignCache {
    std::vector<int> shift;
    std::vector<signed char> dir;  // +1 / -1; 0 = not a closed pair
    int age = 0;
    // Scratch for the (parallel) shift search, reused between recomputes.
    struct Candidates {
        double sum;
        int shift;
        signed char dir;
    };
    std::vector<MorphPath> ra, rb;          // paths resampled to a common count
    std::vector<int> chunkBase;             // first chunk of each path (size matched + 1)
    std::vector<Candidates> chunkBest;      // best candidate per chunk
};

// Morph `from` -> `to` at weight w (0 = from, 1 = to) into `out`. With a pool,
// the aligned mode's shift search (every 30 frames, O(n^2) per closed loop)
// runs in parallel; the result is identical to the serial search.
void morphFrames(int mode, const MorphFrame& from, const MorphFrame& to, double w, MorphFrame& out,
                 AlignCache* cache = nullptr, scene::WorkerPool* pool = nullptr);

}  // namespace reforce::engine
