#pragma once
// Builds a FlowField's source-coordinate grid on worker threads
// (architecture §4.3; scene-format §5.3).
//
// Output: one (srcX, srcY) pair per destination pixel, row 0 = top row,
// in *normalized frame coordinates*: center origin, +y up, the frame edges at
// +-1 on both axes. A config with Aspc=1 is evaluated in its 1:1 coordinates
// (short axis +-1, long axis +-(long-1)/(short-1): square pixel steps, sweep
// #36) and divided by its extents here, so the GPU flow pass never needs to
// know about Aspc.
//
// Non-finite sources (NaN, inf) are written as kOutOfFrame, which the flow
// pass treats like any other out-of-frame source (black, probe #13).
//
// Determinism: every worker runs its own FlowFieldInstance built with the same
// seed (so A vars agree), and the RNG is reseeded per row from (seed, row), so
// the result does not depend on the thread count or scheduling.

#include <atomic>
#include <cstdint>
#include <future>
#include <memory>
#include <vector>

#include "scene/Evaluate.hpp"

namespace reforce::engine {

inline constexpr float kOutOfFrame = 1.0e9f;

struct FlowFieldGrid {
    int width = 0, height = 0;
    double xExtent = 1.0, yExtent = 1.0;  // the config's extents (Aspc)
    bool polar = false;
    std::vector<float> srcXY;             // width * height * 2, normalized frame coords
    double buildSeconds = 0.0;
};

struct FlowFieldBuildOptions {
    int width = 1920, height = 1080;  // grid size (normally the render size)
    int threads = 0;                  // 0 = hardware concurrency
    std::atomic<bool>* cancel = nullptr;
};

// Synchronous build (runs on the calling thread plus `threads - 1` helpers).
// `cfg` must outlive the call.
FlowFieldGrid buildFlowField(const scene::CompiledConfig& cfg, const scene::Environment& env,
                             const FlowFieldBuildOptions& opts);

// Asynchronous build; `cfg` is shared so the caller may drop it.
std::future<FlowFieldGrid> buildFlowFieldAsync(std::shared_ptr<const scene::CompiledConfig> cfg,
                                               scene::Environment env, FlowFieldBuildOptions opts);

// FlowField Union ALFA mask (unclamped, row 0 = top) at opts.width x height,
// evaluated with the same row machinery and per-row RNG seeding.
struct UnionMask {
    int width = 0, height = 0;
    std::vector<float> alpha;
    double buildSeconds = 0;
};
UnionMask buildUnionMask(const scene::CompiledConfig& cfg, const scene::Environment& env,
                         const FlowFieldBuildOptions& opts);
std::future<UnionMask> buildUnionMaskAsync(std::shared_ptr<const scene::CompiledConfig> cfg, scene::Environment env,
                                           FlowFieldBuildOptions opts);

// Identity field (each pixel sources its own center).
FlowFieldGrid identityFlowField(int width, int height);

}  // namespace reforce::engine
