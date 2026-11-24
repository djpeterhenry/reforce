#pragma once
// reforce extension (docs/3d-design.md "Rubik's cube"): six sticker intensities that read as six different colors
// under the current ColorMap. Pure C++, allocation-free.
//
// Solids are drawn as intensity and only take color in the colorize pass, so a sticker's color is the ColorMap at
// its intensity (after ColorMap.MapFunction). No fixed set of six intensities works across the ColorMaps (measured
// over the 212 PNG maps: with evenly spaced levels the closest pair of stickers is under 10 CIELAB units apart on
// half of them), so they are picked per ColorMap:
//   - each intensity k = 0..255 is mapped to its color like colorize_fs does (the MapFunction LUT, then the
//     palette, interpolated) and converted to CIELAB;
//   - a candidate's "wobble" is the largest color change within +-3 levels: a sticker on a steep part of the map
//     changes color with anti-aliasing and trails, so distances are counted net of half of both wobbles;
//   - the score of a set is the smallest such distance between two stickers or between a sticker and the plastic
//     (intensity 0, the color the cube's body gets); greedy farthest-point picks from several starts, then a few
//     rounds of moving one level at a time, keep the best;
//   - the six are paired onto opposite faces so that the closest pairs never touch on a solved cube (like white /
//     yellow on a real cube), and assigned to the faces to change the colors as little as possible.
// StickerPicker adds hysteresis (keeps its set unless a new one scores clearly better on the current map) and eases
// the intensities over `ease` seconds when it switches (ColorMap changes and morphs).

#include <array>
#include <cstdint>

#include "engine/Palette.hpp"

namespace reforce::engine {

struct Lab {
    float l = 0, a = 0, b = 0;
};
Lab labFromSrgb(float r, float g, float b);  // sRGB 0..1 (D65)
float deltaE(const Lab& p, const Lab& q);     // CIE76

// The color colorize gives intensity k / 255, in CIELAB.
using LabTable = std::array<Lab, kPaletteSize>;
void effectiveColors(const Palette& palette, const MapLut& lut, LabTable& out);
// The color (sRGB 0..1) colorize gives intensity i (0..1), as color_of in the colorize shaders.
std::array<float, 3> colorAt(const Palette& palette, const MapLut& lut, float i);

struct StickerPick {
    std::array<int, 6> level{};  // intensity * 255 per face color (rubik color order: opposite faces 0/1, 2/3, 4/5)
    float score = 0;             // smallest net CIELAB distance between stickers or to the plastic
};
// Best set for `lab` (`body`: the plastic's intensity index, 0). `prev` (optional): the faces' current levels, to
// keep the face -> color assignment stable.
StickerPick pickStickers(const LabTable& lab, int body = 0, const std::array<int, 6>* prev = nullptr);
// Score of a given set on `lab` (as pickStickers scores).
float stickerScore(const LabTable& lab, const std::array<int, 6>& level, int body = 0);

class StickerPicker {
public:
    explicit StickerPicker(double ease = 0.5) : ease_(ease) {}
    // Re-picks when the palette or LUT changed since the last call (at most every `minInterval` seconds).
    void update(const Palette& palette, const MapLut& lut, double t, double minInterval = 0.1);
    // Face intensities 0..1 at t (eased after a switch). Before the first update: evenly spaced.
    std::array<float, 6> levels(double t) const;
    float score() const { return score_; }
    int switches() const { return switches_; }

private:
    double ease_;
    bool have_ = false;
    double lastT_ = -1e30, switchT_ = -1e30;
    Palette lastPal_{};
    MapLut lastLut_{};
    LabTable lab_{};
    std::array<int, 6> from_{}, to_{};
    float score_ = 0;
    int switches_ = 0;
};

// The 15 puzzle's four row colors, all light (L* >= 45: the numbers are drawn dark) or all dark (L* 15..40: light
// numbers). No plastic to keep clear of; `prev`: the rows' current levels, to keep each row's color stable.
struct TilePick {
    std::array<int, 4> level{};
    float score = 0;  // smallest net CIELAB distance between two rows (-inf: not enough light / dark colors)
    bool light = true;
};
TilePick pickTiles(const LabTable& lab, bool light, const std::array<int, 4>* prev = nullptr);
float tileScore(const LabTable& lab, const std::array<int, 4>& level, bool light);

// Light tiles with dark numbers unless the ColorMap has no good light colors (score < 8 and the dark set better); the choice is made only when the map changes, with hysteresis, and both the row colors and the
// numbers' dark / light fade over `ease` seconds. numeral(): 0 = dark numbers .. 1 = light.
class TilePicker {
public:
    explicit TilePicker(double ease = 0.5) : ease_(ease) {}
    void update(const Palette& palette, const MapLut& lut, double t, double minInterval = 0.1);
    std::array<float, 4> levels(double t) const;  // per row of the solved board
    float numeral(double t) const;
    bool light() const { return light_; }
    float score() const { return score_; }
    int switches() const { return switches_; }

private:
    float eased(double t) const;
    double ease_;
    bool have_ = false, light_ = true;
    double lastT_ = -1e30, switchT_ = -1e30;
    Palette lastPal_{};
    MapLut lastLut_{};
    LabTable lab_{};
    std::array<int, 4> from_{}, to_{};
    float numFrom_ = 0;
    float score_ = 0;
    int switches_ = 0;
};

// The pyramid hopper's top states (docs/arcade-design.md "Pyramid hopper", Peter 2026-11-23: the change of a top was
// hard to see on some ColorMaps): `n` (2..3) intensity levels, dark .. bright, that read as clearly different colors
// under the current ColorMap and stay clear of the cubes' seams (`edge`) and sides (`sideL`, `sideR`). Organics are
// drawn as intensity like the solids, so it is the stickers' problem with fewer colors. `prefer` (the TLo / THi keys)
// wins whenever it already reads well enough: the pick moves away from it only as far as the map needs.
struct TopPick {
    std::array<int, 3> level{};  // level[0..n): dark .. bright, ascending, 0..255
    int n = 2;
    float score = 0;  // the smallest net CIELAB distance between two states or a state and a seam / side
};
struct TopSpec {
    int n = 2;
    std::array<int, 3> prefer{153, 204, 255};  // levels 0..255, ascending
    int edge = 10, sideL = 56, sideR = 102;
    bool operator==(const TopSpec&) const = default;
};
TopPick pickTops(const LabTable& lab, const TopSpec& spec);
float topScore(const LabTable& lab, const TopSpec& spec, const std::array<int, 3>& level);  // the raw distance

class TopPicker {
public:
    explicit TopPicker(double ease = 0.5) : ease_(ease) {}
    // `version` changes whenever `lab` does (OrganicColors::version); re-picks at most every `minInterval` seconds.
    void update(const LabTable& lab, std::uint64_t version, const TopSpec& spec, double t, double minInterval = 0.1);
    bool have() const { return have_; }
    std::array<float, 3> levels(double t) const;  // 0..1, eased over `ease` seconds after a switch
    float score() const { return score_; }
    int switches() const { return switches_; }

private:
    double ease_;
    bool have_ = false;
    std::uint64_t version_ = 0;
    TopSpec spec_{};
    double lastT_ = -1e30, switchT_ = -1e30;
    std::array<int, 3> from_{}, to_{};
    float score_ = 0;
    int switches_ = 0;
};

// The labyrinth's three colors: its board, walls and ball (docs/3d-design.md "Labyrinth"). Three intensities as far
// apart as possible (net CIELAB distance, wobble as for the stickers), each also clear of black (the holes); the
// darkest goes to the board, the lightest to the walls (their lit tops read against the board), the third to the
// ball. TrioPicker: hysteresis and easing as StickerPicker.
struct TrioPick {
    std::array<int, 3> level{};  // board, walls, ball (intensity * 255)
    float score = 0;
};
TrioPick pickTrio(const LabTable& lab);
float trioScore(const LabTable& lab, const std::array<int, 3>& level);

class TrioPicker {
public:
    explicit TrioPicker(double ease = 0.5) : ease_(ease) {}
    void update(const Palette& palette, const MapLut& lut, double t, double minInterval = 0.1);
    std::array<float, 3> levels(double t) const;  // board, walls, ball 0..1
    float score() const { return score_; }
    int switches() const { return switches_; }

private:
    double ease_;
    bool have_ = false;
    double lastT_ = -1e30, switchT_ = -1e30;
    Palette lastPal_{};
    MapLut lastLut_{};
    LabTable lab_{};
    std::array<int, 3> from_{}, to_{};
    float score_ = 0;
    int switches_ = 0;
};

}  // namespace reforce::engine
