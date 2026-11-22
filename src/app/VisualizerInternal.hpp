#pragma once
// Private to the Visualizer's .cpp files (Visualizer.cpp, Visualizer<Area>.cpp): small helpers they share, the
// WaveShape node. (The built-in configs, declared in Visualizer.hpp, are defined in Visualizer.cpp.)

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <sstream>
#include <string>
#include <utility>

#include "app/Visualizer.hpp"

namespace reforce::app {

namespace visdetail {

using Clock = std::chrono::steady_clock;
inline double msSince(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

inline std::string lowerExt(const std::filesystem::path& p) {
    std::string e = p.extension().string();
    for (char& c : e) c = char(std::tolower(static_cast<unsigned char>(c)));
    return e;
}

inline bool readFile(const std::filesystem::path& p, std::string& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    std::stringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

template <class T>
bool futureReady(const std::future<T>& f) {
    return f.valid() && f.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}

inline int modeSlot(engine::Layer l) {
    switch (l) {
        case engine::Layer::WaveShape: return 0;
        case engine::Layer::ColorMap: return 1;
        case engine::Layer::FlowField: return 2;
        default: return -1;
    }
}

}  // namespace visdetail

// ---------------------------------------------------------------- WaveShape node

// A WaveShape source: a live config (leaf), or a frozen morph between two
// sources (the "from" side after an interruption).
struct Visualizer::WaveNode {
    std::shared_ptr<const scene::CompiledConfig> cfg;
    std::unique_ptr<scene::WaveShapeInstance> inst;
    std::string name;
    std::unique_ptr<WaveNode> a, b;
    double w = 0;
    int mode = 0;
    engine::AlignCache cache;
    scene::WaveShapeFrame frame;
    engine::MorphFrame fa, fb;
    scene::WorkerPool* pool = nullptr;  // the Visualizer's evaluation pool (Phase 5D)
    double t0 = 0;  // load time: a WaveShape's t is config-local, seconds since its own start (probe #37)

    bool leaf() const { return inst != nullptr; }
    int depth() const { return leaf() ? 0 : 1 + std::max(a->depth(), b->depth()); }
    std::string label() const { return leaf() ? name : a->label() + " > " + b->label(); }
    // The config extents (X_EXTENTS, Y_EXTENTS); a frozen morph blends them by its weight.
    std::pair<double, double> extents() const {
        if (leaf()) return {inst->xExtent(), inst->yExtent()};
        const auto ea = a->extents(), eb = b->extents();
        return {ea.first + (eb.first - ea.first) * w, ea.second + (eb.second - ea.second) * w};
    }
    void evaluate(const scene::WaveShapeFrameInput& in, engine::MorphFrame& out) {
        if (leaf()) {
            scene::WaveShapeFrameInput local = in;
            local.t = in.t - t0;
            inst->evaluateFrame(local, frame);
            engine::toMorphFrame(frame, inst->xExtent(), inst->yExtent(), out);
            return;
        }
        a->evaluate(in, fa);
        b->evaluate(in, fb);
        engine::morphFrames(mode, fa, fb, w, out, &cache, pool);
    }
};

}  // namespace reforce::app
