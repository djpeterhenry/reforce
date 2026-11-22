#include <doctest/doctest.h>

#include "render/Layout.hpp"

using reforce::render::aspectFit;

TEST_CASE("aspectFit letterboxes a 16:9 frame into a 4:3 window") {
    auto r = aspectFit(1920, 1080, 1600, 1200);
    CHECK(r.width == 1600);
    CHECK(r.height == 900);
    CHECK(r.x == 0);
    CHECK(r.y == 150);
}

TEST_CASE("aspectFit pillarboxes into a tall window") {
    auto r = aspectFit(1920, 1080, 1920, 1200);
    CHECK(r.height == 1080);
    CHECK(r.y == 60);
    r = aspectFit(1920, 1080, 2560, 1080);
    CHECK(r.width == 1920);
    CHECK(r.x == 320);
}

TEST_CASE("aspectFit handles degenerate sizes") {
    auto r = aspectFit(1920, 1080, 0, 720);
    CHECK(r.width == 0);
    CHECK(r.height == 0);
}
