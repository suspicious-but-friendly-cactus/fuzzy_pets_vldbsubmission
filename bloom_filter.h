#pragma once

#include <vector>
#include <cstdint>
#include <algorithm>
#include <stdexcept>
#include <cmath>

class E2LSH;

class BloomFilter {
public:
    explicit BloomFilter(size_t m_bits, size_t n_items)
        : bits(m_bits, false)
    {
        if (m_bits == 0) {
            throw std::invalid_argument("BloomFilter: m_bits must be > 0");
        }

        k_hashes = k_star_int(m_bits, n_items);
    }

    void clear() {
        std::fill(bits.begin(), bits.end(), false);
    }

    void insert_table_keys(const std::vector<uint64_t>& keys) {
        for (uint64_t key : keys) {
            auto indices = k_indices(key);
            for (size_t idx : indices)
                bits[idx] = true;
        }
    }

    void insert(const std::vector<double>& x, const E2LSH& lsh){
        insert_table_keys(lsh.table_keys(x));
    }

    bool membership(const std::vector<double>& x, const E2LSH& lsh) const {
        auto keys = lsh.table_keys(x);

        for (uint64_t key : keys) {
            auto indices = k_indices(key);

            bool present = true;
            for (size_t idx : indices) {
                if (!bits[idx]) {
                    present = false;
                    break;
                }
            }

            if (present)
                return true; // OR across tables
        }
        return false;
    }

    double fill_ratio() const {
        size_t ones = 0;
        for (bool b : bits) if (b) ++ones;
        return double(ones) / double(bits.size());
    }

    std::vector<size_t> indices_for_key(uint64_t key) const {
        return k_indices(key);
    }

    size_t hash_count() const {
        return k_hashes;
    }

    uint8_t bit_at(size_t idx) const {
        return bits.at(idx) ? 1 : 0;
    }

    size_t bucket_count() const {
        return bits.size();
    }

    size_t total_size_bits() const {
        return bits.size();
    }

private:
    std::vector<bool> bits;
    size_t k_hashes;   

    static uint64_t mix64(uint64_t x) {
        x += 0x9e3779b97f4a7c15ULL;
        x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
        x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
        return x ^ (x >> 31);
    }

    std::vector<size_t> k_indices(uint64_t key) const {
        uint64_t h1 = mix64(key);
        uint64_t h2 = mix64(key ^ 0xD1B54A32D192ED03ULL);

        size_t m = bits.size();
        std::vector<size_t> indices;
        indices.reserve(k_hashes);

        for (size_t i = 0; i < k_hashes; ++i) {
            uint64_t combined = h1 + i * h2;
            indices.push_back(static_cast<size_t>(combined % m));
        }

        return indices;
    }

    static double k_star_real(size_t m_bits, size_t n_items) {
        if (m_bits == 0 || n_items == 0) return 0.0;
        return (double(m_bits) / double(n_items)) * std::log(2.0);
    }

    static size_t k_star_int(size_t m_bits, size_t n_items) {
        double kreal = k_star_real(m_bits, n_items);
        long long kround = std::llround(kreal);
        if (kround < 1) kround = 1;
        return static_cast<size_t>(kround);
    }
};
