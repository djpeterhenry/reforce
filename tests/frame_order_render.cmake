# ctest (parity suite regression): a FlowField cut and a WaveShape cut issued in
# the same step. G-Force runs script commands between a frame's flow pass and
# its WaveShape draw, so the old WaveShape's last frame is moved once more by the
# OLD field, and the new WaveShape's first frame is not (docs/parity.md, run 1
# shot 6: zoom-out rings frozen by the identity field have no ring at r = .5,
# and the precedence dots leave no zoomed copy). All configs are written here.
# Inputs: REFORCE, PNG_PROBE, WORK_DIR.
file(REMOVE_RECURSE "${WORK_DIR}")
set(P "${WORK_DIR}/scenes/Packages")
file(MAKE_DIRECTORY "${WORK_DIR}/out")
file(WRITE "${P}/G-Force.FlowField.package/Zoom Out.txt" "Aspc=0\nsrcX=\"x * 1.5\"\nsrcY=\"y * 1.5\"\n")
file(WRITE "${P}/G-Force.FlowField.package/Identity.txt" "Aspc=0\nsrcX=\"x\"\nsrcY=\"y\"\n")
file(WRITE "${P}/G-Force.WaveShape.package/WaveShapes/Circle.txt"
     "Aspc=0 ConB=1 Stps=100 LWdt=8\nX0=\".5 * cos(2 * PI * s)\"\nY0=\".5 * sin(2 * PI * s)\"\n")
file(WRITE "${P}/G-Force.WaveShape.package/WaveShapes/Dot.txt" "Aspc=0 ConB=0 Stps=1 LWdt=12\nX0=\"-.5\"\nY0=\".5\"\n")
file(WRITE "${P}/G-Force.ColorMap.package/Gray.txt" "R=\"i\" G=\"i\" B=\"i\"\n")

execute_process(
    COMMAND "${REFORCE}" --no-settings --no-audio --deterministic --render-frames 31 --fps 60
            --render-size 640x360 --scenes "${WORK_DIR}/scenes" --slideshow off
            --flowfield "Zoom Out" --waveshape Circle --colormap Gray --set ColorMap.MapFunction=i
            "--command" "0.25=morph:flowfield:0:Identity" "--command" "0.25=morph:waveshape:0:Dot"
            --out "${WORK_DIR}/out" --save-frames 30
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err)
file(WRITE "${WORK_DIR}/log.txt" "${out}${err}")
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "reforce exited with ${rc}:\n${err}")
endif()

# 640x360, center (320,180). Ring radii .5 / 1.5^k: r = .5 -> x = 480 (must be gone), r = 1/3 -> x = 426.7
# (present). Dot at (-.5, .5) -> (160, 90); a zoomed copy would sit at (-1/3, 1/3) -> (213, 120).
execute_process(COMMAND "${PNG_PROBE}" "${WORK_DIR}/out/frame_00030.png"
                "480,180,0,8" "427,180,180,255" "160,90,250,255" "213,120,0,8"
                RESULT_VARIABLE rc OUTPUT_VARIABLE pout ERROR_VARIABLE perr)
message(STATUS "${pout}${perr}")
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "frame order: pixel probe failed")
endif()
