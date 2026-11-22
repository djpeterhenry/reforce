#include <doctest/doctest.h>

#include "scene/SceneType.hpp"

using namespace reforce::scene;

TEST_CASE("scene types map from package directories") {
    CHECK(sceneTypeFromPackage("G-Force.FlowField Union.package") == SceneType::FlowFieldUnion);
    CHECK(sceneTypeFromPackage("G-Force.ColorMap.package") == SceneType::ColorMap);
    CHECK_FALSE(sceneTypeFromPackage("Common.Library.package").has_value());
    CHECK(sceneTypeName(SceneType::FlowFieldUnion) == "FlowField Union");
}
