#pragma once
// WaveShape frame -> line segments in render pixels (the GPU line pass draws
// each segment as a round-capped capsule; dots are zero-length capsules).
//
// Probe results (docs/probe-results.md):
//   #15 overwrite blending in path order (later paths replace earlier pixels,
//       Pen 0 erases) -> segments are emitted in path order, then point order;
//   #16/#38 width per LWdt (G-Force 5.9.7, probe run 24 at 18 frame sizes,
//       area-based widths): s(W, H) = 0.7330 + 8.674e-4 * min(W, H)
//       + 5.097e-4 * max(W, H) px in G-Force's frame pixels (max error 0.0005
//       px per LWdt, portrait included). G-Force renders frames over ~5.2 MP
//       (e.g. 3456x2234 full screen on a 16" MacBook Pro) at half size and
//       doubles the pixels, so lines there are 2 * s(W/2, H/2) output pixels.
//       Width = LWdt * LineWidth.Scale * s + LineWidth.Offset (offset in frame
//       px). The old "(H - 68) / 420" was a 2-point fit through 1980x1260 and
//       that half-size full-screen frame. (Counting lit pixels gives 3 and ~5
//       px per LWdt because of the antialiased edge pixels.)
//   dots (unconnected points) whose center lies outside the pixel grid
//   [0, W) x [0, H) are not drawn: G-Force draws a dot at x = -1 (half
//   visible) but none at x = +1 (parity suite, run 1 shot 7);
//   #7  intensity = Pen (clamped to 0..1).

#include <cstdint>
#include <vector>

#include "engine/Blend.hpp"
#include "scene/Evaluate.hpp"

namespace reforce::engine {

// reforce.LineScale: which frame the width law (#38) is applied to.
//   GForce: G-Force's frame, i.e. half the render size when W x H exceeds
//           kGForceMaxFramePixels (then doubled), so full screen on a Retina
//           Mac looks like G-Force's full screen.
//   Frame:  the actual render size (continuous: no step at ~5.2 MP).
enum class LineScaleMode { GForce, Frame };

struct LineStyle {
    double scale = 1.0;    // WaveShape.LineWidth.Scale
    double offset = 0.0;   // WaveShape.LineWidth.Offset
    bool autoScale = true; // WaveShape.AutoLineScale (G-Force 5.9.7 ignores 0: it always scales; reforce honors it)
    LineScaleMode sizeMode = LineScaleMode::GForce;  // reforce.LineScale
    // Narrowest width passed to the GPU. Lines narrower than 1 px are drawn
    // with a 1-px footprint there: at full pen without AA (and in the legacy
    // analytic mode), with coverage scaled by the width under per-path AA
    // (intensity proportional to width, Phase 5E).
    double minWidthPx = 0.0;
};

// Px per LWdt for a frame of W x H pixels: kLineScaleBase + kLineScaleShort * min + kLineScaleLong * max.
inline constexpr double kLineScaleBase = 0.73304;
inline constexpr double kLineScaleShort = 8.6742e-4;
inline constexpr double kLineScaleLong = 5.0969e-4;
// G-Force halves frames above this many pixels: 2800x1844 (5,163,200) rendered at full size, 3416x1544
// (5,274,304) at half; 5 * 2^20 is in between (exact limit unmeasured).
inline constexpr double kGForceMaxFramePixels = 5.0 * 1024 * 1024;

// Px per LWdt (AutoLineScale law) for a W x H frame, no halving.
double autoLineScale(double width, double height);
// Output pixels per G-Force frame pixel: 2 when G-Force would render W x H at half size (LineScaleMode::GForce).
double gforceFramePixelSize(double width, double height);

// Line width in render pixels for an LWdt value at a given render size.
double lineWidthPixels(double lwdt, const LineStyle& style, double renderWidth, double renderHeight);

// With reforce.ThinLines = gforce the line pass snaps these positions to
// G-Force's pixel grid itself (col = W/2 + trunc(x W/2), probe #32; vertex
// shader), so the geometry stays continuous here for both modes.
//
// Frame coordinates (center origin, +y up, +-extent at the edges) -> render
// pixels (origin top-left, +y down). G-Force draws a line at frame x through
// the CENTER of pixel column (x + 1) * W / 2 (so x = 0 is the center of column
// W/2, not the boundary between W/2 - 1 and W/2), same for y: the probe grid's
// lines at x = 0 / y = 0 cover columns 987..993 / rows 627..633 at 1980x1260
// (corpus sweep, docs/scene-format.md §2). Hence the half-pixel offset.
struct FrameMapping {
    double width = 1920, height = 1080;    // render size in pixels
    double xExtent = 1.0, yExtent = 1.0;   // config extents (Aspc)
    static constexpr double kPixelCenter = 0.5;
    double toPixelX(double x) const { return (x / xExtent + 1.0) * 0.5 * width + kPixelCenter; }
    double toPixelY(double y) const { return (1.0 - y / yExtent) * 0.5 * height + kPixelCenter; }
};

// Must match `struct LineSeg` in the MSL source (32 bytes).
struct LineSegment {
    float ax, ay, bx, by;  // pixels
    float penA, penB;      // intensity 0..1 at a and b
    float radius;          // half the width, pixels (may be < 0.5, see LineStyle::minWidthPx); negative = the same
                           // width with a hard edge (no anti-aliasing: organic cells, OrganicCanvas::dotUnits)
    float path = 0;        // path id: index of the path's first segment in the output (per-path AA)
};
static_assert(sizeof(LineSegment) == 32);

// Appends the segments of `frame` (ConB=1: polylines; ConB=0: dots).
// Non-finite or absurdly far points are skipped (a polyline breaks there).
void appendSegments(const scene::WaveShapeFrame& frame, const FrameMapping& map, const LineStyle& style,
                    std::vector<LineSegment>& out);
// Same for morph output (normalized coordinates: use a mapping with extents 1);
// each path carries its own connected flag.
void appendSegments(const MorphFrame& frame, const FrameMapping& map, const LineStyle& style,
                    std::vector<LineSegment>& out);

}  // namespace reforce::engine
