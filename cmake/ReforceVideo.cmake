# Phase 5F: video sources (movie files, cameras, Syphon inputs) shared by the
# movie/live sprites and the color video layer (architecture §5c).
#   reforce_video        VideoSource (AVFoundation / CoreVideo / Syphon client)
#   reforce-gen-videos   writes our own synthetic test videos (AVAssetWriter)
#   reforce_video_tests  unit tests (spec parsing, media time, Syphon guard,
#                        deterministic frame-exact decoding, composite math)
#   reforce_video_render offline render ctest: a generated movie as a sprite
#                        and on the color layer, pixel-checked
# Included from the top-level CMakeLists.txt (paths are relative to it).

add_library(reforce_video STATIC
    src/video/VideoSpec.cpp
    src/video/CameraGate.cpp
    src/video/VideoSourceMac.mm
    src/video/BodyTracker.mm
)
target_include_directories(reforce_video PUBLIC src)
target_compile_options(reforce_video PRIVATE ${REFORCE_WARNINGS} "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>")
target_link_libraries(reforce_video
    PUBLIC  "-framework AVFoundation" "-framework CoreMedia" "-framework CoreVideo" "-framework Metal"
            "-framework Foundation" "-framework AppKit" "-framework Vision"
            reforce_engine
    PRIVATE syphon)

target_link_libraries(reforce PRIVATE reforce_video)

# Mipmapped / prefiltered sampling of minified video frames (sprites, color layer).
target_sources(reforce_render PRIVATE src/render/VideoMips.cpp)

add_executable(reforce-gen-videos tools/video/gen_videos.cpp)
target_compile_options(reforce-gen-videos PRIVATE ${REFORCE_WARNINGS})
target_link_libraries(reforce-gen-videos PRIVATE reforce_video reforce_render)
target_link_options(reforce-gen-videos PRIVATE -ObjC)

if(REFORCE_BUILD_TESTS)
    enable_testing()
    add_executable(reforce_video_tests tests/test_main.cpp tests/test_video.cpp)
    target_compile_options(reforce_video_tests PRIVATE ${REFORCE_WARNINGS})
    target_link_libraries(reforce_video_tests PRIVATE reforce_video reforce_render reforce_platform doctest::doctest)
    target_link_options(reforce_video_tests PRIVATE -ObjC)
    add_test(NAME reforce_video_tests COMMAND reforce_video_tests)
    set_tests_properties(reforce_video_tests PROPERTIES TIMEOUT 300
        ENVIRONMENT "REFORCE_VIDEO_TMP=${CMAKE_CURRENT_BINARY_DIR}/video-tests")

    add_test(NAME reforce_video_render
        COMMAND ${CMAKE_COMMAND}
            -DREFORCE=$<TARGET_FILE:reforce>
            -DGEN=$<TARGET_FILE:reforce-gen-videos>
            -DPNG_PROBE=$<TARGET_FILE:reforce_png_probe>
            -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/video-render
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/video_render.cmake)
    set_tests_properties(reforce_video_render PROPERTIES TIMEOUT 300)

    # Sprite visibility (PLAN 5K): Mix, movie levels, full-color movie sprites.
    add_test(NAME reforce_sprite_visibility_render
        COMMAND ${CMAKE_COMMAND}
            -DREFORCE=$<TARGET_FILE:reforce>
            -DGEN=$<TARGET_FILE:reforce-gen-videos>
            -DPNG_PROBE=$<TARGET_FILE:reforce_png_probe>
            -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/sprite-visibility-render
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/sprite_visibility_render.cmake)
    set_tests_properties(reforce_sprite_visibility_render PROPERTIES TIMEOUT 300)
endif()
