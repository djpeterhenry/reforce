#include "engine/DancerRig.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace reforce::engine::dance {

float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float length(V3 a) { return std::sqrt(dot(a, a)); }
V3 normalize(V3 a, V3 fallback) {
    const float l = length(a);
    if (!(l > 1e-9f)) return fallback;
    return a * (1.0f / l);
}
V3 lerp(V3 a, V3 b, float t) { return a + (b - a) * t; }

Frame Frame::translated(V3 local) const {
    Frame f = *this;
    f.o = point(local);
    return f;
}
Frame Frame::rotatedX(float a) const {
    const float c = std::cos(a), s = std::sin(a);
    Frame f = *this;
    f.y = y * c + z * s;
    f.z = z * c - y * s;
    return f;
}
Frame Frame::rotatedY(float a) const {
    const float c = std::cos(a), s = std::sin(a);
    Frame f = *this;
    f.z = z * c + x * s;
    f.x = x * c - z * s;
    return f;
}
Frame Frame::rotatedZ(float a) const {
    const float c = std::cos(a), s = std::sin(a);
    Frame f = *this;
    f.y = y * c - x * s;
    f.x = x * c + y * s;
    return f;
}

Frame frameAlong(V3 o, V3 axis, V3 xHint) {
    Frame f;
    f.o = o;
    f.y = normalize(axis);
    V3 x = xHint - f.y * dot(xHint, f.y);
    if (length(x) < 1e-4f) {
        const V3 alt = std::fabs(f.y.x) < 0.9f ? V3{1, 0, 0} : V3{0, 0, 1};
        x = alt - f.y * dot(alt, f.y);
    }
    f.x = normalize(x);
    f.z = cross(f.x, f.y);
    return f;
}

std::optional<Rgb> parseRgb(std::string_view text) {
    std::string s;
    for (char c : text)
        if (!std::isspace(static_cast<unsigned char>(c)) && c != '"') s.push_back(c);
    if (!s.empty() && s[0] == '#') s.erase(0, 1);
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        c = char(std::tolower(static_cast<unsigned char>(c)));
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    int v[6];
    if (s.size() == 3) {
        for (int i = 0; i < 3; ++i) {
            const int h = hex(s[std::size_t(i)]);
            if (h < 0) return std::nullopt;
            v[2 * i] = v[2 * i + 1] = h;
        }
    } else if (s.size() == 6) {
        for (int i = 0; i < 6; ++i)
            if ((v[i] = hex(s[std::size_t(i)])) < 0) return std::nullopt;
    } else {
        return std::nullopt;
    }
    return Rgb{float(v[0] * 16 + v[1]) / 255.0f, float(v[2] * 16 + v[3]) / 255.0f, float(v[4] * 16 + v[5]) / 255.0f};
}

namespace {
std::string lower(std::string_view s) {
    std::string out;
    for (char c : s)
        if (!std::isspace(static_cast<unsigned char>(c)) && c != '"')
            out.push_back(char(std::tolower(static_cast<unsigned char>(c))));
    return out;
}
float clampf(float v, float lo, float hi) { return std::min(hi, std::max(lo, v)); }
}  // namespace

std::optional<HairStyle> parseHairStyle(std::string_view s) {
    const std::string k = lower(s);
    if (k == "short") return HairStyle::Short;
    if (k == "bob") return HairStyle::Bob;
    if (k == "long") return HairStyle::Long;
    if (k == "spiky") return HairStyle::Spiky;
    if (k == "afro") return HairStyle::Afro;
    if (k == "ponytail") return HairStyle::Ponytail;
    if (k == "bun") return HairStyle::Bun;
    if (k == "bald" || k == "none") return HairStyle::Bald;
    if (k == "mohawk") return HairStyle::Mohawk;
    if (k == "pigtails") return HairStyle::Pigtails;
    if (k == "curly" || k == "curls" || k == "ringlets") return HairStyle::Curly;
    if (k == "pompadour") return HairStyle::Pompadour;
    if (k == "sidepart" || k == "side-part" || k == "swoop" || k == "emo") return HairStyle::SidePart;
    if (k == "undercut" || k == "fade") return HairStyle::Undercut;
    if (k == "hightop" || k == "high-top" || k == "flattop") return HairStyle::Coils;  // cut 2026-11-21
    if (k == "bowl" || k == "bowlcut") return HairStyle::Bowl;
    if (k == "mullet") return HairStyle::Mullet;
    if (k == "shag") return HairStyle::Shag;
    if (k == "pixie") return HairStyle::Pixie;
    if (k == "bangs" || k == "fringe") return HairStyle::Bangs;
    if (k == "wavy" || k == "locs" || k == "dreadlocks" || k == "dreads" || k == "boxbraids" || k == "box-braids")
        return HairStyle::Long;  // cut 2026-11-21
    if (k == "braid" || k == "plait") return HairStyle::Braid;
    if (k == "sidepony" || k == "side-pony" || k == "sideponytail") return HairStyle::SidePony;
    if (k == "spacebuns" || k == "space-buns" || k == "buns") return HairStyle::SpaceBuns;
    if (k == "topknot" || k == "top-knot") return HairStyle::TopKnot;
    if (k == "buzz" || k == "buzzcut") return HairStyle::Buzz;
    if (k == "crew" || k == "crewcut") return HairStyle::Short;  // merged 2026-11-21
    if (k == "crop" || k == "frenchcrop") return HairStyle::Short;  // cut 2026-11-21
    if (k == "slickback" || k == "slick-back" || k == "slicked") return HairStyle::SlickBack;
    if (k == "curtains" || k == "middlepart" || k == "middle-part") return HairStyle::Curtains;
    if (k == "balding" || k == "receding") return HairStyle::Balding;
    if (k == "manbun" || k == "man-bun") return HairStyle::ManBun;
    if (k == "quiff") return HairStyle::Quiff;
    if (k == "shaved" || k == "shaven" || k == "shavedhead") return HairStyle::Shaved;
    if (k == "anime" || k == "messy" || k == "tousled" || k == "textured") return HairStyle::Anime;
    if (k == "coils" || k == "twists") return HairStyle::Coils;
    if (k == "waves" || k == "360waves" || k == "360") return HairStyle::Waves;
    if (k == "lob" || k == "shoulder") return HairStyle::Bob;  // cut 2026-11-21
    if (k == "longcurls" || k == "long-curls") return HairStyle::Curly;  // cut 2026-11-21
    if (k == "layers" || k == "layered") return HairStyle::Layers;
    if (k == "halfup" || k == "half-up") return HairStyle::HalfUp;
    if (k == "lowbun" || k == "low-bun" || k == "chignon") return HairStyle::Ponytail;  // cut 2026-11-21
    return std::nullopt;
}
std::optional<Outfit> parseOutfit(std::string_view s) {
    const std::string k = lower(s);
    if (k == "tee" || k == "tshirt" || k == "t-shirt") return Outfit::Tee;
    if (k == "tank") return Outfit::Tank;
    if (k == "hoodie") return Outfit::Hoodie;
    if (k == "dress") return Outfit::Dress;
    if (k == "suit") return Outfit::Suit;
    if (k == "skirt") return Outfit::Skirt;
    if (k == "croptop" || k == "crop-top" || k == "crop") return Outfit::CropTop;
    if (k == "jumpsuit" || k == "overalls") return Outfit::Jumpsuit;
    if (k == "longdress" || k == "long-dress" || k == "gown" || k == "maxi") return Outfit::LongDress;
    return std::nullopt;
}
std::optional<Hat> parseHat(std::string_view s) {
    const std::string k = lower(s);
    if (k == "none" || k.empty()) return Hat::None;
    if (k == "cap") return Hat::Cap;
    if (k == "beanie") return Hat::Beanie;
    if (k == "top" || k == "tophat") return Hat::Top;
    if (k == "headphones") return Hat::Headphones;
    return std::nullopt;
}

namespace {
template <typename E, std::size_t N>
std::optional<E> parseNamed(std::string_view s, const char* const (&names)[N]) {
    const std::string k = lower(s);
    for (std::size_t i = 0; i < N; ++i)
        if (k == names[i]) return E(i);
    return std::nullopt;
}
}  // namespace

std::optional<Pattern> parsePattern(std::string_view s) {
    static const char* const n[] = {"none", "stripes", "dots", "plaid", "gradient", "zigzag"};
    return parseNamed<Pattern>(s, n);
}
std::optional<Fabric> parseFabric(std::string_view s) {
    static const char* const n[] = {"matte", "shiny", "sequins", "velvet", "neon"};
    return parseNamed<Fabric>(s, n);
}
std::optional<Glasses> parseGlasses(std::string_view s) {
    static const char* const n[] = {"none", "round", "aviator", "visor", "heart", "star", "cateye", "square"};
    return parseNamed<Glasses>(s, n);
}
std::optional<EyeShape> parseEyeShape(std::string_view s) {
    static const char* const n[] = {"oval", "round", "sleepy", "happy", "big", "dot"};
    return parseNamed<EyeShape>(s, n);
}
std::optional<Brows> parseBrows(std::string_view s) {
    static const char* const n[] = {"normal", "thick", "thin", "arched"};
    return parseNamed<Brows>(s, n);
}
std::optional<Mouth> parseMouth(std::string_view s) {
    static const char* const n[] = {"smile", "grin", "small", "smirk"};
    return parseNamed<Mouth>(s, n);
}
std::optional<Beard> parseBeard(std::string_view s) {
    static const char* const n[] = {"none", "stubble", "moustache", "goatee", "full", "handlebar", "circle", "short",
                                    "long"};
    if (lower(s) == "chinstrap") return Beard::Short;  // cut 2026-11-21
    return parseNamed<Beard>(s, n);
}

Skeleton makeSkeleton(const Look& look) {
    const float t = clampf(look.tall, 0.7f, 1.3f), b = clampf(look.build, 0.7f, 1.4f);
    const float L = clampf(look.legs, 0.75f, 1.25f), T = clampf(look.torso, 0.8f, 1.2f);
    Skeleton s;
    s.ankleY = 0.05f * t;
    s.thigh = 0.17f * t * L;
    s.shin = 0.165f * t * L;
    s.hipHalf = 0.072f * b;
    s.footHalf = 0.088f * b;
    const float reach = 0.965f * (s.thigh + s.shin), dx = s.footHalf - s.hipHalf;
    s.hipY = s.ankleY + std::sqrt(std::max(0.0f, reach * reach - dx * dx)) + 0.01f;
    s.spine = 0.20f * t * T;
    s.shoulderHalf = 0.15f * b * clampf(look.shoulders, 0.8f, 1.25f);
    s.shoulderY = 0.02f * t;
    s.upperArm = 0.12f * t;
    s.forearm = 0.115f * t;
    s.neck = 0.045f * t;
    s.headR = 0.18f * (0.6f + 0.4f * t) * clampf(look.head, 0.75f, 1.35f);
    return s;
}

DancePose restPose(const Skeleton& s) {
    DancePose p;
    p.handL = {s.shoulderHalf + 0.04f, s.shoulderY - 0.215f, 0.03f};
    p.handR = {-p.handL.x, p.handL.y, p.handL.z};
    p.footL = {s.footHalf, 0, 0};
    p.footR = {-s.footHalf, 0, 0};
    return p;
}

DancePose mix(const DancePose& a, const DancePose& b, float t) {
    auto f = [t](float x, float y) { return x + (y - x) * t; };
    DancePose p;
    p.bodyX = f(a.bodyX, b.bodyX);
    p.bodyZ = f(a.bodyZ, b.bodyZ);
    p.bodyYaw = a.bodyYaw + std::remainder(b.bodyYaw - a.bodyYaw, 6.2831853f) * t;  // the short way round
    p.hip = lerp(a.hip, b.hip, t);
    p.hipYaw = f(a.hipYaw, b.hipYaw);
    p.hipRoll = f(a.hipRoll, b.hipRoll);
    p.hipPitch = f(a.hipPitch, b.hipPitch);
    p.chestYaw = f(a.chestYaw, b.chestYaw);
    p.chestRoll = f(a.chestRoll, b.chestRoll);
    p.chestPitch = f(a.chestPitch, b.chestPitch);
    p.headYaw = f(a.headYaw, b.headYaw);
    p.headRoll = f(a.headRoll, b.headRoll);
    p.headPitch = f(a.headPitch, b.headPitch);
    p.handL = lerp(a.handL, b.handL, t);
    p.handR = lerp(a.handR, b.handR, t);
    p.footL = lerp(a.footL, b.footL, t);
    p.footR = lerp(a.footR, b.footR, t);
    p.footYawL = f(a.footYawL, b.footYawL);
    p.footYawR = f(a.footYawR, b.footYawR);
    p.footPitchL = f(a.footPitchL, b.footPitchL);
    p.footPitchR = f(a.footPitchR, b.footPitchR);
    p.mouth = f(a.mouth, b.mouth);
    p.blink = f(a.blink, b.blink);
    p.brow = f(a.brow, b.brow);
    p.fist = f(a.fist, b.fist);
    p.elbowL = lerp(a.elbowL, b.elbowL, t);
    p.elbowR = lerp(a.elbowR, b.elbowR, t);
    p.kneeL = lerp(a.kneeL, b.kneeL, t);
    p.kneeR = lerp(a.kneeR, b.kneeR, t);
    return p;
}

namespace {
template <typename F>
void forEachPoseFloat(DancePose& p, F&& f) {
    float* scalars[] = {&p.bodyX, &p.bodyZ, &p.bodyYaw, &p.hip.x, &p.hip.y, &p.hip.z, &p.hipYaw, &p.hipRoll, &p.hipPitch,
                        &p.chestYaw, &p.chestRoll, &p.chestPitch, &p.headYaw, &p.headRoll, &p.headPitch,
                        &p.handL.x, &p.handL.y, &p.handL.z, &p.handR.x, &p.handR.y, &p.handR.z,
                        &p.footL.x, &p.footL.y, &p.footL.z, &p.footR.x, &p.footR.y, &p.footR.z,
                        &p.footYawL, &p.footYawR, &p.footPitchL, &p.footPitchR,
                        &p.mouth, &p.blink, &p.brow, &p.fist,
                        &p.elbowL.x, &p.elbowL.y, &p.elbowL.z, &p.elbowR.x, &p.elbowR.y, &p.elbowR.z,
                        &p.kneeL.x, &p.kneeL.y, &p.kneeL.z, &p.kneeR.x, &p.kneeR.y, &p.kneeR.z};
    static_assert(sizeof(scalars) / sizeof(scalars[0]) == kPoseFloats);
    int i = 0;
    for (float* s : scalars) f(i++, *s);
}
}  // namespace

void poseToFloats(const DancePose& p, float* out) {
    DancePose q = p;
    forEachPoseFloat(q, [&](int i, float& v) { out[i] = v; });
}

DancePose poseFromFloats(const float* in) {
    DancePose p;
    forEachPoseFloat(p, [&](int i, float& v) { v = in[i]; });
    return p;
}

DancePose mirrored(const DancePose& q) {
    DancePose p = q;
    auto mx = [](V3 v) { return V3{-v.x, v.y, v.z}; };
    p.bodyX = -q.bodyX;
    p.bodyYaw = -q.bodyYaw;
    p.hip = mx(q.hip);
    p.hipYaw = -q.hipYaw;
    p.hipRoll = -q.hipRoll;
    p.chestYaw = -q.chestYaw;
    p.chestRoll = -q.chestRoll;
    p.headYaw = -q.headYaw;
    p.headRoll = -q.headRoll;
    p.handL = mx(q.handR);
    p.handR = mx(q.handL);
    p.footL = mx(q.footR);
    p.footR = mx(q.footL);
    p.footYawL = q.footYawR;
    p.footYawR = q.footYawL;
    p.footPitchL = q.footPitchR;
    p.footPitchR = q.footPitchL;
    p.elbowL = mx(q.elbowR);
    p.elbowR = mx(q.elbowL);
    p.kneeL = mx(q.kneeR);
    p.kneeR = mx(q.kneeL);
    return p;
}

IkResult solveTwoBone(V3 root, V3 target, float a, float b, V3 pole) {
    IkResult r;
    const V3 d = target - root;
    const float L = length(d);
    const V3 dir = normalize(d, {0, -1, 0});
    const float maxL = 0.995f * (a + b), minL = std::max(std::fabs(a - b) * 1.01f, 1e-4f);
    const float Lc = clampf(L, minL, maxL);
    r.reached = L <= maxL + 1e-5f && L >= minL - 1e-5f;
    r.end = root + dir * Lc;
    const float cosA = clampf((a * a + Lc * Lc - b * b) / (2.0f * a * Lc), -1.0f, 1.0f);
    const float sinA = std::sqrt(std::max(0.0f, 1.0f - cosA * cosA));
    V3 p = pole - dir * dot(pole, dir);
    if (length(p) < 1e-5f) {
        const V3 alt = std::fabs(dir.x) < 0.9f ? V3{1, 0, 0} : V3{0, 0, 1};
        p = cross(dir, alt);
    }
    p = normalize(p);
    r.mid = root + dir * (a * cosA) + p * (a * sinA);
    return r;
}

namespace {

Rgb scaled(Rgb c, float k) { return {c.r * k, c.g * k, c.b * k}; }

struct Builder {
    const Frame& place;
    float scale;
    std::vector<BodyPart>& parts;

    V3 world(V3 p) const { return place.o + place.dir(p) * scale; }
    Frame worldFrame(const Frame& f) const {
        Frame w;
        w.o = world(f.o);
        w.x = place.dir(f.x);
        w.y = place.dir(f.y);
        w.z = place.dir(f.z);
        return w;
    }
    // Every length is in character units and scaled here.
    BodyPart& add(const Frame& f, float len, float r0, float r1, Rgb c0, Rgb c1 = {-1, -1, -1}, float split = 1e9f) {
        BodyPart p;
        p.frame = worldFrame(f);
        p.len = len * scale;
        p.r0 = r0 * scale;
        p.r1 = r1 * scale;
        p.color0 = c0;
        p.color1 = c1.r < 0 ? c0 : c1;
        p.split = split >= 1e8f ? split : split * scale;
        parts.push_back(p);
        return parts.back();
    }
    BodyPart& segment(V3 a, V3 b, V3 xHint, float r0, float r1, Rgb c0, Rgb c1 = {-1, -1, -1}, float split = 1e9f) {
        return add(frameAlong(a, b - a, xHint), length(b - a), r0, r1, c0, c1, split);
    }
    BodyPart& ball(const Frame& f, float r, float sx, float sz, Rgb c) {
        BodyPart& p = add(f, 0, r, r, c);
        p.scaleX = sx;
        p.scaleZ = sz;
        return p;
    }
    void clip(BodyPart& p, V3 n, float w) {
        n = normalize(n);
        p.clip = {n.x, n.y, n.z, w * scale};
    }
};

}  // namespace

namespace {
// ---- hat-aware hair
// A hat as the hair sees it, in the head frame (character units): the band line (a plane: above it is under the
// hat) and the hat's inside above it. Numbers match the hats built in buildBody.
struct HatShape {
    V3 up;            // the band plane's normal, pointing up into the hat
    float line;       // dot(up, p) > line: above the band
    // Cap and beanie: an ellipsoid (center, radii). Top hat: the crown (a cylinder of radius crownR about the head's
    // y axis) and the brim (radii brimX, brimZ) along the band.
    bool topHat = false;
    V3 center;
    V3 radii;
    float crownR = 0, brimX = 0, brimZ = 0;
};
HatShape hatShape(Hat hat, float R, float hs) {
    HatShape h;
    auto band = [&](V3 n, V3 c, float w) {  // a hat clipped where dot(p - c, n) > w (n pointing down)
        n = normalize(n);
        h.up = -n;
        h.line = -(dot(n, c) + w);
    };
    if (hat == Hat::Cap) {
        h.center = V3{0, 0.02f, -0.005f} * hs;
        h.radii = {R * 1.09f * 1.05f, R * 1.09f, R * 1.09f};
        band({0, -1, 0.15f}, h.center, -0.18f * R);
    } else if (hat == Hat::Beanie) {
        h.center = V3{0, 0.03f, -0.005f} * hs;
        h.radii = {R * 1.12f * 1.05f, R * 1.12f, R * 1.12f};
        band({0, -1, 0.35f}, h.center, -0.12f * R);
    } else {  // top hat: the brim's plane
        h.topHat = true;
        h.up = {0, 1, 0};
        h.line = R * 0.78f;
        h.crownR = 0.11f * hs;
        h.brimX = 0.018f * hs * 9.5f;
        h.brimZ = 0.018f * hs * 9.0f;
    }
    return h;
}
// Whether a head-frame point above (or just below) the band is covered by the hat.
bool underHat(const HatShape& h, V3 p, float R) {
    if (h.topHat) {
        const float rr = p.x * p.x + p.z * p.z;
        if (dot(h.up, p) > h.line + 0.03f * R) return rr <= h.crownR * h.crownR * 0.98f;
        const float ex = p.x / h.brimX, ez = p.z / h.brimZ;
        return ex * ex + ez * ez <= 0.97f;
    }
    const V3 d = p - h.center;
    const float ex = d.x / h.radii.x, ey = d.y / h.radii.y, ez = d.z / h.radii.z;
    return ex * ex + ey * ey + ez * ez <= 0.985f * 0.985f;
}
// Calls fn(q) for points on a part's surface, in its local frame (before scaleX / scaleZ, as the clip plane is).
template <class Fn>
void forSurface(const BodyPart& p, Fn&& fn) {
    const int around = 16, along = 6, cap = 4;
    for (int i = 0; i < around; ++i) {
        const float a = 6.2831853f * float(i) / float(around), ca = std::cos(a), sa = std::sin(a);
        for (int j = 0; j <= along; ++j) {
            const float t = float(j) / float(along), r = p.r0 + (p.r1 - p.r0) * t;
            fn(V3{r * ca, p.len * t, r * sa});
        }
        for (int k = 1; k <= cap; ++k) {
            const float e = 1.5707963f * float(k) / float(cap), ce = std::cos(e), se = std::sin(e);
            fn(V3{p.r1 * ce * ca, p.len + p.r1 * se, p.r1 * ce * sa});
            fn(V3{p.r0 * ce * ca, -p.r0 * se, p.r0 * ce * sa});
        }
    }
}
}  // namespace

// Hair under a hat: every hair part (from `begin` on) that has no clip plane of its own is cut at the hat's band
// (nothing of it shows above the band: spikes, buns and the tops of big hair disappear under the hat), and any part
// whose surface at or above the band would still poke out of the hat (the cut of a big mass, a shell standing off
// the head) shrinks until it fits (a shell towards the head center, a mass or a lock towards its lowest point, so
// it still hangs below the hat); one mostly above the band, or that would have to shrink by over 55 %, is dropped.
// Stateless and allocation-free: it runs every frame, after the hair has swung.
void fitHairUnderHat(std::vector<BodyPart>& parts, std::size_t begin, const HatShape& hat, const Frame& head,
                     float scale) {
    const float k = std::max(scale, 1e-4f);
    const float R = std::max(hat.topHat ? hat.line / 0.78f : hat.radii.y / 1.09f, 1e-4f);
    const V3 upW = head.dir(hat.up);
    const float lineW = dot(upW, head.o) + k * hat.line;  // world: dot(upW, x) > lineW is above the band
    auto toHead = [&](V3 x) {
        const V3 d = (x - head.o) * (1.0f / k);
        return V3{dot(d, head.x), dot(d, head.y), dot(d, head.z)};
    };
    auto world = [](const BodyPart& p, V3 q) {
        return p.frame.o + p.frame.x * (q.x * p.scaleX) + p.frame.y * q.y + p.frame.z * (q.z * p.scaleZ);
    };
    auto clipAtBand = [&](BodyPart& p) {  // the band plane in the part's local frame (discard above, a hair up)
        const V3 n{p.scaleX * dot(upW, p.frame.x), dot(upW, p.frame.y), p.scaleZ * dot(upW, p.frame.z)};
        p.clip = {n.x, n.y, n.z, lineW + 0.01f * R * k - dot(upW, p.frame.o)};
    };
    auto fits = [&](const BodyPart& p) {
        bool ok = true;
        forSurface(p, [&](V3 q) {
            if (!ok || q.x * p.clip[0] + q.y * p.clip[1] + q.z * p.clip[2] > p.clip[3] ||
                q.x * p.clip2[0] + q.y * p.clip2[1] + q.z * p.clip2[2] > p.clip2[3])
                return;  // cut away
            const V3 h = toHead(world(p, q));
            if (dot(hat.up, h) > hat.line - 0.04f * R && !underHat(hat, h, R)) ok = false;
        });
        return ok;
    };
    std::size_t out = begin;
    for (std::size_t i = begin; i < parts.size(); ++i) {
        BodyPart p = parts[i];
        const bool ownClip = p.clip[3] < 1e8f;
        if (!ownClip) {
            // Mostly under the hat (a spike, a bun, bangs): leave it out rather than show a cut-off stub.
            int above = 0, all = 0;
            forSurface(p, [&](V3 q) {
                ++all;
                above += dot(upW, world(p, q)) > lineW;
            });
            if (above * 2 > all) continue;
            clipAtBand(p);
        }
        if (!fits(p)) {
            // Shrink (the largest size, to 2 %, that fits): a shell towards the head center; anything else towards
            // its own lowest point, so hair hanging below the hat keeps its length and only loses height.
            const BodyPart full = p;
            V3 anchor = head.o;
            if (!ownClip) {
                float lowest = 1e30f;
                forSurface(full, [&](V3 q) {
                    const V3 x = world(full, q);
                    if (dot(upW, x) < lowest) {
                        lowest = dot(upW, x);
                        anchor = x;
                    }
                });
            }
            auto shrunk = [&](float f) {
                BodyPart s = full;
                s.frame.o = anchor + (full.frame.o - anchor) * f;
                s.len *= f;
                s.r0 *= f;
                s.r1 *= f;
                if (ownClip) s.clip[3] = full.clip[3] * f;
                else clipAtBand(s);
                if (full.clip2[3] < 1e8f) s.clip2[3] = full.clip2[3] * f;
                return s;
            };
            float lo = 0.45f, hi = 1.0f;
            if (!fits(shrunk(lo))) continue;  // dropped
            for (int it = 0; it < 5; ++it) {
                const float mid = 0.5f * (lo + hi);
                (fits(shrunk(mid)) ? lo : hi) = mid;
            }
            p = shrunk(lo);
        }
        parts[out++] = p;
    }
    parts.resize(out);
}

namespace {
// The body frame, the pelvis and the chest for a pose.
Frame bodyOf(const DancePose& pose) {
    return Frame{}.translated({pose.bodyX, 0, pose.bodyZ}).rotatedY(pose.bodyYaw);
}
Frame pelvisOf(const Frame& body, const Skeleton& sk, const DancePose& pose) {
    return body.translated(V3{0, sk.hipY, 0} + pose.hip)
        .rotatedY(pose.hipYaw)
        .rotatedZ(pose.hipRoll)
        .rotatedX(pose.hipPitch);
}
Frame chestOf(const Frame& pelvis, const Skeleton& sk, const DancePose& pose) {
    return pelvis.translated({0, sk.spine, 0}).rotatedY(pose.chestYaw).rotatedZ(pose.chestRoll).rotatedX(pose.chestPitch);
}
// The face stays towards the viewer: nods and turns are limited (a big nod would show the top of the head).
Frame headOn(const Frame& chest, const Skeleton& sk, const DancePose& pose) {
    return chest.translated({0, sk.neck, 0})
        .rotatedY(clampf(pose.headYaw, -0.7f, 0.7f))
        .rotatedZ(clampf(pose.headRoll, -0.4f, 0.4f))
        .rotatedX(clampf(pose.headPitch, -0.35f, 0.32f) - 0.4f * clampf(pose.chestPitch, 0.0f, 0.5f))
        .translated({0, 0.9f * sk.headR, 0});
}
}  // namespace

// The bend direction for a two-bone chain: the rig's own (`own`, world) turned towards a pose's bend hint (in
// `frame`) as far as the hint's length says (0: own, 1: the hint).
static V3 hintedPole(V3 own, const Frame& frame, V3 hint) {
    const float w = std::min(1.0f, length(hint));
    if (w < 1e-4f) return own;
    return normalize(own) * (1 - w) + frame.dir(normalize(hint)) * w;
}

Frame headFrame(const Skeleton& sk, const DancePose& pose) {
    const Frame body = bodyOf(pose);
    return headOn(chestOf(pelvisOf(body, sk, pose), sk, pose), sk, pose);
}

void buildBody(const Look& look, const Skeleton& sk, const DancePose& pose, const Frame& placement, float scale,
               BodyOut& out) {
    Builder B{placement, scale, out.parts};
    const float build = clampf(look.build, 0.7f, 1.4f);
    const float limb = clampf(look.limbs, 0.75f, 1.35f);
    const float belly = clampf(look.belly, 0.0f, 1.0f);
    const float hs = clampf(look.head, 0.75f, 1.35f);  // head-relative offsets (hair, hats, glasses) scale with it

    // ---- frames (character space)
    const Frame body = bodyOf(pose);
    const Frame pelvis = pelvisOf(body, sk, pose);
    const Frame chest = chestOf(pelvis, sk, pose);
    const float R = sk.headR;
    const Frame head = headOn(chest, sk, pose);

    const Rgb sole = scaled(look.shoes, 0.55f);
    const Outfit of = look.outfit;
    // Bare legs under a dress or a skirt (the skirt in the bottom color, a dress in the top color); a jumpsuit's
    // trousers in the top color.
    const bool skirt = of == Outfit::Dress || of == Outfit::Skirt || of == Outfit::LongDress;
    const Rgb legColor = skirt ? look.skin : of == Outfit::Jumpsuit ? look.top : look.bottom;

    // Cloth regions of a part (color0 / color1): skin, the top or the bottom (their print and fabric).
    enum Region { Skin, Top, Bottom };
    auto dress = [&](BodyPart& part, Region r0, Region r1) {
        const Region rs[2] = {r0, r1};
        for (int i = 0; i < 2; ++i) {
            if (rs[i] == Top) {
                part.pattern[i] = std::uint8_t(look.topPattern);
                part.fabric[i] = std::uint8_t(look.topFabric);
            } else if (rs[i] == Bottom) {
                part.pattern[i] = std::uint8_t(look.bottomPattern);
                part.fabric[i] = std::uint8_t(look.bottomFabric);
            }
        }
        part.ink = look.ink;
        part.period = 0.05f * scale;
    };

    // ---- legs and shoes
    V3 knees[2], ankles[2];
    for (int side = 0; side < 2; ++side) {
        const float sx = side == 0 ? 1.0f : -1.0f;
        const V3 foot = side == 0 ? pose.footL : pose.footR;
        const float yaw = pose.bodyYaw + sx * (side == 0 ? pose.footYawL : pose.footYawR);
        const float pitch = side == 0 ? pose.footPitchL : pose.footPitchR;
        const V3 hipJ = pelvis.point({sx * sk.hipHalf, -0.01f, 0});
        const V3 ankleT = body.point(foot + V3{0, sk.ankleY, 0});
        const V3 pole = hintedPole(body.dir({sx * 0.3f, 0, 1}), body, side == 0 ? pose.kneeL : pose.kneeR);
        const IkResult ik = solveTwoBone(hipJ, ankleT, sk.thigh, sk.shin, pole);
        knees[side] = ik.mid;
        ankles[side] = ik.end;
        const Region legR = skirt ? Skin : of == Outfit::Jumpsuit ? Top : Bottom;
        if (!skirt)  // a skirt hides the thighs
            dress(B.segment(hipJ, ik.mid, pelvis.x, 0.062f * build * limb, 0.05f * limb, legColor), legR, legR);
        dress(B.segment(ik.mid, ik.end, pelvis.x, 0.05f * limb, 0.043f * limb, legColor), legR, legR);
        const Frame ff = Frame{}.translated(ik.end).rotatedY(yaw).rotatedX(-pitch);
        BodyPart& shoe = B.ball(ff.translated({0, -0.012f, 0.032f}), 0.046f, 1.2f, 2.0f, sole);
        shoe.color1 = look.shoes;
        shoe.split = -0.018f * scale;
        out.feet[side] = B.world(ik.end);
    }

    // ---- torso: one capsule from the pelvis to the chest (bends with the spine)
    {
        const V3 a = pelvis.point({0, 0.0f, 0}), b = chest.point({0, 0.035f, 0});
        const V3 xh = normalize(pelvis.x + chest.x, chest.x);
        // The torso's lower part: the dress / jumpsuit (top color), the skirt or trousers (bottom), or a bare
        // midriff under a crop top.
        const bool topBelow = of == Outfit::Dress || of == Outfit::LongDress || of == Outfit::Jumpsuit;
        const Rgb low = topBelow ? look.top : of == Outfit::CropTop ? look.skin : look.bottom;
        const Region lowR = topBelow ? Top : of == Outfit::CropTop ? Skin : Bottom;
        BodyPart& torso = B.segment(a, b, xh, 0.102f * (1 + 0.22f * belly), 0.112f, low, look.top,
                                    of == Outfit::CropTop ? 0.15f : 0.085f);
        torso.scaleX = 1.15f * build;
        torso.scaleZ = 0.86f + 0.22f * belly;
        dress(torso, lowR, Top);
        if (of == Outfit::CropTop) {
            // The trousers' waist round the hips, cut flat a little above the pelvis: the midriff shows between it
            // and the top.
            BodyPart& w = B.ball(pelvis.translated({0, -0.005f, 0}), 0.107f * (1 + 0.22f * belly), 1.15f * build,
                                 0.88f + 0.22f * belly, look.bottom);
            B.clip(w, {0, 1, 0}, 0.055f);
            dress(w, Bottom, Bottom);
        }
        if (of == Outfit::Jumpsuit) {  // a belt
            BodyPart& belt = B.segment(pelvis.point({0, 0.075f, 0}), pelvis.point({0, 0.1f, 0}), pelvis.x,
                                       0.111f * (1 + 0.22f * belly), 0.111f * (1 + 0.22f * belly), look.accent);
            belt.scaleX = 1.15f * build;
            belt.scaleZ = 0.88f + 0.22f * belly;
            belt.outline = false;
        }
        if (skirt) {
            // A bell skirt that follows the legs: its axis points from the waist to between the knees (a long dress:
            // the ankles; a skirt: just above the knees), the hem just past them, and it flares wide enough that
            // neither leg pokes through (it swings with steps and kicks). Cut flat at the hem; two-sided so the cut
            // shows its inside.
            const V3 w0 = pelvis.point({0, 0.07f, 0});
            const bool longDress = of == Outfit::LongDress;
            V3 pts[2];
            for (int i = 0; i < 2; ++i)
                pts[i] = longDress ? ankles[i] : of == Outfit::Skirt ? w0 + (knees[i] - w0) * 0.8f : knees[i];
            const V3 mid = (pts[0] + pts[1]) * 0.5f;
            const V3 axis = normalize(mid - w0, -pelvis.y);
            const float len = std::max(0.14f, dot(mid - w0, axis) + (longDress ? -0.01f : 0.04f));
            const float r0 = 0.118f * build * (1 + 0.2f * belly);
            float r1 = (longDress ? 0.24f : 0.2f) * build;
            const float margin = (longDress ? 0.045f : of == Outfit::Skirt ? 0.07f : 0.062f) * limb;
            for (const V3& k : pts) {
                const V3 d = k - w0;
                const float along = std::max(0.05f, dot(d, axis));
                const float perp = length(d - axis * along) + margin;  // the leg's radius + a margin
                // The cone's radius at the leg (r0 + (r1 - r0) * along / len) must cover it.
                r1 = std::max(r1, r0 + (perp - r0) * len / std::min(along, len));
            }
            r1 = std::min(r1, (longDress ? 0.4f : 0.34f) * build);
            const bool bottomSkirt = of == Outfit::Skirt;
            BodyPart& sk2 = B.segment(w0, w0 + axis * len, pelvis.x, r0, r1, bottomSkirt ? look.bottom : look.top);
            sk2.material = Material::Hair;
            B.clip(sk2, {0, 1, 0}, len);
            dress(sk2, bottomSkirt ? Bottom : Top, bottomSkirt ? Bottom : Top);
        }
        if (look.outfit == Outfit::Suit) {
            BodyPart& tie = B.segment(chest.point({0, 0.0f, 0.098f}), chest.point({0, -0.13f, 0.104f}), chest.x,
                                      0.016f, 0.028f, look.accent);
            tie.scaleZ = 0.35f;
            tie.outline = false;
        }
        if (look.outfit == Outfit::Hoodie) {
            dress(B.ball(chest.translated({0, 0.07f, -0.085f}), 0.085f, 1.3f, 0.8f, look.top), Top, Top);
        }
    }

    // ---- arms and hands
    // Arms never pass through the head: where a pose puts the forearm, the hand or the upper arm inside the head
    // ball, the whole arm (its bend kept) swings about the shoulder, away from the head, by the smallest angle that
    // clears it. That angle changes smoothly with the pose, so a move sliding past the head bends around it.
    const V3 headC = head.o;
    out.armInHeadPosed = out.armInHead = 0;
    auto closestOn = [](V3 p, V3 a, V3 b) {
        const V3 ab = b - a;
        const float t = clampf(dot(p - a, ab) / std::max(dot(ab, ab), 1e-8f), 0.0f, 1.0f);
        return a + ab * t;
    };
    auto rotateAbout = [](V3 v, V3 axis, float ang) {  // Rodrigues, axis normalized
        const float c = std::cos(ang), s = std::sin(ang);
        return v * c + cross(axis, v) * s + axis * (dot(axis, v) * (1 - c));
    };
    for (int side = 0; side < 2; ++side) {
        const float sx = side == 0 ? 1.0f : -1.0f;
        const V3 shoulder = chest.point({sx * sk.shoulderHalf, sk.shoulderY, 0});
        const V3 target = chest.point(side == 0 ? pose.handL : pose.handR);
        const V3 pole = hintedPole(chest.dir({sx * 0.8f, -0.45f, -0.6f}), chest, side == 0 ? pose.elbowL : pose.elbowR);
        IkResult ik = solveTwoBone(shoulder, target, sk.upperArm, sk.forearm, pole);
        // The head ball is 1.05 R wide, 0.97 R deep: 1.02 R plus the arm's own radius. The upper arm counts from a
        // third of the way out (its root is the shoulder, always beside the head).
        const float keepFore = R * 1.02f + 0.041f * limb, keepHand = R * 1.02f + 0.045f;
        const float keepUpper = R * 1.02f + 0.047f * limb;
        auto depth = [&](V3 mid, V3 end) {
            const V3 handC = end + normalize(end - mid) * 0.032f;
            float d = keepHand - length(handC - headC);
            d = std::max(d, keepFore - length(closestOn(headC, mid, handC) - headC));
            d = std::max(d, keepUpper - length(closestOn(headC, shoulder + (mid - shoulder) * 0.33f, mid) - headC));
            return d;
        };
        out.handsPosed[side] = ik.end;
        const float posed = depth(ik.mid, ik.end);
        out.armInHeadPosed = std::max(out.armInHeadPosed, std::max(0.0f, posed));
        if (posed > 0) {
            // Swing away from the head: the arm's middle moves away from the head's center (across the arm), with a
            // gentle preference for out to its own side and forward, so the direction stays steady where "away" is
            // ambiguous (an arm reaching straight over the head). The smallest clearing angle by bisection (up to
            // 100 degrees).
            const V3 v = (ik.mid + ik.end) * 0.5f - shoulder;
            const V3 vn = normalize(v, chest.y);
            V3 away = (shoulder + v) - headC;
            away = away - vn * dot(away, vn);
            const V3 pref = normalize(chest.x * sx + chest.z * 0.6f);
            V3 esc = away + (pref - vn * dot(pref, vn)) * 0.08f;
            V3 axis = cross(vn, esc);
            axis = length(axis) > 1e-6f ? normalize(axis) : normalize(cross(vn, chest.z), chest.x);
            auto swung = [&](float ang, V3* mid, V3* end) {
                *mid = shoulder + rotateAbout(ik.mid - shoulder, axis, ang);
                *end = shoulder + rotateAbout(ik.end - shoulder, axis, ang);
            };
            // The first clearing angle: a coarse scan outwards (the overlap need not shrink steadily), then
            // bisection within that step.
            V3 m, e;
            float lo = 0, hi = 1.75f;
            for (int i = 1; i <= 25; ++i) {
                const float ang = 1.75f * float(i) / 25.0f;
                swung(ang, &m, &e);
                if (depth(m, e) <= 0) {
                    hi = ang;
                    break;
                }
                lo = ang;
            }
            for (int i = 0; i < 10; ++i) {
                const float mid = 0.5f * (lo + hi);
                swung(mid, &m, &e);
                (depth(m, e) > 0 ? lo : hi) = mid;
            }
            swung(hi, &ik.mid, &ik.end);
        }
        out.armInHead = std::max(out.armInHead, std::max(0.0f, depth(ik.mid, ik.end)));
        out.hands[side] = ik.end;
        Rgb upper0 = look.top, upper1 = look.skin, lower = look.skin;
        Region u0 = Top, u1 = Skin, lo = Skin;
        float split = 0.55f * sk.upperArm;
        switch (look.outfit) {
            case Outfit::Tank:
            case Outfit::LongDress: upper0 = look.skin; u0 = Skin; split = 1e9f; break;
            case Outfit::Hoodie:
            case Outfit::Suit: upper1 = look.top; lower = look.top; u1 = Top; lo = Top; split = 1e9f; break;
            default: break;
        }
        dress(B.segment(shoulder, ik.mid, chest.x, 0.047f * limb, 0.041f * limb, upper0, upper1, split), u0, u1);
        BodyPart& fore = B.segment(ik.mid, ik.end, chest.x, 0.041f * limb, 0.036f * limb, lower);
        dress(fore, lo, Skin);
        if (lower.r != look.skin.r || lower.g != look.skin.g || lower.b != look.skin.b) {
            fore.color1 = look.skin;  // cuff -> wrist
            fore.split = 0.92f * sk.forearm * scale;
        }
        const V3 along = normalize(ik.end - ik.mid);
        const float hr = 0.05f * (1.0f - 0.12f * clampf(pose.fist, 0, 1));
        const Frame hand = frameAlong(ik.end + along * 0.032f, along, chest.x);
        B.ball(hand, hr, 0.85f, 1.0f, look.skin);
        if (look.glowSticks) {
            // Held in the fist, sticking out past the thumb side and the little finger: a short neon tube.
            const V3 across = normalize(cross(along, chest.z), chest.x);
            const V3 c = ik.end + along * 0.04f;
            BodyPart& stick = B.segment(c - across * 0.07f, c + across * 0.11f, along, 0.017f, 0.017f, look.glow);
            stick.fabric[0] = stick.fabric[1] = std::uint8_t(Fabric::Neon);
            stick.outline = false;
        }
    }

    // ---- head
    const std::size_t headBegin = out.parts.size();  // head, face, glasses, hair and hat from here on
    {
        BodyPart& h = B.ball(head, R, 1.05f, 0.97f, look.skin);
        h.material = Material::Head;
        out.headCenter = B.world(head.o);
        for (int side = 0; side < 2; ++side) {
            const float sx = side == 0 ? 1.0f : -1.0f;
            B.ball(head.translated({sx * R * 1.0f, -0.02f * hs, -0.005f * hs}), 0.034f * hs, 0.6f, 1.0f, look.skin);
        }
        BodyPart& nose = B.ball(head.translated({0, -0.03f * hs, R * 0.96f}), 0.022f * hs, 1.1f, 0.9f,
                                scaled(look.skin, 0.97f));
        nose.outline = false;
    }

    // ---- glasses: two shaped flat lenses (or one visor) in front of the eyes, a bridge and arms to the ears
    if (look.glasses != Glasses::None) {
        const float eyeU = 0.34f, eyeV = 0.06f;
        auto onFace = [&](float u, float v, float lift) {  // a point just in front of the head surface
            return V3{1.05f * R * std::sin(u) * std::cos(v), R * std::sin(v), 0.97f * R * std::cos(u) * std::cos(v) + lift};
        };
        auto lens = [&](V3 at, float yaw, float r, float sx, float sy, int sideFlag) {
            const Frame f = head.translated(at).rotatedY(yaw);
            BodyPart& l = B.ball(f, r, sx, 0.22f, look.lens);
            l.scaleZ = 0.22f;
            l.scaleX = sx;
            l.material = Material::Lens;
            l.color1 = look.frame;
            l.pattern[0] = std::uint8_t(look.glasses);
            l.pattern[1] = std::uint8_t(sideFlag);
            // Lens kind for the shader: 0 dark glass, 1 mirrored, 2 clear (just the frame, the eyes show through).
            l.fabric[0] = look.clearLens ? 2 : look.mirrored ? 1 : 0;
            l.split = sy;  // vertical squash of the shape (shader)
            l.outline = false;
            return at;
        };
        const float lift = 0.022f * hs;
        V3 outer[2];
        if (look.glasses == Glasses::Visor) {
            lens(onFace(0, eyeV, lift + 0.012f * hs), 0, 0.075f * hs, 2.35f, 0.62f, 0);
            outer[0] = onFace(0.62f, eyeV, lift * 0.5f);
            outer[1] = onFace(-0.62f, eyeV, lift * 0.5f);
        } else {
            float r = 0.068f, sy = 1.0f, sxs = 1.0f;
            switch (look.glasses) {
                case Glasses::Aviator: r = 0.074f; sy = 0.9f; break;
                case Glasses::Heart: r = 0.078f; break;
                case Glasses::Star: r = 0.088f; break;
                case Glasses::CatEye: r = 0.072f; sy = 0.75f; sxs = 1.15f; break;
                case Glasses::Square: r = 0.062f; sy = 0.85f; sxs = 1.1f; break;
                default: break;
            }
            for (int side = 0; side < 2; ++side) {
                const float sx = side == 0 ? 1.0f : -1.0f;
                lens(onFace(sx * eyeU, eyeV, lift), sx * eyeU * 0.85f, r * hs, sxs, sy, side);
                outer[side] = onFace(sx * (eyeU + 0.25f), eyeV, lift * 0.5f);
            }
            BodyPart& bridge = B.segment(head.point(onFace(0.12f, eyeV + 0.03f, lift)),
                                         head.point(onFace(-0.12f, eyeV + 0.03f, lift)), head.y, 0.008f * hs,
                                         0.008f * hs, look.frame);
            bridge.outline = false;
        }
        for (int side = 0; side < 2; ++side) {
            const float sx = side == 0 ? 1.0f : -1.0f;
            BodyPart& arm = B.segment(head.point(outer[side]), head.point({sx * R * 1.03f, R * 0.06f, -R * 0.25f}),
                                      head.y, 0.008f * hs, 0.008f * hs, look.frame);
            arm.outline = false;
        }
    }

    // ---- hair
    const std::size_t hairBegin = out.parts.size();
    // Close-fitting styles: shells a little larger than the head, cut open at the face (a smooth hairline); their
    // inside, seen through the cut, is shaded as solid hair (DancerPipeline body_fs). Every hair part is drawn with
    // strands (Material::Strands, set at the end): `strands` says which way they run.
    const Rgb hairC = look.hair;
    auto shell = [&](float k, V3 n, float w, V3 off = {0, 0.012f, -0.012f}) -> BodyPart& {
        BodyPart& s = B.ball(head.translated(off * hs), R * k, 1.05f, 0.98f, hairC);
        s.material = Material::Hair;
        B.clip(s, n, w * R * k);
        return s;
    };
    // A plane cut given in the head's frame: cut away where dot(p - head center, n) > w R (`second`: as clip2, so a
    // part can have two). Works for any part (its own frame and scale).
    auto cut = [&](BodyPart& p, V3 n, float w, bool second = false) {
        n = normalize(n);
        const V3 dW = B.place.dir(head.dir(n));
        const V3 pW = B.world(head.point(n * (w * R)));
        const std::array<float, 4> c{p.scaleX * dot(dW, p.frame.x), dot(dW, p.frame.y), p.scaleZ * dot(dW, p.frame.z),
                                     dot(dW, pW - p.frame.o)};
        (second ? p.clip2 : p.clip) = c;
    };
    // A layer of hair: a ball like the head's (center `off`, radius k, in head radii) that stands off the head where
    // it is off-center (a lift at the front, volume on top) and sinks into the hair under it elsewhere. Cut at the
    // usual hairline (`hairline`: its offset).
    auto layer = [&](V3 off, float k, float hairline) -> BodyPart& {
        BodyPart& s = B.ball(head.translated(off * R), R * k, 1.05f, 0.98f, hairC);
        s.material = Material::Hair;
        cut(s, {0, -0.62f, 0.78f}, hairline);
        return s;
    };
    // A solid mass (never cut open) set up and back so the face pokes out in front of it: where they meet is the
    // hairline (the afro). A mass must cross the head steeply (well off-center) or the hairline steps.
    auto mass = [&](V3 c, float r, float sx = 1.0f, float sz = 1.0f) -> BodyPart& {
        BodyPart& m = B.ball(head.translated(c * R), R * r, sx, sz, hairC);
        m.material = Material::Hair;
        return m;
    };
    auto hp = [&](V3 p) { return head.point(p * hs); };  // a point near the head, in standard-head units
    // Hanging hair in motion: the far end of a piece from `a` to `b` swung by `swing` (head frame, per unit length)
    // times `amount`, its length kept. Swings into the head or the neck are held back (`inward`: the head-frame
    // direction that points into the body from this piece; it may move that way at most a little).
    auto swung = [&](V3 a, V3 b, V3 swing, float amount, V3 inward = {0, 0, 1}) {
        const float L = length(b - a);
        if (L < 1e-6f) return b;
        V3 sw = swing * amount;
        const float in = dot(sw, inward);
        if (in > 0.12f) sw = sw - inward * (in - 0.12f);
        return a + normalize((b - a) * (1.0f / L) + head.dir(sw)) * L;
    };
    const V3 swingL = pose.hairSwing[0], swingR = pose.hairSwing[1];
    const V3 swingMid = (swingL + swingR) * 0.5f;
    // A direction from the head center: u around (+ towards the character's left), v up from the eye line.
    auto sphereDir = [](float u, float v) {
        return V3{std::sin(u) * std::cos(v), std::sin(v), std::cos(u) * std::cos(v)};
    };
    auto mixRgb = [](Rgb a, Rgb b, float t) {
        return Rgb{a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
    };
    auto hash = [](int i) { return std::fabs(std::sin(float(i) * 12.9898f) * 43758.5453f) - std::floor(std::fabs(std::sin(float(i) * 12.9898f) * 43758.5453f)); };
    // Long hair (2026-11-21): close on top (a shell cut at the hairline, the strands parted at `part`, x R; > 5: no
    // parting) over a curtain hanging from under it: a tube round the head, open in front of the cheeks (its inside
    // shows as the hair behind the neck), cut level at the hem `drop` head radii below the head's center, swinging
    // with the dance. Straight sides past the face, never a ball round it. Returns where the hem's middle moved to
    // (swing) from where it hangs at rest, for pieces that hang on the curtain.
    struct Curtain {
        BodyPart* top;
        BodyPart* tube;
        V3 hemShift;  // character space
    };
    auto curtain = [&](float drop, float part, float spread = 1.0f, float frame = 0.0f) -> Curtain {
        BodyPart& top = shell(1.065f, {0, -0.62f, 0.78f}, 0.06f);
        if (part < 5) {
            top.strands = kStrandsParted;
            top.strandPart = part / 1.065f;
        }
        const std::size_t topIndex = out.parts.size() - 1;
        const float len = drop + 0.5f;
        const V3 a = head.point(V3{0, -0.04f, -0.17f} * R);
        const V3 rest = head.point(V3{0, -0.04f - len, -0.17f - 0.12f * len} * R);
        const V3 b = swung(a, rest, swingMid, 0.35f);
        BodyPart& t = B.segment(a, b, head.x, R * 0.97f, R * (0.99f + 0.08f * spread), hairC);
        t.scaleX = 1.24f;
        t.scaleZ = 0.82f;
        t.material = Material::Hair;
        // Open in front of the cheeks; `frame` slants the opening forward lower down (layers framing the face).
        cut(t, {0, frame, 1}, 0.22f);
        t.clip2 = {0, 1, 0, (drop - 0.04f) * R * scale};           // the hem (along the tube, which hangs down)
        return Curtain{&out.parts[topIndex], &out.parts.back(), (b - rest) * (1.0f / std::max(len * R, 1e-6f))};
    };
    // Pieces hanging on a curtain (curls, waves) follow its swing: t of the way down moves t of the hem's shift.
    auto onCurtain = [&](V3 restHead, float t, float drop, const Curtain& c) {
        return head.point(restHead * R) + c.hemShift * (t * (drop + 0.5f) * R);
    };
    // Under a cap or a beanie, big hair is built to hang out below the hat instead (a full mass cut at the band would
    // either stick out past the hat or vanish inside the head); fitHairUnderHat then trims whatever still shows
    // above the band. The top hat sits high enough for the usual shapes.
    const bool capped = look.hat == Hat::Cap || look.hat == Hat::Beanie;
    auto hatLocks = [&](float length, float width) {  // side locks from under the band, and the back
        shell(1.03f, {0, -0.62f, 0.78f}, 0.06f);
        for (int side = 0; side < 2; ++side) {
            const float sx = side == 0 ? 1.0f : -1.0f;
            const V3 a = head.point(V3{sx * 0.98f, 0.02f, -0.12f} * R);
            const V3 b = swung(a, head.point(V3{sx * 1.0f, -0.25f - length, -0.18f} * R), side == 0 ? swingL : swingR, 0.6f,
                               {-sx, 0, 0});
            BodyPart& l = B.segment(a, b, head.z, R * 0.3f * width, R * 0.24f * width, hairC);
            l.scaleX = 0.6f;
            l.material = Material::Hair;
        }
        const V3 a = head.point(V3{0, 0.0f, -0.62f} * R);
        BodyPart& back = B.segment(a, swung(a, head.point(V3{0, -0.3f - length, -0.75f} * R), swingMid, 0.5f), head.x,
                                   R * 0.78f * width, R * 0.62f * width, hairC);
        back.scaleZ = 0.5f;
        back.material = Material::Hair;
    };
    auto hatPuffs = [&](float r) {  // afro / curls showing below the band at the sides and back
        shell(1.03f, {0, -0.62f, 0.78f}, 0.06f).strands = kStrandsCoils;
        const int n = 11;
        for (int i = 0; i < n; ++i) {
            const float u = 1.15f + (6.2831853f - 2.3f) * float(i) / float(n - 1);  // round the back, ear to ear
            for (int row = 0; row < 2; ++row) {
                const V3 d = sphereDir(u, row == 0 ? -0.02f : -0.42f);
                BodyPart& b = B.ball(head.translated(V3{d.x * 1.05f, d.y, d.z * 0.98f} * (R * 0.98f)),
                                     R * r * (row == 0 ? 1.0f : 0.9f), 1, 1, hairC);
                b.material = Material::Hair;
                b.strands = kStrandsCoils;
            }
        }
    };
    // A tail (ponytail, pigtail, side pony) from `a` towards `b` (standard-head units), swinging, tied with the
    // accent color.
    auto tail = [&](V3 a0, V3 b0, V3 swing, float amount, V3 inward, float r0, float r1) {
        const V3 a = hp(a0), b = swung(a, hp(b0), swing, amount, inward);
        BodyPart& t = B.segment(a, b, head.x, r0 * hs, r1 * hs, hairC);
        t.material = Material::Hair;
        B.ball(Frame{}.translated(a), 0.03f * hs, 1, 1, look.accent);
    };
    HairStyle style = look.hairStyle;
    if (capped) {
        switch (style) {
            case HairStyle::Bob: hatLocks(0.55f, 1.0f); style = HairStyle::Bald; break;
            case HairStyle::Long:
            case HairStyle::Bangs:
            case HairStyle::Layers:
            case HairStyle::HalfUp: hatLocks(1.45f, 1.0f); style = HairStyle::Bald; break;
            case HairStyle::Mullet: break;
            case HairStyle::Afro: hatPuffs(0.42f); style = HairStyle::Bald; break;
            case HairStyle::Curly: hatPuffs(0.3f); style = HairStyle::Bald; break;
            case HairStyle::SpaceBuns:
            case HairStyle::TopKnot:
            case HairStyle::ManBun:
            case HairStyle::SlickBack:
            case HairStyle::Curtains:
            case HairStyle::Quiff:
            case HairStyle::Anime:
            case HairStyle::Coils:
            case HairStyle::SidePart: style = HairStyle::Short; break;  // under the hat
            case HairStyle::Pigtails:
                shell(1.03f, {0, -0.62f, 0.78f}, 0.06f);
                for (int side = 0; side < 2; ++side) {
                    const float sx = side == 0 ? 1.0f : -1.0f;
                    tail({sx * 0.165f, -0.03f, -0.08f}, {sx * 0.2f, -0.3f, -0.03f}, side == 0 ? swingL : swingR, 1.0f,
                         {-sx, 0, 0}, 0.05f, 0.03f);
                }
                style = HairStyle::Bald;
                break;
            default: break;
        }
    }
    switch (style) {
        case HairStyle::Short: shell(1.06f, {0, -0.62f, 0.78f}, 0.06f); break;
        case HairStyle::Bob: curtain(1.05f, 0.32f); break;
        case HairStyle::Long: curtain(2.2f, 0.0f); break;
        case HairStyle::Spiky: {
            shell(1.05f, {0, -0.62f, 0.78f}, 0.06f);
            const V3 dirs[] = {{0, 1, 0.25f}, {0.5f, 0.85f, 0.1f}, {-0.5f, 0.85f, 0.1f}, {0.25f, 0.8f, 0.55f},
                               {-0.25f, 0.8f, 0.55f}, {0.3f, 0.7f, -0.6f}, {-0.3f, 0.7f, -0.6f}, {0, 0.75f, -0.66f}};
            for (V3 d : dirs) {
                d = normalize(d);
                BodyPart& c = B.segment(head.point(d * (R * 0.92f)), head.point(d * (R * 1.48f)), head.x, 0.06f * hs,
                                        0.008f * hs, hairC);
                c.outline = true;
            }
            break;
        }
        case HairStyle::Afro: {
            // A big mass with a few lobes, so the outline is not a perfect ball.
            mass({0, 0.45f, -0.6f}, 1.35f, 1.1f).strands = kStrandsCoils;
            mass({0.62f, 0.75f, -0.55f}, 0.85f, 1.0f).strands = kStrandsCoils;
            mass({-0.62f, 0.75f, -0.55f}, 0.85f, 1.0f).strands = kStrandsCoils;
            mass({0, 1.05f, -0.45f}, 0.9f, 1.1f).strands = kStrandsCoils;
            break;
        }
        case HairStyle::Ponytail: {
            // A high ponytail: combed back to a tie at the crown, a full tail hanging from it.
            shell(1.06f, {0, -0.62f, 0.78f}, 0.06f).strands = kStrandsBack;
            B.ball(head.translated(V3{0, 0.13f, -0.14f} * hs), 0.045f * hs, 1.1f, 1.0f, hairC).material = Material::Hair;
            tail({0, 0.14f, -0.18f}, {0, -0.17f, -0.3f}, swingMid, 1.0f, {0, 0, 1}, 0.06f, 0.028f);
            break;
        }
        case HairStyle::Bun:
            shell(1.06f, {0, -0.62f, 0.78f}, 0.06f);
            B.ball(head.translated(V3{0, 0.19f, -0.07f} * hs), 0.075f * hs, 1, 1, hairC);
            break;
        case HairStyle::Mohawk:
            for (int i = 0; i < 5; ++i) {
                const float a = -0.35f + 0.38f * float(i);  // from the forehead over the top to the back
                const V3 d{0, std::cos(a), std::sin(-a) + 0.15f};
                const V3 base = head.point(normalize(d) * (R * 0.95f));
                const V3 tip = head.point(normalize(d) * (R * 1.42f));
                BodyPart& c = B.segment(base, tip, head.x, 0.05f * hs, 0.01f * hs, hairC);
                c.scaleX = 0.5f;
            }
            break;
        case HairStyle::Pigtails: {
            // Parted in the middle, two low tails from behind the ears hanging forward over the shoulders.
            BodyPart& s = shell(1.06f, {0, -0.62f, 0.78f}, 0.06f);
            s.strands = kStrandsParted;
            s.strandPart = 0;
            for (int side = 0; side < 2; ++side) {
                const float sx = side == 0 ? 1.0f : -1.0f;
                tail({sx * 0.17f, -0.03f, -0.08f}, {sx * 0.2f, -0.31f, -0.02f}, side == 0 ? swingL : swingR, 1.0f,
                     {-sx, 0, 0}, 0.055f, 0.03f);
            }
            break;
        }
        case HairStyle::Curly: {
            // Ringlets: a close shell under a cluster of small balls over the top, the back and the sides (a lumpy
            // outline, looser than the afro), leaving the face and the forehead clear.
            shell(1.03f, {0, -0.62f, 0.78f}, 0.1f).strands = kStrandsCoils;
            const V3 face = normalize(V3{0, -0.5f, 0.86f});
            const int n = 44;
            for (int i = 0; i < n; ++i) {
                const float y = 1.0f - 2.0f * (float(i) + 0.5f) / float(n), r = std::sqrt(1.0f - y * y);
                const float a = 2.39996323f * float(i);
                const V3 d{r * std::sin(a), y, r * std::cos(a)};
                if (d.y < -0.35f || (d.y < 0.05f && d.z > -0.1f) || dot(d, face) > -0.12f) continue;
                const float jig = 0.85f + 0.3f * std::fabs(std::sin(float(i) * 12.9898f) * 0.5f + 0.5f - 0.5f);
                BodyPart& c = B.ball(head.translated(V3{d.x * 1.05f, d.y, d.z * 0.98f} * (R * 0.96f)), R * 0.3f * jig,
                                     1.0f, 1.0f, hairC);
                c.material = Material::Hair;
                c.strands = kStrandsCoils;
            }
            break;
        }
        case HairStyle::Pompadour: {
            // Short, combed-back sides; the top swept up and back from the forehead in a big wave (a layer well up
            // and forward of the head, cut off below the temples).
            shell(1.04f, {0, -0.62f, 0.78f}, 0.08f).strands = kStrandsBack;
            BodyPart& wave = layer({0, 0.48f, 0.18f}, 0.82f, 0.08f);
            wave.strands = kStrandsBack;
            cut(wave, {0, -1, 0.3f}, -0.28f);  // rising from the top of the forehead
            cut(wave, {0, -1, 0}, -0.3f, true);
            break;
        }
        case HairStyle::Quiff: {
            // Short sides, a little lift at the front pushed up and back.
            shell(1.04f, {0, -0.62f, 0.78f}, 0.07f).strands = kStrandsBack;
            BodyPart& lift = layer({0, 0.2f, 0.14f}, 0.93f, 0.07f);
            lift.strands = kStrandsBack;
            cut(lift, {0, -1, 0.3f}, -0.36f);
            cut(lift, {0, -1, 0}, -0.2f, true);
            break;
        }
        case HairStyle::SidePart: {
            // A classic short side part: the parting high over one eye, the hair combed over from it with a little
            // lift at the front (a layer cut along the parting).
            BodyPart& s = shell(1.045f, {0, -0.62f, 0.78f}, 0.07f);
            s.strands = kStrandsParted;
            s.strandPart = 0.42f / 1.045f;
            BodyPart& lift = layer({-0.1f, 0.14f, 0.13f}, 0.97f, 0.07f);
            lift.strands = kStrandsParted;
            cut(lift, {0.25f, -1, 0.3f}, -0.4f);  // the front edge, lower on the side it is combed to
            cut(lift, {1, 0, 0}, 0.42f, true);
            break;
        }
        case HairStyle::Undercut: {
            // Clipped sides and back (a fade: the hair color over the skin) under a long top combed back.
            BodyPart& sides = shell(1.02f, {0, -0.62f, 0.78f}, 0.0f);
            sides.color0 = mixRgb(look.skin, hairC, 0.32f);
            BodyPart& top = layer({0, 0.22f, 0.08f}, 0.98f, 0.05f);  // thick on top, sunk into the head at the sides
            top.strands = kStrandsBack;
            cut(top, {0, -1, 0.3f}, -0.42f);
            break;
        }
        case HairStyle::Bowl: {
            // Cut level all round just above the brows.
            shell(1.09f, {0, -1, 0.12f}, -0.36f);
            break;
        }
        case HairStyle::Mullet: {
            // Business in front, party at the back: short on top, long and full down the back of the neck.
            shell(1.05f, {0, -0.62f, 0.78f}, 0.06f);
            const V3 a = head.point(V3{0, -0.05f, -0.5f} * R);
            BodyPart& back = B.segment(a, swung(a, head.point(V3{0, -1.45f, -0.72f} * R), swingMid, 0.6f),
                                       head.x, R * 0.66f, R * 0.5f, hairC);
            back.scaleZ = 0.5f;
            back.material = Material::Hair;
            for (int side = 0; side < 2; ++side) {
                const float sx = side == 0 ? 1.0f : -1.0f;
                const V3 a = head.point(V3{sx * 0.55f, -0.2f, -0.55f} * R);
                const V3 b = swung(a, head.point(V3{sx * 0.72f, -1.15f, -0.62f} * R), side == 0 ? swingL : swingR,
                                   0.9f, {-sx, 0, 0.5f});
                BodyPart& t = B.segment(a, b, head.z, R * 0.32f, R * 0.08f, hairC);
                t.material = Material::Hair;
            }
            break;
        }
        case HairStyle::Shag: {
            // Choppy layers to the shoulders, flaring out at the ends, and a soft fringe.
            curtain(1.3f, 9.0f, 1.8f);
            BodyPart& fringe = layer({0, 0.1f, 0.12f}, 0.97f, 0.3f);
            cut(fringe, {0, 0, -1}, 0.05f, true);
            break;
        }
        case HairStyle::Pixie: {
            // Cropped close all round, with a soft fringe swept to one side over the forehead (a layer at the front
            // cut on a slant).
            shell(1.05f, {0, -0.62f, 0.78f}, 0.1f);
            BodyPart& fringe = layer({0.08f, 0.08f, 0.12f}, 0.97f, 0.1f);
            cut(fringe, {0.33f, -1, 0}, -0.5f);  // lower on the right (-x), up to the left temple
            cut(fringe, {0, 0, -1}, 0.1f, true);
            break;
        }
        case HairStyle::Bangs: {
            // Long and straight with a blunt fringe just above the brows.
            curtain(2.1f, 9.0f);
            shell(1.085f, {0, -1, 0.12f}, -0.36f);
            break;
        }
        case HairStyle::Braid: {
            // A side braid: gathered behind one ear and plaited, hanging forward over the shoulder, tied at the end.
            shell(1.06f, {0, -0.62f, 0.78f}, 0.06f).strands = kStrandsBack;
            const V3 a = hp({0.16f, -0.02f, -0.08f}), b = swung(a, hp({0.2f, -0.36f, 0.0f}), swingL, 1.0f, {-1, 0, 0});
            const int n = 8;
            for (int i = 0; i < n; ++i) {
                const float t = float(i) / float(n - 1);
                const V3 side = head.dir({0, 0, (i % 2 ? 0.012f : -0.012f) * hs});
                BodyPart& k = B.ball(frameAlong(a + (b - a) * t + side, b - a, head.x).rotatedZ(i % 2 ? 0.5f : -0.5f),
                                     (0.048f - 0.016f * t) * hs, 1.0f, 0.8f, hairC);
                k.material = Material::Hair;
            }
            B.ball(Frame{}.translated(b + normalize(b - a) * 0.02f * hs), 0.022f * hs, 1, 1, look.accent);
            break;
        }
        case HairStyle::SidePony: {
            // Gathered at one side and hanging forward over the shoulder.
            shell(1.06f, {0, -0.62f, 0.78f}, 0.06f).strands = kStrandsBack;
            tail({0.155f, 0.03f, -0.07f}, {0.22f, -0.25f, 0.01f}, swingL, 1.0f, {-1, 0, 0}, 0.055f, 0.028f);
            break;
        }
        case HairStyle::SpaceBuns: {
            BodyPart& s = shell(1.06f, {0, -0.62f, 0.78f}, 0.06f);
            s.strands = kStrandsParted;
            s.strandPart = 0;
            for (int side = 0; side < 2; ++side) {
                const float sx = side == 0 ? 1.0f : -1.0f;
                B.ball(head.translated(V3{sx * 0.115f, 0.165f, -0.03f} * hs), 0.062f * hs, 1, 1, hairC).material =
                    Material::Hair;
            }
            break;
        }
        case HairStyle::TopKnot:
            shell(1.06f, {0, -0.62f, 0.78f}, 0.06f);
            B.ball(head.translated(V3{0, 0.235f, -0.02f} * hs), 0.085f * hs, 1.05f, 1.0f, hairC).material = Material::Hair;
            B.ball(head.translated(V3{0, 0.17f, -0.02f} * hs), 0.04f * hs, 1.3f, 1.3f, look.accent);
            break;
        case HairStyle::Shaved: {
            // Shaved to a shadow: barely darker than the skin, closer than a buzz cut.
            BodyPart& sh = shell(1.012f, {0, -0.62f, 0.78f}, 0.08f);
            sh.color0 = sh.color1 = mixRgb(look.skin, hairC, 0.28f);
            sh.strands = kStrandsNone;  // a smooth shadow (lines or a texture read as a cap)
            break;
        }
        case HairStyle::Buzz: {
            // Clipped very short all over: the hair color over the skin.
            BodyPart& s = shell(1.025f, {0, -0.62f, 0.78f}, 0.08f);
            s.color0 = s.color1 = mixRgb(look.skin, hairC, 0.6f);
            break;
        }
        case HairStyle::SlickBack: {
            // Combed straight back from the forehead, smooth and full on top.
            shell(1.05f, {0, -0.62f, 0.78f}, 0.1f).strands = kStrandsBack;
            BodyPart& top = layer({0, 0.12f, -0.04f}, 1.0f, 0.1f);
            top.strands = kStrandsBack;
            cut(top, {0, -1, 0}, -0.2f, true);
            break;
        }
        case HairStyle::Curtains: {
            // A middle part: two layers, each swept down to its own temple, so the hairline is high at the part and
            // low at both sides; the strands run out from the parting.
            for (int side = 0; side < 2; ++side) {
                const float sx = side == 0 ? 1.0f : -1.0f;
                BodyPart& s = shell(1.06f, {sx * 0.5f, -0.58f, 0.64f}, 0.24f, {0, 0.006f, -0.004f});
                s.strands = kStrandsParted;
                s.strandPart = 0;
                cut(s, {0, -1, -0.3f}, 0.25f, true);  // short at the sides, above the ears
            }
            break;
        }
        case HairStyle::Balding: {
            // Hair only round the sides and the back, the crown bare: a close band cut at the hairline and above.
            BodyPart& band = shell(1.07f, {0, -0.62f, 0.78f}, 0.2f);
            cut(band, {0, 1, 0.5f}, 0.3f, true);
            break;
        }
        case HairStyle::ManBun:
            shell(1.06f, {0, -0.62f, 0.78f}, 0.06f).strands = kStrandsBack;
            B.ball(head.translated(V3{0, 0.17f, -0.12f} * hs), 0.07f * hs, 1.1f, 1, hairC).material = Material::Hair;
            break;
        case HairStyle::Coils: {
            // Short coils / twists close over the top, faded at the sides.
            BodyPart& sides = shell(1.03f, {0, -0.62f, 0.78f}, 0.06f);
            sides.color0 = sides.color1 = mixRgb(look.skin, hairC, 0.8f);
            sides.strands = kStrandsCoils;
            const int n = 70;
            for (int i = 0; i < n; ++i) {
                const float y = 1.0f - 2.0f * (float(i) + 0.5f) / float(n), r = std::sqrt(1.0f - y * y);
                const float a = 2.39996323f * float(i);
                const V3 d{r * std::sin(a), y, r * std::cos(a)};
                if (d.y < 0.3f - 0.2f * std::max(0.0f, -d.z) || dot(d, normalize(V3{0, -0.5f, 0.86f})) > -0.2f) continue;
                BodyPart& c = B.ball(head.translated(V3{d.x * 1.05f, d.y, d.z * 0.98f} * (R * 1.0f)), R * 0.16f, 1, 1, hairC);
                c.material = Material::Hair;
                c.strands = kStrandsCoils;
            }
            break;
        }
        case HairStyle::Waves: {
            // 360 waves: brushed close, ripples in rings round the crown.
            shell(1.035f, {0, -0.62f, 0.78f}, 0.07f).strands = kStrandsWaveCut;
            break;
        }
        case HairStyle::Layers: {
            // Long layers: shorter round the face, longer at the back (the hem slants up to the front).
            curtain(2.0f, 0.0f, 1.2f).tube->clip2 = {0, 0, 0, 1e9f};
            cut(out.parts.back(), {0, -1, 0.75f}, 1.55f, true);
            break;
        }
        case HairStyle::HalfUp: {
            // Long, the top half combed back to a little bun at the crown.
            const Curtain c = curtain(2.1f, 9.0f);
            c.top->strands = kStrandsBack;
            B.ball(head.translated(V3{0, 0.82f, -0.62f} * R), R * 0.3f, 1.1f, 1.0f, hairC).material = Material::Hair;
            break;
        }
        case HairStyle::Anime: {
            // Anime hair (2026-11-21, in place of messy): big pointed clumps over a full cap: a fringe of locks over
            // the forehead, locks framing the face to the jaw, flicks sticking out round the back, and a cowlick.
            // Each lock is bent once (two tapering pieces, flat against the head), from inside the hair to its tip.
            shell(1.08f, {0, -0.62f, 0.78f}, 0.1f);
            // A point at (u around, v up, k head radii out); `swing` moves the tip, `amount` of it.
            auto at = [&](float u, float v, float k) { return head.point(sphereDir(u, v) * (R * k)); };
            auto lock = [&](V3 a, V3 m, V3 t, float w, V3 swing, float amount, V3 inward) {
                t = swung(m, t, swing, amount, inward);
                const V3 pieces[2][2] = {{a, m}, {m, t}};
                for (int i = 0; i < 2; ++i) {
                    const V3 p0 = pieces[i][0], p1 = pieces[i][1];
                    const V3 axis = normalize(p1 - p0, head.y);
                    const V3 radial = normalize((p0 + p1) * 0.5f - head.o, head.z);
                    const V3 xh = normalize(cross(axis, radial), head.x);
                    BodyPart& l = B.segment(p0, p1, xh, R * w * (i == 0 ? 1.0f : 0.78f), R * (i == 0 ? w * 0.78f : 0.015f), hairC);
                    l.scaleZ = 0.35f;
                    l.material = Material::Hair;
                }
            };
            // The fringe: five locks from the crown, over the hairline, tips above the brows.
            const float fu[] = {-0.55f, -0.27f, 0.0f, 0.27f, 0.55f};
            // (Straight pieces: a bend's round joint stood out as a ridge along the hairline.)
            for (int i = 0; i < 5; ++i) {
                const float u = fu[i], lean = (i % 2 ? 0.1f : -0.07f);
                const V3 a = at(u * 0.85f, 0.78f, 1.0f), t = at(u * 1.05f + lean, (i == 2 ? 0.2f : 0.27f) + 0.04f * float(i % 2), 1.065f);
                const V3 axis = normalize(t - a), radial = normalize((a + t) * 0.5f - head.o, head.z);
                BodyPart& l = B.segment(a, t, normalize(cross(axis, radial), head.x), R * 0.27f, R * 0.015f, hairC);
                l.scaleZ = 0.35f;
                l.material = Material::Hair;
            }
            for (int side = 0; side < 2; ++side) {
                const float sx = side == 0 ? 1.0f : -1.0f;
                const V3 sw = side == 0 ? swingL : swingR;
                // Framing the face: from the temple past the cheek to the jaw.
                lock(at(sx * 0.85f, 0.55f, 1.0f), at(sx * 1.1f, 0.0f, 1.06f), at(sx * 1.08f, -0.8f, 1.08f), 0.24f, sw,
                     0.5f, {-sx, 0, 0});
                // Flicks round the back, the tips out.
                for (int j = 0; j < 3; ++j) {
                    const float u = sx * (1.75f + 0.5f * float(j));
                    lock(at(u, 0.55f, 0.95f), at(u, 0.0f, 1.07f), at(u * 1.04f, -0.42f - 0.08f * float(j), 1.45f), 0.3f, sw,
                         0.5f, -sphereDir(u, 0));
                }
            }
            lock(at(3.1416f, 0.6f, 0.95f), at(3.1416f, 0.0f, 1.07f), at(3.1416f, -0.55f, 1.42f), 0.3f, swingMid, 0.5f,
                 {0, 0, 1});
            // A cowlick sticking up off the crown and curling forward.
            {
                const V3 a = head.point(V3{0.05f, 0.95f, -0.1f} * R), m = head.point(V3{0.05f, 1.38f, -0.02f} * R);
                const V3 t = swung(m, head.point(V3{0.12f, 1.48f, 0.3f} * R), swingMid, 0.6f);
                B.segment(a, m, head.x, R * 0.09f, R * 0.07f, hairC).material = Material::Hair;
                B.segment(m, t, head.x, R * 0.07f, R * 0.01f, hairC).material = Material::Hair;
            }
            break;
        }
        case HairStyle::Bald: break;
    }
    // Every hair part (not the accent ties) is drawn with strands.
    for (std::size_t i = hairBegin; i < out.parts.size(); ++i) {
        BodyPart& p = out.parts[i];
        const bool hairColor = std::fabs(p.color0.r - hairC.r) + std::fabs(p.color0.g - hairC.g) +
                                    std::fabs(p.color0.b - hairC.b) < 1e-5f;
        if (p.material == Material::Hair || hairColor) p.material = Material::Strands;
    }

    // Hair under a hat (Peter, 2026-11-20: "big hair poking out beyond hats"): fitted so none of it shows above
    // the hat's band (fitHairUnderHat).
    if (look.hat == Hat::Cap || look.hat == Hat::Beanie || look.hat == Hat::Top)
        fitHairUnderHat(out.parts, hairBegin, hatShape(look.hat, R, hs), B.worldFrame(head), scale);

    // ---- hats
    switch (look.hat) {
        case Hat::None: break;
        case Hat::Cap: {
            BodyPart& c = B.ball(head.translated(V3{0, 0.02f, -0.005f} * hs), R * 1.09f, 1.05f, 1.0f, look.accent);
            c.material = Material::Hair;
            B.clip(c, {0, -1, 0.15f}, -0.18f * R);
            BodyPart& brim = B.ball(head.translated(V3{0, 0.065f, 0.16f} * hs).rotatedX(0.12f), 0.02f * hs, 5.0f,
                                    5.5f, look.accent);
            brim.outline = true;
            break;
        }
        case Hat::Beanie: {
            BodyPart& c = B.ball(head.translated(V3{0, 0.03f, -0.005f} * hs), R * 1.12f, 1.05f, 1.0f, look.accent);
            c.material = Material::Hair;
            B.clip(c, {0, -1, 0.35f}, -0.12f * R);
            B.ball(head.translated({0, R * 1.18f, 0}), 0.04f * hs, 1, 1, look.accent);
            break;
        }
        case Hat::Top: {
            B.ball(head.translated({0, R * 0.78f, 0}), 0.018f * hs, 9.5f, 9.0f, look.accent);
            BodyPart& crown = B.segment(head.point({0, R * 0.8f, 0}), head.point({0, R * 0.8f + 0.17f * hs, 0}),
                                        head.x, 0.11f * hs, 0.115f * hs, look.accent, scaled(look.accent, 1.6f),
                                        0.035f * hs);
            crown.color0 = scaled(look.top, 1.0f);  // the band
            crown.color1 = look.accent;
            crown.split = 0.035f * hs * scale;
            break;
        }
        case Hat::Headphones: {
            const int n = 7;
            V3 prev{};
            for (int i = 0; i <= n; ++i) {
                const float a = -1.45f + 2.9f * float(i) / float(n);
                const V3 p = head.point({std::sin(a) * R * 1.13f, std::cos(a) * R * 1.13f, -0.01f * hs});
                if (i > 0) B.segment(prev, p, head.z, 0.016f * hs, 0.016f * hs, look.accent).outline = false;
                prev = p;
            }
            for (int side = 0; side < 2; ++side) {
                const float sx = side == 0 ? 1.0f : -1.0f;
                B.ball(head.translated({sx * R * 1.02f, -0.01f * hs, 0}), 0.06f * hs, 0.55f, 1.0f, look.accent);
            }
            break;
        }
    }

    // ---- beard (2026-11-21: 3D, in place of the painted ones; stubble stays painted)
    // Laid out in the face's own angles (DancerPipeline drawFace: u around, v up from the eye line; the mouth at
    // v -0.30, or -0.35 under a moustache): a point on the face at (u, v), k head radii out.
    if (look.beard != Beard::None && look.beard != Beard::Stubble) {
        const std::size_t beardBegin = out.parts.size();
        const Beard bd = look.beard;
        auto faceLocal = [&](float u, float v, float k) {  // in the head's frame
            return V3{1.05f * std::sin(u) * std::cos(v), std::sin(v), 0.97f * std::cos(u) * std::cos(v)} * (R * k);
        };
        auto face = [&](float u, float v, float k) { return head.point(faceLocal(u, v, k)); };
        // A tapered piece lying on the face from a to b, flat against it (z: out of the face).
        auto onFace = [&](V3 a, V3 b, float r0, float r1, float flat = 0.55f) -> BodyPart& {
            const V3 axis = normalize(b - a, head.y);
            const V3 radial = normalize((a + b) * 0.5f - head.o, head.z);
            BodyPart& p = B.segment(a, b, normalize(cross(axis, radial), head.x), R * r0, R * r1, hairC);
            p.scaleZ = flat;
            p.material = Material::Hair;
            return p;
        };
        // The beard over the jaw and chin, growing out of the skin (2026-11-21: a shell standing off the face, cut
        // by planes, hovered there, the cheek showing under its edge): a ball like the head's, shifted down and
        // back, so it pokes out of the head only on the lower face. Where the two surfaces cross is the beard's
        // edge, its thickness zero there, growing towards the chin and under the jaw. Worked in the head's scaled
        // frame (the head a unit ball): a ball at o (|o| = shift) of radius r covers the head where p . o/|o| > h,
        // h = (1 + shift^2 - r^2) / (2 shift); thickness about shift (p . o/|o| - h). The edge goes through
        // (0, front, 0.85) under the mouth and (0.95, side, 0.1) in front of the ear; `chin`: the thickness at the
        // chin (head radii). Cut away behind the ears.
        auto jaw = [&](float front, float side, float chin) {
            const float t = (side - front) / 0.75f;  // the shift's backward lean, per unit down
            const V3 dir = normalize(V3{0, -1, -t});
            const float h = (-side - 0.1f * t) / length(V3{0, -1, -t});
            const float cChin = dot(dir, normalize(V3{0, -0.85f, 0.5f}));
            const float shift = chin / std::max(cChin - h, 0.05f);
            const float r = std::sqrt(std::max(0.2f, 1.0f + shift * shift - 2.0f * shift * h));
            const V3 o = dir * shift;
            BodyPart& s = B.ball(head.translated(V3{o.x * 1.05f, o.y, o.z * 0.97f} * R), R * r, 1.05f, 0.97f, hairC);
            s.material = Material::Hair;
            cut(s, {0, 0, -1}, -0.12f);  // in front of the ears
        };
        // A moustache: full under the nose, tapering and drooping to the mouth's corners (a center piece and two
        // sides, thick enough to read as hair, not a bar); a handlebar is slimmer, its ends waxed and curled up.
        auto moustache = [&](float thick, bool handlebar) {
            BodyPart& c = B.ball(head.translated(faceLocal(0, -0.25f, 0.98f)), R * thick, 1.6f, 0.75f, hairC);
            c.material = Material::Hair;
            for (int side = 0; side < 2; ++side) {
                const float sx = side == 0 ? 1.0f : -1.0f;
                const V3 m = face(sx * 0.1f, -0.25f, 1.01f), e = face(sx * 0.3f, -0.32f, 1.0f);
                onFace(m, e, thick * 0.95f, handlebar ? thick * 0.45f : thick * 0.6f, 0.6f).scaleX = handlebar ? 1.2f : 1.5f;
                if (handlebar) onFace(e, face(sx * 0.43f, -0.2f, 1.0f), thick * 0.45f, 0.012f, 0.8f);
            }
        };
        // A goatee: a rounded tuft set into the chin, a little pointed below.
        auto goatee = [&]() {
            BodyPart& g = B.ball(head.translated(faceLocal(0, -0.7f, 0.9f)), R * 0.24f, 1.25f, 0.8f, hairC);
            g.material = Material::Hair;
            onFace(face(0, -0.78f, 1.0f), face(0, -1.0f, 1.0f), 0.17f, 0.04f, 0.7f);
        };
        switch (bd) {
            case Beard::Moustache: moustache(0.11f, false); break;
            case Beard::Handlebar: moustache(0.08f, true); break;
            case Beard::Goatee: goatee(); break;
            case Beard::Circle:  // moustache and goatee, joined round the mouth
                moustache(0.09f, false);
                goatee();
                for (int side = 0; side < 2; ++side) {
                    const float sx = side == 0 ? 1.0f : -1.0f;
                    onFace(face(sx * 0.3f, -0.32f, 1.0f), face(sx * 0.17f, -0.66f, 1.0f), 0.06f, 0.07f, 0.7f);
                }
                break;
            case Beard::Short: moustache(0.08f, false); jaw(-0.5f, -0.27f, 0.07f); break;  // close-cropped
            case Beard::Full: moustache(0.1f, false); jaw(-0.52f, -0.27f, 0.14f); break;
            case Beard::Long: {
                moustache(0.1f, false);
                jaw(-0.52f, -0.27f, 0.16f);
                // Hanging from the chin, swinging with the dance.
                const V3 a = face(0, -0.85f, 0.95f);
                const V3 b = swung(a, a + head.dir(V3{0, -0.75f, 0.12f} * R), swingMid, 0.4f);
                BodyPart& l = B.segment(a, b, head.x, R * 0.5f, R * 0.1f, hairC);
                l.scaleZ = 0.6f;
                l.material = Material::Hair;
                break;
            }
            default: break;
        }
        for (std::size_t i = beardBegin; i < out.parts.size(); ++i) out.parts[i].material = Material::Strands;
    }

    out.face.mouth = clampf(pose.mouth, 0, 1);
    out.face.blink = clampf(pose.blink, 0, 1);
    out.face.brow = clampf(pose.brow, -1, 1);
    out.face.eyes = look.eyes;
    out.face.eyeShape = look.eyeShape;
    out.face.brows = look.brows;
    out.face.mouthStyle = look.mouth;
    out.face.beard = look.beard;
    out.face.lashes = look.lashes;
    out.face.freckles = look.freckles;
    out.face.blush = clampf(look.blush, 0, 1);
    out.face.hair = look.hair;
    out.face.lipstick = look.lips.has_value();
    if (look.lips) out.face.lips = *look.lips;
    out.rootFloor = B.world(body.point({pose.hip.x, 0, pose.hip.z}));
    // The top of the head, hair and hat: each part's highest point (its ends' spheres, stretched across the axis).
    out.headTop = out.headCenter.y;
    for (std::size_t i = headBegin; i < out.parts.size(); ++i) {
        const BodyPart& p = out.parts[i];
        const Frame& f = p.frame;
        const float up = std::sqrt(f.x.y * f.x.y * p.scaleX * p.scaleX + f.y.y * f.y.y + f.z.y * f.z.y * p.scaleZ * p.scaleZ);
        out.headTop = std::max({out.headTop, f.o.y + p.r0 * up, f.o.y + f.y.y * p.len + p.r1 * up});
    }
}

}  // namespace reforce::engine::dance
