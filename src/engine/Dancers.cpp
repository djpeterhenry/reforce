#include "engine/Dancers.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>


namespace reforce::engine {

using namespace dance;

namespace {
constexpr double kPi = 3.14159265358979323846;

Mat4 lookAt(V3 eye, V3 target) {
    const V3 f = normalize(target - eye);
    const V3 s = normalize(cross(f, {0, 1, 0}));
    const V3 u = cross(s, f);
    Mat4 m = Mat4::identity();
    // rows: s, u, -f
    m.m[0] = s.x;
    m.m[4] = s.y;
    m.m[8] = s.z;
    m.m[1] = u.x;
    m.m[5] = u.y;
    m.m[9] = u.z;
    m.m[2] = -f.x;
    m.m[6] = -f.y;
    m.m[10] = -f.z;
    m.m[12] = -dot(s, eye);
    m.m[13] = -dot(u, eye);
    m.m[14] = dot(f, eye);
    return m;
}

std::string trimmed(const std::string& s) {
    std::string t;
    for (char c : s)
        if (c != '"') t.push_back(c);
    const auto b = t.find_first_not_of(" \t\r\n");
    const auto e = t.find_last_not_of(" \t\r\n");
    return b == std::string::npos ? std::string() : t.substr(b, e - b + 1);
}

float easeOutBack(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    const float c1 = 1.9f, c3 = c1 + 1;
    return 1 + c3 * std::pow(t - 1, 3.0f) + c1 * std::pow(t - 1, 2.0f);
}
}  // namespace

StageCamera stageCamera(double aspect, double height, float floorBack, double eyeHeight) {
    StageCamera c;
    c.floorBack = std::min(floorBack, kFloorZMax - 0.5f);
    height = std::clamp(height, 0.15, 0.9);
    const double half = std::tan(c.fovY * kPi / 360.0);
    const double D = 1.0 / (height * 2.0 * half);  // a 1-unit dancer fills `height` of the frame at distance D
    const double h = std::clamp(eyeHeight, 0.0, 1.5) * D;  // eye height: looking slightly down
    const double pitch = std::atan(h / D) - std::atan(0.84 * half);  // feet ~8 % above the bottom edge
    c.eye = {0, float(h), float(D)};
    c.distance = std::sqrt(D * D + h * h);
    c.halfWidth = D * half * aspect;
    const V3 target{0, float(h - D * std::tan(pitch)), 0};
    const Mat4 view = lookAt(c.eye, target);
    const double nearZ = 0.1, farZ = D + 20.0;
    Mat4 proj{};
    const double f = 1.0 / half;
    proj.m[0] = float(f / aspect);
    proj.m[5] = float(f);
    proj.m[10] = float(farZ / (nearZ - farZ));
    proj.m[11] = -1.0f;
    proj.m[14] = float(nearZ * farZ / (nearZ - farZ));
    c.proj11 = float(f);
    c.viewProj = proj * view;
    return c;
}

V3 projectStage(const StageCamera& cam, V3 p) {
    const auto c = transform(cam.viewProj, {p.x, p.y, p.z, 1.0f});
    if (c[3] <= 1e-6f) return {std::nanf(""), std::nanf(""), std::nanf("")};
    return {(c[0] / c[3] + 1.0f) * 0.5f, (1.0f - c[1] / c[3]) * 0.5f, c[2] / c[3]};
}

std::vector<std::pair<float, float>> stageSlots(int n, const StageCamera& cam) {
    std::vector<std::pair<float, float>> out;
    if (n <= 0) return out;
    const float width = float(cam.halfWidth) * 2.0f;
    const float spacing = std::min(1.0f, 0.8f * width / float(n));
    for (int k = 0; k < n; ++k) {
        const float x = (float(k) - float(n - 1) * 0.5f) * spacing;
        const float z = (n >= 3 && k % 2 == 1) ? -0.35f : 0.0f;
        out.push_back({x, z});
    }
    return out;
}

namespace {
std::uint32_t mix32(std::uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}
float unit32(std::uint32_t x) { return float(mix32(x) >> 8) / float(1u << 24); }
// Smooth value noise in -1..1: a random value at each integer, quintic in between (no kinks: C2).
float vnoise(std::uint32_t seed, double x) {
    const double i = std::floor(x);
    const float f = float(x - i);
    const auto at = [&](double k) {
        return unit32(seed * 0x9e3779b9U ^ std::uint32_t(std::int64_t(k)) * 0x85ebca6bU) * 2 - 1;
    };
    const float s = f * f * f * (f * (f * 6 - 15) + 10);
    return at(i) + (at(i + 1) - at(i)) * s;
}
float yawTowards(float x, float z, float tx, float tz) {
    return std::clamp(std::atan2(tx - x, tz - z), -1.15f, 1.15f);
}
}  // namespace

// ---- mingle (Peter, 2026-11-20): each dancer on their own clock, groups that form and break up.
namespace {
float rnd(std::uint32_t& s) {
    s = s * 1664525u + 1013904223u;
    return unit32(s);
}
float rndIn(std::uint32_t& s, float lo, float hi) { return lo + (hi - lo) * rnd(s); }
// Distance as it reads on screen: depth counts half.
float screenDist(float ax, float az, float bx, float bz) {
    const float dx = bx - ax, dz = (bz - az) * 0.5f;
    return std::sqrt(dx * dx + dz * dz);
}
// Round a group: neighbors at least kGroupGap apart (a pair face to face; 3+ round the horseshoe, kHorseshoe either
// side of its back). Arms reach about 0.48 from a dancer's axis (Peter, 2026-11-27: arms went into neighbors at the
// old 0.52 / 0.54 / 0.46 gaps).
constexpr float kGroupGap = 0.62f, kHorseshoe = 1.85f;
float groupRadius(std::size_t n) {
    if (n <= 2) return 0.5f * kGroupGap;
    return std::max(0.5f * kGroupGap, kGroupGap / (2.0f * std::sin(kHorseshoe / float(n - 1))));
}
constexpr std::size_t kMaxGroup = 4;
constexpr std::size_t kMaxGroupWith = 6;  // a group grows past kMaxGroup only for someone who picked a member
}  // namespace

std::optional<MingleVibe> parseMingleVibe(std::string_view s) {
    if (s == "mingle" || s.empty()) return MingleVibe::Mingle;
    if (s == "solo" || s == "alone") return MingleVibe::Solo;
    if (s == "crew" || s == "group") return MingleVibe::Crew;
    return std::nullopt;
}

float Mingle::sociable(const Agent& a) const {
    return a.vibe == MingleVibe::Solo ? 0.0f : a.vibe == MingleVibe::Crew ? 1.0f : kPersonality.sociable;
}

bool Mingle::together(std::uint64_t a, std::uint64_t b) const {
    int ga = 0, gb = 0;
    for (const auto& x : agents_) {
        if (x.id == a) ga = x.group;
        if (x.id == b) gb = x.group;
    }
    return ga && ga == gb;
}

std::uint64_t Mingle::watching(std::uint64_t id) const {
    for (const auto& a : agents_)
        if (a.id == id) return a.act == Act::Watch ? a.target : 0;
    return 0;
}

void Mingle::clear() {
    agents_.clear();
    groups_.clear();
    started_ = false;
}

Mingle::Agent* Mingle::find(std::uint64_t id) {
    for (auto& a : agents_)
        if (a.id == id) return &a;
    return nullptr;
}

Mingle::Group* Mingle::findGroup(int id) {
    for (auto& g : groups_)
        if (g.id == id) return &g;
    return nullptr;
}

int Mingle::groupSize(std::uint64_t id) const {
    for (const auto& a : agents_)
        if (a.id == id && a.group)
            for (const auto& g : groups_)
                if (g.id == a.group) return int(g.members.size());
    return 0;
}

Mingle::Traits Mingle::traits(std::uint64_t id) const {
    for (const auto& a : agents_)
        if (a.id == id) return kPersonality;
    return {0, 0, 0};
}

float Mingle::roomHalfWidth(const StageCamera& cam) const {
    // Most of the visible width (less for few people, so they stay near the middle).
    return float(cam.halfWidth) * std::min(0.92f, 0.35f + 0.11f * float(agents_.size()));
}

// A free spot: a few random candidates, the one furthest from everyone (where they are and where they are going),
// not too far a walk. `edge`: somewhere near the sides or the back (taking a break).
std::pair<float, float> Mingle::pickSpot(Agent& a, const StageCamera& cam, bool edge) {
    const float W = roomHalfWidth(cam);
    const float z0 = cam.floorBack + 0.1f, z1 = kFloorZMax - 0.05f;
    float bx = a.x, bz = a.z, best = -1e9f;
    for (int k = 0; k < 10; ++k) {
        const float u = rnd(a.rng) * 2 - 1, z = z0 + rnd(a.rng) * (z1 - z0);
        const float x = u * W * floorWiden(cam, z);  // the floor is wider further back
        float clear = 1.2f;
        for (const auto& o : agents_) {
            if (o.id == a.id) continue;
            clear = std::min({clear, screenDist(x, z, o.x, o.z), screenDist(x, z, o.outX, o.outZ)});
            if (o.room > 0) clear = std::min(clear, std::hypot(x - o.ax, z - o.az) - o.room * o.reach.max);
        }
        for (const auto& g : groups_) clear = std::min(clear, screenDist(x, z, g.cx, g.cz) - groupRadius(g.members.size()));
        const float walk = std::hypot(x - a.x, z - a.z);
        float score = clear - 0.2f * std::max(0.0f, walk - 1.4f) + 0.1f * rnd(a.rng);
        if (walk < 0.3f) score -= 0.3f;  // a wander goes somewhere
        if (edge) score += 0.35f * std::max(std::fabs(x) / std::max(W, 1e-3f), (z1 - z) / (z1 - z0));
        if (score > best) {
            best = score;
            bx = x;
            bz = z;
        }
    }
    return {bx, bz};
}

// Start an activity lasting about `bars` bars, ending on a whole beat of the music (not on the bar line: everyone's
// changes land at different moments).
void Mingle::enter(Agent& a, Act act, double bars, double beat) {
    a.act = act;
    const double toBeat = std::ceil(beat) - beat;
    // On a beat or the off-beat between (fewer people changing in the very same instant).
    a.until = clock_ + toBeat + std::max(2.0, std::round(bars * 4.0)) + (rnd(a.rng) < 0.4f ? 0.5 : 0.0);
    if (act != Act::Seek) a.target = act == Act::Watch ? a.target : 0;
    if (act != Act::Seek) a.targetGroup = 0;
}

void Mingle::leaveGroup(Agent& a, double beat) {
    Group* g = findGroup(a.group);
    a.group = 0;
    if (!g) return;
    g->members.erase(std::remove(g->members.begin(), g->members.end(), a.id), g->members.end());
    if (g->members.size() == 1) {
        // The last one stays a moment, then moves on themselves.
        if (Agent* o = find(g->members[0])) {
            o->group = 0;
            o->sx = o->x;
            o->sz = o->z;
            enter(*o, Act::Solo, rndIn(o->rng, 0.75f, 2.5f), beat);
        }
        g->members.clear();
    }
    if (g->members.empty()) {
        const int id = g->id;
        groups_.erase(std::remove_if(groups_.begin(), groups_.end(), [&](const Group& q) { return q.id == id; }),
                      groups_.end());
    } else {
        layout(*g);
    }
}

// Places round the group: a pair side by side (tilted as they met), 3-4 in a horseshoe open to the audience.
// Members keep their order round the center, so nobody crosses through the middle.
void Mingle::layout(Group& g) {
    std::vector<std::pair<float, std::uint64_t>> order;
    for (const auto id : g.members) {
        const Agent* a = find(id);
        const float ang = a ? std::atan2(a->x - g.cx, -(a->z - g.cz)) : 0.0f;
        order.push_back({ang, id});
    }
    std::sort(order.begin(), order.end());
    if (order.size() == 2 && g.kind == Kind::Circle) g.kind = Kind::Facing;  // down to a pair
    g.members.clear();
    for (const auto& o : order) g.members.push_back(o.second);
    if (g.members.size() == 2) {
        const Agent* a = find(g.members[0]);
        const Agent* b = find(g.members[1]);
        if (a && b) {
            float t = std::atan2(b->z - a->z, b->x - a->x);
            if (t > 1.5708f) t -= 3.14159265f;
            if (t < -1.5708f) t += 3.14159265f;
            g.tilt = std::clamp(t, -0.6f, 0.6f);
            if (b->x < a->x) std::swap(g.members[0], g.members[1]);
        }
    }
}

float Mingle::clearPad(const Agent& a) const {
    // The air kept round a floor move's reach: more the longer they wait for room (Peter, 2026-11-27: never give up,
    // clear it more firmly): up to 0.35 more after about 9 s.
    return kFloorClear + std::min(0.35f, 0.04f * a.waitT);
}

bool Mingle::mayGoDown(const Agent& a) const {
    // Never while walking: on their own spot, showing off, busting a move, or in a pair or group (the others make room
    // and watch: Peter, 2026-11-27); never inside the room someone else on the floor has (they step out first).
    if (!a.anchored)
        for (const auto& o : agents_)
            if (o.id != a.id && o.anchored &&
                (std::hypot(a.x - o.ax, a.z - o.az) < floorNeed(o.reach, o.floorFacing, a.x - o.ax, a.z - o.az, kFloorClear) ||
                 (a.floor && std::hypot(a.x - o.ax, a.z - o.az) < floorNeed(a.reach, 0, o.ax - a.x, o.az - a.z, kFloorKeep))))
                return false;
    if (a.bustMove) return true;
    if (a.act == Act::Solo || a.act == Act::Show) return true;
    if (a.act != Act::Group) return false;
    for (const auto& g : groups_) {
        if (g.id != a.group) continue;
        if (g.floorId) return g.floorId == a.id;  // (one at a time in a group: whoever has waited longest next)
        for (const auto& m : agents_)
            if (m.id != a.id && m.group == a.group && m.floor == 1 && m.wantT > a.wantT) return false;
        return true;
    }
    return false;
}

float Mingle::pickFloorFacing(const Agent& a) const {
    // The turn with the most room for the move's reach (the neighbors inside it, by how far), towards the audience
    // when it's close; a head-first move (face down towards the front) hardly turned away from the camera.
    const float most = a.reach.headFirst ? 0.25f : 0.6f;
    float best = 0, bestScore = -1e9f;
    for (int i = -4; i <= 4; ++i) {
        const float f = most * float(i) / 4.0f;
        float inside = 0;
        for (const auto& o : agents_) {
            if (o.id == a.id) continue;
            const float dx = o.x - a.x, dz = o.z - a.z;
            inside += std::max(0.0f, floorNeed(a.reach, f, dx, dz, kFloorClear) - std::hypot(dx, dz));
        }
        const float score = -inside - 0.15f * std::fabs(f) + (i == 0 ? 0.01f : 0.0f);
        if (score > bestScore) {
            bestScore = score;
            best = f;
        }
    }
    return best;
}

void Mingle::decide(Agent& a, const StageCamera& cam, double beat) {
    const float soc = sociable(a), rest = kPersonality.restless * (a.vibe == MingleVibe::Crew ? 0.3f : 1.0f);
    if (a.group) {
        // Stay: Crew always, with whoever they picked, and (mostly) with someone who picked them.
        bool stay = a.vibe == MingleVibe::Crew;
        if (const Group* g = findGroup(a.group)) {
            for (const auto id : g->members) {
                if (id == a.id) continue;
                const Agent* m = find(id);
                if (id == a.with) stay = true;
                if (m && m->with == a.id && a.vibe != MingleVibe::Solo && rnd(a.rng) < 0.8f) stay = true;
            }
        }
        if (stay) {
            a.until = clock_ + (std::ceil(beat) - beat) + std::round(rndIn(a.rng, 4.0f, 8.0f) * 4.0f);
            return;
        }
        // Time to move on: wander off.
        leaveGroup(a, beat);
        const auto s = pickSpot(a, cam, false);
        a.sx = s.first;
        a.sz = s.second;
        enter(a, Act::Wander, 4, beat);
        return;
    }
    if (a.approachedBy) {  // someone is on their way over: wait for them
        a.until = clock_ + 2;
        return;
    }
    // Who could be joined, watched.
    std::vector<std::pair<float, Agent*>> free, showing;
    const bool crew = a.vibe == MingleVibe::Crew, solo = a.vibe == MingleVibe::Solo;
    for (auto& o : agents_) {
        if (o.id == a.id || o.approachedBy || (o.vibe == MingleVibe::Solo && !o.bustMove)) continue;
        if (o.floor || o.bustMove) continue;  // (going down or on the floor: nobody walks over)
        const float d = std::hypot(o.x - a.x, o.z - a.z);
        if (d > (crew ? 1e9f : 2.4f)) continue;
        if (!o.bustMove && (o.act == Act::Solo || o.act == Act::Show || o.act == Act::Wander || o.act == Act::Rest))
            free.push_back({(0.4f + kPersonality.sociable) / (0.3f + d), &o});
        if (o.act == Act::Show) {
            int watchers = 0;
            for (const auto& w : agents_) watchers += w.act == Act::Watch && w.target == o.id;
            if (watchers < 2) showing.push_back({1.0f / (0.3f + d), &o});
        }
    }
    std::vector<std::pair<float, Group*>> open;
    for (auto& g : groups_) {
        std::size_t pending = 0;
        for (const auto& w : agents_) pending += w.act == Act::Seek && w.targetGroup == g.id;
        if (g.members.size() + pending >= kMaxGroup || g.floorId) continue;
        const float d = std::hypot(g.cx - a.x, g.cz - a.z);
        // Crew: the biggest group with room, anywhere (the nearest of those).
        if (crew) open.push_back({float(g.members.size()) * 100.0f - d, &g});
        else if (d < 2.6f) open.push_back({1.0f / (0.4f + d), &g});
    }
    if (crew && !solo) {
        if (!open.empty()) {
            a.targetGroup = std::max_element(open.begin(), open.end(),
                                             [](const auto& x, const auto& y) { return x.first < y.first; })->second->id;
            a.target = 0;
            enter(a, Act::Seek, 8, beat);
            return;
        }
        if (!free.empty()) {  // nobody in a group yet: the nearest (free holds closeness x sociability)
            Agent* o = std::max_element(free.begin(), free.end(),
                                        [](const auto& x, const auto& y) { return x.first < y.first; })->second;
            a.target = o->id;
            o->approachedBy = a.id;
            if (o->act == Act::Wander) {
                o->sx = o->x;
                o->sz = o->z;
                o->act = Act::Solo;
            }
            enter(a, Act::Seek, 8, beat);
            return;
        }
    }
    auto pickOne = [&](auto& v) {
        float sum = 0;
        for (const auto& c : v) sum += c.first;
        float r = rnd(a.rng) * sum;
        for (const auto& c : v)
            if ((r -= c.first) <= 0) return c.second;
        return v.back().second;
    };
    const float wSeek = free.empty() || solo ? 0 : 0.25f + 1.1f * soc;
    const float wJoin = open.empty() || solo ? 0 : 0.15f + 0.6f * soc;
    const float wWatch = showing.empty() || solo ? 0 : 0.3f + 0.6f * soc;
    const float wWander = 0.3f + 0.7f * rest;
    const float wRest = 0.12f * (1.3f - rest);
    const float wShow = 0.08f + 0.45f * kPersonality.showy;
    const float wSolo = 0.3f;
    float r = rnd(a.rng) * (wSeek + wJoin + wWatch + wWander + wRest + wShow + wSolo);
    const float stay = 1.35f - 0.7f * rest;  // (Crew: less restless, they stay longer)
    if ((r -= wSeek) < 0) {
        Agent* o = pickOne(free);
        a.target = o->id;
        o->approachedBy = a.id;
        if (o->act == Act::Wander) {  // stops to wait
            o->sx = o->x;
            o->sz = o->z;
            o->act = Act::Solo;
        }
        // They notice (a glance their way).
        o->glanceAt = a.id;
        o->glanceUntil = t_ + 1.5;
        enter(a, Act::Seek, 5, beat);
        return;
    }
    if ((r -= wJoin) < 0) {
        a.targetGroup = pickOne(open)->id;
        enter(a, Act::Seek, 5, beat);
        a.target = 0;
        return;
    }
    if ((r -= wWatch) < 0) {
        Agent* o = pickOne(showing);
        a.target = o->id;
        // Stand in front of them, a little to the side we come from.
        const float side = a.x >= o->x ? 1.0f : -1.0f;
        a.sx = o->x + side * kGroupGap;
        a.sz = std::clamp(o->z + 0.18f, cam.floorBack, kFloorZMax);
        enter(a, Act::Watch, rndIn(a.rng, 1.5f, 4.0f), beat);
        return;
    }
    if ((r -= wWander) < 0) {
        const auto s = pickSpot(a, cam, false);
        a.sx = s.first;
        a.sz = s.second;
        enter(a, Act::Wander, 4, beat);
        return;
    }
    if ((r -= wRest) < 0) {
        const auto s = pickSpot(a, cam, true);
        a.sx = s.first;
        a.sz = s.second;
        enter(a, Act::Rest, rndIn(a.rng, 3.0f, 7.0f), beat);
        return;
    }
    a.sx = a.x;
    a.sz = a.z;
    a.tilt = 0.45f * (rnd(a.rng) - 0.5f);
    if ((r -= wShow) < 0) {
        enter(a, Act::Show, rndIn(a.rng, 2.0f, 4.0f), beat);
        return;
    }
    enter(a, Act::Solo, rndIn(a.rng, 2.0f, 6.0f) * stay, beat);
}

bool Mingle::keepWith(Agent& a, const StageCamera& cam, double beat) {
    auto dropSeek = [&]() {
        if (Agent* o = a.target ? find(a.target) : nullptr; o && o->approachedBy == a.id) o->approachedBy = 0;
        a.target = 0;
        a.targetGroup = 0;
    };
    Agent* b = a.with ? find(a.with) : nullptr;
    if (!b) {
        // Solo: out of any group, not going over to anyone.
        if (a.vibe != MingleVibe::Solo || !(a.group || a.act == Act::Seek || a.act == Act::Watch)) return false;
        if (a.group) leaveGroup(a, beat);
        dropSeek();
        a.sx = a.x;
        a.sz = a.z;
        enter(a, Act::Solo, rndIn(a.rng, 1.0f, 3.0f), beat);
        return true;
    }
    // They want to be alone (and didn't pick us): dance near them, watching, and follow them round.
    if (b->vibe == MingleVibe::Solo && b->with != a.id) {
        const bool already = a.act == Act::Watch && a.target == b->id;
        if (!already) {
            if (a.group) leaveGroup(a, beat);
            dropSeek();
            a.target = b->id;
            a.nearSide = a.x >= b->x ? 1.0f : -1.0f;  // kept while near them (no flipping as they cross over)
        }
        // Everyone near them has their own place round them: either side first, then further out in front (behind
        // when they are at the front edge). By id, so the places hold still.
        int k = 0;
        for (const auto& o : agents_)
            if (o.id != a.id && o.with == b->id && o.id < a.id && !(b->with == o.id)) ++k;
        static constexpr float kNear[][2] = {{1.0f, 0.12f}, {-1.0f, 0.12f}, {0.62f, 0.62f}, {-0.62f, 0.62f},
                                             {1.6f, 0.45f}, {-1.6f, 0.45f}};
        const auto& slot = kNear[std::size_t(k) % 6];
        const float depth = b->z + 0.7f > kFloorZMax ? -1.0f : 1.0f;
        a.sx = b->x + a.nearSide * slot[0] * kGroupGap;
        a.sz = std::clamp(b->z + depth * slot[1] * kGroupGap, cam.floorBack, kFloorZMax);
        if (!already) enter(a, Act::Watch, 8, beat);
        a.until = std::max(a.until, clock_ + 8);
        return !already;
    }
    // With them already, or on the way.
    if (a.group && a.group == b->group) return false;
    if (b->anchored || b->bustMove) return false;  // (going down / on the floor: wait for them)
    if (a.act == Act::Seek && (a.target == b->id || (b->group && a.targetGroup == b->group))) return false;
    // Go over to them (or their group).
    if (a.group) leaveGroup(a, beat);
    dropSeek();
    if (b->group) {
        a.targetGroup = b->group;
    } else {
        a.target = b->id;
        if (!b->approachedBy) b->approachedBy = a.id;
        b->glanceAt = a.id;
        b->glanceUntil = t_ + 1.5;
    }
    enter(a, Act::Seek, 16, beat);
    return false;  // (the Seek below starts walking this frame)
}

std::vector<StageSpot> Mingle::plan(const std::vector<MinglePerson>& people, const StageCamera& cam, double beat,
                                    double dt, std::uint32_t seed) {
    // Our own clock in beats: follows the music, but a jump (a new track, the clock re-locking) doesn't strand
    // everyone's schedule.
    if (!started_) {
        lastBeat_ = beat;
        started_ = true;
    }
    clock_ += std::clamp(beat - lastBeat_, 0.0, 0.5);
    lastBeat_ = beat;
    t_ += std::clamp(dt, 0.0, 0.1);

    // Who is here.
    for (auto& a : agents_) a.seen = false;
    for (const auto& p : people) {
        Agent* a = find(p.id);
        if (!a) {
            agents_.push_back({});
            a = &agents_.back();
            a->id = p.id;
            a->rng = mix32(seed ^ std::uint32_t(p.id * 0x9e3779b97f4a7c15ULL >> 32)) | 1u;
            a->x = p.x;
            a->z = p.z;
            a->outX = p.x;
            a->outZ = p.z;
            if (!p.placed) {
                const auto s = pickSpot(*a, cam, false);
                a->x = a->outX = s.first;
                a->z = a->outZ = s.second;
            }
            a->sx = a->x;
            a->sz = a->z;
            a->tilt = 0.45f * (rnd(a->rng) - 0.5f);
            a->nextGlance = t_ + rndIn(a->rng, 1.0f, 4.0f);
            a->react = rndIn(a->rng, 0.3f, 1.1f);
            enter(*a, Act::Solo, rndIn(a->rng, 0.5f, 3.0f), beat);  // dance a moment where they popped in
        }
        a->seen = true;
        if (p.placed) {
            a->x = p.x;
            a->z = p.z;
        }
        a->facing = p.facing;
        a->placed = p.placed;
        a->vibe = p.bustMove ? MingleVibe::Solo : p.vibe;
        a->with = p.with != p.id && !p.bustMove ? p.with : 0;
        a->bustMove = p.bustMove;
        a->style = p.style;
        a->floor = p.bustMove ? std::max(1, p.floor) : p.floor;
        if (a->floor) a->reach = p.reach;
    }
    // Gone: out of their group; whoever was walking over to or watching them changes plans.
    for (auto& a : agents_)
        if (!a.seen && a.group) leaveGroup(a, beat);
    std::vector<std::uint64_t> gone;
    for (const auto& a : agents_)
        if (!a.seen) gone.push_back(a.id);
    if (!gone.empty()) {
        agents_.erase(std::remove_if(agents_.begin(), agents_.end(), [](const Agent& a) { return !a.seen; }),
                      agents_.end());
        auto isGone = [&](std::uint64_t id) { return id && std::find(gone.begin(), gone.end(), id) != gone.end(); };
        for (auto& a : agents_) {
            if (isGone(a.approachedBy)) a.approachedBy = 0;
            if (isGone(a.glanceAt)) a.glanceAt = 0, a.glanceUntil = 0;
            if ((a.act == Act::Seek || a.act == Act::Watch) && isGone(a.target)) {
                a.target = 0;
                a.until = clock_;  // decide again now
            }
        }
    }

    // Each on their own schedule.
    for (std::size_t i = 0; i < agents_.size(); ++i) {
        Agent& a = agents_[i];
        // Floor work (going down or on the floor, busting a move): held where they are, no new plans until it's over.
        if (a.floor && a.act == Act::Seek) {  // (stops walking over; never while walking)
            if (Agent* o = a.target ? find(a.target) : nullptr; o && o->approachedBy == a.id) o->approachedBy = 0;
            a.target = 0;
            a.targetGroup = 0;
            a.sx = a.x;
            a.sz = a.z;
            enter(a, Act::Solo, 1, beat);
        }
        if (a.floor && a.approachedBy) {  // whoever was walking over waits elsewhere
            if (Agent* o = find(a.approachedBy); o && o->act == Act::Seek && o->target == a.id) {
                o->target = 0;
                o->sx = o->x;
                o->sz = o->z;
                enter(*o, Act::Solo, rndIn(o->rng, 1.0f, 3.0f), beat);
            }
            a.approachedBy = 0;
        }
        // Someone else going down or on the floor in the way (they won't be up for a while; of two waiting, the later
        // one): not held here; Mingle walks them out of the way first (the last pass below), then holds them again.
        if (a.anchored && a.floor == 1)
            for (const auto& o : agents_)
                if (o.id != a.id && o.anchored && (o.floor == 2 || o.id < a.id)) {
                    const float dx = o.ax - a.ax, dz = o.az - a.az;
                    if (std::hypot(dx, dz) < floorNeed(a.reach, a.floorFacing, dx, dz, kFloorKeep)) {
                        a.anchored = false;
                        if (Group* g = a.group ? findGroup(a.group) : nullptr; g && g->floorId == a.id) g->floorId = 0;
                        break;
                    }
                }
        // Their group's floor turn taken (someone else is down, maybe for 16 bars): after 6 s they leave it to go down
        // on their own.
        if (a.floor == 1 && a.group && a.wantT > 6)
            if (const Group* g = findGroup(a.group); g && g->floorId && g->floorId != a.id) {
                leaveGroup(a, beat);
                a.sx = a.x;
                a.sz = a.z;
                enter(a, Act::Solo, 1, beat);
            }
        if (a.floor && !a.anchored && (mayGoDown(a) || a.floor == 2)) {
            a.anchored = true;
            a.ax = a.x;
            a.az = a.z;
            a.sx = a.x;
            a.sz = a.z;
            a.floorFacing = pickFloorFacing(a);
            if (a.group)
                if (Group* g = findGroup(a.group); g && !g->floorId) g->floorId = a.id;
        }
        if (!a.floor && a.anchored) {
            a.anchored = false;
            if (Group* g = a.group ? findGroup(a.group) : nullptr; g && g->floorId == a.id) g->floorId = 0;
            a.sx = a.x;
            a.sz = a.z;
        }
        if (a.bustMove && !a.group && a.act != Act::Show) {
            // Busting a move on their own: showing off where they are.
            a.sx = a.x;
            a.sz = a.z;
            enter(a, Act::Show, 8, beat);
        }
        if (a.floor) {
            if (a.act == Act::Wander && !a.anchored) {
                // (stops where they are: going down never while walking)
                a.sx = a.x;
                a.sz = a.z;
                enter(a, Act::Solo, 1, beat);
                continue;
            } else {
                if (a.act == Act::Rest || a.act == Act::Watch || a.act == Act::Wander || a.act == Act::Seek) {  // dances where they are
                    a.sx = a.x;
                    a.sz = a.z;
                    a.target = 0;
                    enter(a, Act::Solo, 1, beat);
                }
                a.until = std::max(a.until, clock_ + 1);
                continue;
            }
        }
        if (a.group)  // the group stays round someone going down until they're up again
            if (const Group* g = findGroup(a.group); g && g->floorId) a.until = std::max(a.until, clock_ + 1);
        if (keepWith(a, cam, beat)) continue;
        if (a.act == Act::Seek) {
            Agent* o = a.target ? find(a.target) : nullptr;
            Group* g = a.targetGroup ? findGroup(a.targetGroup) : nullptr;
            if (o && (o->anchored || o->floor)) {  // going down: not now
                if (o->approachedBy == a.id) o->approachedBy = 0;
                a.target = 0;
                o = nullptr;
            }
            if (g && g->floorId) g = nullptr;
            if (o && o->vibe == MingleVibe::Solo && a.with != o->id) {  // they want to be alone now
                if (o->approachedBy == a.id) o->approachedBy = 0;
                a.target = 0;
                o = nullptr;
            }
            const std::size_t cap = a.with ? kMaxGroupWith : kMaxGroup;
            if (o && o->group) {  // they got together with someone else first: join them instead
                g = findGroup(o->group);
                if (o->approachedBy == a.id) o->approachedBy = 0;
                a.target = 0;
                a.targetGroup = g && g->members.size() < cap ? g->id : 0;
                o = nullptr;
                if (!a.targetGroup) g = nullptr;
            }
            if (!o && !g) {
                enter(a, Act::Solo, rndIn(a.rng, 1.0f, 3.0f), beat);
                a.sx = a.x;
                a.sz = a.z;
                continue;
            }
            if (o) {
                // Meet them at arm's length, from the side we come from.
                float dx = a.x - o->x, dz = a.z - o->z;
                const float d = std::hypot(dx, dz);
                if (d > 1e-3f) dx /= d, dz /= d;
                else dx = 1, dz = 0;
                a.sx = o->x + dx * kGroupGap;
                a.sz = std::clamp(o->z + dz * kGroupGap, cam.floorBack, kFloorZMax);
                // (Not while they are on their way to someone else, unless it is us.)
                if (d < kGroupGap + 0.14f && (o->act != Act::Seek || o->target == a.id)) {
                    Group ng;
                    ng.id = nextGroup_++;
                    ng.members = {a.id, o->id};
                    ng.start = clock_;
                    ng.cx = (a.x + o->x) * 0.5f;
                    ng.cz = std::clamp((a.z + o->z) * 0.5f, cam.floorBack + 0.1f, kFloorZMax - 0.1f);
                    ng.rng = mix32(a.rng ^ o->rng ^ std::uint32_t(ng.id)) | 1u;
                    ng.phase = 6.2831853f * rnd(ng.rng);
                    const float k = rnd(ng.rng);
                    ng.kind = k < 0.35f ? Kind::Partners : k < 0.7f ? Kind::Facing : Kind::Call;
                    groups_.push_back(ng);
                    for (Agent* m : {&a, o}) {
                        m->group = ng.id;
                        m->approachedBy = 0;
                        m->target = 0;
                        m->targetGroup = 0;
                        m->act = Act::Group;
                        const double bars = 4 + 12 * rnd(m->rng) * (0.4f + 0.6f * sociable(*m));
                        m->until = clock_ + (std::ceil(beat) - beat) + std::round(bars * 4);
                    }
                    layout(groups_.back());
                    continue;
                }
            } else {
                const float r = groupRadius(g->members.size() + 1);
                float dx = a.x - g->cx, dz = a.z - g->cz;
                const float d = std::hypot(dx, dz);
                if (d > 1e-3f) dx /= d, dz /= d;
                else dx = 0, dz = -1;
                a.sx = g->cx + dx * r;
                a.sz = std::clamp(g->cz + dz * r, cam.floorBack, kFloorZMax);
                if (d < r + 0.25f) {
                    g->members.push_back(a.id);
                    a.group = g->id;
                    a.targetGroup = 0;
                    a.act = Act::Group;
                    const double bars = 3 + 10 * rnd(a.rng) * (0.4f + 0.6f * sociable(a));
                    a.until = clock_ + (std::ceil(beat) - beat) + std::round(bars * 4);
                    if (g->members.size() >= 3) g->kind = Kind::Circle;
                    layout(*g);
                    continue;
                }
            }
            if (clock_ >= a.until) {  // couldn't get there: give up
                if (o && o->approachedBy == a.id) o->approachedBy = 0;
                a.target = 0;
                a.targetGroup = 0;
                a.sx = a.x;
                a.sz = a.z;
                enter(a, Act::Solo, rndIn(a.rng, 1.0f, 3.0f), beat);
            }
            continue;
        }
        if (a.act == Act::Wander && std::hypot(a.x - a.sx, a.z - a.sz) < 0.12f) {
            a.tilt = 0.45f * (rnd(a.rng) - 0.5f);
            enter(a, Act::Solo, rndIn(a.rng, 1.0f, 4.0f) * (1.35f - 0.7f * kPersonality.restless), beat);
            continue;
        }
        if (a.act == Act::Watch && a.target != a.with) {
            const Agent* o = find(a.target);
            if (!o || o->act != Act::Show) a.until = std::min(a.until, clock_);
        }
        if (clock_ >= a.until) decide(a, cam, beat);
    }

    // Groups: their own clocks. A pair may change how it dances at its own 8-bar marks; groups drift a little.
    for (auto& g : groups_) {
        if (g.floorId) {  // (the one going down left, or is up again)
            const Agent* f = find(g.floorId);
            if (!f || f->group != g.id || !f->anchored) g.floorId = 0;
        }
        const long block = long(std::floor((clock_ - g.start) / 32.0));
        if (block != g.kindBar) {
            g.kindBar = block;
            if (g.members.size() == 2 && block > 0 && rnd(g.rng) < 0.35f) {
                const float k = rnd(g.rng);
                g.kind = k < 0.35f ? Kind::Partners : k < 0.7f ? Kind::Facing : Kind::Call;
            }
            if (g.members.size() >= 3) g.kind = Kind::Circle;
            if (g.members.size() == 2 && g.kind == Kind::Circle) g.kind = Kind::Facing;
        }
    }

    // Glances (seconds, not beats).
    const float fdt = float(std::clamp(dt, 0.0, 0.1));
    // The room kept round someone going down: grows quickly, shrinks over a couple of seconds as they get up (the
    // neighbors drift back).
    for (auto& a : agents_) {
        // Waiting for room (anchored, not down yet): the room cleared more firmly, and every 2 s the turn looked at
        // again (within its limits) while they are still standing.
        a.wantT = a.floor == 1 ? a.wantT + fdt : 0.0f;
        if (a.anchored && a.floor == 1) {
            a.waitT += fdt;
            if (std::floor(a.waitT / 2) != std::floor((a.waitT - fdt) / 2)) a.floorFacing = pickFloorFacing(a);
            // Still no room after 4 s (someone boxed in by the edge of the floor, or by others): they themselves
            // step away from whoever is in the way, slowly (at most 0.3 units/s), before going down.
            if (a.waitT > 4 && !a.clearNow) {
                float px = 0, pz = 0;
                for (const auto& o : agents_) {
                    if (o.id == a.id) continue;
                    const float dx = a.ax - o.x, dz = a.az - o.z, d = std::max(1e-3f, std::hypot(dx, dz));
                    const float in = floorNeed(a.reach, a.floorFacing, -dx, -dz, kFloorKeep) + 0.05f - d;
                    if (in > 0) px += dx / d * in, pz += dz / d * in;
                }
                const float len = std::hypot(px, pz), most = 0.3f * fdt;
                if (len > most) px *= most / len, pz *= most / len;
                a.ax += px;
                a.az = std::clamp(a.az + pz, cam.floorBack, kFloorZMax);
                const float wx = float(cam.halfWidth) * 0.92f * floorWiden(cam, a.az);
                a.ax = std::clamp(a.ax, -wx, wx);
                a.sx = a.ax;
                a.sz = a.az;
            }
        } else {
            a.waitT = 0;
        }
        const float target = a.anchored ? 1.0f : 0.0f;
        a.room += (target - a.room) * std::min(1.0f, fdt / (target > a.room ? 0.3f : 2.0f));
        if (target == 0 && a.room < 0.02f) a.room = 0;
    }
    for (auto& a : agents_) {
        if (a.act == Act::Watch || a.act == Act::Seek) {
            a.glanceAt = a.target;
        } else if (t_ >= a.glanceUntil) {
            a.glanceAt = 0;
            if (t_ >= a.nextGlance) {
                std::vector<std::pair<float, std::uint64_t>> cands;
                for (const auto& o : agents_) {
                    if (o.id == a.id || (a.group && o.group == a.group)) continue;
                    const float d = std::hypot(o.x - a.x, o.z - a.z);
                    if (d > 2.6f) continue;
                    const float rel = std::remainder(std::atan2(o.x - a.x, o.z - a.z) - a.facing, 6.2831853f);
                    if (std::fabs(rel) > 1.6f) continue;  // behind them
                    float w = 1.0f / (0.4f + d);
                    if (o.act == Act::Show) w *= 3;
                    if (o.act == Act::Wander || o.act == Act::Seek) w *= 1.5f;
                    if (o.target == a.id) w *= 5;
                    cands.push_back({w, o.id});
                }
                if (!cands.empty()) {
                    float sum = 0;
                    for (const auto& c : cands) sum += c.first;
                    float r = rnd(a.rng) * sum;
                    a.glanceAt = cands.back().second;
                    for (const auto& c : cands)
                        if ((r -= c.first) <= 0) {
                            a.glanceAt = c.second;
                            break;
                        }
                    a.glanceUntil = t_ + rndIn(a.rng, 0.8f, 2.4f);
                }
                const float gap =
                    rndIn(a.rng, 1.5f, 6.0f) * (1.4f - 0.6f * kPersonality.sociable) * (a.group ? 2.0f : 1.0f);
                a.nextGlance = std::max(a.glanceUntil, t_) + gap;
            }
        }
    }

    // The spots, in the order asked.
    std::vector<StageSpot> out(people.size());
    auto indexOf = [&](std::uint64_t id) {
        for (std::size_t k = 0; k < people.size(); ++k)
            if (people[k].id == id) return int(k);
        return -1;
    };
    const float W = roomHalfWidth(cam);
    const float bar = float(clock_ / 4.0);
    for (std::size_t k = 0; k < people.size(); ++k) {
        Agent& a = *find(people[k].id);
        StageSpot& s = out[k];
        const float wob = float(a.id % 997) * 0.37f;
        auto alone = [&]() {
            // Dancing on their spot, drifting a little; mostly to the audience, turned their own way.
            s.x = a.sx + 0.05f * std::sin(bar * 0.45f + wob);
            s.z = a.sz + 0.04f * std::sin(bar * 0.33f + wob * 1.7f);
            s.facing = -s.x * 0.1f + a.tilt;
        };
        float baseLook = 0;
        switch (a.act) {
            case Act::Solo: alone(); s.role = "dancing"; break;
            case Act::Show: {
                alone();
                s.role = "showing off";
                // Half turned to whoever came to watch.
                for (const auto& w : agents_)
                    if (w.act == Act::Watch && w.target == a.id) {
                        s.facing = 0.5f * yawTowards(a.x, a.z, w.x, w.z);
                        s.other = indexOf(w.id);
                        break;
                    }
                break;
            }
            case Act::Wander:
                s.x = a.sx;
                s.z = a.sz;
                s.facing = -s.x * 0.1f + a.tilt;
                s.effort = 0.85f;
                s.role = "wandering";
                break;
            case Act::Rest:
                alone();
                s.effort = 0.55f;
                s.role = "taking a break";
                break;
            case Act::Seek: {
                s.x = a.sx;
                s.z = a.sz;
                s.effort = 0.85f;
                s.role = "going over to";
                if (a.target) s.other = indexOf(a.target);
                else if (const Group* g = findGroup(a.targetGroup); g && !g->members.empty())
                    s.other = indexOf(g->members[0]);
                break;
            }
            case Act::Watch: {
                s.x = a.sx;
                s.z = a.sz;
                const Agent* o = find(a.target);
                if (o) s.facing = yawTowards(a.x, a.z, o->x, o->z);
                s.react = true;
                s.other = indexOf(a.target);
                s.role = "watching";
                break;
            }
            case Act::Group: {
                Group* g = findGroup(a.group);
                if (!g) {
                    alone();
                    break;
                }
                const std::size_t n = g->members.size();
                const std::size_t j = std::size_t(
                    std::find(g->members.begin(), g->members.end(), a.id) - g->members.begin());
                const float r = groupRadius(n);
                // Each sways on their own (not the whole group sliding together).
                const float cx = std::clamp(g->cx + 0.05f * std::sin(bar * 0.4f + g->phase + wob), -W, W);
                const float cz = std::clamp(g->cz + 0.035f * std::sin(bar * 0.3f + g->phase * 1.3f + wob * 0.7f),
                                            cam.floorBack + r, kFloorZMax - 0.3f * r);
                float lx, lz;
                if (n == 2) {
                    const float side = j == 0 ? -1.0f : 1.0f;
                    lx = side * r * std::cos(g->tilt);
                    lz = side * r * std::sin(g->tilt);
                } else {
                    const float th = 1.85f, ang = -th + 2 * th * float(j) / float(n - 1);
                    lx = r * std::sin(ang);
                    lz = -r * std::cos(ang);
                }
                s.x = cx + lx;
                s.z = std::clamp(cz + lz, cam.floorBack, kFloorZMax);
                const Agent* mate = n == 2 ? find(g->members[1 - j]) : nullptr;
                const float tx = mate ? cx - lx : cx, tz = mate ? cz - lz : cz;
                s.facing = yawTowards(s.x, s.z, tx, tz);
                baseLook = 0.12f * (tx > s.x ? 1.0f : tx < s.x ? -1.0f : 0.0f);
                s.other = indexOf(g->members[(j + 1) % n]);
                // The group's own 2-bar turns.
                const long turn = long(std::floor((clock_ - g->start) / 8.0));
                switch (g->kind) {
                    case Kind::Partners:
                        // (Copying the other's routine only within a style: else face to face, each their own moves.
                        // Peter, 2026-11-27.)
                        if (mate && mate->style != a.style) {
                            s.role = "facing";
                            break;
                        }
                        s.leader = indexOf(g->members[0]);
                        s.mirror = j == 1;
                        s.role = "with";
                        break;
                    case Kind::Call: {
                        const std::size_t caller = std::size_t(turn % long(n));
                        if (j == caller) {
                            s.facing *= 0.5f;
                            s.role = "showing";
                        } else {
                            s.react = true;
                            s.other = indexOf(g->members[caller]);
                            s.role = "watching";
                        }
                        break;
                    }
                    case Kind::Circle: {
                        // Now and then one solos (half to the audience) and the rest cheer; otherwise all dance to
                        // each other.
                        const bool soloing = unit32(g->rng ^ std::uint32_t(turn) * 0x27d4eb2fU) < 0.45f;
                        const std::size_t solo = std::size_t(mix32(std::uint32_t(turn) ^ g->rng) % n);
                        if (soloing && j == solo) {
                            s.facing *= 0.4f;
                            s.role = "soloing in a circle with";
                        } else if (soloing) {
                            s.react = true;
                            s.other = indexOf(g->members[solo]);
                            s.role = "cheering";
                        } else {
                            s.role = "in a circle with";
                        }
                        break;
                    }
                    default: s.role = "facing"; break;
                }
                break;
            }
        }
        // Floor work: held where they went down, turned as they were given.
        if (a.anchored) {
            s.x = a.ax;
            s.z = a.az;
            s.facing = a.floorFacing;
            s.leader = -1;
            s.mirror = false;
            s.react = false;
            s.role = a.floor == 2 ? "on the floor" : "going down";
        } else if (const Group* g = a.act == Act::Group ? findGroup(a.group) : nullptr; g && g->floorId) {
            // The rest of the group make room round them and watch, dancing as watchers do (watchPose: a basic of
            // their own style): an arc open to the audience at the move's reach (a partner mirroring them:
            // kFollowerMirrorsFloor).
            const Agent* f = find(g->floorId);
            const bool copy = kFollowerMirrorsFloor && g->kind == Kind::Partners && !g->members.empty() &&
                              g->members[0] == g->floorId && f && f->style == a.style;
            if (f && f->anchored && !copy) {
                std::vector<std::pair<float, std::uint64_t>> order;
                for (const auto id : g->members)
                    if (id != f->id)
                        if (const Agent* m = find(id)) order.push_back({std::atan2(m->x - f->ax, -(m->z - f->az)), id});
                std::sort(order.begin(), order.end());
                const std::size_t mcount = order.size();
                std::size_t j = 0;
                while (j < mcount && order[j].second != a.id) ++j;
                float ang;
                if (mcount == 1) ang = order[0].first >= 0 ? 1.0f : -1.0f;
                else {
                    const float th = std::min(1.5f, 0.55f * float(mcount));
                    ang = -th + 2 * th * float(j) / float(mcount - 1);
                }
                const float dx = std::sin(ang), dz = -std::cos(ang);
                const float r = floorNeed(f->reach.scaled(std::max(f->room, 0.3f)), f->floorFacing, dx, dz, clearPad(*f) + 0.05f);
                s.x = f->ax + dx * r;
                s.z = std::clamp(f->az + dz * r, cam.floorBack, kFloorZMax);
                s.facing = yawTowards(s.x, s.z, f->ax, f->az);
                s.leader = -1;
                s.mirror = false;
                s.react = true;  // (dancing a basic of their own style as they watch: watchPose)
                s.other = indexOf(f->id);
                s.role = "making room for";
            }
        }
        // The head: a glance at someone, or the group's own look; eased.
        float want = baseLook;
        if (const Agent* o = a.glanceAt && a.act != Act::Watch ? find(a.glanceAt) : nullptr) {
            const float rel = std::remainder(std::atan2(o->x - a.x, o->z - a.z) - a.facing, 6.2831853f);
            want = std::clamp(rel * 0.85f, -0.85f, 0.85f);
        } else if (a.act == Act::Watch) {
            want = 0.15f * (s.facing > 0 ? 1.0f : -1.0f);
        }
        a.look += (want - a.look) * std::min(1.0f, fdt * 5.0f);
        s.look = a.look;
        // In a group or near someone: follow their place with their own lag (Peter, 2026-11-27: groups shifted in
        // unison), easing towards it over about `react` seconds; each lag different, so a group re-forming or
        // drifting doesn't move as one.
        if ((a.act == Act::Group || a.act == Act::Watch) && !a.anchored) {
            if (!a.held) {  // just got here: the place itself
                a.held = true;
                a.hx = s.x;
                a.hz = s.z;
            }
            const float k = 1.0f - std::exp(-fdt / a.react);
            a.hx += (s.x - a.hx) * k;
            a.hz += (s.z - a.hz) * k;
            s.x = a.hx;
            s.z = a.hz;
        } else {
            a.held = false;
        }
        const float wx = float(cam.halfWidth) * 0.92f * floorWiden(cam, s.z);
        s.x = std::clamp(s.x, -wx, wx);
        s.z = std::clamp(s.z, cam.floorBack, kFloorZMax);
        a.outX = s.x;
        a.outZ = s.z;
        if (s.other == int(k)) s.other = -1;
        if (s.leader == int(k)) s.leader = -1;
    }
    // No two spots closer than dancers may stand (Peter, 2026-11-27: jitter): otherwise both walk to their spots, get
    // pushed apart (keepApart), and walk back, every frame. A few rounds of easing them apart, half each; the spots
    // move smoothly as everyone does, so the result does too.
    const float gap = kMinApart + 0.04f;
    // Away from someone on the floor (unit ux, uz); where the front or back of the floor stops them, sideways, and
    // where a side stops them, forward or back.
    auto pushOut = [&](StageSpot& sp, float ux, float uz, float push) {
        const float nx = sp.x + ux * push, nz = sp.z + uz * push;
        const float wx = float(cam.halfWidth) * 0.92f * floorWiden(cam, std::clamp(nz, cam.floorBack, kFloorZMax));
        const float overZ = nz > kFloorZMax ? nz - kFloorZMax : nz < cam.floorBack ? cam.floorBack - nz : 0.0f;
        const float overX = std::fabs(nx) > wx ? std::fabs(nx) - wx : 0.0f;
        sp.x = nx;
        sp.z = nz;
        if (overZ > 0) sp.x += (std::fabs(ux) > 0.05f ? (ux > 0 ? 1.0f : -1.0f) : (sp.x >= 0 ? -1.0f : 1.0f)) * overZ * 1.5f;
        if (overX > 0) sp.z += (std::fabs(uz) > 0.05f ? (uz > 0 ? 1.0f : -1.0f) : (sp.z > -0.3f ? -1.0f : 1.0f)) * overX * 1.5f;
    };
    for (int round = 0; round < 4; ++round) {
        bool moved = false;
        for (std::size_t i = 0; i < out.size(); ++i)
            for (std::size_t j = i + 1; j < out.size(); ++j) {
                float dx = out[j].x - out[i].x, dz = out[j].z - out[i].z;
                const float d = std::hypot(dx, dz);
                // Room round someone going down or on the floor (their reach that way; eased as they get up): they
                // stay, the other steps back.
                const Agent* ai = find(people[i].id);
                const Agent* aj = find(people[j].id);
                const bool fi = ai && ai->room > 0, fj = aj && aj->room > 0;
                const bool hi = ai && ai->anchored, hj = aj && aj->anchored;  // (held where they went down)
                float need = gap;
                if (fi) need = std::max(need, floorNeed(ai->reach.scaled(ai->room), ai->floorFacing, dx, dz, clearPad(*ai)));
                if (fj) need = std::max(need, floorNeed(aj->reach.scaled(aj->room), aj->floorFacing, -dx, -dz, clearPad(*aj)));
                if (d >= need) continue;
                if (d < 1e-4f) {
                    dx = people[i].id < people[j].id ? 1.0f : -1.0f;  // on the same spot: apart sideways
                    dz = 0;
                } else {
                    dx /= d;
                    dz /= d;
                }
                const float push = need - d;
                if (hi != hj) {
                    pushOut(hi ? out[j] : out[i], hi ? dx : -dx, hi ? dz : -dz, push);
                } else if (hi) {
                    // (Two on the floor: neither moves; Mingle lets nobody go down in someone's room.)
                } else {
                    const float half = 0.5f * push;  // (as before: standing crowds unchanged to the bit)
                    out[i].x -= dx * half;
                    out[i].z -= dz * half;
                    out[j].x += dx * half;
                    out[j].z += dz * half;
                }
                moved = true;
            }
        for (auto& sp : out) {
            const float wx = float(cam.halfWidth) * 0.92f * floorWiden(cam, sp.z);
            sp.x = std::clamp(sp.x, -wx, wx);
            sp.z = std::clamp(sp.z, cam.floorBack, kFloorZMax);
        }
        if (!moved) break;
    }
    // Last: everyone out of a floor dancer's room (a standing pair a little close is keepApart's to settle).
    for (int round = 0; round < 3; ++round)
        for (std::size_t i = 0; i < out.size(); ++i) {
            const Agent* f = find(people[i].id);
            if (!f || !f->anchored || f->room <= 0) continue;
            for (std::size_t j = 0; j < out.size(); ++j) {
                const Agent* o = find(people[j].id);
                if (j == i || (o && o->anchored)) continue;
                float dx = out[j].x - f->ax, dz = out[j].z - f->az;
                const float d = std::hypot(dx, dz);
                float need = floorNeed(f->reach.scaled(f->room), f->floorFacing, dx, dz, clearPad(*f));
                // (someone waiting to go down themselves: far enough for their own reach too)
                if (o && o->floor) need = std::max(need, floorNeed(o->reach, 0, -dx, -dz, kFloorKeep + 0.1f));
                if (d >= need) continue;
                if (d < 1e-4f) dx = 1, dz = 0;
                else dx /= d, dz /= d;
                pushOut(out[j], dx, dz, need - d);
                const float wx = float(cam.halfWidth) * 0.92f * floorWiden(cam, out[j].z);
                out[j].x = std::clamp(out[j].x, -wx, wx);
                out[j].z = std::clamp(out[j].z, cam.floorBack, kFloorZMax);
            }
        }
    for (std::size_t k = 0; k < people.size(); ++k)
        if (Agent* a = find(people[k].id)) {
            a->outX = out[k].x;
            a->outZ = out[k].z;
        }
    // Room to go down (Choreographer::setFloorRoom): not while walking (mayGoDown); going down: whether everyone is
    // out of the move's reach (turned as given); otherwise how far the nearest one is.
    for (std::size_t k = 0; k < people.size(); ++k) {
        Agent& a = *find(people[k].id);
        float room = -1;
        if (mayGoDown(a)) {
            room = 1e6f;
            // The longer they wait, the more of the last few cm is left to keepApart (it pushes the others out of a
            // floor dancer's reach): up to 12 cm after about 8 s.
            const float slack = std::min(0.12f, 0.015f * a.waitT);
            for (const auto& o : agents_) {
                if (o.id == a.id) continue;
                if (a.anchored) {
                    const float dx = o.x - a.ax, dz = o.z - a.az;
                    if (std::hypot(dx, dz) < floorNeed(a.reach, a.floorFacing, dx, dz, kFloorKeep) - (o.anchored ? 0.0f : slack))
                        room = 0;
                } else {
                    room = std::min(room, std::hypot(o.x - a.x, o.z - a.z));
                }
            }
        }
        out[k].floorRoom = room;
        a.clearNow = room > 0;
    }
    return out;
}

DancerSpec dancerSpecFromKeys(const std::function<std::string(const char*)>& lookup, const std::string& name) {
    DancerSpec s;
    s.name = name;
    auto text = [&](const char* key) -> std::string { return trimmed(lookup(key)); };
    auto color = [&](const char* key, Rgb& out) {
        const std::string v = text(key);
        if (v.empty()) return;
        if (auto c = parseRgb(v)) out = *c;
        else s.warnings.push_back(std::string(key) + ": not a color: " + v);
    };
    auto number = [&](const char* key, float lo, float hi, float& out) {
        const std::string v = text(key);
        if (v.empty()) return;
        char* end = nullptr;
        const double d = std::strtod(v.c_str(), &end);
        if (!end || *end || !std::isfinite(d)) {
            s.warnings.push_back(std::string(key) + ": not a number: " + v);
            return;
        }
        out = std::clamp(float(d), lo, hi);
    };
    color("SKIN", s.look.skin);
    color("HAIR", s.look.hair);
    color("TOP", s.look.top);
    color("BOTTOM", s.look.bottom);
    color("SHOES", s.look.shoes);
    color("EYES", s.look.eyes);
    color("ACCENT", s.look.accent);
    if (auto v = text("HAIRSTYLE"); !v.empty()) {
        if (auto h = parseHairStyle(v)) s.look.hairStyle = *h;
        else s.warnings.push_back("HairStyle: unknown: " + v);
    }
    if (auto v = text("OUTFIT"); !v.empty()) {
        if (auto o = parseOutfit(v)) s.look.outfit = *o;
        else s.warnings.push_back("Outfit: unknown: " + v);
    }
    if (auto v = text("HAT"); !v.empty()) {
        if (auto h = parseHat(v)) s.look.hat = *h;
        else s.warnings.push_back("Hat: unknown: " + v);
    }
    number("BUILD", 0.75f, 1.3f, s.look.build);
    number("TALL", 0.8f, 1.2f, s.look.tall);
    // Proportions, cloth, sunglasses, glow sticks, face (docs/scene-format.md §5.9).
    number("HEAD", 0.75f, 1.35f, s.look.head);
    number("LEGS", 0.75f, 1.25f, s.look.legs);
    number("TORSO", 0.8f, 1.2f, s.look.torso);
    number("SHOULDERS", 0.8f, 1.25f, s.look.shoulders);
    number("BELLY", 0.0f, 1.0f, s.look.belly);
    number("LIMBS", 0.75f, 1.35f, s.look.limbs);
    number("BLUSH", 0.0f, 1.0f, s.look.blush);
    auto named = [&](const char* key, auto parse, auto& out) {
        if (auto v = text(key); !v.empty()) {
            if (auto e = parse(v)) out = *e;
            else s.warnings.push_back(std::string(key) + ": unknown: " + v);
        }
    };
    named("TOPPATTERN", parsePattern, s.look.topPattern);
    named("BOTTOMPATTERN", parsePattern, s.look.bottomPattern);
    named("TOPFABRIC", parseFabric, s.look.topFabric);
    named("BOTTOMFABRIC", parseFabric, s.look.bottomFabric);
    named("GLASSES", parseGlasses, s.look.glasses);
    named("EYESHAPE", parseEyeShape, s.look.eyeShape);
    named("BROWS", parseBrows, s.look.brows);
    named("MOUTH", parseMouth, s.look.mouth);
    named("BEARD", parseBeard, s.look.beard);
    color("INK", s.look.ink);
    if (auto v = text("LENS"); v == "clear" || v == "none") s.look.clearLens = true;  // ordinary glasses
    else color("LENS", s.look.lens);
    color("FRAME", s.look.frame);
    auto flag = [&](const char* key, bool& out) {
        const std::string v = text(key);
        if (v.empty()) return;
        if (v == "1" || v == "on" || v == "yes" || v == "true") out = true;
        else if (v == "0" || v == "off" || v == "no" || v == "false") out = false;
        else s.warnings.push_back(std::string(key) + ": not 0 / 1: " + v);
    };
    flag("MIRRORED", s.look.mirrored);
    flag("LASHES", s.look.lashes);
    flag("FRECKLES", s.look.freckles);
    flag("RANDOM", s.random);
    if (auto v = text("GLOWSTICKS"); !v.empty()) {  // a color, or none
        if (v == "none" || v == "0" || v == "off") s.look.glowSticks = false;
        else if (auto g = parseRgb(v)) {
            s.look.glowSticks = true;
            s.look.glow = *g;
        } else s.warnings.push_back("GlowSticks: not a color: " + v);
    }
    if (auto v = text("LIPS"); !v.empty()) {
        if (v == "none") s.look.lips.reset();
        else if (auto l = parseRgb(v)) s.look.lips = *l;
        else s.warnings.push_back("Lips: not a color: " + v);
    }
    s.guestId = text("GUEST");  // a saved stream guest (web/GuestBook): their id
    s.tag = text("NAME");
    if (s.tag.size() >= 2 && s.tag.front() == '"' && s.tag.back() == '"') s.tag = s.tag.substr(1, s.tag.size() - 2);
    if (auto v = text("STYLE"); !v.empty()) {
        if (auto st = parseStyle(v)) s.style = *st;
        else s.warnings.push_back("Style: unknown: " + v);
    }
    number("SNAP", 0.0f, 2.0f, s.snap);
    if (auto v = text("MOVES"); !v.empty()) {
        std::string cur;
        auto flush = [&] {
            if (cur.empty()) return;
            const int i = findMove(cur);
            if (i >= 0) s.likes.push_back(i);
            else s.warnings.push_back("Moves: unknown move: " + cur);
            cur.clear();
        };
        for (char c : v) {
            if (c == ',' || std::isspace(static_cast<unsigned char>(c))) flush();
            else cur.push_back(c);
        }
        flush();
    }
    float dur = -1;
    number("PDUR", 0.0f, 1e6f, dur);
    if (dur >= 0) s.duration = dur;
    return s;
}

namespace {
Rgb hsv(float h, float sat, float val) {
    h = h - std::floor(h);
    const float c = val * sat, x = c * (1 - std::fabs(std::fmod(h * 6, 2.0f) - 1)), m = val - c;
    float r = 0, g = 0, b = 0;
    switch (int(h * 6) % 6) {
        case 0: r = c, g = x; break;
        case 1: r = x, g = c; break;
        case 2: g = c, b = x; break;
        case 3: g = x, b = c; break;
        case 4: r = x, b = c; break;
        default: r = c, b = x; break;
    }
    return {r + m, g + m, b + m};
}
Rgb hex(const char* h) { return *parseRgb(h); }
float luma(Rgb c) { return 0.3f * c.r + 0.59f * c.g + 0.11f * c.b; }

// A seeded random source with weighted picks.
struct Dice {
    std::uint32_t s;
    float u() {
        s = mix32(s + 0x9e3779b9U);
        return float(s >> 8) / float(1u << 24);
    }
    float in(float lo, float hi) { return lo + (hi - lo) * u(); }
    bool chance(float p) { return u() < p; }
    template <typename T>
    T pick(std::initializer_list<std::pair<T, float>> w) {
        float total = 0;
        for (const auto& e : w) total += e.second;
        float x = u() * total;
        for (const auto& e : w) {
            if ((x -= e.second) < 0) return e.first;
        }
        return w.begin()->first;
    }
    // As pick, each weight times f(choice) (0: never).
    template <typename T, typename F>
    T pickBy(std::initializer_list<std::pair<T, float>> w, F&& f) {
        float total = 0;
        for (const auto& e : w) total += e.second * f(e.first);
        float x = u() * total;
        for (const auto& e : w) {
            const float k = e.second * f(e.first);
            if (k > 0 && (x -= k) < 0) return e.first;
        }
        for (const auto& e : w)
            if (e.second * f(e.first) > 0) return e.first;
        return w.begin()->first;
    }
    const char* any(std::initializer_list<const char*> l) {
        const auto i = std::size_t(u() * float(l.size()));
        return *(l.begin() + std::min(i, l.size() - 1));
    }
};

// First names for generated characters: feminine, masculine and either (each character picks from its own and
// either).
constexpr const char* kNamesF[] = {
    "Ada", "Bea", "Cleo", "Dot", "Emi", "Fifi", "Hana", "Ivy", "Juno", "Lea", "Mae", "Nell", "Noor", "Pia", "Rae",
    "Rosa", "Suki", "Tess", "Uma", "Wren", "Xia", "Yara", "Zoe", "Ama", "Esme", "Gwen", "Inez", "Lina", "Nia",
    "Priya", "Vida", "Zara", "Edie", "Faye", "Isla",
};
constexpr const char* kNamesM[] = {
    "Gio", "Ike", "Jax", "Max", "Milo", "Obi", "Vik", "Yusuf", "Zed", "Benji", "Hugo", "Kofi", "Mateo", "Omar",
    "Raffi", "Tomas", "Wes", "Bram", "Dov", "Gus", "Hal", "Uli",
};
constexpr const char* kNamesEither[] = {
    "Ari", "Bo", "Cai", "Dani", "Eli", "Fen", "Jo", "Kai", "Kit", "Lou", "Mika", "Oli", "Pax", "Quin", "Remy",
    "Rio", "Sam", "Sol", "Tam", "Val", "Chi", "Desi", "Jun", "Sasha", "Yuki", "Ayo", "Cyd", "Joss", "Femi",
};
// Which way a hairstyle leans (generated characters: their own and either's, now and then the other's).
enum class Lean { Fem, FemOnly, Masc, MascOnly, Either };
Lean hairLean(HairStyle h) {
    using H = HairStyle;
    switch (h) {
        case H::Bald: case H::Balding: case H::ManBun: case H::SlickBack: case H::Buzz: case H::Quiff:
        case H::Undercut: case H::Shaved: case H::Mullet: case H::Bowl: case H::Pompadour:
            return Lean::MascOnly;  // never on a feminine character (Peter, 2026-11-21)
        case H::Short: case H::SidePart: case H::Waves: case H::Spiky: case H::Mohawk:
            return Lean::Masc;
        case H::Bob: case H::Layers: case H::HalfUp: case H::Braid: case H::Bangs: case H::Pixie: case H::Pigtails:
        case H::Bun: case H::SidePony: case H::SpaceBuns: case H::Curls: case H::BigCurls:
        case H::HugeCurls:
            return Lean::FemOnly;  // never on a masculine character (Peter, 2026-11-21)
        case H::Long: case H::Ponytail: case H::TopKnot:
            return Lean::Fem;
        default: return Lean::Either;  // curly, afro, coils, shag, curtains, anime
    }
}

}  // namespace

std::optional<std::string> normalizeRandomStyles(std::string_view value) {
    std::string v;
    for (char c : value)
        if (!std::isspace(static_cast<unsigned char>(c))) v.push_back(char(std::tolower(static_cast<unsigned char>(c))));
    if (v.empty() || v == "none") return std::string("none");
    if (v == "all") return v;
    bool on[kStyleCount] = {};
    std::size_t at = 0;
    while (at <= v.size()) {
        const std::size_t end = std::min(v.find(',', at), v.size());
        const std::string_view name = std::string_view(v).substr(at, end - at);
        if (!name.empty() && name != "latin") {  // latin: a style removed 2026-11-20 (saved settings may name it)
            const auto st = parseStyle(name);
            if (!st) return std::nullopt;
            on[int(*st)] = true;
        }
        at = end + 1;
    }
    std::string out;
    int n = 0;
    for (int i = 0; i < kStyleCount; ++i)
        if (on[i]) {
            if (!out.empty()) out += ',';
            out += styleName(Style(i));
            ++n;
        }
    return n == kStyleCount ? std::string("all") : n == 0 ? std::string("none") : out;
}

bool randomStyleAllowed(std::string_view list, Style style) {
    const auto norm = normalizeRandomStyles(list);
    if (!norm || *norm == "all") return true;
    const std::string name = styleName(style);
    std::size_t at = 0;
    while (at <= norm->size()) {
        const std::size_t end = std::min(norm->find(',', at), norm->size());
        if (norm->compare(at, end - at, name) == 0) return true;
        at = end + 1;
    }
    return false;
}

DancerSpec generateDancer(Style dance, std::uint32_t seed) {
    Dice d{mix32(seed * 0x2545f491U + 0x51ed)};
    DancerSpec spec;
    spec.style = dance;
    // Presentation (Peter, 2026-11-21): every generated character is feminine or masculine, half and half. Only
    // feminine ones wear dresses, skirts, lashes and lipstick; only masculine ones have beards; hair leans their way
    // (hairLean); masculine shoulders a little broader. Not a setting (stream guests choose their own look).
    const bool fem = d.chance(0.5f);
    spec.feminine = fem;
    {
        const std::size_t own = fem ? std::size(kNamesF) : std::size(kNamesM), n = own + std::size(kNamesEither);
        const std::size_t i = std::min(std::size_t(d.u() * float(n)), n - 1);
        spec.name = i < own ? (fem ? kNamesF[i] : kNamesM[i]) : kNamesEither[i - own];
    }
    Look& L = spec.look;
    // The wardrobes were written for the styles before 2026-11-21; each dance style dresses from the closest one
    // (Peter: the dance style comes first, the look goes with it): house from house (now and then rave gear), pop
    // (popping) from robot, lock from retro (70s funk), break and hip-hop from hip-hop, stomp (krump) from hip-hop in
    // darker colors, disco (waack) from disco, jazz (street jazz) from pop, ballet (ballet jazz) from chill or pop.
    enum class St { Disco, HipHop, House, Pop, Robot, Rave, Retro, Chill };
    const St style = [&] {
        switch (dance) {
            case Style::House: return d.chance(0.3f) ? St::Rave : St::House;
            case Style::Pop: return St::Robot;
            case Style::Lock: return St::Retro;
            case Style::Break: case Style::HipHop: case Style::Stomp: return St::HipHop;
            case Style::Disco: return St::Disco;
            case Style::Jazz: return St::Pop;
            case Style::Ballet: return d.chance(0.6f) ? St::Chill : St::Pop;
            default: return St::Pop;
        }
    }();

    // ---- body: stocky, lanky or in between, and a cartoon head of some size
    const float type = d.u();
    L.build = d.in(0.85f, 1.15f);
    // Masculine a little taller on average (about 6 %), the ranges overlapping widely.
    L.tall = fem ? d.in(0.88f, 1.05f) : d.in(0.93f, 1.1f);
    L.head = d.in(0.95f, 1.05f);  // (Peter, 2026-11-21: no small / medium / large heads)
    L.legs = d.in(0.88f, 1.12f);
    L.torso = d.in(0.92f, 1.1f);
    L.shoulders = fem ? d.in(0.88f, 1.02f) : d.in(1.0f, 1.15f);
    L.limbs = d.in(0.88f, 1.15f);
    if (type < 0.22f) {  // stocky
        L.build = d.in(1.1f, 1.25f);
        L.belly = d.in(0.3f, 0.9f);
        L.legs = d.in(0.85f, 0.95f);
        L.limbs = d.in(1.1f, 1.3f);
    } else if (type < 0.4f) {  // lanky
        L.build = d.in(0.85f, 0.95f);
        L.legs = d.in(1.08f, 1.18f);
        L.tall = fem ? d.in(1.0f, 1.07f) : d.in(1.05f, 1.12f);
        L.limbs = d.in(0.85f, 0.95f);
    }

    // ---- skin, hair, eyes
    static const char* const kSkins[] = {"#ffe0c8", "#f6d2b5", "#f0c4a0", "#e3ad86", "#d19a72", "#b9805a",
                                         "#9c6644", "#7d4f33", "#633a24", "#4a2a1a"};
    // 40 % light (the first three tones), 40 % medium (the next three), 20 % dark (the last four), even within each.
    {
        const float x = d.u();
        const std::size_t i = x < 0.4f ? std::size_t(x / 0.4f * 3)
                            : x < 0.8f ? 3 + std::size_t((x - 0.4f) / 0.4f * 3)
                                       : 6 + std::size_t((x - 0.8f) / 0.2f * 4);
        L.skin = hex(kSkins[std::min<std::size_t>(i, 9)]);
    }
    // Dyed (a bright color of any hue) only for rave, robot and house (Peter, 2026-11-20); everyone else natural.
    // Platinum and silver count as natural colors.
    const float dyed = style == St::Rave ? 0.5f : style == St::Robot ? 0.3f : style == St::House ? 0.2f : 0.0f;
    const bool dyedHair = dyed > 0 && d.chance(dyed);
    if (dyedHair) {
        L.hair = hsv(d.u(), d.in(0.6f, 0.9f), d.in(0.7f, 0.95f));
    } else {
        L.hair = hex(d.any({"#120c0a", "#1c1210", "#2a1a12", "#4a2c1a", "#6b3f22", "#8a4a20", "#b5652a", "#c9a24f",
                            "#e2c77a", "#8a8a8a", "#c0c0c8", "#e8e8f0"}));
    }
    L.eyes = hex(d.any({"#2b1a10", "#3a2618", "#4a3520", "#3d5a2e", "#2f5d8c", "#4b5a66"}));
    if (style == St::Rave && d.chance(0.2f)) L.eyes = hsv(d.u(), 0.7f, 0.7f);

    // ---- color scheme: a base hue and a harmony, with the style's saturation and brightness
    float sat0 = 0.65f, sat1 = 0.9f, val0 = 0.65f, val1 = 0.95f;
    float hue = d.u();
    switch (style) {
        case St::Disco: sat0 = 0.55f; val0 = 0.55f; break;
        case St::HipHop: sat0 = 0.7f; sat1 = 1.0f; val0 = 0.7f; break;
        case St::House: sat0 = 0.6f; break;
        case St::Pop: sat0 = 0.4f; sat1 = 0.75f; val0 = 0.85f; val1 = 1.0f; break;
        case St::Robot: sat0 = 0.0f; sat1 = 0.12f; val0 = 0.45f; val1 = 0.85f; break;
        case St::Rave: sat0 = 0.9f; sat1 = 1.0f; val0 = 0.9f; val1 = 1.0f; break;
        case St::Retro: hue = d.pick<float>({{0.07f, 3}, {0.11f, 2}, {0.5f, 2}, {0.02f, 1}, {0.3f, 1}});
            sat0 = 0.5f; sat1 = 0.8f; val0 = 0.6f; val1 = 0.9f; break;
        case St::Chill: sat0 = 0.18f; sat1 = 0.45f; val0 = 0.55f; val1 = 0.85f; break;
        default: break;
    }
    const float harmony = d.pick<float>({{0.5f, 3}, {0.08f, 2}, {-0.08f, 2}, {0.33f, 2}, {0.0f, 1}});
    if (dance == Style::Stomp) {  // krump: hard and dark, a strong color on black and gray
        sat0 = 0.7f; sat1 = 1.0f; val0 = 0.35f; val1 = 0.7f;
    }
    const Rgb main = hsv(hue, d.in(sat0, sat1), d.in(val0, val1));
    Rgb second = hsv(hue + harmony, d.in(sat0, sat1), d.in(val0, val1));
    const Rgb neutral = hex(d.any({"#1b1b22", "#26262e", "#f2f0ea", "#2f3d66", "#3b4a7a", "#7a6a50", "#5a5a62"}));
    if (style == St::Robot) second = hsv(d.u(), 0.9f, 1.0f);  // grays and one bright accent
    L.top = main;
    L.bottom = d.chance(style == St::Rave || style == St::Pop ? 0.45f : 0.6f) ? neutral : second;
    L.accent = second;
    L.shoes = d.chance(0.5f) ? hex(d.any({"#ffffff", "#111111", "#ececec", "#3a2a1e"})) : second;
    L.ink = luma(L.top) < 0.45f ? (d.chance(0.6f) ? hex("#f4f1ea") : second)
                                : (d.chance(0.6f) ? hex("#1d1d24") : second);

    // ---- wardrobe
    auto dressFor = [&](Outfit o) {  // dresses and skirts: feminine only (and a little more often)
        const bool dress = o == Outfit::Dress || o == Outfit::Skirt || o == Outfit::LongDress;
        return !dress ? 1.0f : fem ? 1.5f : 0.0f;
    };
    switch (style) {
        case St::Disco: L.outfit = d.pickBy<Outfit>({{Outfit::Suit, 3}, {Outfit::Dress, 3}, {Outfit::Tee, 2}, {Outfit::Tank, 1}, {Outfit::Jumpsuit, 2}, {Outfit::LongDress, 1.5f}}, dressFor); break;
        case St::HipHop: L.outfit = d.pickBy<Outfit>({{Outfit::Hoodie, 4}, {Outfit::Tee, 3.5f}, {Outfit::Tank, 1.5f}, {Outfit::Suit, 0.5f}, {Outfit::CropTop, 1}}, dressFor); break;
        case St::House: L.outfit = d.pickBy<Outfit>({{Outfit::Tee, 4}, {Outfit::Tank, 3}, {Outfit::Hoodie, 1.5f}, {Outfit::Dress, 1}, {Outfit::CropTop, 1}, {Outfit::Skirt, 1}}, dressFor); break;
        case St::Pop: L.outfit = d.pickBy<Outfit>({{Outfit::Tee, 3.5f}, {Outfit::Dress, 3}, {Outfit::Tank, 1.5f}, {Outfit::Hoodie, 1.5f}, {Outfit::Skirt, 2}, {Outfit::CropTop, 1.5f}}, dressFor); break;
        case St::Robot: L.outfit = d.pickBy<Outfit>({{Outfit::Suit, 3}, {Outfit::Tee, 3}, {Outfit::Tank, 2}, {Outfit::Hoodie, 2}}, dressFor); break;
        case St::Rave: L.outfit = d.pickBy<Outfit>({{Outfit::Tank, 4}, {Outfit::Tee, 3}, {Outfit::Hoodie, 2}, {Outfit::Dress, 1}, {Outfit::CropTop, 2}}, dressFor); break;
        case St::Retro: L.outfit = d.pickBy<Outfit>({{Outfit::Tee, 3}, {Outfit::Suit, 2.5f}, {Outfit::Dress, 2}, {Outfit::Tank, 1.5f}, {Outfit::Skirt, 1.5f}, {Outfit::Jumpsuit, 1}}, dressFor); break;
        case St::Chill: L.outfit = d.pickBy<Outfit>({{Outfit::Hoodie, 3.5f}, {Outfit::Tee, 4}, {Outfit::Tank, 1.5f}, {Outfit::Dress, 1}, {Outfit::LongDress, 1}, {Outfit::Skirt, 1}}, dressFor); break;
        default: break;
    }
    using P = Pattern;
    auto print = [&](std::initializer_list<std::pair<Pattern, float>> w) { return d.pick<Pattern>(w); };
    switch (style) {
        case St::Disco: L.topPattern = print({{P::None, 6}, {P::Zigzag, 1}, {P::Stripes, 1}, {P::Gradient, 1}}); break;
        case St::HipHop: L.topPattern = print({{P::None, 6}, {P::Stripes, 2}, {P::Plaid, 1}, {P::Gradient, 0.5f}});
            L.bottomPattern = print({{P::None, 8}, {P::Stripes, 1.5f}}); break;
        case St::House: L.topPattern = print({{P::None, 7}, {P::Stripes, 1.5f}, {P::Dots, 1}}); break;
        case St::Pop: L.topPattern = print({{P::None, 4}, {P::Dots, 2}, {P::Stripes, 1.5f}, {P::Gradient, 1.5f}});
            L.bottomPattern = print({{P::None, 8}, {P::Dots, 1}}); break;
        case St::Robot: L.topPattern = print({{P::None, 8}, {P::Stripes, 1}, {P::Plaid, 0.5f}}); break;
        case St::Rave: L.topPattern = print({{P::None, 4}, {P::Zigzag, 2}, {P::Gradient, 2}, {P::Stripes, 1.5f}});
            L.bottomPattern = print({{P::None, 7}, {P::Zigzag, 1}, {P::Stripes, 1}}); break;
        case St::Retro: L.topPattern = print({{P::None, 3}, {P::Stripes, 3}, {P::Zigzag, 2}, {P::Plaid, 1.5f}});
            L.bottomPattern = print({{P::None, 7}, {P::Plaid, 1}, {P::Stripes, 1}}); break;
        case St::Chill: L.topPattern = print({{P::None, 5}, {P::Plaid, 2}, {P::Stripes, 1.5f}}); break;
        default: break;
    }
    using F = Fabric;
    auto fabric = [&](std::initializer_list<std::pair<Fabric, float>> w) { return d.pick<Fabric>(w); };
    switch (style) {
        case St::Disco: L.topFabric = fabric({{F::Matte, 2}, {F::Shiny, 3}, {F::Sequins, 3}, {F::Velvet, 1.5f}});
            L.bottomFabric = fabric({{F::Matte, 5}, {F::Velvet, 2}, {F::Shiny, 1}}); break;
        case St::HipHop: L.topFabric = fabric({{F::Matte, 8}, {F::Shiny, 1}, {F::Velvet, 1}}); break;
        case St::House: L.topFabric = fabric({{F::Matte, 7}, {F::Shiny, 1}, {F::Neon, 1}}); break;
        case St::Pop: L.topFabric = fabric({{F::Matte, 6}, {F::Sequins, 2}, {F::Shiny, 1}}); break;
        case St::Robot: L.topFabric = fabric({{F::Matte, 3}, {F::Shiny, 5}}); L.bottomFabric = fabric({{F::Matte, 3}, {F::Shiny, 2}}); break;
        case St::Rave: L.topFabric = fabric({{F::Matte, 4}, {F::Neon, 3}, {F::Shiny, 1.5f}}); L.bottomFabric = fabric({{F::Matte, 6}, {F::Neon, 1}}); break;
        case St::Retro: L.topFabric = fabric({{F::Matte, 6}, {F::Velvet, 2}, {F::Shiny, 1}}); L.bottomFabric = fabric({{F::Matte, 6}, {F::Velvet, 1.5f}}); break;
        case St::Chill: L.topFabric = fabric({{F::Matte, 8}, {F::Velvet, 1.5f}}); break;
        default: break;
    }
    // Metallic tops are often gold or silver; neon is a pure bright hue.
    if ((L.topFabric == F::Shiny || L.topFabric == F::Sequins) && d.chance(0.5f))
        L.top = hex(d.any({"#d9ad3c", "#c9c9d4", "#e0a87a", "#b48ae6"}));
    if (L.topFabric == F::Neon) L.top = hsv(d.u(), 1.0f, 1.0f);
    if (L.bottomFabric == F::Neon) L.bottom = hsv(d.u(), 1.0f, 1.0f);
    if (L.bottomFabric == F::Shiny && style == St::Robot) L.bottom = hex(d.any({"#c9c9d4", "#9a9aa6"}));

    // ---- hats, then hair
    switch (style) {
        case St::HipHop: L.hat = d.pick<Hat>({{Hat::None, 4}, {Hat::Cap, 4.5f}, {Hat::Beanie, 1.5f}, {Hat::Headphones, 1}}); break;
        case St::House: L.hat = d.pick<Hat>({{Hat::None, 6}, {Hat::Headphones, 2}, {Hat::Cap, 1.5f}}); break;
        case St::Chill: L.hat = d.pick<Hat>({{Hat::None, 5}, {Hat::Beanie, 3}, {Hat::Headphones, 1.5f}}); break;
        case St::Robot: L.hat = d.pick<Hat>({{Hat::None, 7}, {Hat::Headphones, 2}}); break;
        case St::Disco: case St::Retro: L.hat = d.pick<Hat>({{Hat::None, 9}, {Hat::Top, 1}}); break;
        default: L.hat = d.pick<Hat>({{Hat::None, 9}, {Hat::Cap, 0.7f}, {Hat::Beanie, 0.7f}}); break;
    }
    // Hair (2026-11-21): every style draws from the same everyday hair, plus its own flavor on top; each
    // character from their own lean and either's (the other's now and then, never a masculine-only style on a
    // feminine character: hairLean); under a hat (not headphones) never tall hair.
    using H = HairStyle;
    float w[kHairStyleCount] = {};
    auto add = [&](std::initializer_list<std::pair<H, float>> l) {
        for (const auto& [h, x] : l) w[int(h)] += x;
    };
    // Everyday: feminine, masculine, either.
    add({{H::Long, 3}, {H::Bob, 2.5f}, {H::Ponytail, 2.5f}, {H::Layers, 2}, {H::HalfUp, 1.5f}, {H::Bangs, 1.2f},
         {H::Bun, 1.2f}, {H::Braid, 1}, {H::Pixie, 1}, {H::SidePony, 0.8f}, {H::Pigtails, 0.8f}, {H::TopKnot, 0.6f},
         {H::Curls, 1.2f}, {H::BigCurls, 0.7f}, {H::HugeCurls, 0.25f}});  // (the long curls: 2026-11-27)
    add({{H::Short, 3}, {H::SidePart, 2}, {H::Quiff, 1.5f}, {H::Buzz, 1.5f}, {H::Undercut, 1.2f}, {H::Shaved, 0.8f},
         {H::SlickBack, 0.8f}, {H::Waves, 0.6f}, {H::Balding, 0.6f}, {H::ManBun, 0.5f}, {H::Bald, 0.4f},
         {H::Mullet, 0.2f}, {H::Bowl, 0.2f}});
    add({{H::Curly, 1.2f}, {H::Coils, 0.8f}, {H::Afro, 0.8f}, {H::Curtains, 0.6f}, {H::Shag, 0.6f}, {H::Anime, 0.5f},
         {H::Spiky, 0.3f}, {H::Mohawk, 0.2f}});
    // Each style's flavor.
    switch (style) {
        case St::Disco: add({{H::Afro, 3}, {H::Curly, 2}, {H::BigCurls, 1.5f}, {H::HugeCurls, 1.2f}, {H::Curls, 0.8f}, {H::Shag, 1.5f}, {H::Long, 1}, {H::Layers, 0.5f}, {H::TopKnot, 0.5f}, {H::Pompadour, 0.5f}}); break;
        case St::HipHop: add({{H::Coils, 2}, {H::Curls, 0.8f}, {H::BigCurls, 0.5f}, {H::Waves, 2}, {H::Afro, 1.5f}, {H::Braid, 1.5f}, {H::Buzz, 1}, {H::Bald, 1}, {H::SpaceBuns, 0.8f}, {H::Pigtails, 0.5f}, {H::Undercut, 0.5f}}); break;
        case St::Pop: add({{H::Ponytail, 1}, {H::Anime, 1}, {H::Pigtails, 0.8f}, {H::SidePony, 0.8f}, {H::SpaceBuns, 0.8f}, {H::Quiff, 0.8f}, {H::Bangs, 0.6f}, {H::Pixie, 0.6f}, {H::Curtains, 0.6f}}); break;
        case St::Robot: add({{H::SlickBack, 1.5f}, {H::Bald, 1}, {H::Bowl, 1}, {H::Buzz, 1}, {H::Shaved, 1}, {H::Bun, 0.8f}, {H::Bob, 0.6f}, {H::Ponytail, 0.6f}, {H::Spiky, 0.6f}, {H::Mohawk, 0.4f}}); break;
        case St::Rave: add({{H::SpaceBuns, 1.5f}, {H::Pigtails, 1.2f}, {H::Spiky, 1}, {H::Mohawk, 1}, {H::Anime, 1}, {H::Pixie, 0.8f}, {H::Undercut, 0.8f}, {H::Shaved, 0.5f}}); break;
        case St::Retro: add({{H::Afro, 3}, {H::Shag, 2}, {H::BigCurls, 1.2f}, {H::HugeCurls, 1}, {H::Pompadour, 2}, {H::Mullet, 1.2f}, {H::Curtains, 1}, {H::Bangs, 1}, {H::SlickBack, 0.8f}, {H::Bowl, 0.6f}, {H::Quiff, 0.6f}}); break;
        case St::Chill: add({{H::HalfUp, 0.5f}, {H::Layers, 0.5f}, {H::Bun, 0.5f}, {H::Curls, 0.5f}, {H::Curly, 0.3f}}); break;
        default: break;  // house: everyday hair
    }
    const bool hatOn = L.hat == Hat::Cap || L.hat == Hat::Beanie || L.hat == Hat::Top;
    auto tall = [](H h) {
        return h == H::Mohawk || h == H::Spiky || h == H::Afro || h == H::Bun || h == H::Curly || h == H::Pompadour ||
               h == H::Undercut || h == H::SpaceBuns || h == H::TopKnot || h == H::Anime;
    };
    {
        float k[kHairStyleCount], total = 0;
        for (int i = 0; i < kHairStyleCount; ++i) {
            const H h = H(i);
            const Lean l = hairLean(h);
            const float lean = l == Lean::Either ? 1.0f
                               : l == Lean::MascOnly ? (fem ? 0.0f : 1.0f)
                               : l == Lean::FemOnly ? (fem ? 1.0f : 0.0f)
                               : (l == Lean::Fem) == fem ? 1.0f : 0.15f;
            k[i] = w[i] * lean * (hatOn && tall(h) ? 0.0f : 1.0f);
            total += k[i];
        }
        float x = d.u() * total;
        L.hairStyle = fem ? H::Long : H::Short;
        for (int i = 0; i < kHairStyleCount; ++i)
            if (k[i] > 0 && (x -= k[i]) < 0) {
                L.hairStyle = H(i);
                break;
            }
    }
    if (L.hat == Hat::Top && (L.hairStyle == H::Pigtails)) L.hat = Hat::None;

    // ---- sunglasses, everyday glasses and glow sticks
    // (2026-11-21, Peter: fewer sunglasses, about 17 % in all, hiding fewer faces; most in popping, disco and rave.)
    using G = Glasses;
    float shades = 0.08f;
    switch (style) {
        case St::Rave: shades = 0.4f; break;
        case St::Robot: shades = 0.35f; break;
        case St::Disco: shades = 0.3f; break;
        case St::Retro: shades = 0.2f; break;
        case St::Pop: case St::HipHop: shades = 0.15f; break;
        default: break;
    }
    if (!d.chance(shades)) {
        // Everyday glasses (clear lenses, just the frame): about 9 % of everyone else, round and square mostly.
        if (d.chance(style == St::Retro || style == St::Chill ? 0.14f : 0.1f)) {
            L.glasses = d.pick<G>({{G::Round, 3}, {G::Square, 3}, {G::CatEye, fem ? 1.0f : 0.0f}, {G::Aviator, 0.5f}});
            L.clearLens = true;
            L.frame = d.pick<Rgb>({{hex("#141414"), 4}, {hex("#5a3a22"), 2.5f}, {hex("#d9ad3c"), 1}, {hex("#9a9aa6"), 1},
                                   {second, 1}});
        }
    } else {
        switch (style) {
            case St::Rave: L.glasses = d.pick<G>({{G::Visor, 3}, {G::Star, 2}, {G::Heart, 2}, {G::Round, 2}, {G::CatEye, 1}}); break;
            case St::Robot: L.glasses = d.pick<G>({{G::Visor, 5}, {G::Square, 2}, {G::Round, 1}}); break;
            case St::Disco: L.glasses = d.pick<G>({{G::Aviator, 3}, {G::Star, 2}, {G::Heart, 1.5f}, {G::Round, 1.5f}}); break;
            case St::Retro: L.glasses = d.pick<G>({{G::Aviator, 3}, {G::Square, 2}, {G::Round, 2}, {G::CatEye, 1.5f}}); break;
            case St::Pop: L.glasses = d.pick<G>({{G::Heart, 3}, {G::CatEye, 2.5f}, {G::Star, 2}, {G::Round, 1.5f}}); break;
            case St::HipHop: L.glasses = d.pick<G>({{G::Square, 3}, {G::Visor, 2}, {G::Aviator, 2}, {G::Round, 1}}); break;
            default: L.glasses = d.pick<G>({{G::Round, 3}, {G::Square, 2}, {G::Aviator, 2}, {G::CatEye, 1}}); break;
        }
        L.mirrored = d.chance(style == St::Rave || style == St::Disco || style == St::Robot ? 0.45f : 0.2f);
        L.lens = d.pick<Rgb>({{hex("#141418"), 5}, {hsv(d.u(), 0.6f, 0.45f), 3}, {hex("#3a2a14"), 1}});
        if (L.mirrored) L.lens = d.pick<Rgb>({{hex("#c9c9d4"), 2}, {hex("#d9ad3c"), 1.5f}, {hsv(d.u(), 0.7f, 0.95f), 2}});
        L.frame = d.pick<Rgb>({{hex("#141414"), 4}, {hex("#f2f0ea"), 1.5f}, {hex("#d9ad3c"), 1.5f}, {second, 2}});
    }
    // Glow sticks: house dancers only, about 1 in 5 of them (Peter, 2026-11-21): 40 % in rave gear (3 in 10 house
    // dancers), 11 % otherwise.
    const float sticks = style == St::Rave ? 0.4f : style == St::House ? 0.114f : 0.0f;
    if (d.chance(sticks)) {
        L.glowSticks = true;
        L.glow = hsv(d.pick<float>({{0.33f, 3}, {0.9f, 2}, {0.55f, 2}, {0.14f, 1.5f}, {0.78f, 1.5f}}), 0.85f, 1.0f);
    }

    // ---- face
    using E = EyeShape;
    L.eyeShape = d.pick<E>({{E::Oval, 3.5f}, {E::Round, 1.5f}, {E::Sleepy, style == St::Chill ? 3.0f : 0.8f},
                            {E::Happy, 1.2f}, {E::Big, style == St::Pop ? 2.5f : 1.5f},
                            {E::Dot, style == St::Robot ? 2.5f : 0.8f}});
    L.brows = d.pick<Brows>({{Brows::Normal, 4}, {Brows::Thick, 1.5f}, {Brows::Thin, 1.2f}, {Brows::Arched, 1.5f}});
    L.mouth = d.pick<Mouth>({{Mouth::Smile, 4}, {Mouth::Grin, 2}, {Mouth::Small, 1.2f}, {Mouth::Smirk, 1.5f}});
    L.lashes = fem && d.chance(0.65f);
    L.freckles = d.chance(L.skin.r > 0.85f ? 0.25f : 0.08f);
    L.blush = d.in(0.1f, 0.5f);
    if (!fem && !dyedHair && d.chance(style == St::Retro ? 0.7f : 0.38f))
        L.beard = d.pick<Beard>({{Beard::Stubble, 2}, {Beard::Moustache, style == St::Retro ? 3.0f : 1.5f},
                                 {Beard::Goatee, 1.5f}, {Beard::Full, 1.5f},
                                 {Beard::Handlebar, style == St::Retro || style == St::Disco ? 1.2f : 0.3f},
                                 {Beard::Circle, 1.2f}, {Beard::Short, style == St::HipHop ? 2.0f : 1.5f},
                                 {Beard::Long, style == St::Chill ? 0.8f : 0.4f}});
    if (fem && d.chance(style == St::Disco ? 0.7f : 0.38f))
        L.lips = hex(d.any({"#c2183a", "#e0457b", "#8e1b3a", "#ff5f8f", "#7a2a6a"}));
    return spec;
}

void DancerRun::restyle(DancerSpec spec) {
    spec.guestId = spec_.guestId;
    const bool newStyle = spec.style != spec_.style || spec.snap != spec_.snap || spec.likes != spec_.likes;
    spec_ = std::move(spec);
    sk_ = makeSkeleton(spec_.look);
    headExtSet_ = false;
    legRatio_ = (sk_.thigh + sk_.shin) / (makeSkeleton(Look{}).thigh + makeSkeleton(Look{}).shin);
    poseToFloats(restPose(sk_), rest_);
    if (newStyle) {
        ChoreoOptions o = choreo_.options();
        o.style = spec_.style;
        o.sharp = spec_.snap;
        o.likes = spec_.likes;
        choreo_ = dance::Choreographer(o);
    }
}

DancerRun::DancerRun(DancerSpec spec, std::uint32_t seed, double t, double beat, double hold)
    : spec_(std::move(spec)),
      sk_(makeSkeleton(spec_.look)),
      choreo_([&] {
          ChoreoOptions o;
          o.style = spec_.style;
          o.seed = seed;
          o.sharp = spec_.snap;
          o.likes = spec_.likes;
          return o;
      }()),
      seed_(seed),
      start_(t),
      hold_(hold) {
    choreo_.hello(beat);
    legRatio_ = (sk_.thigh + sk_.shin) / (makeSkeleton(Look{}).thigh + makeSkeleton(Look{}).shin);
    pose_ = restPose(sk_);
    poseToFloats(pose_, rest_);
    // The personality: everyone dances the same routines a little differently.
    std::uint32_t h = mix32(seed * 0x2545f491U + 99);
    auto u = [&](float lo, float hi) {
        h = mix32(h + 0x9e3779b9U);
        return lo + (hi - lo) * float(h >> 8) / float(1u << 24);
    };
    const float sharp = spec_.snap >= 0 ? spec_.snap : styleFeel(spec_.style).sharp;
    const float tight = sharp > 1.4f ? 0.5f : 1.0f;  // robot / snappy dancers stay closer to the grid
    pers_.lean = u(-0.045f, 0.03f) * tight;            // up to ~20 ms behind or ~15 ms ahead at 128 BPM
    pers_.armGain = u(0.82f, 1.15f);
    pers_.legGain = u(0.85f, 1.15f);
    pers_.hipGain = u(0.8f, 1.25f);
    pers_.headGain = u(0.7f, 1.3f);
    pers_.asym = u(-0.1f, 0.1f);
    pers_.slouch = u(-0.02f, 0.07f);
    pers_.tilt = u(-0.06f, 0.06f);
    pers_.nod = u(-0.04f, 0.04f);
    pers_.weight = u(-0.025f, 0.025f);
    pers_.stance = u(0.9f, 1.12f);
    pers_.breath = u(3.2f, 4.6f);
    pers_.handWn = u(40, 55) * (sharp > 1.4f ? 1.5f : 1.0f);
    pers_.headWn = u(22, 30) * (sharp > 1.4f ? 1.5f : 1.0f);
    // Appended (earlier draws keep their values, so existing characters keep their personality).
    pers_.armLag = u(0.0f, 0.03f) * tight;      // up to ~14 ms at 128 BPM
    pers_.armLagAsym = u(-0.008f, 0.008f) * tight;
    pers_.headLag = u(0.0f, 0.02f) * tight;
    pers_.punch = u(-0.15f, 0.15f);
}

double DancerRun::humanBeat(double beat) const {
    // A fixed lean, a slow drift over a few beats, and each beat landing a few ms differently.
    return beat + humanAmt_ * (pers_.lean + 0.015 * vnoise(seed_ + 30, beat / 3.0) +
                               0.008 * vnoise(seed_ + 31, beat));
}

double DancerRun::danceBeat(double beat) const {
    const double b = humanBeat(beat);
    if (humanAmt_ <= 0) return b;
    // Within each beat: b + s sin(2 pi b) / 2 pi keeps the whole beats (the hits) where they are and moves faster
    // through them (s > 0) or slower (s < 0); monotonic for |s| < 1. The feel drifts a little bar by bar.
    const double s = std::clamp(double(humanAmt_) * (pers_.punch + 0.05 * vnoise(seed_ + 32, beat / 8.0)), -0.4, 0.4);
    return b + s * std::sin(6.283185307179586 * b) / 6.283185307179586;
}

void DancerRun::humanize(float* p, double beat, double t) const {
    const float a = humanAmt_;
    if (a <= 0) return;
    const float effort = 1 + 0.12f * a * vnoise(seed_ + 1, beat / 4.0);  // some bars bigger, some lazier
    auto gain = [&](int i, float g) { p[i] = rest_[i] + (p[i] - rest_[i]) * g; };
    const float arms = (1 + (pers_.armGain - 1) * a) * effort;
    for (int i = 15; i <= 17; ++i) gain(i, arms * (1 + pers_.asym * a));
    for (int i = 18; i <= 20; ++i) gain(i, arms * (1 - pers_.asym * a));
    // Hands never hit the same spot twice: a slow wander of a centimeter or two.
    for (int i = 15; i <= 20; ++i)
        if (i % 3 != 2) p[i] += 0.012f * a * vnoise(seed_ + 10 + std::uint32_t(i), beat / 2.0);
    const float hips = (1 + (pers_.hipGain - 1) * a) * effort;
    for (int i : {3, 6, 7, 9, 10}) gain(i, hips);
    // Legs: how deep the knees bend and how high the feet lift (feet are not moved across the floor: no sliding).
    const float legs = (1 + (pers_.legGain - 1) * a) * effort;
    if (p[4] < rest_[4]) gain(4, legs);
    for (int i : {22, 25})
        if (p[i] > rest_[i]) gain(i, legs);
    p[21] += rest_[21] * (pers_.stance - 1) * a;
    p[24] += rest_[24] * (pers_.stance - 1) * a;
    for (int i = 12; i <= 14; ++i) gain(i, 1 + (pers_.headGain - 1) * a);
    // Posture: a slouch, a head tilt, weight on one leg; the head looks around; breathing.
    p[11] += a * (pers_.slouch + 0.012f * float(std::sin(6.2831853 * t / pers_.breath)));
    p[13] += a * pers_.tilt;
    p[14] += a * (pers_.nod + 0.035f * vnoise(seed_ + 21, t / 3.0));
    p[12] += a * 0.1f * vnoise(seed_ + 20, t / 2.5);
    p[7] += a * pers_.weight;
    p[3] += a * pers_.weight * 0.2f;
}

namespace {
// How much of an avoidance a dancer takes on: the one walking does most of it, someone on their spot gives way a
// little. Shares of a pair add up to about 1.
std::pair<float, float> avoidShares(const DancerRun& a, const DancerRun& b) {
    const float sa = std::hypot(a.vx(), a.vz()), sb = std::hypot(b.vx(), b.vz());
    const float wa = (0.08f + sa) / (0.16f + sa + sb);
    // Someone on the floor isn't pushed (2026-11-27): the other one takes it all.
    const bool fa = a.anchored() >= 0.5f || a.clearing(), fb = b.anchored() >= 0.5f || b.clearing();
    if (fa != fb) return fa ? std::pair<float, float>{0.0f, 1.0f} : std::pair<float, float>{1.0f, 0.0f};
    return {wa, 1 - wa};
}
// How close two dancers may come (centers): bodies, or a floor dancer's reach towards the other one.
float apartNeed(const DancerRun& a, const DancerRun& b, float dx, float dz, float base, float pad) {
    float need = base;
    const float ha = a.clearing() ? 1.0f : a.anchored(), hb = b.clearing() ? 1.0f : b.anchored();
    if (ha > 0) need = std::max(need, ha * floorNeed(a.floorReachNow(), a.facing(), dx, dz, pad));
    if (hb > 0) need = std::max(need, hb * floorNeed(b.floorReachNow(), b.facing(), -dx, -dz, pad));
    return need;
}
}  // namespace

void avoidEachOther(const std::vector<DancerRun*>& runs) {
    for (std::size_t i = 0; i < runs.size(); ++i)
        for (std::size_t j = i + 1; j < runs.size(); ++j) {
            DancerRun& A = *runs[i];
            DancerRun& B = *runs[j];
            if (!A.placed() || !B.placed()) continue;
            const float dx = B.x() - A.x(), dz = B.z() - A.z();  // A -> B
            const float R = apartNeed(A, B, dx, dz, 2 * kBodyRadius, kFloorKeep);
            const float wx = A.vx() - B.vx(), wz = A.vz() - B.vz();  // A's velocity relative to B
            const float a = wx * wx + wz * wz, b = wx * dx + wz * dz, c = dx * dx + dz * dz - R * R;
            if (a < 1e-6f || b <= 0 || c < 0) continue;  // not closing in (overlaps: keepApart)
            const float disc = b * b - a * c;
            if (disc <= 0) continue;  // they pass clear of each other
            const float tau = (b - std::sqrt(disc)) / a;
            if (tau <= 0 || tau > kAvoidHorizon) continue;
            // Where B would be relative to A at contact; A steers away from it, B the other way. Plus keep right
            // (of each one's own relative motion: the same rule for both, so head-on they pass instead of stalling).
            float rx = dx - wx * tau, rz = dz - wz * tau;
            const float rl = std::hypot(rx, rz);
            if (rl > 1e-4f) {
                rx /= rl;
                rz /= rl;
            } else {
                rx = dx / std::sqrt(dx * dx + dz * dz);
                rz = dz / std::sqrt(dx * dx + dz * dz);
            }
            const float wl = std::sqrt(a);
            const float rightX = -wz / wl, rightZ = wx / wl;  // facing +z, a dancer's right is -x
            float ux = -rx + 0.5f * rightX, uz = -rz + 0.5f * rightZ;
            const float ul = std::hypot(ux, uz);
            ux /= ul;
            uz /= ul;
            const float mag = std::min(4.0f, 0.9f * (kAvoidHorizon - tau) / (tau + 0.15f));
            const auto [sa, sb] = avoidShares(A, B);
            A.steer(ux * mag * 2 * sa, uz * mag * 2 * sa);
            B.steer(-ux * mag * 2 * sb, -uz * mag * 2 * sb);
        }
}

void keepApart(const std::vector<DancerRun*>& runs) {
    for (int pass = 0; pass < 3; ++pass)
        for (std::size_t i = 0; i < runs.size(); ++i)
            for (std::size_t j = i + 1; j < runs.size(); ++j) {
                DancerRun& A = *runs[i];
                DancerRun& B = *runs[j];
                if (!A.placed() || !B.placed()) continue;
                float dx = B.x() - A.x(), dz = B.z() - A.z();
                const float d = std::hypot(dx, dz);
                const float need = apartNeed(A, B, dx, dz, kMinApart, kFloorKeep);
                if (d >= need) continue;
                if (d < 1e-4f) {  // on top of each other: apart sideways
                    dx = 1;
                    dz = 0;
                } else {
                    dx /= d;
                    dz /= d;
                }
                // (Round someone on the floor at most 0.8 cm a pass, about 1.4 units/s: pushed out, no pop.)
                const float deficit = std::max(kMinApart - d, std::min(need - d, 0.008f));
                const auto [sa, sb] = avoidShares(A, B);
                A.nudge(-dx * deficit * sa, -dz * deficit * sa);
                B.nudge(dx * deficit * sb, dz * deficit * sb);
            }
}

void DancerRun::setSlot(float x, float z) {
    tx_ = x;
    tz_ = z;
    if (!placed_) {
        x_ = x;
        z_ = z;
        placed_ = true;
    }
}

void DancerRun::leave(double t, double beat, double fade) {
    if (leaving()) return;
    exitStart_ = t;
    exitLen_ = std::max(1.6, fade);  // long enough for the goodbye wave to read
    choreo_.farewell(beat);
}

void DancerRun::step(double t, double dt, const MusicState& m, float energy, const DanceDirection& dir) {
    if (!leaving() && hold_ > 0 && t - start_ >= hold_) leave(t, m.beat, 2.0);
    // Wander to the spot at a walking pace (critically damped spring, at most 0.75 units/s).
    const float w = 2.6f, d = float(std::clamp(dt, 0.0, 0.1));
    float ax = w * w * (tx_ - x_) - 2 * w * vx_ + avoidX_, az = w * w * (tz_ - z_) - 2 * w * vz_ + avoidZ_;
    avoidX_ = avoidZ_ = 0;
    // On the floor (2026-11-27): the spot held where they went down, no steering; any walk eases out (~0.3 s).
    {
        const float g = pose_.ground;
        const float want = g <= 0.25f ? 0.0f : g >= 0.6f ? 1.0f : (g - 0.25f) / 0.35f;
        floorHold_ = floorFixes.hold ? want : 0.0f;
    }
    if (floorHold_ > 0) {
        ax *= 1 - floorHold_;
        az *= 1 - floorHold_;
        const float k = std::exp(-floorHold_ * d / 0.1f);
        vx_ *= k;
        vz_ *= k;
    }
    vx_ += ax * d;
    vz_ += az * d;
    const float speed = std::sqrt(vx_ * vx_ + vz_ * vz_), maxSpeed = 0.75f;
    if (speed > maxSpeed) {
        vx_ *= maxSpeed / speed;
        vz_ *= maxSpeed / speed;
    }
    x_ += vx_ * d;
    z_ += vz_ * d;
    const float moving = std::min(1.0f, std::sqrt(vx_ * vx_ + vz_ * vz_) / 0.35f);  // 0 standing .. 1 walking

    const double age = t - start_;
    const Routine* own = choreo_.routineAt(m.beat);
    const bool greeting = !leaving() && age < 1.0 && own && own->full == findMove("wave");
    // The stage may direct this dancer (not while busting a move: their own routines, whoever they are with).
    const bool free = !leaving() && !greeting && !choreo_.bustMoveAt(danceBeat(m.beat) + 2, false);
    usedLeader_ = free && dir.leader && !dir.react;
    // Room to go down (the stage's say; own moves only: following someone or watching, the own floor moves wait).
    {
        const bool own = !free || (!usedLeader_ && !dir.react);
        const float room = dir.floorRoom > 1e29f ? dir.floorRoom : leaving() || greeting || !own ? -1.0f : dir.floorRoom;
        choreo_.setFloorRoom(room > 1e29f ? room : room / legRatio_);
        // Waiting for room where the stage holds them: the crowd keeps out of the reach already.
        clearing_ = false;
        if (room >= 0 && room < 1e29f && floorHold_ < 0.5f) {
            const int pm = choreo_.floorPending();
            if (pm >= 0 && moveLibrary()[std::size_t(pm)].clip >= 0) {
                clearing_ = true;
                reachNow_ = clipInfo(moveLibrary()[std::size_t(pm)].clip).reach.scaled(legRatio_);
            } else if (choreo_.bustMoveAt(danceBeat(m.beat), true) && !choreo_.bustMoveAt(danceBeat(m.beat), false)) {
                clearing_ = true;
                reachNow_ = bustMoveReach().scaled(legRatio_);
            }
        }
    }
    // The moves run on this dancer's own beat (a little behind or ahead of the music, drifting, with its own feel).
    MusicState mh = m;
    mh.beat = danceBeat(m.beat);
    DancePose next = choreo_.pose(mh, sk_);  // always: keeps the own clock moving (hello, goodbye)
    std::string key = "own";
    desc_ = choreo_.describe(m.beat);
    int source = 0;  // 0 own routine, 1 reaction, 2 leader
    if (free && dir.react) {
        next = watchPose(mh.beat, seed_, spec_.style, sk_, choreo_.options().mocap > 0);
        key = "react";
        desc_ = watchName(m.beat, seed_, spec_.style, choreo_.options().mocap > 0);
        source = 1;
    } else if (usedLeader_) {
        next = dir.leader->pose(mh, sk_, 0, dir.mirror);
        key = "lead" + std::to_string(dir.leader->options().seed) + (dir.mirror ? "m" : "");  // the dancer seed: unique, unlike a reused address
        desc_ = dir.leader->describe(m.beat) + (dir.mirror ? " (mirrored)" : "");
        source = 2;
    }
    if (free && !dir.role.empty()) desc_ = dir.role + ": " + desc_;
    // A new source takes over without a jump: inertial blending. At the switch the difference from the last output
    // (and its velocity) becomes an offset that decays to zero (critically damped, ~0.4 s), so the motion carries on.
    float raw[kPoseFloats], out[kPoseFloats];
    poseToFloats(next, raw);
    if (humanAmt_ > 0) {
        // Timing per body part: the core (hips, chest, feet) is on the dancer's beat; the arms (each hand its own
        // lag) and the head come a little after, the same moves sampled that much earlier. Lags only (never ahead
        // of the clock the routines were planned to).
        auto at = [&](double delay) {
            if (source == 1) return watchPose(mh.beat - delay, seed_, spec_.style, sk_, choreo_.options().mocap > 0);
            if (source == 2) return dir.leader->pose(mh, sk_, delay, dir.mirror);
            return choreo_.pose(mh, sk_, delay);
        };
        float part[kPoseFloats];
        auto take = [&](double lag, int from, int to) {
            if (lag <= 1e-6) return;
            poseToFloats(at(lag), part);
            for (int i = from; i <= to; ++i) raw[i] = part[i];
        };
        const double a = humanAmt_;
        float base[kPoseFloats];
        const bool fade = raw[51] > 0 && floorFixes.fade;
        if (fade) std::copy(raw, raw + kPoseFloats, base);
        take(a * pers_.headLag, 12, 14);
        take(a * std::max(0.0f, pers_.armLag + pers_.armLagAsym), 15, 17);
        take(a * std::max(0.0f, pers_.armLag - pers_.armLagAsym), 18, 20);
        // On the floor the hands and head bear weight: no lag (faded in with `ground`).
        if (fade) {
            const float g = std::clamp(raw[51], 0.0f, 1.0f);
            for (int i = 12; i <= 20; ++i) raw[i] += (base[i] - raw[i]) * g;
        }
    }
    // Humanising (gains, effort, hand wander, posture) faded out on the floor: a planted hand doesn't wander.
    if (raw[51] > 0 && floorFixes.fade) {
        float base[kPoseFloats];
        std::copy(raw, raw + kPoseFloats, base);
        humanize(raw, mh.beat, t);
        const float g = std::clamp(raw[51], 0.0f, 1.0f);
        for (int i = 0; i < kPoseFloats; ++i) raw[i] += (base[i] - raw[i]) * g;
    } else {
        humanize(raw, mh.beat, t);
    }
    // Floor work's pelvis quaternion (47..50) kept on the same side as the last output (q and -q are the same turn),
    // so the filters below never blend across a sign flip. Standing moves: identity, untouched.
    if (haveOut_ && raw[47] * lastOut_[47] + raw[48] * lastOut_[48] + raw[49] * lastOut_[49] + raw[50] * lastOut_[50] < 0)
        for (int i = 47; i <= 50; ++i) raw[i] = -raw[i];
    const float fdt = float(std::max(1e-4, std::min(0.1, dt)));
    if (key != sourceKey_) {
        if (haveOut_)
            for (int i = 0; i < kPoseFloats; ++i) {
                float off = lastOut_[i] - raw[i];
                if (i == 2) off = std::remainder(off, 6.2831853f);  // bodyYaw: the short way round
                inertOff_[i] = off;
                inertVel_[i] = outVel_[i];
            }
        sourceKey_ = key;
    }
    const float wn = 9.0f;
    for (int i = 0; i < kPoseFloats; ++i) {
        inertVel_[i] += (-wn * wn * inertOff_[i] - 2 * wn * inertVel_[i]) * fdt;
        inertOff_[i] += inertVel_[i] * fdt;
        out[i] = raw[i] + inertOff_[i];
        // (bodyYaw the short way: a source's heading may jump by whole turns, a loop's seam or a crossfade's end,
        // invisible by itself; taken as a speed at the next switch it whipped them round 2-7 times. 2026-11-27.)
        const float dOut = i == 2 ? std::remainder(out[i] - lastOut_[i], 6.2831853f) : out[i] - lastOut_[i];
        outVel_[i] = haveOut_ ? dOut / fdt : 0.0f;
        lastOut_[i] = out[i];
    }
    haveOut_ = true;
    // Follow-through: the head and hands trail the body by a few hundredths of a second and settle with a slight
    // overshoot (a spring, damping 0.75, integrated implicitly so any frame rate is stable).
    if (humanAmt_ > 0.02f && followSet_) {
        for (int i = 12; i <= 20; ++i) {
            if (i == 12) continue;  // head yaw: turns (spins, looking at a partner) stay crisp
            const float wn = (i <= 14 ? pers_.headWn : pers_.handWn) / std::max(0.5f, humanAmt_), z = 0.75f;
            const float v = (followVel_[i] + fdt * wn * wn * (out[i] - follow_[i])) /
                            (1 + 2 * z * wn * fdt + wn * wn * fdt * fdt);
            followVel_[i] = v;
            follow_[i] += v * fdt;
            // (Floor work: none, faded with `ground`; the spring keeps following.)
            out[i] = out[51] > 0 && floorFixes.fade ? follow_[i] + (out[i] - follow_[i]) * std::clamp(out[51], 0.0f, 1.0f) : follow_[i];
        }
    } else {
        for (int i = 0; i < kPoseFloats; ++i) {
            follow_[i] = out[i];
            followVel_[i] = 0;
        }
        followSet_ = true;
    }
    pose_ = poseFromFloats(out);
    // Facing: the audience (a little towards the middle of the crew), or as directed; turned smoothly.
    float target = (free && std::isfinite(dir.facing)) ? dir.facing : -x_ * 0.12f;
    // On the move: turned partly towards where they are going.
    if (moving > 0.05f) {
        const float travel = std::clamp(std::atan2(vx_, vz_), -1.2f, 1.2f);
        target += (travel - target) * 0.55f * moving;
    }
    if (!facingSet_) {
        facing_ = target;
        facingSet_ = true;
    }
    // (On the floor the turn is held: the stage gave it before they went down.)
    if (floorHold_ > 0) facing_ += (target - facing_) * (float(std::min(1.0, dt * 5.0)) * (1 - 0.85f * floorHold_));
    else facing_ += (target - facing_) * float(std::min(1.0, dt * 5.0));
    pose_.bodyYaw += facing_;
    if (free) pose_.headYaw += dir.look;
    stepFeet(m.beat, dt, moving);
    faceAccents(pose_, m.beat, t, seed_, energy);
    // Amplitude: blend towards rest when quiet (energy < 1 shrinks the moves).
    // (Not floor work: towards a standing rest pose would lift planted hands; faded with `ground`.)
    // (Not the heading: it holds the stage facing and any turn, an absolute angle; scaled it snapped by 2 pi (1 - e)
    // whenever it crossed +-pi. Spin glitches 2026-11-27.)
    if (energy < 0.999f) {
        const float e = std::clamp(energy, 0.0f, 1.0f), g = std::clamp(pose_.ground, 0.0f, 1.0f);
        const float yaw = pose_.bodyYaw;
        pose_ = mix(restPose(sk_), pose_, g > 0 && floorFixes.fade ? e + (1 - e) * g : e);
        pose_.bodyYaw = yaw;
    }
    // Floor work: the upper body on the floor where the move has it there, never through it, the hair and hat neither,
    // planted hands on it (nothing standing).
    if (pose_.ground > 0 && !headExtSet_) {
        headExt_ = headExtent(spec_.look, sk_);
        headExtSet_ = true;
    }
    groundPose(sk_, pose_, &headExt_);
    if (floorFixes.pins) pinContacts(dt);
    // The reach playing (the stage keeps neighbors out of it).
    if (pose_.ground > 0.05f) {
        const Routine* rr = usedLeader_ ? dir.leader->routineAt(mh.beat) : choreo_.routineAt(mh.beat);
        if (rr && rr->full >= 0 && moveLibrary()[std::size_t(rr->full)].clip >= 0) {
            const ClipInfo& ci = clipInfo(moveLibrary()[std::size_t(rr->full)].clip);
            if (ci.floorWork) reachNow_ = ci.reach.scaled(legRatio_);
        }
    }
    // Entrance: hop in with an overshooting pop. Exit: shrink in the last 0.35 s.
    const float in = float(age / 0.55);
    scale_ = easeOutBack(in);
    lift_ = 0.35f * std::pow(std::max(0.0f, 1.0f - in), 2.0f);
    // A bounce in the step while wandering (two hops a beat).
    lift_ += 0.022f * moving * float(std::fabs(std::sin(3.14159265 * 2.0 * m.beat)));
    if (leaving()) {
        const double left = exitStart_ + exitLen_ - t;
        if (left < 0.35) {
            const float k = float(std::max(0.0, left) / 0.35);
            scale_ *= k * k * (3 - 2 * k);
            lift_ += 0.15f * (1 - k);
        }
    }
    stepHair(dt);
    stepHairSim(dt);
}

void DancerRun::stepHair(double dt) {
    // Where the hanging hair ends at rest (behind and below each side of the head), on the stage.
    const Frame head = headFrame(sk_, pose_);
    const float R = sk_.headR, k = scale_;
    const V3 place{x_, lift_, z_};
    if (k < 0.05f) {
        for (HairTip& h : hair_) h.set = false;
        pose_.hairSwing[0] = pose_.hairSwing[1] = V3{};
        return;
    }
    // About the length of hanging hair: the swing is the lag over this.
    const float len = 1.2f * R * k;
    // Hair hangs partly with gravity: as the head tilts the tips are pulled a little towards straight down.
    const V3 down{0, -1, 0};
    const V3 hang = (down + head.y) * (0.45f * len);
    const float h = float(std::clamp(dt, 0.0, 0.1));
    for (int side = 0; side < 2; ++side) {
        HairTip& t = hair_[side];
        const float sx = side == 0 ? 1.0f : -1.0f;
        const V3 rest = place + (head.o + head.dir(V3{sx * 0.9f * R, -1.1f * R, -0.7f * R})) * k;
        const V3 tip = rest + hang;
        if (!t.set || dt > 0.1) {
            t.x = t.target = tip;
            t.v = {};
            t.set = true;
            continue;
        }
        // A bouncy spring (about 2 Hz, the two sides a little apart so pigtails don't swing as one), damped
        // relative to the head's own motion; sub-stepped so any frame rate is stable.
        const float wn = 6.2831853f * (side == 0 ? 2.0f : 2.25f), zeta = 0.22f;
        const V3 from = t.target, tvel = (tip - from) * (h > 1e-6f ? 1.0f / h : 0.0f);
        const int n = std::max(1, int(std::ceil(h * 240.0f)));
        const float sh = h / float(n);
        for (int i = 1; i <= n; ++i) {
            const V3 at = from + (tip - from) * (float(i) / float(n));
            const V3 a = (at - t.x) * (wn * wn) - (t.v - tvel) * (2.0f * zeta * wn);
            t.v = t.v + a * sh;
            t.x = t.x + t.v * sh;
        }
        t.target = tip;
        // The lag, in the head's frame, per unit length; held to a sensible swing (a spin flings it out, not round).
        const V3 d = (t.x - rest) * (1.0f / len);
        V3 sw{dot(d, head.x), dot(d, head.y), dot(d, head.z)};
        const float m = length(sw);
        if (m > 0.9f) sw = sw * (0.9f / m);
        pose_.hairSwing[side] = sw;
    }
}

// ---- hair physics (Peter, 2026-11-27; docs/dancers-design.md "Hair physics")
namespace {
// Tuning (character units, seconds; scaled by the dancer's size where it matters).
constexpr float kHairRate = 120.0f;       // sub-steps a second (at least; 2 a frame at 60 fps)
constexpr float kHairGravity = 6.0f;      // units / s^2 (a 1-unit dancer)
constexpr float kHairDrag = 1.5f;         // 1 / s, air drag on the chain points
constexpr float kHairShapeWn = 30.0f;     // rad/s: how hard a chain's root link keeps its rest direction
constexpr float kHairTipSoft = 0.5f;      // the tip link's share of that stiffness (softer down the chain: whip)
constexpr float kHairShapeZeta = 0.25f;   // damping of a link's motion relative to the one above it
constexpr float kHairMaxSpeed = 5.0f;     // units / s: the most a point moves relative to the one above it
constexpr float kHairFollow = 0.65f;
constexpr float kHairCosBend = 0.866f, kHairSinBend = 0.5f;  // a joint bends at most 30 degrees       // below the first link, how much a link carries on its parent's direction
constexpr float kHairConeSoft = 0.3f;  // a link past its cone (HairChainDef::cone) turns this much of the way back a sub-step
constexpr float kLagWn = 22.0f, kLagZeta = 0.3f;    // the volumes' shift behind the head (3.5 Hz)
constexpr float kTurnWn = 15.7f, kTurnZeta = 0.35f;  // and their turn (2.5 Hz)
constexpr float kLagDrag = 2.0f, kTurnDrag = 4.0f;  // 1 / s: air drag on them (a steady spin trails them a little)
constexpr float kLagMax = 0.3f;           // head radii
constexpr float kTurnMax = 0.35f;         // radians

Frame lerpFrame(const Frame& a, const Frame& b, float t) {
    Frame f;
    f.o = lerp(a.o, b.o, t);
    const V3 y = normalize(lerp(a.y, b.y, t), b.y);
    V3 z = lerp(a.z, b.z, t);
    z = normalize(z - y * dot(z, y), b.z);
    f.x = cross(y, z);
    f.y = y;
    f.z = z;
    return f;
}
// The turn from frame a to frame b (world axis times about sin(angle)).
V3 turnBetween(const Frame& a, const Frame& b) {
    return (cross(a.x, b.x) + cross(a.y, b.y) + cross(a.z, b.z)) * 0.5f;
}
V3 rotateBy(V3 v, V3 axisAngle) {
    const float ang = length(axisAngle);
    if (ang < 1e-8f) return v;
    const V3 n = axisAngle * (1.0f / ang);
    const float c = std::cos(ang), s = std::sin(ang);
    return v * c + cross(n, v) * s + n * (dot(n, v) * (1 - c));
}
void orthonormalize(Frame& f) {
    f.y = normalize(f.y);
    f.z = normalize(f.z - f.y * dot(f.z, f.y), {0, 0, 1});
    f.x = cross(f.y, f.z);
}
HairCollider toStage(const HairCollider& c, V3 place, float k) {
    HairCollider w = c;
    w.a = place + c.a * k;
    w.b = place + c.b * k;
    w.r0 = c.r0 * k;
    w.r1 = c.r1 * k;
    return w;
}
HairCollider lerpCollider(const HairCollider& a, const HairCollider& b, float t) {
    HairCollider c = b;
    c.a = lerp(a.a, b.a, t);
    c.b = lerp(a.b, b.b, t);
    c.x = lerp(a.x, b.x, t);
    c.z = lerp(a.z, b.z, t);
    return c;
}
// Where on the link p0-p1 (0..1) it comes closest to the segment q0-q1 (closest points of two segments).
float linkClosest(V3 p0, V3 p1, V3 q0, V3 q1) {
    const V3 d1 = p1 - p0, d2 = q1 - q0, r = p0 - q0;
    const float a = dot(d1, d1), e = dot(d2, d2), f = dot(d2, r);
    if (a < 1e-12f) return 0.0f;
    const float c = dot(d1, r);
    if (e < 1e-12f) return std::clamp(-c / a, 0.0f, 1.0f);
    const float b = dot(d1, d2), den = a * e - b * b;
    float s = den > 1e-12f ? std::clamp((b * f - c * e) / den, 0.0f, 1.0f) : 0.0f;
    const float t = (b * s + f) / e;
    if (t < 0) s = std::clamp(-c / a, 0.0f, 1.0f);
    else if (t > 1) s = std::clamp((b - c) / a, 0.0f, 1.0f);
    return s;
}
HairCollider shrunk(HairCollider c, float by) {
    c.r0 = std::max(0.0f, c.r0 - by);
    c.r1 = std::max(0.0f, c.r1 - by);
    return c;
}
}  // namespace

void DancerRun::hairBody(HairCollider out[kHairColliders]) const { hairColliders(spec_.look, sk_, pose_, out); }

void DancerRun::stepHairSim(double dt) {
    HairSim& S = sim_;
    const float k = scale_;
    if (!hairPhysics() || k < 0.05f) {
        S.set = false;
        hairOut_ = HairState{};
        return;
    }
    if (!S.defsSet) {
        // The chains this look has, as buildBody lays them out (head frame), and how far each passes into the body
        // as designed, at rest (the long hair's middle runs down the back inside the torso's outline, a pigtail past
        // the shoulder, a tail's top along the head): each collider shrunk that much for that chain, so the rest look
        // stays as designed and only what moves into the hair pushes it.
        const DancePose rp = restPose(sk_);
        BodyOut rest;
        HairState restHair;  // (still: the chains straight from root to rest tip)
        restHair.bendLong = longHair_;
        buildBody(spec_.look, sk_, rp, Frame{}, 1.0f, rest, longHair_ ? &restHair : nullptr);
        S.chains = std::min(rest.hairChainCount, kHairChains);
        HairCollider col[kHairColliders];
        hairColliders(spec_.look, sk_, rp, col);
        const Frame h = headFrame(sk_, rp);
        for (int c = 0; c < S.chains; ++c) {
            S.def[c] = rest.hairChains[c];
            const HairChainDef& d = S.def[c];
            for (int j = 0; j < kHairColliders; ++j) {
                float in = 0;
                for (int it = 0; it < 60; ++it, in += 0.005f) {
                    float worst = -1;
                    for (int i = 1; i <= 2 * d.segs; ++i)
                        worst = std::max(worst, colliderDepth(shrunk(col[j], in),
                                                              h.point(lerp(d.a, d.b, float(i) / float(2 * d.segs))), d.radius));
                    if (worst < -0.004f) break;
                }
                S.shrink[c][j] = in;
            }
        }
        S.defsSet = true;
    }
    const V3 place{x_, lift_, z_};
    Frame H = headFrame(sk_, pose_);
    H.o = place + H.o * k;
    HairCollider col[kHairColliders];
    hairColliders(spec_.look, sk_, pose_, col);
    for (HairCollider& c : col) c = toStage(c, place, k);
    const float R = sk_.headR;
    const V3 down{0, -1, 0};
    auto goalDir = [&](const HairChainDef& d, const Frame& hf, float* firm) {
        // The rest direction, bent towards straight down by `droop`. As the head turns over (the rest direction
        // from down to up) the shape gives way to hanging straight down (`firm` 1 -> 0, the pull weaker): upside down
        // the hair falls off the head.
        const V3 rest = hf.dir(normalize(d.b - d.a));
        const float u = std::clamp((dot(rest, down) + 0.2f) / 0.8f, 0.0f, 1.0f);
        const float w = u * u * (3 - 2 * u);
        *firm = 0.35f + 0.65f * w;
        return normalize(normalize(rest + down * d.droop, down) * w + down * (1 - w), down);
    };
    const bool reset = !S.set || dt > 0.1 || length(H.o - S.head.o) > 0.5f * k;
    if (reset) {
        for (int c = 0; c < S.chains; ++c) {
            const HairChainDef& d = S.def[c];
            float firm;
            const V3 dir = goalDir(d, H, &firm);
            const float L = length(d.b - d.a) * k / float(d.segs);
            S.x[c][0] = H.o + H.dir(d.a) * k;
            for (int i = 1; i <= d.segs; ++i) S.x[c][i] = S.x[c][i - 1] + dir * L;
            for (int i = 0; i <= d.segs; ++i) S.xp[c][i] = S.x[c][i];
        }
        S.jp = H.o;
        S.jv = S.lw = V3{};
        S.lag = H;
        S.set = true;
    } else if (std::fabs(S.k - k) > 1e-7f) {
        // A change of size (the entrance pop, the crowd scale): the last step's state resized about its placement, so
        // growing doesn't fling the hair.
        const float r = k / S.k;
        auto rs = [&](V3 p) { return S.place + (p - S.place) * r; };
        for (int c = 0; c < S.chains; ++c)
            for (int i = 0; i <= S.def[c].segs; ++i) {
                S.x[c][i] = rs(S.x[c][i]);
                S.xp[c][i] = rs(S.xp[c][i]);
            }
        S.head.o = rs(S.head.o);
        S.jp = rs(S.jp);
        for (HairCollider& c : S.col) {
            c.a = rs(c.a);
            c.b = rs(c.b);
            c.r0 *= r;
            c.r1 *= r;
        }
    }
    const float h = float(std::clamp(dt, 0.0, 0.1));
    if (!reset && h > 1e-6f) {
        const int n = std::clamp(int(std::ceil(h * kHairRate)), 1, 12);  // sub-steps
        const float sh = h / float(n);
        const Frame H0 = S.head;
        const V3 vHead = (H.o - H0.o) * (1.0f / h), wHead = turnBetween(H0, H) * (1.0f / h);
        const float g = kHairGravity * k;
        const float drag = std::max(0.0f, 1.0f - kHairDrag * sh);
        for (int step = 1; step <= n; ++step) {
            const float f = float(step) / float(n);
            const Frame hs = lerpFrame(H0, H, f);
            HairCollider cs[kHairColliders];
            for (int j = 0; j < kHairColliders; ++j) cs[j] = lerpCollider(S.col[j], col[j], f);
            // The volumes' lag: a point after the head's center, a frame after its turn (springs relative to the
            // head's own motion, so a steady glide or turn leaves only a small lag).
            S.jv = S.jv + ((hs.o - S.jp) * (kLagWn * kLagWn) - (S.jv - vHead) * (2 * kLagZeta * kLagWn) - S.jv * kLagDrag) * sh;
            S.jp = S.jp + S.jv * sh;
            S.lw = S.lw + (turnBetween(S.lag, hs) * (kTurnWn * kTurnWn) - (S.lw - wHead) * (2 * kTurnZeta * kTurnWn) -
                           S.lw * kTurnDrag) * sh;
            S.lag.x = rotateBy(S.lag.x, S.lw * sh);
            S.lag.y = rotateBy(S.lag.y, S.lw * sh);
            S.lag.z = rotateBy(S.lag.z, S.lw * sh);
            orthonormalize(S.lag);
            // The chains.
            for (int c = 0; c < S.chains; ++c) {
                const HairChainDef& d = S.def[c];
                V3* x = S.x[c];
                V3* xp = S.xp[c];
                const float L = length(d.b - d.a) * k / float(d.segs), rad = d.radius * k;
                float firm;
                const V3 dir0 = goalDir(d, hs, &firm);
                // Verlet: inertia, drag, gravity. Each point's motion relative to the one above it is damped (hair
                // is lossy: a swing dies out in a second or so instead of ringing), and held to a sane speed (a pose
                // that jumps flings the hair, it doesn't throw it over the head).
                V3 vel[kHairChainPts];
                vel[0] = (hs.o + hs.dir(d.a) * k) - x[0];
                for (int i = 1; i <= d.segs; ++i) vel[i] = x[i] - xp[i];
                xp[0] = x[0];
                x[0] = x[0] + vel[0];
                const float vmax = kHairMaxSpeed * k * sh;
                for (int i = 1; i <= d.segs; ++i) {
                    const float soft = d.segs > 1 ? float(i - 1) / float(d.segs - 1) : 0.0f;
                    const float wn = kHairShapeWn * d.stiff * (1.0f - (1.0f - kHairTipSoft) * soft);
                    V3 rel = vel[i] - vel[i - 1];
                    rel = rel * std::max(0.0f, 1.0f - 2.0f * kHairShapeZeta * d.damp * wn * sh);
                    const float rm = length(rel);
                    if (rm > vmax) rel = rel * (vmax / rm);
                    vel[i] = vel[i - 1] + rel;
                    xp[i] = x[i];
                    x[i] = x[i] + vel[i] * drag + down * (g * sh * sh);
                }
                for (int i = 1; i <= d.segs; ++i) {
                    // The shape: towards the rest direction (below the first link partly carrying on the parent's
                    // direction, so a bend travels down the chain), softer towards the tip.
                    V3 dir = dir0;
                    if (i >= 2)
                        dir = normalize(dir0 * (1 - kHairFollow) + normalize(x[i - 1] - x[i - 2], dir0) * kHairFollow, dir0);
                    const float soft = d.segs > 1 ? float(i - 1) / float(d.segs - 1) : 0.0f;
                    const float wn = kHairShapeWn * d.stiff * (1.0f - (1.0f - kHairTipSoft) * soft) / std::sqrt(hairAmt_);
                    const float s = std::min(0.5f, wn * wn * sh * sh) * firm;
                    x[i] = x[i] + ((x[i - 1] + dir * L) - x[i]) * s;
                }
                // The colliders this chain can reach this step (a quick test against the whole chain's reach).
                HairCollider near[kHairColliders];
                bool nearArm[kHairColliders] = {};
                int nearN = 0;
                const float reach = L * float(d.segs) + rad;
                for (int j = 0; j < kHairColliders; ++j) {
                    if (j == kColHead && !d.head) continue;
                    const HairCollider cj = shrunk(cs[j], S.shrink[c][j] * k);
                    const V3 ab = cj.b - cj.a;
                    const float t = std::clamp(dot(x[0] - cj.a, ab) / std::max(dot(ab, ab), 1e-12f), 0.0f, 1.0f);
                    const float spread = (j == kColTorso ? std::max(cj.sx, cj.sz) : 1.0f) * std::max(cj.r0, cj.r1);
                    if (length(x[0] - (cj.a + ab * t)) < reach + spread) {
                        nearArm[nearN] = j >= kColArms;
                        near[nearN++] = cj;
                    }
                }
                // Pushed out: no velocity into the body, a little friction along it.
                auto push = [&](int at, V3 nrm, float by) {
                    x[at] = x[at] + nrm * by;
                    const V3 v = x[at] - xp[at];
                    const float vn = dot(v, nrm);
                    xp[at] = x[at] - ((v - nrm * vn) * 0.8f + nrm * std::max(0.0f, vn));
                };
                // The link from the point above to point i (an arm can pass between two points), where it comes
                // closest to each collider's axis, then the point itself. The first link's upper half grows out of
                // the head and lies on it.
                auto collideLink = [&](int i) {
                    for (int j = 0; j < nearN; ++j) {
                        // (A long column's first link hangs by the shoulder: an arm raised there passes through it
                        // rather than flick the column round from its root.)
                        if (i == 1 && d.cone > 0 && nearArm[j]) continue;
                        const HairCollider& cj = near[j];
                        float u = linkClosest(x[i - 1], x[i], cj.a, cj.b);
                        if (i == 1) u = std::max(u, 0.5f);
                        for (const float at : {u, 1.0f}) {
                            V3 nrm;
                            const float depth = colliderDepth(cj, lerp(x[i - 1], x[i], at), rad, &nrm);
                            if (depth <= 0) continue;
                            if (i == 1) {  // the root is fixed: turn the link about it
                                push(i, nrm, std::min(depth / at, 3.0f * depth));
                            } else {  // move both ends, so the point on the link moves out by `depth`
                                const float w = (1 - at) * (1 - at) + at * at;
                                push(i - 1, nrm, depth * (1 - at) / w);
                                push(i, nrm, depth * at / w);
                            }
                        }
                    }
                };
                for (int i = 1; i <= d.segs; ++i) {
                    // Lengths (follow the leader, root to tip), no sharp kinks (a joint bends at most 30 degrees: hair
                    // curves, it doesn't fold), then out of the body and above the floor.
                    V3 dirI = normalize(x[i] - x[i - 1], dir0);
                    if (i >= 2) {
                        const V3 up = normalize(x[i - 1] - x[i - 2], dir0);
                        const float c = dot(dirI, up);
                        if (c < kHairCosBend) {
                            const V3 side = normalize(dirI - up * c, std::fabs(up.x) < 0.9f ? V3{1, 0, 0} : V3{0, 0, 1});
                            dirI = up * kHairCosBend + side * kHairSinBend;
                        }
                    }
                    if (d.cone > 0) {  // within the cone about the rest direction (head frame)
                        const V3 rd = hs.dir(normalize(d.b - d.a));
                        // (Upside down the cone opens, more so near the root: the hair falls off the head.)
                        const float inv = (1.0f - firm) / 0.65f, f = float(i) / float(d.segs);
                        const float most = std::min(2.9f, d.cone * (1.0f + 1.1f * inv)) * (f + (1.0f - f) * 0.5f * inv);
                        const float c = dot(dirI, rd);
                        if (c < std::cos(most)) {
                            const V3 side = normalize(dirI - rd * c, std::fabs(rd.x) < 0.9f ? V3{1, 0, 0} : V3{0, 0, 1});
                            dirI = normalize(lerp(dirI, rd * std::cos(most) + side * std::sin(most), kHairConeSoft), rd);
                        }
                    }
                    x[i] = x[i - 1] + dirI * L;
                    collideLink(i);
                    const float above = std::max(rad, d.floor * k);
                    if (x[i].y < above) {
                        x[i].y = above;
                        xp[i].y = std::min(xp[i].y, above);
                    }
                }
                // Once more, tip to root, collisions only: a push further down can move a point back in.
                for (int i = d.segs; i >= 1; --i) collideLink(i);
            }
        }
        // Held to a sensible lag (a spin trails the volumes, it doesn't wind them round).
        const V3 lt = S.jp - H.o;
        const float lm = length(lt), lmax = kLagMax * R * k;
        if (lm > lmax) S.jp = H.o + lt * (lmax / lm);
        const V3 tr = turnBetween(H, S.lag);
        const float tm = length(tr);
        if (tm > std::sin(kTurnMax)) {
            const V3 want = tr * (kTurnMax / std::max(tm, 1e-6f));
            S.lag.x = rotateBy(H.x, want);
            S.lag.y = rotateBy(H.y, want);
            S.lag.z = rotateBy(H.z, want);
        }
    }
    S.head = H;
    S.place = place;
    S.k = k;
    for (int j = 0; j < kHairColliders; ++j) S.col[j] = col[j];
    // Out: character space (as buildBody's pose and placement), the lag in the head frame.
    hairOut_.chains = S.chains;
    hairOut_.bendLong = longHair_;
    for (int c = 0; c < S.chains; ++c)
        for (int i = 0; i <= S.def[c].segs; ++i) hairOut_.pts[c][i] = (S.x[c][i] - place) * (1.0f / k);
    const V3 lt = (S.jp - H.o) * (1.0f / k), tr = turnBetween(H, S.lag);
    // (Hair 2: twice the lag, still held to the limits.)
    const float more = std::min(hairAmt_, 2.0f);
    hairOut_.lagT = V3{dot(lt, H.x), dot(lt, H.y), dot(lt, H.z)} * more;
    hairOut_.lagR = V3{dot(tr, H.x), dot(tr, H.y), dot(tr, H.z)} * more;
}

void DancerRun::stepFeet(double beat, double dt, float moving) {
    // Body frame <-> stage floor. buildBody: the body frame is the placement (x_, z_, scale_) moved by (bodyX, bodyZ)
    // and turned by bodyYaw (local x -> (cos, -sin), local z -> (sin, cos) in stage x, z).
    const float c = std::cos(pose_.bodyYaw), s = std::sin(pose_.bodyYaw);
    const float k = std::max(scale_, 0.05f);
    auto toFloor = [&](V3 f) {
        return std::pair<float, float>{x_ + k * (f.x * c + f.z * s + pose_.bodyX),
                                       z_ + k * (-f.x * s + f.z * c + pose_.bodyZ)};
    };
    auto toBody = [&](float wx, float wz) {
        const float dx = (wx - x_) / k - pose_.bodyX, dz = (wz - z_) / k - pose_.bodyZ;
        return std::pair<float, float>{dx * c - dz * s, dx * s + dz * c};
    };
    V3* foot[2] = {&pose_.footL, &pose_.footR};
    std::pair<float, float> home[2] = {toFloor(*foot[0]), toFloor(*foot[1])};
    // Walking when traveling (not while popping in or out); eased in and out.
    const float want = (moving > 0.12f && scale_ > 0.6f && pose_.ground < 0.5f) ? 1.0f : 0.0f;  // (not on the floor)
    walk_ += (want - walk_) * float(std::min(1.0, dt * 4.0));
    const bool anySwing = feet_[0].swinging || feet_[1].swinging;
    if (!feetPlaced_ || (walk_ < 0.02f && !anySwing)) {
        for (int i = 0; i < 2; ++i) {
            feet_[i].x = home[i].first;
            feet_[i].z = home[i].second;
            feet_[i].swinging = false;
            feet_[i].lift = 0;
        }
        feetPlaced_ = true;
        lastStepBeat_ = beat;
        return;
    }
    // Where a foot should land: under the hip as the dance wants it, a little ahead in the direction of travel.
    const float lead = 0.22f;
    std::pair<float, float> aim[2];
    for (int i = 0; i < 2; ++i) aim[i] = {home[i].first + vx_ * lead, home[i].second + vz_ * lead};
    auto dist = [](std::pair<float, float> a, float x, float z) {
        return std::sqrt((a.first - x) * (a.first - x) + (a.second - z) * (a.second - z));
    };
    // Lost track (a spin, a jump in the clock): put the feet back under the body.
    for (int i = 0; i < 2; ++i)
        if (dist(home[i], feet_[i].x, feet_[i].z) > 0.5f * k) {
            feet_[i] = Foot{};
            feet_[i].x = home[i].first;
            feet_[i].z = home[i].second;
        }
    // Swinging feet travel to their aim in 0.4 beats with a lift.
    const double stepBeats = 0.4;
    for (int i = 0; i < 2; ++i) {
        Foot& f = feet_[i];
        if (!f.swinging) continue;
        double prog = (beat - f.swingBeat) / stepBeats;
        if (prog < 0) prog = 1;  // the clock went back
        const float t = float(std::clamp(prog, 0.0, 1.0)), e = t * t * (3 - 2 * t);
        f.x = f.fromX + (aim[i].first - f.fromX) * e;
        f.z = f.fromZ + (aim[i].second - f.fromZ) * e;
        f.lift = 0.06f * std::sin(3.14159265f * t);
        if (prog >= 1) {
            f.swinging = false;
            f.lift = 0;
        }
    }
    // A new step on each 8th note: the foot furthest from its aim, if it is far enough.
    if (std::floor(beat * 2) > std::floor(lastStepBeat_ * 2) && !feet_[0].swinging && !feet_[1].swinging) {
        const float d0 = dist(aim[0], feet_[0].x, feet_[0].z), d1 = dist(aim[1], feet_[1].x, feet_[1].z);
        const int i = d0 >= d1 ? 0 : 1;
        if (std::max(d0, d1) > 0.03f * k) {
            Foot& f = feet_[i];
            f.swinging = true;
            f.swingBeat = std::floor(beat * 2) / 2;  // on the 8th
            f.fromX = f.x;
            f.fromZ = f.z;
            ++steps_;
        }
    }
    lastStepBeat_ = beat;
    // The feet: planted / stepping, blended with the dance's own footwork by how much the dancer is walking.
    for (int i = 0; i < 2; ++i) {
        const float wx = home[i].first + (feet_[i].x - home[i].first) * walk_;
        const float wz = home[i].second + (feet_[i].z - home[i].second) * walk_;
        const auto b = toBody(wx, wz);
        foot[i]->x = b.first;
        foot[i]->z = b.second;
        foot[i]->y += feet_[i].lift * walk_ / k;
    }
}

void DancerRun::pinContacts(double dt) {
    // Planted contacts (2026-11-27, docs/dancers-design.md "Floor moves held still"): a hand, foot, knee or the head the
    // source dancer had planted is held where it came down (stage x, z): the feet's and hands' targets moved back to
    // it, the knees and head (and hands out of reach) by shifting the whole body (at most kMaxShift). A plant takes
    // hold where the contact is when it starts (no jump), lets go with the source's weight, and gives up (eased out)
    // where it can't be held.
    constexpr float kMaxShift = 0.15f, kShiftSpeed = 0.6f, kHandGiveUp = 0.08f, kFootGiveUp = 0.12f, kRootGiveUp = 0.2f;
    const float fdt = float(std::clamp(dt, 1e-4, 0.1));
    const float g = std::clamp(pose_.ground, 0.0f, 1.0f);
    const float src[7] = {pose_.pinHandL, pose_.pinHandR, pose_.pinFootL, pose_.pinFootR,
                          pose_.pinKneeL, pose_.pinKneeR, pose_.pinHead};
    const float k = std::max(scale_, 0.05f);
    bool any = std::fabs(rootX_) + std::fabs(rootZ_) > 1e-6f;
    for (int i = 0; i < 7; ++i) any = any || src[i] * g > 0.02f || pins_[i].on;
    if (!any) return;
    auto applyShift = [&](float dx, float dz) {  // stage units
        pose_.bodyX += dx / k;
        pose_.bodyZ += dz / k;
    };
    applyShift(rootX_, rootZ_);
    // Where each contact is now (stage x, z): the hand and foot targets, the knees and the head as the rig puts them.
    auto stageOf = [&](V3 q) { return std::pair<float, float>{x_ + k * q.x, z_ + k * q.z}; };
    auto bodyPoint = [&](V3 f) {
        const float c = std::cos(pose_.bodyYaw), sn = std::sin(pose_.bodyYaw);
        return V3{pose_.bodyX + f.x * c + f.z * sn, f.y, pose_.bodyZ - f.x * sn + f.z * c};
    };
    auto where = [&](int i) {
        switch (i) {
            case 0: return stageOf(chestFrame(sk_, pose_).point(pose_.handL));
            case 1: return stageOf(chestFrame(sk_, pose_).point(pose_.handR));
            case 2: return stageOf(bodyPoint(pose_.footL));
            case 3: return stageOf(bodyPoint(pose_.footR));
            case 4: return stageOf(kneeAt(sk_, pose_, 0));
            case 5: return stageOf(kneeAt(sk_, pose_, 1));
            default: return stageOf(headFrame(sk_, pose_).o);
        }
    };
    float w[7];
    for (int i = 0; i < 7; ++i) {
        Pin& p = pins_[i];
        const float want = src[i] * g;
        if (want < 0.02f) {  // let go (the source's weight eased it out)
            p = Pin{};
            w[i] = 0;
            continue;
        }
        if (!p.on && !p.gaveUp) {
            const auto at = where(i);
            p.on = true;
            p.x = at.first;
            p.z = at.second;
            p.amt = 1;
        }
        if (p.gaveUp) p.amt = std::max(0.0f, p.amt - fdt / 0.15f);
        w[i] = p.on ? want * p.amt : 0.0f;
        if (p.on && p.gaveUp && p.amt <= 0) p.on = false;
    }
    // The body's shift: the knees and the head.
    {
        float ex = 0, ez = 0, ws = 0;
        for (int i = 4; i < 7; ++i) {
            if (w[i] <= 0) continue;
            const auto at = where(i);
            const float dx = pins_[i].x - at.first, dz = pins_[i].z - at.second;
            if (!pins_[i].gaveUp && std::hypot(dx, dz) > kRootGiveUp) pins_[i].gaveUp = true;
            ex += w[i] * dx;
            ez += w[i] * dz;
            ws += w[i];
        }
        float dx, dz;
        if (ws > 0) {
            dx = ex / std::max(1.0f, ws);
            dz = ez / std::max(1.0f, ws);
        } else {  // nothing held by the body: the shift eases back
            const float back = 1 - std::exp(-fdt / 0.4f);
            dx = -rootX_ * back;
            dz = -rootZ_ * back;
        }
        const float step = std::hypot(dx, dz), most = (ws > 0 ? kShiftSpeed : 0.3f) * fdt;
        if (step > most) dx *= most / step, dz *= most / step;
        float nx = rootX_ + dx, nz = rootZ_ + dz;
        const float len = std::hypot(nx, nz);
        if (len > kMaxShift) nx *= kMaxShift / len, nz *= kMaxShift / len;
        applyShift(nx - rootX_, nz - rootZ_);
        rootX_ = nx;
        rootZ_ = nz;
    }
    // Feet: back to where they came down (body frame).
    const float c = std::cos(pose_.bodyYaw), sn = std::sin(pose_.bodyYaw);
    for (int side = 0; side < 2; ++side) {
        const int i = 2 + side;
        if (w[i] <= 0) continue;
        const auto at = where(i);
        const float dx = (pins_[i].x - at.first) / k, dz = (pins_[i].z - at.second) / k;
        if (!pins_[i].gaveUp && std::hypot(dx, dz) * k > kFootGiveUp) pins_[i].gaveUp = true;
        V3& f = side == 0 ? pose_.footL : pose_.footR;
        f.x += (dx * c - dz * sn) * w[i];
        f.z += (dx * sn + dz * c) * w[i];
    }
    // Hands: the target moved back in the chest frame, within the arm's reach.
    for (int side = 0; side < 2; ++side) {
        if (w[side] <= 0) continue;
        const Frame chest = chestFrame(sk_, pose_);
        const auto at = where(side);
        const V3 e{(pins_[side].x - at.first) / k * w[side], 0, (pins_[side].z - at.second) / k * w[side]};
        V3& h = side == 0 ? pose_.handL : pose_.handR;
        V3 nh = h + V3{dot(e, chest.x), dot(e, chest.y), dot(e, chest.z)};
        const V3 shoulder{(side == 0 ? 1.0f : -1.0f) * sk_.shoulderHalf, sk_.shoulderY, 0};
        const float reach = 0.995f * (sk_.upperArm + sk_.forearm);
        const V3 r = nh - shoulder;
        if (length(r) > reach) nh = shoulder + r * (reach / length(r));
        const V3 lost = chest.point(nh) - chest.point(h) - e;  // what the arm couldn't reach
        if (!pins_[side].gaveUp && std::hypot(lost.x, lost.z) * k > kHandGiveUp) pins_[side].gaveUp = true;
        h = nh;
    }
}

void DancerRun::floorInfo(MinglePerson& p, double beat) const {
    p.floor = 0;
    if (leaving()) return;
    const bool on = pose_.ground >= 0.3f || floorHold_ > 0.05f;
    if (on) {
        p.floor = 2;
        p.reach = reachNow_;
        return;
    }
    if (choreo_.bustMoveAt(danceBeat(beat), true)) {
        p.floor = 1;
        p.reach = bustMoveReach().scaled(legRatio_);
        return;
    }
    const int m = choreo_.floorPending();
    if (m >= 0 && moveLibrary()[std::size_t(m)].clip >= 0) {
        p.floor = 1;
        p.reach = clipInfo(moveLibrary()[std::size_t(m)].clip).reach.scaled(legRatio_);
    }
}

float floorNeed(const FloorReach& r, float facing, float dx, float dz, float pad) {
    // Angle from the floor dancer's front (+z turned by `facing`) towards (dx, dz), + towards their left (+x).
    const float a = std::atan2(dx, dz) - facing;
    return r.toward(a) + pad;
}

void DancerRun::build(BodyOut& out) const {
    if (scale_ <= 1e-3f) return;
    Frame place;
    place.o = {x_, lift_, z_};
    buildBody(spec_.look, sk_, pose_, place, scale_, out, hairPhysics() && sim_.set ? &hairOut_ : nullptr);
}

std::string DancerRun::describe(double) const { return desc_; }

}  // namespace reforce::engine
