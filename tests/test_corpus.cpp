// Corpus test: every scene .txt under scenes/Packages loads and compiles with
// 0 errors, and evaluates once without crashing. scenes/ is git-ignored
// G-Force content; the test is skipped when it is absent.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "scene/Evaluate.hpp"

using namespace reforce::scene;
namespace fs = std::filesystem;

#ifndef REFORCE_SCENES_DIR
#define REFORCE_SCENES_DIR "scenes"
#endif

namespace {

struct Synthetic {
    std::vector<float> wave, fft;
    Synthetic(int nw, int nf) : wave(std::size_t(nw)), fft(std::size_t(nf)) {
        for (int i = 0; i < nw; ++i) wave[std::size_t(i)] = float(0.5 * std::sin(i * 0.37) + 0.2 * std::sin(i * 1.3));
        for (int i = 0; i < nf; ++i) fft[std::size_t(i)] = float(1.0 / (1.0 + i * 0.05));
    }
};

bool finite(const std::vector<float>& v) {
    for (float x : v)
        if (!std::isfinite(x)) return false;
    return true;
}

}  // namespace

TEST_CASE("corpus: every scene loads, compiles and evaluates") {
    const fs::path root = fs::path(REFORCE_SCENES_DIR) / "Packages";
    if (!fs::exists(root)) {
        MESSAGE("scenes/Packages not found at " << root.string() << "; corpus test skipped");
        return;
    }
    std::vector<fs::path> files;
    for (const auto& e : fs::recursive_directory_iterator(root))
        if (e.is_regular_file() && e.path().extension() == ".txt") files.push_back(e.path());
    std::sort(files.begin(), files.end());

    Environment env;
    env.width = 640;
    env.height = 360;
    env.loadTime = 12.5;
    Synthetic audio(env.numSampleBins, env.numFftBins);

    std::map<SceneType, int> loaded;
    std::map<std::string, int> warningKinds;
    std::size_t errors = 0, warnings = 0, nonFiniteFiles = 0;
    std::vector<std::string> nonFiniteNames;

    for (const auto& path : files) {
        const auto type = sceneTypeFromPath(path.string());
        if (!type) continue;
        auto file = loadSceneFile(path);
        REQUIRE(file.has_value());
        CompiledConfig cfg = compileConfig(*file, *type);
        ++loaded[*type];
        for (const auto& d : cfg.diags.items()) {
            if (d.severity == reforce::expr::Severity::Error) {
                ++errors;
                FAIL_CHECK(fs::relative(path, root).string() << ":" << d.line << " " << d.kind << ": " << d.message);
            } else {
                ++warnings;
                ++warningKinds[d.kind];
            }
        }

        bool ok = true;
        switch (*type) {
            case SceneType::WaveShape:
            case SceneType::Particle: {
                WaveShapeInstance w(cfg, env);
                WaveShapeFrameInput in;
                in.t = 13.0;
                in.audio = {audio.wave.data(), int(audio.wave.size()), audio.fft.data(), int(audio.fft.size())};
                in.bass = 0.6;
                in.level = 0.4;
                in.energy = 0.5;
                if (*type == SceneType::Particle) {
                    in.startTime = 12.5;
                    in.endTime = 12.5 + w.duration();
                }
                WaveShapeFrame f;
                w.evaluateFrame(in, f);
                for (const auto& p : f.paths) ok = ok && finite(p.x) && finite(p.y) && finite(p.pen) && finite(p.width);
                break;
            }
            case SceneType::ColorMap: {
                ColorMapInstance cm(cfg, env);
                std::array<Rgb, ColorMapInstance::kEntries> pal;
                cm.evaluate(13.0, pal);
                for (const auto& c : pal) ok = ok && std::isfinite(c.r) && std::isfinite(c.g) && std::isfinite(c.b);
                break;
            }
            case SceneType::FlowField:
            case SceneType::FlowFieldUnion: {
                FlowFieldInstance ff(cfg, env);
                const int w = 64, h = 36;
                const auto xs = ff.rowXs(w);
                std::vector<double> a(static_cast<std::size_t>(w)), b(static_cast<std::size_t>(w));
                for (int r : {0, 9, 17, 18, 35}) {
                    if (*type == SceneType::FlowField) ff.evaluateRow(ff.rowY(r, h), xs, a.data(), b.data());
                    else ff.evaluateAlphaRow(ff.rowY(r, h), xs, a.data());
                    for (int i = 0; i < w; ++i)
                        ok = ok && std::isfinite(a[std::size_t(i)]) &&
                             (*type == SceneType::FlowFieldUnion || std::isfinite(b[std::size_t(i)]));
                }
                break;
            }
            case SceneType::Sprite: {
                SpriteInstance sp(cfg, env);
                auto f = sp.evaluateFrame(13.0, 0.3);
                ok = std::isfinite(f.x) && std::isfinite(f.y) && std::isfinite(f.pen) && std::isfinite(f.epen);
                break;
            }
            case SceneType::Solid: {  // reforce's own (content/), not G-Force's corpus
                SolidInstance so(cfg, env);
                SolidFrameInput in;
                in.t = 13.0;
                auto f = so.evaluateFrame(in);
                ok = std::isfinite(f.x) && std::isfinite(f.y) && std::isfinite(f.z) && std::isfinite(f.scale);
                break;
            }
            case SceneType::Organic: {  // reforce's own (content/)
                OrganicInstance o(cfg, env);
                OrganicFrameInput in;
                in.t = 13.0;
                auto f = o.evaluateFrame(in);
                ok = std::isfinite(f.grow) && std::isfinite(f.x) && std::isfinite(f.scale);
                break;
            }
            case SceneType::Dancer: break;  // literal keys only (checked in tests/test_engine_dancers.cpp)
        }
        if (!ok) {
            ++nonFiniteFiles;
            nonFiniteNames.push_back(fs::relative(path, root).string());
        }
    }

    std::size_t total = 0;
    std::string byType;
    for (const auto& [t, n] : loaded) {
        total += std::size_t(n);
        byType += std::string(sceneTypeName(t)) + "=" + std::to_string(n) + " ";
    }
    std::printf("[corpus] files loaded: %zu (%s)\n", total, byType.c_str());
    std::printf("[corpus] errors: %zu  warnings: %zu\n", errors, warnings);
    for (const auto& [k, n] : warningKinds) std::printf("[corpus]   warning %-26s %d\n", k.c_str(), n);
    std::printf("[corpus] files with NaN/inf output after one evaluation: %zu\n", nonFiniteFiles);
    for (const auto& n : nonFiniteNames) std::printf("[corpus]   non-finite: %s\n", n.c_str());

    CHECK(total > 0);
    CHECK(errors == 0);
}

TEST_CASE("corpus: parallel, lane-sliced WaveShape evaluation is bit-identical to serial (Phase 5D)") {
    const fs::path root = fs::path(REFORCE_SCENES_DIR) / "Packages" / "G-Force.WaveShape.package";
    if (!fs::exists(root)) {
        MESSAGE("WaveShape package not found; skipped");
        return;
    }
    std::vector<fs::path> files;
    for (const auto& e : fs::recursive_directory_iterator(root))
        if (e.is_regular_file() && e.path().extension() == ".txt") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    Environment env;
    env.width = 1280;
    env.height = 720;
    env.loadTime = 4.0;
    Synthetic audio(env.numSampleBins, env.numFftBins);
    WorkerPool pool(3);
    auto same = [](const std::vector<float>& a, const std::vector<float>& b) {
        return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0);
    };
    int parallel = 0, mismatched = 0;
    for (const auto& path : files) {
        const auto type = sceneTypeFromPath(path.string());
        auto file = loadSceneFile(path);
        if (!type || !file) continue;
        CompiledConfig cfg = compileConfig(*file, *type);
        WaveShapeInstance serial(cfg, env), par(cfg, env);
        serial.setSlicing(false);  // reference: serial whole-row evaluation, as before Phase 5D
        par.setPool(&pool);
        par.setMinParallelWork(0);
        if (par.parallelMode() != WaveShapeInstance::Parallel::Serial) ++parallel;
        WaveShapeFrame fs, fp;
        bool ok = true;
        for (int frame = 0; frame < 3 && ok; ++frame) {
            WaveShapeFrameInput in;
            in.t = 4.0 + frame / 60.0;
            in.audio = {audio.wave.data(), int(audio.wave.size()), audio.fft.data(), int(audio.fft.size())};
            in.bass = 0.6;
            if (*type == SceneType::Particle) {
                in.startTime = 3.5;
                in.endTime = 3.5 + serial.duration();
            }
            serial.evaluateFrame(in, fs);
            par.evaluateFrame(in, fp);
            ok = fs.paths.size() == fp.paths.size();
            for (std::size_t p = 0; ok && p < fs.paths.size(); ++p)
                ok = same(fs.paths[p].x, fp.paths[p].x) && same(fs.paths[p].y, fp.paths[p].y) &&
                     same(fs.paths[p].pen, fp.paths[p].pen) && same(fs.paths[p].width, fp.paths[p].width);
        }
        if (!ok) {
            ++mismatched;
            FAIL_CHECK(fs::relative(path, root).string() << ": parallel result differs from serial");
        }
    }
    std::printf("[corpus] WaveShapes/Particles: %d of %zu evaluate steps in parallel, %d mismatched\n", parallel,
                files.size(), mismatched);
    CHECK(mismatched == 0);
}
