# ctest: deterministic offline render through slideshow transitions on all
# three layers (plus a FlowField Union), using a tiny Packages tree of our own
# configs written here (not G-Force content). Checks the switch log and that
# frames before / mid / after the transitions are not blank.
# Inputs: REFORCE, PNG_CHECK, WORK_DIR.
file(REMOVE_RECURSE "${WORK_DIR}")
set(P "${WORK_DIR}/scenes/Packages")
file(MAKE_DIRECTORY "${WORK_DIR}/out")

file(WRITE "${P}/G-Force.FlowField.package/Zoom In.txt" "Aspc=1\nsrcR=\".97 * r\"\nsrcT=\"theta\"\n")
file(WRITE "${P}/G-Force.FlowField.package/Spin.txt" "Aspc=1\nsrcR=\".99 * r\"\nsrcT=\"theta + .06\"\n")
file(WRITE "${P}/G-Force.FlowField.package/(Hidden).txt" "srcX=\"x\"\nsrcY=\"y\"\n")
file(WRITE "${P}/G-Force.FlowField Union.package/Half.txt" "ALFA=\"x > 0\"\n")
file(WRITE "${P}/G-Force.WaveShape.package/WaveShapes/Circle.txt"
     "Aspc=1 ConB=1 Stps=64\nX0=\".5 * cos( 2 * PI * s )\"\nY0=\".5 * sin( 2 * PI * s )\"\nPen=\"1\" LWdt=\"2\"\n")
file(WRITE "${P}/G-Force.WaveShape.package/WaveShapes/Two Lines.txt"
     "Aspc=0 ConB=1 Stps=32\nX0=\"2 * s - 1\" Y0=\".3\"\nX1=\"2 * s - 1\" Y1=\"-.3\"\nPen=\"1\" LWdt=\"2\"\n")
file(WRITE "${P}/G-Force.WaveShape.package/WaveShapes/(Logo).txt" "ConB=1\nX0=\"s\" Y0=\"s\"\n")
file(WRITE "${P}/G-Force.ColorMap.package/Warm.txt" "H=\".05 + .1 * i\" S=\".9\" V=\"i\"\n")
file(WRITE "${P}/G-Force.ColorMap.package/Cool.txt" "R=\"0\" G=\".6 * i\" B=\"i\"\n")

execute_process(
    COMMAND "${REFORCE}" --no-settings --no-audio --deterministic --render-frames 300 --fps 60
            --render-size 320x180 --scenes "${WORK_DIR}/scenes" --slideshow on
            --flowfield "Zoom In" --waveshape Circle --colormap Warm
            --set FlowField.Slideshow.Interval.Duration=1 --set FlowField.Transition.Duration=1.5
            --set WaveShape.Slideshow.Interval.Duration=1 --set WaveShape.Transition.Duration=1.5
            --set ColorMap.Slideshow.Interval.Duration=1 --set ColorMap.Transition.Duration=1.5
            "--set" "FlowField Union.Slideshow.Interval.Duration=0.5"
            --out "${WORK_DIR}/out" --save-frames 50,90,150,299
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err)
file(WRITE "${WORK_DIR}/log.txt" "${out}${err}")
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "reforce exited with ${rc}:\n${err}")
endif()

foreach(want
        "FlowField -> \"Spin\" (slideshow"
        "WaveShape -> \"WaveShapes/Two Lines\" (slideshow"
        "ColorMap -> \"Cool\" (slideshow"
        "union Half"
        "WaveShape now \"Two Lines\""
        "ColorMap now \"Cool\""
        "FlowField now \"Spin\"")
    string(FIND "${err}" "${want}" pos)
    if(pos EQUAL -1)
        message(FATAL_ERROR "missing in the log: ${want}\n${err}")
    endif()
endforeach()
foreach(never "(Hidden)" "(Logo)")
    string(FIND "${err}" "${never}" pos)
    if(NOT pos EQUAL -1)
        message(FATAL_ERROR "parenthesized config entered the slideshow: ${never}\n${err}")
    endif()
endforeach()

foreach(f 00050 00090 00150 00299)
    execute_process(COMMAND "${PNG_CHECK}" "${WORK_DIR}/out/frame_${f}.png" RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "PNG check failed for frame ${f}")
    endif()
endforeach()
