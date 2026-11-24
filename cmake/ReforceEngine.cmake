# reforce_engine: pure C++ engine pieces (prefs import, FlowField builder,
# line geometry, palettes/MapFunction, scene lookup) + the M3/M4b app wiring,
# engine unit tests and the deterministic render smoke test.
# Included from the top-level CMakeLists.txt (paths are relative to it).

add_library(reforce_engine STATIC
    src/engine/Blend.cpp
    src/engine/Commands.cpp
    src/engine/FlowFieldBuilder.cpp
    src/engine/LineGeometry.cpp
    src/engine/Look.cpp
    src/engine/MediaOptions.cpp
    src/engine/CameraMoves.cpp
    src/engine/FractalZoom.cpp
    src/engine/Meshes.cpp
    src/engine/ModelFile.cpp
    src/engine/ModelScenes.cpp
    src/engine/ObjectBeat.cpp
    src/engine/MovieBeatSync.cpp
    src/engine/Log.cpp
    src/engine/Palette.cpp
    src/engine/Perf.cpp
    src/engine/Particles.cpp
    src/engine/Prefs.cpp
    src/engine/Scheduler.cpp
    src/engine/SceneLibrary.cpp
    src/engine/Sprites.cpp
    src/engine/Solids.cpp
    src/engine/Rubik.cpp
    src/engine/Fifteen.cpp
    src/engine/Rain.cpp
    src/engine/Knight.cpp
    src/engine/Labyrinth.cpp
    src/engine/Soma.cpp
    src/engine/PieceColors.cpp
    src/engine/Jigsaw.cpp
    src/engine/StickerColors.cpp
    src/engine/TreeGrowth.cpp
    src/engine/MandelDive.cpp
    src/engine/Organics.cpp
    src/engine/OrganicKinds.cpp
    src/engine/Dragon.cpp
    src/engine/arcade/Invaders.cpp
    src/engine/arcade/Hopper.cpp
    src/engine/arcade/Cycles.cpp
    src/engine/arcade/Rocks.cpp
    src/engine/arcade/Bricks.cpp
    src/engine/arcade/Chase.cpp
    src/engine/DancerRig.cpp
    src/engine/DanceMoves.cpp
    src/engine/DanceClips.cpp
    src/engine/Dancers.cpp
    src/engine/DancerScene.cpp
    src/engine/DancerInstances.cpp
    src/engine/Themes.cpp
    src/engine/Transition.cpp
    src/engine/VideoComposite.cpp
    src/engine/Body.cpp
    src/engine/LightsSettings.cpp
)
target_include_directories(reforce_engine PUBLIC src)
target_link_libraries(reforce_engine PRIVATE cgltf)
target_compile_options(reforce_engine PRIVATE ${REFORCE_WARNINGS})
target_link_libraries(reforce_engine PUBLIC reforce_scene)

# GPU scene passes + PNG I/O live in reforce_render.
target_sources(reforce_render PRIVATE
    src/render/ScenePipeline.cpp
    src/render/SolidPipeline.cpp
    src/render/JigsawPipeline.cpp
    src/render/ModelPipeline.cpp
    src/render/RainPipeline.cpp
    src/render/FractalPipeline.cpp
    src/render/Downsample.cpp
    src/render/DancerPipeline.cpp
    src/render/FieldPipeline.cpp
    src/render/ImageFiles.cpp
    src/render/TextOverlay.cpp
)
target_link_libraries(reforce_render PUBLIC reforce_engine "-framework ImageIO" "-framework CoreGraphics" "-framework CoreText")

target_sources(reforce PRIVATE
    src/app/AudioDriver.cpp
    src/app/ConfigCache.cpp
    src/app/Hotkeys.cpp
    src/app/Director.cpp
    src/app/Offline.cpp
    src/app/Visualizer.cpp
    src/app/VisualizerDancers.cpp
    src/app/VisualizerLayers.cpp
    src/app/VisualizerOrganics.cpp
    src/app/VisualizerRhythm.cpp
    src/app/VisualizerSolids.cpp
    src/app/ModelLoader.cpp
    src/app/VisualizerFractals.cpp
    src/app/VisualizerSprites.cpp
    src/app/VisualizerVideo.cpp
)
target_link_libraries(reforce PRIVATE reforce_engine reforce_audio)
target_compile_definitions(reforce PRIVATE REFORCE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

if(REFORCE_BUILD_TESTS)
    enable_testing()
    add_executable(reforce_engine_tests
        tests/test_main.cpp
        tests/test_engine_flowfield.cpp
        tests/test_engine_lines.cpp
        tests/test_engine_prefs.cpp
        tests/test_engine_look.cpp
        tests/test_engine_palette.cpp
        tests/test_engine_m5.cpp
        tests/test_engine_m6.cpp
        tests/test_engine_beatsync.cpp
        tests/test_engine_meshes.cpp
        tests/test_engine_models.cpp
        tests/test_engine_fractals.cpp
        tests/test_engine_solids.cpp
        tests/test_engine_rubik.cpp
        tests/test_engine_fifteen.cpp
        tests/test_engine_rain.cpp
        tests/test_engine_knight.cpp
        tests/test_engine_labyrinth.cpp
        tests/test_engine_soma.cpp
        tests/test_engine_jigsaw.cpp
        tests/test_engine_organics.cpp
        tests/test_engine_mandel.cpp
        tests/test_engine_arcade.cpp
        tests/test_engine_dragon.cpp
        tests/test_engine_arcade_invaders.cpp
        tests/test_engine_arcade_hopper.cpp
        tests/test_engine_arcade_cycles.cpp
        tests/test_engine_arcade_rocks.cpp
        tests/test_engine_arcade_bricks.cpp
        tests/test_engine_arcade_chase.cpp
        tests/test_engine_dancers.cpp
        tests/test_engine_danceclips.cpp
        tests/test_engine_body.cpp
        tests/test_hotkeys.cpp
        tests/test_packaging.cpp
        tests/test_perf.cpp
        src/app/ConfigCache.cpp
        src/app/Hotkeys.cpp
        src/app/LogFile.cpp
    )
    target_compile_options(reforce_engine_tests PRIVATE ${REFORCE_WARNINGS})
    target_link_libraries(reforce_engine_tests PRIVATE reforce_engine doctest::doctest)
    target_compile_definitions(reforce_engine_tests PRIVATE REFORCE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME reforce_engine_tests COMMAND reforce_engine_tests)

    # GPU allocations and render passes go through render/GpuPoison.hpp (REFORCE_GPU_POISON).
    add_test(NAME reforce_gpu_poison_lint
        COMMAND ${CMAKE_COMMAND} -DSRC_DIR=${CMAKE_CURRENT_SOURCE_DIR}/src
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/gpu_poison_lint.cmake)

    # Smoke test: deterministic offscreen render of a synthetic scene written
    # by the test itself (our own config text), checks the PNG is not blank.
    add_executable(reforce_png_check tests/png_check.cpp)
    target_link_libraries(reforce_png_check PRIVATE reforce_render)
    add_test(NAME reforce_render_smoke
        COMMAND ${CMAKE_COMMAND}
            -DREFORCE=$<TARGET_FILE:reforce>
            -DPNG_CHECK=$<TARGET_FILE:reforce_png_check>
            -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/smoke
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/render_smoke.cmake)

    # Slideshow + transitions on every layer (and a Union), deterministic,
    # with a Packages tree of our own configs written by the test.
    add_test(NAME reforce_transition_render
        COMMAND ${CMAKE_COMMAND}
            -DREFORCE=$<TARGET_FILE:reforce>
            -DPNG_CHECK=$<TARGET_FILE:reforce_png_check>
            -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/transition
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/transition_render.cmake)

    # M6: a particle and two sprites (our own config + generated test images),
    # deterministic; checks pixel intensities against the probe #19 envelope.
    add_executable(reforce_png_probe tests/png_probe.cpp)
    target_link_libraries(reforce_png_probe PRIVATE reforce_render)
    add_test(NAME reforce_particle_sprite_render
        COMMAND ${CMAKE_COMMAND}
            -DREFORCE=$<TARGET_FILE:reforce>
            -DPNG_PROBE=$<TARGET_FILE:reforce_png_probe>
            -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/m6
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/particle_sprite_render.cmake)

    # 3D solids phase 2 (PLAN 5L.2): a cube from the --solid-test hook; probes vs the CPU projection reference.
    add_test(NAME reforce_solid_render
        COMMAND ${CMAKE_COMMAND}
            -DREFORCE=$<TARGET_FILE:reforce>
            -DPNG_PROBE=$<TARGET_FILE:reforce_png_probe>
            -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/solid
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/solid_render.cmake)

    # Dancers repeat under --deterministic (two runs, same seed, identical frames).
    add_test(NAME reforce_dancer_determinism
        COMMAND ${CMAKE_COMMAND}
            -DREFORCE=$<TARGET_FILE:reforce>
            -DCONTENT=${CMAKE_CURRENT_SOURCE_DIR}/content
            -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/dancer_determinism
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/dancer_determinism_render.cmake)

    # Parity suite regression: a FlowField cut takes effect one step after a
    # WaveShape cut issued with it (G-Force's frame order).
    add_test(NAME reforce_frame_order_render
        COMMAND ${CMAKE_COMMAND}
            -DREFORCE=$<TARGET_FILE:reforce>
            -DPNG_PROBE=$<TARGET_FILE:reforce_png_probe>
            -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/frame_order
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/frame_order_render.cmake)

    # Live Look switches (intensity format, thin lines, AA, rounding, A/B) keep the trail.
    add_test(NAME reforce_look_render
        COMMAND ${CMAKE_COMMAND}
            -DREFORCE=$<TARGET_FILE:reforce>
            -DPNG_PROBE=$<TARGET_FILE:reforce_png_probe>
            -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/look
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/look_render.cmake)

    # M7a parity suite: replays the probe timelines in reforce and compares every
    # probe finding against the local G-Force captures (probes/captures/, never
    # committed). Skipped (exit 77) where the captures are absent, e.g. CI or a
    # fresh clone. Known gaps are listed in tools/parity/parity.py; only new
    # failures fail the test. The report goes to the build tree, not docs/.
    find_program(REFORCE_PYTHON3 python3)
    if(REFORCE_PYTHON3)
        add_test(NAME reforce_parity
            COMMAND ${REFORCE_PYTHON3} -I ${CMAKE_CURRENT_SOURCE_DIR}/tools/parity/parity.py --strict
                    --reforce $<TARGET_FILE:reforce>
                    --work ${CMAKE_CURRENT_BINARY_DIR}/parity
                    --out ${CMAKE_CURRENT_BINARY_DIR}/parity/parity.md)
        set_tests_properties(reforce_parity PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900)
        # Phase 5C audio parity: decodes the G-Force audio probe captures, renders the same WAVs in reforce
        # (deterministic), scores the analyzer (tools/parity/audio_compare.py). Skipped (77) without captures.
        add_test(NAME reforce_audio_parity
            COMMAND ${REFORCE_PYTHON3} -I ${CMAKE_CURRENT_SOURCE_DIR}/tools/parity/audio_compare.py --all --strict
                    --reforce $<TARGET_FILE:reforce>
                    --work ${CMAKE_CURRENT_BINARY_DIR}/audio-parity
                    --out ${CMAKE_CURRENT_BINARY_DIR}/audio-parity/audio-parity.md)
        set_tests_properties(reforce_audio_parity PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900)
    endif()
endif()

# Phase 5E: line antialiasing. GPU tests of the line pass (synthetic segments,
# offscreen) and the line lab (quality review + line-pass GPU timing):
#   reforce-line-lab review DIR | bench [--quick]
add_executable(reforce-line-lab tools/line_lab/line_lab.cpp)
target_compile_options(reforce-line-lab PRIVATE ${REFORCE_WARNINGS})
target_link_libraries(reforce-line-lab PRIVATE reforce_render)
if(REFORCE_BUILD_TESTS)
    add_executable(reforce_render_tests tests/test_main_render.cpp tests/test_render_lines.cpp tests/test_render_look.cpp
        tests/test_render_solids.cpp tests/test_render_jigsaw.cpp tests/test_render_rain.cpp tests/test_render_models.cpp tests/test_render_fractals.cpp tests/test_render_edges.cpp tests/test_render_fields.cpp tests/test_render_dancers.cpp tests/test_render_text.cpp)
    target_compile_options(reforce_render_tests PRIVATE ${REFORCE_WARNINGS})
    target_link_libraries(reforce_render_tests PRIVATE reforce_render doctest::doctest)
    target_compile_definitions(reforce_render_tests PRIVATE REFORCE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME reforce_render_tests COMMAND reforce_render_tests)
endif()

# Phase 5D: WaveShape CPU benchmark over the corpus (build only, run by hand):
#   reforce_waveshape_bench [WxH] [frames] [threads (-1 = serial)] [name filter]
if(REFORCE_BUILD_TESTS)
    add_executable(reforce_waveshape_bench tests/bench_waveshape.cpp)
    target_compile_options(reforce_waveshape_bench PRIVATE ${REFORCE_WARNINGS})
    target_compile_definitions(reforce_waveshape_bench PRIVATE
        REFORCE_SCENES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/scenes")
    target_link_libraries(reforce_waveshape_bench PRIVATE reforce_engine)
endif()
