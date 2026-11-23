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

const char* socialModeName(SocialMode m) {
    switch (m) {
        case SocialMode::Show: return "show";
        case SocialMode::Partner: return "partners";
        case SocialMode::Call: return "call & response";
        case SocialMode::Side: return "face to face";
        case SocialMode::Cypher: return "cypher";
        case SocialMode::Mingle: return "mingling";
    }
    return "?";
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

SocialMode pickSocialMode(const std::string& setting, int dancers, long segment, std::uint32_t seed) {
    if (setting == "mingle") return SocialMode::Mingle;
    if (setting == "off" || dancers < 2) return SocialMode::Show;
    if (setting == "partners") return SocialMode::Partner;
    if (setting == "call") return SocialMode::Call;
    if (setting == "facing") return SocialMode::Side;
    if (setting == "cypher") return dancers >= 3 ? SocialMode::Cypher : SocialMode::Call;
    auto hashOf = [&](long seg) { return seed * 0x9e3779b9U + std::uint32_t(seg) * 0x85ebca6bU; };
    const std::uint32_t h = hashOf(segment);
    // Auto: a third of the draws are "show", but never two show segments in a row (about 3/4 of the time social).
    const bool drawShow = unit32(h) >= 0.65f, prevDrawShow = unit32(hashOf(segment - 1)) >= 0.65f;
    if (setting != "always" && drawShow && !prevDrawShow) return SocialMode::Show;
    const float u = unit32(h + 1), v = unit32(h + 2);
    if (dancers >= 3 && u < 0.3f) return SocialMode::Cypher;
    return v < 0.4f ? SocialMode::Partner : v < 0.75f ? SocialMode::Call : SocialMode::Side;
}

std::vector<std::pair<float, float>> scatterSpots(int n, const StageCamera& cam, double bar, std::uint32_t seed,
                                                  float minDist, int people) {
    std::vector<std::pair<float, float>> p(std::size_t(std::max(0, n)));
    if (n <= 0) return p;
    if (people < 0) people = n;
    // Room: most of the visible width (less for few people, so they stay near the middle), the floor's depth.
    const float W = float(cam.halfWidth) * std::min(0.92f, 0.35f + 0.11f * float(people));
    for (int k = 0; k < n; ++k) {
        // Each wanders on its own schedule: a new spot every 3-5 bars, offset per person.
        const float every = 3.0f + 2.0f * unit32(seed * 31u + std::uint32_t(k) * 7919u);
        const double shifted = bar + double(k) * 1.37;
        const auto period = std::uint32_t(std::max(0.0, std::floor(shifted / every)));
        const std::uint32_t h = seed * 0x9e3779b9U ^ (std::uint32_t(k) * 0x85ebca6bU) ^ (period * 0xc2b2ae35U);
        float z = cam.floorBack + 0.1f + unit32(h + 1) * (kFloorZMax - cam.floorBack - 0.15f);
        float x = (unit32(h) * 2 - 1) * W * floorWiden(cam, z);  // the floor is wider further back
        // ... and drifts a little all the time.
        const float ph = 6.2831853f * unit32(h + 2);
        x += 0.07f * std::sin(float(bar) * 0.45f + ph);
        z += 0.05f * std::sin(float(bar) * 0.33f + ph * 1.7f);
        p[std::size_t(k)] = {x, z};
    }
    // Push apart anyone closer than minDist (deterministic relaxation), keep inside the room.
    for (int it = 0; it < 24; ++it) {
        bool moved = false;
        for (int i = 0; i < n; ++i)
            for (int j = i + 1; j < n; ++j) {
                auto& a = p[std::size_t(i)];
                auto& b = p[std::size_t(j)];
                float dx = b.first - a.first, dz = (b.second - a.second) * 0.5f;  // depth counts half
                float d = std::sqrt(dx * dx + dz * dz);
                if (d >= minDist) continue;
                if (d < 1e-4f) {
                    dx = 1;
                    dz = 0;
                    d = 1;
                }
                const float push = (minDist - d) * 0.5f / d;
                a.first -= dx * push;
                a.second -= dz * push;
                b.first += dx * push;
                b.second += dz * push;
                moved = true;
            }
        for (auto& q : p) {
            q.second = std::clamp(q.second, cam.floorBack, kFloorZMax);
            const float wx = float(cam.halfWidth) * 0.92f * floorWiden(cam, q.second);  // as the spots: wider at the back
            q.first = std::clamp(q.first, -wx, wx);
        }
        if (!moved) break;
    }
    return p;
}

std::vector<StageSpot> planStage(int n, const StageCamera& cam, SocialMode mode, double bar, std::uint32_t seed) {
    std::vector<StageSpot> out(std::size_t(std::max(0, n)));
    if (n <= 0) return out;
    const auto turn = long(std::floor(std::max(0.0, bar) / 2.0));  // whose turn: every 2 bars
    if (mode == SocialMode::Show || mode == SocialMode::Mingle || n < 2) {  // Mingle: engine::Mingle plans it
        const auto spots = scatterSpots(n, cam, bar, seed, 0.75f);
        for (int k = 0; k < n; ++k) {
            StageSpot& s = out[std::size_t(k)];
            s.x = spots[std::size_t(k)].first;
            s.z = spots[std::size_t(k)].second;
            // Mostly towards the audience, each turned a little their own way (changing every few bars).
            const auto period = std::uint32_t(std::max(0.0, std::floor((bar + k * 2.1) / 4.0)));
            s.facing = -s.x * 0.1f + 0.45f * (unit32(seed ^ (std::uint32_t(k) * 0x27d4eb2fU) ^ (period * 0x165667b1U)) - 0.5f);
        }
        return out;
    }
    if (mode == SocialMode::Cypher) {
        const int solo = int(turn % n);
        const int m = n - 1;
        const float sx = 0, sz = 0.2f;
        const float rx = std::min(0.85f * float(cam.halfWidth), 0.42f * float(m) + 0.2f);
        int k = 0;
        for (int i = 0; i < n; ++i) {
            StageSpot& s = out[std::size_t(i)];
            if (i == solo) {
                s.x = sx;
                s.z = sz;
                s.role = "solo";
                continue;
            }
            const float a = m == 1 ? 0.0f : -1.05f + 2.1f * float(k) / float(m - 1);
            ++k;
            const float wob = 6.2831853f * unit32(seed + std::uint32_t(i) * 101u);
            s.x = rx * std::sin(a) + 0.05f * std::sin(float(bar) * 0.5f + wob);
            s.z = -0.4f - 0.45f * std::cos(a) + 0.04f * std::sin(float(bar) * 0.37f + wob * 1.3f);
            s.facing = yawTowards(s.x, s.z, sx, sz);
            s.react = true;
            s.other = solo;
            s.look = 0.15f * (sx > s.x ? 1.0f : -1.0f);
            s.role = "cheering";
        }
        return out;
    }
    // Pairs (and a trio for an odd count), groups left to right.
    std::vector<std::vector<int>> groups;
    int i = 0;
    while (i < n) {
        const int size = (n - i == 3) ? 3 : 2;
        std::vector<int> g;
        for (int j = 0; j < size && i < n; ++j) g.push_back(i++);
        groups.push_back(g);
    }
    if (groups.back().size() == 1) groups[groups.size() - 2].push_back(groups.back()[0]), groups.pop_back();
    // Groups mill around too (scattered, far enough apart that pairs read as pairs), each at its own tilt.
    const auto centers = scatterSpots(int(groups.size()), cam, bar, seed ^ 0xabcdu, 1.25f, n);
    for (std::size_t gi = 0; gi < groups.size(); ++gi) {
        const auto& g = groups[gi];
        const float cx = centers[gi].first, cz = centers[gi].second;
        const float half = 0.25f;
        // Places: a pair side by side; a trio adds one at the back. Turned by the group's tilt (every 4 bars).
        std::vector<std::pair<float, float>> local = {{-half, 0.0f}, {half, 0.0f}};
        if (g.size() == 3) local = {{-0.31f, 0.1f}, {0.31f, 0.1f}, {0.0f, -0.27f}};
        const auto tp = std::uint32_t(std::max(0.0, std::floor((bar + double(gi) * 1.9) / 4.0)));
        const float tilt = 0.75f * (unit32(seed * 13u + std::uint32_t(gi) * 977u + tp * 7717u) * 2 - 1);
        std::vector<std::pair<float, float>> place;
        for (const auto& l : local)
            place.push_back({cx + l.first * std::cos(tilt) - l.second * std::sin(tilt),
                             std::clamp(cz + l.first * std::sin(tilt) + l.second * std::cos(tilt), cam.floorBack, kFloorZMax)});
        float mx = 0, mz = 0;
        for (const auto& p : place) {
            mx += p.first / float(place.size());
            mz += p.second / float(place.size());
        }
        const int caller = g[std::size_t(turn % long(g.size()))];
        for (std::size_t j = 0; j < g.size(); ++j) {
            StageSpot& s = out[std::size_t(g[j])];
            s.x = place[j].first;
            s.z = place[j].second;
            // Face the partner (a pair) or the middle of the trio.
            const float tx = g.size() == 2 ? place[1 - j].first : mx, tz = g.size() == 2 ? place[1 - j].second : mz;
            s.facing = yawTowards(s.x, s.z, tx, tz);
            s.other = g.size() == 2 ? g[1 - j] : g[(j + 1) % g.size()];
            s.look = 0.12f * (tx > s.x ? 1.0f : tx < s.x ? -1.0f : 0.0f);
            switch (mode) {
                case SocialMode::Partner:
                    s.leader = g[0];
                    s.mirror = j == 1;
                    s.role = "with";
                    break;
                case SocialMode::Call:
                    if (g[j] == caller) {
                        s.facing *= 0.5f;  // half towards the audience: showing off to both
                        s.role = "showing";
                        s.other = g[(j + 1) % g.size()];
                    } else {
                        s.react = true;
                        s.other = caller;
                        s.role = "watching";
                    }
                    break;
                default: s.role = "facing"; break;
            }
        }
    }
    return out;
}

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
float groupRadius(std::size_t n) { return n <= 2 ? 0.26f : n == 3 ? 0.34f : 0.4f; }
constexpr std::size_t kMaxGroup = 4;
}  // namespace

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
        if (a.id == id) return a.tr;
    return {};
}

float Mingle::roomHalfWidth(const StageCamera& cam) const {
    // As scatterSpots: most of the visible width (less for few people, so they stay near the middle).
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

void Mingle::decide(Agent& a, const StageCamera& cam, double beat) {
    const float soc = a.tr.sociable, rest = a.tr.restless;
    if (a.group) {
        // Time to move on: wander off, or straight over to someone else (the sociable ones).
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
    for (auto& o : agents_) {
        if (o.id == a.id || o.approachedBy) continue;
        const float d = std::hypot(o.x - a.x, o.z - a.z);
        if (d > 2.4f) continue;
        if (o.act == Act::Solo || o.act == Act::Show || o.act == Act::Wander || o.act == Act::Rest)
            free.push_back({(0.4f + o.tr.sociable) / (0.3f + d), &o});
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
        if (g.members.size() + pending >= kMaxGroup) continue;
        const float d = std::hypot(g.cx - a.x, g.cz - a.z);
        if (d < 2.6f) open.push_back({1.0f / (0.4f + d), &g});
    }
    auto pickOne = [&](auto& v) {
        float sum = 0;
        for (const auto& c : v) sum += c.first;
        float r = rnd(a.rng) * sum;
        for (const auto& c : v)
            if ((r -= c.first) <= 0) return c.second;
        return v.back().second;
    };
    const float wSeek = free.empty() ? 0 : 0.25f + 1.1f * soc;
    const float wJoin = open.empty() ? 0 : 0.15f + 0.6f * soc;
    const float wWatch = showing.empty() ? 0 : 0.3f + 0.6f * soc;
    const float wWander = 0.3f + 0.7f * rest;
    const float wRest = 0.12f * (1.3f - rest);
    const float wShow = 0.08f + 0.45f * a.tr.showy;
    const float wSolo = 0.3f;
    float r = rnd(a.rng) * (wSeek + wJoin + wWatch + wWander + wRest + wShow + wSolo);
    const float stay = 1.35f - 0.7f * rest;  // the restless move on sooner
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
        a.sx = o->x + side * 0.5f;
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
            a->tr.sociable = rnd(a->rng);
            a->tr.restless = rnd(a->rng);
            a->tr.showy = rnd(a->rng);
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
            enter(*a, Act::Solo, rndIn(a->rng, 0.5f, 3.0f), beat);  // dance a moment where they popped in
        }
        a->seen = true;
        if (p.placed) {
            a->x = p.x;
            a->z = p.z;
        }
        a->facing = p.facing;
        a->placed = p.placed;
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
        if (a.act == Act::Seek) {
            Agent* o = a.target ? find(a.target) : nullptr;
            Group* g = a.targetGroup ? findGroup(a.targetGroup) : nullptr;
            if (o && o->group) {  // they got together with someone else first: join them instead
                g = findGroup(o->group);
                if (o->approachedBy == a.id) o->approachedBy = 0;
                a.target = 0;
                a.targetGroup = g && g->members.size() < kMaxGroup ? g->id : 0;
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
                a.sx = o->x + dx * 0.52f;
                a.sz = std::clamp(o->z + dz * 0.52f, cam.floorBack, kFloorZMax);
                if (d < 0.66f) {
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
                        const double bars = 4 + 12 * rnd(m->rng) * (0.4f + 0.6f * m->tr.sociable);
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
                    const double bars = 3 + 10 * rnd(a.rng) * (0.4f + 0.6f * a.tr.sociable);
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
            enter(a, Act::Solo, rndIn(a.rng, 1.0f, 4.0f) * (1.35f - 0.7f * a.tr.restless), beat);
            continue;
        }
        if (a.act == Act::Watch) {
            const Agent* o = find(a.target);
            if (!o || o->act != Act::Show) a.until = std::min(a.until, clock_);
        }
        if (clock_ >= a.until) decide(a, cam, beat);
    }

    // Groups: their own clocks. A pair may change how it dances at its own 8-bar marks; groups drift a little.
    for (auto& g : groups_) {
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
                const float gap = rndIn(a.rng, 1.5f, 6.0f) * (1.4f - 0.6f * a.tr.sociable) * (a.group ? 2.0f : 1.0f);
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
                const float cx = std::clamp(g->cx + 0.06f * std::sin(bar * 0.4f + g->phase), -W, W);
                const float cz = std::clamp(g->cz + 0.04f * std::sin(bar * 0.3f + g->phase * 1.3f),
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
        const float wx = float(cam.halfWidth) * 0.92f * floorWiden(cam, s.z);
        s.x = std::clamp(s.x, -wx, wx);
        s.z = std::clamp(s.z, cam.floorBack, kFloorZMax);
        a.outX = s.x;
        a.outZ = s.z;
        if (s.other == int(k)) s.other = -1;
        if (s.leader == int(k)) s.leader = -1;
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
        case H::Bun: case H::SidePony: case H::SpaceBuns:
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
         {H::Bun, 1.2f}, {H::Braid, 1}, {H::Pixie, 1}, {H::SidePony, 0.8f}, {H::Pigtails, 0.8f}, {H::TopKnot, 0.6f}});
    add({{H::Short, 3}, {H::SidePart, 2}, {H::Quiff, 1.5f}, {H::Buzz, 1.5f}, {H::Undercut, 1.2f}, {H::Shaved, 0.8f},
         {H::SlickBack, 0.8f}, {H::Waves, 0.6f}, {H::Balding, 0.6f}, {H::ManBun, 0.5f}, {H::Bald, 0.4f},
         {H::Mullet, 0.2f}, {H::Bowl, 0.2f}});
    add({{H::Curly, 1.2f}, {H::Coils, 0.8f}, {H::Afro, 0.8f}, {H::Curtains, 0.6f}, {H::Shag, 0.6f}, {H::Anime, 0.5f},
         {H::Spiky, 0.3f}, {H::Mohawk, 0.2f}});
    // Each style's flavor.
    switch (style) {
        case St::Disco: add({{H::Afro, 3}, {H::Curly, 2}, {H::Shag, 1.5f}, {H::Long, 1}, {H::Layers, 0.5f}, {H::TopKnot, 0.5f}, {H::Pompadour, 0.5f}}); break;
        case St::HipHop: add({{H::Coils, 2}, {H::Waves, 2}, {H::Afro, 1.5f}, {H::Braid, 1.5f}, {H::Buzz, 1}, {H::Bald, 1}, {H::SpaceBuns, 0.8f}, {H::Pigtails, 0.5f}, {H::Undercut, 0.5f}}); break;
        case St::Pop: add({{H::Ponytail, 1}, {H::Anime, 1}, {H::Pigtails, 0.8f}, {H::SidePony, 0.8f}, {H::SpaceBuns, 0.8f}, {H::Quiff, 0.8f}, {H::Bangs, 0.6f}, {H::Pixie, 0.6f}, {H::Curtains, 0.6f}}); break;
        case St::Robot: add({{H::SlickBack, 1.5f}, {H::Bald, 1}, {H::Bowl, 1}, {H::Buzz, 1}, {H::Shaved, 1}, {H::Bun, 0.8f}, {H::Bob, 0.6f}, {H::Ponytail, 0.6f}, {H::Spiky, 0.6f}, {H::Mohawk, 0.4f}}); break;
        case St::Rave: add({{H::SpaceBuns, 1.5f}, {H::Pigtails, 1.2f}, {H::Spiky, 1}, {H::Mohawk, 1}, {H::Anime, 1}, {H::Pixie, 0.8f}, {H::Undercut, 0.8f}, {H::Shaved, 0.5f}}); break;
        case St::Retro: add({{H::Afro, 3}, {H::Shag, 2}, {H::Pompadour, 2}, {H::Mullet, 1.2f}, {H::Curtains, 1}, {H::Bangs, 1}, {H::SlickBack, 0.8f}, {H::Bowl, 0.6f}, {H::Quiff, 0.6f}}); break;
        case St::Chill: add({{H::HalfUp, 0.5f}, {H::Layers, 0.5f}, {H::Bun, 0.5f}, {H::Curly, 0.3f}}); break;
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
    return {wa, 1 - wa};
}
}  // namespace

void avoidEachOther(const std::vector<DancerRun*>& runs) {
    const float R = 2 * kBodyRadius;
    for (std::size_t i = 0; i < runs.size(); ++i)
        for (std::size_t j = i + 1; j < runs.size(); ++j) {
            DancerRun& A = *runs[i];
            DancerRun& B = *runs[j];
            if (!A.placed() || !B.placed()) continue;
            const float dx = B.x() - A.x(), dz = B.z() - A.z();  // A -> B
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
                if (d >= kMinApart) continue;
                if (d < 1e-4f) {  // on top of each other: apart sideways
                    dx = 1;
                    dz = 0;
                } else {
                    dx /= d;
                    dz /= d;
                }
                const float deficit = kMinApart - d;
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
    const float ax = w * w * (tx_ - x_) - 2 * w * vx_ + avoidX_, az = w * w * (tz_ - z_) - 2 * w * vz_ + avoidZ_;
    avoidX_ = avoidZ_ = 0;
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
    const bool free = !leaving() && !greeting;  // the stage may direct this dancer
    usedLeader_ = free && dir.leader && !dir.react;
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
        next = dir.leader->pose(mh, sk_, dir.delay, dir.mirror);
        key = "lead" + std::to_string(dir.leader->options().seed) + (dir.mirror ? "m" : "");  // the dancer seed: unique, unlike a reused address
        desc_ = dir.leader->describe(m.beat - dir.delay) + (dir.mirror ? " (mirrored)" : "");
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
            if (source == 2) return dir.leader->pose(mh, sk_, dir.delay + delay, dir.mirror);
            return choreo_.pose(mh, sk_, delay);
        };
        float part[kPoseFloats];
        auto take = [&](double lag, int from, int to) {
            if (lag <= 1e-6) return;
            poseToFloats(at(lag), part);
            for (int i = from; i <= to; ++i) raw[i] = part[i];
        };
        const double a = humanAmt_;
        take(a * pers_.headLag, 12, 14);
        take(a * std::max(0.0f, pers_.armLag + pers_.armLagAsym), 15, 17);
        take(a * std::max(0.0f, pers_.armLag - pers_.armLagAsym), 18, 20);
    }
    humanize(raw, mh.beat, t);
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
        outVel_[i] = haveOut_ ? (out[i] - lastOut_[i]) / fdt : 0.0f;
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
            out[i] = follow_[i];
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
    facing_ += (target - facing_) * float(std::min(1.0, dt * 5.0));
    pose_.bodyYaw += facing_;
    if (free) pose_.headYaw += dir.look;
    stepFeet(m.beat, dt, moving);
    faceAccents(pose_, m.beat, t, seed_, energy);
    // Amplitude: blend towards rest when quiet (energy < 1 shrinks the moves).
    if (energy < 0.999f) pose_ = mix(restPose(sk_), pose_, std::clamp(energy, 0.0f, 1.0f));
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
    const float want = (moving > 0.12f && scale_ > 0.6f) ? 1.0f : 0.0f;
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

void DancerRun::build(BodyOut& out) const {
    if (scale_ <= 1e-3f) return;
    Frame place;
    place.o = {x_, lift_, z_};
    buildBody(spec_.look, sk_, pose_, place, scale_, out);
}

std::string DancerRun::describe(double) const { return desc_; }

}  // namespace reforce::engine
