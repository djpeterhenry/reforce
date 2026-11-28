#pragma once
// reforce extension: one dancer body part as the GPU instance record the dancer shaders read: the Metal renderer
// (render/DancerPipeline) and the web preview's WebGL renderer (web/dancer-preview) both pack parts with this, so
// they draw exactly the same thing. Layout (float4 each, kDancerInstFloats in all):
//   m0 m1 m2 m3   model columns (scale across the axis folded into m0 / m2)
//   n0 n1 n2      normal matrix columns
//   c0            color0, material        c1     color1, split (local y)
//   geo           len, r0, r1, outline     clip   local plane: discard where dot(p, xyz) > w
//   face          mouth, blink, brow, strand look (hair)    eyes   eye color, parting (hair)
//   style         pattern0, pattern1, fabric0, fabric1 (Lens: shape, side, kind, -)
//   ink           print color, repeat     faceA  eye shape, brows, mouth style, beard
//   faceB         lashes, freckles, blush, lipstick
//   lips          lip color (hair: the second clip plane)  beard  beard color
//   bend[6]       long hair that bends (BodyPart::bent, dance::HairBend; zero otherwise): the back, left and right
//                 columns' p2, p3 (xyz); w: y0 (> 0: bent), the back anchor's z, the left anchor's x, z, -, -

#include "engine/DancerRig.hpp"

namespace reforce::engine {

inline constexpr int kDancerInstFloats = 100;
void packDancerPart(const dance::BodyPart& p, const dance::BodyOut& body, float* out);

}  // namespace reforce::engine
