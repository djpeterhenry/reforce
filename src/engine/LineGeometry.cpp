#include "engine/LineGeometry.hpp"

#include <algorithm>
#include <cmath>

namespace reforce::engine {

double autoLineScale(double width, double height) {
    const double lo = std::max(std::min(width, height), 0.0), hi = std::max(std::max(width, height), 0.0);
    return kLineScaleBase + kLineScaleShort * lo + kLineScaleLong * hi;
}

double gforceFramePixelSize(double width, double height) {
    return width * height > kGForceMaxFramePixels ? 2.0 : 1.0;
}

double lineWidthPixels(double lwdt, const LineStyle& style, double renderWidth, double renderHeight) {
    double w = lwdt * style.scale + style.offset;
    if (style.autoScale) {
        // G-Force: LWdt * Scale * s(frame) + Offset, all in its frame pixels (run 24).
        const double px = style.sizeMode == LineScaleMode::GForce ? gforceFramePixelSize(renderWidth, renderHeight) : 1.0;
        w = (lwdt * style.scale * autoLineScale(renderWidth / px, renderHeight / px) + style.offset) * px;
    }
    if (!std::isfinite(w)) w = 0.0;
    return std::max(w, style.minWidthPx);
}

namespace {

template <class Path>
void appendPath(const Path& p, bool connected, const FrameMapping& map, const LineStyle& style,
                std::vector<LineSegment>& out) {
    // Points this far outside the frame cannot touch it (and would only cost
    // fill rate or float precision).
    const double limit = 64.0 * std::max(map.width, map.height);
    const std::size_t n = p.x.size();
    const float pathId = float(out.size());  // segments of this path share it (exact up to 2^24)
    {
        struct Pt { float x, y, pen, r; bool ok; };
        auto point = [&](std::size_t k) {
            const double px = map.toPixelX(p.x[k]), py = map.toPixelY(p.y[k]);
            const double pen = p.pen[k];
            Pt q{float(px), float(py), 0.f, 0.f, false};
            q.ok = std::isfinite(px) && std::isfinite(py) && std::abs(px) < limit && std::abs(py) < limit;
            q.pen = std::isfinite(pen) ? float(std::clamp(pen, 0.0, 1.0)) : 0.f;
            q.r = float(0.5 * lineWidthPixels(p.width[k], style, map.width, map.height));
            return q;
        };
        if (n == 0) return;
        if (!connected || n == 1) {
            for (std::size_t k = 0; k < n; ++k) {
                const Pt a = point(k);
                // A dot is drawn only if its pixel (x + 1) * W / 2 is on the grid (x = -1 is, x = +1 is not).
                const float gx = a.x - float(FrameMapping::kPixelCenter), gy = a.y - float(FrameMapping::kPixelCenter);
                const bool onGrid = gx >= 0.f && gx < float(map.width) && gy >= 0.f && gy < float(map.height);
                if (a.ok && onGrid) out.push_back({a.x, a.y, a.x, a.y, a.pen, a.pen, a.r, pathId});
            }
            return;
        }
        Pt a = point(0);
        for (std::size_t k = 1; k < n; ++k) {
            const Pt b = point(k);
            if (a.ok && b.ok) out.push_back({a.x, a.y, b.x, b.y, a.pen, b.pen, 0.5f * (a.r + b.r), pathId});
            a = b;
        }
    }
}

}  // namespace

void appendSegments(const scene::WaveShapeFrame& frame, const FrameMapping& map, const LineStyle& style,
                    std::vector<LineSegment>& out) {
    for (const scene::WavePath& p : frame.paths) appendPath(p, frame.connected, map, style, out);
}

void appendSegments(const MorphFrame& frame, const FrameMapping& map, const LineStyle& style,
                    std::vector<LineSegment>& out) {
    for (const MorphPath& p : frame) appendPath(p, p.connected, map, style, out);
}

}  // namespace reforce::engine
