# reforce_sync: Ableton Link session clock (sync/LinkClock), the reforce-link-peer
# test tool and the sync unit tests. Included from the top-level CMakeLists.txt.

add_library(reforce_sync STATIC src/sync/LinkClock.cpp src/sync/BeatClock.cpp src/sync/PhraseClock.cpp src/sync/DropDetector.cpp
    src/sync/DeckStarts.cpp src/sync/DeckFeed.cpp src/sync/ControllerMap.cpp src/sync/NowPlaying.cpp)
target_include_directories(reforce_sync PUBLIC src)
target_compile_options(reforce_sync PRIVATE ${REFORCE_WARNINGS})
# Link and asio headers are third-party: no reforce warnings for them.
target_link_libraries(reforce_sync PRIVATE Ableton::Link PUBLIC "-framework CoreMIDI" "-framework CoreFoundation")
target_link_libraries(reforce PRIVATE reforce_sync)

if(REFORCE_BUILD_PROBE)
    add_executable(reforce-link-peer tools/link_peer.cpp)
    target_compile_options(reforce-link-peer PRIVATE ${REFORCE_WARNINGS})
    target_link_libraries(reforce-link-peer PRIVATE reforce_sync)
endif()

if(REFORCE_BUILD_TESTS)
    add_executable(reforce_sync_tests tests/test_main.cpp tests/test_sync_link.cpp tests/test_sync_beatclock.cpp tests/test_sync_phrase.cpp tests/test_sync_drop.cpp tests/test_sync_deckstarts.cpp tests/test_sync_nowplaying.cpp)
    target_compile_options(reforce_sync_tests PRIVATE ${REFORCE_WARNINGS})
    target_link_libraries(reforce_sync_tests PRIVATE reforce_sync doctest::doctest)
    target_compile_definitions(reforce_sync_tests PRIVATE REFORCE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME reforce_sync_tests COMMAND reforce_sync_tests)
endif()

# Beat detection evaluation against hand-made beat grids (tools/beat_eval/beat_eval.py drives it):
#   reforce-beat-eval [--jobs N] [--max-seconds S] [--set NAME=VALUE] MANIFEST.tsv
add_executable(reforce-beat-eval tools/beat_eval/beat_eval.cpp)
target_compile_options(reforce-beat-eval PRIVATE ${REFORCE_WARNINGS})
target_link_libraries(reforce-beat-eval PRIVATE reforce_audio reforce_sync)
