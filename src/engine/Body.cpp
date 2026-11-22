#include "engine/Body.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace reforce::engine {

namespace {

std::string lower(const std::string& s) {
    std::string o;
    for (char c : s) o.push_back(char(std::tolower(static_cast<unsigned char>(c))));
    return o;
}

std::optional<bool> parseFlag(const std::string& v) {
    const std::string s = lower(v);
    if (s == "1" || s == "on" || s == "true" || s == "yes") return true;
    if (s == "0" || s == "off" || s == "false" || s == "no") return false;
    return std::nullopt;
}

std::optional<double> parseNumber(const std::string& v, double lo, double hi) {
    char* end = nullptr;
    const double x = std::strtod(v.c_str(), &end);
    if (v.empty() || end == v.c_str() || *end || !std::isfinite(x) || x < lo || x > hi) return std::nullopt;
    return x;
}

constexpr double kPi = 3.14159265358979323846;

}  // namespace

const char* poseJointName(int j) {
    static const char* names[kPoseJoints] = {
        "nose",       "left eye",  "right eye",  "left ear",    "right ear", "neck",       "left shoulder",
        "right shoulder", "left elbow", "right elbow", "left wrist", "right wrist", "root", "left hip",
        "right hip",  "left knee", "right knee", "left ankle",  "right ankle"};
    return j >= 0 && j < kPoseJoints ? names[j] : "?";
}

const char* bodyBurnName(BodyBurn b) {
    switch (b) {
        case BodyBurn::Fill: return "fill";
        case BodyBurn::Outline: return "outline";
        case BodyBurn::Motion: return "motion";
        default: return "off";
    }
}

std::optional<BodyBurn> parseBodyBurn(const std::string& s) {
    const std::string v = lower(s);
    if (v == "off" || v == "0" || v == "none") return BodyBurn::Off;
    if (v == "fill") return BodyBurn::Fill;
    if (v == "outline" || v == "edge") return BodyBurn::Outline;
    if (v == "motion" || v == "move") return BodyBurn::Motion;
    return std::nullopt;
}

std::string applyBodySetting(BodySettings& s, const std::string& key, const std::string& value) {
    const std::string k = lower(key);
    if (k == "track" || k == "show" || k == "sync") {
        auto f = parseFlag(value);
        if (!f) return k + ": on | off";
        (k == "track" ? s.track : k == "show" ? s.show : s.sync) = *f;
        return {};
    }
    if (k == "quality") {
        const std::string v = lower(value);
        if (v != "fast" && v != "balanced") return "quality: fast | balanced";
        s.fast = v == "fast";
        return {};
    }
    if (k == "burn") {
        auto b = parseBodyBurn(value);
        if (!b) return "burn: off | fill | outline | motion";
        s.burn = *b;
        return {};
    }
    if (k == "front" || k == "burnamount" || k == "trails") {
        auto x = parseNumber(value, 0, 1);
        if (!x) return k + ": 0..1";
        (k == "front" ? s.front : k == "burnamount" ? s.burnAmount : s.trails) = *x;
        return {};
    }
    if (k == "trailwidth") {
        auto x = parseNumber(value, 1, 80);
        if (!x) return "trailwidth: 1..80 px";
        s.trailWidth = *x;
        return {};
    }
    return "unknown body setting \"" + key + "\"";
}

std::string bodySettingKey(const std::string& key) {
    const std::string k = lower(key);
    if (k == "track") return "reforce.Body.Track";
    if (k == "quality") return "reforce.Body.Quality";
    if (k == "front") return "reforce.Body.Front";
    if (k == "burn") return "reforce.Body.Burn";
    if (k == "burnamount") return "reforce.Body.BurnAmount";
    if (k == "trails") return "reforce.Body.Trails";
    if (k == "trailwidth") return "reforce.Body.TrailWidth";
    if (k == "show") return "reforce.Body.Show";
    if (k == "sync") return "reforce.Body.Sync";
    return {};
}

std::optional<std::string> bodySettingValue(const std::string& key, const std::string& value) {
    BodySettings s;
    if (!applyBodySetting(s, key, value).empty()) return std::nullopt;
    const std::string k = lower(key);
    char b[32];
    auto num = [&](double x) {
        std::snprintf(b, sizeof b, "%g", x);
        return std::string(b);
    };
    if (k == "track") return std::string(s.track ? "1" : "0");
    if (k == "show") return std::string(s.show ? "1" : "0");
    if (k == "sync") return std::string(s.sync ? "1" : "0");
    if (k == "quality") return std::string(s.fast ? "fast" : "balanced");
    if (k == "burn") return std::string(bodyBurnName(s.burn));
    if (k == "front") return num(s.front);
    if (k == "burnamount") return num(s.burnAmount);
    if (k == "trails") return num(s.trails);
    if (k == "trailwidth") return num(s.trailWidth);
    return std::nullopt;
}

double OneEuro::filter(double x, double dt) {
    if (!primed_) {
        primed_ = true;
        x_ = x;
        dx_ = 0;
        return x_;
    }
    if (!(dt > 0)) return x_;
    auto alpha = [dt](double cutoff) {
        const double tau = 1.0 / (2 * kPi * cutoff);
        return 1.0 / (1.0 + tau / dt);
    };
    const double dx = (x - x_) / dt;
    dx_ += alpha(dCutoff_) * (dx - dx_);
    x_ += alpha(minCutoff_ + beta_ * std::abs(dx_)) * (x - x_);
    return x_;
}

void PoseSmoother::step(const PoseSample& latest, double dt) {
    if (latest.seq != 0 && latest.seq != lastSeq_) {
        lastSeq_ = latest.seq;
        sinceNew_ = 0;
    } else {
        sinceNew_ += dt;
    }
    const bool fresh = latest.seq != 0 && sinceNew_ < kStale;
    for (int j = 0; j < kPoseJoints; ++j) {
        Joint& J = joints_[std::size_t(j)];
        const bool seen = fresh && latest.conf[std::size_t(j)] >= kMinConf;
        if (seen) {
            if (J.weight <= 0) {  // (re)appearing: start where it is, do not slide in from where it was lost
                fx_[std::size_t(j)].reset();
                fy_[std::size_t(j)].reset();
            }
            J.x = fx_[std::size_t(j)].filter(latest.x[std::size_t(j)], dt);
            J.y = fy_[std::size_t(j)].filter(latest.y[std::size_t(j)], dt);
            J.weight = std::min(1.0, J.weight + dt / kFade);
        } else {
            J.weight = std::max(0.0, J.weight - dt / kFade);
        }
    }
}

void PoseSmoother::reset() {
    joints_ = {};
    for (auto& f : fx_) f.reset();
    for (auto& f : fy_) f.reset();
    lastSeq_ = 0;
    sinceNew_ = 1e9;
}

double PoseSmoother::presence() const {
    double w = 0;
    for (const Joint& j : joints_) w = std::max(w, j.weight);
    return w;
}

std::array<double, 2> cameraToScreen(double x, double y, const UvTransform& uv, bool mirror) {
    double u = (x - double(uv.offset[0])) / std::max(1e-9, double(uv.scale[0]));
    const double v = (y - double(uv.offset[1])) / std::max(1e-9, double(uv.scale[1]));
    if (mirror) u = 1.0 - u;
    return {u, v};
}

void HandTrails::step(const std::array<std::array<double, 3>, 2>& px, double dt, double brightness, double widthPx,
                      double frameW, double frameH, std::vector<LineSegment>& out) {
    (void)dt;
    (void)frameW;
    const float radius = float(std::max(0.5, widthPx * frameH / 1080.0 * 0.5));
    for (std::size_t i = 0; i < 2; ++i) {
        const double w = px[i][2];
        if (w <= 0.01 || brightness <= 0) {
            have_[i] = false;
            continue;
        }
        const std::array<double, 2> p{px[i][0], px[i][1]};
        const std::array<double, 2> a = have_[i] ? last_[i] : p;
        const float pen = float(std::clamp(brightness * w, 0.0, 1.0));
        LineSegment s{float(a[0]), float(a[1]), float(p[0]), float(p[1]), pen, pen, radius, float(out.size())};
        out.push_back(s);
        last_[i] = p;
        have_[i] = true;
    }
}

}  // namespace reforce::engine
