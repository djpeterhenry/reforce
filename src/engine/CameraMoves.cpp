// reforce extension (PLAN 5R.7, docs/model-design.md): keyframed camera paths for model Objects. See CameraMoves.hpp.
#include "engine/CameraMoves.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>

#include "scene/SceneFile.hpp"

namespace reforce::engine {

namespace {

using V3 = std::array<double, 3>;
constexpr double kPi = 3.14159265358979323846;

V3 add(const V3& a, const V3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
V3 sub(const V3& a, const V3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
V3 mul(const V3& a, double k) { return {a[0] * k, a[1] * k, a[2] * k}; }
double dot(const V3& a, const V3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
V3 cross(const V3& a, const V3& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
V3 norm(const V3& a, const V3& fb) {
    const double l = std::sqrt(dot(a, a));
    return l > 1e-12 && std::isfinite(l) ? mul(a, 1.0 / l) : fb;
}
double smooth(double e0, double e1, double x) {
    const double t = std::clamp((x - e0) / (e1 - e0), 0.0, 1.0);
    return t * t * (3 - 2 * t);
}

// Uniform Catmull-Rom between b and c (a, d: the neighbors), u in 0..1.
double cr(double a, double b, double c, double d, double u) {
    return 0.5 * (2 * b + (-a + c) * u + (2 * a - 5 * b + 4 * c - d) * u * u + (-a + 3 * b - 3 * c + d) * u * u * u);
}

// Path keyframes are authored for Side = +1, Dist = 1, Hite = 0.
V3 place(V3 p, const CameraMoveParams& m) {
    const double s = m.side < 0 ? -1.0 : 1.0;
    const double d = m.dist > 0 && std::isfinite(m.dist) ? m.dist : 1.0;
    p[0] *= s * d;
    p[1] = p[1] * d + (std::isfinite(m.height) ? m.height : 0.0);
    p[2] *= d;
    return p;
}
V3 mirror(V3 p, const CameraMoveParams& m) {
    if (m.side < 0) p[0] = -p[0];
    return p;
}

std::uint64_t mix64(std::uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    return x ^ (x >> 33);
}
int shuffled(double seed, long long k, int n) {
    const auto s = std::uint64_t(std::llround(std::fabs(std::isfinite(seed) ? seed : 0.0)));
    return int(mix64(s * 0x9E3779B97F4A7C15ULL + std::uint64_t(k) * 0xD1B54A32D192ED03ULL) % std::uint64_t(n));
}

std::string trim(std::string s) {
    const auto b = s.find_first_not_of(" \t\r\n\"");
    const auto e = s.find_last_not_of(" \t\r\n\"");
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

}  // namespace

double easeMove(double p, double k) { return p + (smooth(0, 1, p) - p) * std::clamp(k, 0.0, 1.0); }

std::optional<CameraKey> parseCameraKey(std::string_view text, std::string* error) {
    std::string s(text);
    std::replace(s.begin(), s.end(), ',', ' ');
    std::vector<double> v;
    const char* p = s.c_str();
    while (*p) {
        while (*p && std::isspace(static_cast<unsigned char>(*p))) ++p;
        if (!*p) break;
        char* end = nullptr;
        const double d = std::strtod(p, &end);
        if (end == p || !std::isfinite(d)) {
            if (error) *error = "not a number at \"" + std::string(p).substr(0, 12) + "\"";
            return std::nullopt;
        }
        v.push_back(d);
        p = end;
    }
    if (v.size() < 7 || v.size() > 9) {
        if (error) *error = "expected t eyeX eyeY eyeZ aimX aimY aimZ [fov [roll]] (" + std::to_string(v.size()) + " numbers)";
        return std::nullopt;
    }
    CameraKey k;
    k.t = v[0];
    k.eye = {v[1], v[2], v[3]};
    k.aim = {v[4], v[5], v[6]};
    if (v.size() > 7) k.fov = std::clamp(v[7], 5.0, 120.0);
    if (v.size() > 8) k.roll = v[8];
    return k;
}

CameraPath cameraPathFrom(const std::function<std::string(const char*)>& get, std::string* errors) {
    CameraPath path;
    for (int i = 1; i <= 64; ++i) {
        const std::string name = "K" + std::to_string(i);
        const std::string text = trim(get(name.c_str()));
        if (text.empty()) continue;
        std::string err;
        if (auto k = parseCameraKey(text, &err)) path.keys.push_back(*k);
        else if (errors) *errors += name + ": " + err + "; ";
    }
    std::stable_sort(path.keys.begin(), path.keys.end(), [](const CameraKey& a, const CameraKey& b) { return a.t < b.t; });
    auto num = [&](const char* key, double fb) {
        const std::string t = trim(get(key));
        if (t.empty()) return fb;
        char* end = nullptr;
        const double d = std::strtod(t.c_str(), &end);
        return end != t.c_str() && std::isfinite(d) ? d : fb;
    };
    path.ease = std::clamp(num("EASE", 0.0), 0.0, 1.0);
    path.step = num("STEP", 0.0) != 0.0;
    path.random = num("RAND", 0.0) != 0.0;
    path.push = std::clamp(num("PUSH", 0.0), -1.0, 0.9);
    return path;
}

std::optional<CameraPath> loadCameraPreset(const std::filesystem::path& file, std::string* errors) {
    const auto f = scene::loadSceneFile(file);
    if (!f) {
        if (errors) *errors += file.string() + ": cannot be read; ";
        return std::nullopt;
    }
    CameraPath path = cameraPathFrom(
        [&](const char* key) {
            const scene::SceneEntry* e = f->find(key);
            return e ? e->value : std::string();
        },
        errors);
    if (path.empty()) {
        if (errors) *errors += file.string() + ": no keyframes; ";
        return std::nullopt;
    }
    return path;
}

std::filesystem::path findCameraPreset(const std::string& name, const std::filesystem::path& modelDir,
                                       const std::filesystem::path& contentDir) {
    std::error_code ec;
    for (const auto& dir : {modelDir, contentDir}) {
        if (dir.empty()) continue;
        const auto p = dir / "Camera" / (name + ".txt");
        if (std::filesystem::is_regular_file(p, ec)) return p;
    }
    return {};
}

ModelShot pathShot(const CameraPath& path, double p, const CameraMoveParams& m) {
    ModelShot s;
    CameraKey k;
    const auto& keys = path.keys;
    if (keys.empty()) {
        k.eye = {0, 0.3, 3.0};
    } else if (path.step) {
        // Beat cuts: each keyframe held for CutB beats, in order or shuffled, pushing in while it is held.
        const int n = int(keys.size());
        const double per = m.cutBeats > 0 && std::isfinite(m.cutBeats) ? m.cutBeats : 4.0;
        const double b = std::isfinite(m.beat) ? m.beat / per : 0.0;
        const long long c = (long long)std::floor(b);
        int i;
        if (path.random && n > 1) {
            i = shuffled(m.seed, c, n);
            if (i == shuffled(m.seed, c - 1, n)) i = (i + 1) % n;
        } else {
            i = int(((c % n) + n) % n);
        }
        k = keys[std::size_t(i)];
        k.eye = mul(k.eye, 1.0 - path.push * (b - std::floor(b)));
    } else {
        // A Catmull-Rom spline through the keyframes on the eased timeline (the ends repeated).
        p = std::isfinite(p) ? std::clamp(p, 0.0, 1.0) : 0.0;
        const double t = easeMove(p, path.ease);
        const std::size_t n = keys.size();
        std::size_t i = 0;
        while (i + 1 < n && keys[i + 1].t <= t) ++i;
        if (i + 1 >= n || t <= keys.front().t) {
            k = t <= keys.front().t ? keys.front() : keys.back();
        } else {
            const CameraKey& b = keys[i];
            const CameraKey& c = keys[i + 1];
            // Past the ends the path continues straight (a = 2b - c, d = 2c - b), so two keyframes glide evenly
            // and the timeline's easing (Ease) alone decides how a path starts and stops.
            auto extend = [](const CameraKey& from, const CameraKey& to) {
                CameraKey e;
                for (std::size_t j = 0; j < 3; ++j) {
                    e.eye[j] = 2 * from.eye[j] - to.eye[j];
                    e.aim[j] = 2 * from.aim[j] - to.aim[j];
                }
                e.fov = 2 * from.fov - to.fov;
                e.roll = 2 * from.roll - to.roll;
                return e;
            };
            const CameraKey a = i > 0 ? keys[i - 1] : extend(b, c);
            const CameraKey d = i + 2 < n ? keys[i + 2] : extend(c, b);
            const double span = c.t - b.t;
            const double u = span > 1e-12 ? (t - b.t) / span : 0.0;
            for (std::size_t j = 0; j < 3; ++j) {
                k.eye[j] = cr(a.eye[j], b.eye[j], c.eye[j], d.eye[j], u);
                k.aim[j] = cr(a.aim[j], b.aim[j], c.aim[j], d.aim[j], u);
            }
            k.fov = cr(a.fov, b.fov, c.fov, d.fov, u);
            k.roll = cr(a.roll, b.roll, c.roll, d.roll, u);
        }
    }
    s.eye = place(k.eye, m);
    s.aim = mirror(k.aim, m);
    s.roll = (m.side < 0 ? -k.roll : k.roll) + (std::isfinite(m.roll) ? m.roll : 0.0);
    s.fov = m.fov > 0 && std::isfinite(m.fov) ? std::clamp(m.fov, 5.0, 120.0) : std::clamp(k.fov, 5.0, 120.0);
    return s;
}

Mat4 shotView(const ModelShot& s) {
    const V3 f = norm(sub(s.aim, s.eye), {0, 0, -1});
    V3 worldUp = std::fabs(f[1]) > 0.999 ? V3{0, 0, -1} : V3{0, 1, 0};
    V3 r = norm(cross(f, worldUp), {1, 0, 0});
    V3 u = cross(r, f);
    if (s.roll != 0 && std::isfinite(s.roll)) {
        const double c = std::cos(s.roll), sn = std::sin(s.roll);
        const V3 r2 = add(mul(r, c), mul(u, sn));
        u = add(mul(u, c), mul(r, -sn));
        r = r2;
    }
    Mat4 v = Mat4::identity();
    // Rows r, u, -f; translation -R * eye.
    const V3 rows[3] = {r, u, mul(f, -1)};
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) v.m[col * 4 + row] = float(rows[row][std::size_t(col)]);
        v.m[12 + row] = float(-dot(rows[row], s.eye));
    }
    return v;
}

Mat4 shotProjection(const ModelShot& s, double aspect, double nearZ, double farZ) {
    const double fov = std::clamp(std::isfinite(s.fov) ? s.fov : 40.0, 5.0, 120.0) * kPi / 180.0;
    const double y = 1.0 / std::tan(fov / 2), x = y / (aspect > 0 ? aspect : 1.0);
    Mat4 p{};
    p.m[0] = float(x);
    p.m[5] = float(y);
    p.m[10] = float(farZ / (nearZ - farZ));
    p.m[11] = -1.0f;
    p.m[14] = float(nearZ * farZ / (nearZ - farZ));
    return p;
}

}  // namespace reforce::engine
