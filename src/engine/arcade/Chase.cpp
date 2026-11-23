// Arcade organic `chase` (PLAN 5Q.8, docs/arcade-design.md "Chase"): the light-cycle game (CyclesSim) in 3D, a
// camera chasing one cycle across the grid. See Chase.hpp.
#include "engine/arcade/Chase.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "engine/arcade/Arcade.hpp"

namespace reforce::engine::arcade {

namespace {

constexpr double kPi = 3.141592653589793;
constexpr double kSub = 1.0 / 60.0;  // spring substep, beats
constexpr int kMaxBits = 1600;       // sparks drawn per frame (the rest skipped by stride)

std::uint64_t mix(std::uint64_t z) {
    z += 0x9e3779b97f4a7c15ULL;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}
// Uniform [0, 1) from a key and an index.
double hashU(std::uint64_t key, std::uint64_t k) { return double(mix(key ^ mix(k + 0x51ed27)) >> 11) * 0x1.0p-53; }
std::uint64_t bitsOf(double v) {
    std::uint64_t b;
    std::memcpy(&b, &v, sizeof b);
    return b;
}
double wrapPi(double a) {
    if (!std::isfinite(a)) return 0;
    a = std::fmod(a + kPi, 2 * kPi);
    if (a < 0) a += 2 * kPi;
    return a - kPi;
}
double smooth(double u) {
    u = std::clamp(u, 0.0, 1.0);
    return u * u * (3 - 2 * u);
}
V3 unit(V3 a, V3 fb) {
    const double l = length(a);
    return l > 1e-12 && std::isfinite(l) ? a * (1.0 / l) : fb;
}
// A critically damped spring towards `to`, `w` per beat, for `h` beats.
void spring(double& x, double& v, double to, double w, double h) {
    v += (w * w * (to - x) - 2 * w * v) * h;
    x += v * h;
}
void spring(V3& x, V3& v, V3 to, double w, double h) {
    spring(x.x, v.x, to.x, w, h);
    spring(x.y, v.y, to.y, w, h);
    spring(x.z, v.z, to.z, w, h);
}

}  // namespace

// ------------------------------------------------------------------ projection

ChaseCamera ChaseCamera::lookAt(V3 eye, V3 target, double fovDeg, double roll) {
    ChaseCamera c;
    c.eye = eye;
    c.fwd = unit(target - eye, {0, 0, 1});
    c.right = unit(cross({0, 1, 0}, c.fwd), {1, 0, 0});
    c.up = cross(c.fwd, c.right);
    const double fov = std::clamp(finiteOr(fovDeg, 62), 10.0, 150.0);
    c.focal = 1.0 / std::tan(0.5 * fov * kPi / 180.0);
    c.roll = finiteOr(roll, 0);
    return c;
}

void ChaseCamera::toScreen(V3 v, double& sx, double& sy) const {
    const double z = std::max(v.z, nearZ);
    const double x = focal * v.x / z, y = focal * v.y / z;
    const double cr = std::cos(roll), sr = std::sin(roll);
    sx = x * cr - y * sr;
    sy = x * sr + y * cr;
}

bool ChaseCamera::project(V3 p, double& sx, double& sy) const {
    const V3 v = toView(p);
    if (!(v.z >= nearZ)) return false;
    toScreen(v, sx, sy);
    return std::isfinite(sx) && std::isfinite(sy);
}

bool ChaseCamera::clipNear(V3& a, V3& b, double& ta, double& tb) const {
    ta = 0;
    tb = 1;
    if (!(a.z >= nearZ) && !(b.z >= nearZ)) return false;
    if (!std::isfinite(a.z) || !std::isfinite(b.z)) return false;
    const V3 a0 = a, b0 = b;
    if (a0.z < nearZ) {
        ta = (nearZ - a0.z) / (b0.z - a0.z);
        a = lerp(a0, b0, ta);
        a.z = nearZ;
    } else if (b0.z < nearZ) {
        tb = (nearZ - a0.z) / (b0.z - a0.z);
        b = lerp(a0, b0, tb);
        b.z = nearZ;
    }
    return true;
}

int ChaseCamera::clipNearPoly(const V3* in, int n, V3* out) const {
    int m = 0;
    if (n < 3 || n > 16) return 0;
    for (int i = 0; i < n; ++i) {
        const V3 &p = in[i], &q = in[(i + 1) % n];
        const bool pin = p.z >= nearZ, qin = q.z >= nearZ;
        if (pin) out[m++] = p;
        if (pin != qin) {
            V3 r = lerp(p, q, (nearZ - p.z) / (q.z - p.z));
            r.z = nearZ;
            out[m++] = r;
        }
    }
    return m >= 3 ? m : 0;
}

bool clipRect2(double& x0, double& y0, double& x1, double& y1, double hx, double hy, double& t0, double& t1) {
    t0 = 0;
    t1 = 1;
    const double dx = x1 - x0, dy = y1 - y0;
    const double p[4] = {-dx, dx, -dy, dy};
    const double q[4] = {x0 + hx, hx - x0, y0 + hy, hy - y0};
    for (int i = 0; i < 4; ++i) {
        if (p[i] == 0) {
            if (q[i] < 0) return false;
            continue;
        }
        const double r = q[i] / p[i];
        if (p[i] < 0) {
            if (r > t1) return false;
            t0 = std::max(t0, r);
        } else {
            if (r < t0) return false;
            t1 = std::min(t1, r);
        }
    }
    const double ax = x0, ay = y0;
    x0 = ax + dx * t0;
    y0 = ay + dy * t0;
    x1 = ax + dx * t1;
    y1 = ay + dy * t1;
    return true;
}

int clipRectPoly(const double* xs, const double* ys, int n, double hx, double hy, double* ox, double* oy) {
    if (n < 3 || n > 24) return 0;
    double ax[32], ay[32], bx[32], by[32];
    int m = n;
    std::copy(xs, xs + n, ax);
    std::copy(ys, ys + n, ay);
    for (int e = 0; e < 4 && m >= 3; ++e) {
        // Edge e: x >= -hx, x <= hx, y >= -hy, y <= hy as s * coord <= h.
        const bool onX = e < 2;
        const double s = (e % 2 == 0) ? -1.0 : 1.0, h = onX ? hx : hy;
        int k = 0;
        for (int i = 0; i < m && k < 30; ++i) {
            const int j = (i + 1) % m;
            const double ci = s * (onX ? ax[i] : ay[i]), cj = s * (onX ? ax[j] : ay[j]);
            const bool iin = ci <= h, jin = cj <= h;
            if (iin) {
                bx[k] = ax[i];
                by[k++] = ay[i];
            }
            if (iin != jin) {
                const double t = (h - ci) / (cj - ci);
                bx[k] = ax[i] + (ax[j] - ax[i]) * t;
                by[k++] = ay[i] + (ay[j] - ay[i]) * t;
            }
        }
        m = k;
        std::copy(bx, bx + m, ax);
        std::copy(by, by + m, ay);
    }
    if (m < 3) return 0;
    m = std::min(m, 28);
    std::copy(ax, ax + m, ox);
    std::copy(ay, ay + m, oy);
    return m;
}

// ------------------------------------------------------------------ the director

ChaseView::ChaseView(const ChaseConfig& cfg, std::uint64_t seed) : cfg_(cfg), seed_(seed), sim_(cfg.game, seed) {
    cfg_.cam = std::clamp(finiteOr(cfg_.cam, 4.2), 0.5, 60.0);
    cfg_.camH = std::clamp(finiteOr(cfg_.camH, 1.5), 0.05, 60.0);
    cfg_.fov = std::clamp(finiteOr(cfg_.fov, 62), 15.0, 130.0);
    cfg_.walH = std::clamp(finiteOr(cfg_.walH, 0.8), 0.0, 10.0);
    cfg_.fog = std::clamp(finiteOr(cfg_.fog, 42), 2.0, 1000.0);
    cfg_.grid = std::clamp(finiteOr(cfg_.grid, 0.45), 0.0, 1.0);
    cfg_.cut = std::clamp(finiteOr(cfg_.cut, 2), 0.25, 1024.0);
    cfg_.wdth = std::clamp(finiteOr(cfg_.wdth, 2.4), 0.0, 50.0);
    cfg_.shot = std::clamp(cfg_.shot, 0, int(kShots));
    cfg_.bike = std::clamp(cfg_.bike, 0, 2);
    fov_ = cfg_.fov;
}

void ChaseView::headAt(int i, double beat, V3& p, double& angle) const {
    double x, y;
    const auto& cy = sim_.cycles()[std::size_t(i)];
    if (cy.trail.empty()) {
        p = {0.5 * (sim_.config().cols - 1), 0, 0.5 * (sim_.config().rows - 1)};
        angle = 0;
        return;
    }
    sim_.head(i, beat, x, y, angle);
    p = {x, 0, y};
}

void ChaseView::chooseTarget(std::uint64_t key, Reason why) {
    // Prefer cycles near others (where the action is) and near a fresh explosion; a little away from the one chased.
    const auto& cs = sim_.cycles();
    const int n = int(cs.size());
    const double now = last_;
    int best = -1;
    double bestScore = -1e300;
    for (int i = 0; i < n; ++i) {
        if (!cs[std::size_t(i)].alive || cs[std::size_t(i)].trail.empty()) continue;
        V3 p;
        double a;
        headAt(i, now, p, a);
        double score = 0.9 * hashU(key, std::uint64_t(i));
        for (int j = 0; j < n; ++j) {
            if (j == i || !cs[std::size_t(j)].alive || cs[std::size_t(j)].trail.empty()) continue;
            V3 q;
            headAt(j, now, q, a);
            score += std::exp(-length(q - p) / 9.0);
        }
        for (const auto& b : sim_.blasts())
            if (now - b.t < 4) score += 0.5 * std::exp(-length(V3{b.x, 0, b.y} - p) / 8.0);
        if (i == target_) score -= 0.35;
        if (score > bestScore) {
            bestScore = score;
            best = i;
        }
    }
    if (best >= 0 && best != target_) {
        target_ = best;
        reason_ = why;
    }
}

int ChaseView::pickShot(std::uint64_t key) const {
    if (cfg_.shot >= 1) return cfg_.shot - 1;
    static const double kW[kShots] = {0.36, 0.2, 0.16, 0.12, 0.16};
    double u = hashU(key, 101);
    int s = kChase;
    for (int k = 0; k < kShots; ++k) {
        if (u < kW[k]) {
            s = k;
            break;
        }
        u -= kW[k];
    }
    // The same special shot twice in a row only now and then.
    if (s == shot_ && s != kChase && hashU(key, 102) < 0.6) s = kChase;
    return s;
}

void ChaseView::desired(double beat, V3& eye, V3& look, double& fov) const {
    const double D = cfg_.cam, H = cfg_.camH;
    if (orbiting_) {
        const double a = std::max(0.0, beat - orbitT_);
        const double ang = orbitA0_ + 0.5 * a;
        const double r = D * (0.85 + 0.12 * a), h = H * (1.0 + 0.3 * a);
        eye = orbitAt_ + V3{std::cos(ang) * r, h, std::sin(ang) * r};
        look = orbitAt_ + V3{0, 0.4, 0};
        fov = cfg_.fov + 8;
        return;
    }
    V3 P;
    double ha;
    headAt(target_, beat, P, ha);
    const V3 F{std::cos(yaw_), 0, std::sin(yaw_)}, U{0, 1, 0};
    const V3 R = V3{std::sin(yaw_), 0, -std::cos(yaw_)} * side_;  // the side this shot keeps to
    fov = cfg_.fov;
    switch (shot_) {
        default:
        case kChase:
            // A little off the wall's line, so the wall reads as a ribbon rather than edge-on.
            eye = P - F * D + R * (0.16 * D) + U * H;
            look = P + F * 2.2 + U * 0.35;
            break;
        case kLow:
            eye = P - F * (0.62 * D) + R * 1.1 + U * (0.72 * H);
            look = P + F * 3.5 + U * 0.3;
            fov += 12;
            break;
        case kSide:
            eye = P + R * (0.75 * D) - F * (0.3 * D) + U * (0.45 * H);
            look = P + F * 1.2 + U * 0.3;
            fov -= 4;
            break;
        case kFront:
            eye = P + F * (0.85 * D) + R * 0.6 + U * (0.55 * H);
            look = P - F * 1.2 + U * 0.3;
            break;
        case kHigh:
            eye = P - F * (1.1 * D) + U * (3.2 * H);
            look = P + F * 2.5;
            break;
    }
}

void ChaseView::snapSprings(double beat) {
    if (!orbiting_) {
        V3 p;
        headAt(target_, beat, p, yaw_);
    }
    yawV_ = 0;
    double f;
    desired(beat, eye_, look_, f);
    fov_ = f;
    eyeV_ = lookV_ = {};
    fovV_ = 0;
}

void ChaseView::advance(double beat, double bar, double drop, double build, double onset) {
    if (!std::isfinite(beat) || std::abs(beat) > 1e12) return;
    if (!std::isfinite(bar)) bar = beat / 4.0;
    sim_.advance(beat, bar, drop, build);
    const bool jump = !started_ || beat < last_ - 1e-9 || beat - last_ > CyclesSim::kMaxCatchUpBeats;
    const long ci = long(std::floor(bar / cfg_.cut + 1e-6));
    const std::uint64_t key = mix(seed_ ^ mix(std::uint64_t(ci) * 0x2545f4914f6cdd1dULL));
    if (jump) {
        last_ = beat;
        orbiting_ = false;
        cutIdx_ = ci;
        chooseTarget(key, started_ ? kCutLine : kStart);
        shot_ = started_ || cfg_.shot >= 2 ? pickShot(key) : int(kChase);
        side_ = hashU(key, 103) < 0.5 ? -1.0 : 1.0;
        snap_ = true;
    } else if (ci != cutIdx_) {
        // A cut line: maybe another cycle, maybe another shot. A new shot (or a far target) is a cut, else a glide.
        cutIdx_ = ci;
        V3 before;
        double a;
        headAt(target_, beat, before, a);
        chooseTarget(key, kCutLine);
        const int s = pickShot(key);
        V3 after;
        headAt(target_, beat, after, a);
        if (s != shot_ || length(after - before) > 22) snap_ = true;
        if (snap_) side_ = hashU(key, 103) < 0.5 ? -1.0 : 1.0;
        shot_ = s;
    }

    // The chased cycle crashed: orbit its explosion, then cut to another on a beat line.
    if (!sim_.cycles()[std::size_t(target_)].alive) {
        if (!orbiting_) {
            orbiting_ = true;
            orbitAt_ = lastHead_;
            orbitT_ = beat;
            for (const auto& k : sim_.crashLog())
                if (k.cycle == target_) {
                    orbitAt_ = {k.x, 0, k.y};
                    orbitT_ = std::min(beat, k.t);
                }
            orbitA0_ = std::atan2(eye_.z - orbitAt_.z, eye_.x - orbitAt_.x);
            if (!std::isfinite(orbitA0_)) orbitA0_ = 0;
        }
        if (beat >= std::ceil(orbitT_ + 2.5 - 1e-9)) {
            chooseTarget(mix(seed_ ^ bitsOf(orbitT_)), kCrashed);
            if (sim_.cycles()[std::size_t(target_)].alive) {
                orbiting_ = false;
                snap_ = true;
            }
        }
    } else if (orbiting_) {
        orbiting_ = false;
        snap_ = true;
    }

    // A teleport (a round's formation, rezzing in): no swing across the arena.
    V3 head;
    double ha;
    headAt(target_, beat, head, ha);
    if (!orbiting_ && length(head - lastHead_) > 3.0 && !jump) snap_ = true;

    if (snap_) {
        snapSprings(beat);
        snap_ = false;
    } else {
        double b = last_;
        for (int guard = 0; b < beat - 1e-12 && guard < 8 * 64; ++guard) {
            const double h = std::min(kSub, beat - b);
            b += h;
            if (!orbiting_) {
                V3 p;
                double a;
                headAt(target_, b, p, a);
                double yt = yaw_ + wrapPi(a - yaw_);
                spring(yaw_, yawV_, yt, 5.0, h);
            }
            V3 e, l;
            double f;
            desired(b, e, l, f);
            spring(eye_, eyeV_, e, 9.0, h);
            spring(look_, lookV_, l, 14.0, h);
            spring(fov_, fovV_, f, 6.0, h);
        }
    }
    lastHead_ = orbiting_ ? orbitAt_ : head;
    last_ = beat;
    lastBar_ = bar;
    started_ = true;
    compose(beat, bar, onset);
}

void ChaseView::compose(double beat, double bar, double onset) {
    V3 E = eye_, L = look_;
    double fov = fov_;
    const auto& g = sim_.config();
    const double D = cfg_.cam;

    // The drop: a low, fast, close angle for most of a bar.
    dropW_ = 0;
    const double ds = beat - sim_.dropBeat();
    if (ds >= 0 && ds < 4.5 && !orbiting_) {
        dropW_ = ds < 0.3 ? smooth(ds / 0.3) : ds < 3.5 ? 1.0 : 1.0 - smooth(ds - 3.5);
        V3 P;
        double ha;
        headAt(target_, beat, P, ha);
        const V3 F{std::cos(yaw_), 0, std::sin(yaw_)}, R{std::sin(yaw_), 0, -std::cos(yaw_)};
        const V3 de = P - F * (0.42 * D) + R * 0.45 + V3{0, 0.3, 0};
        const V3 dl = P + F * 5.0 + V3{0, 0.2, 0};
        E = lerp(E, de, dropW_);
        L = lerp(L, dl, dropW_);
        fov += 22 * dropW_;
    }

    // Phrase lines (the round's reset): crane up to a high wide shot over the arena as the walls burst, then swoop
    // back down behind a cycle as the new round gets going.
    crane_ = 0;
    if (g.reset >= 4) {
        const double r4 = g.reset * 4.0, pb = bar / g.reset;
        const double phase = pb - std::floor(pb + 1e-9);
        const double since = std::max(0.0, phase) * r4, to = r4 - since;
        if (to < 2.0) crane_ = smooth(1.0 - to / 2.0);
        else if (since < 1.5) crane_ = 1.0;
        else if (since < 4.5) crane_ = 1.0 - smooth((since - 1.5) / 3.0);
        if (crane_ > 0) {
            const long phrase = long(std::floor(pb + 1e-9 + (to < 2.0 ? 1 : 0)));
            const V3 C{0.5 * (g.cols - 1), 0, 0.5 * (g.rows - 1)};
            const double span = std::max(double(g.cols), 1.78 * g.rows);
            const double side = (phrase & 1) ? 1.0 : -1.0;
            const double az = -kPi / 2 + side * (0.35 + 0.06 * (since < 6 ? since : 0));
            const V3 ce = C + V3{std::cos(az) * 0.62 * span, 0.55 * span, std::sin(az) * 0.62 * span};
            const double w = crane_;
            E = lerp(E, ce, w);
            L = lerp(L, C, w);
            fov = fov + (cfg_.fov - fov) * w;
        }
    }

    // The beat: a FOV kick and the camera dipping, stronger on bar lines and with the onset.
    const double frac = beat - std::floor(beat);
    const long b4 = std::lround((bar - frac / 4.0) * 4.0);
    const bool barLine = ((b4 % 4) + 4) % 4 == 0;
    const double kick = std::exp(-7.0 * frac) * ((barLine ? 1.0 : 0.55) + 0.3 * std::min(1.0, onset)) * (1 - crane_);
    fov *= 1.0 + 0.07 * kick;
    // (Not down past the top of the walls when it was above them.)
    E.y = std::max({0.12, E.y - 0.2 * cfg_.camH * kick, std::min(E.y, 1.15 * cfg_.walH)});

    // Roll: leaning into the turn the camera is still swinging through.
    roll_ = 0;
    if (!orbiting_) {
        V3 P;
        double ha;
        headAt(target_, beat, P, ha);
        roll_ = std::clamp(-0.32 * wrapPi(ha - yaw_), -0.22, 0.22) * (1 - crane_);
    }
    fogDist_ = cfg_.fog + crane_ * std::max(0.0, length(E - L) - cfg_.cam);
    cam_ = ChaseCamera::lookAt(E, L, fov, roll_);
}

// ------------------------------------------------------------------ drawing

double ChaseView::fog(double d) const {
    // Further out when the camera stands back (the crane): the fog is about what is beyond the action.
    const double u = d / fogDist_;
    return std::exp(-2.3 * u * u);
}

double ChaseView::widthAt(double z) const { return cfg_.wdth * std::clamp(1.1 * cfg_.cam / std::max(z, 1e-3), 0.3, 2.4); }

void ChaseView::seg3(OrganicCanvas& c, V3 a, V3 b, double w, double pa, double pb) {
    V3 va = cam_.toView(a), vb = cam_.toView(b);
    double ta, tb;
    if (!cam_.clipNear(va, vb, ta, tb)) return;
    double p0 = (pa + (pb - pa) * ta) * fog(length(va)), p1 = (pa + (pb - pa) * tb) * fog(length(vb));
    if (p0 < 0.004 && p1 < 0.004) return;
    double x0, y0, x1, y1;
    cam_.toScreen(va, x0, y0);
    cam_.toScreen(vb, x1, y1);
    double s0, s1;
    if (!clipRect2(x0, y0, x1, y1, hx_, hy_, s0, s1)) return;
    const double q0 = p0 + (p1 - p0) * s0, q1 = p0 + (p1 - p0) * s1;
    const double z = va.z + (vb.z - va.z) * 0.5 * (s0 + s1);
    c.line(x0, y0, x1, y1, widthAt(z) * w, q0, q1);
    ++emitted_;
}

void ChaseView::line3(OrganicCanvas& c, V3 a, V3 b, double w, double pa, double pb) {
    // Long lines in pieces, so the fog follows the distance.
    const int n = std::clamp(int(std::ceil(length(b - a) / 5.0)), 1, 24);
    for (int k = 0; k < n; ++k) {
        const double t0 = double(k) / n, t1 = double(k + 1) / n;
        seg3(c, lerp(a, b, t0), lerp(a, b, t1), w, pa + (pb - pa) * t0, pa + (pb - pa) * t1);
    }
}

void ChaseView::dot3(OrganicCanvas& c, V3 p, double diam, double pen, double maxDiam) {
    const V3 v = cam_.toView(p);
    if (!(v.z >= cam_.nearZ * 4)) return;
    pen *= fog(length(v));
    if (pen < 0.004) return;
    double x, y;
    cam_.toScreen(v, x, y);
    if (std::abs(x) > hx_ || std::abs(y) > hy_) return;
    c.dotUnits(x, y, std::min(maxDiam, diam * cam_.focal / v.z), pen);
    ++emitted_;
}

int ChaseView::screenPoly(const V3* pts, int n, double* xs, double* ys) const {
    V3 v[16], cl[17];
    for (int i = 0; i < n; ++i) v[i] = cam_.toView(pts[i]);
    const int m = cam_.clipNearPoly(v, n, cl);
    if (m < 3) return 0;
    double sx[24], sy[24];
    for (int i = 0; i < m; ++i) cam_.toScreen(cl[i], sx[i], sy[i]);
    return clipRectPoly(sx, sy, m, hx_, hy_, xs, ys);
}

namespace {
// Rows of strokes a fill of this screen polygon costs.
double fillCost(const double* ys, int m, double ppu) {
    if (m < 3) return 0;
    double lo = ys[0], hi = ys[0];
    for (int i = 1; i < m; ++i) {
        lo = std::min(lo, ys[i]);
        hi = std::max(hi, ys[i]);
    }
    return (hi - lo) * ppu / OrganicCanvas::kFillSpacingPx + 1;
}
// Convex hull (monotone chain) of n points, in place; returns the count.
int hull(double* xs, double* ys, int n) {
    if (n < 3) return n;
    int idx[32];
    for (int i = 0; i < n; ++i) idx[i] = i;
    std::sort(idx, idx + n, [&](int a, int b) { return xs[a] < xs[b] || (xs[a] == xs[b] && ys[a] < ys[b]); });
    int h[64], k = 0;
    auto crossz = [&](int o, int a, int b) {
        return (xs[a] - xs[o]) * (ys[b] - ys[o]) - (ys[a] - ys[o]) * (xs[b] - xs[o]);
    };
    for (int i = 0; i < n; ++i) {
        while (k >= 2 && crossz(h[k - 2], h[k - 1], idx[i]) <= 0) --k;
        h[k++] = idx[i];
    }
    for (int i = n - 2, t = k + 1; i >= 0; --i) {
        while (k >= t && crossz(h[k - 2], h[k - 1], idx[i]) <= 0) --k;
        h[k++] = idx[i];
    }
    --k;
    double ox[64], oy[64];
    for (int i = 0; i < k; ++i) {
        ox[i] = xs[h[i]];
        oy[i] = ys[h[i]];
    }
    std::copy(ox, ox + k, xs);
    std::copy(oy, oy + k, ys);
    return k;
}
}  // namespace

namespace {

// The machines (our own designs; Peter 2026-11-22: big glowing wheels, one smooth body line, a dark body with bright
// edges, the light wall coming out under the rear wheel). Local units: f forward, u up, s to the side, about a cell
// at scale 1 (drawn at kBikeScale). The body's side profile: the top line from the tail to the nose and the bottom
// line from the nose to the tail (control points, smoothed), extruded to +-hw.
struct BikePt {
    double f, u;
};
struct BikeDesign {
    const char* name;
    double r, front, rear, tire;  // wheel radius, wheel centres (f), tyre half width
    double hw;                    // body half width
    const BikePt* top;
    int nTop;
    const BikePt* bot;
    int nBot;
    bool hubless;     // rings only (no hub, no spokes): tread ticks instead
    double seam;      // the glowing seam along each flank: this far up from the bottom line (0 = none)
};
constexpr double kBikeScale = 1.45;
// 0 "Arc": a low enclosed fairing arching from the tail over the rider to the nose, the wheels' upper halves inside
// it (the body hides them), a glowing seam along each flank.
constexpr BikePt kArcTop[] = {{-0.76, 0.20}, {-0.64, 0.48}, {-0.30, 0.64}, {0.08, 0.62}, {0.46, 0.44}, {0.80, 0.16}};
constexpr BikePt kArcBot[] = {{0.76, 0.10}, {0.44, 0.28}, {0.0, 0.24}, {-0.42, 0.28}, {-0.74, 0.12}};
// 1 "Blade": a long low wedge, a sharp nose, a tall swept tail fin, smaller wheels standing out of the body.
constexpr BikePt kBladeTop[] = {{-0.78, 0.14}, {-0.74, 0.50}, {-0.56, 0.46}, {-0.10, 0.32}, {0.40, 0.18}, {0.88, 0.06}};
constexpr BikePt kBladeBot[] = {{0.84, 0.04}, {0.48, 0.10}, {0.0, 0.12}, {-0.48, 0.10}, {-0.76, 0.08}};
// 2 "Orb": two huge open hubless rings (no discs: see-through), a slim spine with a rider hump riding high between
// their tops; the rings over the body.
constexpr BikePt kOrbTop[] = {{-0.66, 0.62}, {-0.34, 0.86}, {0.0, 0.94}, {0.32, 0.84}, {0.64, 0.62}};
constexpr BikePt kOrbBot[] = {{0.62, 0.52}, {0.22, 0.48}, {-0.22, 0.48}, {-0.64, 0.52}};
constexpr BikeDesign kBikes[] = {
    {"arc", 0.29, 0.44, -0.42, 0.07, 0.17, kArcTop, 6, kArcBot, 5, false, 0.45},
    {"blade", 0.23, 0.50, -0.48, 0.06, 0.15, kBladeTop, 6, kBladeBot, 5, false, 0.5},
    {"orb", 0.37, 0.43, -0.43, 0.08, 0.10, kOrbTop, 5, kOrbBot, 4, true, 0.0},
};
constexpr int kBikeCount = int(sizeof kBikes / sizeof kBikes[0]);

// Catmull-Rom through the control points, `sub` pieces each.
int smoothLine(const BikePt* c, int n, int sub, BikePt* out) {
    int m = 0;
    for (int k = 0; k + 1 < n; ++k) {
        const BikePt &p0 = c[std::max(0, k - 1)], &p1 = c[k], &p2 = c[k + 1], &p3 = c[std::min(n - 1, k + 2)];
        for (int j = 0; j < sub; ++j) {
            const double t = double(j) / sub, t2 = t * t, t3 = t2 * t;
            auto cr = [&](double a, double b, double cc, double d) {
                return 0.5 * (2 * b + (-a + cc) * t + (2 * a - 5 * b + 4 * cc - d) * t2 + (-a + 3 * b - 3 * cc + d) * t3);
            };
            out[m++] = {cr(p0.f, p1.f, p2.f, p3.f), cr(p0.u, p1.u, p2.u, p3.u)};
        }
    }
    out[m++] = c[n - 1];
    return m;
}
// The height of a line (BikePt, in either direction along f) at `f`; the nearest end outside it.
double heightAt(const BikePt* c, int n, double f) {
    for (int k = 0; k + 1 < n; ++k) {
        const double a = c[k].f, b = c[k + 1].f;
        if ((f - a) * (f - b) <= 0 && a != b) return c[k].u + (c[k + 1].u - c[k].u) * (f - a) / (b - a);
    }
    return std::abs(f - c[0].f) < std::abs(f - c[n - 1].f) ? c[0].u : c[n - 1].u;
}

}  // namespace

double ChaseView::wheelTurn(double beat, int spd, double radius) {
    // Cells ridden (Spd per beat; a drop's boost is not counted, so the wheel never jumps) over the radius.
    return finiteOr(beat, 0) * double(std::max(1, spd)) / std::max(0.05, radius);
}

double ChaseView::bikeRear() const {
    const auto& b = kBikes[std::clamp(cfg_.bike, 0, kBikeCount - 1)];
    return -b.rear * kBikeScale;
}

void ChaseView::drawCycle(OrganicCanvas& c, int i, double beat, double pen, bool fast) {
    const auto& cy = sim_.cycles()[std::size_t(i)];
    const BikeDesign& B = kBikes[std::clamp(cfg_.bike, 0, kBikeCount - 1)];
    const long seg0 = emitted_ + fillUsed_;
    V3 P;
    double a;
    headAt(i, beat, P, a);
    V3 Pp;
    double ap;
    headAt(i, beat - 0.12, Pp, ap);
    const double bank = std::clamp(1.6 * wrapPi(a - ap), -0.6, 0.6);
    const V3 F{std::cos(a), 0, std::sin(a)}, R0{std::sin(a), 0, -std::cos(a)}, U0{0, 1, 0};
    // Leaning into the turn about the contact line.
    const V3 U = U0 * std::cos(bank) - R0 * std::sin(bank), R = R0 * std::cos(bank) + U0 * std::sin(bank);
    const double k = kBikeScale;
    auto at = [&](double f, double s, double u) { return P + (F * f + R * s + U * u) * k; };
    const double frac = beat - std::floor(beat);
    const double pulse = std::exp(-7.0 * frac);
    const double squash = 1.0 - 0.07 * pulse;  // the body settles a little on each beat
    const double lvl = std::min(1.0, cy.level + 0.08);
    const double fogK = fog(length(cam_.toView(P)));
    // Level of detail: a machine a few pixels across gets single rims with fewer pieces, no bloom, no tread marks,
    // hub rings, seams or bottom lines.
    const double rpx = B.r * k * cam_.focal / std::max(0.1, cam_.toView(P).z) * ppu_;
    const bool fine = rpx > 12;
    if (fogK < 0.01) return;
    const double p = pen * lvl;

    // Lines with a glow: the halos first (one path), the cores over them (the next).
    struct GL {
        V3 a, b;
        double w, pa, pb, halo;
    };
    GL gl[320];
    int ng = 0;
    auto glow = [&](V3 x, V3 y, double w, double pa, double pb, double halo) {
        if (ng < 320) gl[ng++] = {x, y, w, pa, pb, halo};
    };
    auto flush = [&]() {
        // A wide faint bloom under the brightest lines, a halo under every glowing one, then the cores.
        c.newPath();
        for (int q = 0; q < ng; ++q)
            if (fine && gl[q].halo >= 0.5)
                seg3(c, gl[q].a, gl[q].b, gl[q].w * 7.0, gl[q].pa * 0.22, gl[q].pb * 0.22);
        c.newPath();
        for (int q = 0; q < ng; ++q)
            if (gl[q].halo > 0)
                seg3(c, gl[q].a, gl[q].b, gl[q].w * 3.4, gl[q].pa * gl[q].halo, gl[q].pb * gl[q].halo);
        c.newPath();
        for (int q = 0; q < ng; ++q) seg3(c, gl[q].a, gl[q].b, gl[q].w, gl[q].pa, gl[q].pb);
        ng = 0;
    };

    // The body's lines, both flanks; the nose narrower.
    BikePt top[64], bot[64];
    const int nt = smoothLine(B.top, B.nTop, 4, top), nb = smoothLine(B.bot, B.nBot, 3, bot);
    const double fMax = B.top[B.nTop - 1].f;
    auto halfW = [&](double f) { return B.hw * (f > 0.35 ? 1.0 - 0.6 * (f - 0.35) / std::max(0.05, fMax - 0.35) : 1.0); };
    auto bodyAt = [&](double f, double side, double u) { return at(f, side * halfW(f), u * squash); };

    // The floor's light pool under the machine.
    c.newPath();
    {
        V3 pool[12];
        for (int q = 0; q < 12; ++q) {
            const double t = 2 * kPi * q / 12.0;
            pool[q] = P + F * (0.85 * k * std::cos(t) - 0.05 * k) + R0 * (0.36 * k * std::sin(t)) + V3{0, 0.004, 0};
        }
        double xs[32], ys[32];
        const int m = screenPoly(pool, 12, xs, ys);
        if (m >= 3 && fillUsed_ + fillCost(ys, m, ppu_) <= kFillBudget) {  // (over the budget: no pool)
            c.fillPolygon(xs, ys, m, p * 0.36 * fogK);
            fillUsed_ += long(fillCost(ys, m, ppu_));
        }
    }

    // The wheels: dark discs, thick glowing rims (both edges of the tyre), tread marks rolling round with the
    // distance ridden, a hub ring (open hubless rings: no disc, no hub, more tread marks).
    auto wheels = [&]() {
        for (double wf : {B.front, B.rear}) {
            if (B.hubless) break;  // open rings
            V3 disc[16];
            for (int q = 0; q < 16; ++q) {
                const double t = 2 * kPi * q / 16.0;
                disc[q] = at(wf + 0.9 * B.r * std::cos(t), 0, B.r + 0.9 * B.r * std::sin(t));
            }
            double xs[32], ys[32];
            const int m = screenPoly(disc, 16, xs, ys);
            if (m >= 3 && fillUsed_ + fillCost(ys, m, ppu_) <= kFillBudget) {  // (over the budget: open rims)
                c.fillPolygon(xs, ys, m, p * 0.12 * fogK);
                fillUsed_ += long(fillCost(ys, m, ppu_));
            }
        }
        // Rolling: the angle is the distance ridden over the radius (forward motion turns the wheel's top forward).
        const double turn = -wheelTurn(beat, sim_.config().spd, B.r * k);
        for (double wf : {B.front, B.rear}) {
            const double r = B.r, cu = r;
            for (double ts : {-B.tire, B.tire}) {
                if (!fine && ts > 0) break;
                V3 prev = at(wf + r, ts, cu);
                for (int q = 1, n = fine ? 20 : 12; q <= n; ++q) {
                    const double t = 2 * kPi * q / double(n);
                    const V3 nx = at(wf + r * std::cos(t), ts, cu + r * std::sin(t));
                    // The bloom only under the tyre edge nearer the camera (the far one: a halo).
                    const double halo = dot(R, cam_.eye - P) * ts > 0 ? 0.6 : 0.45;
                    glow(prev, nx, 1.15, std::min(1.0, p * 1.1), std::min(1.0, p * 1.1), halo);
                    prev = nx;
                }
            }
            // Tread marks: short arcs just inside the rim, carried round by the rolling.
            const int marks = !fine ? 0 : B.hubless ? 6 : 4;
            const double ir = B.hubless ? 0.8 * r : 0.74 * r;
            for (int q = 0; q < marks; ++q) {
                const double t0 = turn + 2 * kPi * q / marks;
                V3 prev = at(wf + ir * std::cos(t0), 0, cu + ir * std::sin(t0));
                for (int j = 1; j <= 3; ++j) {
                    const double t = t0 + 0.16 * j;
                    const V3 nx = at(wf + ir * std::cos(t), 0, cu + ir * std::sin(t));
                    glow(prev, nx, 0.55, p * 0.75, p * 0.75, 0.3);
                    prev = nx;
                }
            }
            if (!B.hubless && fine) {
                // The hub: a small ring and a hot centre.
                V3 prev = at(wf + 0.22 * r, 0, cu);
                for (int q = 1; q <= 8; ++q) {
                    const double t = 2 * kPi * q / 8.0;
                    const V3 nx = at(wf + 0.22 * r * std::cos(t), 0, cu + 0.22 * r * std::sin(t));
                    glow(prev, nx, 0.5, p * 0.8, p * 0.8, 0);
                    prev = nx;
                }
            }
        }
    };

    // The body: a dark shell (the hull of its outline from this angle), then its lines: the top line on both flanks
    // (the silhouette's highlight), a glowing seam along each side, the bottom line dimmer, the nose and the tail
    // joined across; the tail light brightest.
    auto body = [&]() {
        c.newPath();
        {
            double xs[40], ys[40];
            int n = 0;
            bool ok = true;
            for (int side : {-1, 1}) {
                for (int q = 0; q < B.nTop && ok; ++q, ++n) ok = cam_.project(bodyAt(B.top[q].f, side, B.top[q].u), xs[n], ys[n]);
                for (int q = 0; q < B.nBot && ok; ++q, ++n) ok = cam_.project(bodyAt(B.bot[q].f, side, B.bot[q].u), xs[n], ys[n]);
            }
            if (ok && n <= 32) {
                n = hull(xs, ys, n);
                double ox[32], oy[32];
                const int m = clipRectPoly(xs, ys, n, hx_, hy_, ox, oy);
                if (m >= 3) {
                    c.fillPolygon(ox, oy, m, p * 0.16 * fogK);
                    fillUsed_ += long(fillCost(oy, m, ppu_));
                }
            }
        }
        for (int side : {-1, 1}) {
            for (int q = 0; q + 1 < nt; ++q)
                glow(bodyAt(top[q].f, side, top[q].u), bodyAt(top[q + 1].f, side, top[q + 1].u), 1.0, p, p, 0.5);
            for (int q = 0; fine && q + 1 < nb; ++q)
                glow(bodyAt(bot[q].f, side, bot[q].u), bodyAt(bot[q + 1].f, side, bot[q + 1].u), 0.4, p * 0.45, p * 0.45,
                     0);
            if (B.seam > 0 && fine) {
                // From just ahead of the tail to just behind the nose, a fraction of the way up the flank.
                const double f0 = B.top[0].f + 0.12, f1 = fMax - 0.14;
                V3 prev;
                for (int q = 0; q <= 12; ++q) {
                    const double f = f0 + (f1 - f0) * q / 12.0;
                    const double hb = heightAt(B.bot, B.nBot, f), ht = heightAt(top, nt, f);
                    const V3 nx = bodyAt(f, side * 1.03, hb + (ht - hb) * B.seam);
                    if (q > 0) glow(prev, nx, 0.45, p * 0.85, p * 0.85, 0.35);
                    prev = nx;
                }
            }
            glow(bodyAt(top[0].f, side, top[0].u), bodyAt(bot[nb - 1].f, side, bot[nb - 1].u), 0.5, p * 0.7, p * 0.5, 0);
        }
        glow(bodyAt(top[nt - 1].f, -1, top[nt - 1].u), bodyAt(top[nt - 1].f, 1, top[nt - 1].u), 0.6, p, p, 0.3);
        {
            // The tail light: a bar across the back, the hottest thing on the machine.
            const double tf = 0.5 * (B.top[0].f + B.top[1].f), tu = 0.5 * (B.top[0].u + B.top[1].u);
            const double tp = std::min(1.0, p * 1.3);
            glow(bodyAt(tf, -1.05, tu), bodyAt(tf, 1.05, tu), 1.3, tp, tp, 0.55);
        }
        // At speed (a drop's boost, a breakdown): two streaks off the tail.
        if (fast) {
            for (int side : {-1, 1}) {
                const V3 s0 = bodyAt(B.top[0].f, side, 0.5 * (B.top[0].u + B.top[1].u));
                glow(s0, s0 - F * (1.8 * k), 0.6, p * 0.8, 0, 0.3);
            }
        }
    };
    // The dark fills (body shell, then the wheels' discs over it) in one path; then every glowing line: blooms,
    // halos, and the cores, the wheels' last, so they are clearly round glowing rings from the side and three-quarter
    // views, never hidden by the body.
    body();
    wheels();
    flush();
    if (!B.hubless)
        for (double wf : {B.front, B.rear}) dot3(c, at(wf, 0, B.r), 0.08, p);

    // Rezzing in (a round, or after a crash): a shrinking diamond on the floor.
    const double since = beat - cy.spawnBeat;
    if (since >= 0 && since < 1) {
        const double r = 0.7 + 3.5 * (1 - since) * (1 - since), q = pen * cy.level * (1 - since);
        const V3 c0 = P + V3{r, 0.02, 0}, c1 = P + V3{0, 0.02, r}, c2 = P + V3{-r, 0.02, 0}, c3 = P + V3{0, 0.02, -r};
        seg3(c, c0, c1, 0.7, q, q);
        seg3(c, c1, c2, 0.7, q, q);
        seg3(c, c2, c3, 0.7, q, q);
        seg3(c, c3, c0, 0.7, q, q);
    }
    bikeSegs_ += emitted_ + fillUsed_ - seg0;
}

void ChaseView::step(double beat, double bar, double drop, double build, double onset, double pen,
                     OrganicCanvas& c) {
    advance(beat, bar, drop, build, onset);
    if (!started_) return;
    draw(last_, std::clamp(finiteOr(build, 0), 0.0, 1.0), pen, c);
}

void ChaseView::draw(double beat, double build, double pen, OrganicCanvas& c) {
    const double hw = std::clamp(c.halfWidth(), 0.25, 8.0);
    hx_ = hw * 1.04 + 0.05;
    hy_ = 1.06;
    ppu_ = std::max(1.0, c.pixelsPerUnit());
    items_.clear();
    chunks_.clear();
    order_.clear();
    fillUsed_ = 0;
    bikeSegs_ = 0;
    const auto& g = sim_.config();
    const bool fast = sim_.boosted(long(std::floor(beat))) || build >= 0.5;
    const double frac = beat - std::floor(beat);
    const double sinceDrop = beat - sim_.dropBeat();
    const double flash = sinceDrop >= 0 && sinceDrop < 4 ? std::exp(-1.2 * sinceDrop) : 0.0;
    const V3 eye = cam_.eye;
    const double H = cfg_.walH;

    // The floor: grid lines pulsing on the beat, the arena's border brighter; outside the arena the floor goes on,
    // dimmer and coarser, towards the horizon (only near the eye: the fog hides the rest).
    {
        const double x0 = -0.5, x1 = g.cols - 0.5, z0 = -0.5, z1 = g.rows - 0.5;
        const double gp = pen * cfg_.grid * (0.75 + 0.25 * std::exp(-5.0 * frac)) + pen * 0.25 * flash * cfg_.grid;
        const double bp = std::min(1.0, pen * (0.35 + 1.4 * cfg_.grid) + pen * 0.4 * flash);
        if (gp > 0.002) {
            const double reach = 1.25 * fogDist_, ext = 24;
            const double lx = std::max(x0 - ext, eye.x - reach), hx = std::min(x1 + ext, eye.x + reach);
            const double lz = std::max(z0 - ext, eye.z - reach), hz = std::min(z1 + ext, eye.z + reach);
            const double op = 0.4 * gp;
            for (int k = int(std::ceil(lx - x0)); x0 + k <= hx; ++k) {
                const double x = x0 + k;
                if (k == 0 || x == x1) continue;
                const bool in = x > x0 && x < x1;
                if (!in && (k & 1)) continue;
                if (in) {
                    // The arena's part bright, beyond its edges dim.
                    if (lz < z1 && hz > z0) line3(c, {x, 0, std::max(z0, lz)}, {x, 0, std::min(z1, hz)}, 0.35, gp, gp);
                    if (lz < z0) line3(c, {x, 0, lz}, {x, 0, std::min(z0, hz)}, 0.3, op, op);
                    if (hz > z1) line3(c, {x, 0, std::max(z1, lz)}, {x, 0, hz}, 0.3, op, op);
                } else {
                    line3(c, {x, 0, lz}, {x, 0, hz}, 0.3, op, op);
                }
            }
            for (int k = int(std::ceil(lz - z0)); z0 + k <= hz; ++k) {
                const double z = z0 + k;
                if (k == 0 || z == z1) continue;
                const bool in = z > z0 && z < z1;
                if (!in && (k & 1)) continue;
                if (in) {
                    if (lx < x1 && hx > x0) line3(c, {std::max(x0, lx), 0, z}, {std::min(x1, hx), 0, z}, 0.35, gp, gp);
                    if (lx < x0) line3(c, {lx, 0, z}, {std::min(x0, hx), 0, z}, 0.3, op, op);
                    if (hx > x1) line3(c, {std::max(x1, lx), 0, z}, {hx, 0, z}, 0.3, op, op);
                } else {
                    line3(c, {lx, 0, z}, {hx, 0, z}, 0.3, op, op);
                }
            }
        }
        line3(c, {x0, 0, z0}, {x1, 0, z0}, 0.7, bp, bp);
        line3(c, {x1, 0, z0}, {x1, 0, z1}, 0.7, bp, bp);
        line3(c, {x1, 0, z1}, {x0, 0, z1}, 0.7, bp, bp);
        line3(c, {x0, 0, z1}, {x0, 0, z0}, 0.7, bp, bp);
    }

    // Walls as chunks of at most 3 cells (so the far-to-near order holds), a ribbon each.
    const double tailB = sim_.tailBeats(), u = beat - tailB;
    const auto wallPen = [&](double level, double t) {
        const double age = std::clamp((beat - t) / tailB, 0.0, 1.0);
        const double p = level * (0.92 - 0.42 * age * std::sqrt(age));
        return pen * (p + (1.0 - p) * 0.85 * flash);
    };
    const auto heightAt = [&](double t) {
        const double age = std::clamp((beat - t) / tailB, 0.0, 1.0);
        return H * (age < 0.8 ? 1.0 : 1.0 - 0.75 * (age - 0.8) / 0.2);
    };
    const auto addRun = [&](V3 a, V3 b, double ta, double tb, double level, bool ghost, double gp, double gh) {
        const int n = std::clamp(int(std::ceil(length(b - a) / 3.0 - 1e-9)), 1, 64);
        for (int k = 0; k < n; ++k) {
            const double f0 = double(k) / n, f1 = double(k + 1) / n;
            Chunk ch;
            ch.a = lerp(a, b, f0);
            ch.b = lerp(a, b, f1);
            const double t0 = ta + (tb - ta) * f0, t1 = ta + (tb - ta) * f1;
            if (ghost) {
                ch.h = gh;
                ch.penTop = gp;
            } else {
                ch.h = heightAt(0.5 * (t0 + t1));
                ch.penTop = 0.5 * (wallPen(level, t0) + wallPen(level, t1));
            }
            ch.penBot = 0.5 * ch.penTop;
            ch.penFill = ghost ? 0 : pen * level * (0.42 + 0.3 * flash);
            ch.ghost = ghost;
            ch.fill = false;
            const V3 mid = lerp(ch.a, ch.b, 0.5) + V3{0, 0.5 * ch.h, 0};
            const double d = length(mid - eye);
            if (d > 1.6 * fogDist_) continue;
            // How close the camera is to the ribbon: right at it, the wall turns see-through.
            const V3 ab = ch.b - ch.a;
            const double l2 = dot(ab, ab);
            const double t = l2 > 0 ? std::clamp(dot(V3{eye.x, 0, eye.z} - ch.a, ab) / l2, 0.0, 1.0) : 0.0;
            const V3 q = ch.a + ab * t;
            const double dy = eye.y > ch.h ? eye.y - ch.h : eye.y < 0 ? -eye.y : 0.0;
            const double dn = std::sqrt((eye.x - q.x) * (eye.x - q.x) + (eye.z - q.z) * (eye.z - q.z) + dy * dy);
            ch.fade = smooth((dn - 0.3) / 1.2);
            items_.push_back({d, 0, int(chunks_.size())});
            chunks_.push_back(ch);
        }
    };
    double tailDepth[CyclesSim::kMaxCycles];
    std::fill(tailDepth, tailDepth + CyclesSim::kMaxCycles, 1e300);
    for (int i = 0; i < g.num; ++i) {
        const auto& cy = sim_.cycles()[std::size_t(i)];
        const auto& tr = cy.trail;
        if (!cy.alive || tr.size() < 2) continue;
        V3 hp;
        double ha;
        headAt(i, beat, hp, ha);
        struct Vt {
            double x, y, t;
        };
        std::vector<Vt> verts;
        verts.reserve(64);
        std::size_t j = 0;
        while (j + 1 < tr.size() && tr[j + 1].t <= u) ++j;
        if (tr[j].t < u && j + 1 < tr.size()) {
            const auto &a = tr[j], &b = tr[j + 1];
            const double f = (u - a.t) / (b.t - a.t);
            verts.push_back({a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f, u});
        } else {
            verts.push_back({double(tr[j].x), double(tr[j].y), tr[j].t});
        }
        for (std::size_t k = j + 1; k + 1 < tr.size() && tr[k].t < beat; ++k) {
            const auto &p = tr[k - 1], &q = tr[k], &r = tr[k + 1];
            const bool corner = (q.x - p.x) != (r.x - q.x) || (q.y - p.y) != (r.y - q.y);
            if (corner) verts.push_back({double(q.x), double(q.y), q.t});
        }
        // The ribbon comes out of the machine's tail, not through its body: end it a little short of the head.
        {
            const Vt& pv = verts.back();
            const double dx = hp.x - pv.x, dz = hp.z - pv.y, len = std::hypot(dx, dz);
            const double cut = std::min(bikeRear(), 0.9 * len);  // under the rear wheel
            if (len > 1e-9) verts.push_back({hp.x - dx / len * cut, hp.z - dz / len * cut, beat});
        }
        if (verts.size() < 2) continue;
        const std::size_t before = items_.size();
        for (std::size_t k = 0; k + 1 < verts.size(); ++k)
            addRun({verts[k].x, 0, verts[k].y}, {verts[k + 1].x, 0, verts[k + 1].y}, verts[k].t, verts[k + 1].t,
                   cy.level, false, 0, 0);
        // The chunk at the tail (the last one added): the machine goes on top of it whatever the midpoints say.
        if (items_.size() > before && items_.back().kind == 0) tailDepth[i] = items_.back().depth;
    }
    // A crashed cycle's wall, dissolving from the crash back to its tail (sinking, outlines only).
    for (const auto& gh : sim_.ghosts()) {
        const std::size_t n = gh.pts.size();
        const double a = beat - gh.t0;
        if (a < 0 || n < 2) continue;
        const double p = pen * gh.level * 0.92 * (1.0 - 0.5 * a / CyclesSim::kDissolveBeats);
        const double h = H * std::max(0.1, 1.0 - 0.7 * a / CyclesSim::kDissolveBeats);
        std::size_t last = n;
        while (last > 0 && gh.t0 + CyclesSim::kDissolveBeats * double(n - last) / double(n - 1) <= beat) --last;
        for (std::size_t j = 0, k = 0; j + 1 < last; j = k) {
            k = j + 1;
            while (k + 1 < last && gh.pts[k + 1].x - gh.pts[k].x == gh.pts[j + 1].x - gh.pts[j].x &&
                   gh.pts[k + 1].y - gh.pts[k].y == gh.pts[j + 1].y - gh.pts[j].y)
                ++k;
            addRun({double(gh.pts[j].x), 0, double(gh.pts[j].y)}, {double(gh.pts[k].x), 0, double(gh.pts[k].y)}, 0,
                   0, gh.level, true, p, h);
        }
    }

    // The fill budget: nearest walls first (they hide the most); the rest as outlines.
    {
        std::vector<std::pair<double, int>> near;
        near.reserve(chunks_.size());
        for (const auto& it : items_)
            if (it.kind == 0 && !chunks_[std::size_t(it.idx)].ghost && chunks_[std::size_t(it.idx)].fade > 0.98)
                near.push_back({it.depth, it.idx});
        std::sort(near.begin(), near.end());
        double budget = kFillBudget - 400.0 * g.num;
        for (const auto& [d, k] : near) {
            auto& ch = chunks_[std::size_t(k)];
            const V3 q[4] = {ch.a, ch.b, ch.b + V3{0, ch.h, 0}, ch.a + V3{0, ch.h, 0}};
            double xs[32], ys[32];
            const int m = screenPoly(q, 4, xs, ys);
            if (m < 3) continue;
            const double cost = fillCost(ys, m, ppu_);
            if (cost <= budget) {
                ch.fill = true;
                budget -= cost;
            }
        }
    }

    // Cycles.
    for (int i = 0; i < g.num; ++i) {
        const auto& cy = sim_.cycles()[std::size_t(i)];
        if (!cy.alive || cy.trail.empty()) continue;
        V3 p;
        double a;
        headAt(i, beat, p, a);
        items_.push_back({std::min(length(p + V3{0, 0.25, 0} - eye), tailDepth[i] * (1 - 1e-9)), 1, i});
    }

    // Sparks, fragments, explosions, speed lines: "bits" between the occluders.
    struct Bit {
        V3 a, b;
        double pen, size;
    };
    std::vector<Bit> bits;
    const auto addBit = [&](V3 a, V3 b, double p, double size) {
        if (p <= 0.004) return;
        const double d = length(lerp(a, b, 0.5) - eye);
        if (d > 1.6 * fogDist_) return;
        items_.push_back({d, 2, int(bits.size())});
        bits.push_back({a, b, p, size});
    };
    {
        const auto& sp = sim_.sparks();
        const std::size_t stride = std::max<std::size_t>(1, (sp.size() + kMaxBits - 1) / kMaxBits);
        for (std::size_t k = 0; k < sp.size(); k += stride) {
            const auto& s = sp[k];
            const double a = beat - s.t0;
            if (a < 0 || a >= s.life) continue;
            const double e = (1.0 - std::exp(-2.0 * a)) / 2.0, f = 1.0 - a / s.life;
            const std::uint64_t key = bitsOf(s.t0) ^ std::uint64_t(k) * 0x9e3779b97f4a7c15ULL;
            const double h0 = H * (0.15 + 0.85 * hashU(key, 1)), vu = 0.5 + 2.5 * hashU(key, 2);
            const double y = std::max(0.02, h0 + vu * a - 1.1 * a * a);
            const V3 at{s.x + s.vx * e, y, s.y + s.vy * e};
            if (s.len > 0) {
                const double ang = s.ang + s.spin * e;
                const V3 dir = unit({std::cos(ang), 0.7 * std::sin(1.3 * ang), std::sin(ang)}, {1, 0, 0}) * (0.5 * s.len);
                addBit(at - dir, at + dir, pen * s.level * f, -1);
            } else {
                addBit(at, at, pen * s.level * f * f, 0.12 + 0.14 * f);
            }
        }
        for (const auto& k : sim_.blasts()) {
            const double a = beat - k.t;
            if (a < 0 || a > 1) continue;
            const V3 at{k.x, 0, k.y};
            if (a < 0.3) addBit(at + V3{0, 0.35, 0}, at + V3{0, 0.35, 0}, pen, 2.6 * (1.0 - a / 0.3));
            if (a < 0.6) addBit(at, at + V3{0, 0.5 + 5.0 * a / 0.6, 0}, pen * (1.0 - a / 0.6), -2);
            if (a < 0.75) {
                const double r = 0.5 + 6.0 * a, p = pen * std::pow(1.0 - a / 0.75, 1.5);
                for (int q = 0; q < 16; ++q) {
                    const double a0 = 2 * kPi * q / 16.0, a1 = 2 * kPi * (q + 1) / 16.0;
                    addBit(at + V3{r * std::cos(a0), 0.05, r * std::sin(a0)},
                           at + V3{r * std::cos(a1), 0.05, r * std::sin(a1)}, p, -1.4);
                }
            }
        }
        // Speed lines: on a drop, and through a breakdown.
        const double sw = std::max(dropW_, build >= 0.5 ? 0.55 * build : 0.0);
        if (sw > 0.05 && !orbiting_ && sim_.cycles()[std::size_t(target_)].alive) {
            V3 P;
            double a;
            headAt(target_, beat, P, a);
            const V3 F{std::cos(a), 0, std::sin(a)}, R{std::sin(a), 0, -std::cos(a)};
            for (int j = 0; j < 40; ++j) {
                const double u1 = hashU(seed_, 900 + j), u2 = hashU(seed_, 1900 + j), u3 = hashU(seed_, 2900 + j),
                             u4 = hashU(seed_, 3900 + j);
                const double side = (u1 < 0.5 ? -1.0 : 1.0) * (0.6 + 3.5 * u2), y = 0.08 + 2.2 * u3 * u3;
                double fo = std::fmod(u4 * 22.0 - beat * 16.0, 22.0);
                if (fo < 0) fo += 22.0;
                fo -= 6.0;
                const V3 a0 = P + F * fo + R * side + V3{0, y, 0};
                addBit(a0, a0 - F * 2.2, pen * 0.6 * sw, -0.6);
            }
        }
    }

    // Far to near: each occluder (a wall chunk, a cycle) on its own path over what is behind it.
    std::sort(items_.begin(), items_.end(), [](const Item& a, const Item& b) {
        return a.depth != b.depth ? a.depth > b.depth : (a.kind != b.kind ? a.kind < b.kind : a.idx < b.idx);
    });
    for (const auto& it : items_) {
        if (it.kind == 0) {
            const auto& ch = chunks_[std::size_t(it.idx)];
            const V3 a1 = ch.a + V3{0, ch.h, 0}, b1 = ch.b + V3{0, ch.h, 0};
            if (!ch.ghost && ch.fade > 0.98) {
                c.newPath();
                order_.push_back(it.depth);
                const V3 q[4] = {ch.a, ch.b, b1, a1};
                double xs[32], ys[32];
                const int m = screenPoly(q, 4, xs, ys);
                if (m >= 3) {
                    const double fp = ch.penFill * fog(it.depth);
                    if (ch.fill) {
                        c.fillPolygon(xs, ys, m, fp);
                        fillUsed_ += long(fillCost(ys, m, ppu_));
                    }
                }
                c.newPath();
                if (!ch.fill) {
                    // Over budget: a sparse body of horizontal lines instead of the fill.
                    for (double f : {0.33, 0.66})
                        seg3(c, ch.a + V3{0, ch.h * f, 0}, ch.b + V3{0, ch.h * f, 0}, 0.5, ch.penFill, ch.penFill);
                }
            }
            const double k = ch.ghost ? 1.0 : 0.35 + 0.65 * ch.fade;
            seg3(c, a1, b1, 1.0, k * ch.penTop, k * ch.penTop);
            seg3(c, ch.a, ch.b, 0.6, k * ch.penBot, k * ch.penBot);
        } else if (it.kind == 1) {
            order_.push_back(it.depth);
            drawCycle(c, it.idx, beat, pen, fast);
        } else {
            const auto& b = bits[std::size_t(it.idx)];
            if (b.size > 1) dot3(c, b.a, b.size, b.pen, 0.3);  // a blast's flash
            else if (b.size > 0) dot3(c, b.a, b.size, b.pen, 0.018);
            else seg3(c, b.a, b.b, -b.size, b.pen, b.pen);
        }
    }
}

// ------------------------------------------------------------------ the organic

namespace {

class Chase : public OrganicBody {
public:
    Chase(scene::OrganicInstance& in, std::uint64_t seed) : view_(config(in), seed) {}

    static ChaseConfig config(scene::OrganicInstance& in) {
        ChaseConfig c;
        c.game.num = keyCount(in, "NUM", 4, 1, CyclesSim::kMaxCycles);
        c.game.cols = keyCount(in, "COLS", 48, 8, 400);
        c.game.rows = keyCount(in, "ROWS", 27, 6, 300);
        c.game.spd = keyCount(in, "SPD", 2, 1, 8);
        c.game.turn = keyCount(in, "TURN", 1, 1, 4);
        c.game.tail = keyNum(in, "TAIL", 4, 0.25, 64);
        c.game.reset = keyNum(in, "RSD", 16, 0, 1024);
        c.game.mirror = keyNum(in, "MIRR", 1, 0, 1);
        c.game.out = keyNum(in, "OUT", 2, 0.25, 64);
        c.cam = keyNum(in, "CAM", 4.2, 0.5, 60);
        c.camH = keyNum(in, "CAMH", 1.5, 0.05, 60);
        c.fov = keyNum(in, "FOV", 62, 15, 130);
        c.walH = keyNum(in, "WALH", 0.8, 0, 10);
        c.fog = keyNum(in, "FOG", 42, 2, 1000);
        c.grid = keyNum(in, "GRID", 0.45, 0, 1);
        c.cut = keyNum(in, "CUT", 2, 0.25, 1024);
        c.wdth = keyNum(in, "WDTH", 2.4, 0, 50);
        c.shot = keyCount(in, "SHOT", 0, 0, ChaseView::kShots);
        c.bike = keyCount(in, "BIKE", 0, 0, 2);
        return c;
    }

    void step(const OrganicStep& s, OrganicCanvas& c) override {
        view_.step(s.beat, s.bar, dropOf(s), buildOf(s), onsetOf(s), s.pen, c);
    }

    std::string summary() const override {
        const auto& c = view_.config();
        char b[160];
        std::snprintf(b, sizeof b, "%d cycles on %d x %d in 3D, %d cells per beat, camera %g behind, cut every %g bars",
                      c.game.num, c.game.cols, c.game.rows, c.game.spd, c.cam, c.cut);
        return b;
    }

private:
    ChaseView view_;
};

}  // namespace

std::unique_ptr<OrganicBody> makeChase(scene::OrganicInstance& inst, std::uint64_t seed) {
    return std::make_unique<Chase>(inst, seed);
}

}  // namespace reforce::engine::arcade
