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
    ElbowRx, ElbowRy, ElbowRz, KneeLx, KneeLy, KneeLz, KneeRx, KneeRy, KneeRz, ChCount
};
constexpr const char* kChNames[ChCount] = {
    "bodyX", "bodyZ", "bodyYaw", "hipX", "hipY", "hipZ", "hipYaw", "hipRoll", "hipPitch", "chestYaw", "chestRoll",
    "chestPitch", "headYaw", "headRoll", "headPitch", "handLu", "handLv", "handLw", "handRu", "handRv", "handRw",
    "footLu", "footLy", "footLz", "footRu", "footRy", "footRz", "footYawL", "footYawR", "footPitchL", "footPitchR",
    "elbowLx", "elbowLy", "elbowLz", "elbowRx", "elbowRy", "elbowRz", "kneeLx", "kneeLy", "kneeLz", "kneeRx",
    "kneeRy", "kneeRz"};

struct Clip {
    ClipInfo info;
    int spb = 16;
    int turns = 0;               // whole body turns over one loop
    std::vector<float> samples;  // beats * spb frames of ChCount values
    int frames() const { return info.beats * spb; }
};

struct Library {
    std::vector<std::unique_ptr<Clip>> clips;  // stable addresses: MoveDef::name points into them
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

std::string loadClips(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "can't open " + path;
    const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return loadClipsFromMemory(bytes.data(), bytes.size(), path);
}

std::string loadClipsFromMemory(const unsigned char* data, std::size_t size, const std::string& path) {
    if (!data) return path + ": no data";
    Reader r{data, data + size};
    if (r.str(8) != "RFMOVES2") return path + ": not a moves file (RFMOVES2)";
    const auto nch = r.get<std::uint32_t>();
    if (nch == 0 || nch > 256) return path + ": bad channel count";
    std::vector<int> map(nch, -1);  // file channel -> Ch
    for (std::uint32_t i = 0; i < nch; ++i) {
        const std::string n = r.str8();
        for (int c = 0; c < ChCount; ++c)
            if (n == kChNames[c]) map[i] = c;
    }
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
        c->info.routine = r.get<std::uint8_t>() != 0;
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
    // Energy within each genre by thirds of motion: calm ones are chill, busy ones hype.
    std::vector<MoveDef> moves;
    for (const auto& c : clips) {
        std::vector<float> same;
        for (const auto& o : clips)
            if (o->info.genre == c->info.genre) same.push_back(o->info.motion);
        std::sort(same.begin(), same.end());
        const float rank = float(std::lower_bound(same.begin(), same.end(), c->info.motion) - same.begin()) /
                           float(std::max<std::size_t>(1, same.size()));
        const std::uint8_t energy = rank < 0.33f ? std::uint8_t(Chill | Groove)
                                    : rank < 0.67f ? std::uint8_t(Groove)
                                                   : std::uint8_t(Groove | Hype);
        MoveDef d{c->info.label.c_str(), Part::Full, c->info.beats, clipGenreStyles(c->info.genre), energy, nullptr};
        d.clip = int(moves.size());
        moves.push_back(d);
    }
    lib().clips = std::move(clips);
    const int first = setClipMoves(moves);
    for (std::size_t i = 0; i < lib().clips.size(); ++i) lib().clips[i]->info.move = first + int(i);
    return {};
}

int clipCount() { return int(lib().clips.size()); }

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
    for (int ch = 0; ch < ChCount; ++ch) {
        float a[4];
        for (int k = 0; k < 4; ++k) a[k] = s[k][ch] + (ch == BodyYaw ? wrap[k] * TAU * float(c.turns) : 0.0f);
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
}

}  // namespace reforce::engine::dance
