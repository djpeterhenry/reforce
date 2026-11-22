#include "scene/WorkerPool.hpp"

#include <algorithm>
#include <chrono>

#ifdef __APPLE__
#include <pthread.h>
#include <sys/sysctl.h>
#endif

namespace reforce::scene {

int WorkerPool::automaticThreads() {
    int cores = 0;
#ifdef __APPLE__
    int v = 0;
    std::size_t len = sizeof v;
    if (sysctlbyname("hw.perflevel0.physicalcpu", &v, &len, nullptr, 0) == 0 && v > 0) cores = v;
#endif
    if (cores <= 0) cores = int(std::max(1u, std::thread::hardware_concurrency() / 2));
    return std::clamp(cores - 2, 0, 15);
}

WorkerPool::WorkerPool(int threads) {
    if (threads < 0) threads = automaticThreads();
    threads_.reserve(std::size_t(threads));
    for (int i = 0; i < threads; ++i) threads_.emplace_back([this, i] { threadMain(i + 1); });
}

WorkerPool::~WorkerPool() {
    {
        std::lock_guard lk(m_);
        stop_ = true;
        ++gen_;
    }
    genSpin_.fetch_add(1, std::memory_order_release);
    cv_.notify_all();
    for (auto& t : threads_) t.join();
}

void WorkerPool::work(const Job& job, int lane) {
    for (;;) {
        const int t = next_.fetch_add(1, std::memory_order_relaxed);
        if (t >= job.tasks) return;
        job.fn(job.ctx, t, lane);
        if (done_.fetch_add(1, std::memory_order_acq_rel) + 1 == job.tasks) done_.notify_one();
    }
}

void WorkerPool::dispatch(int tasks, RawFn fn, void* ctx) {
    if (tasks <= 0) return;
    if (threads_.empty() || tasks == 1) {
        for (int t = 0; t < tasks; ++t) fn(ctx, t, 0);
        return;
    }
    Job job{fn, ctx, tasks};
    {
        // A worker that woke late for the previous job may still be claiming
        // (and finding no) tasks: let it leave before the counters reset.
        std::unique_lock lk(m_);
        idle_.wait(lk, [&] { return inside_ == 0; });
        job_ = job;
        next_.store(0, std::memory_order_relaxed);
        done_.store(0, std::memory_order_relaxed);
        ++gen_;
    }
    genSpin_.fetch_add(1, std::memory_order_release);
    cv_.notify_all();
    work(job, 0);
    for (int d = done_.load(std::memory_order_acquire); d < tasks; d = done_.load(std::memory_order_acquire))
        done_.wait(d, std::memory_order_acquire);
}

void WorkerPool::threadMain(int lane) {
#ifdef __APPLE__
    // Same class as the render thread: these tasks are on its critical path.
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
    std::uint64_t seen = 0;
    for (;;) {
        // Short spin first: back-to-back dispatches (one per WaveShape ID, the
        // morph after the shapes) then skip the sleep/wake latency.
        const auto spinEnd = std::chrono::steady_clock::now() + std::chrono::microseconds(50);
        while (genSpin_.load(std::memory_order_acquire) == seen && std::chrono::steady_clock::now() < spinEnd)
            std::this_thread::yield();
        Job job;
        {
            std::unique_lock lk(m_);
            cv_.wait(lk, [&] { return stop_ || gen_ != seen; });
            if (stop_) return;
            seen = gen_;
            job = job_;
            ++inside_;
        }
        work(job, lane);
        {
            std::lock_guard lk(m_);
            if (--inside_ == 0) idle_.notify_one();
        }
    }
}

}  // namespace reforce::scene
