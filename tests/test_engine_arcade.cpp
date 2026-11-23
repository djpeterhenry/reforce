// Arcade organics (PLAN 5Q, docs/arcade-design.md): the canvas's filled shapes, shared by the arcade kinds.
#include <doctest/doctest.h>

#include <cmath>
#include <vector>

#include "arcade_harness.hpp"
#include "engine/OrganicKinds.hpp"

using namespace reforce::engine;

namespace {

// Rasterizes capsules onto a grid of pixel centres: covered = within radius of some segment.
std::vector<char> cover(const std::vector<LineSegment>& segs, int w, int h) {
    std::vector<char> g(std::size_t(w * h), 0);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float px = x + 0.5f, py = y + 0.5f;
            for (const auto& s : segs) {
                const float dx = s.bx - s.ax, dy = s.by - s.ay, l2 = dx * dx + dy * dy;
                float u = l2 > 0 ? ((px - s.ax) * dx + (py - s.ay) * dy) / l2 : 0;
                u = std::clamp(u, 0.f, 1.f);
                const float ex = s.ax + u * dx - px, ey = s.ay + u * dy - py;
                if (ex * ex + ey * ey <= s.radius * s.radius) {
                    g[std::size_t(y * w + x)] = 1;
                    break;
                }
            }
        }
    return g;
}

}  // namespace

TEST_CASE("arcade canvas: a filled rectangle covers its pixels with no gaps and stays inside") {
    const FrameMapping map = organicMapping(400, 200);  // 100 px per tree unit
    LineStyle style;
    std::vector<LineSegment> out;
    OrganicCanvas c(map, style, out, 0, 0, 1, 0);
    CHECK(c.pixelsPerUnit() == doctest::Approx(100));
    c.rect(0, 0, 0.6, 0.4, 0.7);  // 60 x 40 px around the centre (200, 100)
    REQUIRE(!out.empty());
    for (const auto& s : out) CHECK(s.penA == doctest::Approx(0.7f));
    const auto g = cover(out, 400, 200);
    int inside = 0, outside = 0;
    for (int y = 0; y < 200; ++y)
        for (int x = 0; x < 400; ++x) {
            const bool in = x >= 169 && x < 231 && y >= 79 && y < 121;  // kPixelCenter shifts by half a pixel
            const bool core = x >= 172 && x < 228 && y >= 82 && y < 118;  // away from the rounded corners
            if (core) inside += g[std::size_t(y * 400 + x)] ? 1 : 0;
            if (!in && g[std::size_t(y * 400 + x)]) ++outside;
        }
    CHECK(inside == 56 * 36);
    CHECK(outside == 0);
}

TEST_CASE("arcade canvas: polygons (rotated, concave), thin shapes, bitmaps, paths, junk") {
    const FrameMapping map = organicMapping(400, 200);
    LineStyle style;
    std::vector<LineSegment> out;
    {
        // A diamond: the centre is covered, the corners of its bounding box are not.
        OrganicCanvas c(map, style, out, 0, 0, 1, 0);
        const double xs[4] = {0, 0.5, 0, -0.5}, ys[4] = {-0.3, 0, 0.3, 0};
        c.fillPolygon(xs, ys, 4, 1);
        const auto g = cover(out, 400, 200);
        CHECK(g[std::size_t(100 * 400 + 200)]);
        CHECK(!g[std::size_t(75 * 400 + 155)]);
    }
    {
        // A U shape (concave): the notch stays empty.
        out.clear();
        OrganicCanvas c(map, style, out, 0, 0, 1, 0);
        const double xs[8] = {-0.3, 0.3, 0.3, 0.1, 0.1, -0.1, -0.1, -0.3};
        const double ys[8] = {-0.3, -0.3, 0.3, 0.3, -0.1, -0.1, 0.3, 0.3};
        c.fillPolygon(xs, ys, 8, 1);
        const auto g = cover(out, 400, 200);
        CHECK(!g[std::size_t(80 * 400 + 200)]);  // in the notch (y = +.2 is above the centre: row 80)
        CHECK(g[std::size_t(120 * 400 + 200)]);  // the bottom bar
    }
    {
        // Rotated a quarter turn by the organic's Rot: a wide rect becomes tall.
        out.clear();
        OrganicCanvas c(map, style, out, 0, 0, 1, 1.5707963267948966);
        c.rect(0, 0, 0.6, 0.1, 1);
        const auto g = cover(out, 400, 200);
        CHECK(g[std::size_t(75 * 400 + 200)]);
        CHECK(!g[std::size_t(100 * 400 + 225)]);
    }
    {
        // Thinner than a stroke: one stroke; a bitmap merges runs; mirror flips.
        out.clear();
        OrganicCanvas c(map, style, out, 0, 0, 1, 0);
        c.rect(0, 0, 0.5, 0.01, 1);
        CHECK(out.size() == 1);
        out.clear();
        const char* rows[] = {"XX.X", "...."};
        c.bitmap(rows, 2, 0, 0, 0.1, 1);  // 40 x 20 px: the top row has runs at columns 0-1 and 3
        const auto g = cover(out, 400, 200);
        CHECK(g[std::size_t(95 * 400 + 185)]);
        CHECK(!g[std::size_t(95 * 400 + 205)]);
        CHECK(g[std::size_t(95 * 400 + 215)]);
        CHECK(!g[std::size_t(105 * 400 + 185)]);
        out.clear();
        c.bitmap(rows, 2, 0, 0, 0.1, 1, true);
        const auto m = cover(out, 400, 200);
        CHECK(m[std::size_t(95 * 400 + 185)]);
        CHECK(!m[std::size_t(95 * 400 + 195)]);
        CHECK(m[std::size_t(95 * 400 + 215)]);
    }
    {
        // newPath starts a path at the current end of the output.
        out.clear();
        OrganicCanvas c(map, style, out, 0, 0, 1, 0);
        c.rect(0, 0, 0.2, 0.2, 1);
        const std::size_t n = out.size();
        c.newPath();
        c.rect(0, 0, 0.1, 0.1, 0.5);
        CHECK(out.front().path == 0.f);
        CHECK(out.back().path == float(n));
    }
    {
        // Junk: nothing, or finite.
        out.clear();
        OrganicCanvas c(map, style, out, 0, 0, 1, 0);
        const double xs[3] = {0, NAN, 1}, ys[3] = {0, 0, 1};
        c.fillPolygon(xs, ys, 3, 1);
        c.fillPolygon(xs, ys, 2, 1);
        c.rect(0, 0, 0.3, 0.3, 0);
        c.rect(0, 0, 1e12, 1e12, 1);
        CHECK(arcade_test::allFinite(out));
    }
}
