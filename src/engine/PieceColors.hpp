#pragma once
// reforce extension (docs/3d-design.md "Soma cube"): seven piece intensities that read as seven different colors under
// the current ColorMap, for the soma solid. Pure C++, allocation-free. The same method as engine/StickerColors (the
// colors colorize gives each intensity, in CIELAB, scored net of their "wobble"), with two differences:
//   - instead of a plastic, the pieces keep clear of the background (intensity 0, a little more leniently);
//   - the colors are light enough to shade (L* >= 40 where the map has 21 such levels; lowered to 30, 20, 0 when
//     it doesn't): the soma's sides are its colors darkened by the shading, and a dark color would turn black.
// PiecePicker adds the same hysteresis and easing as StickerPicker, and keeps each piece's color as far as possible
// when the map changes; on a map that changes over time it also switches when a piece's color has turned dark.

#include <array>

#include "engine/Palette.hpp"
#include "engine/StickerColors.hpp"

namespace reforce::engine {

inline constexpr int kPieceColors = 7;

struct PiecePick {
    std::array<int, kPieceColors> level{};  // intensity * 255 per piece (engine/Soma piece order)
    float score = 0;  // smallest net CIELAB distance between two pieces (or 1.25 x to the background)
    float minL = 0;   // the lightness floor used
};
// `prev`: the pieces' current levels, to keep each piece's color stable.
PiecePick pickPieces(const LabTable& lab, const std::array<int, kPieceColors>* prev = nullptr);
float pieceScore(const LabTable& lab, const std::array<int, kPieceColors>& level);

class PiecePicker {
public:
    explicit PiecePicker(double ease = 0.5) : ease_(ease) {}
    // Re-picks when the palette or LUT changed since the last call (at most every `minInterval` seconds).
    void update(const Palette& palette, const MapLut& lut, double t, double minInterval = 0.1);
    std::array<float, kPieceColors> levels(double t) const;  // 0..1, eased after a switch
    float score() const { return score_; }
    int switches() const { return switches_; }

private:
    double ease_;
    bool have_ = false;
    double lastT_ = -1e30, switchT_ = -1e30;
    Palette lastPal_{};
    MapLut lastLut_{};
    LabTable lab_{};
    std::array<int, kPieceColors> from_{}, to_{};
    float score_ = 0;
    int switches_ = 0;
};

}  // namespace reforce::engine
