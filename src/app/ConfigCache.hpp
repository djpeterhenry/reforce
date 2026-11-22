#pragma once
// Prepared-config cache (Phase 5D): scene configs are parsed and compiled on
// a background worker ahead of their switch, so a switch on the render thread
// finds them ready. CompiledConfig is immutable and does not depend on the
// load environment (seed, time and size are applied when an instance is
// made), so caching it changes nothing about what plays.
//
// Entries are keyed by string ("cfg|<type>|<path>", "png|<path>", ...) and
// hold any immutable value. get() returns a ready entry at once, waits for one
// a worker is computing, and computes a missing (or still queued) one on the
// calling thread (tagged perf::TagCompile: a render-thread cache miss).
// Without workers (deterministic runs) everything computes on the caller.

#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace reforce::app {

class ConfigCache {
public:
    using Value = std::shared_ptr<const void>;
    using Producer = std::function<Value()>;

    explicit ConfigCache(int workers);
    ~ConfigCache();
    ConfigCache(const ConfigCache&) = delete;
    ConfigCache& operator=(const ConfigCache&) = delete;

    // Queues `make` for `key` unless known; `front`: ahead of the queue.
    void prefetch(const std::string& key, Producer make, bool front = false);
    // The value for `key` (see above). `computedHere` (optional): true when the
    // caller had to compute it.
    Value get(const std::string& key, const Producer& make, bool* computedHere = nullptr);

    template <class T>
    std::shared_ptr<const T> get(const std::string& key, const std::function<std::shared_ptr<const T>()>& make,
                                 bool* computedHere = nullptr) {
        return std::static_pointer_cast<const T>(get(key, Producer([&make]() -> Value { return make(); }),
                                                     computedHere));
    }

    bool background() const { return !threads_.empty(); }
    std::size_t size() const;
    std::size_t queued() const;

private:
    struct Slot {
        std::promise<Value> promise;
        std::shared_future<Value> future;
        bool started = false;  // under mutex_
        Producer make;
    };
    void run();

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::unordered_map<std::string, std::shared_ptr<Slot>> slots_;
    std::deque<std::shared_ptr<Slot>> queue_;
    bool stop_ = false;
    std::vector<std::thread> threads_;
};

}  // namespace reforce::app
