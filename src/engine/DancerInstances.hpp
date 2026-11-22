#pragma once
// reforce extension: one dancer body part as the GPU instance record the dancer shaders read: the Metal renderer
// (render/DancerPipeline) and the web preview's WebGL renderer (web/dancer-preview) both pack parts with this, so
// they draw exactly the same thing. Layout (float4 each, kDancerInstFloats in all):
//   m0 m1 m2 m3   model columns (scale across the axis folded into m0 / m2)
//   n0 n1 n2      normal matrix columns
//   c0            colour0, material        c1     colour1, split (local y)
//   geo           len, r0, r1, outline     clip   local plane: discard where dot(p, xyz) > w
//   face          mouth, blink, brow, strand look (hair)    eyes   eye colour, parting (hair)
//   style         pattern0, pattern1, fabric0, fabric1 (Lens: shape, side, kind, -)
//   ink           print colour, repeat     faceA  eye shape, brows, mouth style, beard
//   faceB         lashes, freckles, blush, lipstick
//   lips          lip colour (hair: the second clip plane)  beard  beard colour

#include "engine/DancerRig.hpp"

namespace reforce::engine {

inline constexpr int kDancerInstFloats = 76;
void packDancerPart(const dance::BodyPart& p, const dance::BodyOut& body, float* out);

}  // namespace reforce::engine
