// WaveShape CPU benchmark over the corpus (Phase 5D; not run by ctest).
//   ./build/reforce_waveshape_bench [WxH] [frames] [threads] [filter]
// For every WaveShape/Particle config under scenes/Packages: per-frame time of
// evaluateFrame, toMorphFrame + segment generation, at the given render size.
// Then the heaviest configs are morphed pairwise (aligned mode). Prints the
// top configs and the worst/p99 frame.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "engine/Blend.hpp"
#include "engine/LineGeometry.hpp"
#if __has_include("scene/WorkerPool.hpp")
#include "scene/WorkerPool.hpp"
#define HAVE_POOL 1
#endif
#include "scene/Evaluate.hpp"
#include "scene/SceneFile.hpp"

#ifndef REFORCE_SCENES_DIR
#define REFORCE_SCENES_DIR "scenes"
#endif

using namespace reforce;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

double msSince(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }

struct Result {
    std::string name;
    double evalMean = 0, evalMax = 0, geomMean = 0;
    int ids = 0, paths = 0, steps = 0;
    std::size_t segments = 0;
    char mode = '-';
    std::shared_ptr<scene::CompiledConfig> cfg;
};

}  // namespace

int main(int argc, char** argv) {
    int W = 1920, H = 1080;
    if (argc > 1) std::sscanf(argv[1], "%dx%d", &W, &H);
    const int frames = argc > 2 ? std::atoi(argv[2]) : 30;
    const int threads = argc > 3 ? std::atoi(argv[3]) : 0;
    const std::string filter = argc > 4 ? argv[4] : "";
#ifdef HAVE_POOL
    scene::WorkerPool pool(threads < 0 ? 0 : (threads == 0 ? -1 : threads));
    const int workers = pool.workers();
#define SETPOOL(i) (i).setPool(threads < 0 ? nullptr : &pool)
#define MORPHPOOL (threads < 0 ? nullptr : &pool)
#else
    const int workers = 0;
    (void)threads;
#define SETPOOL(i) (void)0
#define MORPHPOOL nullptr
#endif
    std::printf("render %dx%d, %d frames, %d eval worker(s)\n", W, H, frames, workers);

    const fs::path root = fs::path(REFORCE_SCENES_DIR) / "Packages" / "G-Force.WaveShape.package";
    std::vector<fs::path> files;
    for (const auto& e : fs::recursive_directory_iterator(root))
        if (e.is_regular_file() && e.path().extension() == ".txt") files.push_back(e.path());
    std::sort(files.begin(), files.end());

    scene::Environment env;
    env.width = W;
    env.height = H;
    env.loadTime = 3;
    env.numSampleBins = 255;  // app defaults (Audio.PCM.NumBins, Audio.FFT.NumBins.vis)
    env.numFftBins = 255;
    std::vector<float> wave(std::size_t(env.numSampleBins)), fft(std::size_t(env.numFftBins));
    scene::WaveShapeFrameInput in;
    in.audio = {wave.data(), int(wave.size()), fft.data(), int(fft.size())};
    engine::FrameMapping map{double(W), double(H), 1.0, 1.0};
    engine::LineStyle style;

    std::vector<Result> results;
    std::vector<double> allFrames;
    for (const auto& f : files) {
        const std::string name = f.parent_path().filename().string() + "/" + f.stem().string();
        if (!filter.empty() && name.find(filter) == std::string::npos) continue;
        auto text = scene::loadSceneFile(f);
        if (!text) continue;
        auto cfg = std::make_shared<scene::CompiledConfig>(scene::compileConfig(*text, scene::SceneType::WaveShape));
        if (cfg->diags.errorCount()) continue;
        scene::WaveShapeInstance inst(*cfg, env);
        SETPOOL(inst);
        scene::WaveShapeFrame frame;
        engine::MorphFrame mf;
        std::vector<engine::LineSegment> segs;
        Result r;
        r.name = name;
        r.cfg = cfg;
        r.ids = inst.numInstances();
        r.paths = int(inst.pathIndices().size());
        r.steps = inst.numSteps();
#ifdef HAVE_POOL
        r.mode = "SIF"[int(inst.parallelMode())];
#endif
        for (int i = 0; i < frames; ++i) {
            in.t = 3 + i / 60.0;
            for (std::size_t k = 0; k < wave.size(); ++k) wave[k] = float(0.5 * std::sin(k * 0.37 + i * .2));
            for (std::size_t k = 0; k < fft.size(); ++k) fft[k] = float(1.0 / (1.0 + k * 0.05) * (1 + .5 * std::sin(i * .3 + k)));
            in.bass = .5 + .3 * std::sin(i * .1);
            auto t0 = Clock::now();
            inst.evaluateFrame(in, frame);
            const double e = msSince(t0);
            t0 = Clock::now();
            engine::toMorphFrame(frame, inst.xExtent(), inst.yExtent(), mf);
            segs.clear();
            engine::appendSegments(mf, map, style, segs);
            const double g = msSince(t0);
            r.evalMean += e;
            r.evalMax = std::max(r.evalMax, e);
            r.geomMean += g;
            allFrames.push_back(e + g);
        }
        r.evalMean /= frames;
        r.geomMean /= frames;
        r.segments = segs.size();
        results.push_back(std::move(r));
    }
    std::sort(results.begin(), results.end(), [](const Result& a, const Result& b) { return a.evalMax > b.evalMax; });
    double total = 0;
    for (const auto& r : results) total += r.evalMean;
    int modes[4] = {};
    for (const auto& r : results) modes[r.mode == 'S' ? 0 : r.mode == 'I' ? 1 : r.mode == 'F' ? 2 : 3]++;
    std::printf("%zu configs, sum of mean eval %.1f ms; parallel modes: %d serial, %d per-ID, %d frame\n",
                results.size(), total, modes[0], modes[1], modes[2]);
    std::printf("%-46s %5s %4s %6s %3s %8s %8s %8s %8s\n", "config", "IDs", "path", "steps", "par", "eval", "evalMax",
                "geom", "segs");
    for (std::size_t i = 0; i < std::min<std::size_t>(15, results.size()); ++i) {
        const auto& r = results[i];
        std::printf("%-46s %5d %4d %6d %3c %8.2f %8.2f %8.2f %8zu\n", r.name.c_str(), r.ids, r.paths, r.steps, r.mode,
                    r.evalMean, r.evalMax, r.geomMean, r.segments);
    }
    std::sort(allFrames.begin(), allFrames.end());
    if (!allFrames.empty())
        std::printf("single-shape frame (eval+geom): p50 %.2f  p99 %.2f  max %.2f ms\n",
                    allFrames[allFrames.size() / 2], allFrames[std::size_t(double(allFrames.size()) * .99)],
                    allFrames.back());

    // Morph: the heaviest configs pairwise (both evaluated + aligned morph + segments).
    const std::size_t heavy = std::min<std::size_t>(6, results.size());
    std::vector<double> morphFrames;
    double morphWorst = 0;
    std::string worstPair;
    for (std::size_t a = 0; a < heavy; ++a)
        for (std::size_t b = 0; b < heavy; ++b) {
            if (a == b) continue;
            scene::WaveShapeInstance ia(*results[a].cfg, env), ib(*results[b].cfg, env);
            SETPOOL(ia);
            SETPOOL(ib);
            scene::WaveShapeFrame fa, fb;
            engine::MorphFrame ma, mb, out;
            engine::AlignCache cache;
            std::vector<engine::LineSegment> segs;
            for (int i = 0; i < std::max(4, frames / 3); ++i) {
                in.t = 3 + i / 60.0;
                const auto t0 = Clock::now();
                ia.evaluateFrame(in, fa);
                ib.evaluateFrame(in, fb);
                engine::toMorphFrame(fa, ia.xExtent(), ia.yExtent(), ma);
                engine::toMorphFrame(fb, ib.xExtent(), ib.yExtent(), mb);
                engine::morphFrames(engine::blend::WaveAligned, ma, mb, .3 + .01 * i, out, &cache, MORPHPOOL);
                segs.clear();
                engine::appendSegments(out, map, style, segs);
                const double ms = msSince(t0);
                morphFrames.push_back(ms);
                if (ms > morphWorst) {
                    morphWorst = ms;
                    worstPair = results[a].name + " > " + results[b].name;
                }
            }
        }
    std::sort(morphFrames.begin(), morphFrames.end());
    if (!morphFrames.empty())
        std::printf("heavy morph frame: p50 %.2f  p99 %.2f  max %.2f ms (%s)\n", morphFrames[morphFrames.size() / 2],
                    morphFrames[std::size_t(double(morphFrames.size()) * .99)], morphFrames.back(), worstPair.c_str());
    return 0;
}
