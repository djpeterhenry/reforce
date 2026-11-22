# ctest: live Look switches (engine/Look.hpp) in a deterministic run. A ring is
# drawn for the first 0.15 s only; an identity FlowField keeps it as a trail
# that decays 1 level per frame. At 0.25 s the trail buffer switches to hq
# (16-bit float) and at 0.4 s back to gforce (8-bit), plus thin-line / AA /
# rounding / A-B commands: the trail must survive every switch (not black,
# level within the decay budget), and the engine log must show the changes.
# Inputs: REFORCE, PNG_PROBE, WORK_DIR.
file(REMOVE_RECURSE "${WORK_DIR}")
set(P "${WORK_DIR}/scenes/Packages")
file(MAKE_DIRECTORY "${WORK_DIR}/out")
file(WRITE "${P}/G-Force.FlowField.package/Identity.txt" "Aspc=0\nsrcX=\"x\"\nsrcY=\"y\"\n")
file(WRITE "${P}/G-Force.WaveShape.package/WaveShapes/Ring.txt"
     "Aspc=0 ConB=1 Stps=200 LWdt=8\nX0=\".5 * cos(2 * PI * s) + 9 * (t > .15)\"\nY0=\".5 * sin(2 * PI * s)\"\n")
file(WRITE "${P}/G-Force.ColorMap.package/Gray.txt" "R=\"i\" G=\"i\" B=\"i\"\n")

execute_process(
    COMMAND "${REFORCE}" --no-settings --no-audio --deterministic --render-frames 40 --fps 60
            --render-size 640x360 --scenes "${WORK_DIR}/scenes" --slideshow off
            --flowfield Identity --waveshape Ring --colormap Gray --set ColorMap.MapFunction=i
            "--command" "0.25=look:intensity:hq"
            "--command" "0.3=look:thinlines:gforce" "--command" "0.3=look:flowrounding:nearest"
            "--command" "0.35=look:aa:off" "--command" "0.35=look:aa:on"
            "--command" "0.4=look:intensity:gforce"
            "--command" "0.5=look:ab" "--command" "0.55=look:ab"
            --out "${WORK_DIR}/out" --save-frames 14,15,24,39
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err)
file(WRITE "${WORK_DIR}/log.txt" "${out}${err}")
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "reforce exited with ${rc}:\n${err}")
endif()
foreach(want "look intensity = hq" "look intensity = gforce" "look thinlines = gforce" "look aa = off"
             "look A/B toggle")
    string(FIND "${out}${err}" "${want}" pos)
    if(pos EQUAL -1)
        message(FATAL_ERROR "log is missing \"${want}\":\n${out}${err}")
    endif()
endforeach()

# 640x360, ring radius .5 -> x = 160 / 480 on the center row. Last drawn at frame 8 or 9 (t <= .15);
# frame 39 = ~30 decay levels later in any format (255 - 30 = 225; allow quantization slack).
# The ring's gone from the draw (moved off screen), so nothing new is drawn after t = .15.
execute_process(COMMAND "${PNG_PROBE}" "${WORK_DIR}/out/frame_00024.png" "480,180,230,250" "320,180,0,2"
                RESULT_VARIABLE rc OUTPUT_VARIABLE pout ERROR_VARIABLE perr)
message(STATUS "${pout}${perr}")
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "look: trail lost after the hq switch")
endif()
execute_process(COMMAND "${PNG_PROBE}" "${WORK_DIR}/out/frame_00039.png" "480,180,215,240" "160,180,215,240"
                "320,180,0,2"
                RESULT_VARIABLE rc OUTPUT_VARIABLE pout ERROR_VARIABLE perr)
message(STATUS "${pout}${perr}")
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "look: trail lost after switching back to gforce")
endif()
