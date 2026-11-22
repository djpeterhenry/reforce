#pragma once
// Pure layout math (no Metal), so it can be unit-tested.

#include <algorithm>

namespace reforce::render {

struct Rect {
    double x = 0, y = 0, width = 0, height = 0;
};

// Largest rectangle with the source aspect ratio that fits inside the
// destination, centered (letterbox / pillarbox). Pixel-snapped.
inline Rect aspectFit(double srcW, double srcH, double dstW, double dstH) {
    if (srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0) return {};
    const double scale = std::min(dstW / srcW, dstH / srcH);
    const double w = static_cast<double>(static_cast<long long>(srcW * scale + 0.5));
    const double h = static_cast<double>(static_cast<long long>(srcH * scale + 0.5));
    const double x = static_cast<double>(static_cast<long long>((dstW - w) / 2));
    const double y = static_cast<double>(static_cast<long long>((dstH - h) / 2));
    return {x, y, w, h};
}

}  // namespace reforce::render
