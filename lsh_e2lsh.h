#pragma once

#include <vector>
#include <random>
#include <cmath>
#include <cstdint>
#include <stdexcept>

class E2LSH {
public:
    E2LSH(int dim_, int L_, int k_, double w_, uint64_t seed = 42, bool portable_rng = false)
        : dim(dim_), L(L_), k(k_), w(w_),
          A(L_, std::vector<std::vector<double>>(k_, std::vector<double>(dim_))),
          B(L_, std::vector<double>(k_))
    {
        if (dim <= 0 || L <= 0 || k <= 0 || !(w > 0.0)) {
            throw std::invalid_argument("E2LSH: dim,L,k must be >0 and w>0");
        }

        std::mt19937_64 gen(seed);
        if (portable_rng) {
            for (int l = 0; l < L; ++l) {
                for (int i = 0; i < k; ++i) {
                    for (int j = 0; j < dim; ++j) {
                        A[l][i][j] = portable_gaussian(gen);
                    }
                    B[l][i] = portable_uniform(gen) * w;
                }
            }
        } else {
            std::normal_distribution<double> gaussian(0.0, 1.0);
            std::uniform_real_distribution<double> uniform(0.0, w);

            for (int l = 0; l < L; ++l) {
                for (int i = 0; i < k; ++i) {
                    for (int j = 0; j < dim; ++j) {
                        A[l][i][j] = gaussian(gen);
                    }
                    B[l][i] = uniform(gen);
                }
            }
        }
    }

    // Returns L compound keys (one per table)
    std::vector<uint64_t> table_keys(const std::vector<double>& x) const {
        if ((int)x.size() != dim) {
            throw std::invalid_argument("E2LSH: x.size()!=dim");
        }

        std::vector<uint64_t> out;
        out.reserve(L);

        for (int l = 0; l < L; ++l) {

            // Start with table index to differentiate tables
            uint64_t key = splitmix64(0x9e3779b97f4a7c15ULL ^ (uint64_t)l);

            for (int i = 0; i < k; ++i) {

                double dot = 0.0;
                for (int j = 0; j < dim; ++j) {
                    dot += A[l][i][j] * (double)x[j];
                }

                // h_{a,b}(x)
                int64_t h = (int64_t)std::floor((dot + B[l][i]) / w);

                // Mix each h into compound key
                uint64_t v = (uint64_t)h ^ (uint64_t)(i * 0xBF58476D1CE4E5B9ULL);
                key = splitmix64(key ^ v);
            }

            out.push_back(key);
        }

        return out;
    }

    // Returns the independent H_{l,k}(x) keys used by the paper's [L] x [K]
    // fuzzy-cardinality Bloom construction.
    std::vector<std::vector<uint64_t>> hash_keys_by_table(const std::vector<double>& x) const {
        if ((int)x.size() != dim) {
            throw std::invalid_argument("E2LSH: x.size()!=dim");
        }

        std::vector<std::vector<uint64_t>> out(
            L,
            std::vector<uint64_t>(k)
        );

        for (int l = 0; l < L; ++l) {
            for (int i = 0; i < k; ++i) {
                double dot = 0.0;
                for (int j = 0; j < dim; ++j) {
                    dot += A[l][i][j] * (double)x[j];
                }

                const int64_t h = (int64_t)std::floor((dot + B[l][i]) / w);
                uint64_t key = splitmix64(0x9e3779b97f4a7c15ULL ^ (uint64_t)l);
                key = splitmix64(key ^ ((uint64_t)i * 0xBF58476D1CE4E5B9ULL));
                key = splitmix64(key ^ (uint64_t)h);
                out[l][i] = key;
            }
        }

        return out;
    }

    int get_L() const { return L; }
    int get_k() const { return k; }

private:
    static uint64_t splitmix64(uint64_t x) {
        x += 0x9e3779b97f4a7c15ULL;
        x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
        x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
        return x ^ (x >> 31);
    }

    static double portable_uniform(std::mt19937_64& gen) {
        static constexpr double denom = 9007199254740992.0; // 2^53
        const uint64_t bits = gen() >> 11;
        return (static_cast<double>(bits) + 0.5) / denom;
    }

    static double portable_gaussian(std::mt19937_64& gen) {
        const double u1 = portable_uniform(gen);
        const double u2 = portable_uniform(gen);
        static constexpr double two_pi = 6.283185307179586476925286766559;
        return std::sqrt(-2.0 * std::log(u1)) * std::cos(two_pi * u2);
    }

    int dim;
    int L;
    int k;
    double w;

    std::vector<std::vector<std::vector<double>>> A;  // [L][k][dim]
    std::vector<std::vector<double>> B;               // [L][k]
};
