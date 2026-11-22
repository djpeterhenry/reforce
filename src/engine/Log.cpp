#include "engine/Log.hpp"

#include <pthread.h>

#include <atomic>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

namespace reforce::logx {
namespace {

constexpr std::size_t kSlots = 512;
constexpr std::size_t kLine = 1024;

struct Logger {
    std::mutex mutex;
    std::condition_variable cv;
    char lines[kSlots][kLine];
    std::uint32_t len[kSlots] = {};
    std::size_t head = 0, tail = 0;  // head: next write, tail: next read (under mutex)
    bool stop = false;
    std::thread thread;
    std::atomic<bool> on{false};
    std::atomic<std::uint64_t> dropped{0};

    void run() {
        pthread_setname_np("reforce.log");
        static char out[kLine];
        for (;;) {
            std::size_t n = 0;
            {
                std::unique_lock<std::mutex> lock(mutex);
                cv.wait(lock, [&] { return stop || head != tail; });
                if (head == tail && stop) return;
                n = len[tail % kSlots];
                std::memcpy(out, lines[tail % kSlots], n);
                ++tail;
            }
            std::fwrite(out, 1, n, stderr);
            std::fflush(stderr);
        }
    }
};

Logger& logger() {
    static Logger* l = new Logger();  // never destroyed: usable during static teardown
    return *l;
}

}  // namespace

void setAsync(bool on) {
    Logger& l = logger();
    if (on == l.on.load()) return;
    if (on) {
        {
            std::lock_guard<std::mutex> lock(l.mutex);
            l.stop = false;
        }
        l.thread = std::thread([&l] { l.run(); });
        l.on.store(true);
    } else {
        l.on.store(false);
        {
            std::lock_guard<std::mutex> lock(l.mutex);
            l.stop = true;
        }
        l.cv.notify_all();
        if (l.thread.joinable()) l.thread.join();
    }
}

bool async() { return logger().on.load(std::memory_order_relaxed); }
std::uint64_t dropped() { return logger().dropped.load(); }

}  // namespace reforce::logx

namespace reforce {

void rlog(const char* fmt, ...) {
    char buf[reforce::logx::kLine];
    va_list ap;
    va_start(ap, fmt);
    int n = std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (std::size_t(n) >= sizeof buf) {
        n = int(sizeof buf - 1);
        buf[n - 1] = '\n';
    }
    logx::Logger& l = logx::logger();
    if (!l.on.load(std::memory_order_acquire)) {
        std::fwrite(buf, 1, std::size_t(n), stderr);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(l.mutex);
        if (l.head - l.tail >= logx::kSlots) {
            l.dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        std::memcpy(l.lines[l.head % logx::kSlots], buf, std::size_t(n));
        l.len[l.head % logx::kSlots] = std::uint32_t(n);
        ++l.head;
    }
    l.cv.notify_one();
}

}  // namespace reforce
