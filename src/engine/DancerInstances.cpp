#include "engine/DancerInstances.hpp"

#include <cstring>

namespace reforce::engine {

namespace {
void put(float* d, dance::V3 v, float w) {
    d[0] = v.x;
    d[1] = v.y;
    d[2] = v.z;
    d[3] = w;
}
void put(float* d, dance::Rgb c, float w) {
    d[0] = c.r;
    d[1] = c.g;
    d[2] = c.b;
    d[3] = w;
}
}  // namespace

void packDancerPart(const dance::BodyPart& p, const dance::BodyOut& body, float* o) {
    const auto& f = p.frame;
    put(o + 0, f.x * p.scaleX, 0);
    put(o + 4, f.y, 0);
    put(o + 8, f.z * p.scaleZ, 0);
    put(o + 12, f.o, 1);
    put(o + 16, f.x * (1.0f / p.scaleX), 0);
    put(o + 20, f.y, 0);
    put(o + 24, f.z * (1.0f / p.scaleZ), 0);
    put(o + 28, p.color0, float(int(p.material)));
    put(o + 32, p.color1, p.split);
    o[36] = p.len;
    o[37] = p.r0;
    o[38] = p.r1;
    o[39] = p.outline ? 1.0f : 0.0f;
    std::memcpy(o + 40, p.clip.data(), 4 * sizeof(float));
    const auto& fc = body.face;
    o[44] = fc.mouth;
    o[45] = fc.blink;
    o[46] = fc.brow;
    // Hair (Material::Strands) reuses slots the face only reads on the head: face.w the strand look, eyes.w the
    // parting, the lips the second clip plane.
    o[47] = float(p.strands);
    put(o + 48, fc.eyes, p.strandPart);
    o[52] = p.pattern[0];
    o[53] = p.pattern[1];
    o[54] = p.fabric[0];
    o[55] = p.fabric[1];
    put(o + 56, p.ink, p.period);
    o[60] = float(int(fc.eyeShape));
    o[61] = float(int(fc.brows));
    o[62] = float(int(fc.mouthStyle));
    o[63] = float(int(fc.beard));
    o[64] = fc.lashes ? 1.0f : 0.0f;
    o[65] = fc.freckles ? 1.0f : 0.0f;
    o[66] = fc.blush;
    o[67] = fc.lipstick ? 1.0f : 0.0f;
    if (p.material == dance::Material::Strands) std::memcpy(o + 68, p.clip2.data(), 4 * sizeof(float));
    else put(o + 68, fc.lips, 0);
    put(o + 72, fc.hair, 0);
    if (p.bent) {
        const dance::HairBend& b = body.bend;
        const float w[6] = {b.y0, b.backZ, b.sideX, b.sideZ, 0, 0};
        for (int c = 0; c < 3; ++c) {
            put(o + 76 + c * 8, b.p2[c], w[c * 2]);
            put(o + 80 + c * 8, b.p3[c], w[c * 2 + 1]);
        }
    } else {
        std::memset(o + 76, 0, 24 * sizeof(float));
    }
}

}  // namespace reforce::engine
