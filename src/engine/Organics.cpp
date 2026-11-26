#include "engine/Organics.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <unordered_map>
#include <utility>

#include "expr/Rng.hpp"

namespace reforce::engine {

namespace {
constexpr double kPi = 3.141592653589793;
constexpr double kSwayHz = 0.3;      // sway wave frequency
constexpr int kMaxIterationsPerStep = 8;
constexpr int kMaxLeaves = 1500;  // two segments each

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
    return k == OrganicField::Kind::Julia ? "julia" : "mandel";
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
    if (kind == "julia") return OrganicKind::Field;
    if (kind == "mandel" || kind == "mandelbrot") return OrganicKind::Field;
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
        field_.flags = unsigned(num("FLAG", 0, 0, 1e9));
        if (kindName_ == "julia") {
            field_.kind = OrganicField::Kind::Julia;
            field_.iter = count("ITER", 120, 8, 1000);
        } else {  // mandel / mandelbrot (organicKind admits no other field kind)
            field_.kind = OrganicField::Kind::Mandel;
            MandelDiveParams mp;
            mp.diveBars = num("DIVE", 16, 0.25, 1024);
            mp.dbl = num("DBL", 2, 0, 64);
            mp.deep = num("DEEP", 34, 0, 44);
            mp.out = num("OUT", 4, 0, 256);
            mp.kick = num("KICK", 0.3, -4, 4);
            mp.drop = num("DROP", 6, 0, 40);
            mp.iter = count("ITER", 200, 8, 4000);
            mp.itk = num("ITK", 1.5, 0, 20);
            mp.maxIter = count("MAXI", 4000, 8, 20000);
            mp.entry = num("ENT", 6, 0, 64);
            mp.align = num("ALGN", 16, 0, 1024);
            mp.minis = count("MINI", 0, 0, 2);
            mp.affine = num("AFF", 1, 0, 1) >= 0.5;
            mp.shared = true;
            mandel_ = MandelDive(mp, env.seed);
            mandelPrec_ = count("PREC", 0, 0, 3);
            field_.res = num("RES", 1, 0.25, 1);
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
    seed_ = env.seed;
    maxNodes_ = std::max(2, maxNodes);
    strikes_ = count("STRK", 1, 1, 16);
    strikeGap_ = num("SGAP", 4, 0.25, 64);
    strikeAlign_ = num("SALN", 1, 0.25, 64);
    strike_ = strikes_ > 1 ? -1 : 0;
    swayBeats_ = num("SWYB", 0, 0, 64);
    vibRate_ = num("VIBF", 3, 0, 32);
    pulses_ = inst_->defines("PULS");
    pulseBeats_ = num("PLSB", 1, 0.125, 64);
    pulseEvery_ = num("PLSE", 1, 0.125, 64);
    pulseRng_ = expr::Pcg32(env.seed ^ 0x9e15eULL);
    leadStrength_ = num("PLSL", 0, 0, 4);
    phraseStrength_ = num("PLSP", 0, 0, 4);
    rewire_ = num("RWIR", 0, 0, 0.9);
    rewireBars_ = num("RWRB", 4, 0.25, 64);
    rewireRng_ = expr::Pcg32(env.seed ^ 0x2e1e5ULL);
    leaves_ = inst_->defines("LEAF");
    leafW_ = num("LEAFW", 0.05, 0.0, 1.0);
    leafD_ = num("LEAFD", 2, 1, 64);
    leafA_ = num("LEAFA", 0.9, -3.2, 3.2);
    leafU_ = num("LEAFU", 4, 0.01, 256);
    buildTree(0);
    {
        // The lead roots' order: every root once, shuffled by the seed.
        const int roots = int(tree_->params().roots.size());
        for (int r = 0; r < roots; ++r) leadOrder_.push_back(r);
        for (int r = roots - 1; r > 0; --r)
            std::swap(leadOrder_[std::size_t(r)], leadOrder_[std::size_t(rewireRng_.uniform01Exclusive() * (r + 1))]);
    }

    width_ = std::clamp(finiteOr(inst_->loaded("WDTH", 8.0), 8.0), 0.0, 200.0);
    tip_ = std::clamp(finiteOr(inst_->loaded("TIP", 1.0), 1.0), 0.0, 200.0);
    budWidth_ = std::clamp(finiteOr(inst_->loaded("BUDW", 4.0), 4.0), 0.0, 200.0);
    wither_ = finiteOr(inst_->loaded("WTHR", 1.0), 1.0) != 0.0;
}

void OrganicRun::buildTree(int strike) {
    // Strike k evaluates the per-point and per-root keys again (rnd runs on, so every strike differs) and grows
    // from its own seed.
    auto count = [&](const char* key, double fb, int lo, int hi) {
        const double v = finiteOr(inst_->loaded(key, fb), fb);
        return std::clamp(int(std::lround(std::clamp(v, double(lo), double(hi)))), lo, hi);
    };
    const std::uint64_t salt = std::uint64_t(strike) * 0x9e3779b97f4a7c15ULL;
    if (strike > 0) inst_->reloadA();
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
        // Default crown: an ellipse .9 x .55 centered at (0, .3), evenly filled.
        expr::Pcg32 rng(seed_ ^ 0x7ee5ULL ^ salt);
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
    p.maxNodes = maxNodes_;
    tree_ = std::make_unique<TreeGrowth>(std::move(p), seed_ ^ salt);
    acc_ = 0;
    contactNodes_ = 0;
    contacts_.clear();
    leafBorn_.clear();
}

void OrganicRun::updateContacts() {
    // Roots whose branches meet: nodes of both within about two segments (cells of 2.5 x Seg, a 3 x 3 search).
    const auto& nodes = tree_->nodes();
    if (contactNodes_ == nodes.size() && contactRevision_ == tree_->revision() && !contacts_.empty()) return;
    contactNodes_ = nodes.size();
    contactRevision_ = tree_->revision();
    const std::size_t roots = tree_->params().roots.size();
    contacts_.assign(roots, 0);
    if (roots < 2 || roots > 64) return;
    const double cell = 2.5 * tree_->params().segment;
    auto key = [](long long cx, long long cy) { return (cx << 32) ^ (cy & 0xffffffffLL); };
    std::unordered_map<long long, std::uint64_t> cells;
    cells.reserve(nodes.size());
    for (const auto& nd : nodes) {
        if (nd.root < 0 || std::size_t(nd.root) >= roots) continue;
        cells[key((long long)std::floor(nd.x / cell), (long long)std::floor(nd.y / cell))] |= 1ULL << nd.root;
    }
    for (const auto& [k, m] : cells) {
        const long long cx = k >> 32, cy = (long long)(std::int32_t)(k & 0xffffffffLL);
        std::uint64_t near = 0;
        for (long long dy = -1; dy <= 1; ++dy)
            for (long long dx = -1; dx <= 1; ++dx) {
                const auto it = cells.find(key(cx + dx, cy + dy));
                if (it != cells.end()) near |= it->second;
            }
        for (std::size_t r = 0; r < roots; ++r)
            if (m & (1ULL << r)) contacts_[r] |= near & ~(1ULL << r);
    }
}

void OrganicRun::fire(double clock, bool all, double strength) {
    const int roots = int(tree_->params().roots.size());
    if (live_.size() > 256) return;  // bounded
    if (all) {
        for (int r = 0; r < roots; ++r) live_.push_back({r, clock, strength});
        return;
    }
    // A cascade: the next neuron is one whose dendrites meet the last one's (not the one before, if it can help).
    int next = 0;
    if (roots > 1) {
        updateContacts();
        std::uint64_t cand = lastFired_ >= 0 ? contacts_[std::size_t(lastFired_)] : 0;
        if (beforeFired_ >= 0 && (cand & ~(1ULL << beforeFired_))) cand &= ~(1ULL << beforeFired_);
        if (!cand) cand = (roots >= 64 ? ~0ULL : (1ULL << roots) - 1) & ~(lastFired_ >= 0 ? 1ULL << lastFired_ : 0ULL);
        int n = 0;
        for (int r = 0; r < roots; ++r) n += (cand >> r) & 1ULL ? 1 : 0;
        int pick = int(pulseRng_.uniform01Exclusive() * n);
        for (int r = 0; r < roots; ++r)
            if ((cand >> r) & 1ULL) {
                if (pick-- == 0) {
                    next = r;
                    break;
                }
            }
    }
    beforeFired_ = lastFired_;
    lastFired_ = next;
    live_.push_back({next, clock, strength});
}

void OrganicRun::startRewire(double clock) {
    // Cuts: side branches (a node whose parent branches) of 3 nodes up to 12% of the tree, in a seeded order, until
    // the share Rwir of the non-root nodes is marked; a cut's whole sub-branch withers with it.
    const auto& nodes = tree_->nodes();
    const std::size_t n = nodes.size(), roots = tree_->params().roots.size();
    if (n <= roots + 10) return;
    std::vector<int> size(n, 1);
    for (std::size_t i = n; i-- > 0;)
        if (nodes[i].parent >= 0) size[std::size_t(nodes[i].parent)] += size[i];
    const double total = double(n - roots);
    std::vector<int> cand;
    for (std::size_t i = 0; i < n; ++i) {
        const TreeNode& nd = nodes[i];
        if (nd.parent < 0 || nodes[std::size_t(nd.parent)].children < 2) continue;
        if (size[i] >= 3 && double(size[i]) <= 0.12 * total) cand.push_back(int(i));
    }
    for (std::size_t k = cand.size(); k > 1; --k)
        std::swap(cand[k - 1], cand[std::size_t(rewireRng_.uniform01Exclusive() * double(k))]);
    std::vector<char> cut(n, 0);
    double marked = 0;
    for (int c : cand) {
        if (marked >= rewire_ * total) break;
        bool inside = false;  // under (or over) a cut already chosen
        for (int a = c; a >= 0 && !inside; a = nodes[std::size_t(a)].parent) inside = cut[std::size_t(a)] != 0;
        if (inside) continue;
        cut[std::size_t(c)] = 1;
        marked += size[std::size_t(c)];
    }
    dieRel_.assign(n, -1);
    std::vector<int> cutOf(n, -1);
    for (std::size_t i = 0; i < n; ++i) {
        const int pa = nodes[i].parent;
        if (cut[i]) {
            dieRel_[i] = 0;
            cutOf[i] = int(i);
        } else if (pa >= 0 && dieRel_[std::size_t(pa)] >= 0) {
            // A cut chosen below an earlier one cannot happen (inside), so this is the cut's own sub-branch.
            dieRel_[i] = dieRel_[std::size_t(pa)] + 1;
            cutOf[i] = cutOf[std::size_t(pa)];
        }
    }
    std::vector<int> deepest(n, 0);
    for (std::size_t i = 0; i < n; ++i)
        if (cutOf[i] >= 0) deepest[std::size_t(cutOf[i])] = std::max(deepest[std::size_t(cutOf[i])], dieRel_[i]);
    dieMax_.assign(n, 0);
    for (std::size_t i = 0; i < n; ++i)
        if (cutOf[i] >= 0) dieMax_[i] = deepest[std::size_t(cutOf[i])];
    dying_ = marked > 0;
    dieStart_ = clock;
}

void OrganicRun::finishRewire() {
    std::vector<char> remove(dieRel_.size(), 0);
    for (std::size_t i = 0; i < dieRel_.size(); ++i) remove[i] = dieRel_[i] == 0 ? 1 : 0;
    const std::vector<int> map = tree_->prune(remove);
    if (!leafBorn_.empty()) {
        std::vector<double> born(tree_->nodes().size(), std::nan(""));
        for (std::size_t i = 0; i < map.size() && i < leafBorn_.size(); ++i)
            if (map[i] >= 0) born[std::size_t(map[i])] = leafBorn_[i];
        leafBorn_ = std::move(born);
    }
    dieRel_.clear();
    dieMax_.clear();
    dying_ = false;
    // New attraction points: the cloud's keys again (rnd runs on), Pts x Rwir of them.
    const int pts = int(std::lround(std::clamp(finiteOr(inst_->loaded("PTS", 300), 300), 10.0, 4000.0)));
    const int count = std::max(10, int(std::lround(double(pts) * rewire_)));
    std::vector<double> xs, ys;
    if (inst_->defines("PX") || inst_->defines("PY")) {
        using PK = scene::OrganicInstance::PointKey;
        const PK keys[] = {{"PX", 0.0}, {"PY", 0.0}};
        std::vector<std::vector<double>> v;
        inst_->evaluatePoints(count, keys, v);
        xs = std::move(v[0]);
        ys = std::move(v[1]);
    } else {
        for (int i = 0; i < count; ++i) {
            const double a = rewireRng_.uniform01Exclusive() * 2 * kPi, r = std::sqrt(rewireRng_.uniform01());
            xs.push_back(0.9 * r * std::cos(a));
            ys.push_back(0.3 + 0.55 * r * std::sin(a));
        }
    }
    tree_->addPoints(xs, ys);
}

std::string OrganicRun::summary() const {
    char b[96];
    if (body_) return body_->summary();
    if (kind_ == OrganicKind::Field) {
        if (field_.kind == OrganicField::Kind::Julia) std::snprintf(b, sizeof b, "%d iterations", field_.iter);
        else if (field_.kind == OrganicField::Kind::Mandel && mandel_.params().dbl > 0)
            std::snprintf(b, sizeof b, "dives of %g bars to 2^%g, a doubling per %g beat(s)", mandel_.params().diveBars,
                          mandel_.params().deep, mandel_.params().dbl);
        else
            std::snprintf(b, sizeof b, "dives of %g bars to 2^%g, paced to the dive", mandel_.params().diveBars,
                          mandel_.params().deep);
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
    o.pen = std::clamp(finiteOr(f.pen, 0.0), 0.0, 1.0);
    o.opacity = std::clamp(fader, 0.0, 1.0);
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
    if (o.kind == OrganicField::Kind::Mandel) {
        // The clock: the beat clock's beats and (phrase-aligned) bars while it is on, else 120 BPM from the start.
        const bool clock = audio.sync.on > 0;
        const double beat = finiteOr(clock ? audio.sync.beat : 2.0 * (t - start_), 0.0);
        const double bar = finiteOr(clock ? audio.sync.bar : beat / 4.0, 0.0);
        const double tgt = finiteOr(inst_->frameKey("TGT", -1.0), -1.0);
        const int fixed = tgt < 0 ? -1 : int(std::lround(std::min(tgt, 1000.0)));
        const MandelView v = mandel_.step(beat, bar, clock ? audio.sync.drop : 0.0, dtSim, fixed);
        o.cx = v.cx;
        o.cy = v.cy;
        o.half = v.half;
        o.depth = v.depth;
        o.dive = v.dive;
        o.target = v.target;
        o.iter = v.iter;
        o.precision = mandelPrec_ > 0 ? mandelPrec_ - 1 : (v.depth > 12.0 ? 2 : 0);
        o.x = std::clamp(finiteOr(f.x, 0.0), -1e4, 1e4);
        o.y = std::clamp(finiteOr(f.y, 0.0), -1e4, 1e4);
        o.scale = std::clamp(finiteOr(f.scale, 1.0), -1e4, 1e4);
        o.rot = finiteOr(f.rot, 0.0) + v.turn;
        {
            const MandelTarget& mt = mandelTarget(v.target);  // the nucleus / size of a mini
            o.mini = v.mini;
            o.nre = mt.re;
            o.nim = mt.im;
            o.sre = mt.sre;
            o.sim = mt.sim;
        }
        o.gain = std::clamp(finiteOr(inst_->frameKey("GAIN", 1.0), 1.0), 0.0, 100.0);
        o.bands = std::clamp(finiteOr(inst_->frameKey("BAND", 0.0), 0.0), 0.0, 1e6);
        o.glow = std::clamp(finiteOr(inst_->frameKey("GLOW", 1.0), 1.0), 0.0, 100.0);
        // Bands flow inward one per beat unless Phas says otherwise.
        o.phase = std::isfinite(f.phas) ? f.phas : -2.0 * kPi * (beat - std::floor(beat / 64.0) * 64.0);
        fieldOn_ = o.pen > 0 && o.opacity > 0;
        return;
    }
}

void OrganicRun::step(double t, const SpriteAudio& audio, const FrameMapping& map, const LineStyle& style,
                      std::vector<LineSegment>& out, const OrganicColors* colors) {
    const double dtSim = lastT_ < 0 ? 0.0 : std::clamp(t - lastT_, 0.0, 0.25);
    lastT_ = t;
    const double age = t - start_;
    barHold_.follow(env_, age, audio.sync);
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
    // Trees: the beat clock (120 BPM from the start without it), strikes on its lines, `s` = beats since the current
    // growth began.
    const double clock = audio.sync.on > 0 && std::isfinite(audio.sync.beat) ? audio.sync.beat : 2.0 * age;
    // Strike lines every SAln beats on the phrase-aligned bar clock (4 beats a bar): with SAln 4 the bar lines.
    const double barBeats =
        4.0 * (audio.sync.on > 0 && std::isfinite(audio.sync.bar) ? audio.sync.bar : clock / 4.0);
    const auto lineAfter = [&](bool orNow) {
        const double k = barBeats / strikeAlign_, f = k - std::floor(k);
        if (orNow && f < 0.02 / strikeAlign_) return clock;
        return clock + (orNow ? std::ceil(k) - k : std::floor(k) - k) * strikeAlign_;
    };
    double sGrow = std::nan("");
    if (kind_ == OrganicKind::Tree) {
        if (!clockStarted_) {
            clockStarted_ = true;
            growBeat_ = clock;
            // The first strike on the next strike line (at once when the clock is on one).
            nextStrike_ = lineAfter(true);
        }
        if (strikes_ > 1) {
            // The clock jumped (a new lock, Link joined): re-base on the strike line just passed.
            if (clock < nextStrike_ - strikeGap_ - strikeAlign_ || clock > nextStrike_ + strikeAlign_) {
                nextStrike_ = lineAfter(false);
                growBeat_ = std::min(growBeat_, clock);
            }
            if (strike_ < strikes_ - 1 && clock >= nextStrike_) {
                ++strike_;
                if (strike_ > 0) buildTree(strike_);
                growBeat_ = nextStrike_;
                nextStrike_ += strikeGap_;
            }
        } else if (clock < growBeat_) {
            growBeat_ = clock;
        }
        sGrow = strike_ < 0 ? 0.0 : std::max(0.0, clock - growBeat_);
    }
    const scene::OrganicFrame f = inst_->evaluateFrame(in, sGrow);
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
        st.colors = colors;
        st.ending = env_.fadingOut(age);
        OrganicCanvas canvas(map, style, out, f.x, f.y, f.scale, f.rot);
        if (st.pen > 0) body_->step(st, canvas);
        return;
    }

    if (strike_ < 0) return;  // waiting for the first strike's beat line

    // Bar and phrase lines crossed this frame (the phrase-aligned bar clock; 16 bars a phrase without the clock).
    bool barCrossed = false, phraseCrossed = false;
    double lineAt = clock;  // clock beat of the bar line just crossed
    if (leadStrength_ > 0 || phraseStrength_ > 0 || rewire_ > 0) {
        const double bars = barBeats / 4.0;
        const double phrases =
            audio.sync.on > 0 && std::isfinite(audio.sync.phrase) ? audio.sync.phrase : clock / 64.0;
        const long long bl = (long long)std::floor(bars + 1e-9), pl = (long long)std::floor(phrases + 1e-9);
        if (fireStarted_ || rewireStarted_) {
            barCrossed = bl > barLine_ && bl - barLine_ <= 4;
            phraseCrossed = pl > phraseLine_ && pl - phraseLine_ <= 2;
        }
        rewireStarted_ = true;
        lineAt = clock - std::max(0.0, barBeats - 4.0 * double(bl));
        barLine_ = bl;
        phraseLine_ = pl;
    }
    // Rewiring: the cut branches wither back over RwrB bars, then go, and new points let new branches grow.
    const bool ending = (env_.earlyOut >= 0 && age >= env_.earlyOut) || age >= env_.fade + env_.hold;
    double dieP = 0;
    if (dying_) {
        dieP = std::clamp((clock - dieStart_) / (4.0 * rewireBars_), 0.0, 1.0);
        if (clock < dieStart_ - 1.0) dieStart_ = clock;  // the clock jumped back
        if (dieP >= 1.0) {
            finishRewire();
            dieP = 0;
        }
    }
    if (phraseCrossed && rewire_ > 0 && !dying_ && tree_->done() && !ending) startRewire(lineAt);

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
    // Sway: each node turns about its parent, more the thinner it is; the phase runs along the branches. With SwyB
    // the whole tree sways back and forth on the beat clock, SwyB beats per cycle, about its fixed root: every node
    // bends a little (the trunk a little), the thin ones most, the tips lagging slightly.
    const double sway = std::clamp(finiteOr(f.sway, 0.0), -3.0, 3.0);
    const double perNode = sway * 3.0 / std::max(G, 10.0);
    const double wt = 2 * kPi * kSwayHz * (t - start_);
    // On the phrase-aligned bar clock, so with SwyB 8 the extremes land on the bar lines.
    const double beatWave = swayBeats_ > 0 ? 2 * kPi * barBeats / swayBeats_ : 0.0;
    const double maxTips = std::max(1.0f, tree_->maxTips());
    const double gNorm = 1.0 / std::max(G, 1.0);
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
        double a;
        if (swayBeats_ > 0)
            a = ang_[pa] + perNode * (0.15 + 0.85 * thin * thin) * std::cos(beatWave - 0.5 * double(nd.gen) * gNorm);
        else
            a = ang_[pa] + perNode * thin * thin * std::sin(wt + nd.phase);
        const double ex = nd.x - nodes[pa].x, ey = nd.y - nodes[pa].y;
        const double c = std::cos(a), s = std::sin(a);
        ang_[i] = float(a);
        px_[i] = float(px_[pa] + c * ex - s * ey);
        py_[i] = float(py_[pa] + s * ex + c * ey);
    }
    // Vibration (Vib): every node moves out from its root and back, Vib x its distance, ringing VibF times per beat
    // at the tips and slower towards the root (from .6 x to 1.4 x VibF by depth), all outwards on the beat line.
    const double vib = std::clamp(finiteOr(inst_->frameKey("VIB", 0.0), 0.0), -2.0, 2.0);
    if (vib != 0.0) {
        const double frac = clock - std::floor(clock);
        for (std::size_t i = 0; i < n; ++i) {
            const TreeNode& nd = nodes[i];
            if (nd.parent < 0) continue;
            const double depth = std::min(1.0, double(nd.gen) * gNorm);
            const double k = vib * std::cos(2 * kPi * vibRate_ * (0.6 + 0.8 * depth) * frac);
            const std::size_t r = std::size_t(nd.root);
            px_[i] = float(px_[r] + (1.0 + k) * (px_[i] - px_[r]));
            py_[i] = float(py_[r] + (1.0 + k) * (py_[i] - py_[r]));
        }
    }

    // Pulses (Puls): a root fires on every line of PlsE beats (a cascade through the roots whose branches meet), every
    // root when PAll rises past .5; the pulse runs out to the tips in PlsB beats and the tips flash as it arrives.
    double puls = 0;
    if (pulses_) {
        puls = std::clamp(finiteOr(inst_->frameKey("PULS", 0.0), 0.0), 0.0, 4.0);
        const double pall = finiteOr(inst_->frameKey("PALL", 0.0), 0.0);
        const long long line = (long long)std::floor(clock / pulseEvery_ + 1e-9);
        // Phrase lines: every root together (PlsP); bar lines: a lead root restarts the cascade, stronger (PlsL);
        // a beat fire within half a beat of either is left out.
        if (phraseCrossed && phraseStrength_ > 0) {
            fire(lineAt, true, phraseStrength_);
            structBeat_ = lineAt;
        } else if (barCrossed && leadStrength_ > 0 && !leadOrder_.empty()) {
            const int lead = leadOrder_[std::size_t(((barLine_ % (long long)leadOrder_.size()) +
                                                     (long long)leadOrder_.size()) % (long long)leadOrder_.size())];
            if (live_.size() <= 256) live_.push_back({lead, lineAt, leadStrength_});
            beforeFired_ = -1;
            lastFired_ = lead;
            structBeat_ = lineAt;
        }
        if (!fireStarted_) {
            fireStarted_ = true;
            fireLine_ = line;
            fire(double(line) * pulseEvery_, false);
        } else if (line < fireLine_ || line - fireLine_ > 4) {
            fireLine_ = line;  // the clock jumped
        } else {
            while (fireLine_ < line) {
                ++fireLine_;
                const double at = double(fireLine_) * pulseEvery_;
                if (std::abs(at - structBeat_) >= 0.5) fire(at, false);
            }
        }
        // A drop: every root at once, stronger than a phrase line.
        if (pall > 0.5 && prevAll_ <= 0.5) fire(clock, true, 1.6);
        prevAll_ = pall;
        const double keep = pulseBeats_ + 2.0;
        std::erase_if(live_, [&](const Pulse& p) { return clock - p.beat > keep || clock - p.beat < -1.0; });
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
    const double penBase = std::max(0.0, finiteOr(f.pen, 0.0) * penScale);
    const float pen = float(std::clamp(penBase, 0.0, 1.0));
    const float budPen = float(std::clamp(finiteOr(f.pen, 0.0) * finiteOr(f.bud, 0.0) * penScale, 0.0, 1.0));
    const double pulsPen = puls * penScale;
    const bool pulsing = pulsPen > 0 && !live_.empty();
    const double leafKey = leaves_ ? finiteOr(inst_->frameKey("LEAF", 0.0), 0.0) : 0.0;
    const float leafPen = float(std::clamp(finiteOr(f.pen, 0.0) * leafKey * penScale, 0.0, 1.0));
    if (pen <= 0.f && budPen <= 0.f && !pulsing && leafPen <= 0.f) return;
    const float pathId = float(out.size());
    auto radius = [&](double lwdt) {
        return float(0.5 * lineWidthPixels(lwdt * absScal, style, map.width, map.height));
    };
    // Per root: its deepest generation (the pulse's length); the cell body flashes as it fires.
    const std::size_t roots = tree_->params().roots.size();
    std::vector<int> rootGen;
    if (pulsing) {
        rootGen.assign(roots, 1);
        for (const auto& nd : nodes)
            if (nd.root >= 0 && std::size_t(nd.root) < roots)
                rootGen[std::size_t(nd.root)] = std::max(rootGen[std::size_t(nd.root)], nd.depth);
        for (const Pulse& p : live_) {
            const double tau = clock - p.beat;
            if (tau < 0 || std::size_t(p.root) >= n) continue;
            const float fp = float(std::clamp(pulsPen * std::min(p.strength, 1.0) * std::exp(-4.0 * tau), 0.0, 1.0));
            float cx, cy;
            if (fp > 0.01f && toPx(px_[std::size_t(p.root)], py_[std::size_t(p.root)], cx, cy))
                out.push_back({cx, cy, cx, cy, fp, fp, radius(2.0 * budWidth_ * std::max(1.0, p.strength)), pathId});
        }
    }
    for (std::size_t i = 0; i < n; ++i) {
        const TreeNode& nd = nodes[i];
        if (nd.parent < 0) continue;
        double frac = std::clamp(limit - double(nd.gen - 1), 0.0, 1.0);
        if (dying_ && i < dieRel_.size() && dieRel_[i] >= 0)  // withering back, its tips first
            frac = std::min(frac, std::clamp((dieMax_[i] + 1.0) * (1.0 - dieP) - double(dieRel_[i]), 0.0, 1.0));
        if (frac <= 0.0) continue;
        const std::size_t pa = std::size_t(nd.parent);
        const double bx = px_[pa] + frac * (px_[i] - px_[pa]), by = py_[pa] + frac * (py_[i] - py_[pa]);
        float ax, ay, cx, cy;
        if (!toPx(px_[pa], py_[pa], ax, ay) || !toPx(bx, by, cx, cy)) continue;
        const double w = tip_ + (width_ - tip_) * std::sqrt(std::min(1.0, double(nd.tips) / maxTips));
        float r = radius(w);
        float segPen = pen, tipPen = (nd.children == 0 && frac >= 1.0) ? budPen : 0.f;
        if (pulsing && std::size_t(nd.root) < roots) {
            // The front at depth rootDepth x tau / PlsB: a bright head with a tail a quarter of the way behind, as
            // strong as the pulse (bar-line leads and drops brighter and thicker).
            double bump = 0, flash = 0;
            const double gens = rootGen[std::size_t(nd.root)];
            const double band = std::max(3.0, 0.25 * gens);
            for (const Pulse& p : live_) {
                if (p.root != nd.root) continue;
                const double d = gens * (clock - p.beat) / pulseBeats_ - double(nd.depth);
                if (d >= 0 && d < band) bump = std::max(bump, p.strength * (1.0 - d / band));
                if (nd.children == 0 && d >= 0)
                    flash = std::max(flash, p.strength * std::exp(-5.0 * d / gens * pulseBeats_));
            }
            if (bump > 0) {
                segPen = float(std::clamp(penBase + pulsPen * bump, 0.0, 1.0));
                r *= float(1.0 + 0.8 * bump);
            }
            if (flash > 0.01 && frac >= 1.0) tipPen = std::max(tipPen, float(std::clamp(pulsPen * flash, 0.0, 1.0)));
        }
        if (segPen > 0.f) out.push_back({ax, ay, cx, cy, segPen, segPen, r, pathId});
        if (tipPen > 0.f) out.push_back({cx, cy, cx, cy, tipPen, tipPen, std::max(r, radius(budWidth_)), pathId});
    }

    // Leaves: a path of their own over the branches. Each is a pointed oval of two hard-edged capsules (a body and a
    // narrower tip; an anti-aliased rim on a fat stroke takes unrelated ColorMap colors), on its twig's displayed
    // position and direction (so it rustles with the sway), turned LeafA off it (tips: almost straight on),
    // fluttering by LFlt. It unfurls over LeafU beats once its node is on an outer twig (opening from along it).
    // While the tree withers they come loose, staggered, and fall turning and shrinking.
    if (leafPen <= 0.f || leafW_ <= 0.0) return;
    const double flutter = std::clamp(finiteOr(inst_->frameKey("LFLT", 0.0), 0.0), -3.0, 3.0);
    const double ppu = 0.5 * map.height / (map.yExtent > 0 ? map.yExtent : 1.0) * absScal;  // pixels per tree unit
    const float leafPath = float(out.size());
    const double grown = tree_->done() ? G : std::max(0.0, G - 1.0);  // generations fully drawn before withering
    const double fall = (wither_ && fadingOut) ? std::clamp(1.0 - in.fader, 0.0, 1.0) : 0.0;
    leafBorn_.resize(n, std::nan(""));
    int drawn = 0;
    for (std::size_t i = 0; i < n && drawn < kMaxLeaves; ++i) {
        const TreeNode& nd = nodes[i];
        if (nd.parent < 0) continue;
        const std::uint64_t h = (std::uint64_t(i) + 1) * 0x9e3779b97f4a7c15ULL ^ (seed_ * 0xbf58476d1ce4e5b9ULL);
        const std::uint64_t hh = (h ^ (h >> 29)) * 0x94d049bb133111ebULL;
        const bool tip = nd.children == 0;
        if (!std::isfinite(leafBorn_[i])) {
            // A leaf starts once its node is grown and on an outer twig; then it stays (the twig may branch on).
            if (fall > 0 || double(nd.gen) > grown || nd.tips > 4.0f || maxTips < 8.0 || nd.tips > 0.25 * maxTips)
                continue;
            if (!tip && std::size_t(hh >> 40) % std::size_t(std::lround(leafD_)) != 0) continue;
            leafBorn_[i] = clock;
        }
        const double rnd = double((hh >> 8) & 0xffff) / 65535.0;  // per leaf, 0..1
        const double born = std::min(leafBorn_[i], clock);
        double u = std::clamp((clock - born) / leafU_, 0.0, 1.0);
        u = u * u * (3.0 - 2.0 * u);
        double fallP = 0.0;
        if (fall > 0) {
            fallP = std::clamp((fall - 0.5 * rnd) / 0.5, 0.0, 1.0);
            if (fallP >= 1.0) continue;
        }
        const double size = leafW_ * (0.8 + 0.4 * rnd) * u * (1.0 - fallP);
        if (size * ppu < 0.75) continue;
        const std::size_t pa = std::size_t(nd.parent);
        double dx = px_[i] - px_[pa], dy = py_[i] - py_[pa];
        const double len = std::hypot(dx, dy);
        if (!(len > 1e-12)) continue;
        dx /= len;
        dy /= len;
        const double side = ((hh >> 3) & 1) ? 1.0 : -1.0;
        const double open = tip ? 0.25 * side : leafA_ * side;
        double a = open * (0.3 + 0.7 * u) + flutter * std::sin(2 * kPi * 3.0 * clock + 6.0 * nd.phase + 7.0 * rnd);
        double bx0 = px_[i], by0 = py_[i];
        if (fallP > 0) {
            a += side * 4.0 * fallP;
            bx0 += 0.15 * fallP * std::sin(3.0 * kPi * fallP + 6.0 * rnd);
            by0 -= 0.9 * fallP * fallP;
        }
        const double ca = std::cos(a), sa = std::sin(a);
        const double ux = ca * dx - sa * dy, uy = sa * dx + ca * dy;
        auto at = [&](double k, float& ox, float& oy) { return toPx(bx0 + k * size * ux, by0 + k * size * uy, ox, oy); };
        float x0, y0, x1, y1, x2, y2;
        if (!at(0.24, x0, y0) || !at(0.56, x1, y1) || !at(0.9, x2, y2)) continue;
        const float rb = float(std::max(0.5, 0.2 * size * ppu)), rt = float(std::max(0.5, 0.075 * size * ppu));
        out.push_back({x0, y0, x1, y1, leafPen, leafPen, -rb, leafPath});
        out.push_back({x1, y1, x2, y2, leafPen, leafPen, -rt, leafPath});
        ++drawn;
    }
}

}  // namespace reforce::engine
