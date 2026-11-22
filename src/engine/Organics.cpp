#include "engine/Organics.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <utility>

#include "expr/Rng.hpp"

namespace reforce::engine {

namespace {
constexpr double kPi = 3.141592653589793;
constexpr double kSwayHz = 0.3;      // sway wave frequency
constexpr int kMaxIterationsPerStep = 8;

std::string lowerTrim(std::string s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    const auto e = s.find_last_not_of(" \t\r\n");
    s = b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

double finiteOr(double v, double fb) { return std::isfinite(v) ? v : fb; }
}  // namespace

const char* organicKindName(OrganicKind k) {
    switch (k) {
        case OrganicKind::Tree: return "tree";
        case OrganicKind::Pendulums: return "pendulums";
        case OrganicKind::Body: return "body";
        case OrganicKind::Field: return "field";
    }
    return "?";
}

const char* organicFieldKindName(OrganicField::Kind k) {
    return k == OrganicField::Kind::Julia ? "julia" : "ripples";
}

std::optional<OrganicKind> organicKind(const scene::CompiledConfig& cfg, std::string* written) {
    std::string kind;
    if (const scene::CompiledKey* k = cfg.find("KIND")) kind = k->text;
    if (written) *written = kind;
    kind = lowerTrim(kind);
    if (kind.empty() || kind == "tree") return OrganicKind::Tree;
    if (kind == "pendulums" || kind == "pendulum") return OrganicKind::Pendulums;
    for (const auto& k : organicBodyKinds())
        if (kind == k) return OrganicKind::Body;
    if (kind == "ripples" || kind == "julia") return OrganicKind::Field;
    return std::nullopt;
}

FrameMapping organicMapping(double width, double height) {
    FrameMapping m;
    m.width = width;
    m.height = height;
    m.xExtent = height > 0 ? width / height : 1.0;
    m.yExtent = 1.0;
    return m;
}

OrganicRun::OrganicRun(std::shared_ptr<const scene::CompiledConfig> cfg, const scene::Environment& env,
                       std::string name, double startTime, double fade, double defaultHold,
                       std::shared_ptr<const TransitionFunction> fn, int maxNodes)
    : cfg_(std::move(cfg)), name_(std::move(name)), start_(startTime) {
    scene::Environment e = env;
    e.loadTime = startTime;
    inst_ = std::make_unique<scene::OrganicInstance>(*cfg_, e);
    double hold = inst_->duration();
    if (!(std::isfinite(hold) && hold >= 0)) hold = defaultHold;
    env_.fade = std::max(0.0, fade);
    env_.hold = std::min(hold, 24.0 * 3600.0);
    env_.fn = std::move(fn);

    auto count = [&](const char* key, double fb, int lo, int hi) {
        const double v = finiteOr(inst_->loaded(key, fb), fb);
        return std::clamp(int(std::lround(std::clamp(v, double(lo), double(hi)))), lo, hi);
    };
    kind_ = organicKind(*cfg_).value_or(OrganicKind::Tree);
    {
        std::string k;
        if (const scene::CompiledKey* kk = cfg_->find("KIND")) k = lowerTrim(kk->text);
        kindName_ = kind_ == OrganicKind::Tree ? "tree" : kind_ == OrganicKind::Pendulums ? "pendulums" : k;
    }
    if (kind_ == OrganicKind::Body) {
        body_ = makeOrganicBody(kindName_, *inst_, env.seed);
        wither_ = false;
        return;
    }
    auto num = [&](const char* key, double fb, double lo, double hi) {
        return std::clamp(finiteOr(inst_->loaded(key, fb), fb), lo, hi);
    };
    if (kind_ == OrganicKind::Field) {
        wither_ = false;
        rng_ = env.seed ^ 0xd209ULL;
        field_.flags = unsigned(num("FLAG", 0, 0, 1e9));
        if (kindName_ == "julia") {
            field_.kind = OrganicField::Kind::Julia;
            field_.iter = count("ITER", 120, 8, 1000);
        } else {
            field_.kind = OrganicField::Kind::Ripples;
            dropRate_ = num("RATE", 1, 0, 64);
            dropRadius_ = num("RAD", 0.03, 0.002, 0.5);
            field_.speed = num("SPD", 0.4, 0.01, 4);
            field_.decay = num("DECY", 2.5, 0.05, 600);
            field_.reflect = num("EDGE", 0, 0, 1) >= 0.5;
        }
        return;
    }
    if (kind_ == OrganicKind::Pendulums) {
        pendulums_ = count("NUM", 15, 1, 200);
        cycleBars_ = num("CYC", 16, 0.25, 256);
        osc_ = num("OSC", cycleBars_, 0, 1000);
        spread_ = num("SPRD", 3.0, 0, 20);
        length_ = num("LEN", 1.4, 0, 10);
        view_ = int(std::lround(num("VIEW", 0, 0, 2)));
        bobWidth_ = num("BOBW", 10, 0, 200);
        stringWidth_ = num("WDTH", 1.5, 0, 200);
        wither_ = false;
        return;
    }
    TreeParams p;
    const int pts = count("PTS", 300, 10, 4000);
    using PK = scene::OrganicInstance::PointKey;
    std::vector<std::vector<double>> v;
    if (inst_->defines("PX") || inst_->defines("PY")) {
        const PK keys[] = {{"PX", 0.0}, {"PY", 0.0}};
        inst_->evaluatePoints(pts, keys, v);
        p.pointsX = std::move(v[0]);
        p.pointsY = std::move(v[1]);
    } else {
        // Default crown: an ellipse .9 x .55 centred at (0, .3), evenly filled.
        expr::Pcg32 rng(env.seed ^ 0x7ee5ULL);
        for (int i = 0; i < pts; ++i) {
            const double a = rng.uniform01Exclusive() * 2 * kPi, r = std::sqrt(rng.uniform01());
            p.pointsX.push_back(0.9 * r * std::cos(a));
            p.pointsY.push_back(0.3 + 0.55 * r * std::sin(a));
        }
    }
    const int roots = count("RTS", 1, 1, 64);
    const PK rootKeys[] = {{"RTX", 0.0}, {"RTY", -1.0}, {"RTA", kPi / 2}};
    inst_->evaluatePoints(roots, rootKeys, v);
    for (std::size_t i = 0; i < std::size_t(roots); ++i) p.roots.push_back({v[0][i], v[1][i], v[2][i]});
    p.segment = inst_->loaded("SEG", 0.02);
    p.influence = inst_->loaded("INFL", 0.3);
    p.kill = inst_->loaded("KILL", 2 * finiteOr(p.segment, 0.02));
    p.jitter = inst_->loaded("JITR", 0.15);
    p.maxNodes = std::max(2, maxNodes);
    tree_ = std::make_unique<TreeGrowth>(std::move(p), env.seed);

    width_ = std::clamp(finiteOr(inst_->loaded("WDTH", 8.0), 8.0), 0.0, 200.0);
    tip_ = std::clamp(finiteOr(inst_->loaded("TIP", 1.0), 1.0), 0.0, 200.0);
    budWidth_ = std::clamp(finiteOr(inst_->loaded("BUDW", 4.0), 4.0), 0.0, 200.0);
    wither_ = finiteOr(inst_->loaded("WTHR", 1.0), 1.0) != 0.0;
}

std::string OrganicRun::summary() const {
    char b[96];
    if (body_) return body_->summary();
    if (kind_ == OrganicKind::Field) {
        if (field_.kind == OrganicField::Kind::Julia) std::snprintf(b, sizeof b, "%d iterations", field_.iter);
        else
            std::snprintf(b, sizeof b, "%g drop(s) per beat, speed %g, decay %g s%s", dropRate_, field_.speed,
                          field_.decay, field_.reflect ? ", reflecting edges" : "");
        return b;
    }
    if (kind_ == OrganicKind::Pendulums)
        std::snprintf(b, sizeof b, "%d pendulum(s), %g bars per cycle", pendulums_, cycleBars_);
    else
        std::snprintf(b, sizeof b, "%d point(s), %zu root(s), %zu node(s)", tree_->livePoints(),
                      tree_->params().roots.size(), tree_->nodes().size());
    return b;
}

void OrganicRun::stepPendulums(double t, const scene::OrganicFrame& f, double penScale, const SpriteAudio& audio,
                               const FrameMapping& map, const LineStyle& style, std::vector<LineSegment>& out) {
    // Cycle position: Phas when given; else the beat clock's bars (aligned to the phrase) / Cyc while it is
    // confident; else 120 BPM from the start.
    double phase;
    if (std::isfinite(f.phas)) phase = f.phas;
    else if (audio.sync.on > 0) phase = audio.sync.bar / cycleBars_;
    else phase = (t - start_) * 0.5 / cycleBars_;  // 2 beats/s, 4 beats per bar
    const bool side_ = view_ == 1, ring = view_ == 2;
    const double amp = finiteOr(f.amp, side_ ? 0.5 : ring ? 0.25 : 0.45);
    const float pen = float(std::clamp(finiteOr(f.pen, 0.0) * penScale, 0.0, 1.0));
    const float strPen = float(std::clamp(finiteOr(f.pen, 0.0) * finiteOr(f.strg, side_ ? 0.6 : 0.35) * penScale,
                                          0.0, 1.0));
    if (pen <= 0.f && strPen <= 0.f) return;
    const double X = finiteOr(f.x, 0.0), Y = finiteOr(f.y, 0.0);
    const double scal = std::clamp(finiteOr(f.scale, 1.0), -100.0, 100.0);
    const double rot = finiteOr(f.rot, 0.0);
    const double rc = std::cos(rot) * scal, rs = std::sin(rot) * scal;
    const double limitPx = 64.0 * std::max(map.width, map.height);
    auto toPx = [&](double x, double y, float& ox, float& oy) {
        const double qx = map.toPixelX(X + rc * x - rs * y), qy = map.toPixelY(Y + rs * x + rc * y);
        ox = float(qx);
        oy = float(qy);
        return std::isfinite(qx) && std::isfinite(qy) && std::abs(qx) < limitPx && std::abs(qy) < limitPx;
    };
    const float pathId = float(out.size());
    const float bobR = float(0.5 * lineWidthPixels(bobWidth_ * std::abs(scal), style, map.width, map.height));
    const float strR = float(0.5 * lineWidthPixels(stringWidth_ * std::abs(scal), style, map.width, map.height));
    float px = 0, py = 0, fx = 0, fy = 0;
    bool havePrev = false, haveFirst = false;
    for (int k = 0; k < pendulums_; ++k) {
        const double x = pendulums_ > 1 ? -0.5 * spread_ + spread_ * double(k) / double(pendulums_ - 1) : 0.0;
        const double swing = std::cos(2 * kPi * (osc_ + double(k)) * phase);
        float bx, by;
        if (ring) {
            // A ring: pivots around a circle of radius Len / 2, bobs swinging in and out along their radius.
            const double a = 2 * kPi * double(k) / double(pendulums_) + kPi / 2;
            const double r = 0.5 * length_ + amp * swing;
            if (!toPx(r * std::cos(a), r * std::sin(a), bx, by)) continue;
            if (havePrev && strPen > 0.f) out.push_back({px, py, bx, by, strPen, strPen, strR, pathId});
            if (!haveFirst) {
                fx = bx;
                fy = by;
                haveFirst = true;
            }
            px = bx;
            py = by;
            havePrev = true;
        } else if (side_) {
            // Side view: strings hang from a bar at y = .9; bobs swing on them.
            const double th = amp * swing;
            const double top = 0.9;
            float ax, ay;
            if (!toPx(x, top, ax, ay) || !toPx(x + length_ * std::sin(th), top - length_ * std::cos(th), bx, by))
                continue;
            if (strPen > 0.f) out.push_back({ax, ay, bx, by, strPen, strPen, strR, pathId});
        } else {
            // From above: each bob moves up and down its own column; the line through them is the "snake".
            if (!toPx(x, amp * swing, bx, by)) continue;
            if (havePrev && strPen > 0.f) out.push_back({px, py, bx, by, strPen, strPen, strR, pathId});
            px = bx;
            py = by;
            havePrev = true;
        }
        if (pen > 0.f) out.push_back({bx, by, bx, by, pen, pen, bobR, pathId});
    }
    if (ring && haveFirst && havePrev && pendulums_ > 2 && strPen > 0.f)
        out.push_back({px, py, fx, fy, strPen, strPen, strR, pathId});  // close the ring
}

void OrganicRun::stepField(double t, double dtSim, const scene::OrganicFrame& f, double fader,
                           const SpriteAudio& audio, const FrameMapping& map) {
    OrganicField& o = field_;
    o.reset = !fieldStarted_;
    fieldStarted_ = true;
    o.dt = dtSim;
    o.pen = std::clamp(finiteOr(f.pen, 0.0), 0.0, 1.0);
    o.opacity = std::clamp(fader, 0.0, 1.0);
    o.drops.clear();
    const double aspect = map.xExtent > 0 ? map.xExtent : 1.0;
    if (o.kind == OrganicField::Kind::Julia) {
        o.cre = finiteOr(inst_->frameKey("CRE", -0.8), -0.8);
        o.cim = finiteOr(inst_->frameKey("CIM", 0.156), 0.156);
        o.x = finiteOr(f.x, 0.0);
        o.y = finiteOr(f.y, 0.0);
        o.scale = std::clamp(finiteOr(f.scale, 1.0), -1e4, 1e4);
        o.rot = finiteOr(f.rot, 0.0);
        o.gain = std::clamp(finiteOr(inst_->frameKey("GAIN", 1.0), 1.0), 0.0, 100.0);
        o.bands = std::clamp(finiteOr(inst_->frameKey("BAND", 0.0), 0.0), 0.0, 1000.0);
        o.phase = finiteOr(f.phas, 0.0);
        fieldOn_ = o.pen > 0 && o.opacity > 0;
        return;
    }
    // Ripples: drops on the beat lines (1 / Rate beats apart; 2 beats per second of age without a beat clock).
    o.gain = std::clamp(finiteOr(inst_->frameKey("GAIN", 3.0), 3.0), 0.0, 100.0);
    const double amp = std::clamp(finiteOr(f.amp, 1.0), -100.0, 100.0);
    auto uniform = [&] {
        rng_ = rng_ * 6364136223846793005ULL + 1442695040888963407ULL;
        return double(rng_ >> 11) * (1.0 / 9007199254740992.0);
    };
    const bool hasDx = inst_->defines("DX"), hasDy = inst_->defines("DY");
    const double dx = hasDx ? finiteOr(inst_->frameKey("DX", 0.0), 0.0) : 0.0;
    const double dy = hasDy ? finiteOr(inst_->frameKey("DY", 0.0), 0.0) : 0.0;
    // x, y in tree units (x = +-aspect at the frame's sides, y = +-1 at the top / bottom) -> frame UV.
    auto drop = [&](double radius, double a, bool keyed) {
        const double x = keyed && hasDx ? dx : (uniform() * 2 - 1) * 0.8 * aspect;
        const double y = keyed && hasDy ? dy : (uniform() * 2 - 1) * 0.8;
        o.drops.push_back({0.5 + 0.5 * x / aspect, 0.5 - 0.5 * y, radius, a});
    };
    if (dropRate_ > 0) {
        const double beat = audio.sync.on > 0 ? audio.sync.beat : 2.0 * (t - start_);
        const long long idx = (long long)std::floor(beat * dropRate_ + 1e-9);
        if (!haveDrop_) {
            lastDrop_ = idx - 1;  // the first drop at once
            haveDrop_ = true;
        }
        if (idx < lastDrop_) lastDrop_ = idx;  // the clock jumped back
        if (idx - lastDrop_ > 4) lastDrop_ = idx - 1;
        for (; lastDrop_ < idx; ++lastDrop_) drop(dropRadius_, amp, true);
    }
    const double rain = std::clamp(finiteOr(inst_->frameKey("RAIN", 0.0), 0.0), 0.0, 200.0);
    rainAcc_ = std::min(rainAcc_ + rain * dtSim, 8.0);
    for (; rainAcc_ >= 1.0; rainAcc_ -= 1.0) drop(dropRadius_ * 0.6, amp * (0.25 + 0.2 * uniform()), false);
    fieldOn_ = true;  // the water keeps moving while it is invisible
}

void OrganicRun::step(double t, const SpriteAudio& audio, const FrameMapping& map, const LineStyle& style,
                      std::vector<LineSegment>& out) {
    const double dtSim = lastT_ < 0 ? 0.0 : std::clamp(t - lastT_, 0.0, 0.25);
    lastT_ = t;
    const double age = t - start_;
    scene::OrganicFrameInput in;
    in.t = t;
    in.startTime = start_;
    in.endTime = endTime();
    in.dt = in.endTime > start_ ? std::clamp(age / (in.endTime - start_), 0.0, 1.0) : 0.0;
    in.fader = env_(age);
    in.bass = audio.bass;
    in.level = audio.level;
    in.energy = audio.energy;
    in.mid = audio.mid;
    in.treble = audio.treble;
    in.beat = audio.beat;
    in.tempo = audio.tempo;
    in.linkOn = audio.linkOn;
    in.linkBpm = audio.linkBpm;
    in.linkBeat = audio.linkBeat;
    in.linkPhase = audio.linkPhase;
    in.sync = audio.sync;
    const scene::OrganicFrame f = inst_->evaluateFrame(in);
    if (kind_ == OrganicKind::Pendulums) {
        stepPendulums(t, f, in.fader, audio, map, style, out);
        return;
    }
    if (kind_ == OrganicKind::Field) {
        stepField(t, dtSim, f, in.fader, audio, map);
        return;
    }
    if (body_) {
        OrganicStep st;
        st.t = t;
        st.age = age;
        st.dt = dtSim;
        st.clock = audio.sync.on > 0;
        st.beat = st.clock ? audio.sync.beat : 2.0 * age;
        st.bar = st.clock ? audio.sync.bar : st.beat / 4.0;
        st.pen = std::clamp(finiteOr(f.pen, 0.0) * in.fader, 0.0, 1.0);
        st.frame = &f;
        st.audio = &audio;
        OrganicCanvas canvas(map, style, out, f.x, f.y, f.scale, f.rot);
        if (st.pen > 0) body_->step(st, canvas);
        return;
    }

    // Growth in simulation time; the newest generation is drawn at the fraction reached.
    const double trpX = std::clamp(finiteOr(f.trpX, 0.0), -10.0, 10.0);
    const double trpY = std::clamp(finiteOr(f.trpY, 0.0), -10.0, 10.0);
    if (!tree_->done()) {
        acc_ += std::clamp(finiteOr(f.grow, 0.0), 0.0, 1000.0) * dtSim;
        int n = 0;
        while (acc_ >= 1.0 && n < kMaxIterationsPerStep && !tree_->done()) {
            tree_->iterate(trpX, trpY);
            acc_ -= 1.0;
            ++n;
        }
        if (n == kMaxIterationsPerStep) acc_ = std::min(acc_, 1.0);
    }
    const double G = tree_->generation();
    double limit = tree_->done() ? G : std::max(0.0, G - 1.0 + std::clamp(acc_, 0.0, 1.0));
    // Withering: during the fade-out the tree ungrows, newest generations first.
    const bool fadingOut = (env_.earlyOut >= 0 && age >= env_.earlyOut) || age >= env_.fade + env_.hold;
    double penScale = in.fader;
    if (wither_) {
        penScale = 1.0;
        if (fadingOut) limit *= in.fader;
    }

    const auto& nodes = tree_->nodes();
    const std::size_t n = nodes.size();
    px_.resize(n);
    py_.resize(n);
    ang_.resize(n);
    // Sway: each node turns about its parent, more the thinner it is; the phase runs along the branches.
    const double sway = std::clamp(finiteOr(f.sway, 0.0), -3.0, 3.0);
    const double perNode = sway * 3.0 / std::max(G, 10.0);
    const double wt = 2 * kPi * kSwayHz * (t - start_);
    const double maxTips = std::max(1.0f, tree_->maxTips());
    for (std::size_t i = 0; i < n; ++i) {
        const TreeNode& nd = nodes[i];
        if (nd.parent < 0) {
            px_[i] = nd.x;
            py_[i] = nd.y;
            ang_[i] = 0;
            continue;
        }
        const std::size_t pa = std::size_t(nd.parent);
        const double thin = 1.0 - std::sqrt(std::min(1.0, double(nd.tips) / maxTips));
        const double a = ang_[pa] + perNode * thin * thin * std::sin(wt + nd.phase);
        const double ex = nd.x - nodes[pa].x, ey = nd.y - nodes[pa].y;
        const double c = std::cos(a), s = std::sin(a);
        ang_[i] = float(a);
        px_[i] = float(px_[pa] + c * ex - s * ey);
        py_[i] = float(py_[pa] + s * ex + c * ey);
    }

    // Whole-tree transform, then frame pixels.
    const double X = finiteOr(f.x, 0.0), Y = finiteOr(f.y, 0.0);
    const double scal = std::clamp(finiteOr(f.scale, 1.0), -100.0, 100.0);
    const double rot = finiteOr(f.rot, 0.0);
    const double rc = std::cos(rot) * scal, rs = std::sin(rot) * scal;
    const double absScal = std::abs(scal);
    const double limitPx = 64.0 * std::max(map.width, map.height);
    auto toPx = [&](double x, double y, float& ox, float& oy) {
        const double qx = map.toPixelX(X + rc * x - rs * y), qy = map.toPixelY(Y + rs * x + rc * y);
        ox = float(qx);
        oy = float(qy);
        return std::isfinite(qx) && std::isfinite(qy) && std::abs(qx) < limitPx && std::abs(qy) < limitPx;
    };
    const float pen = float(std::clamp(finiteOr(f.pen, 0.0) * penScale, 0.0, 1.0));
    const float budPen = float(std::clamp(finiteOr(f.pen, 0.0) * finiteOr(f.bud, 0.0) * penScale, 0.0, 1.0));
    if (pen <= 0.f && budPen <= 0.f) return;
    const float pathId = float(out.size());
    auto radius = [&](double lwdt) {
        return float(0.5 * lineWidthPixels(lwdt * absScal, style, map.width, map.height));
    };
    for (std::size_t i = 0; i < n; ++i) {
        const TreeNode& nd = nodes[i];
        if (nd.parent < 0) continue;
        const double frac = std::clamp(limit - double(nd.gen - 1), 0.0, 1.0);
        if (frac <= 0.0) continue;
        const std::size_t pa = std::size_t(nd.parent);
        const double bx = px_[pa] + frac * (px_[i] - px_[pa]), by = py_[pa] + frac * (py_[i] - py_[pa]);
        float ax, ay, cx, cy;
        if (!toPx(px_[pa], py_[pa], ax, ay) || !toPx(bx, by, cx, cy)) continue;
        const double w = tip_ + (width_ - tip_) * std::sqrt(std::min(1.0, double(nd.tips) / maxTips));
        const float r = radius(w);
        if (pen > 0.f) out.push_back({ax, ay, cx, cy, pen, pen, r, pathId});
        if (budPen > 0.f && nd.children == 0 && frac >= 1.0)
            out.push_back({cx, cy, cx, cy, budPen, budPen, std::max(r, radius(budWidth_)), pathId});
    }
}

}  // namespace reforce::engine
