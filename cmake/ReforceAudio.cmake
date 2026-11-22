# reforce_audio: CoreAudio input (AUHAL), file source (ExtAudioFile), SPSC ring
# buffer, vDSP analyzer. Plus the reforce-audio-probe CLI and audio unit tests.
# Included from the top-level CMakeLists.txt (paths are relative to it).

add_library(reforce_audio STATIC
    src/audio/Analyzer.cpp
    src/audio/BeatTracker.cpp
    src/audio/AudioFileSource.cpp
    src/audio/CoreAudioInput.cpp
    src/audio/MicPermission.mm
)
target_include_directories(reforce_audio PUBLIC src)
target_compile_options(reforce_audio PRIVATE ${REFORCE_WARNINGS})
set_source_files_properties(src/audio/MicPermission.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
target_link_libraries(reforce_audio PUBLIC
    "-framework Accelerate" "-framework AudioToolbox" "-framework CoreAudio"
    "-framework CoreFoundation" "-framework AVFoundation" "-framework Foundation")

if(REFORCE_BUILD_PROBE)
    add_executable(reforce-audio-probe tools/audio_probe.cpp)
    target_compile_options(reforce-audio-probe PRIVATE ${REFORCE_WARNINGS})
    target_link_libraries(reforce-audio-probe PRIVATE reforce_audio)
    # Phase 5C: plays a WAV to one named output device (never the default output).
    add_executable(reforce-audio-play tools/audio_play.cpp)
    target_compile_options(reforce-audio-play PRIVATE ${REFORCE_WARNINGS})
    target_link_libraries(reforce-audio-play PRIVATE
        "-framework AudioToolbox" "-framework CoreAudio" "-framework CoreFoundation")
endif()

if(REFORCE_BUILD_TESTS)
    enable_testing()
    add_executable(reforce_audio_tests
        tests/test_main.cpp
        tests/test_audio_ring.cpp
        tests/test_audio_analyzer.cpp
        tests/test_audio_file.cpp
        tests/test_audio_beat.cpp
    )
    target_compile_options(reforce_audio_tests PRIVATE ${REFORCE_WARNINGS})
    target_link_libraries(reforce_audio_tests PRIVATE reforce_audio reforce_sync doctest::doctest)
    add_test(NAME reforce_audio_tests COMMAND reforce_audio_tests)
endif()
