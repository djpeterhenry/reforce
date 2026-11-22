# ctest smoke test: render a synthetic scene (our own config text, not G-Force
# content) deterministically offscreen and check the PNG is not blank.
# Inputs: REFORCE, PNG_CHECK, WORK_DIR.
file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}/out")
file(WRITE "${WORK_DIR}/smoke_flow.txt" "Aspc=1\nsrcR=\".98 * r\"\nsrcT=\"theta + .03\"\n")
file(WRITE "${WORK_DIR}/smoke_wave.txt"
     "Aspc=1 ConB=1 Stps=64\nX0=\".6 * cos( 2 * PI * s + t )\"\nY0=\".6 * sin( 2 * PI * s + t )\"\nPen=\"1\" LWdt=\"2\"\n")
file(WRITE "${WORK_DIR}/smoke_color.txt" "H=\".1 + .5 * i\" S=\".9\" V=\"i\"\n")
execute_process(
    COMMAND "${REFORCE}" --no-settings --no-audio --deterministic --render-frames 30 --fps 60
            --render-size 320x180
            --flowfield "${WORK_DIR}/smoke_flow.txt"
            --waveshape "${WORK_DIR}/smoke_wave.txt"
            --colormap "${WORK_DIR}/smoke_color.txt"
            --out "${WORK_DIR}/out" --save-frames 29
    RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "reforce exited with ${rc}")
endif()
execute_process(COMMAND "${PNG_CHECK}" "${WORK_DIR}/out/frame_00029.png" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "PNG check failed (${rc})")
endif()
