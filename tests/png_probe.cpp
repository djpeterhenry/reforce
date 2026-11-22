// Pixel probe for render tests.
//   reforce_png_probe FILE X,Y,MIN,MAX [X,Y,MIN,MAX ...]
//       checks the red channel (gray ColorMap + MapFunction "i": = intensity x 255)
//       at each pixel is within [MIN, MAX]; prints the values; exit 1 on mismatch.
//       A "g:" or "b:" prefix (g:X,Y,MIN,MAX) checks the green or blue channel instead.
//   reforce_png_probe --make-test-image FILE W H
//       writes our own test card: left half white, right half black.
//   reforce_png_probe --bbox FILE THRESHOLD
//       prints "bbox X0 Y0 X1 Y1 COUNT SUM": the inclusive extent and number of pixels whose red channel is
//       above THRESHOLD, and the red channel's sum / 255 over the frame (coverage-weighted area); exit 1 if none.
//   reforce_png_probe --stats FILE X0 Y0 X1 Y1
//       prints "stats MIN MAX MEAN STDDEV" of the red channel over the inclusive rectangle (3D solid face textures).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "render/ImageFiles.hpp"

int main(int argc, char** argv) {
    if (argc >= 5 && std::strcmp(argv[1], "--make-test-image") == 0) {
        const int w = std::atoi(argv[3]), h = std::atoi(argv[4]);
        if (w <= 0 || h <= 0) return 2;
        std::vector<std::uint8_t> bgra(std::size_t(w) * h * 4, 0);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                std::uint8_t* p = &bgra[(std::size_t(y) * w + x) * 4];
                const std::uint8_t v = x < w / 2 ? 255 : 0;
                p[0] = p[1] = p[2] = v;
                p[3] = 255;
            }
        return reforce::render::writePngBgra8(argv[2], bgra.data(), w, h, std::size_t(w) * 4) ? 0 : 1;
    }
    if (argc >= 4 && std::strcmp(argv[1], "--bbox") == 0) {
        reforce::render::ImageRgba8 im;
        if (!reforce::render::loadImageRgba8(argv[2], im) || im.width == 0) {
            std::fprintf(stderr, "cannot read %s\n", argv[2]);
            return 1;
        }
        const int th = std::atoi(argv[3]);
        int x0 = im.width, y0 = im.height, x1 = -1, y1 = -1;
        long count = 0;
        double sum = 0;
        for (int y = 0; y < im.height; ++y)
            for (int x = 0; x < im.width; ++x) {
                const int v = im.rgba[(std::size_t(y) * im.width + x) * 4];
                sum += v / 255.0;
                if (v <= th) continue;
                ++count;
                x0 = x < x0 ? x : x0;
                y0 = y < y0 ? y : y0;
                x1 = x > x1 ? x : x1;
                y1 = y > y1 ? y : y1;
            }
        std::printf("bbox %d %d %d %d %ld %.1f\n", x0, y0, x1, y1, count, sum);
        return count > 0 ? 0 : 1;
    }
    if (argc >= 7 && std::strcmp(argv[1], "--stats") == 0) {
        reforce::render::ImageRgba8 im;
        if (!reforce::render::loadImageRgba8(argv[2], im) || im.width == 0) {
            std::fprintf(stderr, "cannot read %s\n", argv[2]);
            return 1;
        }
        const int x0 = std::atoi(argv[3]), y0 = std::atoi(argv[4]), x1 = std::atoi(argv[5]), y1 = std::atoi(argv[6]);
        if (x0 < 0 || y0 < 0 || x1 >= im.width || y1 >= im.height || x1 < x0 || y1 < y0) return 2;
        int lo = 255, hi = 0;
        double sum = 0, sum2 = 0;
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                const int v = im.rgba[(std::size_t(y) * im.width + x) * 4];
                lo = v < lo ? v : lo;
                hi = v > hi ? v : hi;
                sum += v;
                sum2 += double(v) * v;
            }
        const double n = double(x1 - x0 + 1) * double(y1 - y0 + 1);
        const double mean = sum / n, var = sum2 / n - mean * mean;
        std::printf("stats %d %d %.1f %.1f\n", lo, hi, mean, var > 0 ? std::sqrt(var) : 0.0);
        return 0;
    }
    if (argc < 3) return 2;
    reforce::render::ImageRgba8 img;
    if (!reforce::render::loadImageRgba8(argv[1], img) || img.width == 0) {
        std::fprintf(stderr, "cannot read %s\n", argv[1]);
        return 1;
    }
    int rc = 0;
    for (int i = 2; i < argc; ++i) {
        int x = 0, y = 0, lo = 0, hi = 0, ch = 0;
        const char* spec = argv[i];
        if ((spec[0] == 'r' || spec[0] == 'g' || spec[0] == 'b') && spec[1] == ':') {
            ch = spec[0] == 'r' ? 0 : spec[0] == 'g' ? 1 : 2;
            spec += 2;
        }
        if (std::sscanf(spec, "%d,%d,%d,%d", &x, &y, &lo, &hi) != 4 || x < 0 || y < 0 || x >= img.width ||
            y >= img.height) {
            std::fprintf(stderr, "bad probe %s\n", argv[i]);
            return 2;
        }
        const int v = img.rgba[(std::size_t(y) * img.width + x) * 4 + std::size_t(ch)];
        const bool ok = v >= lo && v <= hi;
        std::printf("%s (%d,%d)%s = %d, want %d..%d%s\n", argv[1], x, y, ch == 1 ? " green" : ch == 2 ? " blue" : "", v,
                    lo, hi, ok ? "" : "  MISMATCH");
        if (!ok) rc = 1;
    }
    return rc;
}
