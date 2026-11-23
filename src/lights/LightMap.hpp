#pragma once
// reforce extension: room lights that follow the visuals (docs/lights.md).
//
// LightMapper turns a small capture of the visuals (BGRA8, top row first: render/LightsCapture) into one color per
// light panel. The panels' layout is laid over the whole frame (stretched to it, layout y pointing up), and each panel
// takes the color of the area under it: bright, saturated pixels weigh more than dark ones, so thin bright lines on
// black still color the panel instead of averaging to a dim gray. Then saturation and brightness, a floor, and
// smoothing (fast rise, slower fall). Pure: no I/O, no threads.
#include <cstdint>
#include <vector>

namespace reforce::lights {

struct Panel {
    int id = 0;
    double x = 0, y = 0;  // layout units (Nanoleaf: the panel's center, y up)
    double o = 0;         // orientation in degrees (a triangle points up at 0, 120, 240; down at 60, 180, 300)
};

struct Layout {
    std::vector<Panel> panels;
    double sideLength = 150;  // layout units (a triangle's side)
};

struct Rgb {
    std::uint8_t r = 0, g = 0, b = 0;
    bool operator==(const Rgb&) const = default;
};

struct MapSettings {
    double brightness = 1.0;  // reforce.Lights.Brightness 0..2: scales the panels' level
    double saturation = 1.3;  // reforce.Lights.Saturation 0..3: 1 = as on screen
    double floor = 0.0;       // reforce.Lights.Floor 0..0.5: the panels never go darker (keeping their color)
    double release = 0.3;     // reforce.Lights.Smoothing 0..2 s: how slowly a panel falls back (rises 6x faster)
    double pulse = 0.0;       // reforce.Lights.Pulse 0..1: flash on each beat and fade until the next (0 = off)
    double pulseLeadMs = 40;  // reforce.Lights.PulseTimingMs: the flash this much early (network, the panels' delay)
};

// The pulse's brightness factor `frac` (0..1) of the way through a beat: 1 on the beat, falling to 1 - pulse.
double pulseFactor(double frac, double pulse);

class LightMapper {
public:
    void setLayout(const Layout& layout);
    const Layout& layout() const { return layout_; }
    // The panel's center in the frame, 0..1 (u right, v down).
    double u(std::size_t i) const { return i < uv_.size() ? uv_[i].first : 0; }
    double v(std::size_t i) const { return i < uv_.size() ? uv_[i].second : 0; }

    // One update from a capture `w` x `h` (BGRA8, `rowBytes` per row, top row first), `dt` seconds after the last.
    // `out` gets one color per panel, in layout order. No allocation once the grid size and layout are settled.
    void update(const std::uint8_t* bgra, int w, int h, int rowBytes, double dt, const MapSettings& s,
                std::vector<Rgb>& out);

    // The color for one panel before smoothing (0..1 per channel): exposed for tests.
    static void shade(double r, double g, double b, double meanLevel, const MapSettings& s, double out[3]);

private:
    void buildCells(int w, int h);

    Layout layout_;
    std::vector<std::pair<double, double>> uv_;  // per panel
    double ru_ = 0.05, rv_ = 0.1;                // sample radius in u and v
    int gridW_ = 0, gridH_ = 0;
    std::vector<std::uint32_t> cells_;           // per panel: offsets into the grid (pixel index), concatenated
    std::vector<std::uint32_t> cellStart_;       // per panel + 1: where its run starts in cells_
    std::vector<double> state_;                  // smoothed color per panel (3 doubles)
};

// Nanoleaf external control v2 (UDP): the panel count, then per panel id, R, G, B, W and the transition time
// (in 100 ms), all big-endian 16-bit where two bytes.
void encodeStreamV2(const std::vector<int>& ids, const std::vector<Rgb>& colors, int transition,
                    std::vector<std::uint8_t>& out);

}  // namespace reforce::lights
