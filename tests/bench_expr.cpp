// Micro-benchmark for the expression VM (not run by ctest).
//   ./build/reforce_expr_bench
#include <chrono>
#include <cstdio>
#include <vector>

#include "expr/Compiler.hpp"
#include "expr/Vm.hpp"
#include "scene/Evaluate.hpp"

using namespace reforce;
using Clock = std::chrono::steady_clock;

namespace {

double nsSince(Clock::time_point t0) {
    return double(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count());
}

void benchExpr(const char* src, int iters) {
    expr::Diagnostics d;
    expr::BasicResolver names;
    expr::Program p = expr::compileExpression(src, names, d);
    expr::EvalContext ctx;
    for (int f = 0; f < 4; ++f) ctx.ensureSlots(f, 8);
    ctx.slot(1, 0) = 0.3;
    ctx.slot(0, 1) = 0.7;
    std::vector<float> wave(200, 0.1f);
    ctx.audio = {wave.data(), 200, wave.data(), 185};
    double sink = 0;
    const auto t0 = Clock::now();
    for (int i = 0; i < iters; ++i) {
        ctx.set(expr::Var::S, double(i % 200) / 199.0);
        sink += expr::run(p, ctx).s_;
    }
    const double ns = nsSince(t0) / iters;
    std::printf("%-70s %3zu instrs  %7.1f ns/eval  %5.2f ns/instr  (sink %g)\n", src, p.code.size(), ns,
                ns / double(p.code.size()), sink);
}

}  // namespace

int main() {
    std::printf("== scalar expressions ==\n");
    benchExpr("sin(B0 + s * 2 * pi) * (.5 + .3 * mag(s)) + A1 * cos(t * .7)", 5'000'000);
    benchExpr("x * .95 + .02 * sin(y * 6)", 5'000'000);
    benchExpr("1 + 2", 20'000'000);
    benchExpr("sqrt(sqr(a0) + sqr(b0)) * trwv(s * 4) + pos(c1 - .5)", 5'000'000);

    std::printf("== FlowField 1920x1080 build (row-vectorized) ==\n");
    {
        auto f = scene::parseSceneText(
            "Aspc=1 A0=\".02 + rnd(.01)\" D0=\"r * (1 - A0 * sin(theta * 5))\" srcR=\"D0\" srcT=\"theta + A0 * (1 - r)\"");
        auto cfg = scene::compileConfig(f, scene::SceneType::FlowField);
        scene::FlowFieldInstance ff(cfg, {});
        std::vector<float> grid;
        const auto t0 = Clock::now();
        ff.evaluateGrid(1920, 1080, grid);
        const double ns = nsSince(t0);
        std::printf("polar swirl field: %.1f ms total, %.1f ns/pixel\n", ns / 1e6, ns / (1920.0 * 1080.0));
    }

    std::printf("== WaveShape frame ==\n");
    {
        auto f = scene::parseSceneText(
            "NUM=3 Stps=512 B0=\"B0 + .01\" C0=\"s * 2 * pi + B0 + ID\" X0=\"cos(C0) * (.5 + .3 * mag(s))\" "
            "Y0=\"sin(C0) * (.5 + .3 * mag(s))\" X1=\"-X0\" Y1=\"-Y0\" Pen=\"1 - s\" LWdt=2 ConB=1");
        auto cfg = scene::compileConfig(f, scene::SceneType::WaveShape);
        scene::WaveShapeInstance w(cfg, {});
        scene::WaveShapeFrame frame;
        std::vector<float> wave(200, 0.1f);
        scene::WaveShapeFrameInput in;
        in.audio = {wave.data(), 200, wave.data(), 185};
        const int frames = 2000;
        const auto t0 = Clock::now();
        for (int i = 0; i < frames; ++i) {
            in.t = i / 60.0;
            w.evaluateFrame(in, frame);
        }
        const double ns = nsSince(t0) / frames;
        std::printf("3 instances x 512 steps x 2 paths: %.1f us/frame\n", ns / 1e3);
    }

    std::printf("== ColorMap palette ==\n");
    {
        auto f = scene::parseSceneText("B0=\"t * .1\" C0=\"i ^ 1.5\" H=\"B0 + C0 * .3\" S=\".8\" V=\"C0\"");
        auto cfg = scene::compileConfig(f, scene::SceneType::ColorMap);
        scene::ColorMapInstance cm(cfg, {});
        std::array<scene::Rgb, 256> pal;
        const int reps = 20000;
        const auto t0 = Clock::now();
        for (int i = 0; i < reps; ++i) cm.evaluate(i * .01, pal);
        std::printf("256 entries: %.1f us/palette\n", nsSince(t0) / reps / 1e3);
    }
    return 0;
}
