#include "engine/Perf.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace reforce::perf {

namespace {
const char* const kTagNames[TagCount] = {
    "switch-wave", "switch-color", "switch-field", "field-begin", "field-bake", "field-end", "sprite-start",
    "particle-start", "compile", "alloc", "command", "catch-up", "timer", "audio", "theme", "file-io", "stress",
    "kill", "morph-end", "video-start", "video-stop", "solid-start",
    "organic-start",
    "dancer-start",
};
const char* const kHitchNames[5] = {"cpu", "gpu", "late", "repeat", "skip"};
}  // namespace

const char* tagName(int bit) { return bit >= 0 && bit < TagCount ? kTagNames[bit] : "?"; }
const char* hitchKindName(int bit) { return bit >= 0 && bit < 5 ? kHitchNames[bit] : "?"; }

std::string describeTags(std::uint32_t tags) {
    std::string s;
    for (int b = 0; b < TagCount; ++b)
        if (tags & (1u << b)) {
            if (!s.empty()) s += ',';
            s += kTagNames[b];
        }
    return s.empty() ? std::string("none") : s;
}

std::atomic<std::uint32_t>& frameTags() {
    static std::atomic<std::uint32_t> tags{0};
    return tags;
}

std::atomic<std::uint64_t>& renderAllocs() {
    static std::atomic<std::uint64_t> n{0};
    return n;
}

// ---------------------------------------------------------------- Histogram

void Histogram::add(double ms) {
    if (!(ms >= 0)) ms = 0;
    const int b = std::min(kBins - 1, int(ms / kBinMs));
    ++bins_[std::size_t(b)];
    ++count_;
    sum_ += ms;
    max_ = std::max(max_, ms);
}

double Histogram::percentile(double p) const {
    if (!count_) return 0;
    const double want = std::clamp(p, 0.0, 1.0) * double(count_);
    std::uint64_t acc = 0;
    for (int b = 0; b < kBins; ++b) {
        acc += bins_[std::size_t(b)];
        if (double(acc) >= want && acc > 0) return b == kBins - 1 ? max_ : std::min(max_, (b + 1) * kBinMs);
    }
    return max_;
}

std::vector<std::uint32_t> Histogram::coarse(const std::vector<double>& edges) const {
    std::vector<std::uint32_t> out(edges.size() + 1, 0);
    for (int b = 0; b < kBins; ++b) {
        if (!bins_[std::size_t(b)]) continue;
        const double lo = b * kBinMs;
        std::size_t k = 0;
        while (k < edges.size() && lo >= edges[k] - 1e-9) ++k;
        out[k] += bins_[std::size_t(b)];
    }
    return out;
}

// ---------------------------------------------------------------- FrameStats

FrameStats::FrameStats(std::size_t capacity)
    : ring_(std::max<std::size_t>(capacity, 16)),
      gpu_(new std::atomic<float>[std::max<std::size_t>(capacity, 16)]),
      cap_(std::max<std::size_t>(capacity, 16)) {
    for (std::size_t i = 0; i < cap_; ++i) gpu_[i].store(-1.f);
}

std::uint64_t FrameStats::reserve() {
    // Ring full of unfinalized frames (GPU stalled): force them out.
    if (next_ - finalized_ >= cap_ - 1) finalize(0);
    if (!reserved_) gpu_[next_ % cap_].store(-1.f, std::memory_order_release);
    reserved_ = true;
    return next_;
}

std::uint64_t FrameStats::add(FrameRecord r) {
    if (!reserved_) {
        if (next_ - finalized_ >= cap_ - 1) finalize(0);
        gpu_[next_ % cap_].store(r.gpuMs >= 0 ? r.gpuMs : 0.f, std::memory_order_release);
    }
    reserved_ = false;
    r.index = next_;
    ring_[next_ % cap_] = r;
    return next_++;
}

void FrameStats::setGpu(std::uint64_t index, double ms) {
    if (!(ms >= 0)) ms = 0;
    gpu_[index % cap_].store(float(ms), std::memory_order_release);
}

std::uint8_t FrameStats::cadenceHitch(const Cadence& c, int steps, int vsyncs, int& vsyncsHeld) {
    const double k = c.stepsPerVsync > 0 ? c.stepsPerVsync : 1.0;
    const int hold = std::max(1, int(std::lround(1.0 / k)));      // vsyncs per sim frame
    const int maxSteps = std::max(1, int(std::lround(k)));        // sim steps per vsync
    std::uint8_t h = 0;
    if (vsyncsHeld >= 0) vsyncsHeld += std::max(1, vsyncs);
    if (steps > 0) {
        if (vsyncsHeld >= 0) {
            if (vsyncsHeld > hold) h |= HitchRepeat;
            else if (vsyncsHeld < hold) h |= HitchSkip;  // previous sim frame shown too briefly
        }
        if (steps > maxSteps * std::max(1, vsyncs) || (steps > maxSteps && vsyncs <= 1)) h |= HitchSkip;
        vsyncsHeld = 0;
    }
    return h;
}

std::uint8_t FrameStats::detect(FrameRecord& r) {
    std::uint8_t h = 0;
    const double lim = 1.5 * cadence_.budgetMs;
    if (r.cpuMs > lim) h |= HitchCpu;
    if (r.gpuMs > lim) h |= HitchGpu;
    if (r.index > 0 && r.intervalMs > lim) h |= HitchLate;
    h |= cadenceHitch(cadence_, r.steps, r.vsyncs, vsyncsHeld_);
    return h;
}

void FrameStats::account(const FrameRecord& r) {
    ++frames_;
    cpu_.add(r.cpuMs);
    if (r.gpuMs >= 0) gpu_h_.add(r.gpuMs);
    if (r.index > 0) interval_.add(r.intervalMs);
    work_.add(std::max(r.cpuMs, double(std::max(0.f, r.gpuMs))));
    if (r.steps > 1) ++catchUp_;
    if (r.tags & TagTimer) ++timer_;
    if (!r.hitch) return;
    ++hitches_;
    for (int b = 0; b < 5; ++b)
        if (r.hitch & (1u << b)) ++byKind_[b];
    const std::uint32_t causes = r.tags & ~(TagTimer | TagCatchUp);
    if (!causes) ++untagged_;
    for (int b = 0; b < TagCount; ++b)
        if (r.tags & (1u << b)) ++byTag_[b];
}

void FrameStats::reset(std::uint64_t renderAllocsNow) {
    cpu_.clear();
    gpu_h_.clear();
    interval_.clear();
    work_.clear();
    frames_ = hitches_ = untagged_ = catchUp_ = timer_ = 0;
    std::fill(std::begin(byKind_), std::end(byKind_), 0);
    std::fill(std::begin(byTag_), std::end(byTag_), 0);
    allocBase_ = renderAllocsNow;
    lastHitch_.clear();
}

PerfSummary FrameStats::summary(std::uint64_t renderAllocsNow) const {
    PerfSummary s;
    s.frames = frames_;
    s.hitches = hitches_;
    std::copy(std::begin(byKind_), std::end(byKind_), s.byKind);
    std::copy(std::begin(byTag_), std::end(byTag_), s.byTag);
    s.untagged = untagged_;
    s.cpuP50 = cpu_.percentile(.5), s.cpuP99 = cpu_.percentile(.99), s.cpuMax = cpu_.max();
    s.gpuP50 = gpu_h_.percentile(.5), s.gpuP99 = gpu_h_.percentile(.99), s.gpuMax = gpu_h_.max();
    s.intervalP50 = interval_.percentile(.5), s.intervalP99 = interval_.percentile(.99),
    s.intervalMax = interval_.max();
    s.workP50 = work_.percentile(.5), s.workP99 = work_.percentile(.99), s.workMax = work_.max();
    s.budgetMs = cadence_.budgetMs;
    s.displayHz = cadence_.periodMs > 0 ? 1000.0 / cadence_.periodMs : 0;
    s.renderAllocs = renderAllocsNow >= allocBase_ ? renderAllocsNow - allocBase_ : 0;
    s.catchUpFrames = catchUp_;
    s.timerFrames = timer_;
    const double b = cadence_.budgetMs;
    s.histEdges = {1, 2, 4, b * 0.25, b * 0.5, b * 0.75, b, b * 1.5, b * 2};
    std::sort(s.histEdges.begin(), s.histEdges.end());
    s.histEdges.erase(std::unique(s.histEdges.begin(), s.histEdges.end(),
                                  [](double x, double y) { return std::abs(x - y) < 0.05; }),
                      s.histEdges.end());
    s.hist = work_.coarse(s.histEdges);
    s.lastHitch = lastHitch_;
    return s;
}

std::vector<FrameRecord> FrameStats::recent(std::size_t n) const {
    std::vector<FrameRecord> out;
    const std::uint64_t avail = std::min<std::uint64_t>(finalized_, cap_);
    const std::uint64_t take = std::min<std::uint64_t>(avail, n);
    for (std::uint64_t i = finalized_ - take; i < finalized_; ++i) out.push_back(ring_[i % cap_]);
    return out;
}

std::string FrameStats::describe(const FrameRecord& r) {
    std::string kinds;
    for (int b = 0; b < 5; ++b)
        if (r.hitch & (1u << b)) {
            if (!kinds.empty()) kinds += ',';
            kinds += kHitchNames[b];
        }
    char buf[320];
    std::snprintf(buf, sizeof buf,
                  "frame %llu t=%.2fs [%s] interval %.2f ms (%d vsync), cpu %.2f ms (sim %.2f, colorize %.2f, "
                  "submit %.2f), gpu %.2f ms, steps %d; causes: %s",
                  (unsigned long long)r.index, r.hostTime, kinds.c_str(), r.intervalMs, r.vsyncs, r.cpuMs, r.simMs,
                  r.colorizeMs, r.submitMs, double(r.gpuMs), r.steps, describeTags(r.tags).c_str());
    return buf;
}

std::string FrameStats::report(const PerfSummary& s) {
    std::string out;
    char buf[256];
    auto line = [&](const char* fmt, auto... args) {
        std::snprintf(buf, sizeof buf, fmt, args...);
        out += buf;
        out += '\n';
    };
    line("frames %llu, display %.1f Hz, budget %.2f ms (hitch threshold %.2f ms)", (unsigned long long)s.frames,
         s.displayHz, s.budgetMs, 1.5 * s.budgetMs);
    line("  %-10s %8s %8s %8s", "ms", "p50", "p99", "max");
    line("  %-10s %8.2f %8.2f %8.2f", "cpu", s.cpuP50, s.cpuP99, s.cpuMax);
    line("  %-10s %8.2f %8.2f %8.2f", "gpu", s.gpuP50, s.gpuP99, s.gpuMax);
    line("  %-10s %8.2f %8.2f %8.2f", "work", s.workP50, s.workP99, s.workMax);
    line("  %-10s %8.2f %8.2f %8.2f", "interval", s.intervalP50, s.intervalP99, s.intervalMax);
    out += "work-time histogram (max(cpu, gpu)):\n";
    std::uint32_t peak = 1;
    for (auto c : s.hist) peak = std::max(peak, c);
    for (std::size_t i = 0; i < s.hist.size(); ++i) {
        char range[48];
        if (i == 0) std::snprintf(range, sizeof range, "      < %5.2f", s.histEdges[0]);
        else if (i == s.hist.size() - 1) std::snprintf(range, sizeof range, "     >= %5.2f", s.histEdges[i - 1]);
        else std::snprintf(range, sizeof range, "%5.2f - %5.2f", s.histEdges[i - 1], s.histEdges[i]);
        const int bar = int(std::lround(40.0 * double(s.hist[i]) / double(peak)));
        line("  %s ms %8u %s", range, s.hist[i], std::string(std::size_t(bar), '#').c_str());
    }
    line("hitches: %llu (cpu %llu, gpu %llu, late %llu, repeat %llu, skip %llu); untagged %llu",
         (unsigned long long)s.hitches, (unsigned long long)s.byKind[0], (unsigned long long)s.byKind[1],
         (unsigned long long)s.byKind[2], (unsigned long long)s.byKind[3], (unsigned long long)s.byKind[4],
         (unsigned long long)s.untagged);
    std::string causes;
    for (int b = 0; b < TagCount; ++b)
        if (s.byTag[b]) {
            std::snprintf(buf, sizeof buf, "%s%s %llu", causes.empty() ? "" : ", ", kTagNames[b],
                          (unsigned long long)s.byTag[b]);
            causes += buf;
        }
    line("hitch causes (tags active in hitch frames): %s", causes.empty() ? "-" : causes.c_str());
    line("catch-up frames %llu, timer-driven frames %llu, render-thread Metal allocations %llu",
         (unsigned long long)s.catchUpFrames, (unsigned long long)s.timerFrames, (unsigned long long)s.renderAllocs);
    return out;
}

}  // namespace reforce::perf
