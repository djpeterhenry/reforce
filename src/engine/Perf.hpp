#pragma once
// Frame-time instrumentation (Phase 5D.1).
//
//   - cause tags: render-thread code ORs a bit into perf::frameTags() whenever
//     it does something that might cost a frame (a switch, a config compile, a
//     Metal resource creation, ...). The app reads and clears the tags once per
//     display frame and stores them with the frame's timings.
//   - FrameStats: a ring buffer of per-frame records (CPU split, GPU time from
//     the command buffer, display interval, simulation steps), fine-grained
//     histograms for percentiles, and a hitch detector:
//       over budget  CPU or GPU time, or the display interval, > 1.5 x budget
//       cadence      a display frame that repeats a simulation frame longer
//                    than the ideal cadence (e.g. 3 vsyncs instead of 2 at
//                    60 Hz sim / 120 Hz display), or runs more steps than it
//                    should (catch-up)
//     Each hitch is counted per kind and per cause tag active that frame.
// Pure C++ (no Metal), unit-tested.

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace reforce::perf {

enum Tag : std::uint32_t {
    TagSwitchWave = 1u << 0,     // WaveShape switch (instance created)
    TagSwitchColor = 1u << 1,    // ColorMap switch
    TagSwitchField = 1u << 2,    // FlowField switch requested
    TagFieldBegin = 1u << 3,     // FlowField transition starts (field adopted)
    TagFieldBake = 1u << 4,      // interrupted FlowField transition baked
    TagFieldEnd = 1u << 5,       // FlowField transition finished
    TagSpriteStart = 1u << 6,    // sprite adopted (texture registered)
    TagParticleStart = 1u << 7,  // particle instance created
    TagCompile = 1u << 8,        // a config parsed/compiled on the render thread (cache miss)
    TagAlloc = 1u << 9,          // Metal resource created on the render thread
    TagCommand = 1u << 10,       // engine command handled
    TagCatchUp = 1u << 11,       // more than one simulation step this frame
    TagTimer = 1u << 12,         // frame driven by the fallback timer
    TagAudio = 1u << 13,         // audio source swapped / serviced
    TagTheme = 1u << 14,         // theme applied (playlists rebuilt)
    TagFileIo = 1u << 15,        // file read on the render thread
    TagStress = 1u << 16,        // --stress injected commands this frame
    TagKill = 1u << 17,          // particles/sprites/solids/organics killed
    TagMorphEnd = 1u << 18,      // WaveShape/ColorMap transition finished
    TagVideoStart = 1u << 19,    // video source opened or adopted (movie/live sprite, color layer)
    TagVideoStop = 1u << 20,     // video source stopped / released
    TagSolidStart = 1u << 21,    // 3D solid started (scene file compiled, PLAN 5L)
    TagOrganicStart = 1u << 22,  // organic started (scene file compiled, tree points evaluated, PLAN 5N)
    TagDancerStart = 1u << 23,   // dancer started (scene file read, PLAN 5P)
};

inline constexpr int TagCount = 24;  // number of Tag bits

const char* tagName(int bit);
std::string describeTags(std::uint32_t tags);

// Tags of the frame in progress (render thread writes; relaxed atomics so a
// worker may tag too).
std::atomic<std::uint32_t>& frameTags();
inline void tag(std::uint32_t t) { frameTags().fetch_or(t, std::memory_order_relaxed); }

// Metal resources created on the render thread (should stay flat after warm-up).
std::atomic<std::uint64_t>& renderAllocs();
inline void countRenderAlloc() {
    renderAllocs().fetch_add(1, std::memory_order_relaxed);
    tag(TagAlloc);
}

enum HitchKind : std::uint8_t {
    HitchCpu = 1u << 0,     // CPU time of the frame > 1.5 x budget
    HitchGpu = 1u << 1,     // GPU time of the frame > 1.5 x budget
    HitchLate = 1u << 2,    // display interval > 1.5 x the expected one (missed vsync)
    HitchRepeat = 1u << 3,  // a simulation frame shown longer than the cadence says
    HitchSkip = 1u << 4,    // more simulation steps in one display frame than the cadence says
};
const char* hitchKindName(int bit);

struct FrameRecord {
    std::uint64_t index = 0;
    double hostTime = 0;     // s
    double intervalMs = 0;   // since the previous display frame
    double cpuMs = 0;        // whole onFrame
    double simMs = 0;        // simulation steps (Director::step incl. encode of the step)
    double colorizeMs = 0;   // colorize encode
    double submitMs = 0;     // Syphon + present capture + commit
    float gpuMs = -1;        // command buffer GPUEnd - GPUStart (-1 = unknown)
    int steps = 0;           // simulation steps run
    int vsyncs = 1;          // display refreshes since the previous frame
    std::uint32_t tags = 0;
    std::uint8_t hitch = 0;  // HitchKind bits (set when finalized)
};

// Fine histogram: 0.1 ms bins up to 100 ms, + overflow.
class Histogram {
public:
    static constexpr int kBins = 1001;
    static constexpr double kBinMs = 0.1;
    void add(double ms);
    void clear() { *this = Histogram{}; }
    std::uint64_t count() const { return count_; }
    double percentile(double p) const;  // ms (bin upper edge); 0 if empty
    double max() const { return max_; }
    double mean() const { return count_ ? sum_ / double(count_) : 0.0; }
    // Coarse counts for display, edges in ms (last bin open-ended).
    std::vector<std::uint32_t> coarse(const std::vector<double>& edges) const;

private:
    std::array<std::uint32_t, kBins> bins_{};
    std::uint64_t count_ = 0;
    double max_ = 0, sum_ = 0;
};

// Display cadence: what one display frame should do.
struct Cadence {
    double budgetMs = 1000.0 / 60.0;  // display frame budget (refresh period at the pinned rate)
    double periodMs = 1000.0 / 60.0;  // display refresh period (actual vsync spacing)
    double stepsPerVsync = 1.0;       // sim rate x period (0.5 at 60 Hz sim on 120 Hz)
};

struct PerfSummary {
    std::uint64_t frames = 0, hitches = 0;
    std::uint64_t byKind[5] = {};
    std::uint64_t byTag[TagCount] = {};
    std::uint64_t untagged = 0;       // hitches with no cause tag
    double cpuP50 = 0, cpuP99 = 0, cpuMax = 0;
    double gpuP50 = 0, gpuP99 = 0, gpuMax = 0;
    double intervalP50 = 0, intervalP99 = 0, intervalMax = 0;
    double workP50 = 0, workP99 = 0, workMax = 0;  // max(cpu, gpu) per frame
    double budgetMs = 0, displayHz = 0;
    std::uint64_t renderAllocs = 0;   // since the stats reset
    std::uint64_t catchUpFrames = 0;
    std::uint64_t timerFrames = 0;
    std::vector<double> histEdges;    // coarse work-time histogram (ms edges)
    std::vector<std::uint32_t> hist;
    std::string lastHitch;            // description of the latest hitch
};

class FrameStats {
public:
    explicit FrameStats(std::size_t capacity = 4096);

    void setCadence(const Cadence& c) { cadence_ = c; }
    const Cadence& cadence() const { return cadence_; }

    // Render thread: reserves the record index of the frame being built (pass
    // it to setGpu from the command buffer's completion handler), then add()
    // stores the finished record under it. add() without reserve() takes the
    // next index (and no GPU time is expected: gpuMs is used as given).
    std::uint64_t reserve();
    std::uint64_t add(FrameRecord r);
    // Any thread (command buffer completion): GPU time of frame `index`.
    void setGpu(std::uint64_t index, double ms);
    // Render thread: finalizes frames whose GPU time has arrived (or that are
    // older than `maxLag` frames): hitch detection + histograms. Returns the
    // number of new hitches; `onHitch` (optional) gets each hitch record.
    template <class F>
    int finalize(std::size_t maxLag, F&& onHitch);
    int finalize(std::size_t maxLag = 8) {
        return finalize(maxLag, [](const FrameRecord&) {});
    }

    // Clears histograms and counters (e.g. after warm-up).
    void reset(std::uint64_t renderAllocsNow = 0);

    PerfSummary summary(std::uint64_t renderAllocsNow = 0) const;
    // Recent records (oldest first), at most n, finalized ones only.
    std::vector<FrameRecord> recent(std::size_t n) const;

    // Pure cadence check for one frame (exposed for tests): updates `vsyncsHeld`
    // (display refreshes the current sim frame has been on screen).
    static std::uint8_t cadenceHitch(const Cadence& c, int steps, int vsyncs, int& vsyncsHeld);

    static std::string describe(const FrameRecord& r);
    static std::string report(const PerfSummary& s);

private:
    std::uint8_t detect(FrameRecord& r);
    void account(const FrameRecord& r);

    Cadence cadence_;
    std::vector<FrameRecord> ring_;
    std::unique_ptr<std::atomic<float>[]> gpu_;
    std::size_t cap_;
    std::uint64_t next_ = 0, finalized_ = 0;
    bool reserved_ = false;
    int vsyncsHeld_ = 0;
    Histogram cpu_, gpu_h_, interval_, work_;
    std::uint64_t frames_ = 0, hitches_ = 0, byKind_[5] = {}, byTag_[TagCount] = {}, untagged_ = 0;
    std::uint64_t catchUp_ = 0, timer_ = 0;
    std::uint64_t allocBase_ = 0;
    std::string lastHitch_;
};

template <class F>
int FrameStats::finalize(std::size_t maxLag, F&& onHitch) {
    int n = 0;
    while (finalized_ < next_) {
        FrameRecord& r = ring_[finalized_ % cap_];
        const float g = gpu_[finalized_ % cap_].load(std::memory_order_acquire);
        if (g < 0 && next_ - finalized_ <= maxLag) break;  // GPU time not in yet
        r.gpuMs = g;
        r.hitch = detect(r);
        account(r);
        if (r.hitch) {
            ++n;
            lastHitch_ = describe(r);
            onHitch(r);
        }
        ++finalized_;
    }
    return n;
}

}  // namespace reforce::perf
