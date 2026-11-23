# ctest (Phase 5F): a generated movie (our own Card pattern, reforce-gen-videos)
# as a movie sprite and on the color video layer, deterministic offline
# renders, pixel-checked (red channel). Card layout (320x180): top band = frame
# index bits (block k white when bit 7-k is set, centers x = 20 + 40k, y = 22),
# red | gray 128 at y = 67, white | green at y = 135.
# Inputs: REFORCE, GEN, PNG_PROBE, WORK_DIR.
file(REMOVE_RECURSE "${WORK_DIR}")
set(P "${WORK_DIR}/scenes/Packages")
file(MAKE_DIRECTORY "${WORK_DIR}/out")
file(WRITE "${P}/G-Force.FlowField.package/Void.txt" "srcX=\"5\"\nsrcY=\"5\"\n")
file(WRITE "${P}/G-Force.FlowField.package/Still.txt" "srcX=\"x\"\nsrcY=\"y\"\n")
file(WRITE "${P}/G-Force.WaveShape.package/WaveShapes/Off.txt" "ConB=0 Stps=2\nX0=\"5\" Y0=\"5\" Pen=\"0\"\n")
file(WRITE "${P}/G-Force.ColorMap.package/Gray.txt" "R=\"i\" G=\"i\" B=\"i\"\n")
file(WRITE "${P}/G-Force.ColorMap.package/Quarter.txt" "R=\".25\" G=\".25\" B=\".25\"\n")
set(S "${P}/G-Force.Sprite.package")
file(MAKE_DIRECTORY "${S}/Movies")
file(WRITE "${S}/_default_sprite_params.txt" "X=\"0\" Y=\"0\" EPen=\"1\" Flag=7\n")
execute_process(COMMAND "${GEN}" "${S}/Movies/Clip.mov" --size 320x180 --fps 30 --frames 90 --pattern card
                RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "could not write the test movie")
endif()
file(COPY_FILE "${S}/Movies/Clip.mov" "${S}/Movies/Neg~.mov")
# A larger movie for the minified path (drawn at 1/4 size: prefiltered mips, render/VideoMips).
execute_process(COMMAND "${GEN}" "${S}/Movies/Big.mov" --size 1280x720 --fps 30 --frames 60 --pattern card
                RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "could not write the large test movie")
endif()
# Per-movie options (engine/MediaOptions): Looper loops via its own .reforce.txt.
file(COPY_FILE "${S}/Movies/Clip.mov" "${S}/Movies/Looper.mov")
file(WRITE "${S}/Movies/Looper.reforce.txt" "Loop = 1  // written by tools/media_review\n")
# Trim loops the section 1..2 s; TrimOnce plays it once (MediaOptions Start/End).
file(COPY_FILE "${S}/Movies/Clip.mov" "${S}/Movies/Trim.mov")
file(WRITE "${S}/Movies/Trim.reforce.txt" "Loop = 1\nStart = 1\nEnd = 0:02\n")
file(COPY_FILE "${S}/Movies/Clip.mov" "${S}/Movies/TrimOnce.mov")
file(WRITE "${S}/Movies/TrimOnce.reforce.txt" "Start = 1\nEnd = 2\n")
# The inverted one draws with Pen (sibling .txt replaces the defaults), so its fade shows directly.
file(WRITE "${S}/Movies/Neg~.txt" "X=\"0\" Y=\"0\" Pen=\"1\" EPen=\"0\" Flag=7\n")

function(render name)
    execute_process(
        COMMAND "${REFORCE}" --no-settings --no-audio --deterministic --fps 60 --render-size 320x180
                --scenes "${WORK_DIR}/scenes" --slideshow off --waveshape Off
                --set ColorMap.MapFunction=i --set Sprite.Transition.Duration=0.5
                --set Sprite.Transition.Duration.Deviation=0
                --out "${WORK_DIR}/out/${name}" ${ARGN}
        RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    file(WRITE "${WORK_DIR}/${name}.log" "${out}${err}")
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "reforce (${name}) exited with ${rc}:\n${err}")
    endif()
    set(LOG "${err}" PARENT_SCOPE)
endfunction()
function(expect_log want)
    string(FIND "${LOG}" "${want}" pos)
    if(pos EQUAL -1)
        message(FATAL_ERROR "missing in the log: ${want}\n${LOG}")
    endif()
endfunction()
function(probe name frame)
    execute_process(COMMAND "${PNG_PROBE}" "${WORK_DIR}/out/${name}/frame_${frame}.png" ${ARGN}
                    RESULT_VARIABLE rc OUTPUT_VARIABLE pout ERROR_VARIABLE perr)
    message(STATUS "${pout}${perr}")
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "pixel probe failed: ${name} frame ${frame}")
    endif()
endfunction()

# 1. Movie sprite from the package (default params: EPen 1, no Pen, Flag 7 = cover), on a still
#    FlowField. reforce.SpriteLayering = gforce (probe #40): EPen-only is a display-only half-way
#    lighten over black = 0.5 x Rec.601 luminance, drawn on the current frame:
#    t = 1.0 shows age .9 (full) -> media frame 27 = 00011011 (blocks 3,4,6,7 white).
render(sprite --flowfield Still --colormap Gray --render-frames 61 --save-frames 60 "--command" "0.1=start:sprite:Clip")
expect_log("Sprite \"Clip\" starts: 320x180 (file video) -> 320x180 px, flags 7")
expect_log("EPen only")
# white 128, green .587 x 128 = 75, red .299 x 128 = 38, gray 64; frame bits
probe(sprite 00060 "80,135,120,132" "240,135,67,81" "80,67,31,44" "240,67,57,69"
      "20,22,0,6" "140,22,120,132" "220,22,0,6" "260,22,120,132" "300,22,120,132")
# 1b. melt (reforce's old layering): EPen x0.5 fed back, shown one step later -> frame 26 (block 7 black).
render(sprite_melt --flowfield Still --colormap Gray --render-frames 61 --save-frames 60
       --set reforce.SpriteLayering=melt "--command" "0.1=start:sprite:Clip")
probe(sprite_melt 00060 "80,135,120,132" "260,22,120,132" "300,22,0,6")

# 2. '~' inverts a movie sprite (Pen 1, EPen 0, Void FlowField): white -> 0, black blocks -> 255.
#    t = .3: age .2 of a .5 s fade: 1-(1-.4)^1.5 = .535 -> 136.
render(neg --flowfield Void --colormap Gray --render-frames 61 --save-frames 18,60 "--command" "0.1=start:sprite:Neg~")
expect_log(", inverted")
probe(neg 00060 "80,135,0,6" "20,22,247,255")
probe(neg 00018 "20,22,128,145")

# 3. Color layer over a constant .25 gray visualizer, by library name, chroma key on green,
#    1 s crossfade from nothing; at t = 2 blend screen, opacity .5, no key.
render(layer --flowfield Void --colormap Quarter --render-frames 151 --save-frames 36,90,150
       "--command" "0=videoset:loop:on" "--command" "0=videoset:key:chroma" "--command" "0.1=video:layer:Clip"
       "--command" "2=videoset:blend:screen" "--command" "2=videoset:opacity:0.5" "--command" "2=videoset:key:none")
expect_log("video layer: \"Clip\" 320x180 starts, crossfade 1.00 s")
# t = .6: w = .5 -> white .25 + .75 * .5 = .625 (159); keyed green stays 64
probe(layer 00036 "80,135,152,166" "240,135,58,70")
# t = 1.5: full. red 255, gray 128, white 255, green keyed -> 64; media 1.4 -> frame 42 = 00101010
probe(layer 00090 "80,67,245,255" "240,67,120,136" "80,135,245,255" "240,135,58,70"
      "100,22,245,255" "140,22,0,8")
# t = 2.5: screen at .5: white mix(.25, 1, .5) = .625 (159); gray mix(.25, screen(.25,.5), .5) = .4375 (112)
probe(layer 00150 "80,135,152,166" "240,67,104,120")

# 3b. Movies play once by default: the 3 s clip started at t = .1 begins its 1 s fade-out at t = 2.1
#     and the layer is gone at t = 3.1; listed in reforce.Video.Loop it loops instead.
render(once --flowfield Void --colormap Quarter --render-frames 200 --save-frames 199 "--command" "0.1=video:layer:Clip")
expect_log("video layer: \"Clip\" ends (plays once)")
expect_log("video layer: \"Clip\" stopped")
probe(once 00199 "80,135,58,70" "80,67,58,70")
render(looplist --flowfield Void --colormap Quarter --render-frames 200 --save-frames 199
       --set "reforce.Video.Loop=clip" "--command" "0.1=video:layer:Clip")
string(FIND "${LOG}" "ends (plays once)" pos)
if(NOT pos EQUAL -1)
    message(FATAL_ERROR "reforce.Video.Loop=clip should loop:\n${LOG}")
endif()
probe(looplist 00199 "80,67,245,255")

render(sidecar --flowfield Void --colormap Quarter --render-frames 200 --save-frames 199 "--command" "0.1=video:layer:Looper")
string(FIND "${LOG}" "ends (plays once)" pos)
if(NOT pos EQUAL -1)
    message(FATAL_ERROR "Looper.reforce.txt (Loop = 1) should loop:\n${LOG}")
endif()
probe(sidecar 00199 "80,67,245,255")

# t = 2.6: age 2.5 -> media 1 + (2.5 mod 1) = 1.5 -> frame 45 = 00101101 (blocks 2,4,5,7 white)
render(trim --flowfield Void --colormap Quarter --render-frames 157 --save-frames 156 "--command" "0.1=video:layer:Trim")
probe(trim 00156 "100,22,245,255" "180,22,245,255" "220,22,245,255" "300,22,245,255"
      "20,22,0,8" "60,22,0,8" "140,22,0,8" "260,22,0,8")
string(FIND "${LOG}" "ends (plays once)" pos)
if(NOT pos EQUAL -1)
    message(FATAL_ERROR "Trim (Loop = 1) should loop:\n${LOG}")
endif()
# Played once, the 1 s section ends (fade over the 1 s crossfade) and the layer stops by t = 1.2.
render(trimonce --flowfield Void --colormap Quarter --render-frames 90 --save-frames 89 "--command" "0.1=video:layer:TrimOnce")
expect_log("video layer: \"TrimOnce\" ends (plays once)")
expect_log("video layer: \"TrimOnce\" stopped")

# 3c. "Loop until stopped": the 3 s Clip as a sprite (plays once: would end at ~3.1 s) held at t = .2
#     is still on screen at t = 6.6 (looping), and a stop fades it out.
render(held --flowfield Still --colormap Gray --render-frames 400 --save-frames 396
       "--command" "0.1=start:sprite:Clip" "--command" "0.2=videohold" "--command" "6.62=videostop:sprite")
expect_log("1 video sprite(s) loop until stopped")
expect_log("1 video sprite(s) stopped")
string(FIND "${LOG}" "Sprite \"Clip\" ended" pos)
if(NOT pos EQUAL -1)
    message(FATAL_ERROR "the held sprite ended before the stop:\n${LOG}")
endif()
probe(held 00396 "240,67,57,69")

# 4. A 1280x720 movie on the color layer at 320x180 (scale .25: sampled through the
#    half-size box filter + mips): same patches, same orientation, no crossfade.
render(minified --flowfield Void --colormap Quarter --render-frames 61 --save-frames 60
       "--command" "0=videoset:crossfade:0" "--command" "0.1=video:layer:Big")
expect_log("video layer: \"Big\" 1280x720 starts")
# t = 1: red 255 | gray 128 (y 67), white 255 | green 0 (y 135); media .9 -> frame 27 = 00011011
probe(minified 00060 "80,67,245,255" "240,67,120,136" "80,135,245,255" "240,135,0,10"
      "100,22,0,8" "140,22,245,255" "300,22,245,255")
# 5. The same large movie as a sprite (drawn 320x180: minified path): same values as case 1.
render(spritebig --flowfield Still --colormap Gray --render-frames 61 --save-frames 60 "--command" "0.1=start:sprite:Big")
expect_log("Sprite \"Big\" starts: 1280x720 (file video) -> 320x180 px")
probe(spritebig 00060 "80,135,120,132" "240,135,67,81" "80,67,31,44" "240,67,57,69"
      "20,22,0,6" "140,22,120,132" "220,22,0,6" "260,22,120,132" "300,22,120,132")
