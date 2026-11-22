// reforce-gen-videos: writes our own synthetic test videos (AVAssetWriter,
// hardware encoders). Never G-Force content. Output stays out of git (build/).
//
//   reforce-gen-videos FILE [--size WxH] [--fps F] [--frames N | --seconds S]
//                      [--codec h264|hevc|prores] [--pattern card|motion]
//   reforce-gen-videos --suite DIR [--quick]
//       the perf/test set: card 320x180 H.264 (frame-exact tests), motion
//       1080p H.264 / HEVC / ProRes 422 and 2160p H.264 / HEVC / ProRes 422,
//       10 s each (--quick: 2 s and 1080p only)
//   reforce-gen-videos --bench FILE...
//       decode benchmark: every frame through the deterministic VideoSource
//       path (AVAssetReader + CVMetalTextureCache), per-frame decode ms and
//       frames/s (how far above real time the decoder runs)
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

#include "video/VideoSource.hpp"

namespace {

using reforce::video::TestCodec;
using reforce::video::TestPattern;
using reforce::video::TestVideoSpec;

int usage() {
    std::fprintf(stderr,
                 "usage: reforce-gen-videos FILE [--size WxH] [--fps F] [--frames N | --seconds S]\n"
                 "                          [--codec h264|hevc|prores] [--pattern card|motion]\n"
                 "       reforce-gen-videos --suite DIR [--quick]\n");
    return 2;
}

bool write(const std::string& path, const TestVideoSpec& s) {
    const auto t0 = std::chrono::steady_clock::now();
    std::string err;
    if (!reforce::video::writeTestVideo(path, s, &err)) {
        std::fprintf(stderr, "%s: %s\n", path.c_str(), err.c_str());
        return false;
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("%s: %dx%d, %d frames at %.3g fps (%.1f s to write)\n", path.c_str(), s.width, s.height, s.frames,
                s.fps, secs);
    return true;
}

}  // namespace

int bench(int argc, char** argv) {
    MTL::Device* dev = MTL::CreateSystemDefaultDevice();
    if (!dev) return 1;
    for (int i = 2; i < argc; ++i) {
        reforce::video::SourceSpec spec;
        spec.id = argv[i];
        spec.loop = false;
        reforce::video::OpenOptions o;
        o.deterministic = true;
        const auto t0 = std::chrono::steady_clock::now();
        auto src = reforce::video::openSource(dev, spec, o);
        src->waitReady(30);
        if (!src->ready()) {
            std::fprintf(stderr, "%s: %s\n", argv[i], src->error().c_str());
            continue;
        }
        const double first = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        const double fps = src->frameRate() > 0 ? src->frameRate() : 30;
        const int frames = int(src->duration() * fps + 0.5);
        const auto t1 = std::chrono::steady_clock::now();
        for (int f = 0; f < frames; ++f) src->frameAt(double(f) / fps);
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count();
        const auto st = src->stats();
        std::printf("%-28s %dx%d: first frame %.1f ms, decode %.2f ms/frame avg (max %.2f), %d frames in %.2f s = "
                    "%.0f fps (%.1fx real time)\n",
                    std::filesystem::path(argv[i]).filename().string().c_str(), src->width(), src->height(), first,
                    st.decodeMsAvg, st.decodeMsMax, frames, secs, frames / secs, frames / secs / fps);
    }
    dev->release();
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) return usage();
    if (std::strcmp(argv[1], "--bench") == 0) return bench(argc, argv);
    if (std::strcmp(argv[1], "--suite") == 0) {
        if (argc < 3) return usage();
        const std::string dir = argv[2];
        const bool quick = argc > 3 && std::strcmp(argv[3], "--quick") == 0;
        const int secs = quick ? 2 : 10;
        struct Item { const char* name; int w, h; TestCodec codec; TestPattern pattern; int seconds; };
        std::vector<Item> items = {
            {"card_180p_h264.mov", 320, 180, TestCodec::H264, TestPattern::Card, 4},
            {"motion_1080p_h264.mp4", 1920, 1080, TestCodec::H264, TestPattern::Motion, secs},
            {"motion_1080p_hevc.mov", 1920, 1080, TestCodec::HEVC, TestPattern::Motion, secs},
            {"motion_1080p_prores.mov", 1920, 1080, TestCodec::ProRes422, TestPattern::Motion, secs},
        };
        if (!quick) {
            items.push_back({"motion_2160p_h264.mp4", 3840, 2160, TestCodec::H264, TestPattern::Motion, secs});
            items.push_back({"motion_2160p_hevc.mov", 3840, 2160, TestCodec::HEVC, TestPattern::Motion, secs});
            // ProRes 4K is ~70 MB/s: keep it short.
            items.push_back({"motion_2160p_prores.mov", 3840, 2160, TestCodec::ProRes422, TestPattern::Motion, 5});
        }
        bool ok = true;
        for (const Item& it : items) {
            TestVideoSpec s;
            s.width = it.w;
            s.height = it.h;
            s.fps = 30;
            s.frames = it.seconds * 30;
            s.codec = it.codec;
            s.pattern = it.pattern;
            ok = write(dir + "/" + it.name, s) && ok;
        }
        return ok ? 0 : 1;
    }
    TestVideoSpec s;
    double seconds = -1;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        const bool has = i + 1 < argc;
        if (a == "--size" && has) {
            if (std::sscanf(argv[++i], "%dx%d", &s.width, &s.height) != 2) return usage();
        } else if (a == "--fps" && has) s.fps = std::atof(argv[++i]);
        else if (a == "--frames" && has) s.frames = std::atoi(argv[++i]);
        else if (a == "--seconds" && has) seconds = std::atof(argv[++i]);
        else if (a == "--codec" && has) {
            const std::string c = argv[++i];
            if (c == "h264") s.codec = TestCodec::H264;
            else if (c == "hevc") s.codec = TestCodec::HEVC;
            else if (c == "prores") s.codec = TestCodec::ProRes422;
            else return usage();
        } else if (a == "--pattern" && has) {
            const std::string p = argv[++i];
            if (p == "card") s.pattern = TestPattern::Card;
            else if (p == "motion") s.pattern = TestPattern::Motion;
            else return usage();
        } else return usage();
    }
    if (seconds > 0) s.frames = int(seconds * s.fps + 0.5);
    return write(argv[1], s) ? 0 : 1;
}
