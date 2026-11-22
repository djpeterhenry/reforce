#pragma once
// Lock-free single-producer / single-consumer ring buffer of floats.
//
// Producer: the CoreAudio IO thread (or a file feeder thread, or the render
// thread itself in deterministic mode). Consumer: the render thread (Analyzer).
// write() and read() never allocate, lock, or block, so write() is safe on a
// real-time audio thread. Capacity is rounded up to a power of two; one
// instance is allocated up front and never resized.
//
// When the buffer is full, write() stores as many samples as fit and drops the
// rest (newest), counting them in droppedSamples(). The consumer drains every
// frame, so this only happens when the render thread stalls.

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>

namespace reforce::audio {

class SpscRingBuffer {
public:
    explicit SpscRingBuffer(size_t minCapacity = 1u << 16) {
        size_t cap = 1;
        while (cap < minCapacity) cap <<= 1;
        capacity_ = cap;
        mask_ = cap - 1;
        data_ = std::make_unique<float[]>(cap);
    }
    SpscRingBuffer(const SpscRingBuffer&) = delete;
    SpscRingBuffer& operator=(const SpscRingBuffer&) = delete;

    size_t capacity() const { return capacity_; }

    // Producer side. Returns the number of samples stored.
    size_t write(const float* src, size_t n) {
        const uint64_t w = writePos_.load(std::memory_order_relaxed);
        const uint64_t r = readPos_.load(std::memory_order_acquire);
        const size_t freeSpace = capacity_ - static_cast<size_t>(w - r);
        const size_t todo = std::min(n, freeSpace);
        if (todo < n) dropped_.fetch_add(n - todo, std::memory_order_relaxed);
        if (todo == 0) return 0;
        const size_t start = static_cast<size_t>(w & mask_);
        const size_t first = std::min(todo, capacity_ - start);
        std::memcpy(data_.get() + start, src, first * sizeof(float));
        if (todo > first) std::memcpy(data_.get(), src + first, (todo - first) * sizeof(float));
        writePos_.store(w + todo, std::memory_order_release);
        return todo;
    }

    // Consumer side. Returns the number of samples copied into dst.
    size_t read(float* dst, size_t maxN) {
        const uint64_t r = readPos_.load(std::memory_order_relaxed);
        const uint64_t w = writePos_.load(std::memory_order_acquire);
        const size_t todo = std::min(maxN, static_cast<size_t>(w - r));
        if (todo == 0) return 0;
        const size_t start = static_cast<size_t>(r & mask_);
        const size_t first = std::min(todo, capacity_ - start);
        std::memcpy(dst, data_.get() + start, first * sizeof(float));
        if (todo > first) std::memcpy(dst + first, data_.get(), (todo - first) * sizeof(float));
        readPos_.store(r + todo, std::memory_order_release);
        return todo;
    }

    // Consumer side: discard up to n samples without copying.
    size_t skip(size_t n) {
        const uint64_t r = readPos_.load(std::memory_order_relaxed);
        const uint64_t w = writePos_.load(std::memory_order_acquire);
        const size_t todo = std::min(n, static_cast<size_t>(w - r));
        readPos_.store(r + todo, std::memory_order_release);
        return todo;
    }

    // Either side (approximate when called concurrently).
    size_t available() const {
        return static_cast<size_t>(writePos_.load(std::memory_order_acquire) -
                                   readPos_.load(std::memory_order_acquire));
    }
    uint64_t droppedSamples() const { return dropped_.load(std::memory_order_relaxed); }
    uint64_t totalWritten() const { return writePos_.load(std::memory_order_acquire); }

private:
    std::unique_ptr<float[]> data_;
    size_t capacity_ = 0;
    size_t mask_ = 0;
    alignas(64) std::atomic<uint64_t> writePos_{0};
    alignas(64) std::atomic<uint64_t> readPos_{0};
    alignas(64) std::atomic<uint64_t> dropped_{0};
};

}  // namespace reforce::audio
