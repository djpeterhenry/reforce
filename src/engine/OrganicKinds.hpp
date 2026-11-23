#pragma once
// Organics (PLAN 5O.4): the procedural kinds besides trees and pendulums (engine/Organics.hpp). Each is a small
// "body" that draws line segments through an OrganicCanvas (tree units, the whole-organic transform X / Y / Scal /
// Rot, one path per organic). All are driven by the beat clock when it is confident (SYNC_BEAT / SYNC_BAR, aligned to
// the phrase clock), else by 120 BPM from the start.
//
//   phyllotaxis   a sunflower head: seed n at angle n * Ang and radius Spc * sqrt(n), Rate seeds per beat
//   harmonograph  a Lissajous figure in bars: x = AX sin(2 pi FX b + PhX), y = AY sin(2 pi FY b + PhY), drawn over
//                 the last Len bars (b = the bar count), fading towards its tail
//   automaton     a 1D cellular automaton (Wolfram rule `Rule`): one generation per beat / Rate, scrolling
//   life          Conway's Game of Life on a Cols x Rows grid, one generation per beat / Rate, reseeded on phrases
//   lsystem       an L-system (literal Axm, R1..R4 rules "F=FF"), one generation per Rate bars, each new generation
//                 traced over its first bar
//   attractor     a Clifford attractor (PA, PB, PC, PD per frame): Num points per frame as dots
//   invaders, hopper, cycles, rocks, bricks, chase   arcade games (engine/arcade, docs/arcade-design.md)

#include <memory>
#include <string>
#include <vector>

#include "engine/LineGeometry.hpp"
#include "engine/Sprites.hpp"
#include "scene/Evaluate.hpp"

namespace reforce::engine {

// Draws in tree units (y = +-1 at the bottom / top) through the organic's transform into frame pixels.
class OrganicCanvas {
public:
    OrganicCanvas(const FrameMapping& map, const LineStyle& style, std::vector<LineSegment>& out, double x, double y,
                  double scale, double rot);
    // Widths in LWdt units (scaled with Scal); pens 0..1. Off-frame or non-finite points are skipped.
    void line(double x0, double y0, double x1, double y1, double width, double pen0, double pen1);
    void line(double x0, double y0, double x1, double y1, double width, double pen) {
        line(x0, y0, x1, y1, width, pen, pen);
    }
    void dot(double x, double y, double width, double pen);
    // A dot `diameter` tree units across (cells of a grid), at least 1 px. `hard`: no anti-aliased rim (its in-between
    // intensities take unrelated ColorMap colours, a speckled fringe on big dots).
    void dotUnits(double x, double y, double diameter, double pen, bool hard = false);

    // Filled shapes (PLAN 5Q, arcade kinds), made of horizontal strokes on the frame's pixel grid, one per pair of
    // pixel rows (kFillSpacingPx), so they stay solid at any Scal / Rot. Hard-edged (no anti-aliased rim, whose
    // in-between intensities the ColorMap paints in unrelated colours: a speckled fringe), with square corners. Strokes of one path
    // max-combine, so overlapping fills do not add up. The polygon is in tree units (even-odd rule, any shape).
    static constexpr float kFillSpacingPx = 2.0f;
    void fillPolygon(const double* xs, const double* ys, int n, double pen);
    // An axis-aligned rectangle (in tree units, before Rot) centred at (cx, cy), w x h.
    void rect(double cx, double cy, double w, double h, double pen);
    // Pixel art: `rows` top to bottom, one char per pixel ('.' or ' ' = empty, anything else = lit), each pixel
    // `px` tree units square, the whole picture centred at (cx, cy). Runs of lit pixels in a row are one rectangle.
    // `mirror` flips it left-right.
    void bitmap(const char* const* rows, int nRows, double cx, double cy, double px, double pen, bool mirror = false);

    // Starts a new path: what is drawn from here on overwrites (rather than max-combines with) what this organic
    // drew before, e.g. a character over the floor it stands on. The whole organic still goes after the WaveShape.
    void newPath() { path_ = float(out_.size()); }
    // Frame pixels per tree unit at the current Scal (H / 2 x |Scal|).
    double pixelsPerUnit() const;
    // Half the frame's width in tree units (W / H; 16 / 9 for 16:9), before the organic's X / Y / Scal / Rot: a game
    // fitting its field to the frame. Constant for a run (the render size).
    double halfWidth() const { return map_.height > 0 ? map_.width / map_.height : 16.0 / 9.0; }

private:
    bool toPx(double x, double y, float& ox, float& oy) const;
    float radius(double width) const;
    const FrameMapping& map_;
    const LineStyle& style_;
    std::vector<LineSegment>& out_;
    double x_, y_, rc_, rs_, absScale_, limit_;
    float path_;
};

// What a body gets each step.
struct OrganicStep {
    double t = 0, age = 0, dt = 0;  // sim time, age of the organic, seconds since the last step
    double beat = 0;                // beats: the beat clock when confident, else 2 per second of age
    double bar = 0;                 // bars, aligned to the phrase clock when confident, else beat / 4
    bool clock = false;             // the beat clock was used
    double pen = 1;                 // Pen x FADER
    const scene::OrganicFrame* frame = nullptr;
    const SpriteAudio* audio = nullptr;
};

class OrganicBody {
public:
    virtual ~OrganicBody() = default;
    virtual void step(const OrganicStep& s, OrganicCanvas& c) = 0;
    virtual std::string summary() const = 0;
};

// The body for `kind` ("phyllotaxis", "harmonograph", "automaton", "life", "lsystem", "attractor"; lower case), or
// null for another kind. Load-time keys come from `inst` (OrganicInstance::loaded / literal()).
std::unique_ptr<OrganicBody> makeOrganicBody(const std::string& kind, scene::OrganicInstance& inst,
                                             std::uint64_t seed);
// The kinds makeOrganicBody knows.
const std::vector<std::string>& organicBodyKinds();

}  // namespace reforce::engine
