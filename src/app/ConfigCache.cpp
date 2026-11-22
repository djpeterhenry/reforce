#include "app/ConfigCache.hpp"

#include <pthread.h>

#include "engine/Perf.hpp"

namespace reforce::app {

ConfigCache::ConfigCache(int workers) {
    for (int i = 0; i < workers; ++i) threads_.emplace_back([this] { run(); });
}

ConfigCache::~ConfigCache() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    for (auto& t : threads_) t.join();
}

void ConfigCache::prefetch(const std::string& key, Producer make, bool front) {
    if (threads_.empty()) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = slots_.find(key);
        if (it != slots_.end()) {
            // Known: move a still-queued one to the front if asked.
            if (front && !it->second->started) {
                for (auto q = queue_.begin(); q != queue_.end(); ++q)
                    if (*q == it->second) {
                        queue_.erase(q);
                        break;
                    }
                queue_.push_front(it->second);
            }
            return;
        }
        auto slot = std::make_shared<Slot>();
        slot->future = slot->promise.get_future().share();
        slot->make = std::move(make);
        slots_.emplace(key, slot);
        if (front) queue_.push_front(slot);
        else queue_.push_back(slot);
    }
    cv_.notify_one();
}

ConfigCache::Value ConfigCache::get(const std::string& key, const Producer& make, bool* computedHere) {
    std::shared_ptr<Slot> slot;
    bool mine = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = slots_.find(key);
        if (it == slots_.end()) {
            slot = std::make_shared<Slot>();
            slot->future = slot->promise.get_future().share();
            slots_.emplace(key, slot);
            slot->started = true;
            mine = true;
        } else {
            slot = it->second;
            if (!slot->started) {  // still queued: do it here rather than wait for the queue
                slot->started = true;
                mine = true;
            }
        }
    }
    if (computedHere) *computedHere = mine;
    if (mine) {
        perf::tag(perf::TagCompile);
        Value v;
        try {
            v = make();
        } catch (...) {
            v = nullptr;
        }
        slot->promise.set_value(v);
        slot->make = nullptr;
        return v;
    }
    return slot->future.get();
}

void ConfigCache::run() {
    pthread_setname_np("reforce.prepare");
    pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
    for (;;) {
        std::shared_ptr<Slot> slot;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [&] { return stop_ || !queue_.empty(); });
            if (stop_) return;
            slot = queue_.front();
            queue_.pop_front();
            if (slot->started) continue;
            slot->started = true;
        }
        Value v;
        try {
            v = slot->make();
        } catch (...) {
            v = nullptr;
        }
        slot->make = nullptr;
        slot->promise.set_value(v);
    }
}

std::size_t ConfigCache::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return slots_.size();
}

std::size_t ConfigCache::queued() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

}  // namespace reforce::app
