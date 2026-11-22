#pragma once
// Deterministic PCG32 random stream (one per evaluation context / instance).

#include <cstdint>
#include <cstring>

namespace reforce::expr {

class Pcg32 {
public:
    explicit Pcg32(std::uint64_t seed = 0x853c49e6748fea9bULL) { reseed(seed); }

    void reseed(std::uint64_t seed, std::uint64_t stream = 0xda3e39cb94b95bdbULL) {
        state_ = 0;
        inc_ = (stream << 1u) | 1u;
        next();
        state_ += splitmix(seed);
        next();
    }

    // Seeds from the bit pattern of a double ("seed the RNG from the bits of x").
    void reseedFromDouble(double x) {
        if (x == 0.0) x = 0.0;  // fold -0 into +0
        std::uint64_t bits;
        std::memcpy(&bits, &x, sizeof bits);
        reseed(bits);
    }

    std::uint32_t next() {
        const std::uint64_t old = state_;
        state_ = old * 6364136223846793005ULL + inc_;
        const auto xorshifted = std::uint32_t(((old >> 18u) ^ old) >> 27u);
        const auto rot = std::uint32_t(old >> 59u);
        return (xorshifted >> rot) | (xorshifted << ((32u - rot) & 31u));
    }

    // Uniform in [0, 1] inclusive (rnd(x) is documented as inclusive of x).
    double uniform01() { return double(next()) / 4294967295.0; }
    // Uniform in [0, 1).
    double uniform01Exclusive() { return double(next()) / 4294967296.0; }

private:
    static std::uint64_t splitmix(std::uint64_t z) {
        z += 0x9e3779b97f4a7c15ULL;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31);
    }
    std::uint64_t state_ = 0;
    std::uint64_t inc_ = 1;
};

}  // namespace reforce::expr
