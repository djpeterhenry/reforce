#include "lights/LightMap.hpp"

#include <algorithm>
#include <cmath>

namespace reforce::lights {

void LightMapper::setLayout(const Layout& layout) {
    layout_ = layout;
    uv_.clear();
    state_.assign(layout.panels.size() * 3, 0.0);
    gridW_ = gridH_ = 0;  // cells rebuilt on the next update
    if (layout.panels.empty()) return;
    double x0 = layout.panels[0].x, x1 = x0, y0 = layout.panels[0].y, y1 = y0;
    for (const Panel& p : layout.panels) {
        x0 = std::min(x0, p.x), x1 = std::max(x1, p.x);
        y0 = std::min(y0, p.y), y1 = std::max(y1, p.y);
    }
    // The panels' extent, not just their centers: half a side across, about a third of the height up and down.
    const double side = layout.sideLength > 0 ? layout.sideLength : 150;
    x0 -= side * 0.5, x1 += side * 0.5;
    y0 -= side * 0.3, y1 += side * 0.3;
    const double w = std::max(1.0, x1 - x0), h = std::max(1.0, y1 - y0);
    for (const Panel& p : layout.panels) uv_.emplace_back((p.x - x0) / w, (y1 - p.y) / h);
    // Each panel samples a disc about its own size (stretched with the layout).
    ru_ = side * 0.5 / w;
    rv_ = side * 0.5 / h;
}

void LightMapper::buildCells(int w, int h) {
    gridW_ = w, gridH_ = h;
    cells_.clear();
    cellStart_.assign(1, 0);
    for (const auto& [pu, pv] : uv_) {
        const int cx0 = std::max(0, int(std::floor((pu - ru_) * w))), cx1 = std::min(w - 1, int(std::ceil((pu + ru_) * w)));
        const int cy0 = std::max(0, int(std::floor((pv - rv_) * h))), cy1 = std::min(h - 1, int(std::ceil((pv + rv_) * h)));
        const std::size_t before = cells_.size();
        for (int y = cy0; y <= cy1; ++y)
            for (int x = cx0; x <= cx1; ++x) {
                const double du = ((x + 0.5) / w - pu) / ru_, dv = ((y + 0.5) / h - pv) / rv_;
                if (du * du + dv * dv <= 1.0) cells_.push_back(std::uint32_t(y * w + x));
            }
        if (cells_.size() == before)  // a tiny grid: at least the nearest pixel
            cells_.push_back(std::uint32_t(std::clamp(int(pv * h), 0, h - 1) * w + std::clamp(int(pu * w), 0, w - 1)));
        cellStart_.push_back(std::uint32_t(cells_.size()));
    }
}

void LightMapper::shade(double r, double g, double b, double meanLevel, const MapSettings& s, double out[3]) {
    // The level: sparse bright lines still light the panel (square root of the area's mean level).
    double level = std::clamp(s.brightness * std::sqrt(std::max(0.0, meanLevel)), 0.0, 1.0);
    level = std::max(level, std::clamp(s.floor, 0.0, 1.0));
    // The color: the bright pixels' color at full strength, its saturation scaled around its own gray.
    const double m = std::max({r, g, b});
    double c[3] = {1, 1, 1};
    if (m > 1e-4) c[0] = r / m, c[1] = g / m, c[2] = b / m;
    const double gray = (c[0] + c[1] + c[2]) / 3.0;
    for (double& k : c) k = std::clamp(gray + (k - gray) * s.saturation, 0.0, 1.0);
    const double m2 = std::max({c[0], c[1], c[2]});
    for (int i = 0; i < 3; ++i) out[i] = (m2 > 1e-4 ? c[i] / m2 : 1.0) * level;
}

void LightMapper::update(const std::uint8_t* bgra, int w, int h, int rowBytes, double dt, const MapSettings& s,
                         std::vector<Rgb>& out) {
    const std::size_t n = layout_.panels.size();
    out.resize(n);
    if (n == 0 || !bgra || w <= 0 || h <= 0) return;
    if (w != gridW_ || h != gridH_) buildCells(w, h);
    const double rise = std::max(0.0, s.release) / 6.0, fall = std::max(0.0, s.release);
    for (std::size_t i = 0; i < n; ++i) {
        double ar = 0, ag = 0, ab = 0, wsum = 0, lsum = 0;
        const std::uint32_t a = cellStart_[i], e = cellStart_[i + 1];
        for (std::uint32_t k = a; k < e; ++k) {
            const std::uint32_t idx = cells_[k];
            const std::uint8_t* p = bgra + std::size_t(idx / std::uint32_t(w)) * std::size_t(rowBytes) + (idx % std::uint32_t(w)) * 4;
            const double b = p[0] / 255.0, g = p[1] / 255.0, r = p[2] / 255.0;
            const double l = std::max({r, g, b});
            const double wt = 0.002 + l * l;  // bright pixels decide the color
            ar += wt * r, ag += wt * g, ab += wt * b, wsum += wt;
            lsum += l;
        }
        const double cnt = double(e - a);
        double target[3];
        shade(ar / wsum, ag / wsum, ab / wsum, lsum / cnt, s, target);
        double* st = &state_[i * 3];
        for (int c = 0; c < 3; ++c) {
            const double tau = target[c] > st[c] ? rise : fall;
            const double k = tau <= 1e-6 ? 1.0 : 1.0 - std::exp(-dt / tau);
            st[c] += (target[c] - st[c]) * k;
        }
        out[i] = Rgb{std::uint8_t(std::lround(std::clamp(st[0], 0.0, 1.0) * 255)),
                     std::uint8_t(std::lround(std::clamp(st[1], 0.0, 1.0) * 255)),
                     std::uint8_t(std::lround(std::clamp(st[2], 0.0, 1.0) * 255))};
    }
}

double pulseFactor(double frac, double pulse) {
    const double p = std::clamp(pulse, 0.0, 1.0);
    const double f = frac - std::floor(frac);
    return (1.0 - p) + p * std::exp(-4.5 * f);  // a fifth of the beat later about 40 % of the flash is left
}

void encodeStreamV2(const std::vector<int>& ids, const std::vector<Rgb>& colors, int transition,
                    std::vector<std::uint8_t>& out) {
    const std::size_t n = std::min(ids.size(), colors.size());
    out.clear();
    out.push_back(std::uint8_t(n >> 8));
    out.push_back(std::uint8_t(n & 0xff));
    const int t = std::clamp(transition, 0, 0xffff);
    for (std::size_t i = 0; i < n; ++i) {
        out.push_back(std::uint8_t((ids[i] >> 8) & 0xff));
        out.push_back(std::uint8_t(ids[i] & 0xff));
        out.push_back(colors[i].r);
        out.push_back(colors[i].g);
        out.push_back(colors[i].b);
        out.push_back(0);  // W: unused by Light Panels
        out.push_back(std::uint8_t(t >> 8));
        out.push_back(std::uint8_t(t & 0xff));
    }
}

}  // namespace reforce::lights
