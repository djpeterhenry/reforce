#include "engine/DanceMoves.hpp"

#include "engine/DanceClips.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace reforce::engine::dance {

namespace {

constexpr float TAU = 6.28318530718f;
constexpr float PI = 3.14159265359f;

float fr(float x) { return x - std::floor(x); }
float clampf(float v, float lo, float hi) { return std::min(hi, std::max(lo, v)); }
float smooth(float t) {
    t = clampf(t, 0, 1);
    return t * t * (3 - 2 * t);
}
// sin / cos over a period (in beats) with a phase (in periods).
float sn(float b, float per, float ph = 0) { return std::sin(TAU * (b / per + ph)); }
float cs(float b, float per, float ph = 0) { return std::cos(TAU * (b / per + ph)); }
// 1 on every beat, falling off between (k: sharper with more).
float onBeat(float b, float k = 1.5f) { return std::pow(0.5f + 0.5f * std::cos(TAU * b), k); }
// Snappy transition inside a cycle position t (0..1): sharp moves arrive early and hold.
float snap(float t, float sharp) {
    const float len = clampf(1.0f - 0.38f * sharp, 0.15f, 1.0f);
    return smooth(t / len);
}
// Square-ish alternation (+1 / -1) every `per` beats, with eased switches.
float alt(float b, float per, float sharp) {
    const float c = fr(b / (2 * per));
    const float h = c < 0.5f ? snap(c * 2, sharp) : 1 - snap((c - 0.5f) * 2, sharp);
    return 2 * h - 1;
}
// Bump: 0 at the cycle edges, 1 in the middle (lifts, jumps).
float bump(float t) { return std::sin(PI * clampf(t, 0, 1)); }

V3 L(float x, float y, float z) { return {x, y, z}; }
V3 R(float x, float y, float z) { return {-x, y, z}; }  // the same target on the right side

// Common pelvis groove: hip-hop style drops on the beat; house style bounces up.
void groove(float b, const MoveCtx& c, DancePose& p, float depth) {
    p.hip.y -= depth * c.amp * onBeat(b, 1.2f + c.sharp);
}


// Footwork from keyframes on whole beats (cyclic): offsets (x, z) of each foot from its rest spot, times `amp`. A foot
// that moves between two keys lifts on the way (`lift`), so steps look like steps and nothing slides or teleports.
struct FeetKey {
    float lx, lz, rx, rz;
};
void keyFeet(float b, const FeetKey* keys, int n, const MoveCtx& c, DancePose& p, float lift = 0.05f) {
    const int k = int(std::floor(b)) % n, k2 = (k + 1) % n;
    const float in = snap(fr(b), c.sharp);
    const FeetKey& a = keys[k];
    const FeetKey& d = keys[k2];
    auto lerpf = [&](float u, float v) { return (u + (v - u) * in) * c.amp; };
    const float up = bump(in);
    const bool lMoves = std::fabs(d.lx - a.lx) + std::fabs(d.lz - a.lz) > 1e-3f;
    const bool rMoves = std::fabs(d.rx - a.rx) + std::fabs(d.rz - a.rz) > 1e-3f;
    p.footL = p.footL + V3{lerpf(a.lx, d.lx), lMoves ? lift * up : 0.0f, lerpf(a.lz, d.lz)};
    p.footR = p.footR + V3{lerpf(a.rx, d.rx), rMoves ? lift * up : 0.0f, lerpf(a.rz, d.rz)};
    // The pelvis rides over the feet, leaning onto the standing leg.
    const float mx = (p.footL.x + p.footR.x) * 0.5f, mz = (p.footL.z + p.footR.z) * 0.5f;
    p.hip.x += mx * 0.85f;
    p.hip.z += mz * 0.6f;
    if (lMoves != rMoves) p.hipRoll += (lMoves ? -0.07f : 0.07f) * up;
}
// 0..1 weight that is 1 on the even `per`-beat blocks and 0 on the odd ones, eased over the switch (centered on it).
float sideWeight(float b, float per, float ease = 0.35f) {
    const float x = b / per;  // switches at whole numbers
    const float f = fr(x);
    const bool even = (long(std::floor(x)) % 2) == 0;
    const float e = ease / per;
    float w = even ? 1.0f : 0.0f;
    if (f > 1 - e) w = even ? 1 - smooth((f - (1 - e)) / (2 * e)) * 1.0f : smooth((f - (1 - e)) / (2 * e));
    if (f < e) w = even ? 0.5f + 0.5f * smooth(f / e) : 0.5f - 0.5f * smooth(f / e);
    return w;
}

// ---------------------------------------------------------------- legs
void legsGroove(float b, const MoveCtx& c, DancePose& p) {
    groove(b, c, p, 0.035f);
    const float w = sn(b, 2) * 0.02f * c.amp;
    p.hip.x += w;
    p.hipRoll += -w * 1.5f;
    if (c.variant & 1) {  // weight shift with a heel lift
        p.footPitchL = std::max(0.0f, -sn(b, 2)) * 0.25f;
        p.footPitchR = std::max(0.0f, sn(b, 2)) * 0.25f;
    }
}

void legsStepTouch(float b, const MoveCtx& c, DancePose& p) {
    // 4 beats: the right foot steps out (1), the left closes (2), the left steps back out (3), the right closes (4).
    const float w = (c.variant & 1) ? 0.16f : 0.11f;
    const FeetKey keys[4] = {{0, 0, 0, 0}, {0, 0, -w, 0}, {-w, 0, -w, 0}, {0, 0, -w, 0}};
    keyFeet(b, keys, 4, c, p, 0.045f);
    groove(b, c, p, 0.03f);
}

void legsTwoStep(float b, const MoveCtx& c, DancePose& p) {
    // Hip-hop two-step: out on 1, together on 2, out the other way on 3, together on 4; down groove.
    const int beat = int(std::floor(b)) % 4;
    const float in = snap(fr(b), c.sharp);
    const float w = 0.13f * c.amp;
    float lx = c.foot, rx = -c.foot, ly = 0, ry = 0;
    switch (beat) {
        case 0: lx += w * in; ly = bump(in) * 0.05f; break;
        case 1: lx += w; rx += w * in; ry = bump(in) * 0.05f; break;
        case 2: lx += w; rx += w - w * in * 2; ry = bump(in) * 0.05f; break;
        case 3: lx += w - w * in; rx -= w * (1 - in); ly = bump(in) * 0.05f; break;
    }
    p.footL = {lx, ly, 0};
    p.footR = {rx, ry, 0};
    p.hip.x += (lx + rx) * 0.5f;
    p.hipYaw += sn(b, 4) * 0.15f;
    groove(b, c, p, 0.05f);
    p.chestPitch += onBeat(b, 2) * 0.08f * c.amp;
}

void legsKickBall(float b, const MoveCtx& c, DancePose& p) {
    // Kick (1), ball-change (and-2): kick, step back on the ball, change weight.
    const float cyc = fr(b / 2);
    const bool left = (int(std::floor(b / 2)) % 2 == 0) != bool(c.variant & 1);
    const float kick = cyc < 0.5f ? bump(cyc * 2) : 0.0f;
    const float ball = cyc >= 0.5f ? bump((cyc - 0.5f) * 2) : 0.0f;
    V3& f = left ? p.footL : p.footR;
    f.z += kick * 0.2f * c.amp - ball * 0.06f;
    f.y += kick * 0.12f * c.amp + ball * 0.03f;
    (left ? p.footPitchL : p.footPitchR) = kick * 0.4f - ball * 0.5f;
    p.hip.z -= kick * 0.025f;
    p.chestPitch -= kick * 0.08f;
    groove(b, c, p, 0.03f);
}

void legsRunningMan(float b, const MoveCtx& c, DancePose& p) {
    // Each foot: lifts and comes down in front (half a beat), then slides back flat for a beat and a half; the feet are
    // a beat apart, so one knee is always coming up as the other foot slides back.
    auto foot = [&](float ph, V3& f, float& pitch) {
        ph = fr(ph / 2) * 2;  // 0..2
        if (ph < 0.5f) {
            const float t = ph / 0.5f;
            f.z += (-0.11f + 0.17f * smooth(t)) * c.amp;
            f.y += 0.15f * bump(t) * c.amp;
            pitch = 0.3f * bump(t);
        } else {
            const float t = (ph - 0.5f) / 1.5f;
            f.z += (0.06f - 0.17f * t) * c.amp;
        }
    };
    foot(b, p.footL, p.footPitchL);
    foot(b + 1, p.footR, p.footPitchR);
    p.hip.y -= 0.04f * onBeat(b, 2) * c.amp;
    p.chestPitch += 0.1f;
    p.hip.z -= 0.02f;
}

void legsTStep(float b, const MoveCtx& c, DancePose& p) {
    // Shuffle T-step: the standing foot twists heel-toe (8ths) while the other kicks out sideways; 4 beats each side.
    const float e = fr(b * 2);
    const float twist = (int(std::floor(b * 2)) % 2 == 0 ? 1.0f : -1.0f) * snap(e, c.sharp);
    const float wl = (c.variant & 1) ? 1 - sideWeight(b, 4) : sideWeight(b, 4);  // 1 = the left foot kicks
    const float u = fr(b / 4), env = std::min(1.0f, 5.0f * std::min(u, 1 - u));  // kick in and out of each side
    const float kick = (0.1f + 0.06f * bump(e)) * c.amp * env, kickY = 0.06f * bump(e) * c.amp * env;
    p.footL.x += kick * wl;
    p.footL.y += kickY * wl;
    p.footR.x -= kick * (1 - wl);
    p.footR.y += kickY * (1 - wl);
    p.footYawR = twist * 0.5f * wl * env;
    p.footYawL = twist * 0.5f * (1 - wl) * env;
    p.bodyX += 0.05f * sn(b, 8);
    p.hip.y -= 0.03f * onBeat(b * 2, 2);
}

void legsHeelToe(float b, const MoveCtx& c, DancePose& p) {
    // House shuffle: heel, toe on the 8ths, one foot then the other, with an up-bounce.
    const float e = fr(b * 2);
    const bool left = int(std::floor(b)) % 2 == 0;
    V3& f = left ? p.footL : p.footR;
    float& pitch = left ? p.footPitchL : p.footPitchR;
    const bool heel = int(std::floor(b * 2)) % 2 == 0;
    f.z += 0.1f * bump(e) * c.amp;
    f.y += 0.04f * bump(e);
    pitch = (heel ? 0.5f : -0.4f) * bump(e);
    p.hip.y += 0.025f * (1 - onBeat(b, 2)) * c.amp;  // up on the off-beat
    p.chestPitch += 0.06f * onBeat(b, 1);
}

void legsJack(float b, const MoveCtx& c, DancePose& p) {
    // House jack: the chest and pelvis wave forward-back on every beat, knees soft.
    const float j = onBeat(b, 1.0f + c.sharp);
    p.hip.y -= 0.035f * j * c.amp;
    p.hip.z += 0.03f * (j - 0.5f) * c.amp;
    p.chestPitch += (0.18f * j - 0.06f) * c.amp;
    p.headPitch += -0.12f * j;
    if (c.variant & 1) {
        p.footL.x += 0.04f;
        p.footR.x -= 0.04f;
    }
}

void legsLatinSide(float b, const MoveCtx& c, DancePose& p) {
    // Cumbia / merengue side steps: small steps on every beat with a strong hip sway.
    const float s = alt(b, 1, c.sharp);
    p.footL.x += 0.04f * s * c.amp;
    p.footR.x += 0.04f * s * c.amp;
    (s > 0 ? p.footR : p.footL).y += 0.025f * bump(fr(b));
    p.hip.x += 0.045f * s * c.amp;
    p.hipRoll += -0.2f * s * c.amp;
    p.chestRoll += 0.12f * s;
    p.hip.y -= 0.02f * onBeat(b, 2);
}

void legsTwist(float b, const MoveCtx& c, DancePose& p) {
    // The twist: low, hips swivel against the shoulders, on the balls of the feet.
    const float s = sn(b, 1);
    p.hip.y -= (0.06f + 0.02f * (c.variant & 1)) * c.amp;
    p.hipYaw += 0.5f * s * c.amp;
    p.chestYaw += -0.45f * s * c.amp;
    p.footYawL = 0.4f * s;
    p.footYawR = -0.4f * s;
    p.footPitchL = p.footPitchR = -0.25f;
    p.footL.x += 0.02f;
    p.footR.x -= 0.02f;
    if (c.variant & 2) {  // going down
        p.hip.y -= 0.05f * (0.5f - 0.5f * cs(b, 8));
    }
}

void legsCharleston(float b, const MoveCtx& c, DancePose& p) {
    // Kick forward (1), back to center (2), kick back (3), center (4) with the other foot; toes swivel.
    const int beat = int(std::floor(b)) % 4;
    const float in = bump(fr(b));
    const bool left = (int(std::floor(b / 4)) % 2 == 0) != bool(c.variant & 1);
    V3& f = left ? p.footL : p.footR;
    if (beat == 0) {
        f.z += 0.18f * in * c.amp;
        f.y += 0.1f * in;
    } else if (beat == 2) {
        f.z -= 0.15f * in * c.amp;
        f.y += 0.09f * in;
    }
    p.footYawL = 0.45f * sn(b, 1);
    p.footYawR = -0.45f * sn(b, 1);
    p.hip.y -= 0.03f * onBeat(b);
    p.chestPitch += (beat == 0 ? -0.1f : beat == 2 ? 0.12f : 0.0f) * in;
}

void legsCrissCross(float b, const MoveCtx& c, DancePose& p) {
    // Jump-cross: feet crossed on 1, open on 2 (the change happens in the air), the front foot alternating.
    const int k = int(std::floor(b));
    auto stance = [&](int beat, V3& l, V3& r) {
        if (beat % 2 == 0) {  // crossed
            const bool leftFront = (beat / 2) % 2 == 0;
            l = {-0.03f, 0, leftFront ? 0.05f : -0.03f};
            r = {0.03f, 0, leftFront ? -0.03f : 0.05f};
        } else {
            l = {c.foot + 0.06f, 0, 0};
            r = {-c.foot - 0.06f, 0, 0};
        }
    };
    V3 l0, r0, l1, r1;
    stance(k % 4, l0, r0);
    stance((k + 1) % 4, l1, r1);
    const float s = smooth((fr(b) - 0.2f) / 0.6f);
    const float jump = bump(fr(b));
    p.footL = lerp(l0, l1, s) + V3{0, 0.05f * jump * c.amp, 0};
    p.footR = lerp(r0, r1, s) + V3{0, 0.05f * jump * c.amp, 0};
    p.hip.y += 0.04f * jump * c.amp - 0.02f;
}

void legsRobotWalk(float b, const MoveCtx& c, DancePose& p) {
    // Stiff steps in place snapped to the beat: lift a straight leg, put it down.
    const bool left = int(std::floor(b)) % 2 == 0;
    const float in = snap(fr(b), 1.8f);
    const float up = in < 0.5f ? in * 2 : 2 - in * 2;
    V3& f = left ? p.footL : p.footR;
    f.y += 0.07f * up * c.amp;
    f.z += 0.03f * up;
    p.hipRoll += (left ? 0.05f : -0.05f) * up;
    p.bodyYaw += (c.variant & 1) ? 0.35f * alt(b, 4, 2) : 0;
}

void legsGlide(float b, const MoveCtx& c, DancePose& p) {
    // Backslide: one foot up on its toe in front while the other slides back flat; they swap each beat (the slid foot
    // is pulled forward onto its toe in the first part of the beat).
    auto foot = [&](float ph, V3& f, float& pitch) {
        ph = fr(ph / 2) * 2;  // 0..2: 0..1 toe in front, 1..2 sliding back
        if (ph < 1) {
            const float t = smooth(ph / 0.3f);
            f.z += (-0.10f + 0.16f * t) * c.amp;
            f.y += 0.03f * t + 0.03f * bump(std::min(1.0f, ph / 0.3f));
            pitch = -0.6f * t;
        } else {
            const float t = ph - 1;
            f.z += (0.06f - 0.16f * t) * c.amp;
            f.y += 0.03f * (1 - smooth(t / 0.2f));
            pitch = -0.6f * (1 - smooth(t / 0.2f));
        }
    };
    foot(b, p.footL, p.footPitchL);
    foot(b + 1, p.footR, p.footPitchR);
    p.bodyZ -= 0.04f * sn(b, 8);
    p.hip.y -= 0.02f;
    p.chestPitch -= 0.05f;
}

void legsKneeSway(float b, const MoveCtx& c, DancePose& p) {
    // Hip circles / sway with soft knees.
    const float x = sn(b, 2), z = cs(b, 2);
    p.hip.x += 0.04f * x * c.amp;
    p.hip.z += 0.025f * z * c.amp * float(c.variant & 1);
    p.hipRoll += -0.15f * x * c.amp;
    p.chestRoll += 0.1f * x;
    p.hip.y -= 0.025f;
    p.footL.x += 0.02f;
    p.footR.x -= 0.02f;
}

void legsHop(float b, const MoveCtx& c, DancePose& p) {
    // Hops on every beat (both feet), landing on the beat.
    const float air = bump(fr(b + 0.5f));
    p.hip.y += (0.11f * air - 0.03f * onBeat(b, 3)) * c.amp;
    p.footL.y += 0.1f * air * c.amp;
    p.footR.y += 0.1f * air * c.amp;
    p.footPitchL = p.footPitchR = -0.3f * air;
    if (c.variant & 1) p.bodyYaw += 0.3f * alt(b, 1, 1.5f);
}

void legsGrapevine(float b, const MoveCtx& c, DancePose& p) {
    // 8 beats: side, behind, side, close (traveling to the dancer's left), then the same back.
    const float w = 0.12f;
    const FeetKey keys[8] = {
        {0, 0, 0, 0},             {w, 0, 0, 0},           {w, 0, w - 0.02f, -0.08f}, {2 * w, 0, w - 0.02f, -0.08f},
        {2 * w, 0, 2 * w, 0},     {2 * w, 0, w, 0},       {w + 0.02f, -0.08f, w, 0}, {w + 0.02f, -0.08f, 0, 0},
    };
    keyFeet(b, keys, 8, c, p, 0.04f);
    p.hip.x -= w * c.amp * 0.85f;  // centered on the middle of the walk
    p.footL.x -= w * c.amp;
    p.footR.x -= w * c.amp;
    p.hip.y -= 0.025f * onBeat(b, 2);
    p.hipYaw += 0.1f * sn(b, 8, 0.25f);
}

void legsKickStep(float b, const MoveCtx& c, DancePose& p) {
    // Kicks forward on alternating beats; a hop on the standing foot.
    const bool left = int(std::floor(b)) % 2 == 0;
    const float in = bump(fr(b));
    V3& f = left ? p.footL : p.footR;
    const float high = (c.variant & 1) ? 0.2f : 0.12f;
    f.z += 0.16f * in * c.amp;
    f.y += high * in * c.amp;
    (left ? p.footPitchL : p.footPitchR) = 0.4f * in;
    p.hip.y += 0.02f * in;
    p.chestPitch -= 0.06f * in;
}

void legsJackLegs(float b, const MoveCtx& c, DancePose& p) {
    // Jumping-jack legs: out on 1, in on 2.
    const float open = (alt(b, 1, c.sharp) + 1) * 0.5f;
    const float air = bump(fr(b));
    p.footL.x += 0.1f * open * c.amp;
    p.footR.x -= 0.1f * open * c.amp;
    p.footL.y += 0.06f * air;
    p.footR.y += 0.06f * air;
    p.hip.y += 0.07f * air * c.amp - 0.02f * open;
}

// ---------------------------------------------------------------- arms
void armsSwing(float b, const MoveCtx& c, DancePose& p) {
    const float s = sn(b, 2) * c.amp;
    const float sh = c.shoulder;
    const float bend = (c.variant & 1) ? 0.06f : 0.0f;
    p.handL = L(sh + 0.05f, -0.18f + bend + 0.04f * std::fabs(s), 0.05f + 0.1f * s);
    p.handR = R(sh + 0.05f, -0.18f + bend + 0.04f * std::fabs(s), 0.05f - 0.1f * s);
    p.fist = 0.5f;
    p.headRoll += 0.05f * sn(b, 2, 0.25f);
    p.headPitch += 0.08f * onBeat(b);
}

void armsClap(float b, const MoveCtx& c, DancePose& p) {
    // Claps in front of the chest: on every beat (variant 1), on 2 and 4 (0, 2), overhead (3).
    const bool every = c.variant == 1;
    const float per = every ? 1.0f : 2.0f;
    const float ph = fr((b - (every ? 0.0f : 1.0f)) / per);
    const float meet = std::pow(0.5f + 0.5f * std::cos(TAU * ph), 2.0f + c.sharp * 2);
    const bool over = c.variant == 3;
    const float y = over ? 0.3f : 0.0f;
    const float gap = 0.03f + 0.2f * (1 - meet) * c.amp;
    p.handL = L(gap, y - 0.02f, over ? 0.04f : 0.17f);
    p.handR = R(gap, y - 0.02f, over ? 0.04f : 0.17f);
    p.headPitch += 0.1f * meet;
    p.mouth = std::max(p.mouth, 0.3f * meet);
}

void armsDiscoPoint(float b, const MoveCtx& c, DancePose& p) {
    // One arm points up and out (1), down and across (2); the other hand on the hip.
    const bool up = int(std::floor(b)) % 2 == 0;
    const float in = snap(fr(b), c.sharp);
    const V3 hi = L(c.shoulder + 0.15f, 0.25f, 0.08f), lo = L(-0.08f, -0.16f, 0.16f);
    const V3 a = up ? lo : hi, d = up ? hi : lo;
    const V3 h = lerp(a, d, in);
    const bool right = c.variant & 1;
    if (right) {
        p.handR = {-h.x, h.y, h.z};
        p.handL = L(c.shoulder - 0.01f, -0.16f, 0.06f);
    } else {
        p.handL = h;
        p.handR = R(c.shoulder - 0.01f, -0.16f, 0.06f);
    }
    p.fist = 0.6f;
    p.hipRoll += (up ? 0.08f : -0.05f) * (right ? -1.0f : 1.0f);
    p.headYaw += (up ? 0.25f : -0.15f) * (right ? -1.0f : 1.0f) * in;
    p.headPitch += up ? -0.15f * in : 0.1f * in;
}

void armsRoll(float b, const MoveCtx& c, DancePose& p) {
    // Rolling fists in front of the chest (disco roll); variant 1 rolls up and down the body.
    const float a = TAU * b * 2;
    const float y = (c.variant & 1) ? 0.12f * sn(b, 4) : 0.0f;
    p.handL = L(0.04f + 0.05f * std::cos(a), 0.0f + y + 0.05f * std::sin(a), 0.18f);
    p.handR = R(0.04f - 0.05f * std::cos(a), 0.0f + y - 0.05f * std::sin(a), 0.17f);
    p.fist = 1;
    p.chestYaw += 0.15f * sn(b, 2) * c.amp;
}

void armsHitchhike(float b, const MoveCtx& c, DancePose& p) {
    // Thumb over the shoulder, jerking on the 8ths, two beats an arm.
    const float wl = sideWeight(b, 2, 0.3f);  // 1 = the left arm is out
    const float jerk = onBeat(b * 2, 3);
    const V3 out = L(c.shoulder + 0.13f + 0.04f * jerk, 0.05f + 0.02f * jerk, 0.04f);
    const V3 hip = L(c.shoulder - 0.01f, -0.16f, 0.06f);
    p.handL = lerp(hip, out, wl);
    p.handR = lerp(V3{-hip.x, hip.y, hip.z}, V3{-out.x, out.y, out.z}, 1 - wl);
    p.fist = 1;
    p.headYaw += 0.3f * (2 * wl - 1);
    p.hipRoll += -0.06f * (2 * wl - 1) * c.amp;
}

void armsRaiseRoof(float b, const MoveCtx& c, DancePose& p) {
    // Flat palms pushing up on every beat.
    const float push = onBeat(b, 1 + c.sharp);
    const float y = 0.16f + 0.1f * push * c.amp;
    p.handL = L(c.shoulder - 0.02f, y, 0.05f);
    p.handR = R(c.shoulder - 0.02f, y, 0.05f);
    p.headPitch -= 0.15f * push;
    p.mouth = std::max(p.mouth, 0.5f * push);
    p.chestPitch -= 0.05f * push;
}

void armsFistPump(float b, const MoveCtx& c, DancePose& p) {
    // A fist punching up on the beat; variant 1 both arms, 2 alternating (switching between the pumps).
    const float push = onBeat(b, 1 + c.sharp * 1.5f);
    const V3 up = L(c.shoulder - 0.02f, 0.10f + 0.16f * push * c.amp, 0.06f);
    const V3 rest = L(c.shoulder - 0.02f, -0.12f, 0.08f);
    float wl = 1, wr = 0;
    if (c.variant == 1) wr = 1;
    else if (c.variant == 2) {
        wl = sideWeight(b + 0.5f, 1, 0.25f);
        wr = 1 - wl;
    }
    p.handL = lerp(rest, up, wl);
    const V3 r = lerp(rest, up, wr);
    p.handR = {-r.x, r.y, r.z};
    p.fist = 1;
    p.headPitch += 0.18f * push;
    p.mouth = std::max(p.mouth, 0.6f * push);
}

void armsRobot(float b, const MoveCtx& c, DancePose& p) {
    // Elbows at 90 degrees; forearms snap up / down / out on the 8ths; the head snaps side to side.
    const int k = int(std::floor(b * 2)) % 8;
    static const float ups[8][2] = {{1, 0}, {0, 1}, {1, 1}, {0, 0}, {1, 0}, {0, 1}, {-1, -1}, {0, 0}};
    const float sharp = 2.0f;
    const float in = snap(fr(b * 2), sharp);
    const int kp = (k + 7) % 8;
    auto h = [&](float u) { return L(c.shoulder + 0.02f, -0.12f + 0.16f * u, 0.17f - 0.05f * std::fabs(u)); };
    const float ul = ups[kp][0] + (ups[k][0] - ups[kp][0]) * in;
    const float ur = ups[kp][1] + (ups[k][1] - ups[kp][1]) * in;
    p.handL = h(ul);
    const V3 r = h(ur);
    p.handR = {-r.x, r.y, r.z};
    p.fist = 0.2f;
    const int hk = int(std::floor(b)) % 4;
    p.headYaw += (hk == 1 ? 0.4f : hk == 3 ? -0.4f : 0.0f);
    p.chestYaw += (c.variant & 1) ? 0.2f * alt(b, 2, 2) : 0;
}

void armsTut(float b, const MoveCtx& c, DancePose& p) {
    // King tut: boxy arm shapes held, snapping to a new one on each beat.
    static const V3 shapesL[6] = {{0.06f, 0.10f, 0.15f}, {0.30f, 0.12f, 0.02f}, {0.10f, -0.02f, 0.18f},
                                  {0.28f, -0.06f, 0.08f}, {0.05f, 0.25f, 0.06f}, {0.20f, 0.0f, 0.16f}};
    static const V3 shapesR[6] = {{0.25f, 0.0f, 0.10f}, {0.06f, -0.02f, 0.18f}, {0.30f, 0.14f, 0.02f},
                                  {0.08f, 0.10f, 0.16f}, {0.26f, 0.16f, 0.04f}, {0.05f, 0.22f, 0.08f}};
    const int k = (int(std::floor(b)) + c.variant) % 6, kp = (k + 5) % 6;
    const float in = snap(fr(b), 2.0f);
    const V3 l = lerp(shapesL[kp], shapesL[k], in), r = lerp(shapesR[kp], shapesR[k], in);
    p.handL = {l.x + c.shoulder - 0.15f, l.y, l.z};
    p.handR = {-(r.x + c.shoulder - 0.15f), r.y, r.z};
    p.fist = 0;
    p.headRoll += 0.15f * alt(b, 1, 2);
}

void armsWave(float b, const MoveCtx& c, DancePose& p) {
    // An arm wave traveling from one hand, through the shoulders, to the other hand and back (4 beats).
    const float t = fr(b / 4);
    const float pos = t < 0.5f ? t * 2 : 2 - t * 2;  // 0 at the left hand .. 1 at the right
    auto bumpAt = [&](float x) { return std::exp(-std::pow((x - pos) * 4.0f, 2.0f)); };
    const float sh = c.shoulder;
    p.handL = L(sh + 0.2f, 0.02f + 0.1f * bumpAt(0.0f), 0.02f);
    p.handR = R(sh + 0.2f, 0.02f + 0.1f * bumpAt(1.0f), 0.02f);
    p.chestRoll += 0.12f * (bumpAt(0.35f) - bumpAt(0.65f));
    p.headRoll += 0.15f * (bumpAt(0.4f) - bumpAt(0.6f));
    p.hip.y -= 0.02f * onBeat(b);
    (void)c;
}

void armsSwim(float b, const MoveCtx& c, DancePose& p) {
    // Breast-stroke swim, one stroke per 2 beats.
    const float a = TAU * b / 2;
    const float x = 0.06f + 0.12f * (0.5f - 0.5f * std::cos(a));
    const float z = 0.2f - 0.08f * (0.5f - 0.5f * std::cos(a));
    const float y = 0.0f + 0.05f * std::sin(a);
    p.handL = L(x, y, z);
    p.handR = R(x, y, z);
    p.chestPitch += 0.1f * std::sin(a);
    p.headPitch -= 0.08f * std::sin(a);
    p.fist = 0;
    (void)c;
}

void armsHandsUp(float b, const MoveCtx& c, DancePose& p) {
    // Both hands up, swaying side to side over 2 beats (variant 1: waving hands).
    const float s = sn(b, 4) * c.amp;
    const float wave = (c.variant & 1) ? 0.04f * sn(b, 1) : 0.0f;
    // A V beside the head (the arms are too short for over it: the head is nearly as big as an arm is long).
    p.handL = L(c.shoulder + 0.12f + 0.05f * s + wave, 0.2f, 0.06f);
    p.handR = R(c.shoulder + 0.12f - 0.05f * s - wave, 0.2f, 0.06f);
    p.chestRoll += -0.12f * s;
    p.hipRoll += 0.08f * s;
    p.headRoll += -0.1f * s;
    p.headPitch -= 0.12f;
    p.mouth = std::max(p.mouth, 0.35f);
}

void armsRock(float b, const MoveCtx& c, DancePose& p) {
    // Hip-hop rock: shoulders shrug on the beat, bent arms rocking with the torso.
    const float push = onBeat(b, 1.5f + c.sharp);
    const float s = alt(b, 1, c.sharp);
    p.handL = L(c.shoulder + 0.02f, -0.08f + 0.05f * push, 0.14f);
    p.handR = R(c.shoulder + 0.02f, -0.08f + 0.05f * push, 0.14f);
    p.handL.z += 0.04f * s;
    p.handR.z -= 0.04f * s;
    p.chestYaw += 0.15f * s * c.amp;
    p.chestPitch += 0.1f * push;
    p.headPitch += 0.18f * push;
    p.fist = 1;
}

void armsSprinkler(float b, const MoveCtx& c, DancePose& p) {
    // One hand behind the head, the other arm out sweeping across in three ticks, then sweeping back.
    const bool right = c.variant & 1;
    const float ph = fr(b / 4);
    float sweep;
    if (ph < 0.75f) sweep = std::floor(ph * 8) / 6.0f + snap(fr(ph * 8), 1.8f) / 6.0f * 0.0f;
    else sweep = 1.0f - smooth((ph - 0.75f) * 4);
    sweep = clampf(sweep, 0, 1);
    const float a = -0.3f + 1.1f * sweep;  // the arm angle across the front
    const V3 arm = L(c.shoulder + 0.25f * std::cos(a), 0.06f, 0.25f * std::sin(a));
    const V3 head = L(0.19f, 0.17f, -0.09f);  // at the back of the head, by the ear (as far as an arm reaches)
    if (right) {
        p.handR = {-arm.x, arm.y, arm.z};
        p.handL = head;
    } else {
        p.handL = arm;
        p.handR = {-head.x, head.y, head.z};
    }
    p.chestYaw += (right ? -1.0f : 1.0f) * (a - 0.25f) * 0.4f;
    p.fist = 0.3f;
}

void armsPointCrowd(float b, const MoveCtx& c, DancePose& p) {
    // Point at the crowd on every beat, sweeping across over 4 beats.
    const float s = sn(b, 8) * c.amp;
    const float jab = onBeat(b, 2 + c.sharp);
    const bool right = c.variant & 1;
    const V3 arm = L(c.shoulder * 0.5f + 0.1f * s, 0.08f + 0.04f * jab, 0.2f + 0.05f * jab);
    const V3 rest = L(c.shoulder - 0.01f, -0.14f, 0.07f);
    if (right) {
        p.handR = {-arm.x, arm.y, arm.z};
        p.handL = rest;
    } else {
        p.handL = arm;
        p.handR = {-rest.x, rest.y, rest.z};
    }
    p.fist = 0.6f;
    p.chestYaw += 0.2f * s;
    p.headYaw += 0.25f * s;
    p.mouth = std::max(p.mouth, 0.4f * jab);
}

void armsGlowCircles(float b, const MoveCtx& c, DancePose& p) {
    // Hands draw circles (glow sticks), opposite directions; variant 1 big overhead circles.
    const float a = TAU * b / ((c.variant & 1) ? 2.0f : 1.0f);
    const float r = (c.variant & 1) ? 0.12f : 0.07f;
    const float cy = (c.variant & 1) ? 0.12f : 0.0f;
    p.handL = L(c.shoulder + 0.08f + r * std::cos(a), cy + r * std::sin(a), 0.16f);
    p.handR = R(c.shoulder + 0.08f + r * std::cos(a), cy - r * std::sin(a), 0.16f);
    p.fist = 0.8f;
    p.headPitch += 0.1f * onBeat(b);
}

void armsHipHands(float b, const MoveCtx& c, DancePose& p) {
    // Hands on hips, head bob and shoulder pops.
    p.handL = L(c.shoulder - 0.02f, -0.17f, 0.04f);
    p.handR = R(c.shoulder - 0.02f, -0.17f, 0.04f);
    const float s = alt(b, 1, c.sharp);
    p.chestRoll += 0.1f * s * c.amp;
    p.headRoll += -0.12f * s;
    p.headPitch += 0.1f * onBeat(b);
    if (c.variant & 1) p.chestYaw += 0.15f * s;
    p.fist = 0.4f;
}

void armsSnap(float b, const MoveCtx& c, DancePose& p) {
    // Finger snaps at shoulder height, alternating sides on the beat.
    const float s = alt(b, 1, c.sharp);
    const float snapHit = onBeat(b, 3);
    p.handL = L(c.shoulder + 0.02f + 0.05f * std::max(0.0f, s), -0.02f + 0.06f * std::max(0.0f, s), 0.12f);
    p.handR = R(c.shoulder + 0.02f + 0.05f * std::max(0.0f, -s), -0.02f + 0.06f * std::max(0.0f, -s), 0.12f);
    p.headRoll += -0.08f * s;
    p.fist = 0.7f;
    p.blink = std::max(p.blink, 0.0f * snapHit);
}

void armsMonkey(float b, const MoveCtx& c, DancePose& p) {
    // The monkey: hands alternate up and down in front of the face, body rocking.
    const float s = alt(b, 1, c.sharp);
    p.handL = L(0.1f, 0.12f + 0.1f * s, 0.18f);
    p.handR = R(0.1f, 0.12f - 0.1f * s, 0.18f);
    p.chestPitch += 0.06f * s * c.amp;
    p.fist = 0.7f;
}

void armsPunch(float b, const MoveCtx& c, DancePose& p) {
    // Punches forward on the beat, alternating (the arms swap between punches).
    const float wl = sideWeight(b + 0.5f, 1, 0.25f);
    const float jab = onBeat(b, 2 + c.sharp * 2);
    const V3 guard = L(0.08f, -0.01f, 0.14f);
    const V3 hit = L(0.06f, 0.02f, 0.14f + 0.15f * jab * c.amp);
    p.handL = lerp(guard, hit, wl);
    const V3 r = lerp(guard, hit, 1 - wl);
    p.handR = {-r.x, r.y, r.z};
    p.chestYaw += -0.18f * jab * (2 * wl - 1);
    p.fist = 1;
}

void armsJazz(float b, const MoveCtx& c, DancePose& p) {
    // Jazz hands: open hands out to the sides, shaking (16ths).
    const float shake = 0.015f * std::sin(TAU * b * 4);
    const float up = (c.variant & 1) ? 0.18f : 0.04f;
    p.handL = L(c.shoulder + 0.18f, up + shake, 0.08f);
    p.handR = R(c.shoulder + 0.18f, up - shake, 0.08f);
    p.mouth = std::max(p.mouth, 0.7f);
    p.brow = 1;
    p.fist = 0;
    p.headRoll += 0.1f * sn(b, 2);
}

void armsChestPop(float b, const MoveCtx& c, DancePose& p) {
    // Chest pops on the beat with bent arms.
    const float pop = onBeat(b, 3 + c.sharp * 2);
    p.chestPitch -= 0.12f * pop * c.amp;
    p.hip.z -= 0.01f * pop;
    p.handL = L(c.shoulder + 0.03f, -0.1f, 0.12f);
    p.handR = R(c.shoulder + 0.03f, -0.1f, 0.12f);
    p.headPitch -= 0.1f * pop;
    p.fist = 0.8f;
}

void armsJackArms(float b, const MoveCtx& c, DancePose& p) {
    // Arms open and close with the house jack.
    const float j = onBeat(b, 1.5f);
    p.handL = L(c.shoulder + 0.06f + 0.12f * j, -0.08f + 0.06f * j, 0.12f - 0.05f * j);
    p.handR = R(c.shoulder + 0.06f + 0.12f * j, -0.08f + 0.06f * j, 0.12f - 0.05f * j);
    p.fist = 0.4f;
    (void)c;
}

// ---------------------------------------------------------------- full-body moves
void fullSpin(float b, const MoveCtx& c, DancePose& p) {
    // A full turn over 2 beats with arms out, then 2 beats of a pose.
    const float t = clampf(b / 2, 0, 1);
    const float dir = (c.variant & 1) ? -1.0f : 1.0f;
    p.bodyYaw += t < 1 ? dir * TAU * smooth(t) : 0.0f;  // a full turn is back where it started
    const float open = bump(t);
    p.handL = L(c.shoulder + 0.15f * open + 0.02f, -0.05f + 0.15f * open, 0.04f);
    p.handR = R(c.shoulder + 0.15f * open + 0.02f, -0.05f + 0.15f * open, 0.04f);
    p.hip.y += 0.03f * open - 0.02f;
    p.footPitchL = p.footPitchR = -0.3f * open;
    if (b >= 2) {  // pose: one hand up, one on the hip (released at the end)
        const float in = smooth((b - 2) * 3) * (1 - smooth((b - 3.5f) / 0.5f));
        p.handL = lerp(p.handL, L(c.shoulder + 0.08f, 0.26f, 0.06f), in);
        p.handR = lerp(p.handR, R(c.shoulder - 0.02f, -0.16f, 0.05f), in);
        p.hipRoll += 0.08f * in;
        p.headPitch -= 0.1f * in;
        p.mouth = std::max(p.mouth, 0.6f * in);
    }
}

void fullJump(float b, const MoveCtx& c, DancePose& p) {
    // Crouch (0..0.5), a big jump with arms up (0.5..1.5), land and bounce.
    float y = 0;
    float arms = 0;
    if (b < 0.6f) {
        y = -0.1f * smooth(b / 0.6f);
        arms = -0.3f * smooth(b / 0.6f);
    } else if (b < 1.6f) {
        const float t = (b - 0.6f) / 1.0f;
        y = 0.28f * std::sin(PI * t) * c.amp - 0.1f * (1 - smooth(t / 0.2f));  // spring up out of the crouch
        arms = -0.3f + 1.3f * smooth(t / 0.35f);  // throw the arms up as the jump starts
    } else {
        const float t = clampf((b - 1.6f) / 0.4f, 0, 1);  // land and bounce by the end of the move
        y = -0.08f * bump(t);
        arms = 1.0f - smooth(t);
    }
    p.hip.y += y;
    const float air = std::max(0.0f, y);
    p.footL.y += air * 0.9f;
    p.footR.y += air * 0.9f;
    p.footL.x += 0.05f * arms;
    p.footR.x -= 0.05f * arms;
    p.handL = lerp(L(c.shoulder + 0.03f, -0.2f, 0.0f), L(c.shoulder + 0.07f, 0.27f, 0.05f), clampf(arms, 0, 1));
    p.handR = {-p.handL.x, p.handL.y, p.handL.z};
    p.mouth = std::max(p.mouth, clampf(arms, 0, 1));
    p.brow = clampf(arms, 0, 1);
    p.headPitch -= 0.15f * clampf(arms, 0, 1);
}

void fullFloss(float b, const MoveCtx& c, DancePose& p) {
    // Hips swing one way while straight arms swing the other, crossing in front and behind.
    const float s = sn(b, 1);   // hips (one swing per beat)
    const float front = clampf(sn(b, 2) * 4.0f, -1.0f, 1.0f);  // arms in front / behind, swapping smoothly
    p.hip.x += 0.05f * s * c.amp;
    p.hipRoll += -0.12f * s;
    const float ax = -0.12f * s;
    p.handL = L(c.shoulder * 0.4f + ax, -0.2f, 0.09f * front);
    p.handR = R(c.shoulder * 0.4f - ax, -0.2f, -0.09f * front);
    p.handL.x = 0.05f + ax;
    p.handR.x = -0.05f + ax;
    p.fist = 1;
    p.headRoll += 0.1f * s;
}

void fullDab(float b, const MoveCtx& c, DancePose& p) {
    // Groove for 3 beats, dab on beat 4 (held), back.
    legsGroove(b, c, p);
    armsSwing(b, c, p);
    const float in = b >= 3 ? smooth((b - 3) * 4) * (1 - smooth((b - 3.6f) * 2.5f)) : 0.0f;  // hit, hold, release
    const bool right = c.variant & 1;
    const V3 face = L(0.02f, 0.12f, 0.16f), out = L(c.shoulder + 0.22f, 0.15f, 0.04f);
    if (!right) {
        p.handL = lerp(p.handL, out, in);
        p.handR = lerp(p.handR, V3{-face.x + 0.06f, face.y, face.z}, in);
        p.headYaw += -0.4f * in;
    } else {
        p.handR = lerp(p.handR, V3{-out.x, out.y, out.z}, in);
        p.handL = lerp(p.handL, face, in);
        p.headYaw += 0.4f * in;
    }
    p.headPitch += 0.5f * in;
    p.chestPitch += 0.15f * in;
}

void fullCabbage(float b, const MoveCtx& c, DancePose& p) {
    // Cabbage patch: fists together circling in front, hips circling, knees bent.
    const float a = TAU * b / 2;
    const V3 center = L(0.0f, -0.02f + 0.06f * std::sin(a), 0.16f + 0.05f * std::cos(a));
    p.handL = center + V3{0.05f + 0.06f * std::cos(a), 0, 0};
    p.handR = center + V3{-0.05f + 0.06f * std::cos(a), 0.01f, 0.02f};
    p.hip.x += 0.04f * std::cos(a) * c.amp;
    p.hip.z += 0.02f * std::sin(a);
    p.hip.y -= 0.05f;
    p.hipRoll += -0.1f * std::cos(a);
    p.fist = 1;
    p.headRoll += 0.08f * std::cos(a);
}

void fullHeadBang(float b, const MoveCtx& c, DancePose& p) {
    // Head bangs on every beat, body pumping, one fist up.
    const float hit = onBeat(b, 2 + c.sharp);
    p.headPitch += (0.55f * hit - 0.1f) * c.amp;
    p.chestPitch += 0.25f * hit;
    p.hip.y -= 0.05f * hit;
    p.hip.z -= 0.02f;
    p.footL.x += 0.05f;
    p.footR.x -= 0.05f;
    p.handL = L(c.shoulder - 0.02f, 0.18f + 0.06f * hit, 0.06f);
    p.handR = R(c.shoulder + 0.02f, -0.12f, 0.1f);
    p.fist = 1;
    p.mouth = std::max(p.mouth, 0.7f * hit);
    p.blink = std::max(p.blink, 0.8f * hit);
}

void fullJumpingJacks(float b, const MoveCtx& c, DancePose& p) {
    legsJackLegs(b, c, p);
    const float open = (alt(b, 1, c.sharp) + 1) * 0.5f;
    p.handL = lerp(L(c.shoulder + 0.05f, -0.2f, 0.02f), L(c.shoulder + 0.08f, 0.27f, 0.02f), open);
    p.handR = {-p.handL.x, p.handL.y, p.handL.z};
    p.mouth = std::max(p.mouth, 0.4f * open);
}

void fullMoonwalk(float b, const MoveCtx& c, DancePose& p) {
    // Glide backwards with a slight turn to the side, hat-tip arms.
    p.bodyYaw += 0.5f * ((c.variant & 1) ? -1.0f : 1.0f);
    legsGlide(b, c, p);
    p.handL = L(c.shoulder + 0.02f, -0.12f, 0.12f);
    p.handR = R(c.shoulder - 0.03f, -0.15f, 0.06f);
    p.handL = lerp(L(0.06f, 0.22f, 0.1f), p.handL, sideWeight(b, 4));  // every other 4 beats: a hand to the hat brim
    p.headPitch -= 0.05f;
}

void fullBodyRoll(float b, const MoveCtx& c, DancePose& p) {
    // A wave down the body over 2 beats: head, chest, hips, knees.
    const float t = TAU * b / 2;
    p.headPitch += 0.2f * std::sin(t) * c.amp;
    p.chestPitch += 0.18f * std::sin(t - 0.8f) * c.amp;
    p.hip.z += 0.04f * std::sin(t - 1.6f) * c.amp;
    p.hip.y -= 0.03f * (0.5f + 0.5f * std::sin(t - 2.4f));
    p.handL = L(c.shoulder + 0.04f, -0.08f, 0.1f + 0.04f * std::sin(t - 0.8f));
    p.handR = R(c.shoulder + 0.04f, -0.08f, 0.1f + 0.04f * std::sin(t - 0.8f));
    if (c.variant & 1) {  // one hand sliding down the body
        const float u = fr(b / 2), tri = u < 0.5f ? u * 2 : 2 - u * 2;
        p.handL = L(0.06f, 0.1f - 0.25f * tri, 0.15f);
    }
    p.fist = 0;
}

void fullShimmy(float b, const MoveCtx& c, DancePose& p) {
    // Shoulders shake fast (16ths), slowly going down and up, arms out low.
    const float shake = std::sin(TAU * b * 4) * c.amp;
    p.chestYaw += 0.12f * shake;
    p.chestPitch -= 0.08f;
    p.hip.y -= 0.07f * (0.5f - 0.5f * cs(b, 4));
    p.handL = L(c.shoulder + 0.12f, -0.12f, 0.12f);
    p.handR = R(c.shoulder + 0.12f, -0.12f, 0.12f);
    p.mouth = std::max(p.mouth, 0.5f);
    p.brow = 0.6f;
    p.fist = 0;
}

void fullTwist(float b, const MoveCtx& c, DancePose& p) {
    legsTwist(b, c, p);
    const float s = sn(b, 1);
    p.handL = L(c.shoulder + 0.05f, -0.05f, 0.12f + 0.08f * s);
    p.handR = R(c.shoulder + 0.05f, -0.05f, 0.12f - 0.08f * s);
    p.fist = 0.8f;
    p.mouth = std::max(p.mouth, 0.4f);
}

void fullFreeze(float b, const MoveCtx& c, DancePose& p) {
    // Groove 3 beats, then hit a pose on beat 4 (one of four) and hold.
    legsGroove(b, c, p);
    armsSwing(b, c, p);
    if (b < 3) return;
    const float in = snap((b - 3) * 1.0f, 2.0f) * (1 - smooth((b - 3.7f) / 0.3f));  // hit, hold, release
    DancePose q = p;
    switch (c.variant & 3) {
        case 0:  // point to the sky
            q.handL = L(c.shoulder + 0.14f, 0.25f, 0.08f);
            q.handR = R(c.shoulder - 0.02f, -0.16f, 0.05f);
            q.hipRoll = 0.15f;
            q.headYaw = 0.3f;
            q.headPitch = -0.2f;
            break;
        case 1:  // arms crossed, lean back
            q.handL = R(0.1f, -0.06f, 0.14f);
            q.handR = L(0.1f, -0.04f, 0.15f);
            q.chestPitch = -0.15f;
            q.headRoll = 0.15f;
            break;
        case 2:  // star
            q.handL = L(c.shoulder + 0.2f, 0.18f, 0.02f);
            q.handR = R(c.shoulder + 0.2f, 0.18f, 0.02f);
            q.footL.x += 0.1f;
            q.footR.x -= 0.1f;
            q.mouth = 1;
            break;
        default:  // crouch with a fist
            q.hip.y = -0.08f;
            q.handL = L(0.06f, 0.0f, 0.18f);
            q.handR = R(c.shoulder - 0.02f, -0.1f, 0.06f);
            q.chestPitch = 0.2f;
            q.fist = 1;
            break;
    }
    p = mix(p, q, in);
}

void fullLawnmower(float b, const MoveCtx& c, DancePose& p) {
    // Pull-start a lawnmower: lean, pull on 1 and 3, push along on 2 and 4.
    const bool pull = int(std::floor(b)) % 2 == 0;
    const float in = snap(fr(b), c.sharp);
    p.chestPitch += 0.25f;
    p.hip.y -= 0.05f;
    p.footL.z += 0.06f;
    p.footR.z -= 0.04f;
    const V3 low = L(0.0f, -0.25f, 0.2f), high = L(c.shoulder + 0.1f, 0.05f, -0.05f);
    p.handL = pull ? lerp(low, high, in) : lerp(high, low, in);
    p.handR = R(c.shoulder - 0.02f, -0.12f, 0.08f);
    p.chestYaw += (pull ? 0.25f * in : 0.25f * (1 - in));
    p.fist = 1;
    p.mouth = std::max(p.mouth, 0.3f);
}

void fullWave(float b, const MoveCtx& c, DancePose& p) {
    // Waving hello / goodbye with one hand, a small bounce.
    const float w = std::sin(TAU * b * 2);
    p.handL = L(c.shoulder + 0.12f + 0.05f * w, 0.24f, 0.08f);
    p.handR = R(c.shoulder + 0.02f, -0.18f, 0.05f);
    p.headRoll += 0.1f * sn(b, 2);
    p.mouth = std::max(p.mouth, 0.5f);
    p.hip.y -= 0.015f * onBeat(b);
    p.fist = 0;
}

void fullPony(float b, const MoveCtx& c, DancePose& p) {
    // The pony: a skipping step (1-and-2), fists bouncing in front.
    const float e = fr(b);
    const bool left = int(std::floor(b)) % 2 == 0;
    V3& f = left ? p.footL : p.footR;
    f.y += 0.08f * bump(e < 0.5f ? e * 2 : 0) * c.amp;
    (left ? p.footR : p.footL).y += 0.04f * bump(e >= 0.5f ? (e - 0.5f) * 2 : 0);
    p.hip.y += 0.03f * bump(e) - 0.03f;
    p.handL = L(0.08f, -0.05f + 0.04f * onBeat(b * 2, 2), 0.16f);
    p.handR = R(0.08f, -0.05f + 0.04f * onBeat(b * 2, 2), 0.16f);
    p.fist = 1;
}


// ---------------------------------------------------------------- more legs (robot, chill, house)
void legsPopWalk(float b, const MoveCtx& c, DancePose& p) {
    // Popping: the knees and chest "hit" on every beat (a quick contraction), a stiff step every 2 beats.
    const float hit = onBeat(b, 6);
    p.hip.y -= 0.03f * hit * c.amp;
    p.chestPitch -= 0.08f * hit;
    const float wl = sideWeight(b, 2, 0.3f);
    const float in = snap(fr(b / 2), 2);
    const float stepOut = 0.07f * (in < 0.5f ? in * 2 : 2 - in * 2);
    p.footL.x += stepOut * wl;
    p.footR.x -= stepOut * (1 - wl);
    if (c.variant & 1) p.bodyYaw += 0.25f * (2 * wl - 1);
}

void legsTick(float b, const MoveCtx& c, DancePose& p) {
    // Ticking: tiny side steps on the 8ths, staccato, traveling and coming back over 4 beats.
    const float k = std::floor(b * 2);
    const float in = snap(fr(b * 2), 2);
    const float pos = (k + in);  // 0..8
    const float x = (pos < 4 ? pos : 8 - pos) * 0.03f * c.amp;
    p.bodyX += x - 0.06f;
    const bool left = int(k) % 2 == 0;
    (left ? p.footL : p.footR).y += 0.025f * bump(in);
    p.hip.y -= 0.015f * onBeat(b * 2, 4);
}

void legsChaCha(float b, const MoveCtx& c, DancePose& p) {
    // Cha-cha: rock forward (2), replace (3), cha-cha to the side (4-and-1), then the same with the other foot.
    const FeetKey keys[8] = {
        {0, 0, 0, 0},         {0, 0.12f, 0, 0},         {0, 0, 0, 0},       {0.07f, 0, 0, 0},
        {0.07f, 0, 0.07f, 0}, {0.07f, 0, 0.07f, -0.12f}, {0.07f, 0, 0.07f, 0}, {0.07f, 0, 0, 0},
    };
    keyFeet(b, keys, 8, c, p, 0.03f);
    p.hip.x -= 0.035f * c.amp * 0.85f;
    p.footL.x -= 0.035f * c.amp;
    p.footR.x -= 0.035f * c.amp;
    const float h = sn(b, 2, 0.25f);
    p.hipRoll += -0.12f * h * c.amp;
    p.hip.x += 0.025f * h;
}

void legsToeTap(float b, const MoveCtx& c, DancePose& p) {
    // A lazy toe tap out to the side on every other beat, weight on the other leg; four beats a side.
    const float wl = (c.variant & 1) ? 1 - sideWeight(b, 4) : sideWeight(b, 4);  // 1 = the left foot taps
    const float tap = bump(fr(b / 2) * 2 > 1 ? 0 : fr(b / 2) * 2);
    p.footL.x += 0.06f * tap * wl;
    p.footR.x -= 0.06f * tap * (1 - wl);
    p.footL.y += 0.03f * tap * wl;
    p.footR.y += 0.03f * tap * (1 - wl);
    p.footPitchL = -0.4f * tap * wl;
    p.footPitchR = -0.4f * tap * (1 - wl);
    const float side = 2 * wl - 1;
    p.hip.x += -0.02f * side;
    p.hipRoll += 0.06f * side;
    p.hip.y -= 0.02f * onBeat(b);
    (void)c;
}

void legsWeightShift(float b, const MoveCtx& c, DancePose& p) {
    // A slow weight shift over 2 beats with a knee pop on the light leg.
    const float s = sn(b, 4);
    p.hip.x += 0.045f * s * c.amp;
    p.hipRoll += -0.12f * s;
    p.footPitchL = std::max(0.0f, -s) * 0.3f;
    p.footPitchR = std::max(0.0f, s) * 0.3f;
    p.hip.y -= 0.02f * onBeat(b, 2);
    p.headRoll += 0.06f * s;
}

void legsLooseLegs(float b, const MoveCtx& c, DancePose& p) {
    // House loose legs: a foot flicks out and in (ankle twist) on the off-beats, alternating.
    const bool left = int(std::floor(b + 0.5f)) % 2 == 0;  // switch feet when both are down (on the beat)
    const float flick = bump(fr(b + 0.5f));
    V3& f = left ? p.footL : p.footR;
    f.x += (left ? 1.0f : -1.0f) * 0.07f * flick * c.amp;
    f.y += 0.05f * flick;
    (left ? p.footYawL : p.footYawR) = -0.6f * flick;
    p.hip.y += 0.02f * (1 - onBeat(b, 2));
    p.chestPitch += 0.08f * onBeat(b);
}

void legsSkate(float b, const MoveCtx& c, DancePose& p) {
    // Skating: long glides to the side, the trailing foot sweeping behind.
    const float s = sn(b, 4) * c.amp;
    p.bodyX += 0.1f * s;
    p.hip.x += 0.06f * s;
    p.hip.y -= 0.05f;
    p.hipRoll += -0.12f * s;
    V3& trail = s > 0 ? p.footR : p.footL;
    trail.z -= 0.08f * std::fabs(s);
    trail.y += 0.04f * std::fabs(s);
    (s > 0 ? p.footL : p.footR).x += (s > 0 ? 0.06f : -0.06f) * std::fabs(s);
    p.chestPitch += 0.15f;
    p.chestYaw += -0.2f * s;
}

// ---------------------------------------------------------------- more arms
void armsTickTock(float b, const MoveCtx& c, DancePose& p) {
    // Arms as clock hands, ticking on the 16ths: one arm sweeps up over 2 beats and back down over 2, the other holds.
    const float ticks = std::floor(b * 4) + snap(fr(b * 4), 2);  // 0..16
    const float tri = ticks < 8 ? ticks / 8 : 2 - ticks / 8;
    const float a = -1.3f + PI * 0.9f * tri;
    const V3 tick = L(c.shoulder + 0.22f * std::cos(a) * 0.6f + 0.04f, 0.02f + 0.22f * std::sin(a), 0.1f);
    const V3 hold = L(c.shoulder + 0.02f, -0.1f, 0.15f);
    if (c.variant & 1) {
        p.handR = {-tick.x, tick.y, tick.z};
        p.handL = hold;
    } else {
        p.handL = tick;
        p.handR = {-hold.x, hold.y, hold.z};
    }
    p.headYaw += 0.15f * alt(b, 1, 2);
    p.fist = 0.3f;
}

void armsPopArms(float b, const MoveCtx& c, DancePose& p) {
    // Arms straight out to the sides, popping at the elbows / wrists on the beat (a wave of hits).
    const float hitL = onBeat(b, 8), hitR = onBeat(b - 0.5f, 8);
    p.handL = L(c.shoulder + 0.22f - 0.06f * hitL, 0.03f + 0.04f * hitL, 0.02f + 0.06f * hitL);
    p.handR = R(c.shoulder + 0.22f - 0.06f * hitR, 0.03f + 0.04f * hitR, 0.02f + 0.06f * hitR);
    p.chestPitch -= 0.06f * hitL;
    p.headRoll += 0.08f * (hitL - hitR);
    p.fist = 0;
    (void)c;
}

void armsMaracas(float b, const MoveCtx& c, DancePose& p) {
    // Shaking maracas: fists at shoulder height, shaking on the 8ths, alternating high and low.
    const float sh = 0.03f * std::sin(TAU * b * 2);
    const float s = sn(b, 4);
    p.handL = L(c.shoulder + 0.1f, 0.02f + 0.08f * s + sh, 0.12f);
    p.handR = R(c.shoulder + 0.1f, 0.02f - 0.08f * s - sh, 0.12f);
    p.fist = 1;
    p.headYaw += 0.15f * s;
    p.mouth = std::max(p.mouth, 0.35f);
    (void)c;
}

void armsLighter(float b, const MoveCtx& c, DancePose& p) {
    // One arm up holding a lighter (or a phone), swaying slowly over 4 beats.
    const float s = sn(b, 8) * c.amp;
    const bool right = c.variant & 1;
    const V3 up = L(c.shoulder - 0.02f + 0.07f * s, 0.25f, 0.06f);
    const V3 down = L(c.shoulder + 0.02f, -0.18f, 0.04f);
    p.handL = right ? down : up;
    p.handR = right ? V3{-up.x, up.y, up.z} : V3{-down.x, down.y, down.z};
    p.fist = 1;
    p.chestRoll += -0.08f * s;
    p.headRoll += -0.08f * s;
    p.headPitch -= 0.08f;
    p.blink = std::max(p.blink, 0.6f * (0.5f + 0.5f * sn(b, 16)));  // eyes half closed: feeling it
}

void armsFingerGuns(float b, const MoveCtx& c, DancePose& p) {
    // Finger guns pointing forward, a recoil on 2 and 4.
    const float rec = onBeat(b - 1, 4);
    const float s = sn(b, 8);
    p.handL = L(c.shoulder * 0.7f + 0.05f * s, -0.02f + 0.04f * rec, 0.2f - 0.04f * rec);
    p.handR = R(c.shoulder * 0.7f - 0.05f * s, -0.02f + 0.04f * rec, 0.2f - 0.04f * rec);
    p.fist = 0.8f;
    p.headYaw += 0.2f * s;
    p.mouth = std::max(p.mouth, 0.3f * rec);
    p.brow = 0.5f * rec;
    (void)c;
}

void armsHandJive(float b, const MoveCtx& c, DancePose& p) {
    // Hand jive: pat the knees, clap, cross, thumbs over the shoulders (a beat each), moving from one to the next.
    auto shape = [&](int k, V3& l, V3& r) {
        switch (k) {
            case 0: l = L(c.shoulder - 0.02f, -0.18f, 0.12f); r = {-l.x, l.y, l.z}; break;
            case 1: l = L(0.04f, 0.0f, 0.17f); r = {-l.x, l.y, l.z}; break;
            case 2: l = L(-0.05f, -0.03f, 0.17f); r = R(-0.05f, 0.01f, 0.2f); break;
            default: l = L(c.shoulder + 0.1f, 0.06f, 0.08f); r = R(c.shoulder + 0.1f, 0.06f, 0.08f); break;
        }
    };
    const int k = int(std::floor(b)) % 4;
    V3 l0, r0, l1, r1;
    shape((k + 3) % 4, l0, r0);
    shape(k, l1, r1);
    const float in = snap(fr(b) * 2.0f, c.sharp);  // move in the first half of the beat, then the "and" bounce
    const float e = onBeat(b * 2, 3) * 0.03f;
    p.handL = lerp(l0, l1, in) + V3{0, e, 0};
    p.handR = lerp(r0, r1, in) + V3{0, e, 0};
    p.fist = k == 3 ? 1.0f : 0.2f;
}

void armsCrossedNod(float b, const MoveCtx& c, DancePose& p) {
    // Arms crossed, nodding hard on the beat (the too-cool-to-dance move).
    p.handL = R(0.1f, -0.06f, 0.14f);
    p.handR = L(0.11f, -0.04f, 0.155f);
    p.headPitch += 0.22f * onBeat(b, 2 + c.sharp) - 0.05f;
    p.chestPitch += 0.05f * onBeat(b, 2);
    p.fist = 0.6f;
    if (c.variant & 1) p.headYaw += 0.25f;
}

// ---------------------------------------------------------------- more full-body moves
void fullRobotTurn(float b, const MoveCtx& c, DancePose& p) {
    // Turn in quarter-turn snaps (one per beat), arms robotic, then snap back.
    const float k = std::floor(b);
    const float in = snap(fr(b), 2);
    const float dir = (c.variant & 1) ? -1.0f : 1.0f;
    const float steps[5] = {0, 1, 2, 1, 0};
    const int ki = std::min(int(k), 3);
    const float yaw = (steps[ki] + (steps[ki + 1] - steps[ki]) * in) * 0.75f * dir;
    p.bodyYaw += yaw;
    armsRobot(b, c, p);
    (in < 0.5f ? p.footL : p.footR).y += 0.04f * bump(in);
}

void fullSlowMo(float b, const MoveCtx& c, DancePose& p) {
    // Slow-motion run: exaggerated running poses at a quarter speed, mouth open.
    const float t = TAU * b / 4;
    const float s = std::sin(t);
    p.footL.z += 0.1f * s;
    p.footR.z -= 0.1f * s;
    p.footL.y += 0.08f * std::max(0.0f, s);
    p.footR.y += 0.08f * std::max(0.0f, -s);
    p.handL = L(c.shoulder + 0.04f, -0.05f + 0.06f * std::max(0.0f, -s), 0.15f * -s);
    p.handR = R(c.shoulder + 0.04f, -0.05f + 0.06f * std::max(0.0f, s), 0.15f * s);
    p.chestPitch += 0.12f;
    p.headRoll += 0.06f * s;
    p.mouth = std::max(p.mouth, 0.7f);
    p.fist = 0.5f;
}

void fullVibe(float b, const MoveCtx& c, DancePose& p) {
    // Eyes closed, head rolling, swaying with both hands loose: lost in the music.
    const float s = sn(b, 4), q = cs(b, 4);
    p.headRoll += 0.18f * s * c.amp;
    p.headPitch += 0.1f * q - 0.05f;
    p.chestRoll += -0.08f * s;
    p.hip.x += 0.03f * s;
    p.hipRoll += 0.08f * s;
    p.handL = L(c.shoulder + 0.06f, -0.12f + 0.05f * q, 0.1f);
    p.handR = R(c.shoulder + 0.06f, -0.12f - 0.05f * q, 0.1f);
    p.blink = std::max(p.blink, 0.9f);
    p.hip.y -= 0.02f * onBeat(b, 2);
    p.fist = 0;
}

void fullConga(float b, const MoveCtx& c, DancePose& p) {
    // Conga: three steps and a kick out to the side (1, 2, 3, kick), hands up front.
    const int k = int(std::floor(b)) % 4;
    const float in = bump(fr(b));
    const bool left = (int(std::floor(b / 4)) % 2 == 0) != bool(c.variant & 1);
    if (k < 3) (k % 2 == 0 ? p.footL : p.footR).y += 0.03f * in;
    else {
        V3& f = left ? p.footL : p.footR;
        f.x += (left ? 1.0f : -1.0f) * 0.13f * in * c.amp;
        f.y += 0.1f * in;
        p.hipRoll += (left ? -0.1f : 0.1f) * in;
    }
    p.handL = L(c.shoulder - 0.02f, -0.02f, 0.2f);
    p.handR = R(c.shoulder - 0.02f, -0.02f, 0.2f);
    p.hip.x += 0.02f * sn(b, 2);
    p.mouth = std::max(p.mouth, 0.4f * (k == 3 ? in : 0.0f));
}

// ---------------------------------------------------------------- the library

// The hand-made moves keep the tags of the styles they were written for (before 2026-11-21); each old style counts
// for the new styles closest to it.
enum class OldStyle { Disco, HipHop, House, Pop, Robot, Rave, Retro, Chill };
constexpr std::uint32_t S(OldStyle s) {
    switch (s) {
        case OldStyle::Disco: return styleBit(Style::Disco);
        case OldStyle::HipHop: return styleBit(Style::HipHop) | styleBit(Style::Break) | styleBit(Style::Stomp);
        case OldStyle::House: return styleBit(Style::House);
        case OldStyle::Pop: return styleBit(Style::Pop) | styleBit(Style::Jazz) | styleBit(Style::Ballet);
        case OldStyle::Robot: return styleBit(Style::Pop) | styleBit(Style::Lock);
        case OldStyle::Rave: return styleBit(Style::House);
        case OldStyle::Retro: return styleBit(Style::Lock) | styleBit(Style::Disco);
        case OldStyle::Chill: return styleBit(Style::Jazz) | styleBit(Style::Ballet);
    }
    return 0;
}
constexpr std::uint32_t kAll = (1u << kStyleCount) - 1;
using St = OldStyle;

// Mocap: the chance that a pick on an 8-bar point of the phrase starts a routine (16 bars, or an 8-bar half) rather
// than basic moves. Peter (2026-11-21): routines about 60 % of the time (first half). Every routine fills 16 bars,
// basics run until the next 8-bar point, so the share is 2p / (1 + p): 0.43 gives 60 %.
constexpr float kRoutineChance = 0.43f;

std::vector<MoveDef> kMoves = {
    // name, part, beats, styles, energy, fn
    {"groove", Part::Legs, 2, kAll, Chill | Groove, legsGroove},
    {"stepTouch", Part::Legs, 4, S(St::Pop) | S(St::Disco) | S(St::Chill) | S(St::Retro) | S(St::House), Chill | Groove, legsStepTouch},
    {"twoStep", Part::Legs, 4, S(St::HipHop) | S(St::Pop) | S(St::House), Groove, legsTwoStep},
    {"kickBallChange", Part::Legs, 4, S(St::HipHop) | S(St::Retro) | S(St::Pop) | S(St::Disco), Groove | Hype, legsKickBall},
    {"runningMan", Part::Legs, 2, S(St::HipHop) | S(St::Rave) | S(St::House), Hype, legsRunningMan},
    {"tStep", Part::Legs, 8, S(St::Rave) | S(St::House), Groove | Hype, legsTStep},
    {"heelToe", Part::Legs, 2, S(St::House) | S(St::Rave), Groove | Hype, legsHeelToe},
    {"jack", Part::Legs, 1, S(St::House), Chill | Groove | Hype, legsJack},
    {"latinSide", Part::Legs, 2, S(St::Pop), Chill | Groove, legsLatinSide},
    {"twistLegs", Part::Legs, 2, S(St::Retro) | S(St::Disco), Groove | Hype, legsTwist},
    {"charleston", Part::Legs, 8, S(St::Retro) | S(St::Rave), Groove | Hype, legsCharleston},
    {"crissCross", Part::Legs, 4, S(St::HipHop) | S(St::Rave) | S(St::Pop), Hype, legsCrissCross},
    {"robotWalk", Part::Legs, 8, S(St::Robot), Chill | Groove, legsRobotWalk},
    {"glide", Part::Legs, 2, S(St::Robot) | S(St::Pop) | S(St::HipHop), Groove, legsGlide},
    {"kneeSway", Part::Legs, 2, S(St::Chill) | S(St::Pop) | S(St::Disco), Chill | Groove, legsKneeSway},
    {"hop", Part::Legs, 2, S(St::Rave) | S(St::Pop) | S(St::House) | S(St::HipHop), Hype, legsHop},
    {"grapevine", Part::Legs, 8, S(St::Pop) | S(St::Retro) | S(St::Disco), Groove, legsGrapevine},
    {"kickStep", Part::Legs, 2, S(St::Rave) | S(St::Retro) | S(St::Disco), Groove | Hype, legsKickStep},
    {"jackLegs", Part::Legs, 2, S(St::Pop) | S(St::Rave), Hype, legsJackLegs},

    {"swing", Part::Arms, 2, kAll, Chill | Groove, armsSwing},
    {"clap", Part::Arms, 2, S(St::Pop) | S(St::Disco) | S(St::Retro) | S(St::Chill) | S(St::House), Chill | Groove | Hype, armsClap},
    {"discoPoint", Part::Arms, 2, S(St::Disco) | S(St::Pop), Groove | Hype, armsDiscoPoint},
    {"roll", Part::Arms, 4, S(St::Disco) | S(St::Retro), Groove, armsRoll},
    {"hitchhike", Part::Arms, 4, S(St::Disco) | S(St::Retro), Groove, armsHitchhike},
    {"raiseRoof", Part::Arms, 1, S(St::HipHop) | S(St::Rave) | S(St::Pop), Hype, armsRaiseRoof},
    {"fistPump", Part::Arms, 2, S(St::Rave) | S(St::House) | S(St::HipHop), Hype, armsFistPump},
    {"robotArms", Part::Arms, 4, S(St::Robot), Groove | Hype, armsRobot},
    {"tut", Part::Arms, 6, S(St::Robot) | S(St::HipHop), Chill | Groove, armsTut},
    {"armWave", Part::Arms, 4, S(St::Robot) | S(St::Pop) | S(St::Chill) | S(St::HipHop) | S(St::House), Chill | Groove, armsWave},
    {"swim", Part::Arms, 2, S(St::House) | S(St::Retro), Groove, armsSwim},
    {"handsUp", Part::Arms, 4, S(St::Chill) | S(St::Rave) | S(St::Pop) | S(St::House), Chill | Hype, armsHandsUp},
    {"rock", Part::Arms, 2, S(St::HipHop), Groove, armsRock},
    {"sprinkler", Part::Arms, 4, S(St::Retro) | S(St::Pop), Groove, armsSprinkler},
    {"pointCrowd", Part::Arms, 8, S(St::Pop) | S(St::HipHop) | S(St::Disco), Groove | Hype, armsPointCrowd},
    {"glowCircles", Part::Arms, 2, S(St::Rave) | S(St::House) | S(St::Chill), Groove | Hype, armsGlowCircles},
    {"hipHands", Part::Arms, 2, S(St::Pop) | S(St::Chill) | S(St::Disco), Chill | Groove, armsHipHands},
    {"snap", Part::Arms, 2, S(St::Chill) | S(St::Retro), Chill, armsSnap},
    {"monkey", Part::Arms, 2, S(St::Retro), Groove, armsMonkey},
    {"punch", Part::Arms, 2, S(St::HipHop) | S(St::Rave) | S(St::Robot), Hype, armsPunch},
    {"jazzHands", Part::Arms, 2, S(St::Retro) | S(St::Pop), Groove | Hype, armsJazz},
    {"chestPop", Part::Arms, 1, S(St::HipHop) | S(St::Robot), Groove, armsChestPop},
    {"jackArms", Part::Arms, 1, S(St::House), Groove, armsJackArms},

    {"spin", Part::Full, 4, kAll & ~S(St::Chill), Groove | Hype, fullSpin},
    {"jump", Part::Full, 2, kAll, Hype, fullJump},
    {"floss", Part::Full, 2, S(St::Pop) | S(St::HipHop), Hype, fullFloss},
    {"dab", Part::Full, 4, S(St::HipHop) | S(St::Pop), Groove, fullDab},
    {"cabbagePatch", Part::Full, 4, S(St::HipHop) | S(St::Retro), Groove, fullCabbage},
    {"headBang", Part::Full, 1, S(St::Rave), Hype, fullHeadBang},
    {"jumpingJacks", Part::Full, 2, S(St::Pop) | S(St::Rave) | S(St::House), Hype, fullJumpingJacks},
    {"moonwalk", Part::Full, 8, S(St::Robot) | S(St::Pop), Groove, fullMoonwalk},
    {"bodyRoll", Part::Full, 2, S(St::Pop) | S(St::Chill) | S(St::HipHop) | S(St::House), Chill | Groove, fullBodyRoll},
    {"shimmy", Part::Full, 4, S(St::Retro) | S(St::Disco), Groove | Hype, fullShimmy},
    {"twist", Part::Full, 2, S(St::Retro), Groove | Hype, fullTwist},
    {"freeze", Part::Full, 4, S(St::Pop) | S(St::Robot) | S(St::Disco) | S(St::HipHop), Groove | Hype, fullFreeze},
    {"lawnmower", Part::Full, 2, S(St::Retro) | S(St::Pop), Groove, fullLawnmower},
    {"pony", Part::Full, 2, S(St::Retro) | S(St::Rave), Hype, fullPony},
    {"popWalk", Part::Legs, 4, S(St::Robot) | S(St::HipHop), Groove | Hype, legsPopWalk},
    {"tick", Part::Legs, 4, S(St::Robot), Groove, legsTick},
    {"chaCha", Part::Legs, 8, S(St::Retro), Groove, legsChaCha},
    {"toeTap", Part::Legs, 8, S(St::Chill) | S(St::Pop) | S(St::Retro) | S(St::Robot), Chill, legsToeTap},
    {"weightShift", Part::Legs, 4, S(St::Chill) | S(St::House) | S(St::Robot), Chill, legsWeightShift},
    {"looseLegs", Part::Legs, 2, S(St::House) | S(St::Rave), Groove | Hype, legsLooseLegs},
    {"skate", Part::Legs, 8, S(St::House) | S(St::Rave) | S(St::Chill), Chill | Groove, legsSkate},
    {"tickTock", Part::Arms, 4, S(St::Robot), Groove, armsTickTock},
    {"popArms", Part::Arms, 2, S(St::Robot) | S(St::HipHop), Groove | Hype, armsPopArms},
    {"maracas", Part::Arms, 4, S(St::Retro), Groove | Hype, armsMaracas},
    {"lighter", Part::Arms, 8, S(St::Chill) | S(St::Rave) | S(St::Pop), Chill, armsLighter},
    {"fingerGuns", Part::Arms, 8, S(St::Chill) | S(St::Pop) | S(St::Disco) | S(St::Robot), Chill | Groove, armsFingerGuns},
    {"handJive", Part::Arms, 4, S(St::Retro) | S(St::House), Groove, armsHandJive},
    {"crossedNod", Part::Arms, 1, S(St::Chill) | S(St::HipHop) | S(St::House) | S(St::Robot), Chill | Groove, armsCrossedNod},
    {"robotTurn", Part::Full, 4, S(St::Robot), Groove | Hype, fullRobotTurn},
    {"slowMo", Part::Full, 8, S(St::Robot) | S(St::Chill) | S(St::Pop), Chill, fullSlowMo},
    {"vibe", Part::Full, 8, S(St::Chill) | S(St::House), Chill, fullVibe},
    {"conga", Part::Full, 8, S(St::Retro) | S(St::Pop), Groove | Hype, fullConga},
    {"wave", Part::Full, 2, 0, Chill, fullWave},  // hello / goodbye only (no style picks it)
};

std::uint32_t hash32(std::uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}
float unit(std::uint32_t& s) {
    s = hash32(s + 0x9e3779b9U);
    return float(s >> 8) / float(1u << 24);
}

}  // namespace

const char* styleName(Style s) {
    switch (s) {
        case Style::House: return "house";
        case Style::Pop: return "pop";
        case Style::Lock: return "lock";
        case Style::Break: return "break";
        case Style::Disco: return "disco";
        case Style::Stomp: return "stomp";
        case Style::HipHop: return "hiphop";
        case Style::Jazz: return "jazz";
        case Style::Ballet: return "ballet";
        default: return "?";
    }
}

std::optional<Style> parseStyle(std::string_view s) {
    std::string k;
    for (char c : s)
        if (std::isalnum(static_cast<unsigned char>(c))) k.push_back(char(std::tolower(static_cast<unsigned char>(c))));
    for (int i = 0; i < kStyleCount; ++i)
        if (k == styleName(Style(i))) return Style(i);
    // The dances' own names.
    if (k == "popping" || k == "robot" || k == "boogaloo") return Style::Pop;
    if (k == "locking" || k == "funk" || k == "retro" || k == "70s" || k == "60s" || k == "swing" || k == "oldies")
        return Style::Lock;
    if (k == "breaking" || k == "breakdance" || k == "breakdancing" || k == "bboy" || k == "bgirl" || k == "toprock")
        return Style::Break;
    if (k == "waack" || k == "waacking" || k == "whacking" || k == "punking") return Style::Disco;
    if (k == "krump" || k == "krumping") return Style::Stomp;
    if (k == "rave" || k == "shuffle" || k == "techno") return Style::House;
    if (k == "streetjazz" || k == "chill") return Style::Jazz;  // chill: an old style (Peter: to jazz)
    if (k == "balletjazz" || k == "lyrical") return Style::Ballet;
    if (k == "middlehiphop" || k == "lahiphop" || k == "partyhiphop") return Style::HipHop;
    return std::nullopt;
}

const char* styleLabel(Style s) { return s == Style::HipHop ? "hip-hop" : styleName(s); }

const std::vector<MoveDef>& moveLibrary() { return kMoves; }

void runMove(const MoveDef& m, float b, const MoveCtx& c, DancePose& p) {
    if (m.clip >= 0) {
        static const Skeleton kDefault = makeSkeleton(Look{});
        clipPose(m.clip, b, c.sk ? *c.sk : kDefault, p);
    } else if (m.fn) {
        m.fn(b, c, p);
    }
}

int setClipMoves(const std::vector<MoveDef>& moves) {
    kMoves.erase(std::remove_if(kMoves.begin(), kMoves.end(), [](const MoveDef& m) { return m.clip >= 0; }),
                 kMoves.end());
    const int first = int(kMoves.size());
    kMoves.insert(kMoves.end(), moves.begin(), moves.end());
    return first;
}

int findMove(std::string_view name) {
    std::string k;
    for (char c : name)
        if (!std::isspace(static_cast<unsigned char>(c))) k.push_back(char(std::tolower(static_cast<unsigned char>(c))));
    for (std::size_t i = 0; i < kMoves.size(); ++i) {
        std::string n;
        for (const char* p = kMoves[i].name; *p; ++p) n.push_back(char(std::tolower(static_cast<unsigned char>(*p))));
        if (n == k) return int(i);
    }
    return -1;
}

int routineCount(Style s) {
    int legs = 0, arms = 0, full = 0;
    for (const auto& m : kMoves) {
        if (m.clip >= 0 || !(m.styles & styleBit(s))) continue;
        (m.part == Part::Legs ? legs : m.part == Part::Arms ? arms : full)++;
    }
    return legs * arms + full;
}

StyleFeel styleFeel(Style s) {
    switch (s) {
        // The feels of the styles before 2026-11-21 they come closest to (the hand-made moves were tuned to them):
        // house, pop, lock (retro), break / stomp / hip-hop (hip-hop), disco, jazz (pop, a little looser).
        case Style::House: return {1.0f, -0.02f, 0.05f, 0.1f};
        case Style::Pop: return {1.1f, 0.02f, 0.1f, 0.3f};
        case Style::Lock: return {1.0f, 0.02f, 0.05f, 0.35f};
        case Style::Break: return {1.3f, 0.04f, 0.15f, 0.25f};
        case Style::Disco: return {1.0f, 0.02f, 0.05f, 0.25f};
        case Style::Stomp: return {1.3f, 0.04f, 0.15f, 0.25f};
        case Style::HipHop: return {1.3f, 0.04f, 0.15f, 0.25f};
        case Style::Jazz: return {0.9f, 0.02f, 0.2f, 0.25f};
        case Style::Ballet: return {0.9f, 0.02f, 0.2f, 0.25f};
        default: return {1, 0.02f, 0.1f, 0.25f};
    }
}

// ---------------------------------------------------------------- choreographer

Choreographer::Choreographer(const ChoreoOptions& o) : opts_(o), feel_(styleFeel(o.style)) {
    if (opts_.sharp >= 0) feel_.sharp = opts_.sharp;
    rng_ = hash32(o.seed * 2654435761U + 17);
}

Routine Choreographer::pick(double start, double end, const MusicState& g, bool dropHit) {
    Routine r;
    r.start = start;
    r.end = end;
    std::uint32_t s = hash32(rng_ ^ (counter_++ * 0x85ebca6bU));
    const std::uint32_t sb = styleBit(opts_.style);
    // Energy wanted now.
    std::uint8_t want = Groove;
    if (g.build > 0.05) want = g.build > 0.6 ? std::uint8_t(Groove | Hype) : std::uint8_t(Chill);
    if (g.drop > 0.2) want = Hype;
    if (!g.locked) want = Chill | Groove;

    // Moves recorded near Peter's usual tempo (120-130 BPM) come up more often: one recorded far off it plays
    // faster or slower than the dancer did it (at most 3x less likely).
    auto tempo = [](int bpm) {
        const float off = std::max({0.0f, 118.0f - float(bpm), float(bpm) - 132.0f});
        return std::max(0.33f, 1.0f - 0.02f * off);
    };
    // Routines (16 bars, or an 8-bar half) start only on an 8-bar point of the phrase (bar 1 or 9) and then play
    // whole; anywhere else a basic move (4 bars) fills in until the next 8-bar point.
    const bool onEight = std::fabs(std::remainder(phrasePos(start, g), 32.0)) < 1e-6;
    // A mocap move of this style with energy `want` (weighted by energy and tempo, never the last one); -1: none.
    // On an 8-bar point a routine or basics is a deliberate choice (kRoutineChance: routines half the time), not the
    // libraries' sizes (by count it was about a third); then the move within that group by energy and tempo.
    auto chooseClip = [&](std::uint8_t wantE, bool only) {
        auto weight = [&](std::size_t i) {
            const MoveDef& m = kMoves[i];
            if (m.clip < 0 || !(m.styles & sb) || int(i) == lastFull_) return 0.0f;
            if (m.beats >= 32 && !onEight) return 0.0f;
            if (only && !(m.energy & wantE)) return 0.0f;
            return ((m.energy & wantE) ? 1.0f : 0.15f) * tempo(clipInfo(m.clip).bpm);
        };
        float groups[2] = {0, 0};  // basic, routine
        for (std::size_t i = 0; i < kMoves.size(); ++i) groups[kMoves[i].beats >= 32 ? 1 : 0] += weight(i);
        int group = -1;  // -1: either
        if (groups[0] > 0 && groups[1] > 0) group = unit(s) < kRoutineChance ? 1 : 0;
        auto w = [&](std::size_t i) {
            return group < 0 || (kMoves[i].beats >= 32 ? 1 : 0) == group ? weight(i) : 0.0f;
        };
        float total = 0;
        for (std::size_t i = 0; i < kMoves.size(); ++i) total += w(i);
        if (total <= 0) return -1;
        float u = unit(s) * total;
        for (std::size_t i = 0; i < kMoves.size(); ++i) {
            u -= w(i);
            if (u <= 0 && w(i) > 0) return int(i);
        }
        return -1;
    };
    if (dropHit) {
        // The drop (Peter, 2026-11-21: "replace jumps with high-energy mocap move"): a hype mocap move from the drop,
        // by the Mocap share; else the hand-made jump.
        if (opts_.mocap > 0 && unit(s) < opts_.mocap) {
            r.full = chooseClip(Hype, true);
            if (r.full >= 0) {
                lastFull_ = r.full;
                r.mirror = unit(s) < 0.5f;
                alignClip(r, g, false);
                return r;
            }
        }
        r.full = findMove("jump");
        r.amp = 1.2f;
        r.end = start + 2;
        lastFull_ = r.full;
        return r;
    }
    if (tryMove_ >= 0 && tryMove_ < int(kMoves.size())) {
        r.full = tryMove_;
        alignClip(r, g, true);
        return r;
    }
    // A mocap move of this style (reforce.Dancer.Mocap); weighted like the procedural ones by energy, never the same
    // one twice in a row. Plays at least its whole length.
    if (opts_.mocap > 0 && unit(s) < opts_.mocap) {
        r.full = chooseClip(want, false);
        if (r.full >= 0) {
            lastFull_ = r.full;
            r.mirror = unit(s) < 0.5f;
            alignClip(r, g, false);
            return r;
        }
    }
    auto weightOf = [&](int i, int last) {
        const MoveDef& m = kMoves[std::size_t(i)];
        if (m.clip >= 0 || !(m.styles & sb)) return 0.0f;
        float w = (m.energy & want) ? 1.0f : 0.15f;
        if (i == last) w *= 0.05f;
        for (int like : opts_.likes)
            if (like == i) w *= 4.0f;
        return w;
    };
    auto choose = [&](Part part, int last) {
        float total = 0;
        for (std::size_t i = 0; i < kMoves.size(); ++i)
            if (kMoves[i].part == part) total += weightOf(int(i), last);
        if (total <= 0) return -1;
        float u = unit(s) * total;
        for (std::size_t i = 0; i < kMoves.size(); ++i) {
            if (kMoves[i].part != part) continue;
            u -= weightOf(int(i), last);
            if (u <= 0) return int(i);
        }
        return -1;
    };
    const bool full = unit(s) < feel_.fullOdds + (want == Hype ? 0.1f : 0.0f);
    if (full) r.full = choose(Part::Full, lastFull_);
    if (r.full < 0) {
        r.legs = choose(Part::Legs, lastLegs_);
        r.arms = choose(Part::Arms, lastArms_);
        if (r.legs < 0) r.legs = findMove("groove");
        if (r.arms < 0) r.arms = findMove("swing");
        lastLegs_ = r.legs;
        lastArms_ = r.arms;
    } else {
        lastFull_ = r.full;
    }
    r.variant = int(unit(s) * 4) & 3;
    r.mirror = unit(s) < 0.5f;
    const float tp = unit(s);
    r.speed = 1;
    if (want == Chill && tp < feel_.halfTime + 0.25f) r.speed = 0.5f;
    else if (tp < feel_.halfTime) r.speed = 0.5f;
    else if (want == Hype && tp > 0.85f) r.speed = 2;
    if (r.full >= 0 && kMoves[std::size_t(r.full)].beats >= 4) r.speed = std::min(r.speed, 1.0f);
    r.amp = 0.85f + 0.3f * unit(s);
    if (want == Chill) r.amp *= 0.8f;
    if (want == Hype) r.amp *= 1.15f;
    return r;
}

double Choreographer::phrasePos(double beat, const MusicState& g) {
    constexpr double kPhrase = 64;  // 16 bars
    // The phrase line at or before g.beat, from the music's running phrase count.
    const double phraseNow = g.beat - (g.phrase - std::floor(g.phrase)) * kPhrase;
    double pos = std::fmod(beat - phraseNow, kPhrase);
    if (pos < 0) pos += kPhrase;
    return pos > kPhrase - 1e-6 ? 0.0 : pos;
}

void Choreographer::alignClip(Routine& r, const MusicState& g, bool tried) const {
    const double len = std::max(1, kMoves[std::size_t(r.full)].beats);
    const double phraseBeats = 64;  // 16 bars
    const double p0 = r.start - phrasePos(r.start, g);  // the phrase line at or before the start
    r.aligned = true;
    // Tried in the panel, or a routine (started on an 8-bar point): from its own bar 1, whole. Every routine fills 16
    // bars: an 8-bar half plays twice (a style with few whole routines, street jazz, still dances routines half the
    // time).
    if (tried || len >= 32) {
        r.origin = r.start;
        r.end = r.start + (tried ? 1e6 : std::max(len, phraseBeats));
        return;
    }
    // A basic move: on the 4-bar grid of the phrase (joining mid-cycle when picked off it).
    r.origin = p0 + std::floor((r.start - p0) / len + 1e-9) * len;
    double nextPhrase = p0 + phraseBeats;
    if (nextPhrase - r.start < 4) nextPhrase += phraseBeats;  // starting in the last bar: on through the next phrase
    {
        // A short move finishes its cycle (at least the planned bars), but changes on the phrase line.
        const double cycles = std::max(1.0, std::ceil((r.end - r.origin) / len - 1e-9));
        r.end = std::min(r.origin + cycles * len, nextPhrase);
        if (r.end - r.start < 4) r.end = std::min(r.end + len, nextPhrase);
    }
}

void Choreographer::tryMove(int move) {
    if (move == tryMove_) return;
    tryMove_ = move;
    tryChanged_ = true;
}

void Choreographer::farewell(double beat) {
    advance(MusicState{beat});
    const double start = std::ceil(beat);
    while (!routines_.empty() && routines_.back().start >= start) routines_.pop_back();
    if (!routines_.empty()) routines_.back().end = start;
    Routine r;
    r.full = findMove("wave");
    r.start = start;
    r.end = start + 1e6;
    routines_.push_back(r);
}

void Choreographer::hello(double beat) {
    const double start = std::floor(beat);
    Routine r;
    r.full = findMove("wave");
    r.start = start - 1;
    r.end = start + 2;
    routines_.clear();
    routines_.push_back(r);
}

void Choreographer::advance(const MusicState& g) {
    const double beat = g.beat;
    // The beat count jumped (the clock locked, Link joined, a new song): start over from here.
    if (!routines_.empty() && (beat < routines_.front().start - 4 || beat > routines_.back().end + 64)) {
        const bool waving = routines_.back().full == findMove("wave") && routines_.back().end > 1e5;
        routines_.clear();
        if (waving) {  // keep saying goodbye
            Routine r;
            r.full = findMove("wave");
            r.start = std::floor(beat);
            r.end = r.start + 1e6;
            routines_.push_back(r);
            return;
        }
    }
    if (tryChanged_) {
        tryChanged_ = false;
        const double barStart = beat - (g.bar - std::floor(g.bar)) * 4;
        const double start = barStart + 4;
        const bool waving = !routines_.empty() && routines_.back().full == findMove("wave") && routines_.back().end > 1e5;
        if (!waving) {
            while (!routines_.empty() && routines_.back().start >= start) routines_.pop_back();
            if (!routines_.empty()) routines_.back().end = start;
            if (routines_.empty() || tryMove_ >= 0) routines_.push_back(pick(start, start + 8, g, false));
        }
    }
    const bool dropNow = tryMove_ < 0 && g.drop > 0.9 && beat - lastDropBeat_ > 16;
    if (dropNow) {
        lastDropBeat_ = beat;
        const double start = std::round(beat);
        while (!routines_.empty() && routines_.back().start >= start) routines_.pop_back();
        if (!routines_.empty()) routines_.back().end = start;
        routines_.push_back(pick(start, start + 2, g, true));
        routines_.back().created = beat;
    }
    // A routine runs to a bar line 1-4 bars on (weighted to 2), cut at the next phrase line.
    auto nextEnd = [&](double start) {
        const double beatsPerBar = 4;
        const double barStart = beat - (g.bar - std::floor(g.bar)) * beatsPerBar;  // beat of the current bar line
        std::uint32_t s = hash32(rng_ ^ (counter_ * 0x27d4eb2fU));
        const float u = unit(s);
        const int bars = u < 0.15f ? 1 : u < 0.65f ? 2 : u < 0.9f ? 4 : 3;
        double end = barStart + std::ceil((start + 2 - barStart) / beatsPerBar - 1e-6) * beatsPerBar;
        end += double(bars - 1) * beatsPerBar;
        const double phraseBeats = 64;  // 16 bars
        const double nextPhrase = std::round(beat + (1 - (g.phrase - std::floor(g.phrase))) * phraseBeats);
        if (end > nextPhrase && nextPhrase >= start + 2) end = nextPhrase;
        return end;
    };
    if (routines_.empty()) {
        const double start = std::floor(beat);
        routines_.push_back(pick(start, nextEnd(start), g, false));
    }
    int guard = 0;
    // Schedule a beat ahead, so the crossfade into the next routine can start before its bar line.
    while (routines_.back().end <= beat + 1.0 && guard++ < 64) {
        const double start = routines_.back().end;
        routines_.push_back(pick(start, nextEnd(start), g, false));
    }
    while (routines_.size() > 2 && routines_[1].end < beat - 64) routines_.pop_front();
    lastPhrase_ = g.phrase;
}

const Routine* Choreographer::routineAt(double beat) const {
    for (auto it = routines_.rbegin(); it != routines_.rend(); ++it)
        if (beat >= it->start) return &*it;
    return routines_.empty() ? nullptr : &routines_.front();
}

DancePose Choreographer::evaluate(const Routine& r, double beat, const Skeleton& sk) const {
    DancePose p = restPose(sk);
    MoveCtx c;
    c.amp = r.amp * opts_.energy;
    c.sharp = feel_.sharp;
    c.variant = r.variant;
    c.shoulder = sk.shoulderHalf;
    c.foot = sk.footHalf;
    c.sk = &sk;
    // Before its start (fading in) a routine plays the end of its cycle, so it arrives in motion. Mocap moves run on
    // the phrase grid (Routine::origin).
    const double local = (beat - (r.aligned ? r.origin : r.start)) * r.speed;
    auto phase = [&](int m) {
        const double len = double(kMoves[std::size_t(m)].beats);
        double ph = std::fmod(local, len);
        if (ph < 0) ph += len;
        return float(ph);
    };
    if (r.full >= 0) {
        runMove(kMoves[std::size_t(r.full)], phase(r.full), c, p);
    } else {
        if (r.legs >= 0) runMove(kMoves[std::size_t(r.legs)], phase(r.legs), c, p);
        if (r.arms >= 0) runMove(kMoves[std::size_t(r.arms)], phase(r.arms), c, p);
    }
    // The style's pelvis groove on top (down on the beat, or an up-bounce for house).
    if (r.full < 0) {
        const float b = float(std::fmod(local, 1.0));
        if (feel_.groove >= 0) p.hip.y -= feel_.groove * c.amp * std::pow(0.5f + 0.5f * std::cos(TAU * b), 2.0f);
        else p.hip.y += -feel_.groove * c.amp * std::pow(0.5f - 0.5f * std::cos(TAU * b), 2.0f);
    }
    return r.mirror ? mirrored(p) : p;
}

DancePose Choreographer::pose(const MusicState& g, const Skeleton& sk, double delay, bool mirror) {
    advance(g);
    const double beat = g.beat - delay;
    const Routine* cur = routineAt(beat);
    if (!cur) return restPose(sk);
    // Crossfade over 2 x kBlend beats centered on each change (from half a beat before the bar line to half a beat
    // after), with a smootherstep so it eases in and out. A routine that cut in (a drop) fades in only from then.
    const Routine* prev = nullptr;
    const Routine* next = nullptr;
    for (std::size_t i = 0; i < routines_.size(); ++i)
        if (&routines_[i] == cur) {
            if (i > 0) prev = &routines_[i - 1];
            if (i + 1 < routines_.size()) next = &routines_[i + 1];
        }
    auto smoother = [](double x) {
        x = std::clamp(x, 0.0, 1.0);
        return float(x * x * x * (x * (x * 6 - 15) + 10));
    };
    auto weightInto = [&](const Routine& r) {  // 0 -> 1 across r's change
        const double from = std::max(r.start - kBlend, r.created), to = r.start + kBlend;
        return to > from ? smoother((beat - from) / (to - from)) : 1.0f;
    };
    DancePose p = evaluate(*cur, beat, sk);
    if (next && beat > next->start - kBlend && beat >= next->created) {
        p = mix(p, evaluate(*next, beat, sk), weightInto(*next));
    } else if (prev && beat < cur->start + kBlend) {
        p = mix(evaluate(*prev, beat, sk), p, weightInto(*cur));
    }
    return mirror ? mirrored(p) : p;
}

std::string Choreographer::describe(double beat) const {
    const Routine* r = routineAt(beat);
    if (!r) return "";
    std::string s;
    if (r->full >= 0) s = kMoves[std::size_t(r->full)].name;
    else s = std::string(kMoves[std::size_t(r->legs)].name) + " + " + kMoves[std::size_t(r->arms)].name;
    if (r->speed == 0.5f) s += " (half time)";
    if (r->speed == 2) s += " (double time)";
    return s;
}

namespace {
struct Reaction {
    const char* legs;
    const char* arms;
    int variant;
};
constexpr Reaction kReactions[] = {
    {"groove", "clap", 0},        {"groove", "clap", 1},          {"stepTouch", "clap", 3},
    {"groove", "pointCrowd", 0},  {"kneeSway", "raiseRoof", 0},   {"groove", "crossedNod", 0},
    {"weightShift", "hipHands", 1}, {"groove", "swing", 1},       {"stepTouch", "fistPump", 1},
    {"kneeSway", "snap", 0},
};
constexpr int kReactionCount = int(sizeof(kReactions) / sizeof(kReactions[0]));
int reactionIndex(double beat, std::uint32_t seed) {
    const auto slot = std::uint32_t(std::max(0.0, std::floor(beat / 8.0)));
    return int(hash32(seed * 0x9e3779b9U + slot * 0x85ebca6bU) % std::uint32_t(kReactionCount));
}
}  // namespace

DancePose reactionPose(double beat, std::uint32_t seed, Style style, const Skeleton& sk) {
    const Reaction& r = kReactions[reactionIndex(beat, seed)];
    DancePose p = restPose(sk);
    MoveCtx c;
    c.amp = 0.9f;
    c.sharp = styleFeel(style).sharp;
    c.variant = r.variant;
    c.shoulder = sk.shoulderHalf;
    c.foot = sk.footHalf;
    const int legs = findMove(r.legs), arms = findMove(r.arms);
    const MoveDef& L = moveLibrary()[std::size_t(legs)];
    const MoveDef& A = moveLibrary()[std::size_t(arms)];
    L.fn(float(std::fmod(std::max(0.0, beat), double(L.beats))), c, p);
    A.fn(float(std::fmod(std::max(0.0, beat), double(A.beats))), c, p);
    return p;
}

namespace {
// The calm basic move a watcher dances in the 4-bar slot of `beat` (-1: none).
int watchMove(double beat, std::uint32_t seed, Style style) {
    std::vector<int> calm, any;
    for (std::size_t i = 0; i < kMoves.size(); ++i) {
        const MoveDef& m = kMoves[i];
        if (m.clip < 0 || m.beats != 16 || !(m.styles & styleBit(style))) continue;
        any.push_back(int(i));
        if (m.energy & Chill) calm.push_back(int(i));
    }
    const std::vector<int>& from = calm.empty() ? any : calm;
    if (from.empty()) return -1;
    const auto slot = std::uint32_t(std::max(0.0, std::floor(beat / 16.0)));
    return from[hash32(seed * 0x2545f491U + slot * 0x9e3779b9U) % std::uint32_t(from.size())];
}
}  // namespace

DancePose watchPose(double beat, std::uint32_t seed, Style style, const Skeleton& sk, bool mocap) {
    const int m = mocap ? watchMove(beat, seed, style) : -1;
    if (m < 0) return reactionPose(beat, seed, style, sk);
    DancePose p = restPose(sk);
    MoveCtx c;
    c.sk = &sk;
    double ph = std::fmod(beat, 16.0);
    if (ph < 0) ph += 16.0;
    runMove(kMoves[std::size_t(m)], float(ph), c, p);
    return p;
}

std::string watchName(double beat, std::uint32_t seed, Style style, bool mocap) {
    const int m = mocap ? watchMove(beat, seed, style) : -1;
    if (m < 0) return reactionName(beat, seed);
    return std::string(kMoves[std::size_t(m)].name) + " (watching)";
}

std::string reactionName(double beat, std::uint32_t seed) {
    const Reaction& r = kReactions[reactionIndex(beat, seed)];
    return std::string(r.arms) + " (watching)";
}

void faceAccents(DancePose& p, double beat, double t, std::uint32_t seed, float energy) {
    // Blink: about every 3-5 s, 0.15 s long, seeded per dancer.
    const double period = 3.0 + double(hash32(seed) % 2000) / 1000.0;
    const double ph = std::fmod(t + double(hash32(seed + 7) % 1000) / 1000.0 * period, period);
    if (ph < 0.15) p.blink = std::max(p.blink, float(std::sin(ph / 0.15 * 3.14159265)));
    // Mouth: a little "singing" on the beats when energetic.
    const float b = float(beat - std::floor(beat));
    p.mouth = std::max(p.mouth, 0.25f * energy * std::pow(0.5f + 0.5f * std::cos(TAU * b), 4.0f));
}

}  // namespace reforce::engine::dance
