#pragma once

#include <vector>
#include <random>
#include <string>
#include <cstddef>

extern bool DEBUG_PHASES;
extern bool CALIBRATION_HOLDOUT;
extern bool CALIBRATION_HOLDOUT_USE_HELD_OUT;
extern bool GENERATE_CLIENT_FROM_SERVER;
extern bool RAW_CLIENT_SPLIT;
extern bool APPEND_RANDOM_SERVER;
extern bool APPEND_AUGMENTED_SERVER;
extern bool APPEND_DISTRIBUTION_SERVER;
extern bool PORTABLE_DATASET_SAMPLING;
extern double SYNTHETIC_CLIENT_CLOSE_RADIUS_DEG;
extern double SYNTHETIC_CLIENT_FAR_MIN_RADIUS_DEG;
extern double AUGMENTED_SERVER_JITTER_DEG;
extern double DISTRIBUTION_SERVER_NOISE_SCALE;
extern std::string APPEND_RANDOM_SERVER_PATH;
extern std::string CLIENT_CLOSE_PATH;
extern std::string CLIENT_FAR_PATH;
extern std::string RAW_CLIENT_PATH;
#define PHASE_LOG if (!DEBUG_PHASES) {} else std::cout
using std::vector;
using std::string;
using std::mt19937;



bool has_flag(int argc, char** argv, const std::string& flag);
bool get_int_arg(int argc, char** argv, const std::string& prefix, int& out);
bool get_size_arg(int argc, char** argv, const std::string& prefix, size_t& out);
bool get_double_arg(int argc, char** argv, const std::string& prefix, double& out);
std::vector<char*> strip_custom_args(int argc, char** argv);
/*
 * Clamp pixel to [0,255]
 */
int clamp255(int x);

/*
 * Add uniform noise in [low, high] to every non-zero pixel.
 * Returns new image.
 */
std::vector<int> add_noise_range(
    const std::vector<int>& img,
    std::mt19937& rng,
    int low,
    int high
);

/*
 * For each Client image:
 *   - find related Server image
 *   - if client_is_close[i] -> add small noise
 *   - else -> add large noise
 */
void overwrite_client_with_conditioned_noise(
    std::vector<std::vector<int>>& client,
    const std::vector<bool>& client_is_close,
    std::mt19937& rng,
    int close_low,
    int close_high,
    int far_low,
    int far_high
);


std::vector<int> fake_lsh_keys(
    const std::vector<int>& img,
    int num_keys,
    std::mt19937& rng
);


std::vector<std::vector<int>> make_client_from_server(
    const std::vector<std::vector<int>>& server,
    std::mt19937& rng,
    int noise_max_inclusive = 5
);
double l2_dist(const std::vector<int>& a, const std::vector<int>& b);
void print_single_image(const std::vector<int>& v);
void debug_print_dataset(
    const std::vector<std::string>& server_ids,
    const std::vector<std::vector<int>>& server,
    const std::vector<std::vector<int>>& client,
    const std::vector<bool>& client_is_close,
    const std::vector<std::string>& client_relates_to,
    size_t maxn = 10
);

double compute_avg_close_far_gap(
    const std::vector<std::vector<int>>& server,
    const std::vector<std::string>& server_ids,
    const std::vector<std::vector<int>>& client,
    const std::vector<std::string>& client_relates_to,
    const std::vector<bool>& client_is_close
);

void print_images(
    const std::string& name,
    const std::vector<std::vector<int>>& imgs,
    size_t max_imgs = 3,
    size_t max_pix = 60
);
void load_server_from_json(
    const std::string& server_path,
    int N,
    std::mt19937& rng,
    std::vector<std::string>& server_ids,
    std::vector<std::vector<double>>& server_imgs
);
size_t count_server_json_entries(const std::string& server_path);

bool get_string_arg(int argc, char** argv, const std::string& prefix, std::string& out);
void load_client_from_json(
    int N,
    std::mt19937& rng,
    std::vector<std::vector<double>>& client_imgs,
    std::vector<bool>& client_is_close);

void load_client_from_json_for_server(
    const std::vector<std::string>& server_ids,
    const std::vector<std::vector<double>>& server_imgs,
    int N,
    std::mt19937& rng,
    std::vector<std::vector<double>>& client_imgs,
    std::vector<bool>& client_is_close);

void load_client_from_json_EDGE_TEST(
    const std::string& client_path,
    const std::vector<std::string>& server_ids,
    int client_size,
    std::mt19937& rng,
    std::vector<std::vector<int>>& client_imgs,
    std::vector<bool>& client_is_close,
    std::vector<std::string>& client_relates_to
);

std::vector<std::vector<int>> create_server(
    int N,
    int DOMAIN_MIN,
    int DOMAIN_MAX,
    std::mt19937& rng
);

std::vector<std::vector<int>> create_client(
    int N,
    int DOMAIN_MIN,
    int DOMAIN_MAX,
    std::mt19937& rng
);

// Command-line configuration
struct Config {
    string filter_type;

    int N = -1;
    int DOMAIN_MIN = -1;
    int DOMAIN_MAX = -1;
    int d_inner = -1;
    int d_out = -1;
    int num_lshs = -1;
};

// Parse CLI arguments
Config parse(int argc, char* argv[]);

int absdiff(int a, int b);
