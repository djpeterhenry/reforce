# M5b: built-in web control UI (architecture §5b).
#   reforce_web        HTTP server (cpp-httplib), JSON API, embedded UI assets
#   reforce_web_tests  API/JSON unit tests + server integration test
# Included from the top-level CMakeLists.txt (paths are relative to it).

include(FetchContent)

# --- cpp-httplib (MIT) --------------------------------------------------------
# Header-only. Pinned by commit (tag v0.65.0). SOURCE_SUBDIR points at a
# directory that does not exist so MakeAvailable only downloads it: we use the
# single header without its CMake (no OpenSSL/zlib/brotli autodetection).
FetchContent_Declare(httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG        7e5d0683ac326788e52ce3c44585c1af24b04532  # v0.65.0
    GIT_SHALLOW    FALSE
    SOURCE_SUBDIR  reforce-header-only
)
FetchContent_MakeAvailable(httplib)
add_library(reforce_httplib INTERFACE)
target_include_directories(reforce_httplib SYSTEM INTERFACE "${httplib_SOURCE_DIR}")

# --- embedded UI --------------------------------------------------------------
# Every file in src/web/ui becomes a byte array in a generated header, so the
# binary serves the UI with no files on disk (no build step for the UI itself).
file(GLOB REFORCE_WEB_UI_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/web/ui/*")
set(REFORCE_WEB_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/web")
set(REFORCE_WEB_ASSETS_HEADER "${REFORCE_WEB_GEN_DIR}/web/UiAssetsData.hpp")
add_custom_command(
    OUTPUT  "${REFORCE_WEB_ASSETS_HEADER}"
    COMMAND ${CMAKE_COMMAND}
        -DOUT=${REFORCE_WEB_ASSETS_HEADER}
        "-DFILES=${REFORCE_WEB_UI_FILES}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedFiles.cmake"
    DEPENDS ${REFORCE_WEB_UI_FILES} "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedFiles.cmake"
    COMMENT "Embedding web UI assets"
    VERBATIM)

add_library(reforce_web STATIC
    src/web/Json.cpp
    src/web/Api.cpp
    src/web/WebServer.cpp
    src/web/UiAssets.cpp
    src/web/Guests.cpp
    src/web/GuestBook.cpp
    src/web/HttpGet.mm
    "${REFORCE_WEB_ASSETS_HEADER}"
)
set_source_files_properties(src/web/HttpGet.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
target_include_directories(reforce_web PUBLIC src PRIVATE "${REFORCE_WEB_GEN_DIR}")
target_compile_options(reforce_web PRIVATE ${REFORCE_WARNINGS})
target_link_libraries(reforce_web PUBLIC reforce_engine "-framework Foundation" PRIVATE reforce_httplib)

target_link_libraries(reforce PRIVATE reforce_web)

if(REFORCE_BUILD_TESTS)
    enable_testing()
    add_executable(reforce_web_tests
        tests/test_main.cpp
        tests/test_web_api.cpp
        tests/test_web_server.cpp
        tests/test_web_guests.cpp
    )
    target_compile_options(reforce_web_tests PRIVATE ${REFORCE_WARNINGS})
    target_link_libraries(reforce_web_tests PRIVATE reforce_web reforce_httplib doctest::doctest)
    add_test(NAME reforce_web_tests COMMAND reforce_web_tests)
endif()
