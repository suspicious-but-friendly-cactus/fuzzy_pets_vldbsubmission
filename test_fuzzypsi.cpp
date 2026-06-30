/*
Runs the FuzzyPSI protocol 
Two modes:
- If --CALIBRATE_LKW is set, calibrate Cuckoo L/k/w over the full Server dataset
  and exit.
- If --L,--k and --w are set, run the experiment with the given parameters.
*/

#include <iostream>
#include <vector>
#include <random>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <fstream>
#include <sstream>
#include <memory>
#include <functional>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <array>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <thread>
#include <mutex>
#include <atomic>
#include <exception>
#include <span>
#include "lsh_e2lsh.h"
#include "hash_xor.h"
#include "bloom_filter.h"
#include "cuckoo_filter.h"
#include "batchpir_row_reader.h"
#include "disco/contact-discovery/psetggm/AES.h"
#include "Utils/utils.h"
#include "disco/contact-discovery/oprf_c/disco_gcaes_oprf_adapter.h"
#include <string>
#include <vector>
#include <stdexcept>
#define PIR_PUNC_NO_DEMO_MAIN
#include "disco/pir_punc_psetggm_port.cpp"

#if __has_include("frodoPIR/client.hpp") && \
    __has_include("frodoPIR/server.hpp") && \
    __has_include("sha3/turboshake128.hpp") && \
    __has_include("randomshake/randomshake.hpp")
#define FUZZY_PETS_HAS_FRODOPIR 1
#include "frodoPIR/client.hpp"
#include "frodoPIR/server.hpp"
#else
#define FUZZY_PETS_HAS_FRODOPIR 0
#endif

#if defined(FUZZY_PETS_ENABLE_OPENFHE) && __has_include("openfhe.h")
#define FUZZY_PETS_HAS_OPENFHE 1
#include "openfhe.h"
#include "ciphertext-ser.h"
#include "cryptocontext-ser.h"
#include "scheme/bfvrns/bfvrns-ser.h"
#else
#define FUZZY_PETS_HAS_OPENFHE 0
#endif
using namespace std;

// Keep this experiment as one translation unit, but split the large implementation
// into focused fragments so templates, globals, and OpenFHE guards stay simple.
#include "test_fuzzypsi_parts/globals_and_utils.inc.cpp"
#include "test_fuzzypsi_parts/pir_readers.inc.cpp"
#include "test_fuzzypsi_parts/oprf_filters.inc.cpp"
#include "test_fuzzypsi_parts/pir_membership.inc.cpp"
#include "test_fuzzypsi_parts/datasets.inc.cpp"
#include "test_fuzzypsi_parts/grid_search.inc.cpp"
#include "test_fuzzypsi_parts/ca_only.inc.cpp"
#include "test_fuzzypsi_parts/experiment_runner.inc.cpp"
#include "test_fuzzypsi_parts/cli_args.inc.cpp"

static void print_usage(const char* program_name) {
    cout
        << "Usage:\n"
        << "  " << program_name << " --filter=Cuckoo --L=5 --k=5 --w=0.5 --server_size=1000 --client_size=1000 --filter_size=3152\n"
        << "  " << program_name << " --filter=Cuckoo --PIR_BatchPIR --OPRF --oprf_mechanism=GCAES --oprf_addr=127.0.0.1:50051 --L=5 --k=5 --w=0.5 --server_size=4500000 --client_size=1000 --filter_size=4718592\n"
        << "\n"
        << "Functionality Flags:\n"
        << "  --filter=Bloom|Cuckoo       Filter family. PIR paths support Bloom and Cuckoo.\n"
        << "  --L --k --w               E2LSH parameters for normal runs.\n"
        << "  --server_size=N --client_size=N     Server and client sample sizes for the run.\n"
        << "  --filter_size=N                 Bloom/Cuckoo filter size\n"
        << "  --cuckoo_pow2_buckets           Round Cuckoo bucket count up to a power of two.\n"
        << "  --num_runs=N                    Number of times to repeat the experiment and get the average\n"
        << "  --db=PRESET                     gowalla, gowalla_calibration, random, random_small, MNIST, FashionMNIST, febrl, nf_bot_iot, acm_dblp\n"
        << "  --use_stored_client_split       Use client_close/client_far JSON instead of generated clients.\n"
        << "\n"
        << "Privacy Flags:\n"
        << "  --PIR_BatchPIR                  Uses BatchPIR.\n"
        << "  --PIR_double                    Uses Disco's two-servers PIR \n"
        << "  --PIR_single                    Uses FrodoPIR single-server \n"
        << "  --OPRF                          Enables.\n"
        << "  --oprf_mechanism=GCAES|ECNR \n"
        << "  --oprf_addr=HOST:PORT           Address for server of GCAES OPRF. Be sure to first spawn a server (see README)\n"
        << "\n"
        << "Experiment Flags:\n"
        << "  --CALIBRATE_LKW                 Calibrate Cuckoo L/k/w and exit. Legacy mode uses all Server entries.\n"
        << "  --calibration_holdout           Reserve 20% of Server for calibration; normal runs use the other 80%.\n"
        << "  --SWEEP_FILTER_SIZE             Find the best filter size for a fixed L/k/w.\n"
        << "  --CA_only                       Cardinality-only Fuzzy PSI. Requires make OpenFHE=1 during build.\n"
        << "\n"
        << "Useful wrappers:\n"
        << "  ./run_cuckoo_server_sweep.sh --help\n                   Runs the experiment on many server and client sizes\n" 
        << "\n";
}
int main(int argc, char* argv[]) {
    if (has_flag(argc, argv, "--help") || has_flag(argc, argv, "-h")) {
        print_usage(argv[0]);
        return 0;
    }
    //Parse arguments
    ParsedCliArgs args;
    try {
        if (!parse_cli_args(argc, argv, args)) {
            return -1;
        }
    } catch (const std::exception& ex) {
        cerr << "Invalid command line: " << ex.what() << "\n";
        return -1;
    }

    if (args.calibrate_lkw) {
        try {
            vector<LkwCandidate> calibration_candidates =
                parse_lkw_candidates(args.calibration_lkw_arg);
            if (calibration_candidates.empty()) {
                calibration_candidates = default_lkw_candidates();
            }
            return run_cuckoo_lkw_calibration_once(
                server_path,
                client_path,
                dim,
                N,
                CLIENT_SIZE_SEARCH,
                filter_size,
                calibration_candidates,
                args.calibration_target_fp,
                args.calibration_target_fn,
                args.calibration_abort_fail_rate,
                args.calibration_abort_fill_ratio,
                args.calibration_abort_min_attempts,
                args.calibration_progress_steps,
                args.calibration_stop_on_first_target,
                args.calibration_holdout
            );
        } catch (const exception& ex) {
            cerr << "Calibration failed: " << ex.what() << "\n";
            return -1;
        }
    }

    if (!args.ca_only && !args.has_all_lsh_params) {
        cerr << "Normal runs require --L=, --k=, and --w=. Use --CALIBRATE_LKW to calibrate parameters.\n";
        return -1;
    }
    REUSE_DATASET_BETWEEN_GRID_AND_TEST = 0;
    cout << "*** USING GIVEN PARAMETERS ***" << endl;

    cout << "\n[best params] L=" << L << " k=" << k << " w=" << w << "\n";
    cout << "[filter config] filter=" << args.filter
         << " filter_size=" << filter_size
         << " sweep_filter_size=" << (args.sweep_filter_size ? "true" : "false");
    if (args.ca_only) {
        cout << " ca_only=true";
        if (CA_COMPOUND_LSH) {
            cout << " ca_compound_lsh=true";
        }
        if (CA_PLAIN_LOCAL_PRF) {
            cout << " ca_plain_local_prf=true";
        }
        cout << " ca_pir=" << active_pir_label()
             << " ca_oprf=" << (CA_USE_OPRF ? "true" : "false")
             << " ca_pir_storage="
             << (CA_DISABLE_OPRF_AND_PIR
                     ? "direct_memory"
                     : ((CA_MATERIALIZE_PIR_DB || USE_PIR_BATCHPIR) ? "memory" : "disk_sharded"))
             << " ca_pir_shard_rows=" << CA_PIR_SHARD_ROWS
             << " ca_pir_chunk_cache_limit=" << CA_PIR_CHUNK_CACHE_LIMIT
             << " ca_bloom_hashes="
             << (CA_BLOOM_HASHES_OVERRIDE > 0
                     ? std::to_string(CA_BLOOM_HASHES_OVERRIDE)
                     : std::string("k_star"));
        if (args.filter == "Cuckoo") {
            cout << " ca_cuckoo_fp_bits=" << CA_CUCKOO_FP_BITS
                 << " ca_cuckoo_bucket_tag_bits=" << CA_CUCKOO_BUCKET_TAG_BITS
                 << " ca_cuckoo_tag_hashes=" << CA_CUCKOO_TAG_HASHES;
        }
    }
    if (args.filter == "Cuckoo") {
        cout << " cuckoo_pow2_buckets="
             << (CUCKOO_POWER_OF_TWO_BUCKETS ? "true" : "false");
    }
    cout << " use_oprf=" << (USE_OPRF ? "true" : "false")
         << " use_batch=" << (USE_BATCH ? "true" : "false");
    if (USE_OPRF) {
        cout << " oprf_mechanism=" << disco_oprf_mechanism()
             << " oprf_addr=" << disco_gcaes_oprf_server_host()
             << ":" << disco_gcaes_oprf_server_port()
             << " server_prf="
             << (OPRF_SERVER_INTERACTIVE
                     ? (std::string("interactive_") + disco_oprf_mechanism() + "_oprf")
                     : (std::string("local_") + disco_oprf_mechanism() + "_server_prf"));
        if (args.filter == "Cuckoo") {
            cout << " cuckoo_oprf="
                 << (CUCKOO_OPRF_SPLIT_OUTPUT
                         ? "one_block_split_bucket_tag"
                         : "two_domain_blocks_bucket_tag");
        }
    }
    cout << "\n";
    cout << "[dataset config] Running with |Server|=" << N
         << " db=" << db_preset
         << " server=" << server_path
         << " raw_client_split=" << (RAW_CLIENT_SPLIT ? "true" : "false")
         << " raw_client=" << (RAW_CLIENT_SPLIT ? RAW_CLIENT_PATH : "(unused)")
         << " client_close=" << CLIENT_CLOSE_PATH
         << " client_far=" << CLIENT_FAR_PATH
         << " generate_client_from_server=" << (GENERATE_CLIENT_FROM_SERVER ? "true" : "false")
         << " append_random_server=" << (APPEND_RANDOM_SERVER ? "true" : "false")
         << " append_augmented_server=" << (APPEND_AUGMENTED_SERVER ? "true" : "false")
         << " append_distribution_server=" << (APPEND_DISTRIBUTION_SERVER ? "true" : "false")
         << " calibration_holdout=" << (CALIBRATION_HOLDOUT ? "true" : "false")
         << " holdout_side="
         << (CALIBRATION_HOLDOUT_USE_HELD_OUT ? "calibration" : "train")
         << " augmented_server_jitter=" << AUGMENTED_SERVER_JITTER_DEG
         << " distribution_server_noise_scale=" << DISTRIBUTION_SERVER_NOISE_SCALE
         << " append_random_server_path=" << APPEND_RANDOM_SERVER_PATH
         << " synthetic_client_close_radius=" << SYNTHETIC_CLIENT_CLOSE_RADIUS_DEG
         << " synthetic_client_far_min_radius=" << SYNTHETIC_CLIENT_FAR_MIN_RADIUS_DEG
         << " debug=" << (DEBUG_PHASES ? "1" : "0")
         << "\n";

    REUSE_DATASET_BETWEEN_GRID_AND_TEST = 0; //in case we want to run the exact same sampling of datasets for parameter optimization and experiment run (less privacy but better utility)
    if (args.ca_only) {
        cout << "-----------------------------" << endl;
        cout << "[CA_only mode] Fuzzy PSI cardinality using "
             << (args.filter == "Cuckoo" ? "Cuckoo filters" : "PiCardSum on Bloom filters")
             << endl;
        const int rc = (args.filter == "Cuckoo")
            ? fuzzypsi_cardinality_only_cuckoo(L, k, w, "CA_ONLY")
            : fuzzypsi_cardinality_only(L, k, w, "CA_ONLY");
        cout << "-----------------------------" << endl;
        return rc;
    }

    if (args.sweep_filter_size) {
        cout << "-----------------------------" << endl;
        for (size_t sweep_filter_size : FILTER_SIZE_SWEEP) {
            filter_size = sweep_filter_size;
            cout << "\n================ FILTER_SIZE=" << filter_size
                 << " FILTER=" << args.filter
                 << " ================\n";
            fuzzypsi(L, k, w, "FILTER SIZE SWEEP", args.filter);
        }
        cout << "-----------------------------" << endl;
        return 0;
    }

    cout << "-----------------------------" << endl;
    cout << "Grid Search found best parameters. Now running FUZZY PSI" << endl;
    fuzzypsi(L, k, w, "DIFFERENT SAMPLE", args.filter);
    cout << "-----------------------------" << endl;
    return 0;
}
