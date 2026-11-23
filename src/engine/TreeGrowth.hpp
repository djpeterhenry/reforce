#pragma once
// Organics (PLAN 5N, docs/organics-design.md): tree growth by space colonisation (Runions et al. 2007).
//
// Attraction points pull the nearest node within `influence`; each pulled node grows one segment towards the
// average pull (plus tropism and jitter); points within `kill` of a node are used up. Before any point is in reach
// the newest tips grow on (the trunk): along their root's direction with a little jitter, no tropism (tropism bends
// branches; added to a trunk every step it would curl it right round). Pure C++, deterministic for a given seed and call sequence.
//
// Units are "tree units" (y = +-1 at the bottom / top of the frame, x in the same units); nothing here depends on
// the frame size.

#include <cstdint>
#include <vector>

#include "expr/Rng.hpp"

namespace reforce::engine {

struct TreeRoot {
    double x = 0, y = -1;
    double angle = 1.5707963267948966;  // initial growth direction, radians (PI/2 = up)
};

struct TreeParams {
    std::vector<double> pointsX, pointsY;  // attraction points (same length)
    std::vector<TreeRoot> roots;           // at least one (an empty list plants one default root)
    double segment = 0.02;                 // segment length
    double influence = 0.3;                // a point pulls the nearest node within this distance
    double kill = 0.04;                    // a point is used up once a node is this close
    double jitter = 0.15;                  // random wobble added to each new direction (unit vector scale)
    int maxNodes = 6000;                   // growth stops here
    int maxTrunk = 400;                    // trunk iterations without reaching any point before giving up
};

struct TreeNode {
    float x = 0, y = 0;    // position
    float dx = 0, dy = 1;  // growth direction (unit) of the segment that ends here
    int parent = -1;       // index of the parent (always lower), -1 for a root
    int root = 0;          // index of its root node
    int gen = 0;           // growth iteration that made this node (roots: 0)
    int depth = 0;         // segments from its root
    int children = 0;
    float tips = 1;        // pipe model: tips (childless nodes) this node feeds, itself included
    float phase = 0;       // sway phase (radians), continuous along a branch
};

class TreeGrowth {
public:
    TreeGrowth(TreeParams params, std::uint64_t seed);

    // One growth iteration with tropism (tx, ty) added to every new direction. Returns the nodes added
    // (0 once the tree is complete).
    int iterate(double tx = 0, double ty = 0);

    // Rewiring (docs/organics-design.md "Pulses", Rwir): removes the nodes marked in `remove` (index = node; roots
    // are never removed; a removed node's descendants go too) and returns, per old node, its new index or -1. Growth
    // stops continuing old trunks.
    std::vector<int> prune(const std::vector<char>& remove);
    // Adds attraction points (finite ones within the grid's bounds, not already within Kill of a node) and lets the
    // tree grow on towards them. Returns how many were added.
    int addPoints(const std::vector<double>& xs, const std::vector<double>& ys);
    // Changes with every structural change (nodes added or removed).
    int revision() const { return revision_; }

    bool done() const { return done_; }
    int generation() const { return gen_; }  // iterations that added nodes
    const std::vector<TreeNode>& nodes() const { return nodes_; }
    int livePoints() const { return livePoints_; }
    float maxTips() const { return maxTips_; }  // largest root's tips (pipe-model normaliser), >= 1
    const TreeParams& params() const { return p_; }

private:
    struct Grid {
        double x0 = 0, y0 = 0, cell = 1;
        int cols = 1, rows = 1;
        std::vector<std::vector<int>> cells;
        int index(double x, double y) const;
        void insert(int id, double x, double y) { cells[std::size_t(index(x, y))].push_back(id); }
    };
    int addNode(int parent, double x, double y, double dx, double dy);
    void killNear(double x, double y);
    void updateTips();
    double rand01() { return rng_.uniform01Exclusive(); }

    TreeParams p_;
    expr::Pcg32 rng_;
    std::vector<TreeNode> nodes_;
    std::vector<char> alive_;  // per attraction point
    int livePoints_ = 0;
    Grid nodeGrid_, pointGrid_;
    std::vector<int> tips_;    // nodes added by the last iteration (trunk growth continues from them)
    std::vector<char> trunk_;  // per node: on a trunk (no point has pulled its lineage yet)
    std::vector<double> pullX_, pullY_;
    std::vector<int> pullN_, pulled_;
    int gen_ = 0;
    int revision_ = 0;
    bool done_ = false;
    float maxTips_ = 1;
};

}  // namespace reforce::engine
