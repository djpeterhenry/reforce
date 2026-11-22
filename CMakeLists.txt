cmake_minimum_required(VERSION 3.25)

# macOS only. Must be set before project().
set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING "Minimum macOS version")

project(reforce
    VERSION 0.1.0
    DESCRIPTION "Real-time music visualizer that runs G-Force scene files"
    LANGUAGES C CXX OBJC OBJCXX)

if(NOT APPLE)
    message(FATAL_ERROR "reforce currently targets macOS only (Metal + Syphon).")
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_OBJCXX_STANDARD 20)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(REFORCE_BUILD_TESTS "Build unit tests (doctest)" ON)
option(REFORCE_BUILD_PROBE "Build the reforce-syphon-probe helper" ON)

include(cmake/Dependencies.cmake)   # metal-cpp, Syphon, doctest (FetchContent, pinned)

set(REFORCE_WARNINGS -Wall -Wextra -Wno-unused-parameter)

# Let CMake order static libraries by dependency instead of repeating them
# (Apple's ld warns "ignoring duplicate libraries"). Needs CMake >= 3.31;
# older versions ignore the variable and keep the harmless warning.
set(CMAKE_LINK_LIBRARIES_STRATEGY REORDER_FREELY)

# ---------------------------------------------------------------------------
# reforce_scene: scene parser / engine (placeholder for now; no Metal deps)
# ---------------------------------------------------------------------------
add_library(reforce_scene STATIC
    src/scene/SceneType.cpp
)
target_include_directories(reforce_scene PUBLIC src)
target_compile_options(reforce_scene PRIVATE ${REFORCE_WARNINGS})

# ---------------------------------------------------------------------------
# reforce_render: Metal device objects, offscreen IOSurface target, present
# ---------------------------------------------------------------------------
add_library(reforce_render STATIC
    src/render/MetalCppImpl.cpp
    src/render/Presenter.cpp
    src/render/Renderer.cpp
    src/render/ShaderCompiler.cpp
)
target_include_directories(reforce_render PUBLIC src)
target_link_libraries(reforce_render PUBLIC metal_cpp "-framework IOSurface" "-framework CoreFoundation")
target_compile_options(reforce_render PRIVATE ${REFORCE_WARNINGS})

# ---------------------------------------------------------------------------
# reforce_platform: Cocoa shell (window, fullscreen, display link) + Syphon
# ---------------------------------------------------------------------------
add_library(reforce_platform STATIC
    src/platform/mac/MacApplication.mm
    src/platform/mac/SyphonPublisher.mm
)
target_include_directories(reforce_platform PUBLIC src)
target_compile_options(reforce_platform PRIVATE -fobjc-arc ${REFORCE_WARNINGS})
target_link_libraries(reforce_platform
    PUBLIC  "-framework Cocoa" "-framework Metal" "-framework QuartzCore"
    PRIVATE syphon)

# ---------------------------------------------------------------------------
# reforce: the app bundle
# ---------------------------------------------------------------------------
# Bundle identity (Phase 5A). scripts/install-app.sh passes the git version.
set(REFORCE_BUNDLE_ID "com.reforce.app" CACHE STRING "CFBundleIdentifier (also the code-signing identifier)")
set(REFORCE_BUNDLE_VERSION "${PROJECT_VERSION}" CACHE STRING "CFBundleVersion (install-app.sh: git describe)")
set(REFORCE_APP_ICON "${CMAKE_CURRENT_SOURCE_DIR}/resources/AppIcon.icns")
set_source_files_properties("${REFORCE_APP_ICON}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)

add_executable(reforce MACOSX_BUNDLE
    src/app/main.cpp
    src/app/App.cpp
    src/app/Describe.cpp
    src/app/LogFile.cpp
    "${REFORCE_APP_ICON}"
)
target_compile_options(reforce PRIVATE ${REFORCE_WARNINGS})
target_link_libraries(reforce PRIVATE reforce_platform reforce_render reforce_scene)
# Syphon is a static ObjC library that defines categories; make sure they load.
target_link_options(reforce PRIVATE -ObjC)
set_target_properties(reforce PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist.in"
    MACOSX_BUNDLE_BUNDLE_NAME "reforce"
    MACOSX_BUNDLE_GUI_IDENTIFIER "${REFORCE_BUNDLE_ID}"
    MACOSX_BUNDLE_BUNDLE_VERSION "${REFORCE_BUNDLE_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    MACOSX_BUNDLE_ICON_FILE "AppIcon.icns"
)
# reforce's own scene content (content/) goes into the bundle as Resources/content, so the app also works away from
# this source tree (main.cpp resolveContentDir prefers the source tree's copy when it exists). POST_BUILD only runs
# when the app relinks, so a change to any content file relinks it (LINK_DEPENDS), which also re-signs the bundle.
file(GLOB_RECURSE REFORCE_CONTENT_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/content/*")
set_property(TARGET reforce APPEND PROPERTY LINK_DEPENDS ${REFORCE_CONTENT_FILES})
add_custom_command(TARGET reforce POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E rm -rf "$<TARGET_BUNDLE_CONTENT_DIR:reforce>/Resources/content"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/content"
            "$<TARGET_BUNDLE_CONTENT_DIR:reforce>/Resources/content"
    COMMENT "Bundling content/ into reforce.app"
    VERBATIM)

# Ad-hoc sign the whole bundle (the linker only signs the inner binary).
# An ad-hoc signature's default designated requirement is its cdhash, which
# changes with every build; macOS privacy (TCC) grants such as the microphone
# are keyed on that requirement. An explicit identifier-only requirement keeps
# the same requirement across rebuilds (see docs/usage.md "Install").
add_custom_command(TARGET reforce POST_BUILD
    COMMAND codesign --force --sign - --timestamp=none --identifier "${REFORCE_BUNDLE_ID}"
            "-r=designated => identifier \"${REFORCE_BUNDLE_ID}\""
            "$<TARGET_BUNDLE_DIR:reforce>"
    COMMENT "Ad-hoc signing reforce.app"
    VERBATIM)

# ---------------------------------------------------------------------------
# Helpers and tests
# ---------------------------------------------------------------------------
if(REFORCE_BUILD_PROBE)
    add_executable(reforce-syphon-probe tests/syphon_probe.mm)
    target_compile_options(reforce-syphon-probe PRIVATE -fobjc-arc ${REFORCE_WARNINGS})
    target_link_libraries(reforce-syphon-probe PRIVATE syphon "-framework Foundation" "-framework Metal"
        "-framework CoreGraphics" "-framework ImageIO" "-framework UniformTypeIdentifiers")
    target_link_options(reforce-syphon-probe PRIVATE -ObjC)
    # Counts black / transparent frames as a client (OBS) sees them (Syphon flicker debugging).
    add_executable(reforce-syphon-monitor tests/syphon_monitor.mm)
    target_compile_options(reforce-syphon-monitor PRIVATE -fobjc-arc ${REFORCE_WARNINGS} -Wno-deprecated-declarations)
    target_link_libraries(reforce-syphon-monitor PRIVATE syphon "-framework Foundation" "-framework Metal"
        "-framework OpenGL" "-framework IOSurface")
    # How often a Syphon client (OBS, OpenGL) reads a half-written frame (tearing), per publish mode.
    add_executable(reforce-syphon-tear-test tests/syphon_tear_test.mm)
    target_compile_options(reforce-syphon-tear-test PRIVATE -fobjc-arc ${REFORCE_WARNINGS} -Wno-deprecated-declarations)
    target_link_libraries(reforce-syphon-tear-test PRIVATE syphon "-framework Foundation" "-framework Metal"
        "-framework OpenGL" "-framework IOSurface")
    target_link_options(reforce-syphon-tear-test PRIVATE -ObjC)
    target_link_options(reforce-syphon-monitor PRIVATE -ObjC)
    # Body tracking feasibility: Apple Vision person segmentation + body pose on the webcam (--window: live view).
    add_executable(reforce-body-probe tests/body_probe.mm)
    target_compile_options(reforce-body-probe PRIVATE -fobjc-arc ${REFORCE_WARNINGS})
    target_link_libraries(reforce-body-probe PRIVATE "-framework Foundation" "-framework AppKit" "-framework AVFoundation"
        "-framework CoreMedia" "-framework CoreVideo" "-framework CoreImage" "-framework CoreGraphics" "-framework Vision"
        "-framework ImageIO" "-framework UniformTypeIdentifiers" "-framework QuartzCore")
    target_link_options(reforce-body-probe PRIVATE
        "LINKER:-sectcreate,__TEXT,__info_plist,${CMAKE_CURRENT_SOURCE_DIR}/tests/body_probe-Info.plist")
endif()

if(REFORCE_BUILD_TESTS)
    enable_testing()
    add_executable(reforce_tests
        tests/test_main.cpp
        tests/test_layout.cpp
        tests/test_scene_type.cpp
    )
    target_compile_options(reforce_tests PRIVATE ${REFORCE_WARNINGS})
    target_link_libraries(reforce_tests PRIVATE reforce_scene doctest::doctest)
    add_test(NAME reforce_tests COMMAND reforce_tests)
endif()

# Audio subsystem (reforce_audio, reforce-audio-probe, reforce_audio_tests).
include(cmake/ReforceAudio.cmake)

# Expression engine + scene loader/evaluation (reforce_expr, reforce_expr_tests, reforce_expr_bench).
include(cmake/ReforceExpr.cmake)

# Engine (prefs, FlowField builder, lines, palettes), GPU scene passes, app wiring (M3/M4b).
include(cmake/ReforceEngine.cmake)

# Built-in web control UI (reforce_web, reforce_web_tests): M5b.
include(cmake/ReforceWeb.cmake)

# Room lights (reforce_lights, reforce_lights_tests): Nanoleaf panels following the visuals.
include(cmake/ReforceLights.cmake)

# Video: movie/live sprites, colour video layer, camera + Syphon inputs (Phase 5F).
include(cmake/ReforceVideo.cmake)

# Ableton Link session clock (reforce_sync, reforce-link-peer, reforce_sync_tests): Phase 5H.
include(cmake/ReforceSync.cmake)
