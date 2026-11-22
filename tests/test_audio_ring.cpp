#include <doctest/doctest.h>

#include "audio/SpscRingBuffer.hpp"

#include <thread>
#include <vector>

using reforce::audio::SpscRingBuffer;

TEST_CASE("SpscRingBuffer: capacity rounds up to a power of two") {
    SpscRingBuffer rb(1000);
    CHECK(rb.capacity() == 1024);
    CHECK(rb.available() == 0);
}

TEST_CASE("SpscRingBuffer: write/read order, wraparound, overflow drop") {
    SpscRingBuffer rb(8);
    std::vector<float> in(6), out(8);
    for (int round = 0; round < 10; ++round) {  // 6 per round on capacity 8 forces wraps
        for (int i = 0; i < 6; ++i) in[i] = static_cast<float>(round * 6 + i);
        REQUIRE(rb.write(in.data(), 6) == 6);
        REQUIRE(rb.available() == 6);
        REQUIRE(rb.read(out.data(), 8) == 6);
        for (int i = 0; i < 6; ++i) CHECK(out[i] == static_cast<float>(round * 6 + i));
    }
    // Overflow: only capacity fits, the rest is dropped and counted.
    std::vector<float> big(12, 1.f);
    CHECK(rb.write(big.data(), 12) == 8);
    CHECK(rb.droppedSamples() == 4);
    CHECK(rb.write(big.data(), 1) == 0);
    CHECK(rb.skip(3) == 3);
    CHECK(rb.available() == 5);
    CHECK(rb.read(out.data(), 2) == 2);
    CHECK(rb.available() == 3);
}

TEST_CASE("SpscRingBuffer: concurrent producer/consumer keeps every sample in order") {
    SpscRingBuffer rb(256);
    constexpr int kTotal = 2'000'000;  // < 2^24 so floats are exact
    std::thread producer([&] {
        float buf[37];
        int next = 0;
        while (next < kTotal) {
            const int n = std::min(37, kTotal - next);
            for (int i = 0; i < n; ++i) buf[i] = static_cast<float>(next + i);
            size_t done = 0;
            while (done < static_cast<size_t>(n)) {
                // Retry on full instead of dropping, so we can check continuity.
                const size_t free = rb.capacity() - rb.available();
                const size_t chunk = std::min(free, static_cast<size_t>(n) - done);
                if (chunk) done += rb.write(buf + done, chunk);
                else std::this_thread::yield();
            }
            next += n;
        }
    });
    int expect = 0;
    bool ok = true;
    float out[53];
    while (expect < kTotal) {
        const size_t got = rb.read(out, 53);
        if (got == 0) { std::this_thread::yield(); continue; }
        for (size_t i = 0; i < got; ++i) {
            if (out[i] != static_cast<float>(expect)) ok = false;
            ++expect;
        }
    }
    producer.join();
    CHECK(ok);
    CHECK(expect == kTotal);
    CHECK(rb.droppedSamples() == 0);
}
