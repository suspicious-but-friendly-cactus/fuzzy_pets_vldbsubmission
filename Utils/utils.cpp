#include "utils.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
#include <unordered_map>
#include <nlohmann/json.hpp>
#include <unordered_set>
#include <limits>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <algorithm>
#include <random>
#include <sstream>
#include <filesystem>
#include <numeric>

using namespace std;
using json = nlohmann::json;

std::string CLIENT_CLOSE_PATH = "datasets/gowalla_client/gowalla_client_close.json";
std::string CLIENT_FAR_PATH = "datasets/gowalla_client/gowalla_client_far.json";

static size_t portable_rng_index(std::mt19937& rng, size_t exclusive_upper) {
    if (exclusive_upper == 0) {
        throw std::runtime_error("portable_rng_index called with empty range");
    }
    const uint64_t value = (static_cast<uint64_t>(rng()) << 32) |
                           static_cast<uint64_t>(rng());
    return static_cast<size_t>(value % static_cast<uint64_t>(exclusive_upper));
}

template <typename T>
static void dataset_shuffle(std::vector<T>& values, std::mt19937& rng) {
    if (!PORTABLE_DATASET_SAMPLING) {
        std::shuffle(values.begin(), values.end(), rng);
        return;
    }
    for (size_t i = values.size(); i > 1; --i) {
        const size_t j = portable_rng_index(rng, i);
        std::swap(values[i - 1], values[j]);
    }
}

size_t count_server_json_entries(const std::string& server_path) {
    std::ifstream in(server_path);
    if (!in) {
        throw std::runtime_error("Cannot open " + server_path);
    }

    json j;
    in >> j;
    if (!j.is_object()) {
        throw std::runtime_error(server_path + " must be {id: pixels}");
    }
    return j.size();
}

static double clamp_double(double value, double lo, double hi) {
    if (value < lo) return lo;
    if (value > hi) return hi;
    return value;
}

static double sample_interpolated_marginal(
    const std::vector<double>& sorted_values,
    std::mt19937& rng
) {
    if (sorted_values.empty()) {
        throw std::runtime_error("Cannot sample from an empty marginal distribution");
    }
    if (sorted_values.size() == 1) {
        return sorted_values.front();
    }

    std::uniform_real_distribution<double> pos_dist(
        0.0,
        static_cast<double>(sorted_values.size() - 1)
    );
    const double pos = pos_dist(rng);
    const size_t lo = static_cast<size_t>(std::floor(pos));
    const size_t hi = std::min(lo + 1, sorted_values.size() - 1);
    const double frac = pos - static_cast<double>(lo);
    return sorted_values[lo] * (1.0 - frac) + sorted_values[hi] * frac;
}

static void append_distribution_server_points(
    const std::vector<std::vector<double>>& base_imgs,
    size_t extra_count,
    std::mt19937& rng,
    std::vector<std::string>& server_ids,
    std::vector<std::vector<double>>& server_imgs
) {
    if (base_imgs.empty()) {
        throw std::runtime_error("Cannot fit Server distribution from an empty pool");
    }

    const size_t dim = base_imgs.front().size();
    if (dim == 0) {
        throw std::runtime_error("Cannot fit Server distribution for zero-dimensional points");
    }

    const size_t n = base_imgs.size();
    std::vector<double> min_v(dim, std::numeric_limits<double>::infinity());
    std::vector<double> max_v(dim, -std::numeric_limits<double>::infinity());
    std::vector<long double> sum(dim, 0.0L);
    std::vector<double> predictor_values;
    predictor_values.reserve(n);

    for (const auto& point : base_imgs) {
        if (point.size() != dim) {
            throw std::runtime_error("Server distribution fit requires equal point dimensions");
        }
        predictor_values.push_back(point[0]);
        for (size_t d = 0; d < dim; ++d) {
            min_v[d] = std::min(min_v[d], point[d]);
            max_v[d] = std::max(max_v[d], point[d]);
            sum[d] += point[d];
        }
    }

    std::vector<double> mean(dim, 0.0);
    for (size_t d = 0; d < dim; ++d) {
        mean[d] = static_cast<double>(sum[d] / static_cast<long double>(n));
    }

    std::vector<long double> var_sum(dim, 0.0L);
    long double predictor_var_sum = 0.0L;
    for (const auto& point : base_imgs) {
        const long double predictor_delta =
            static_cast<long double>(point[0]) - static_cast<long double>(mean[0]);
        predictor_var_sum += predictor_delta * predictor_delta;
        for (size_t d = 0; d < dim; ++d) {
            const long double delta =
                static_cast<long double>(point[d]) - static_cast<long double>(mean[d]);
            var_sum[d] += delta * delta;
        }
    }

    std::vector<double> stddev(dim, 0.0);
    for (size_t d = 0; d < dim; ++d) {
        stddev[d] = std::sqrt(static_cast<double>(var_sum[d] / static_cast<long double>(n)));
    }

    std::vector<double> slope(dim, 0.0);
    std::vector<double> intercept = mean;
    if (predictor_var_sum > 0.0L) {
        for (size_t d = 1; d < dim; ++d) {
            long double cov_sum = 0.0L;
            for (const auto& point : base_imgs) {
                cov_sum +=
                    (static_cast<long double>(point[0]) - static_cast<long double>(mean[0])) *
                    (static_cast<long double>(point[d]) - static_cast<long double>(mean[d]));
            }
            slope[d] = static_cast<double>(cov_sum / predictor_var_sum);
            intercept[d] = mean[d] - slope[d] * mean[0];
        }
    }

    std::vector<double> residual_std(dim, 0.0);
    for (size_t d = 1; d < dim; ++d) {
        long double residual_sum = 0.0L;
        for (const auto& point : base_imgs) {
            const long double predicted =
                static_cast<long double>(intercept[d]) +
                static_cast<long double>(slope[d]) * static_cast<long double>(point[0]);
            const long double residual = static_cast<long double>(point[d]) - predicted;
            residual_sum += residual * residual;
        }
        residual_std[d] =
            std::sqrt(static_cast<double>(residual_sum / static_cast<long double>(n)));
    }

    std::sort(predictor_values.begin(), predictor_values.end());
    const double noise_scale = DISTRIBUTION_SERVER_NOISE_SCALE > 0.0
        ? DISTRIBUTION_SERVER_NOISE_SCALE
        : 1.0;
    const double predictor_jitter_std = stddev[0] * 0.001 * noise_scale;
    std::normal_distribution<double> predictor_jitter(0.0, predictor_jitter_std);

    std::cout << "[Server distribution model]"
              << " requested_extra=" << extra_count
              << " base_pool=" << n
              << " dim=" << dim
              << " predictor_mean=" << mean[0]
              << " predictor_std=" << stddev[0]
              << " noise_scale=" << noise_scale;
    if (dim > 1) {
        std::cout << " dim1_slope=" << slope[1]
                  << " dim1_intercept=" << intercept[1]
                  << " dim1_residual_std=" << residual_std[1];
    }
    std::cout << "\n";

    for (size_t i = 0; i < extra_count; ++i) {
        std::vector<double> synthetic(dim, 0.0);
        double predictor = sample_interpolated_marginal(predictor_values, rng);
        if (predictor_jitter_std > 0.0) {
            predictor += predictor_jitter(rng);
        }
        synthetic[0] = clamp_double(predictor, min_v[0], max_v[0]);

        for (size_t d = 1; d < dim; ++d) {
            const double jitter_std = residual_std[d] > 0.0
                ? residual_std[d] * noise_scale
                : stddev[d] * 0.001 * noise_scale;
            double value = intercept[d] + slope[d] * synthetic[0];
            if (jitter_std > 0.0) {
                std::normal_distribution<double> residual_noise(0.0, jitter_std);
                value += residual_noise(rng);
            }
            synthetic[d] = clamp_double(value, min_v[d], max_v[d]);
        }

        server_ids.push_back("distribution:" + std::to_string(i));
        server_imgs.push_back(std::move(synthetic));
    }
}

static void append_augmented_server_points(
    const std::vector<std::string>& base_ids,
    const std::vector<std::vector<double>>& base_imgs,
    const std::vector<size_t>& base_indices,
    size_t extra_count,
    std::mt19937& rng,
    std::vector<std::string>& server_ids,
    std::vector<std::vector<double>>& server_imgs
) {
    if (extra_count == 0) {
        return;
    }
    if (base_indices.empty()) {
        throw std::runtime_error("Cannot append augmented Server points from an empty base pool");
    }

    const double jitter = AUGMENTED_SERVER_JITTER_DEG > 0.0
        ? AUGMENTED_SERVER_JITTER_DEG
        : 0.002;
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    std::uniform_real_distribution<double> signed_unit(-1.0, 1.0);
    constexpr double pi = 3.14159265358979323846;

    for (size_t i = 0; i < extra_count; ++i) {
        const size_t base_idx = base_indices[i % base_indices.size()];
        const auto& base = base_imgs[base_idx];
        std::vector<double> augmented = base;

        if (augmented.size() == 2) {
            const double angle = unit(rng) * 2.0 * pi;
            const double dist = jitter * std::sqrt(unit(rng));
            augmented[0] += dist * std::sin(angle);
            augmented[1] += dist * std::cos(angle);
        } else {
            for (double& value : augmented) {
                value += signed_unit(rng) * jitter;
            }
        }

        server_ids.push_back("augmented:" + base_ids[base_idx] + ":" + std::to_string(i));
        server_imgs.push_back(std::move(augmented));
    }
}

static uint64_t stable_id_hash(const std::string& value) {
    uint64_t hash = 1469598103934665603ULL;
    for (unsigned char ch : value) {
        hash ^= static_cast<uint64_t>(ch);
        hash *= 1099511628211ULL;
    }
    hash += 0x9e3779b97f4a7c15ULL;
    hash = (hash ^ (hash >> 30)) * 0xbf58476d1ce4e5b9ULL;
    hash = (hash ^ (hash >> 27)) * 0x94d049bb133111ebULL;
    return hash ^ (hash >> 31);
}

static void build_holdout_split_indices(
    const std::vector<std::string>& ids,
    std::vector<size_t>& train_indices,
    std::vector<size_t>& holdout_indices
) {
    std::vector<size_t> ordered(ids.size());
    std::iota(ordered.begin(), ordered.end(), 0);
    std::sort(ordered.begin(), ordered.end(), [&](size_t left, size_t right) {
        const uint64_t left_hash = stable_id_hash(ids[left]);
        const uint64_t right_hash = stable_id_hash(ids[right]);
        if (left_hash != right_hash) {
            return left_hash < right_hash;
        }
        return ids[left] < ids[right];
    });

    const size_t holdout_count = ids.size() / 5;
    holdout_indices.assign(ordered.begin(), ordered.begin() + holdout_count);
    train_indices.assign(ordered.begin() + holdout_count, ordered.end());
}

static std::vector<unsigned char> build_holdout_split_active_mask(
    const std::vector<std::string>& ids,
    bool use_holdout
) {
    std::vector<size_t> train_indices;
    std::vector<size_t> holdout_indices;
    build_holdout_split_indices(ids, train_indices, holdout_indices);

    std::vector<unsigned char> active(ids.size(), 0);
    const std::vector<size_t>& selected = use_holdout ? holdout_indices : train_indices;
    for (size_t idx : selected) {
        active[idx] = 1;
    }
    return active;
}

// --------------------------------------------------
// Fake LSH black box (TEMPORARY)
// Returns random integer keys for an image
// --------------------------------------------------
std::vector<int> fake_lsh_keys(
    const std::vector<int>& img,
    int num_keys,
    std::mt19937& rng
) {
    (void)img; // unused for now
    std::uniform_int_distribution<int> dist(0, 1 << 20);

    std::vector<int> keys;
    keys.reserve(num_keys);
    for (int i = 0; i < num_keys; i++) keys.push_back(dist(rng));
    return keys;
}

// --------------------------------------------------
// L2 distance between two images (pixel vectors)
// --------------------------------------------------
double l2_dist(const std::vector<int>& a, const std::vector<int>& b) {
    assert(a.size() == b.size());
    long double sum = 0.0;
    for (size_t i = 0; i < a.size(); i++) {
        long double d = (long double)a[i] - (long double)b[i];
        sum += d * d;
    }
    return std::sqrt((double)sum);
}

void print_single_image(const std::vector<int>& v) {
    std::cout << "[";
    for (size_t i = 0; i < v.size(); ++i) {
        std::cout << v[i];
        if (i + 1 < v.size())
            std::cout << ", ";
    }
    std::cout << "]\n";
}

// --------------------------------------------------
// Debug: print a few images
// --------------------------------------------------
void print_images(
    const std::string& name,
    const std::vector<std::vector<int>>& imgs,
    size_t max_imgs,
    size_t max_pix
) {
    max_pix = 10;
    cout << "\n[" << name << "] count=" << imgs.size() << "\n";
    for (size_t i = 0; i < imgs.size() && i < max_imgs; i++) {
        cout << "Image " << i 
             << " (length=" << imgs[i].size() << "): [";

        for (size_t j = 0; j < imgs[i].size() && j < max_pix; j++) {
            cout << imgs[i][j];
            if (j + 1 < imgs[i].size() && j + 1 < max_pix)
                cout << ", ";
        }
        cout << ", ...]\n";
    }
}

// --------------------------------------------------
// Debug: print L2 distance of each Client image to the Server image it relates to
// --------------------------------------------------
void debug_print_dataset(
    const std::vector<std::string>& server_ids,
    const std::vector<std::vector<int>>& server,
    const std::vector<std::vector<int>>& client,
    const std::vector<bool>& client_is_close,
    const std::vector<std::string>& client_relates_to,
    size_t /*maxn*/
) {
    // safety
    if (client.size() != client_is_close.size() || client.size() != client_relates_to.size()) {
        std::cout << "\n[DEBUG] size mismatch:"
                  << " client=" << client.size()
                  << " client_is_close=" << client_is_close.size()
                  << " client_relates_to=" << client_relates_to.size()
                  << "\n";
        return;
    }
    if (server.empty() || server_ids.size() != server.size()) {
        std::cout << "\n[DEBUG] server empty or server_ids mismatch:"
                  << " server=" << server.size()
                  << " server_ids=" << server_ids.size()
                  << "\n";
        return;
    }

    // Map server_id -> index in server
    std::unordered_map<std::string, size_t> idx;
    idx.reserve(server_ids.size());
    for (size_t i = 0; i < server_ids.size(); i++) idx[server_ids[i]] = i;

    std::cout << "\n=== DEBUG L2 CHECK (Client vs related Server) ===\n";
    for (size_t i = 0; i < client.size(); i++) {
        const std::string& qid = client_relates_to[i];
        auto it = idx.find(qid);

        if (it == idx.end()) {
            // qid not found → compute min distance to all Server
            double best_d = std::numeric_limits<double>::infinity();
            size_t best_j = 0;

            for (size_t j = 0; j < server.size(); ++j) {
                double d = l2_dist(server[j], client[i]);
                if (d < best_d) {
                    best_d = d;
                    best_j = j;
                }
            }
            std::cout << "Client[" << i << "] "
                      << (client_is_close[i] ? "CLOSE" : "FAR")
                      << " relates_to=" << qid
                      << " (NOT IN SERVER IDS!)"
                      << "  min_L2_to_any_server=" << best_d
                      << "  nearest_server_id=" << server_ids[best_j]
                      << "  nearest_server_idx=" << best_j
                      << "\n";
            continue;
        }

        size_t aidx = it->second;
        double d = l2_dist(server[aidx], client[i]);

        std::cout << "Client[" << i << "] "
                  << (client_is_close[i] ? "CLOSE" : "FAR")
                  << " relates_to=" << qid
                  << "  L2=" << d << "\n";
    }
    std::cout << "===========================================\n";
}

// --------------------------------------------------
// Load gowalla_server.json and sample N test_ids/images
// gowalla_server.json format: { "test_id": [pixels...], ... }
// --------------------------------------------------
void load_server_from_json(
    const std::string& server_path,
    int N,
    std::mt19937& rng,
    std::vector<std::string>& server_ids,
    std::vector<std::vector<double>>& server_imgs
) {
    // ------------------------
    // STATIC CACHE
    // ------------------------
    static std::vector<std::string> all_ids;
    static std::vector<std::vector<double>> all_imgs;
    static bool initialized = false;
    static std::string cached_server_path;
    static std::vector<std::string> random_append_ids;
    static std::vector<std::vector<double>> random_append_imgs;
    static bool random_append_initialized = false;
    static std::string cached_random_append_path;
    static std::vector<size_t> all_indices;

    auto load_pool = [](const std::string& path,
                        const std::string& id_prefix,
                        std::vector<std::string>& ids,
                        std::vector<std::vector<double>>& imgs) {
        PHASE_LOG << "[phase] Server JSON load start path=" << path << "\n" << std::flush;

        std::ifstream in(path);
        if (!in) throw std::runtime_error("Cannot open " + path);

        json j;
        in >> j;
        PHASE_LOG << "[phase] Server JSON parse done entries=" << j.size() << "\n" << std::flush;

        if (!j.is_object())
            throw std::runtime_error(path + " must be {id: pixels}");

        ids.clear();
        imgs.clear();
        ids.reserve(j.size());
        imgs.reserve(j.size());

        for (auto it = j.begin(); it != j.end(); ++it) {
            const std::string id = id_prefix + it.key();
            const json& pix = it.value();

            if (!pix.is_array())
                throw std::runtime_error("server[" + id + "] must be array");

            std::vector<double> img;
            img.reserve(pix.size());

            for (const auto& v : pix) {
                img.push_back(v.get<double>());
            }
            ids.push_back(id);
            imgs.push_back(std::move(img));
        }

        if (ids.empty())
            throw std::runtime_error(path + " is empty");

        PHASE_LOG << "[phase] Server pool build done path=" << path
                  << " size=" << ids.size() << "\n" << std::flush;
    };

    if (!initialized || cached_server_path != server_path) {
        load_pool(server_path, "", all_ids, all_imgs);
        all_indices.resize(all_ids.size());
        std::iota(all_indices.begin(), all_indices.end(), 0);
        cached_server_path = server_path;
        initialized = true;
    }

    server_ids.clear();
    server_imgs.clear();

    if (N <= 0) {
        return;
    }

    const size_t requested_count = static_cast<size_t>(N);
    const bool use_split_pool = CALIBRATION_HOLDOUT;
    const std::vector<std::string>* source_ids = &all_ids;
    const std::vector<std::vector<double>>* source_imgs = &all_imgs;

    std::vector<size_t> train_indices;
    std::vector<size_t> holdout_indices;
    std::vector<size_t> active_indices;
    if (use_split_pool) {
        build_holdout_split_indices(*source_ids, train_indices, holdout_indices);
        active_indices = CALIBRATION_HOLDOUT_USE_HELD_OUT ? holdout_indices : train_indices;
    } else {
        active_indices = all_indices;
    }
    const size_t pool_count = active_indices.size();
    if (pool_count == 0) {
        throw std::runtime_error("Selected Server pool is empty after holdout split");
    }
    PHASE_LOG << "[phase] Server sampling start requested=" << requested_count
              << " pool=" << pool_count
              << " holdout_mode=" << (use_split_pool ? "true" : "false")
              << " split=" << (CALIBRATION_HOLDOUT_USE_HELD_OUT ? "holdout" : "train")
              << "\n" << std::flush;
    if (use_split_pool) {
        std::cout << "[Server holdout split]"
                  << " total=" << source_ids->size()
                  << " original=" << all_ids.size()
                  << " synthetic_pre_split=" << (source_ids->size() - all_ids.size())
                  << " train=" << train_indices.size()
                  << " holdout=" << holdout_indices.size()
                  << " using="
                  << (CALIBRATION_HOLDOUT_USE_HELD_OUT ? "holdout" : "train")
                  << "\n";
    }
    server_ids.reserve(requested_count);
    server_imgs.reserve(requested_count);

    std::vector<size_t> indices(pool_count);
    std::iota(indices.begin(), indices.end(), 0);
    dataset_shuffle(indices, rng);

    const size_t unique_count = std::min(requested_count, pool_count);
    for (size_t i = 0; i < unique_count; i++) {
        size_t idx = active_indices[indices[i]];
        server_ids.push_back((*source_ids)[idx]);
        server_imgs.push_back((*source_imgs)[idx]);
    }

    if (requested_count > pool_count && APPEND_DISTRIBUTION_SERVER) {
        const size_t extra_count = requested_count - pool_count;
        PHASE_LOG << "[phase] Server distribution append start requested_extra=" << extra_count
                  << " base_pool=" << pool_count
                  << "\n" << std::flush;

        std::vector<std::vector<double>> distribution_base;
        distribution_base.reserve(active_indices.size());
        for (size_t idx : active_indices) {
            distribution_base.push_back((*source_imgs)[idx]);
        }
        append_distribution_server_points(
            distribution_base,
            extra_count,
            rng,
            server_ids,
            server_imgs
        );

        PHASE_LOG << "[phase] Server distribution append done selected=" << server_imgs.size()
                  << "\n" << std::flush;
    } else if (requested_count > pool_count && APPEND_AUGMENTED_SERVER) {
        const size_t extra_count = requested_count - pool_count;
        const double jitter = AUGMENTED_SERVER_JITTER_DEG > 0.0
            ? AUGMENTED_SERVER_JITTER_DEG
            : 0.002;
        PHASE_LOG << "[phase] Server augmented append start requested_extra=" << extra_count
                  << " base_pool=" << pool_count
                  << " jitter=" << jitter
                  << "\n" << std::flush;

        append_augmented_server_points(
            *source_ids,
            *source_imgs,
            active_indices,
            extra_count,
            rng,
            server_ids,
            server_imgs
        );

        PHASE_LOG << "[phase] Server augmented append done selected=" << server_imgs.size()
                  << "\n" << std::flush;
    } else if (requested_count > pool_count && APPEND_RANDOM_SERVER) {
        const size_t extra_count = requested_count - pool_count;
        PHASE_LOG << "[phase] Server random append start requested_extra=" << extra_count
                  << " path=" << APPEND_RANDOM_SERVER_PATH << "\n" << std::flush;

        if (!random_append_initialized || cached_random_append_path != APPEND_RANDOM_SERVER_PATH) {
            load_pool(APPEND_RANDOM_SERVER_PATH, "random:", random_append_ids, random_append_imgs);
            cached_random_append_path = APPEND_RANDOM_SERVER_PATH;
            random_append_initialized = true;
        }

        const size_t random_pool_count = random_append_ids.size();
        std::vector<size_t> random_indices(random_pool_count);
        std::iota(random_indices.begin(), random_indices.end(), 0);
        dataset_shuffle(random_indices, rng);

        const size_t random_unique_count = std::min(extra_count, random_pool_count);
        for (size_t i = 0; i < random_unique_count; ++i) {
            const size_t idx = random_indices[i];
            server_ids.push_back(random_append_ids[idx]);
            server_imgs.push_back(random_append_imgs[idx]);
        }

        if (extra_count > random_pool_count) {
            const size_t replacement_count = extra_count - random_pool_count;
            std::cout << "[Server sampling] requested_extra=" << extra_count
                      << " random_pool=" << random_pool_count
                      << " using replacement for random extra=" << replacement_count
                      << "\n";

            std::uniform_int_distribution<size_t> dist(0, random_pool_count - 1);
            for (size_t i = 0; i < replacement_count; ++i) {
                const size_t idx = dist(rng);
                server_ids.push_back(random_append_ids[idx]);
                server_imgs.push_back(random_append_imgs[idx]);
            }
        }

        PHASE_LOG << "[phase] Server random append done selected=" << server_imgs.size()
                  << "\n" << std::flush;
    } else if (requested_count > pool_count) {
        const size_t extra_count = requested_count - pool_count;
        std::cout << "[Server sampling] requested=" << requested_count
                  << " pool=" << pool_count
                  << " using replacement for extra=" << extra_count
                  << "\n";

        std::uniform_int_distribution<size_t> dist(0, pool_count - 1);
        for (size_t i = 0; i < extra_count; ++i) {
            const size_t idx = active_indices[dist(rng)];
            server_ids.push_back((*source_ids)[idx]);
            server_imgs.push_back((*source_imgs)[idx]);
        }
    }

    PHASE_LOG << "[phase] Server sampling done selected=" << server_imgs.size() << "\n" << std::flush;
}

// --------------------------------------------------
// Load gowalla_client.json and sample client_size images
// gowalla_client.json format:
// {
//   "test_id": {
//      "close": [{"train_id":..., "distance":..., "pixels":[...]}, ...],
//      "far":   [{"train_id":..., "distance":..., "pixels":[...]}, ...]
//   },
//   ...
// }
//
// Logic:
// 1) Uses the *same test_ids* as Server (server_ids)
// 2) For each test_id, picks 1 close + 1 far until client_size reached
// 3) Returns client_imgs, and client_is_close (true/false), and client_relates_to (qid)
// --------------------------------------------------
double compute_avg_close_far_gap(
    const std::vector<std::vector<int>>& server,
    const std::vector<std::string>& server_ids,
    const std::vector<std::vector<int>>& client,
    const std::vector<std::string>& client_relates_to,
    const std::vector<bool>& client_is_close
) {
    // Build server_id -> index map (for CLOSE)
    std::unordered_map<std::string, size_t> server_index;
    server_index.reserve(server_ids.size());
    for (size_t i = 0; i < server_ids.size(); ++i) {
        server_index[server_ids[i]] = i;
    }

    double sum_close = 0.0, sum_far = 0.0;
    size_t count_close = 0, count_far = 0;

    for (size_t i = 0; i < client.size(); ++i) {
        if (client_is_close[i]) {
            // CLOSE: distance to its related Server (must exist)
            auto it = server_index.find(client_relates_to[i]);
            if (it == server_index.end()) continue;

            size_t a_idx = it->second;
            double d = l2_dist(client[i], server[a_idx]);
            sum_close += d;
            count_close++;
        } else {
            // FAR: distance = min_{a in Server} L2(client[i], a)
            double best = std::numeric_limits<double>::infinity();
            for (size_t a = 0; a < server.size(); ++a) {
                double d = l2_dist(client[i], server[a]);
                if (d < best) best = d;
            }
            if (std::isfinite(best)) {
                sum_far += best;
                count_far++;
            }
        }
    }

    double avg_close = (count_close > 0) ? sum_close / count_close : 0.0;
    double avg_far   = (count_far > 0)   ? sum_far   / count_far   : 0.0;

    double gap = std::abs(avg_close - avg_far);

    std::cout << "\n[Distance stats]"
              << "\n  avg_close = " << avg_close
              << "\n  avg_far   = " << avg_far
              << "\n  |avg(close) - avg(far)| = " << gap
              << "\n";

    return gap;
}

int clamp255(int x) {
    return x < 0 ? 0 : (x > 255 ? 255 : x);
    //return x % 256;
}

std::vector<int> add_noise_range(
    const std::vector<int>& img,
    std::mt19937& rng,
    int low,
    int high
) {
    std::uniform_int_distribution<int> dist(low, high);
    std::vector<int> out = img;

    for (int& px : out) {
        if (px != 0) {
            px = clamp255(px - dist(rng));
        }
    }
    return out;
}



void overwrite_client_with_conditioned_noise(
    std::vector<std::vector<int>>& client,
    const std::vector<bool>& client_is_close,
    std::mt19937& rng,
    int close_low,
    int close_high,
    int far_low,
    int far_high
) {
    //cout << "****** ADDING CONDITIONED NOISE TO CLIENT (NO OVERWRITE) ****" << endl;

    for (size_t i = 0; i < client.size(); ++i) {

        if (i >= client_is_close.size())
            continue;

        if (!client_is_close[i]) {
            // Large noise applied to Client's own image
            client[i] = add_noise_range(
                client[i], rng,
                far_low, far_high
            );
        }
        else{
             client[i] = add_noise_range(
                client[i], rng,
                close_low, close_high
            );
        }
    }
}           




// assumes you already have:
// using json = nlohmann::json;
// std::string get_obj_id(const json& obj);
// std::vector<int> pixels_to_vec(const json& pix);
// double l2_dist(const std::vector<int>& a, const std::vector<int>& b);

void load_client_from_json(
    int N,
    std::mt19937& rng,
    std::vector<std::vector<double>>& client_imgs,
    std::vector<bool>& client_is_close  
) {
    // ------------------------
    // STATIC CACHE: load client from json once and create close/far pools
    // than resample for each run
    // ------------------------
    static std::vector<std::vector<double>> close_pool;
    static std::vector<std::vector<double>> far_pool;
    static std::string cached_close_path;
    static std::string cached_far_path;
    static bool initialized = false;

    if (!initialized || cached_close_path != CLIENT_CLOSE_PATH || cached_far_path != CLIENT_FAR_PATH) {
        std::cout << "Building close/far pools for Client...\n";
        std::cout << "Client close path: " << CLIENT_CLOSE_PATH << "\n";
        std::cout << "Client far path: " << CLIENT_FAR_PATH << "\n";

        close_pool.clear();
        far_pool.clear();

        json j_close, j_far;

        {
            std::ifstream in(CLIENT_CLOSE_PATH);
            if (!in) throw std::runtime_error("Cannot open " + CLIENT_CLOSE_PATH);
            in >> j_close;
        }

        {
            std::ifstream in(CLIENT_FAR_PATH);
            if (!in) throw std::runtime_error("Cannot open " + CLIENT_FAR_PATH);
            in >> j_far;
        }

        // ------------------------
        // BUILD CLOSE POOL
        // ------------------------
        for (auto it = j_close.begin(); it != j_close.end(); ++it) {

            const json& entry = it.value().at("close");

            for (const auto& nn : entry) {

                const json& pix = nn.at("pixels");

                std::vector<double> img;
                img.reserve(pix.size());

                for (const auto& v : pix)
                    img.push_back((v.get<double>()));

                close_pool.push_back(std::move(img));
            }
        }

        auto add_far_entry = [&](const json& entry) {
            const json& pix = entry.at("pixels");

            std::vector<double> img;
            img.reserve(pix.size());

            for (const auto& v : pix)
                img.push_back((v.get<double>()))    ;

            far_pool.push_back(std::move(img));
        };

        // ------------------------
        // BUILD FAR POOL
        // ------------------------
        for (auto it = j_far.begin(); it != j_far.end(); ++it) {
            const json& value = it.value();
            if (value.is_array()) {
                for (const auto& entry : value) {
                    add_far_entry(entry);
                }
            } else {
                add_far_entry(value);
            }
        }

        if (close_pool.empty() || far_pool.empty())
            throw std::runtime_error("Empty pools");

        std::cout << "close_pool size: " << close_pool.size() << "\n";
        std::cout << "far_pool size: " << far_pool.size() << "\n";

        cached_close_path = CLIENT_CLOSE_PATH;
        cached_far_path = CLIENT_FAR_PATH;
        initialized = true;
    }


        // ------------------------
        // SAMPLING (without replacement)
        // ------------------------
        int target_close = N/2;
        int target_far   = N/2;

        if (target_close > (int)close_pool.size() || target_far > (int)far_pool.size()) {
            throw std::runtime_error("Not enough elements in pools to sample without replacement");
        }

        client_imgs.clear();
        client_is_close.clear();

        // ---- CLOSE ----
        std::vector<int> close_indices(close_pool.size());
        std::iota(close_indices.begin(), close_indices.end(), 0);
        dataset_shuffle(close_indices, rng);

        for (int i = 0; i < target_close; i++) {
            int idx = close_indices[i];
            client_imgs.push_back(close_pool[idx]);
            client_is_close.push_back(true);
        }

        // ---- FAR ----
        std::vector<int> far_indices(far_pool.size());
        std::iota(far_indices.begin(), far_indices.end(), 0);
        dataset_shuffle(far_indices, rng);

        for (int i = 0; i < target_far; i++) {
            int idx = far_indices[i];
            client_imgs.push_back(far_pool[idx]);
            client_is_close.push_back(false);
        }
}

void load_client_from_json_for_server(
    const std::vector<std::string>& server_ids,
    const std::vector<std::vector<double>>& server_imgs,
    int N,
    std::mt19937& rng,
    std::vector<std::vector<double>>& client_imgs,
    std::vector<bool>& client_is_close
) {
    const size_t expected_dim = server_imgs.empty() ? 0 : server_imgs.front().size();
    for (size_t i = 0; i < server_imgs.size(); ++i) {
        if (server_imgs[i].size() != expected_dim) {
            throw std::runtime_error(
                "Server point dimension mismatch at sampled index " + std::to_string(i) +
                ": expected " + std::to_string(expected_dim) +
                ", got " + std::to_string(server_imgs[i].size())
            );
        }
    }

    if (RAW_CLIENT_SPLIT) {
        (void)server_ids;

        if (N < 0) {
            throw std::runtime_error("Client size must be nonnegative");
        }
        if (N == 0) {
            client_imgs.clear();
            client_is_close.clear();
            PHASE_LOG << "[phase] Raw Client split sampling skipped for requested=0\n" << std::flush;
            return;
        }

        static std::vector<std::vector<double>> raw_client_pool;
        static std::string cached_raw_client_path;
        static bool initialized = false;

        if (!initialized || cached_raw_client_path != RAW_CLIENT_PATH) {
            PHASE_LOG << "[phase] Raw Client JSON load start path=" << RAW_CLIENT_PATH << "\n" << std::flush;
            std::ifstream in(RAW_CLIENT_PATH);
            if (!in) {
                throw std::runtime_error("Cannot open " + RAW_CLIENT_PATH);
            }

            json j;
            in >> j;
            if (!j.is_object()) {
                throw std::runtime_error(RAW_CLIENT_PATH + " must be {id: pixels}");
            }

            raw_client_pool.clear();
            raw_client_pool.reserve(j.size());
            size_t filtered_by_dim = 0;
            for (auto it = j.begin(); it != j.end(); ++it) {
                const json& pix = it.value();
                if (!pix.is_array()) {
                    throw std::runtime_error("raw_client[" + it.key() + "] must be array");
                }

                std::vector<double> img;
                img.reserve(pix.size());
                for (const auto& v : pix) {
                    img.push_back(v.get<double>());
                }
                if (expected_dim != 0 && img.size() != expected_dim) {
                    ++filtered_by_dim;
                    continue;
                }
                raw_client_pool.push_back(std::move(img));
            }

            if (raw_client_pool.empty()) {
                throw std::runtime_error(RAW_CLIENT_PATH + " has no compatible client rows");
            }

            std::cout << "[Raw Client split] pool size: " << raw_client_pool.size()
                      << " filtered_by_dim=" << filtered_by_dim << "\n";
            cached_raw_client_path = RAW_CLIENT_PATH;
            initialized = true;
        }

        auto vector_key = [](const std::vector<double>& values) {
            std::ostringstream out;
            out << std::setprecision(17);
            for (double value : values) {
                out << value << ',';
            }
            return out.str();
        };

        std::unordered_set<std::string> server_vector_keys;
        server_vector_keys.reserve(server_imgs.size() * 2 + 1);
        for (const auto& img : server_imgs) {
            server_vector_keys.insert(vector_key(img));
        }

        client_imgs.clear();
        client_is_close.clear();
        client_imgs.reserve(static_cast<size_t>(N));
        client_is_close.reserve(static_cast<size_t>(N));

        std::vector<size_t> indices(raw_client_pool.size());
        std::iota(indices.begin(), indices.end(), 0);
        dataset_shuffle(indices, rng);

        std::vector<size_t> sampled_indices;
        sampled_indices.reserve(static_cast<size_t>(N));
        if (static_cast<size_t>(N) <= indices.size()) {
            sampled_indices.assign(indices.begin(), indices.begin() + N);
        } else {
            sampled_indices = indices;
            std::uniform_int_distribution<size_t> pick(0, raw_client_pool.size() - 1);
            while (sampled_indices.size() < static_cast<size_t>(N)) {
                sampled_indices.push_back(pick(rng));
            }
        }

        size_t exact_overlap = 0;
        std::unordered_set<std::string> unique_sampled_vectors;
        unique_sampled_vectors.reserve(sampled_indices.size() * 2 + 1);
        for (size_t idx : sampled_indices) {
            const auto& img = raw_client_pool[idx];
            const bool is_close = server_vector_keys.find(vector_key(img)) != server_vector_keys.end();
            if (is_close) {
                ++exact_overlap;
            }
            unique_sampled_vectors.insert(vector_key(img));
            client_imgs.push_back(img);
            client_is_close.push_back(is_close);
        }

        std::cout << "[Raw Client split] sampled=" << sampled_indices.size()
                  << " exact_overlap_with_sampled_server=" << exact_overlap
                  << " non_overlap=" << (sampled_indices.size() - exact_overlap)
                  << " unique_vectors=" << unique_sampled_vectors.size()
                  << " replacement="
                  << (sampled_indices.size() <= indices.size() ? "no" : "yes")
                  << "\n";
        return;
    }

    if (GENERATE_CLIENT_FROM_SERVER) {
        PHASE_LOG << "[phase] Synthetic Client generation start requested=" << N
                  << " server_ids=" << server_ids.size()
                  << " server_imgs=" << server_imgs.size()
                  << " close_radius=" << SYNTHETIC_CLIENT_CLOSE_RADIUS_DEG
                  << "\n" << std::flush;

        if (N < 0) {
            throw std::runtime_error("Client size must be nonnegative");
        }
        if (N > 0 && server_imgs.empty()) {
            throw std::runtime_error("Cannot generate Client close points without sampled Server points");
        }

        constexpr double pi = 3.14159265358979323846;

        const int target_close = N / 2;
        const int target_far = N - target_close;
        const double radius = SYNTHETIC_CLIENT_CLOSE_RADIUS_DEG > 0.0
            ? SYNTHETIC_CLIENT_CLOSE_RADIUS_DEG
            : 0.005;
        const double far_min_radius = SYNTHETIC_CLIENT_FAR_MIN_RADIUS_DEG > radius
            ? SYNTHETIC_CLIENT_FAR_MIN_RADIUS_DEG
            : radius * 2.0;

        std::uniform_int_distribution<size_t> pick_server(
            0,
            server_imgs.empty() ? 0 : server_imgs.size() - 1
        );
        std::uniform_real_distribution<double> unit(0.0, 1.0);

        client_imgs.clear();
        client_is_close.clear();
        client_imgs.reserve(static_cast<size_t>(N));
        client_is_close.reserve(static_cast<size_t>(N));
        double close_dist_sum = 0.0;
        double close_dist_max = 0.0;
        double far_source_dist_min = std::numeric_limits<double>::infinity();
        double far_source_dist_sum = 0.0;

        for (int i = 0; i < target_close; ++i) {
            const auto& server = server_imgs[pick_server(rng)];
            if (server.size() != 2) {
                throw std::runtime_error("Synthetic Client generation expects 2D Server points");
            }

            const double angle = unit(rng) * 2.0 * pi;
            const double dist = radius * std::sqrt(unit(rng));
            std::vector<double> point = {
                server[0] + dist * std::sin(angle),
                server[1] + dist * std::cos(angle)
            };
            close_dist_sum += dist;
            close_dist_max = std::max(close_dist_max, dist);
            client_imgs.push_back(std::move(point));
            client_is_close.push_back(true);
        }

        for (int i = 0; i < target_far; ++i) {
            const auto& server = server_imgs[pick_server(rng)];
            if (server.size() != 2) {
                throw std::runtime_error("Synthetic Client generation expects 2D Server points");
            }

            const double angle = unit(rng) * 2.0 * pi;
            const double dist = far_min_radius * (1.0 + unit(rng));
            client_imgs.push_back({
                server[0] + dist * std::sin(angle),
                server[1] + dist * std::cos(angle)
            });
            far_source_dist_min = std::min(far_source_dist_min, dist);
            far_source_dist_sum += dist;
            client_is_close.push_back(false);
        }

        std::cout << "[Synthetic Client debug]"
                  << " close_target_radius=" << radius
                  << " close_avg_dist=" << (target_close ? close_dist_sum / target_close : 0.0)
                  << " close_max_dist=" << close_dist_max
                  << " far_min_radius=" << far_min_radius
                  << " far_avg_source_dist=" << (target_far ? far_source_dist_sum / target_far : 0.0)
                  << " far_min_source_dist="
                  << (target_far ? far_source_dist_min : 0.0)
                  << "\n";

        PHASE_LOG << "[phase] Synthetic Client generation done client=" << client_imgs.size()
                  << " close=" << target_close
                  << " far=" << target_far
                  << "\n" << std::flush;
        return;
    }

    if (N == 0) {
        client_imgs.clear();
        client_is_close.clear();
        PHASE_LOG << "[phase] Client sampling skipped for requested=0\n" << std::flush;
        return;
    }

    struct Cell {
        long long x = 0;
        long long y = 0;

        bool operator==(const Cell& other) const {
            return x == other.x && y == other.y;
        }
    };

    struct CellHash {
        size_t operator()(const Cell& cell) const {
            uint64_t x = static_cast<uint64_t>(cell.x);
            uint64_t y = static_cast<uint64_t>(cell.y);
            x ^= x >> 33;
            x *= 0xff51afd7ed558ccdULL;
            x ^= x >> 33;
            y ^= y >> 33;
            y *= 0xc4ceb9fe1a85ec53ULL;
            y ^= y >> 33;
            return static_cast<size_t>(x ^ (y + 0x9e3779b97f4a7c15ULL + (x << 6) + (x >> 2)));
        }
    };

    static std::vector<std::vector<double>> close_pool;
    static std::vector<std::string> close_row_ids;
    static std::vector<unsigned char> close_is_exact;
    static std::vector<std::vector<double>> far_pool;
    static std::vector<std::string> far_row_ids;
    static std::unordered_map<std::string, std::vector<size_t>> close_by_id;
    static std::string cached_close_path;
    static std::string cached_far_path;
    static bool initialized = false;
    static double max_close_distance = 0.0;
    static size_t exact_close_count = 0;

    if (!initialized || cached_close_path != CLIENT_CLOSE_PATH || cached_far_path != CLIENT_FAR_PATH) {
        PHASE_LOG << "[phase] Client pool build start\n"
                  << "[phase] Client close path=" << CLIENT_CLOSE_PATH << "\n"
                  << "[phase] Client far path=" << CLIENT_FAR_PATH << "\n"
                  << std::flush;

        close_pool.clear();
        close_row_ids.clear();
        close_is_exact.clear();
        far_pool.clear();
        far_row_ids.clear();
        close_by_id.clear();
        max_close_distance = 0.0;
        exact_close_count = 0;

        json j_close, j_far;
        size_t close_json_neighbors = 0;
        size_t close_filtered_by_dim = 0;
        size_t far_json_entries = 0;
        size_t far_filtered_by_dim = 0;

        {
            PHASE_LOG << "[phase] Client close JSON load start\n" << std::flush;
            std::ifstream in(CLIENT_CLOSE_PATH);
            if (!in) throw std::runtime_error("Cannot open " + CLIENT_CLOSE_PATH);
            in >> j_close;
            PHASE_LOG << "[phase] Client close JSON parse done entries=" << j_close.size() << "\n" << std::flush;
        }

        {
            PHASE_LOG << "[phase] Client far JSON load start\n" << std::flush;
            std::ifstream in(CLIENT_FAR_PATH);
            if (!in) throw std::runtime_error("Cannot open " + CLIENT_FAR_PATH);
            in >> j_far;
            PHASE_LOG << "[phase] Client far JSON parse done entries=" << j_far.size() << "\n" << std::flush;
        }

        PHASE_LOG << "[phase] Client close pool materialization start\n" << std::flush;
        for (auto it = j_close.begin(); it != j_close.end(); ++it) {
            const json& entry = it.value().at("close");

            for (const auto& nn : entry) {
                ++close_json_neighbors;
                double dist = nn.at("distance").get<double>();
                const bool is_exact_close = dist <= 1e-12;
                if (is_exact_close) {
                    ++exact_close_count;
                }
                if (!is_exact_close) {
                    max_close_distance = std::max(max_close_distance, dist);
                }

                const json& pix = nn.at("pixels");
                std::vector<double> img;
                img.reserve(pix.size());
                for (const auto& v : pix) {
                    img.push_back(v.get<double>());
                }
                if (expected_dim != 0 && img.size() != expected_dim) {
                    ++close_filtered_by_dim;
                    continue;
                }

                const size_t pos = close_pool.size();
                close_pool.push_back(std::move(img));
                close_row_ids.push_back(it.key());
                close_is_exact.push_back(is_exact_close ? 1 : 0);
                close_by_id[it.key()].push_back(pos);
            }
        }

        auto add_far_entry = [&](const std::string& row_id, const json& entry) {
            ++far_json_entries;
            const json& pix = entry.at("pixels");
            std::vector<double> img;
            img.reserve(pix.size());
            for (const auto& v : pix) {
                img.push_back(v.get<double>());
            }
            if (expected_dim != 0 && img.size() != expected_dim) {
                ++far_filtered_by_dim;
                return;
            }

            far_row_ids.push_back(row_id);
            far_pool.push_back(std::move(img));
        };

        PHASE_LOG << "[phase] Client far pool materialization start\n" << std::flush;
        for (auto it = j_far.begin(); it != j_far.end(); ++it) {
            const json& value = it.value();
            if (value.is_array()) {
                for (const auto& entry : value) {
                    add_far_entry(it.key(), entry);
                }
            } else {
                add_far_entry(it.key(), value);
            }
        }

        if (close_pool.empty() || far_pool.empty()) {
            throw std::runtime_error("Empty Client pools");
        }

        PHASE_LOG << "[phase] Client pool build done"
                  << " close_pool=" << close_pool.size()
                  << " far_pool=" << far_pool.size()
                  << "\n" << std::flush;
        std::cout << "close_pool size: " << close_pool.size() << "\n";
        std::cout << "[Client sampling debug] close JSON neighbors=" << close_json_neighbors
                  << " exact_kept=" << exact_close_count
                  << " filtered_by_dim=" << close_filtered_by_dim
                  << "\n";
        std::cout << "far_pool size: " << far_pool.size() << "\n";
        std::cout << "[Client sampling debug] far JSON entries=" << far_json_entries
                  << " filtered_by_dim=" << far_filtered_by_dim
                  << "\n";

        cached_close_path = CLIENT_CLOSE_PATH;
        cached_far_path = CLIENT_FAR_PATH;
        initialized = true;
    }

    PHASE_LOG << "[phase] Client sampling start requested=" << N
              << " server_ids=" << server_ids.size()
              << " server_imgs=" << server_imgs.size()
              << "\n" << std::flush;

    const int target_close = N / 2;
    const int target_far = N - target_close;

    client_imgs.clear();
    client_is_close.clear();
    client_imgs.reserve(N);
    client_is_close.reserve(N);

    std::vector<unsigned char> active_close_mask(close_pool.size(), 1);
    std::vector<unsigned char> active_far_mask(far_pool.size(), 1);
    if (CALIBRATION_HOLDOUT) {
        active_close_mask = build_holdout_split_active_mask(
            close_row_ids,
            CALIBRATION_HOLDOUT_USE_HELD_OUT
        );
        active_far_mask = build_holdout_split_active_mask(
            far_row_ids,
            CALIBRATION_HOLDOUT_USE_HELD_OUT
        );
        const size_t active_close_count = static_cast<size_t>(
            std::count(active_close_mask.begin(), active_close_mask.end(), 1)
        );
        const size_t active_far_count = static_cast<size_t>(
            std::count(active_far_mask.begin(), active_far_mask.end(), 1)
        );
        std::cout << "[Client holdout split]"
                  << " close_total=" << close_pool.size()
                  << " far_total=" << far_pool.size()
                  << " active_close=" << active_close_count
                  << " active_far=" << active_far_count
                  << " using="
                  << (CALIBRATION_HOLDOUT_USE_HELD_OUT ? "holdout" : "train")
                  << "\n";
    }

    std::vector<size_t> close_candidates;

    const size_t dim = server_imgs.empty() ? 0 : server_imgs.front().size();
    if (dim == 2) {
        double radius = max_close_distance;
        if (!(radius > 0.0)) {
            radius = 1e-12;
        }

        const double radius_sq = radius * radius;
        const double min_close_dist_sq = 1e-24;
        const double cell_size = radius;
        std::unordered_map<Cell, std::vector<size_t>, CellHash> close_grid;
        close_grid.reserve(close_pool.size());

        auto cell_for = [cell_size](const std::vector<double>& p) {
            return Cell{
                static_cast<long long>(std::floor(p[0] / cell_size)),
                static_cast<long long>(std::floor(p[1] / cell_size))
            };
        };

        for (size_t i = 0; i < close_pool.size(); ++i) {
            if (!active_close_mask[i]) continue;
            close_grid[cell_for(close_pool[i])].push_back(i);
        }

        std::vector<unsigned char> selected(close_pool.size(), 0);

        std::vector<size_t> server_order(server_imgs.size());
        std::iota(server_order.begin(), server_order.end(), 0);
        dataset_shuffle(server_order, rng);

        auto collect_for_server = [&](const std::vector<double>& server,
                                     size_t limit,
                                     bool allow_identical) {
            if (server.size() != 2) return;

            Cell base = cell_for(server);
            std::vector<size_t> local_candidates;
            for (long long dx = -1; dx <= 1; ++dx) {
                for (long long dy = -1; dy <= 1; ++dy) {
                    auto found = close_grid.find(Cell{base.x + dx, base.y + dy});
                    if (found == close_grid.end()) continue;

                    for (size_t idx : found->second) {
                        if (!active_close_mask[idx]) continue;
                        if (selected[idx]) continue;

                        const auto& candidate = close_pool[idx];
                        const double d0 = candidate[0] - server[0];
                        const double d1 = candidate[1] - server[1];
                        const double dist_sq = d0 * d0 + d1 * d1;
                        if (dist_sq <= radius_sq &&
                            (allow_identical || dist_sq > min_close_dist_sq)) {
                            local_candidates.push_back(idx);
                        }
                    }
                }
            }

            dataset_shuffle(local_candidates, rng);
            size_t added = 0;
            for (size_t candidate : local_candidates) {
                if (selected[candidate]) continue;
                selected[candidate] = 1;
                close_candidates.push_back(candidate);
                ++added;
                if (added >= limit ||
                    static_cast<int>(close_candidates.size()) >= target_close) {
                    break;
                }
            }
        };

        const size_t probe_server_count = std::min(
            server_order.size(),
            std::max<size_t>(static_cast<size_t>(target_close) * 10, static_cast<size_t>(target_close))
        );

        for (size_t order_idx = 0; order_idx < probe_server_count; ++order_idx) {
            size_t server_idx = server_order[order_idx];
            collect_for_server(server_imgs[server_idx], 1, false);
        }

        if (static_cast<int>(close_candidates.size()) < target_close) {
            for (size_t server_idx : server_order) {
                collect_for_server(server_imgs[server_idx], static_cast<size_t>(target_close), false);
                if (static_cast<int>(close_candidates.size()) >= target_close) {
                    break;
                }
            }
        }

        if (static_cast<int>(close_candidates.size()) < target_close) {
            for (size_t server_idx : server_order) {
                collect_for_server(server_imgs[server_idx], static_cast<size_t>(target_close), true);
                if (static_cast<int>(close_candidates.size()) >= target_close) {
                    break;
                }
            }
        }

        std::cout << "[Client sampling] Server-conditioned close candidates="
                  << close_candidates.size()
                  << " radius=" << radius
                  << " mode=spatial\n";
    } else {
        std::unordered_set<size_t> selected;
        auto collect_by_id = [&](bool allow_identical) {
            for (const auto& server_id : server_ids) {
                auto found = close_by_id.find(server_id);
                if (found == close_by_id.end()) continue;
                for (size_t idx : found->second) {
                    if (!active_close_mask[idx]) continue;
                    const bool is_exact = idx < close_is_exact.size() && close_is_exact[idx];
                    if (!allow_identical && is_exact) continue;
                    if (selected.insert(idx).second) {
                        close_candidates.push_back(idx);
                    }
                    if (static_cast<int>(close_candidates.size()) >= target_close) {
                        return;
                    }
                }
            }
        };

        collect_by_id(false);
        if (static_cast<int>(close_candidates.size()) < target_close) {
            collect_by_id(true);
        }

        std::cout << "[Client sampling] Server-conditioned close candidates="
                  << close_candidates.size()
                  << " mode=id\n";
    }

    if (close_candidates.empty()) {
        throw std::runtime_error(
            "No Client-close candidates match the sampled Server set; "
            "increase |Server| or regenerate Client close data"
        );
    }
    if (far_pool.empty()) {
        throw std::runtime_error("No Client-far candidates available");
    }
    if (std::none_of(active_far_mask.begin(), active_far_mask.end(), [](unsigned char active) {
            return active != 0;
        })) {
        throw std::runtime_error("No Client-far candidates available after holdout split");
    }

    auto vector_key = [](const std::vector<double>& values) {
        std::ostringstream out;
        out << std::setprecision(17);
        for (double value : values) {
            out << value << ',';
        }
        return out.str();
    };

    std::unordered_set<std::string> unique_close_candidate_vectors;
    unique_close_candidate_vectors.reserve(close_candidates.size());
    for (size_t idx : close_candidates) {
        unique_close_candidate_vectors.insert(vector_key(close_pool[idx]));
    }

    std::cout << "[Client sampling debug] target_close=" << target_close
              << " target_far=" << target_far
              << " close_candidate_indices=" << close_candidates.size()
              << " close_candidate_unique_vectors="
              << unique_close_candidate_vectors.size() << "\n";

    dataset_shuffle(close_candidates, rng);
    std::vector<size_t> sampled_close_indices;
    sampled_close_indices.reserve(target_close);

    if (target_close <= static_cast<int>(close_candidates.size())) {
        for (int i = 0; i < target_close; ++i) {
            size_t idx = close_candidates[i];
            sampled_close_indices.push_back(idx);
            client_imgs.push_back(close_pool[idx]);
            client_is_close.push_back(true);
        }
    } else {
        std::cout << "[Client sampling warning] only " << close_candidates.size()
                  << " unique Server-conditioned close candidates for target "
                  << target_close << "; sampling close with replacement\n";
        for (size_t idx : close_candidates) {
            sampled_close_indices.push_back(idx);
            client_imgs.push_back(close_pool[idx]);
            client_is_close.push_back(true);
        }

        std::uniform_int_distribution<size_t> pick_close(0, close_candidates.size() - 1);
        while (static_cast<int>(client_imgs.size()) < target_close) {
            size_t idx = close_candidates[pick_close(rng)];
            sampled_close_indices.push_back(idx);
            client_imgs.push_back(close_pool[idx]);
            client_is_close.push_back(true);
        }
    }

    std::vector<size_t> far_indices;
    far_indices.reserve(far_pool.size());
    for (size_t i = 0; i < far_pool.size(); ++i) {
        if (active_far_mask[i]) {
            far_indices.push_back(i);
        }
    }
    dataset_shuffle(far_indices, rng);
    std::vector<size_t> sampled_far_indices;
    sampled_far_indices.reserve(target_far);

    if (target_far <= static_cast<int>(far_indices.size())) {
        for (int i = 0; i < target_far; ++i) {
            size_t idx = far_indices[i];
            sampled_far_indices.push_back(idx);
            client_imgs.push_back(far_pool[idx]);
            client_is_close.push_back(false);
        }
    } else {
        std::cout << "[Client sampling warning] only " << far_indices.size()
                  << " unique far candidates for target "
                  << target_far << "; sampling far with replacement\n";
        for (size_t idx : far_indices) {
            sampled_far_indices.push_back(idx);
            client_imgs.push_back(far_pool[idx]);
            client_is_close.push_back(false);
        }

        std::uniform_int_distribution<size_t> pick_far(0, far_indices.size() - 1);
        while (static_cast<int>(client_imgs.size()) < N) {
            size_t idx = far_indices[pick_far(rng)];
            sampled_far_indices.push_back(idx);
            client_imgs.push_back(far_pool[idx]);
            client_is_close.push_back(false);
        }
    }

    std::unordered_set<size_t> unique_sampled_close_indices(sampled_close_indices.begin(),
                                                            sampled_close_indices.end());
    std::unordered_set<size_t> unique_sampled_far_indices(sampled_far_indices.begin(),
                                                          sampled_far_indices.end());
    std::unordered_set<std::string> unique_sampled_close_vectors;
    std::unordered_set<std::string> unique_sampled_far_vectors;
    unique_sampled_close_vectors.reserve(sampled_close_indices.size());
    unique_sampled_far_vectors.reserve(sampled_far_indices.size());
    for (size_t idx : sampled_close_indices) {
        unique_sampled_close_vectors.insert(vector_key(close_pool[idx]));
    }
    for (size_t idx : sampled_far_indices) {
        unique_sampled_far_vectors.insert(vector_key(far_pool[idx]));
    }

    std::cout << "[Client sampling debug] final close sampled=" << sampled_close_indices.size()
              << " unique_indices=" << unique_sampled_close_indices.size()
              << " unique_vectors=" << unique_sampled_close_vectors.size()
              << " replacement="
              << (sampled_close_indices.size() == unique_sampled_close_indices.size() ? "no" : "yes")
              << "\n";
    std::cout << "[Client sampling debug] final far sampled=" << sampled_far_indices.size()
              << " unique_indices=" << unique_sampled_far_indices.size()
              << " unique_vectors=" << unique_sampled_far_vectors.size()
              << " replacement="
              << (sampled_far_indices.size() == unique_sampled_far_indices.size() ? "no" : "yes")
              << "\n";
    PHASE_LOG << "[phase] Client sampling done client=" << client_imgs.size()
              << " labels=" << client_is_close.size()
              << "\n" << std::flush;
}
/*
void load_client_from_json(
    const std::string& client_path,
    const std::vector<std::string>& server_ids,
    int client_size,
    std::mt19937& rng,
    std::vector<std::vector<int>>& client_imgs,
    std::vector<bool>& client_is_close,
    std::vector<std::string>& client_relates_to
) {
    std::ifstream in; // only used when (re)loading cache

    static std::string cached_path;
    static json cached_j;

    if (cached_path != client_path) {
        in.open(client_path);
        if (!in) throw std::runtime_error("Cannot open " + client_path);

        in >> cached_j;

        if (!cached_j.is_object())
            throw std::runtime_error("gowalla_client.json must be a JSON object keyed by test_id");

        cached_path = client_path;
    }

    const json& j = cached_j;
    if (!j.is_object())
        throw std::runtime_error("gowalla_client.json must be a JSON object keyed by test_id");

    client_imgs.clear();
    client_is_close.clear();
    client_relates_to.clear();
    client_imgs.reserve(client_size);
    client_is_close.reserve(client_size);
    client_relates_to.reserve(client_size);

    int target_close = client_size / 2;
    int target_far = client_size - target_close;
    int have_close = 0, have_far = 0;

    std::vector<std::string> ids = server_ids;
    dataset_shuffle(ids, rng);
    if (ids.empty()) throw std::runtime_error("server_ids is empty; cannot build Client");

    auto pick_from_list = [&](const json& lst) -> std::vector<int> {
        std::uniform_int_distribution<size_t> pick(0, lst.size() - 1);
        const json& obj = lst.at(pick(rng));
        const json& pix = obj.at("pixels");

        std::vector<int> img;
        img.reserve(pix.size());
        for (const auto& v : pix) img.push_back((int)std::lround(v.get<double>()));
        return img;
    };

    size_t idx = 0;
    while ((int)client_imgs.size() < client_size) {
        const std::string& qid = ids[idx % ids.size()];
        idx++;

        if (!j.contains(qid)) {
            throw std::runtime_error(
                "gowalla_client.json missing test_id " + qid + " (did you export enough queries?)"
            );
        }

        const json& entry = j.at(qid);
        const json& close_list = entry.at("close");
        const json& far_list = entry.at("far");

        if (!close_list.is_array() || close_list.empty())
            throw std::runtime_error("gowalla_client.json[" + qid + "].close empty");
        if (!far_list.is_array() || far_list.empty())
            throw std::runtime_error("gowalla_client.json[" + qid + "].far empty");

        // Add close if needed
        if (have_close < target_close && (int)client_imgs.size() < client_size) {
            client_imgs.push_back(pick_from_list(close_list));
            client_is_close.push_back(true);
            client_relates_to.push_back(qid);
            have_close++;
        }

        // Add far if needed
        if (have_far < target_far && (int)client_imgs.size() < client_size) {
            client_imgs.push_back(pick_from_list(far_list));
            client_is_close.push_back(false);
            client_relates_to.push_back(qid);
            have_far++;
        }

        // If one side is exhausted, fill with the other (keeping relates_to aligned)
        while (have_far < target_far && have_close >= target_close && (int)client_imgs.size() < client_size) {
            client_imgs.push_back(pick_from_list(far_list));
            client_is_close.push_back(false);
            client_relates_to.push_back(qid);
            have_far++;
        }
        while (have_close < target_close && have_far >= target_far && (int)client_imgs.size() < client_size) {
            client_imgs.push_back(pick_from_list(close_list));
            client_is_close.push_back(true);
            client_relates_to.push_back(qid);
            have_close++;
        }
    }

    // hard sanity check
    assert(client_imgs.size() == client_is_close.size());
    assert(client_imgs.size() == client_relates_to.size());
}
*/
// --------------------------------------------------
// CLI parsing (unchanged)
// --------------------------------------------------
Config parse(int argc, char* argv[]) {
    if (argc < 2) {
        throw runtime_error(
            "Usage: ./test ..."
        );
    }

    Config cfg;
    cfg.filter_type = argv[1];

    for (int i = 2; i < argc; i++) {
        string arg(argv[i]);

        if (arg.rfind("N=", 0) == 0) {
            cfg.N = stoi(arg.substr(2));
        }
        else if (arg.rfind("num_lshs=", 0) == 0) {
            cfg.num_lshs = stoi(arg.substr(9));
        }
        else if (arg.rfind("D_INNER=", 0) == 0) {
            cfg.d_inner = stoi(arg.substr(8));
        }
        else if (arg.rfind("D_OUT=", 0) == 0) {
            cfg.d_out = stoi(arg.substr(6));
        }
        else {
            throw runtime_error("Unknown argument: " + arg);
        }
    }

    if (cfg.N < 0) {
        throw runtime_error("Missing required parameters");
    }

    return cfg;
}


// ============================================================
// ===================== CLI HELPERS ==========================
// ============================================================

bool has_flag(int argc, char** argv, const string& flag) {
    for (int i = 1; i < argc; i++) {
        if (string(argv[i]) == flag) return true;
    }
    return false;
}

bool get_int_arg(int argc, char** argv, const string& prefix, int& out) {
    for (int i = 1; i < argc; i++) {
        string a = argv[i];
        if (a.rfind(prefix, 0) == 0) {
            const string value = a.substr(prefix.size());
            size_t parsed = 0;
            out = stoi(value, &parsed);
            if (parsed != value.size()) {
                throw invalid_argument("Invalid numeric value for " + prefix + value);
            }
            return true;
        }
    }
    return false;
}

bool get_size_arg(int argc, char** argv, const string& prefix, size_t& out) {
    for (int i = 1; i < argc; i++) {
        string a = argv[i];
        if (a.rfind(prefix, 0) == 0) {
            const string value = a.substr(prefix.size());
            size_t parsed = 0;
            out = static_cast<size_t>(stoull(value, &parsed));
            if (parsed != value.size()) {
                throw invalid_argument("Invalid numeric value for " + prefix + value);
            }
            return true;
        }
    }
    return false;
}

bool get_string_arg(int argc, char** argv, const std::string& prefix, std::string& out) {
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a.rfind(prefix, 0) == 0) {
            out = a.substr(prefix.size());
            return true;
        }
    }
    return false;
}

bool get_double_arg(int argc, char** argv, const string& prefix, double& out) {
    for (int i = 1; i < argc; i++) {
        string a = argv[i];
        if (a.rfind(prefix, 0) == 0) {
            const string value = a.substr(prefix.size());
            size_t parsed = 0;
            out = stod(value, &parsed);
            if (parsed != value.size()) {
                throw invalid_argument("Invalid numeric value for " + prefix + value);
            }
            return true;
        }
    }
    return false;
}

// Remove custom flags before passing to your parse(argc,argv) (Config cfg = parse(...))
vector<char*> strip_custom_args(int argc, char** argv) {
    vector<char*> clean;
    clean.push_back(argv[0]);

    for (int i = 1; i < argc; i++) {
        string a = argv[i];

        // mode/params
        if (a == "--GRID_SEARCH" || a == "--grid_search") continue;
        if (a.rfind("--L=", 0) == 0) continue;
        if (a.rfind("--k=", 0) == 0) continue;
        if (a.rfind("--w=", 0) == 0) continue;

        // noise globals
        if (a == "--CLOSE_NOISE") continue;
        if (a.rfind("--CLOSE_NOISE_START=", 0) == 0) continue;
        if (a.rfind("--CLOSE_NOISE_END=", 0) == 0) continue;
        if (a.rfind("--FAR_NOISE_START=", 0) == 0) continue;
        if (a.rfind("--FAR_NOISE_END=", 0) == 0) continue;

        clean.push_back(argv[i]);
    }

    return clean;
}
