// Checks that a rendered PNG is not blank: some pixels lit, not all the same.
//   reforce_png_check FILE [minLitFraction]
#include <cstdio>
#include <cstdlib>

#include "render/ImageFiles.hpp"

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    const double minLit = argc > 2 ? std::atof(argv[2]) : 0.002;
    reforce::render::ImageRgba8 img;
    if (!reforce::render::loadImageRgba8(argv[1], img) || img.width == 0) {
        std::fprintf(stderr, "cannot read %s\n", argv[1]);
        return 1;
    }
    const std::size_t n = std::size_t(img.width) * std::size_t(img.height);
    std::size_t lit = 0;
    int lo = 255, hi = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const auto* p = &img.rgba[i * 4];
        const int l = (p[0] * 2 + p[1] * 5 + p[2]) / 8;
        lit += l > 16;
        lo = l < lo ? l : lo;
        hi = l > hi ? l : hi;
    }
    const double frac = double(lit) / double(n);
    std::printf("%s: %dx%d, lit %.2f%%, luma %d..%d\n", argv[1], img.width, img.height, 100.0 * frac, lo, hi);
    return (frac >= minLit && hi - lo > 32) ? 0 : 1;
}
