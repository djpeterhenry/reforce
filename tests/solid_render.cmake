# ctest: 3D solids phase 2 (PLAN 5L.2, docs/3d-design.md), deterministic offscreen render of the phase-2 test hook
# (--solid-test): a cube with the flat texture source (Text=2) at a fixed rotation, no spin, Pen 1 / no EPen /
# Flag 0 (a keyed copy into the feedback buffer, like a Pen sprite). A 'void' FlowField clears the buffer every frame
# and a gray ColorMap with MapFunction "i" makes pixel = intensity x 255, so the PNG shows solid_fs directly.
# MSAA edges can differ slightly across GPUs: probes and tolerances only, no hashes.
# The log carries the CPU reference (render::solidReference): the projected vertex extent, the projected hull's
# area and each front face's center and expected luminance. Checks, at 4x MSAA and at reforce.Solid.Msaa = 1:
#   - the bright region's extent matches the projected extent within 2 px, and its pixel count the hull area
#     within 3 %
#   - every front face's center is within 16 levels of its expected luminance, and the faces differ (shading)
#   - outside the solid stays black.
#
# Phase 3 (PLAN 5L.3), face textures: a cube left of center (x = -.45, its front face around x 47..245) with Pen 1
# (keyed copy into the feedback buffer), shade 0, edge 0, so face pixels = (fill + (1 - fill) * tex) x 255.
# 'Bars' draws 8 vertical lines at x = .15 .. .85 (Aspc 0), i.e. only in the frame's right half, away from the
# cube, and in the right part of the WaveShape tile (u >= .575).
#   - Text=0: the face's left part (no tile lines) is flat at the Fill floor (fill .15 -> 38, .4 -> 102); its right
#     part carries the bars (bright maximum, high spread) and never drops below the floor.
#   - Text=1: the face shows the previous frame's centered square crop, which holds the cube itself (Droste): the
#     left part, flat under Text=0, now varies; the floor still holds.
# Inputs: REFORCE, PNG_PROBE, WORK_DIR.
file(REMOVE_RECURSE "${WORK_DIR}")
set(P "${WORK_DIR}/scenes/Packages")
file(WRITE "${P}/G-Force.FlowField.package/Void.txt" "srcX=\"5\"\nsrcY=\"5\"\n")
file(WRITE "${P}/G-Force.WaveShape.package/WaveShapes/Off.txt" "ConB=0 Stps=2\nX0=\"5\" Y0=\"5\" Pen=\"0\"\n")
file(WRITE "${P}/G-Force.ColorMap.package/Gray.txt" "R=\"i\" G=\"i\" B=\"i\"\n")
file(WRITE "${P}/G-Force.WaveShape.package/WaveShapes/Bars.txt"
     "Aspc=0 ConB=1 NUM=8 Stps=16\nX0=\".15 + .1 * ID\"\nY0=\"2 * s - 1\"\nPen=\"1\" LWdt=\"2\"\n")

function(check_run name)
    set(OUT "${WORK_DIR}/${name}")
    file(MAKE_DIRECTORY "${OUT}")
    execute_process(
        COMMAND "${REFORCE}" --no-settings --no-audio --deterministic --render-frames 3 --fps 60
                --render-size 640x360 --scenes "${WORK_DIR}/scenes" --slideshow off
                --flowfield Void --waveshape Off --colormap Gray --set ColorMap.MapFunction=i
                --solid-test "cube,rotx=.5,roty=.7,scale=.6,pen=1,flag=0" ${ARGN}
                --out "${OUT}" --save-frames 2
        RESULT_VARIABLE rc
        OUTPUT_VARIABLE out
        ERROR_VARIABLE err)
    file(WRITE "${OUT}/log.txt" "${out}${err}")
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "${name}: reforce exited with ${rc}:\n${err}")
    endif()
    set(PNG "${OUT}/frame_00002.png")

    # CPU reference from the log.
    string(REGEX MATCH "extent ([0-9]+)\\.[0-9]+,([0-9]+)\\.[0-9]+ - ([0-9]+)\\.[0-9]+,([0-9]+)\\.[0-9]+ px, hull area ([0-9]+)"
           m "${err}")
    if(NOT m)
        message(FATAL_ERROR "${name}: no solid test line in the log:\n${err}")
    endif()
    set(ex0 ${CMAKE_MATCH_1})
    set(ey0 ${CMAKE_MATCH_2})
    set(ex1 ${CMAKE_MATCH_3})
    set(ey1 ${CMAKE_MATCH_4})
    set(area ${CMAKE_MATCH_5})

    # Bright region vs the projected extent and hull area.
    execute_process(COMMAND "${PNG_PROBE}" --bbox "${PNG}" 16 RESULT_VARIABLE rc OUTPUT_VARIABLE bout)
    message(STATUS "${name}: ${bout}reference extent ${ex0},${ey0} - ${ex1},${ey1}, hull area ${area}")
    if(NOT rc EQUAL 0 OR NOT bout MATCHES "bbox ([0-9]+) ([0-9]+) ([0-9]+) ([0-9]+) ([0-9]+)")
        message(FATAL_ERROR "${name}: no bright pixels")
    endif()
    set(b ${CMAKE_MATCH_1} ${CMAKE_MATCH_2} ${CMAKE_MATCH_3} ${CMAKE_MATCH_4})
    set(e ${ex0} ${ey0} ${ex1} ${ey1})
    foreach(i 0 1 2 3)
        list(GET b ${i} bv)
        list(GET e ${i} ev)
        math(EXPR d "${bv} - ${ev}")
        if(d GREATER 2 OR d LESS -2)
            message(FATAL_ERROR "${name}: bright extent ${b} vs projected ${e} (side ${i} off by ${d} px)")
        endif()
    endforeach()
    set(count ${CMAKE_MATCH_5})
    math(EXPR lo "${area} * 97 / 100")
    math(EXPR hi "${area} * 103 / 100")
    if(count LESS lo OR count GREATER hi)
        message(FATAL_ERROR "${name}: ${count} bright pixels, hull area ${area} (want ${lo}..${hi})")
    endif()

    # Front faces: center luminance; shading must differ between faces.
    string(REGEX MATCHALL "solid test face: [0-9]+\\.[0-9],[0-9]+\\.[0-9] lum [0-9]\\.[0-9]+" faces "${err}")
    list(LENGTH faces nf)
    if(nf LESS 2)
        message(FATAL_ERROR "${name}: want >= 2 front faces in the log, got ${nf}")
    endif()
    set(probes)
    set(minv 255)
    set(maxv 0)
    foreach(f ${faces})
        string(REGEX MATCH "face: ([0-9]+)\\.[0-9],([0-9]+)\\.[0-9] lum ([0-9])\\.([0-9][0-9][0-9])" _ "${f}")
        math(EXPR v "(${CMAKE_MATCH_3} * 1000 + ${CMAKE_MATCH_4}) * 255 / 1000")
        math(EXPR vlo "${v} - 16")
        math(EXPR vhi "${v} + 16")
        list(APPEND probes "${CMAKE_MATCH_1},${CMAKE_MATCH_2},${vlo},${vhi}")
        if(v LESS minv)
            set(minv ${v})
        endif()
        if(v GREATER maxv)
            set(maxv ${v})
        endif()
    endforeach()
    math(EXPR spread "${maxv} - ${minv}")
    if(spread LESS 40)
        message(FATAL_ERROR "${name}: face shading spread only ${spread} levels")
    endif()
    # Outside: the corners and 4 px beyond each side of the extent.
    math(EXPR cx "(${ex0} + ${ex1}) / 2")
    math(EXPR cy "(${ey0} + ${ey1}) / 2")
    math(EXPR l "${ex0} - 4")
    math(EXPR r "${ex1} + 5")
    math(EXPR t "${ey0} - 4")
    math(EXPR bo "${ey1} + 5")
    list(APPEND probes "0,0,0,2" "639,0,0,2" "0,359,0,2" "639,359,0,2"
         "${l},${cy},0,2" "${r},${cy},0,2" "${cx},${t},0,2" "${cx},${bo},0,2")
    execute_process(COMMAND "${PNG_PROBE}" "${PNG}" ${probes} RESULT_VARIABLE rc OUTPUT_VARIABLE pout ERROR_VARIABLE perr)
    message(STATUS "${pout}${perr}")
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "${name}: pixel probe failed")
    endif()
endfunction()

check_run(msaa4)
check_run(msaa1 --set reforce.Solid.Msaa=1)

# ---- phase 3: face textures
function(texture_run name text fill)
    set(OUT "${WORK_DIR}/${name}")
    file(MAKE_DIRECTORY "${OUT}")
    execute_process(
        COMMAND "${REFORCE}" --no-settings --no-audio --deterministic --render-frames 4 --fps 60
                --render-size 640x360 --scenes "${WORK_DIR}/scenes" --slideshow off
                --flowfield Void --waveshape Bars --colormap Gray --set ColorMap.MapFunction=i
                --solid-test "cube,x=-.45,scale=.8,shade=0,edge=0,pen=1,flag=0,text=${text},fill=${fill}"
                --out "${OUT}" --save-frames 3
        RESULT_VARIABLE rc
        OUTPUT_VARIABLE out
        ERROR_VARIABLE err)
    file(WRITE "${OUT}/log.txt" "${out}${err}")
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "${name}: reforce exited with ${rc}:\n${err}")
    endif()
    foreach(region "left;60 100 140 260" "right;165 100 235 260")
        list(GET region 0 rname)
        list(GET region 1 rect)
        separate_arguments(rect)
        execute_process(COMMAND "${PNG_PROBE}" --stats "${OUT}/frame_00003.png" ${rect} RESULT_VARIABLE rc
                        OUTPUT_VARIABLE sout)
        if(NOT rc EQUAL 0 OR NOT sout MATCHES "stats ([0-9]+) ([0-9]+) ([0-9.]+) ([0-9]+)")
            message(FATAL_ERROR "${name}: stats probe failed: ${sout}")
        endif()
        message(STATUS "${name} ${rname}: ${sout}")
        set(${rname}_min ${CMAKE_MATCH_1} PARENT_SCOPE)
        set(${rname}_max ${CMAKE_MATCH_2} PARENT_SCOPE)
        set(${rname}_sd ${CMAKE_MATCH_4} PARENT_SCOPE)
    endforeach()
endfunction()

foreach(fill .15 .4)
    if(fill STREQUAL ".15")
        set(floor 38)
    else()
        set(floor 102)
    endif()
    math(EXPR flo "${floor} - 3")
    math(EXPR fhi "${floor} + 3")
    texture_run(text0_fill${fill} 0 ${fill})
    # Left: no tile lines -> flat at the Fill floor.
    if(left_min LESS flo OR left_max GREATER fhi)
        message(FATAL_ERROR "text0 fill ${fill}: left of face ${left_min}..${left_max}, want the floor ${floor} +-3")
    endif()
    # Right: the bars (texture present), never below the floor.
    if(right_min LESS flo OR right_max LESS 150 OR right_sd LESS 20)
        message(FATAL_ERROR "text0 fill ${fill}: right of face min ${right_min} max ${right_max} sd ${right_sd}: "
                            "want >= ${flo}, max >= 150, sd >= 20")
    endif()
endforeach()

texture_run(text1 1 .15)
if(left_min LESS 35 OR left_sd LESS 10 OR left_max LESS 80)
    message(FATAL_ERROR "text1: left of face min ${left_min} max ${left_max} sd ${left_sd}: want the frame (the cube "
                        "itself) there: min >= 35, max >= 80, sd >= 10")
endif()
if(right_min LESS 35 OR right_sd LESS 10 OR right_max LESS 120)
    message(FATAL_ERROR "text1: right of face min ${right_min} max ${right_max} sd ${right_sd}")
endif()

# ---- phase 4 (PLAN 5L.4): solid scene files (reforce.Solid.package under --content), started by command or theme.
# A scene file with the pose of the msaa4 run above (no fade: Object.Transition.Duration.Quick = 0) must give the very
# same frame as the test hook; a moving Text=0 solid renders the same twice (deterministic); a theme with
# Object.Slideshow.Interval.Duration starts solids from the slideshow.
set(C "${WORK_DIR}/content/Packages/reforce.Solid.package")
file(WRITE "${C}/Probe.txt"
     "Vers=100\nMesh=\"cube\"\nText=\"2\"\nRotX=\".5\"\nRotY=\".7\"\nScal=\".6\"\nPen=\"1\"\nFlag=\"0\"\n")
file(WRITE "${C}/Drift.txt"
     "Vers=100\nMesh=\"icosahedron\"\nText=\"0\"\nB0=\"t - START_TIME\"\nX=\".3 * sin(B0)\"\nZ=\".5 * sin(.7 * B0)\"\n"
     "RotX=\".35\"\nRotY=\".5 * B0\"\nPen=\"1\"\nFlag=\"0\"\n")
file(WRITE "${WORK_DIR}/content/Themes/Solids Test.txt"
     "ThemeName = \"Solids Test\"\nObject.Slideshow.Interval.Duration = 0.5\n")

function(scene_run name frames)
    set(OUT "${WORK_DIR}/${name}")
    file(MAKE_DIRECTORY "${OUT}")
    math(EXPR last "${frames} - 1")
    execute_process(
        COMMAND "${REFORCE}" --no-settings --no-audio --deterministic --render-frames ${frames} --fps 60
                --render-size 640x360 --scenes "${WORK_DIR}/scenes" --content "${WORK_DIR}/content"
                --flowfield Void --colormap Gray --set ColorMap.MapFunction=i
                --set Object.Transition.Duration.Quick=0 ${ARGN}
                --out "${OUT}" --save-frames ${last}
        RESULT_VARIABLE rc
        OUTPUT_VARIABLE out
        ERROR_VARIABLE err)
    file(WRITE "${OUT}/log.txt" "${out}${err}")
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "${name}: reforce exited with ${rc}:\n${err}")
    endif()
    string(LENGTH "${last}" n)
    set(pad "00000${last}")
    string(SUBSTRING "${pad}" ${n} 5 pad)  # frame_00029.png
    set(${name}_png "${OUT}/frame_${pad}.png" PARENT_SCOPE)
    set(${name}_log "${err}" PARENT_SCOPE)
endfunction()

scene_run(scene_probe 3 --slideshow off --waveshape Off --command "0=switch:object:Probe")
file(SHA256 "${scene_probe_png}" h_scene)
file(SHA256 "${WORK_DIR}/msaa4/frame_00002.png" h_hook)
if(NOT h_scene STREQUAL h_hook)
    message(FATAL_ERROR "scene file solid differs from the test hook at the same pose:\n${scene_probe_png}\n"
                        "${WORK_DIR}/msaa4/frame_00002.png")
endif()
if(NOT scene_probe_log MATCHES "Solid \"Probe\" starts: cube, Text=2")
    message(FATAL_ERROR "scene_probe: no start line in the log:\n${scene_probe_log}")
endif()

scene_run(scene_drift_a 30 --slideshow off --waveshape Bars --command "0=switch:object:Drift")
scene_run(scene_drift_b 30 --slideshow off --waveshape Bars --command "0=switch:object:Drift")
file(SHA256 "${scene_drift_a_png}" ha)
file(SHA256 "${scene_drift_b_png}" hb)
if(NOT ha STREQUAL hb)
    message(FATAL_ERROR "moving Text=0 solid is not deterministic: ${scene_drift_a_png} vs ${scene_drift_b_png}")
endif()
execute_process(COMMAND "${PNG_PROBE}" --bbox "${scene_drift_a_png}" 16 RESULT_VARIABLE rc OUTPUT_VARIABLE bout)
if(NOT rc EQUAL 0 OR NOT bout MATCHES "bbox ([0-9]+) ([0-9]+) ([0-9]+) ([0-9]+) ([0-9]+)" OR CMAKE_MATCH_5 LESS 5000)
    message(FATAL_ERROR "scene_drift: the solid is missing: ${bout}")
endif()

scene_run(scene_theme 75 --slideshow on --waveshape Off --theme "Solids Test"
          --set Object.Slideshow.Interval.Duration.Deviation=0)
string(REGEX MATCHALL "Object \\+ \"[A-Za-z]+\" \\(slideshow" starts "${scene_theme_log}")
list(LENGTH starts ns)
if(ns LESS 1)
    message(FATAL_ERROR "scene_theme: the theme started no solid:\n${scene_theme_log}")
endif()
message(STATUS "phase 4: scene file = hook, deterministic drift, theme started ${ns} solid(s)")
