# Syphon alpha

`reforce.Syphon.Alpha` (on by default; web UI: Video card, *Syphon alpha*) sends transparency in the Syphon output's alpha
channel. It replaces an OBS Color Key filter on the reforce source, so a camera layered behind reforce shows
through the dark parts of the visuals, while the dancers, their disco floor (and chosen movies) stay solid even where
they are dark.

Instead of a camera layered in OBS, reforce can draw the camera itself with the same key: see
[backdrop.md](backdrop.md). While that backdrop is on, the Syphon output is opaque.

## OBS setup

1. Syphon Client source `reforce`: tick **Allow Transparency**.
2. Remove the Color Key filter from it.
3. Keep the camera source below it in the scene.

## What is keyed

The output's alpha is `opaque + (1 - opaque) * key(color)`:

- **key(color)** is OBS's Color Key (v2) on black: `clamp((|rgb| - similarity) / smoothness, 0, 1)`, with `|rgb|`
  the distance from black of the 8-bit sRGB values and similarity / smoothness in OBS's units divided by 1000
  (`reforce.Syphon.KeySimilarity` = 80, `reforce.Syphon.KeySmoothness` = 50, both 1..1000, on the Video card while
  Syphon alpha is on). With the same numbers as the old OBS filter, the visuals look the same as before.
- **opaque** is built up while the frame is drawn, in the frame texture's alpha (0 after colorize):
  - Full-color movie sprites (`reforce.MovieSprite.Color = over`) with `ColorKey = 0` in their `.reforce.txt`
    raise it by their opacity. Other movies are keyed like the visuals. Movies drawn as luminance into the visuals
    (Color = off) are part of the visuals and always keyed.
  - 3D solids with `ColorKey = 0` (e.g. Rubik Cube) raise it by their coverage times their opacity, so the whole
    solid stays opaque, dark colors included (`encodeColorize`'s opaque solids: the solid target's alpha, max
    blend). Other solids are part of the visuals and keyed.
  - Dancers: the bodies set it to 1, and so does the disco floor where it covers the frame (its faded edges by
    their coverage). Floor shadows outside the floor set it to their darkness, so they darken the camera behind.

The color is never changed (straight alpha, which OBS expects). `reforce.Syphon.AlphaPremultiplied = 1`
multiplies it by the alpha for a client that wants premultiplied alpha.

## Where it happens

- `ScenePipeline::encodeColorize(..., trackAlpha)`: alpha 0 instead of 1; `ColorKey = 0` quads blend over it.
- `DancerPipeline::encode(..., trackAlpha)`: a second R8 target in the stage pass (max blend) holds the opaque
  share; the composite adds it to the frame's alpha (`a' = o + (1 - o) * a`).
- `ScenePipeline::encodeSyphonAlpha`: one full-screen pass from the frame into a separate texture that only
  Syphon gets (`App::frame`). The window shows the frame as before (its layer is opaque and ignores the alpha).

Off (`reforce.Syphon.Alpha = 0`), every alpha stays 1 and nothing extra is drawn: the plain frame goes to Syphon,
for a client without Allow Transparency or an OBS Color Key setup.

Check without OBS: `build/reforce-syphon-probe --alpha` prints the shares of transparent / partial / opaque
pixels and a coarse map of the alpha.
