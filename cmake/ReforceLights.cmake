# Room lights that follow the visuals (docs/lights.md): Nanoleaf panels over the local network.
#   reforce_lights        panel mapping (lights/LightMap), the Nanoleaf connection thread (lights/LightsOutput)
#   reforce_lights_tests  mapping, packets, parsing, and a fake Nanoleaf (HTTP + UDP on localhost)
# Included from the top-level CMakeLists.txt after ReforceWeb.cmake (JSON, cpp-httplib).

add_library(reforce_lights STATIC src/lights/LightMap.cpp src/lights/LightsOutput.cpp)
target_include_directories(reforce_lights PUBLIC src)
target_compile_options(reforce_lights PRIVATE ${REFORCE_WARNINGS})
target_link_libraries(reforce_lights PUBLIC reforce_web PRIVATE reforce_httplib)
target_link_libraries(reforce PRIVATE reforce_lights)

# The GPU readback lives with the other passes.
target_sources(reforce_render PRIVATE src/render/LightsCapture.cpp)

if(REFORCE_BUILD_TESTS)
    add_executable(reforce_lights_tests tests/test_main.cpp tests/test_lights.cpp)
    target_compile_options(reforce_lights_tests PRIVATE ${REFORCE_WARNINGS})
    target_link_libraries(reforce_lights_tests PRIVATE reforce_lights reforce_httplib doctest::doctest)
    add_test(NAME reforce_lights_tests COMMAND reforce_lights_tests)
endif()
