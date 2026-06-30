// hash_xor.h
#pragma once
#include <cstdint>
#include <random>

// IMPORTANT: must change across attempts, otherwise bff_fuzzy.h may retry forever.
class xorHashFamily {
public:
    xorHashFamily() {
        std::random_device rd;
        std::mt19937_64 gen(((uint64_t)rd() << 32) ^ (uint64_t)rd());
        seed1 = gen() | 1ULL;
        seed2 = gen() | 1ULL;
        seed3 = gen() | 1ULL;
        seed4 = gen() | 1ULL;
    }

    uint64_t operator()(uint64_t x) const {
        uint64_t h = splitmix64(x + seed1);
        h ^= splitmix64(x + seed2);
        h ^= splitmix64(x + seed3);
        h ^= splitmix64(x + seed4);
        return h;
    }

private:
    uint64_t seed1, seed2, seed3, seed4;

    static inline uint64_t splitmix64(uint64_t x) {
        x += 0x9e3779b97f4a7c15ULL;
        x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
        x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
        return x ^ (x >> 31);
    }
};