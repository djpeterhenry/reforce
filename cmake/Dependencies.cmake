# Third-party dependencies, fetched at configure time with FetchContent and
# pinned to exact versions (a SHA-256 for archives, a commit hash for git).
#
# To build offline, or to try a different version, point FetchContent at a
# local checkout, e.g.
#   cmake -B build -DFETCHCONTENT_SOURCE_DIR_METAL_CPP=/path/to/metal-cpp
#   cmake -B build -DFETCHCONTENT_SOURCE_DIR_SYPHON=/path/to/Syphon-Framework

include(FetchContent)

# --- metal-cpp (Apple, Apache-2.0) -------------------------------------------
# Header-only C++ bindings for Metal / Foundation / QuartzCore.
# Apple serves this from developer.apple.com with no version control; the file
# at this URL could be replaced in place, so we pin its SHA-256. If the hash
# check fails, Apple has updated the archive: review and bump the hash.
set(REFORCE_METAL_CPP_URL "https://developer.apple.com/metal/cpp/files/metal-cpp_26.zip"
    CACHE STRING "metal-cpp archive URL")
set(REFORCE_METAL_CPP_SHA256 "4df3c078b9aadcb516212e9cb03004cbc5ce9a3e9c068fa3144d021db585a3a4"
    CACHE STRING "metal-cpp archive SHA-256")
FetchContent_Declare(metal_cpp
    URL      "${REFORCE_METAL_CPP_URL}"
    URL_HASH "SHA256=${REFORCE_METAL_CPP_SHA256}"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

# --- Syphon-Framework (BSD-2-Clause) -----------------------------------------
# No release tag contains SyphonMetalServer (tag "5" is from 2019), so pin a
# commit on main. 2026-09-21: "SyphonServer.name copies the new name".
FetchContent_Declare(syphon
    GIT_REPOSITORY https://github.com/Syphon/Syphon-Framework.git
    GIT_TAG        f4761677a45b8034a3c2069ec0f3d2553da81fba
)

# --- doctest (MIT) ------------------------------------------------------------
set(DOCTEST_NO_INSTALL ON CACHE BOOL "" FORCE)
set(DOCTEST_WITH_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG        2d0a9359a60c51affe2a9bebb1be1dca47868151  # v2.5.3
    EXCLUDE_FROM_ALL
)

# --- Ableton Link (GPL-2.0-or-later; Peter OK'd the GPL for this private project,
# 2026-11-12) ------------------------------------------------------------------
# Header-only tempo/beat/phase sync (rekordbox Performance mode "LINK"). Only
# the asio-standalone submodule is needed. SOURCE_SUBDIR points at a folder
# without a CMakeLists.txt so MakeAvailable only downloads it (no examples or
# tests); the Ableton::Link target comes from AbletonLinkConfig.cmake below.
FetchContent_Declare(ableton_link
    GIT_REPOSITORY https://github.com/Ableton/link.git
    GIT_TAG        5ba907ae027a8c2ec8824463e37296dcefc5e9be  # Link-4.1.1
    GIT_SUBMODULES "modules/asio-standalone"
    GIT_SHALLOW    FALSE
    SOURCE_SUBDIR  reforce-no-cmake
)

# metal-cpp and Syphon have no CMakeLists.txt, so MakeAvailable only downloads
# them; their targets are defined below / in third_party/syphon.
set(_reforce_fetch metal_cpp syphon ableton_link)
if(REFORCE_BUILD_TESTS)
    list(APPEND _reforce_fetch doctest)
endif()
FetchContent_MakeAvailable(${_reforce_fetch})

add_library(metal_cpp INTERFACE)
target_include_directories(metal_cpp SYSTEM INTERFACE "${metal_cpp_SOURCE_DIR}")
target_link_libraries(metal_cpp INTERFACE
    "-framework Foundation" "-framework Metal" "-framework QuartzCore")

add_subdirectory(third_party/syphon)

# AbletonLinkConfig.cmake sets CMAKE_CXX_STANDARD 17 in our scope: keep ours.
set(_reforce_cxx_std ${CMAKE_CXX_STANDARD})
include("${ableton_link_SOURCE_DIR}/AbletonLinkConfig.cmake")  # Ableton::Link
set(CMAKE_CXX_STANDARD ${_reforce_cxx_std})
