# ctest: dancers repeat under --deterministic. Two offline runs with the same seed, four generated dancers mingling,
# must write byte-identical frames. (Mingle once keyed people by memory address, so the dancing changed per run.)
# Only built-in base scenes and reforce's content: needs no G-Force. Inputs: REFORCE, CONTENT, WORK_DIR.
file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}/scenes")
foreach(run a b)
    file(MAKE_DIRECTORY "${WORK_DIR}/${run}")
    execute_process(
        COMMAND "${REFORCE}" --no-settings --no-audio --deterministic --seed 11 --render-frames 360 --fps 60
                --render-size 640x360 --scenes "${WORK_DIR}/scenes" --content "${CONTENT}"
                --command "0.2=switch:dancer:Random House" --command "0.6=switch:dancer:Random Pop"
                --command "1.0=switch:dancer:Random Break" --command "1.4=switch:dancer:Random Disco"
                --out "${WORK_DIR}/${run}" --save-frames 120,240,359
        RESULT_VARIABLE rc
        OUTPUT_VARIABLE out
        ERROR_VARIABLE out)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "run ${run} failed (${rc}):\n${out}")
    endif()
    if(NOT out MATCHES "Dancer \"[^\"]+\" starts")
        message(FATAL_ERROR "run ${run}: no dancer started:\n${out}")
    endif()
endforeach()
foreach(f frame_00120.png frame_00240.png frame_00359.png)
    execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files "${WORK_DIR}/a/${f}" "${WORK_DIR}/b/${f}"
                    RESULT_VARIABLE same)
    if(NOT same EQUAL 0)
        message(FATAL_ERROR "${f} differs between two runs with the same seed: dancers are not deterministic")
    endif()
endforeach()
message(STATUS "dancers: 3 frames identical across two runs")
