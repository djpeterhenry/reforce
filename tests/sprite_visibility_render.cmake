# ctest (PLAN 5K): sprite visibility. Deterministic offline renders of our own content (test card images from
# reforce_png_probe, a generated movie from reforce-gen-videos), pixel-checked. A gray ColorMap with MapFunction "i"
# makes red = intensity x 255.
#   A. Sprite Mix (reforce.Sprite.Mix / reforce.MovieSprite.Mix, spritemix:sprite|movie:W) over a busy field
#      (--noise on a still FlowField).
#   B. Movie levels (reforce.MovieSprite.Gamma, per-movie Black / White / Gamma in <stem>.reforce.txt).
#   C. Full-colour movie sprites (reforce.MovieSprite.Colour = over, videoset:spritecolour:over).
# Movie: our own Card pattern (reforce-gen-videos, 320x180): red | gray 128 at y = 67, white | green at y = 135.
# Inputs: REFORCE, GEN, PNG_PROBE, WORK_DIR.
file(REMOVE_RECURSE "${WORK_DIR}")
set(P "${WORK_DIR}/scenes/Packages")
file(MAKE_DIRECTORY "${WORK_DIR}/out")
file(WRITE "${P}/G-Force.FlowField.package/Still.txt" "srcX=\"x\"\nsrcY=\"y\"\n")
file(WRITE "${P}/G-Force.FlowField.package/Void.txt" "srcX=\"5\"\nsrcY=\"5\"\n")
file(WRITE "${P}/G-Force.WaveShape.package/WaveShapes/Off.txt" "ConB=0 Stps=2\nX0=\"5\" Y0=\"5\" Pen=\"0\"\n")
file(WRITE "${P}/G-Force.ColorMap.package/Gray.txt" "R=\"i\" G=\"i\" B=\"i\"\n")
file(WRITE "${P}/G-Force.ColorMap.package/Quarter.txt" "R=\".25\" G=\".25\" B=\".25\"\n")
set(S "${P}/G-Force.Sprite.package")
file(MAKE_DIRECTORY "${S}/Test")
file(MAKE_DIRECTORY "${S}/Movies")
file(WRITE "${S}/_default_sprite_params.txt" "X=\"0\" Y=\"0\" EPen=\"1\" Flag=7\n")
# Card: left half white, right half black; EPen only (package defaults), covering the frame (360x180: white x < 160).
execute_process(COMMAND "${PNG_PROBE}" --make-test-image "${S}/Test/Card.png" 64 32 RESULT_VARIABLE rc1)
# PenCard: the same with Pen (a keyed copy into the feedback), EPen 0.
execute_process(COMMAND "${PNG_PROBE}" --make-test-image "${S}/Test/PenCard.png" 64 32 RESULT_VARIABLE rc2)
if(NOT rc1 EQUAL 0 OR NOT rc2 EQUAL 0)
    message(FATAL_ERROR "could not write the test images")
endif()
file(WRITE "${S}/Test/PenCard.txt" "X=\"0\" Y=\"0\" Pen=\"1\" EPen=\"0\" Flag=7\n")
execute_process(COMMAND "${GEN}" "${S}/Movies/Clip.mov" --size 320x180 --fps 30 --frames 90 --pattern card
                RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "could not write the test movie")
endif()
# Per-movie levels (engine/MediaOptions). Gamma 2 here times a global 0.5 = 1 (unchanged).
file(COPY_FILE "${S}/Movies/Clip.mov" "${S}/Movies/Steep.mov")
file(WRITE "${S}/Movies/Steep.reforce.txt" "Gamma = 2  // written by tools/media_review\n")
file(COPY_FILE "${S}/Movies/Clip.mov" "${S}/Movies/Lev.mov")
file(WRITE "${S}/Movies/Lev.reforce.txt" "Black = 0.25\nWhite = 0.75\n")

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
function(same a b frame)
    file(SHA256 "${WORK_DIR}/out/${a}/frame_${frame}.png" ha)
    file(SHA256 "${WORK_DIR}/out/${b}/frame_${frame}.png" hb)
    if(NOT ha STREQUAL hb)
        message(FATAL_ERROR "${a} and ${b} differ at frame ${frame}")
    endif()
endfunction()
set(NOISE --flowfield Still --colormap Gray --noise --render-frames 61 --save-frames 30,60)

# ---- A. Sprite Mix ------------------------------------------------------------------------------------------------
# Today (no setting) and Mix 0 are the same pictures (G-Force: a half-way lighten, the noise shows through).
render(today ${NOISE} "--command" "0.1=start:sprite:Card")
render(mix0 ${NOISE} --set reforce.Sprite.Mix=0 "--command" "0.1=start:sprite:Card")
same(today mix0 00030)
same(today mix0 00060)
# Black half: the field itself (noise, decayed); white half: half-way to white.
probe(mix0 00060 "250,100,140,150" "220,120,54,64" "40,40,170,186")
# Mix 1: the sprite takes over its rectangle: white 255, black 0, whatever the field.
render(mix1 ${NOISE} --set reforce.Sprite.Mix=1 "--command" "0.1=start:sprite:Card")
probe(mix1 00060 "250,100,0,0" "220,120,0,0" "300,150,0,0" "40,40,254,255" "100,100,254,255")
# Mix 0.5 (by command): the field under the black half is dimmed to half.
render(mixhalf ${NOISE} "--command" "0=spritemix:sprite:0.5" "--command" "0.1=start:sprite:Card")
expect_log("image sprite mix 0.50")
probe(mixhalf 00060 "250,100,68,77" "220,120,26,33")
# Image Mix leaves movie sprites alone and the other way round: a movie Mix of 1 keeps the image sprite G-Force.
render(moviemixonly ${NOISE} --set reforce.MovieSprite.Mix=1 "--command" "0.1=start:sprite:Card")
same(today moviemixonly 00060)
# A Pen sprite (keyed copy) gets a display-only copy with the mix: its black half hides the field on screen.
render(penmix0 ${NOISE} "--command" "0.1=start:sprite:PenCard")
probe(penmix0 00060 "250,100,140,150" "40,40,254,255")
render(penmix1 ${NOISE} --set reforce.Sprite.Mix=1 "--command" "0.1=start:sprite:PenCard")
probe(penmix1 00060 "250,100,0,0" "220,120,0,0" "40,40,254,255")
# A movie sprite with a movie Mix of 1 over the noise shows the movie's own luminance: gray 128 (not 64 + noise).
render(moviemix1 ${NOISE} --set reforce.MovieSprite.Mix=1 "--command" "0.1=start:sprite:Clip")
probe(moviemix1 00060 "240,67,123,133" "80,135,250,255" "240,135,145,155")

# ---- B. Movie levels ----------------------------------------------------------------------------------------------
# Default (G-Force): EPen-only = 0.5 x Rec.601 luminance on a still, empty field: gray 128 -> 64, red .299 -> 38.
set(MOVIE --flowfield Still --colormap Gray --render-frames 61 --save-frames 60)
render(levels_off ${MOVIE} "--command" "0.1=start:sprite:Clip")
probe(levels_off 00060 "240,67,58,70" "80,67,31,44" "80,135,120,132")
render(levels_one ${MOVIE} --set reforce.MovieSprite.Gamma=1 "--command" "0.1=start:sprite:Clip")
same(levels_off levels_one 00060)
# reforce.MovieSprite.Gamma 0.5 raises the grey levels: gray .502^.5 = .709 -> 90; red .299^.5 = .547 -> 70; white stays.
render(gamma ${MOVIE} --set reforce.MovieSprite.Gamma=0.5 "--command" "0.1=start:sprite:Clip")
expect_log("Sprite \"Clip\": levels black 0.000, white 1.000, gamma 0.500")
probe(gamma 00060 "240,67,84,96" "80,67,64,76" "80,135,120,132")
# The movie's own Gamma 2 multiplies the global 0.5: unchanged.
render(gamma_movie ${MOVIE} --set reforce.MovieSprite.Gamma=0.5 "--command" "0.1=start:sprite:Steep")
probe(gamma_movie 00060 "240,67,58,70" "80,67,31,44")
# Black .25 / White .75: red (.299 -> .098) darker, green (.587 -> .674 -> 86) brighter, white and gray (.504) stay.
render(blackwhite ${MOVIE} "--command" "0.1=start:sprite:Lev")
expect_log("Sprite \"Lev\": levels black 0.250, white 0.750, gamma 1.000")
probe(blackwhite 00060 "80,67,6,18" "240,135,80,92" "80,135,120,132" "240,67,58,70")

# ---- C. Full-colour movie sprites ---------------------------------------------------------------------------------
# Over a black visualizer (Void FlowField): the movie's own colours at alpha = envelope x (0.5 + 0.5 Mix).
# Mix 0: red patch (80,67) -> R 128, G 0; green patch (240,135) -> G 128, R 0; white -> 128 in both.
set(COLOUR --flowfield Void --colormap Gray --render-frames 61 --save-frames 60)
render(colour ${COLOUR} --set reforce.MovieSprite.Colour=over "--command" "0.1=start:sprite:Clip")
probe(colour 00060 "80,67,122,134" "g:80,67,0,6" "240,135,0,6" "g:240,135,122,134" "80,135,122,134" "g:80,135,122,134")
# By command (the panel toggle), with a movie Mix of 1: the movie itself, opaque.
render(colour_mix1 ${COLOUR} --set reforce.MovieSprite.Mix=1 "--command" "0=videoset:spritecolour:over"
       "--command" "0.1=start:sprite:Clip")
expect_log("full-colour movie sprites on (over)")
probe(colour_mix1 00060 "80,67,248,255" "g:80,67,0,6" "b:80,67,0,6" "g:240,135,248,255" "240,135,0,6"
      "240,67,122,134" "g:240,67,122,134")
# Over a visualizer: the colour sprite covers the .25 gray at half alpha: red .5 x 255 + .5 x 64 = 159, green 32.
render(colour_over ${COLOUR} --colormap Quarter --set reforce.MovieSprite.Colour=over "--command" "0.1=start:sprite:Clip")
probe(colour_over 00060 "80,67,153,165" "g:80,67,26,38")
# Off (the default) stays the luminance sprite: R = G at the red patch (38).
render(colour_off ${COLOUR} --set reforce.MovieSprite.Colour=off "--command" "0.1=start:sprite:Clip")
probe(colour_off 00060 "80,67,31,44" "g:80,67,31,44")
