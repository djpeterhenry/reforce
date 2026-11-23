#include "engine/TreeGrowth.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace reforce::engine {

namespace {
constexpr double kTwoPi = 6.283185307179586;
}

int TreeGrowth::Grid::index(double x, double y) const {
    const int c = std::clamp(int(std::floor((x - x0) / cell)), 0, cols - 1);
    const int r = std::clamp(int(std::floor((y - y0) / cell)), 0, rows - 1);
    return r * cols + c;
}

TreeGrowth::TreeGrowth(TreeParams params, std::uint64_t seed) : p_(std::move(params)), rng_(seed) {
    auto finiteOr = [](double v, double fb) { return std::isfinite(v) ? v : fb; };
    p_.segment = std::clamp(finiteOr(p_.segment, 0.02), 1e-4, 1.0);
    p_.influence = std::clamp(finiteOr(p_.influence, 0.3), p_.segment, 10.0);
    p_.kill = std::clamp(finiteOr(p_.kill, 2 * p_.segment), 0.5 * p_.segment, p_.influence);
    p_.jitter = std::clamp(finiteOr(p_.jitter, 0.15), 0.0, 4.0);
    p_.maxNodes = std::clamp(p_.maxNodes, 2, 200000);
    if (p_.roots.empty()) p_.roots.push_back({});

    // Keep the finite points; bounds of points and roots, grown by the influence radius.
    const std::size_t n = std::min(p_.pointsX.size(), p_.pointsY.size());
    std::vector<double> px, py;
    px.reserve(n);
    py.reserve(n);
    double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
    auto grow = [&](double x, double y) {
        x0 = std::min(x0, x); y0 = std::min(y0, y);
        x1 = std::max(x1, x); y1 = std::max(y1, y);
    };
    for (std::size_t i = 0; i < n; ++i) {
        const double x = p_.pointsX[i], y = p_.pointsY[i];
        if (!std::isfinite(x) || !std::isfinite(y) || std::abs(x) > 1e3 || std::abs(y) > 1e3) continue;
        px.push_back(x);
        py.push_back(y);
        grow(x, y);
    }
    for (auto& r : p_.roots) {
        r.x = finiteOr(r.x, 0.0);
        r.y = finiteOr(r.y, -1.0);
        r.angle = finiteOr(r.angle, kTwoPi / 4);
        grow(r.x, r.y);
    }
    p_.pointsX = std::move(px);
    p_.pointsY = std::move(py);
    x0 -= p_.influence; y0 -= p_.influence;
    x1 += p_.influence; y1 += p_.influence;
    // Cells of the influence radius (a 3 x 3 search finds every node in reach), at most 256 x 256 of them.
    double cell = p_.influence;
    cell = std::max({cell, (x1 - x0) / 256.0, (y1 - y0) / 256.0});
    for (Grid* g : {&nodeGrid_, &pointGrid_}) {
        g->x0 = x0;
        g->y0 = y0;
        g->cell = cell;
        g->cols = std::max(1, int(std::ceil((x1 - x0) / cell)));
        g->rows = std::max(1, int(std::ceil((y1 - y0) / cell)));
        g->cells.assign(std::size_t(g->cols * g->rows), {});
    }
    alive_.assign(p_.pointsX.size(), 1);
    livePoints_ = int(p_.pointsX.size());
    for (std::size_t i = 0; i < p_.pointsX.size(); ++i) pointGrid_.insert(int(i), p_.pointsX[i], p_.pointsY[i]);

    nodes_.reserve(std::size_t(std::min(p_.maxNodes, 8192)));
    for (const auto& r : p_.roots) {
        const int id = addNode(-1, r.x, r.y, std::cos(r.angle), std::sin(r.angle));
        nodes_[std::size_t(id)].phase = float(rand01() * kTwoPi);
        tips_.push_back(id);
    }
    trunk_.assign(nodes_.size(), 1);
    // The roots may already sit among the points.
    for (const auto& nd : nodes_) killNear(nd.x, nd.y);
    updateTips();
    if (p_.pointsX.empty()) done_ = true;
}

int TreeGrowth::addNode(int parent, double x, double y, double dx, double dy) {
    TreeNode nd;
    nd.x = float(x);
    nd.y = float(y);
    nd.dx = float(dx);
    nd.dy = float(dy);
    nd.parent = parent;
    nd.root = parent < 0 ? int(nodes_.size()) : nodes_[std::size_t(parent)].root;
    nd.gen = parent < 0 ? 0 : gen_ + 1;
    nd.depth = parent < 0 ? 0 : nodes_[std::size_t(parent)].depth + 1;
    if (parent >= 0) {
        TreeNode& pa = nodes_[std::size_t(parent)];
        // Sway phase runs on along a branch; a side branch starts a little out of step.
        nd.phase = pa.phase + 0.12f + (pa.children > 0 ? float((rand01() - 0.5) * 1.2) : 0.0f);
        ++pa.children;
    }
    nodes_.push_back(nd);
    const int id = int(nodes_.size()) - 1;
    nodeGrid_.insert(id, x, y);
    return id;
}

void TreeGrowth::killNear(double x, double y) {
    const double k2 = p_.kill * p_.kill;
    const int c = std::clamp(int(std::floor((x - pointGrid_.x0) / pointGrid_.cell)), 0, pointGrid_.cols - 1);
    const int r = std::clamp(int(std::floor((y - pointGrid_.y0) / pointGrid_.cell)), 0, pointGrid_.rows - 1);
    for (int rr = std::max(0, r - 1); rr <= std::min(pointGrid_.rows - 1, r + 1); ++rr)
        for (int cc = std::max(0, c - 1); cc <= std::min(pointGrid_.cols - 1, c + 1); ++cc)
            for (int i : pointGrid_.cells[std::size_t(rr * pointGrid_.cols + cc)]) {
                if (!alive_[std::size_t(i)]) continue;
                const double ex = p_.pointsX[std::size_t(i)] - x, ey = p_.pointsY[std::size_t(i)] - y;
                if (ex * ex + ey * ey <= k2) {
                    alive_[std::size_t(i)] = 0;
                    --livePoints_;
                }
            }
}

void TreeGrowth::updateTips() {
    for (auto& nd : nodes_) nd.tips = 0;
    maxTips_ = 1;
    for (std::size_t i = nodes_.size(); i-- > 0;) {
        TreeNode& nd = nodes_[i];
        if (nd.children == 0) nd.tips = 1;
        if (nd.parent >= 0)
            nodes_[std::size_t(nd.parent)].tips += nd.tips;
        else
            maxTips_ = std::max(maxTips_, nd.tips);
    }
}

int TreeGrowth::iterate(double tx, double ty) {
    if (done_) return 0;
    if (!std::isfinite(tx)) tx = 0;
    if (!std::isfinite(ty)) ty = 0;
    const std::size_t count = nodes_.size();
    pullX_.assign(count, 0.0);
    pullY_.assign(count, 0.0);
    pullN_.assign(count, 0);
    pulled_.clear();

    // Each live point pulls its nearest node in reach.
    const double i2 = p_.influence * p_.influence;
    for (std::size_t i = 0; i < p_.pointsX.size(); ++i) {
        if (!alive_[i]) continue;
        const double x = p_.pointsX[i], y = p_.pointsY[i];
        const int c = std::clamp(int(std::floor((x - nodeGrid_.x0) / nodeGrid_.cell)), 0, nodeGrid_.cols - 1);
        const int r = std::clamp(int(std::floor((y - nodeGrid_.y0) / nodeGrid_.cell)), 0, nodeGrid_.rows - 1);
        int best = -1;
        double bestD = i2;
        for (int rr = std::max(0, r - 1); rr <= std::min(nodeGrid_.rows - 1, r + 1); ++rr)
            for (int cc = std::max(0, c - 1); cc <= std::min(nodeGrid_.cols - 1, c + 1); ++cc)
                for (int id : nodeGrid_.cells[std::size_t(rr * nodeGrid_.cols + cc)]) {
                    const TreeNode& nd = nodes_[std::size_t(id)];
                    const double ex = x - nd.x, ey = y - nd.y;
                    const double d = ex * ex + ey * ey;
                    if (d < bestD || (d == bestD && best >= 0 && id < best)) {
                        bestD = d;
                        best = id;
                    }
                }
        if (best < 0) continue;
        const TreeNode& nd = nodes_[std::size_t(best)];
        const double len = std::sqrt(bestD);
        if (len > 1e-9) {
            pullX_[std::size_t(best)] += (x - nd.x) / len;
            pullY_[std::size_t(best)] += (y - nd.y) / len;
        }
        if (pullN_[std::size_t(best)]++ == 0) pulled_.push_back(best);
    }
    std::sort(pulled_.begin(), pulled_.end());

    std::vector<int> grown;
    grown.reserve(pulled_.size() + tips_.size());
    auto growFrom = [&](int from, double dx, double dy, bool trunk) {
        if (int(nodes_.size()) >= p_.maxNodes) return;
        const TreeNode& nd = nodes_[std::size_t(from)];
        if (trunk) {
            // Keep near the root's direction: half the jitter, pulled back each step, no tropism.
            const TreeNode& root = nodes_[std::size_t(nd.root)];
            dx += 0.3 * root.dx + 0.5 * p_.jitter * (rand01() * 2 - 1);
            dy += 0.3 * root.dy + 0.5 * p_.jitter * (rand01() * 2 - 1);
        } else {
            dx += tx + p_.jitter * (rand01() * 2 - 1);
            dy += ty + p_.jitter * (rand01() * 2 - 1);
        }
        double len = std::hypot(dx, dy);
        if (!(len > 1e-9)) {
            dx = nd.dx;
            dy = nd.dy;
            len = 1;
        }
        dx /= len;
        dy /= len;
        const double x = nd.x + p_.segment * dx, y = nd.y + p_.segment * dy;
        // A node pulled from both sides keeps proposing the same spot: never grow onto an existing node.
        const double near2 = 0.25 * p_.segment * p_.segment;
        const int c = std::clamp(int(std::floor((x - nodeGrid_.x0) / nodeGrid_.cell)), 0, nodeGrid_.cols - 1);
        const int r = std::clamp(int(std::floor((y - nodeGrid_.y0) / nodeGrid_.cell)), 0, nodeGrid_.rows - 1);
        for (int rr = std::max(0, r - 1); rr <= std::min(nodeGrid_.rows - 1, r + 1); ++rr)
            for (int cc = std::max(0, c - 1); cc <= std::min(nodeGrid_.cols - 1, c + 1); ++cc)
                for (int id : nodeGrid_.cells[std::size_t(rr * nodeGrid_.cols + cc)]) {
                    const double ex = x - nodes_[std::size_t(id)].x, ey = y - nodes_[std::size_t(id)].y;
                    if (ex * ex + ey * ey < near2) return;
                }
        const int id = addNode(from, x, y, dx, dy);
        trunk_.push_back(trunk ? 1 : 0);
        grown.push_back(id);
    };

    for (int n : pulled_) {
        const double px = pullX_[std::size_t(n)], py = pullY_[std::size_t(n)];
        const double len = std::hypot(px, py);
        if (len > 1e-9)
            growFrom(n, px / len, py / len, false);
        else
            growFrom(n, nodes_[std::size_t(n)].dx, nodes_[std::size_t(n)].dy, false);
    }
    // Trunks: tips that no point has reached yet grow straight on.
    for (int n : tips_) {
        if (!trunk_[std::size_t(n)] || pullN_.size() <= std::size_t(n) || pullN_[std::size_t(n)] > 0) continue;
        if (nodes_[std::size_t(n)].gen >= p_.maxTrunk) continue;
        growFrom(n, nodes_[std::size_t(n)].dx, nodes_[std::size_t(n)].dy, true);
    }

    for (int id : grown) killNear(nodes_[std::size_t(id)].x, nodes_[std::size_t(id)].y);
    if (grown.empty()) {
        done_ = true;
        return 0;
    }
    ++gen_;
    ++revision_;
    tips_ = std::move(grown);
    updateTips();
    if (livePoints_ == 0 || int(nodes_.size()) >= p_.maxNodes) done_ = true;
    return int(tips_.size());
}

std::vector<int> TreeGrowth::prune(const std::vector<char>& remove) {
    const std::size_t n = nodes_.size();
    std::vector<int> map(n, -1);
    std::vector<char> gone(n, 0);
    // Parents always come first, so one pass carries a removal down to the descendants.
    for (std::size_t i = 0; i < n; ++i) {
        const TreeNode& nd = nodes_[i];
        if (nd.parent < 0) continue;
        gone[i] = (i < remove.size() && remove[i]) || gone[std::size_t(nd.parent)];
    }
    std::vector<TreeNode> kept;
    std::vector<char> trunk;
    kept.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        if (gone[i]) continue;
        map[i] = int(kept.size());
        TreeNode nd = nodes_[i];
        nd.children = 0;
        if (nd.parent >= 0) {
            nd.parent = map[std::size_t(nd.parent)];
            ++kept[std::size_t(nd.parent)].children;
        }
        nd.root = map[std::size_t(nd.root)];
        kept.push_back(nd);
        trunk.push_back(i < trunk_.size() ? trunk_[i] : 0);
    }
    if (kept.size() == n) return map;
    nodes_ = std::move(kept);
    trunk_ = std::move(trunk);
    tips_.clear();
    for (auto& c : nodeGrid_.cells) c.clear();
    for (std::size_t i = 0; i < nodes_.size(); ++i) nodeGrid_.insert(int(i), nodes_[i].x, nodes_[i].y);
    updateTips();
    ++revision_;
    return map;
}

int TreeGrowth::addPoints(const std::vector<double>& xs, const std::vector<double>& ys) {
    const std::size_t n = std::min(xs.size(), ys.size());
    const double x1 = nodeGrid_.x0 + nodeGrid_.cell * nodeGrid_.cols, y1 = nodeGrid_.y0 + nodeGrid_.cell * nodeGrid_.rows;
    const double k2 = p_.kill * p_.kill;
    int added = 0;
    for (std::size_t i = 0; i < n && p_.pointsX.size() < 200000; ++i) {
        const double x = xs[i], y = ys[i];
        if (!std::isfinite(x) || !std::isfinite(y) || x < nodeGrid_.x0 || y < nodeGrid_.y0 || x > x1 || y > y1) continue;
        // Already reached: a node within Kill.
        bool near = false;
        const int c = std::clamp(int(std::floor((x - nodeGrid_.x0) / nodeGrid_.cell)), 0, nodeGrid_.cols - 1);
        const int r = std::clamp(int(std::floor((y - nodeGrid_.y0) / nodeGrid_.cell)), 0, nodeGrid_.rows - 1);
        for (int rr = std::max(0, r - 1); !near && rr <= std::min(nodeGrid_.rows - 1, r + 1); ++rr)
            for (int cc = std::max(0, c - 1); !near && cc <= std::min(nodeGrid_.cols - 1, c + 1); ++cc)
                for (int id : nodeGrid_.cells[std::size_t(rr * nodeGrid_.cols + cc)]) {
                    const double ex = x - nodes_[std::size_t(id)].x, ey = y - nodes_[std::size_t(id)].y;
                    if (ex * ex + ey * ey <= k2) {
                        near = true;
                        break;
                    }
                }
        if (near) continue;
        p_.pointsX.push_back(x);
        p_.pointsY.push_back(y);
        alive_.push_back(1);
        pointGrid_.insert(int(p_.pointsX.size()) - 1, x, y);
        ++livePoints_;
        ++added;
    }
    if (added > 0 && int(nodes_.size()) < p_.maxNodes) done_ = false;
    return added;
}

}  // namespace reforce::engine
