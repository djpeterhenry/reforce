#include "engine/DanceClips.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>

namespace reforce::engine::dance {

namespace {

constexpr float TAU = 6.28318530718f;

// The channels the engine reads, by name (a file may order them differently or carry more).
enum Ch {
    BodyX, BodyZ, BodyYaw, HipX, HipY, HipZ, HipYaw, HipRoll, HipPitch, ChestYaw, ChestRoll, ChestPitch,
    HeadYaw, HeadRoll, HeadPitch, HandLu, HandLv, HandLw, HandRu, HandRv, HandRw, FootLu, FootLy, FootLz,
    FootRu, FootRy, FootRz, FootYawL, FootYawR, FootPitchL, FootPitchR, ElbowLx, ElbowLy, ElbowLz,
    ElbowRx, ElbowRy, ElbowRz, KneeLx, KneeLy, KneeLz, KneeRx, KneeRy, KneeRz,
    // Floor work only (tools/aist_import build_floor.py): the pelvis's turn as a quaternion, the upper body's clearance,
    // then each hand's, knee's and the head's own (2026-11-27).
    HipQx, HipQy, HipQz, HipQw, Clear, FloorHandL, FloorHandR, FloorKneeL, FloorKneeR, FloorHead,
    // Planted contacts (build_floor.py, 2026-11-27): hands, feet, knees, the head.
    PinHandL, PinHandR, PinFootL, PinFootR, PinKneeL, PinKneeR, PinHead,
    // Made here from the hands' clearance (loadClips): how much each hand is planted.
    PlantL, PlantR, ChCount
};
constexpr const char* kChNames[ChCount] = {
    "bodyX", "bodyZ", "bodyYaw", "hipX", "hipY", "hipZ", "hipYaw", "hipRoll", "hipPitch", "chestYaw", "chestRoll",
    "chestPitch", "headYaw", "headRoll", "headPitch", "handLu", "handLv", "handLw", "handRu", "handRv", "handRw",
    "footLu", "footLy", "footLz", "footRu", "footRy", "footRz", "footYawL", "footYawR", "footPitchL", "footPitchR",
    "elbowLx", "elbowLy", "elbowLz", "elbowRx", "elbowRy", "elbowRz", "kneeLx", "kneeLy", "kneeLz", "kneeRx",
    "kneeRy", "kneeRz", "hipQx", "hipQy", "hipQz", "hipQw", "clear", "floorHandL", "floorHandR", "floorKneeL",
    "floorKneeR", "floorHead", "pinHandL", "pinHandR", "pinFootL", "pinFootR", "pinKneeL", "pinKneeR", "pinHead",
    "(plantL)", "(plantR)"};

struct Clip {
    ClipInfo info;
    int spb = 16;
    int turns = 0;               // whole body turns over one loop
    bool floor = false;          // floor work: the file has the pelvis quaternion and the clearance
    bool contacts = false;       // and each hand's, knee's and the head's clearance
    bool pins = false;           // and the planted contacts
    std::uint8_t energy = 0;     // Energy bits (ranked within the genre of its own file)
    std::vector<float> samples;  // beats * spb frames of ChCount values
    int frames() const { return info.beats * spb; }
};

struct Library {
    std::vector<std::unique_ptr<Clip>> clips;  // stable addresses: MoveDef::name points into them
    float tierWeight[4] = {1, kDefaultFloorWeight, 0, 0};  // setFloorWeights (flips 0: never)
    FloorReach bustReach;
};
Library& lib() {
    static Library l;
    return l;
}

// Each AIST genre is one of our styles (Peter, 2026-11-21): middle ("party") and LA hip-hop are hip-hop, street jazz
// is jazz, ballet jazz is ballet, waack is disco, krump is stomp. The names are what the move labels say.
struct Genre {
    const char* code;
    const char* name;
    Style style;
};
const Genre kGenres[] = {
    {"HO", "house", Style::House},
    {"PO", "pop", Style::Pop},
    {"LO", "lock", Style::Lock},
    {"BR", "break", Style::Break},
    {"WA", "disco", Style::Disco},
    {"KR", "stomp", Style::Stomp},
    {"MH", "party hip-hop", Style::HipHop},
    {"LH", "LA hip-hop", Style::HipHop},
    {"JS", "street jazz", Style::Jazz},
    {"JB", "ballet jazz", Style::Ballet},
};

// Reads little-endian values from a byte buffer; any read past the end sets `bad`.
struct Reader {
    const unsigned char* p;
    const unsigned char* end;
    bool bad = false;
    template <typename T>
    T get() {
        T v{};
        if (end - p < std::ptrdiff_t(sizeof(T))) {
            bad = true;
            return v;
        }
        std::memcpy(&v, p, sizeof(T));  // the file is little-endian, as are all our targets
        p += sizeof(T);
        return v;
    }
    std::string str(std::size_t n) {
        if (std::size_t(end - p) < n) {
            bad = true;
            return {};
        }
        std::string s(reinterpret_cast<const char*>(p), n);
        p += n;
        return s;
    }
    std::string str8() { return str(get<std::uint8_t>()); }
};

// Inverse of the importer's `across` (feet): |u| <= 1 inside +-half, beyond that half + (|u| - 1) * reach.
float acrossTo(float u, float half, float reach) {
    const float a = std::fabs(u);
    const float v = a <= 1 ? a * half : half + (a - 1) * reach;
    return u < 0 ? -v : v;
}

// A hand target in the chest frame from (u, v, w), its offset from its own shoulder in arm lengths (side sx = +1
// left, -1 right). Our characters' shoulders are wider for their arms than a real dancer's (half-width 0.64 arm
// against about 0.38), so a hand moving in towards the middle is pulled in by the difference: claps meet in the
// middle. The pull never takes a hand further from its shoulder than the dancer had it (or an arm's length).
V3 handTarget(float u, float v, float w, float sx, const Skeleton& sk) {
    constexpr float kSourceShoulder = 0.38f;  // a real dancer's shoulder half-width in arm lengths
    const float arm = sk.upperArm + sk.forearm;
    const float extra = sk.shoulderHalf - kSourceShoulder * arm;
    const float in = std::clamp(-sx * u / kSourceShoulder, 0.0f, 1.0f);  // 0 at the shoulder or outside, 1 mid
    float off = u * arm - sx * extra * in;  // across, from the shoulder
    const float reach = std::max(1.0f, std::sqrt(u * u + v * v + w * w)) * arm;
    const float room = reach * reach - (v * v + w * w) * arm * arm;
    if (room > 0 && std::fabs(off) > std::sqrt(room)) off = std::copysign(std::sqrt(room), off);
    return {sx * sk.shoulderHalf + off, sk.shoulderY + v * arm, w * arm};
}

FloorReach measureReach(int clip);

}  // namespace

const std::vector<std::string_view>& clipGenres() {
    static const std::vector<std::string_view> v = [] {
        std::vector<std::string_view> r;
        for (const Genre& g : kGenres) r.emplace_back(g.code);
        return r;
    }();
    return v;
}

std::string_view clipGenreName(std::string_view code) {
    for (const Genre& g : kGenres)
        if (code == g.code) return g.name;
    return code;
}

std::optional<Style> clipGenreStyle(std::string_view code) {
    for (const Genre& g : kGenres)
        if (code == g.code) return g.style;
    return std::nullopt;
}

std::uint32_t clipGenreStyles(std::string_view code) {
    const auto s = clipGenreStyle(code);
    return s ? styleBit(*s) : 0;
}

const char* clipTierName(int tier) {
    switch (tier) {
        case kFloorTier: return "floor";
        case kFlips: return "flips";
        default: return "standing";
    }
}

void setFloorWeights(float floor) {
    lib().tierWeight[kFloorTier] = std::isfinite(floor) ? std::clamp(floor, 0.0f, 4.0f) : 0.0f;
}

float clipWeight(int clip) {
    if (clip < 0 || clip >= clipCount()) return 0;
    return lib().tierWeight[std::clamp(lib().clips[std::size_t(clip)]->info.tier, 0, 3)];
}

namespace {
// The whole clip turned by a fixed angle (its heading and its travel with it: the dance itself is unchanged), the one
// that has the most samples facing within 90 degrees of the camera; among those (within half a percent) the one
// whose mean heading is nearest the camera, then the smallest turn. Mirroring mirrors it with the rest.
void faceAudience(Clip& c) {
    const int n = c.frames();
    if (n <= 0) return;
    std::vector<float> yaw(static_cast<std::size_t>(n));
    for (int k = 0; k < n; ++k) yaw[std::size_t(k)] = c.samples[std::size_t(k) * ChCount + BodyYaw];
    auto away = [&](float off) {
        int a = 0;
        for (float y : yaw) a += std::fabs(std::remainder(y + off, TAU)) > TAU / 4;
        return float(a) / float(n);
    };
    float sc = 0, ss = 0;  // the circular mean heading
    for (float y : yaw) {
        sc += std::cos(y);
        ss += std::sin(y);
    }
    const float mean = std::atan2(ss, sc);
    constexpr int kSteps = 360;
    float best = 2;
    for (int i = 0; i < kSteps; ++i) best = std::min(best, away(TAU * float(i) / kSteps - TAU / 2));
    float pick = 0, pickCost = 1e9f;
    for (int i = 0; i < kSteps; ++i) {
        const float off = TAU * float(i) / kSteps - TAU / 2;
        if (away(off) > best + 0.005f) continue;
        const float cost = std::fabs(std::remainder(mean + off, TAU)) + 0.01f * std::fabs(off);
        if (cost < pickCost) pickCost = cost, pick = off;
    }
    c.info.awayBefore = away(0);
    // (No turn for less than 2 % more facing the camera: a clip facing it already, or spinning evenly all round, stays
    // exactly as it was.)
    if (away(pick) > c.info.awayBefore - 0.02f) pick = 0;
    c.info.awayAfter = away(pick);
    c.info.yawOffset = pick;
    if (pick == 0) return;
    // The body frame: local x -> (cos, -sin), local z -> (sin, cos) on the stage (DancerRun, buildBody).
    const float co = std::cos(pick), si = std::sin(pick);
    for (int k = 0; k < n; ++k) {
        float* v = &c.samples[std::size_t(k) * ChCount];
        const float x = v[BodyX], z = v[BodyZ];
        v[BodyX] = x * co + z * si;
        v[BodyZ] = -x * si + z * co;
        v[BodyYaw] += pick;
    }
}
}  // namespace

std::string loadClips(const std::string& path, bool append) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "can't open " + path;
    const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return loadClipsFromMemory(bytes.data(), bytes.size(), path, append);
}

std::string loadClipsFromMemory(const unsigned char* data, std::size_t size, const std::string& path, bool append) {
    if (!data) return path + ": no data";
    Reader r{data, data + size};
    if (r.str(8) != "RFMOVES2") return path + ": not a moves file (RFMOVES2)";
    const auto nch = r.get<std::uint32_t>();
    if (nch == 0 || nch > 256) return path + ": bad channel count";
    std::vector<int> map(nch, -1);  // file channel -> Ch
    bool has[ChCount] = {};
    for (std::uint32_t i = 0; i < nch; ++i) {
        const std::string n = r.str8();
        for (int c = 0; c < ChCount; ++c)
            if (n == kChNames[c]) {
                map[i] = c;
                has[c] = true;
            }
    }
    const bool floor = has[HipQx] && has[HipQy] && has[HipQz] && has[HipQw] && has[Clear];
    const bool contacts = floor && has[FloorHandL] && has[FloorHandR] && has[FloorKneeL] && has[FloorKneeR] && has[FloorHead];
    bool pins = floor;
    for (int c = PinHandL; c <= PinHead; ++c) pins = pins && has[c];
    std::vector<int> bits(nch);  // 8 or 16 per channel
    for (std::uint32_t i = 0; i < nch; ++i) {
        bits[i] = r.get<std::uint8_t>();
        if (bits[i] != 8 && bits[i] != 16) return path + ": bad channel bits";
    }
    const auto count = r.get<std::uint32_t>();
    if (r.bad || count > 100000) return path + ": bad header";
    std::vector<std::unique_ptr<Clip>> clips;
    for (std::uint32_t m = 0; m < count && !r.bad; ++m) {
        auto c = std::make_unique<Clip>();
        c->info.id = r.str8();
        c->info.label = r.str8();
        c->info.genre = r.str(2);
        const auto kind = r.get<std::uint8_t>();  // bit 0 routine; bits 4-5 the floor tier
        c->info.routine = (kind & 1) != 0;
        c->info.tier = floor ? (kind >> 4) & 3 : 0;
        if (c->info.tier == 2) c->info.tier = kFloorTier;  // (freezes: none made; read as floor)
        c->floor = floor;
        c->info.floorWork = floor;
        c->contacts = contacts;
        c->pins = pins;
        c->info.bpm = r.get<std::uint16_t>();
        c->info.beats = r.get<std::uint16_t>();
        c->spb = r.get<std::uint16_t>();
        c->turns = r.get<std::int16_t>();
        if (r.bad || c->info.beats < 1 || c->info.beats > 256 || c->spb < 2 || c->spb > 120)
            return path + ": bad move " + std::to_string(m);
        std::vector<float> lo(nch), hi(nch);
        for (std::uint32_t i = 0; i < nch; ++i) {
            lo[i] = r.get<float>();
            hi[i] = r.get<float>();
        }
        const int n = c->frames();
        c->samples.assign(std::size_t(n) * ChCount, 0.0f);
        for (int k = 0; k < n; ++k)
            for (std::uint32_t i = 0; i < nch; ++i) {
                const float q = bits[i] == 16 ? float(r.get<std::uint16_t>()) / 65535.0f
                                              : float(r.get<std::uint8_t>()) / 255.0f;
                if (map[i] >= 0) c->samples[std::size_t(k) * ChCount + std::size_t(map[i])] = lo[i] + q * (hi[i] - lo[i]);
            }
        if (r.bad) return path + ": truncated at move " + std::to_string(m);
        for (float v : c->samples)
            if (!std::isfinite(v)) return path + ": bad numbers in move " + std::to_string(m);
        faceAudience(*c);
        if (contacts) {
            // A hand the source dancer had on the floor (its wrist's contact surface within 2 % of a leg of it, fully
            // off by 7 %) is planted; widened by an 8th of a beat either way and smoothed as much, so a hand slapped
            // down fast still eases onto the floor instead of popping there.
            const int reach = std::max(1, c->spb / 8);
            for (int side = 0; side < 2; ++side) {
                const int from = side == 0 ? FloorHandL : FloorHandR, to = side == 0 ? PlantL : PlantR;
                std::vector<float> raw(static_cast<std::size_t>(n)), wide(static_cast<std::size_t>(n));
                for (int k = 0; k < n; ++k) {
                    const float t = std::clamp((c->samples[std::size_t(k) * ChCount + std::size_t(from)] - 0.02f) / 0.05f, 0.0f, 1.0f);
                    raw[std::size_t(k)] = 1 - t * t * (3 - 2 * t);
                }
                auto at = [&](const std::vector<float>& v, int k) { return v[std::size_t(((k % n) + n) % n)]; };
                for (int k = 0; k < n; ++k) {
                    float hi = 0;
                    for (int d = -reach; d <= reach; ++d) hi = std::max(hi, at(raw, k + d));
                    wide[std::size_t(k)] = hi;
                }
                for (int k = 0; k < n; ++k) {
                    float sum = 0, wsum = 0;
                    for (int d = -reach; d <= reach; ++d) {
                        const float wt = float(reach + 1 - std::abs(d));
                        sum += wt * at(wide, k + d);
                        wsum += wt;
                    }
                    c->samples[std::size_t(k) * ChCount + std::size_t(to)] = sum / wsum;
                }
            }
        }
        // Motion: the hands and feet, in arm / leg lengths per beat.
        double sum = 0;
        for (int k = 0; k < n; ++k) {
            const float* a = &c->samples[std::size_t(k) * ChCount];
            const float* b = &c->samples[std::size_t((k + 1) % n) * ChCount];
            for (int ch : {HandLu, HandLv, HandLw, HandRu, HandRv, HandRw, FootLu, FootLy, FootLz, FootRu, FootRy, FootRz})
                sum += std::fabs(b[ch] - a[ch]);
        }
        c->info.motion = float(sum / double(c->info.beats));
        clips.push_back(std::move(c));
    }
    if (r.bad) return path + ": truncated";
    // Energy within each genre by thirds of motion (among this file's moves): calm ones are chill, busy ones hype.
    for (const auto& c : clips) {
        std::vector<float> same;
        for (const auto& o : clips)
            if (o->info.genre == c->info.genre) same.push_back(o->info.motion);
        std::sort(same.begin(), same.end());
        const float rank = float(std::lower_bound(same.begin(), same.end(), c->info.motion) - same.begin()) /
                           float(std::max<std::size_t>(1, same.size()));
        c->energy = rank < 0.33f ? std::uint8_t(Chill | Groove)
                    : rank < 0.67f ? std::uint8_t(Groove)
                                   : std::uint8_t(Groove | Hype);
    }
    if (append)
        for (auto& c : clips) lib().clips.push_back(std::move(c));
    else
        lib().clips = std::move(clips);
    // Floor work's reach (the stage clears that much room round a dancer going down), and a bust's.
    lib().bustReach = FloorReach{};
    for (std::size_t i = 0; i < lib().clips.size(); ++i) {
        Clip& c = *lib().clips[i];
        if (!c.floor) continue;
        if (c.info.reach.max <= 0) c.info.reach = measureReach(int(i));
        if (c.info.genre == "BR" && c.info.beats >= 64)
            for (int k = 0; k < 8; ++k) {
                lib().bustReach.dir[k] = std::max(lib().bustReach.dir[k], c.info.reach.dir[k]);
                lib().bustReach.max = std::max(lib().bustReach.max, c.info.reach.max);
            }
    }
    std::vector<MoveDef> moves;
    for (const auto& c : lib().clips) {
        MoveDef d{c->info.label.c_str(), Part::Full, c->info.beats, clipGenreStyles(c->info.genre), c->energy, nullptr};
        d.clip = int(moves.size());
        moves.push_back(d);
    }
    const int first = setClipMoves(moves);
    for (std::size_t i = 0; i < lib().clips.size(); ++i) lib().clips[i]->info.move = first + int(i);
    return {};
}

int clipCount() { return int(lib().clips.size()); }

const FloorReach& bustMoveReach() { return lib().bustReach; }

float FloorReach::toward(float angle) const {
    float a = angle / (TAU / 8);
    a -= std::floor(a / 8) * 8;
    const int i = int(a) & 7;
    const float t = a - std::floor(a);
    return dir[i] + (dir[(i + 1) & 7] - dir[i]) * t;
}

FloorReach FloorReach::scaled(float k) const {
    FloorReach r = *this;
    for (float& d : r.dir) d *= k;
    r.max *= k;
    return r;
}

const ClipInfo& clipInfo(int clip) { return lib().clips[std::size_t(clip)]->info; }

int findClip(std::string_view id) {
    for (std::size_t i = 0; i < lib().clips.size(); ++i)
        if (lib().clips[i]->info.id == id || lib().clips[i]->info.label == id) return int(i);
    return -1;
}

void clipPose(int clip, float b, const Skeleton& sk, DancePose& p) {
    if (clip < 0 || clip >= clipCount()) return;
    const Clip& c = *lib().clips[std::size_t(clip)];
    const int n = c.frames();
    float x = b * float(c.spb);
    x -= std::floor(x / float(n)) * float(n);
    const int i1 = std::min(n - 1, int(std::floor(x)));
    const float t = x - float(i1);
    // Catmull-Rom over the loop; the body's whole turns are added where the samples wrap.
    float v[ChCount];
    const float* s[4];
    float wrap[4];
    for (int k = 0; k < 4; ++k) {
        int i = i1 - 1 + k;
        wrap[k] = i < 0 ? -1.0f : i >= n ? 1.0f : 0.0f;
        i = (i % n + n) % n;
        s[k] = &c.samples[std::size_t(i) * ChCount];
    }
    const float t2 = t * t, t3 = t2 * t;
    const float w0 = -0.5f * t3 + t2 - 0.5f * t, w1 = 1.5f * t3 - 2.5f * t2 + 1, w2 = -1.5f * t3 + 2 * t2 + 0.5f * t,
                w3 = 0.5f * t3 - 0.5f * t2;
    // Floor work's pelvis quaternion: each sample on the same side as the second (q and -q are the same turn).
    float qs[4] = {1, 1, 1, 1};
    if (c.floor)
        for (int k = 0; k < 4; ++k) {
            float d = 0;
            for (int ch = HipQx; ch <= HipQw; ++ch) d += s[k][ch] * s[1][ch];
            qs[k] = d < 0 ? -1.0f : 1.0f;
        }
    for (int ch = 0; ch < ChCount; ++ch) {
        float a[4];
        for (int k = 0; k < 4; ++k)
            a[k] = s[k][ch] * (ch >= HipQx && ch <= HipQw ? qs[k] : 1.0f) +
                   (ch == BodyYaw ? wrap[k] * TAU * float(c.turns) : 0.0f);
        v[ch] = w0 * a[0] + w1 * a[1] + w2 * a[2] + w3 * a[3];
    }
    const float leg = sk.thigh + sk.shin;
    p.bodyX = v[BodyX] * leg;
    p.bodyZ = v[BodyZ] * leg;
    p.bodyYaw = v[BodyYaw];
    p.hip = {v[HipX] * leg, v[HipY] * leg, v[HipZ] * leg};
    p.hipYaw = v[HipYaw];
    p.hipRoll = v[HipRoll];
    p.hipPitch = v[HipPitch];
    p.chestYaw = v[ChestYaw];
    p.chestRoll = v[ChestRoll];
    p.chestPitch = v[ChestPitch];
    p.headYaw = v[HeadYaw];
    p.headRoll = v[HeadRoll];
    p.headPitch = v[HeadPitch];
    p.handL = handTarget(v[HandLu], v[HandLv], v[HandLw], 1, sk);
    p.handR = handTarget(v[HandRu], v[HandRv], v[HandRw], -1, sk);
    p.footL = {acrossTo(v[FootLu], sk.footHalf, leg), std::max(0.0f, v[FootLy] * leg), v[FootLz] * leg};
    p.footR = {acrossTo(v[FootRu], sk.footHalf, leg), std::max(0.0f, v[FootRy] * leg), v[FootRz] * leg};
    p.footYawL = v[FootYawL];
    p.footYawR = v[FootYawR];
    p.footPitchL = v[FootPitchL];
    p.footPitchR = v[FootPitchR];
    auto hint = [](float x, float y, float z) {  // at most unit length (the interpolation can overshoot)
        const V3 h{x, y, z};
        const float l = length(h);
        return l > 1 ? h * (1 / l) : h;
    };
    p.elbowL = hint(v[ElbowLx], v[ElbowLy], v[ElbowLz]);
    p.elbowR = hint(v[ElbowRx], v[ElbowRy], v[ElbowRz]);
    p.kneeL = hint(v[KneeLx], v[KneeLy], v[KneeLz]);
    p.kneeR = hint(v[KneeRx], v[KneeRy], v[KneeRz]);
    if (c.floor) {  // the rig normalizes the quaternion; groundPose holds the body to the floor
        p.hipQx = v[HipQx];
        p.hipQy = v[HipQy];
        p.hipQz = v[HipQz];
        p.hipQw = v[HipQw];
        p.ground = 1;
        p.clear = std::max(0.0f, v[Clear]) * leg;
        // Planted hands (loadClips; groundPose puts them on the floor).
        p.plantL = c.contacts ? std::clamp(v[PlantL], 0.0f, 1.0f) : 0.0f;
        p.plantR = c.contacts ? std::clamp(v[PlantR], 0.0f, 1.0f) : 0.0f;
        // Planted contacts (DancerRun holds them where they came down).
        float* pin[7] = {&p.pinHandL, &p.pinHandR, &p.pinFootL, &p.pinFootR, &p.pinKneeL, &p.pinKneeR, &p.pinHead};
        for (int k = 0; k < 7; ++k) *pin[k] = c.pins ? std::clamp(v[PinHandL + k], 0.0f, 1.0f) : 0.0f;
    } else {
        p.hipQx = p.hipQy = p.hipQz = 0;
        p.hipQw = 1;
        p.ground = p.clear = 0;
        p.plantL = p.plantR = 0;
        p.pinHandL = p.pinHandR = p.pinFootL = p.pinFootR = p.pinKneeL = p.pinKneeR = p.pinHead = 0;
    }
}

bool clipContacts(int clip, float b, const Skeleton& sk, float out[5]) {
    if (clip < 0 || clip >= clipCount()) return false;
    const Clip& c = *lib().clips[std::size_t(clip)];
    if (!c.contacts) return false;
    const int n = c.frames();
    float x = b * float(c.spb);
    x -= std::floor(x / float(n)) * float(n);
    const int i0 = std::min(n - 1, int(std::floor(x)));
    const float t = x - float(i0);
    const float* s0 = &c.samples[std::size_t(i0) * ChCount];
    const float* s1 = &c.samples[std::size_t((i0 + 1) % n) * ChCount];
    const float leg = sk.thigh + sk.shin;
    for (int k = 0; k < 5; ++k) out[k] = (s0[FloorHandL + k] + (s1[FloorHandL + k] - s0[FloorHandL + k]) * t) * leg;
    return true;
}

namespace {
FloorReach measureReach(int clip) {
    // The standard body (a reach is scaled to each dancer's legs), sampled every 8th of a beat, grounded as danced.
    const Look look;
    const Skeleton sk = makeSkeleton(look);
    const ClipInfo& ci = lib().clips[std::size_t(clip)]->info;
    FloorReach r;
    float headFront = 0, headBack = 0;
    for (int k = 0; k < ci.beats * 8; ++k) {
        DancePose p = restPose(sk);
        clipPose(clip, float(k) / 8.0f, sk, p);
        groundPose(sk, p);
        HairCollider col[kHairColliders];
        hairColliders(look, sk, p, col);
        auto add = [&](V3 q, float rad) {
            r.max = std::max(r.max, std::hypot(q.x, q.z) + rad);
            for (int d = 0; d < 8; ++d) {
                const float a = float(d) * TAU / 8;  // from the front (+z) towards the left (+x)
                r.dir[d] = std::max(r.dir[d], q.z * std::cos(a) + q.x * std::sin(a) + rad);
            }
        };
        for (const HairCollider& c : col) {
            if (c.r0 <= 0 && c.r1 <= 0) continue;
            const float sx = std::max(c.sx, c.sz);  // (the torso: flattened across)
            add(c.a, c.r0 * sx);
            add(c.b, c.r1 * sx);
        }
        for (int side = 0; side < 2; ++side) {
            add(kneeAt(sk, p, side), 0.05f);
            add((side == 0 ? p.footL : p.footR) + V3{p.bodyX, 0, p.bodyZ}, 0.06f);  // (near enough: the toes)
        }
        add(pelvisAt(sk, p), 0.1f);
        // Head first: the head low, ahead of the hips (towards the front) or behind.
        const V3 head = headFrame(sk, p).o, hips = pelvisAt(sk, p);
        if (head.y < hips.y + 0.05f) {
            headFront = std::max(headFront, head.z - hips.z);
            headBack = std::max(headBack, hips.z - head.z);
        }
    }
    r.headFirst = headFront > 0.2f && headFront > 2 * headBack && r.dir[0] > 1.4f * r.dir[4];
    return r;
}
}  // namespace

}  // namespace reforce::engine::dance
