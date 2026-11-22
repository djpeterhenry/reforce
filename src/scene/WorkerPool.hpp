#pragma once
// Persistent worker pool for data-parallel loops on the render thread
// (Phase 5D: WaveShape step evaluation, morph alignment).
//
//   pool.run(tasks, [&](int task, int lane) { ... });
//
// runs tasks 0..tasks-1 on the caller (lane 0) and the pool's threads (lanes
// 1..lanes()-1) and returns when all are done. Tasks are claimed dynamically,
// so a slow core only delays its current task. A dispatch allocates nothing.
// One caller at a time (the render thread); run() is not reentrant.
// Results must not depend on which lane runs a task: callers give each lane
// its own scratch state and write results by task index.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <type_traits>
#include <vector>

namespace reforce::scene {

class WorkerPool {
public:
    // threads: worker threads besides the caller. < 0: automatic (performance
    // cores minus two: the render and present threads), 0: run everything on
    // the caller.
    explicit WorkerPool(int threads = -1);
    ~WorkerPool();
    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    int lanes() const { return int(threads_.size()) + 1; }
    int workers() const { return int(threads_.size()); }

    template <class F>
    void run(int tasks, F&& f) {
        using Fn = std::remove_reference_t<F>;
        dispatch(tasks, [](void* c, int task, int lane) { (*static_cast<Fn*>(c))(task, lane); },
                 const_cast<void*>(static_cast<const void*>(&f)));
    }

    static int automaticThreads();

private:
    using RawFn = void (*)(void*, int, int);
    struct Job {
        RawFn fn = nullptr;
        void* ctx = nullptr;
        int tasks = 0;
    };
    void dispatch(int tasks, RawFn fn, void* ctx);
    void work(const Job& job, int lane);
    void threadMain(int lane);

    std::vector<std::thread> threads_;
    std::mutex m_;
    std::condition_variable cv_, idle_;
    Job job_;                          // guarded by m_
    std::uint64_t gen_ = 0;            // guarded by m_
    int inside_ = 0;                   // workers holding the current job (guarded by m_)
    bool stop_ = false;
    std::atomic<std::uint64_t> genSpin_{0};  // mirror of gen_ for the short spin before sleeping
    std::atomic<int> next_{0};         // next unclaimed task
    std::atomic<int> done_{0};         // finished tasks
};

}  // namespace reforce::scene
