struct ParsedCliArgs {
    std::string filter;

    bool calibrate_lkw = false;
    bool sweep_filter_size = false;
    bool ca_only = false;
    bool has_all_lsh_params = false;

    std::string calibration_lkw_arg;
    double calibration_target_fp = 0.035;
    double calibration_target_fn = 0.015;
    double calibration_abort_fail_rate = 0.02;
    double calibration_abort_fill_ratio = 0.98;
    size_t calibration_abort_min_attempts = 1'000'000;
    size_t calibration_progress_steps = 20;
    bool calibration_stop_on_first_target = true;
    bool calibration_holdout = false;
};

static bool removed_parallel_flag_present(int argc, char* argv[]) {
    return has_flag(argc, argv, "--PARALLEL") ||
           has_flag(argc, argv, "--USE_PARALLEL") ||
           has_flag(argc, argv, "--parallel") ||
           has_flag(argc, argv, "--use_parallel");
}

static bool removed_lowmc_oprf_flag_present(int argc, char* argv[]) {
    return has_flag(argc, argv, "--GCLOWMC_OPRF") ||
           has_flag(argc, argv, "--OPRF_GCLOWMC") ||
           has_flag(argc, argv, "--oprf_gclowmc") ||
           has_flag(argc, argv, "--LOWMC_OPRF") ||
           has_flag(argc, argv, "--OPRF_LOWMC") ||
           has_flag(argc, argv, "--oprf_lowmc");
}

static bool removed_grid_search_flag_present(int argc, char* argv[]) {
    return has_flag(argc, argv, "--GRID_SEARCH") ||
           has_flag(argc, argv, "--grid_search");
}

static bool parse_oprf_config(int argc, char* argv[]) {
    if (removed_lowmc_oprf_flag_present(argc, argv)) {
        cerr << "LOWMC/GCLOWMC OPRF has been removed; use GCAES or ECNR.\n";
        return false;
    }

    USE_OPRF = has_flag(argc, argv, "--OPRF") ||
               has_flag(argc, argv, "--USE_OPRF") ||
               has_flag(argc, argv, "--oprf") ||
               has_flag(argc, argv, "--use_oprf") ||
               has_flag(argc, argv, "--ECNR_OPRF") ||
               has_flag(argc, argv, "--OPRF_ECNR") ||
               has_flag(argc, argv, "--oprf_ecnr") ||
               has_flag(argc, argv, "--GCAES_OPRF") ||
               has_flag(argc, argv, "--OPRF_GCAES") ||
               has_flag(argc, argv, "--oprf_gcaes");

    std::string oprf_mechanism_arg;
    if (get_string_arg(argc, argv, "--oprf_mechanism=", oprf_mechanism_arg) ||
        get_string_arg(argc, argv, "--OPRF_mechanism=", oprf_mechanism_arg) ||
        get_string_arg(argc, argv, "--oprf_type=", oprf_mechanism_arg) ||
        get_string_arg(argc, argv, "--OPRF_type=", oprf_mechanism_arg) ||
        get_string_arg(argc, argv, "--oprf=", oprf_mechanism_arg) ||
        get_string_arg(argc, argv, "--OPRF=", oprf_mechanism_arg)) {
        try {
            USE_OPRF = true;
            disco_oprf_set_mechanism(oprf_mechanism_arg.c_str());
        } catch (const std::exception& ex) {
            cerr << "Invalid OPRF mechanism: " << ex.what() << "\n";
            return false;
        }
    }

    if (has_flag(argc, argv, "--ECNR_OPRF") ||
        has_flag(argc, argv, "--OPRF_ECNR") ||
        has_flag(argc, argv, "--oprf_ecnr")) {
        disco_oprf_set_mechanism("ECNR");
    }
    if (has_flag(argc, argv, "--GCAES_OPRF") ||
        has_flag(argc, argv, "--OPRF_GCAES") ||
        has_flag(argc, argv, "--oprf_gcaes")) {
        disco_oprf_set_mechanism("GCAES");
    }

    const std::string active_oprf_mechanism = disco_oprf_mechanism();
    if (active_oprf_mechanism != "GCAES" && active_oprf_mechanism != "ECNR") {
        cerr << "Invalid OPRF mechanism. Use GCAES or ECNR.\n";
        return false;
    }

    OPRF_SERVER_INTERACTIVE = has_flag(argc, argv, "--server_oprf_interactive") ||
                             has_flag(argc, argv, "--OPRF_server_interactive") ||
                             has_flag(argc, argv, "--oprf_server_interactive");
    if (has_flag(argc, argv, "--server_oprf_local") ||
        has_flag(argc, argv, "--OPRF_server_local") ||
        has_flag(argc, argv, "--oprf_server_local")) {
        OPRF_SERVER_INTERACTIVE = false;
    }
    CUCKOO_OPRF_SPLIT_OUTPUT = !(has_flag(argc, argv, "--cuckoo_oprf_two_blocks") ||
                                 has_flag(argc, argv, "--OPRF_cuckoo_two_blocks") ||
                                 has_flag(argc, argv, "--oprf_cuckoo_two_blocks"));

    std::string oprf_addr;
    if (get_string_arg(argc, argv, "--oprf_addr=", oprf_addr) ||
        get_string_arg(argc, argv, "--oprfaddr=", oprf_addr) ||
        get_string_arg(argc, argv, "--gcaes_oprf_addr=", oprf_addr) ||
        get_string_arg(argc, argv, "--ecnr_oprf_addr=", oprf_addr)) {
        const size_t colon = oprf_addr.rfind(':');
        if (colon == std::string::npos || colon == 0 || colon + 1 >= oprf_addr.size()) {
            cerr << "--oprf_addr must be host:port\n";
            return false;
        }
        try {
            const std::string host = oprf_addr.substr(0, colon);
            const int port = std::stoi(oprf_addr.substr(colon + 1));
            disco_gcaes_oprf_set_server(host.c_str(), port);
        } catch (const std::exception& ex) {
            cerr << "Invalid --oprf_addr: " << ex.what() << "\n";
            return false;
        }
    }

    return true;
}

static bool validate_parsed_args(const ParsedCliArgs& args) {
    const vector<string> accepted_filters = {"Bloom", "Cuckoo"};
    if (find(accepted_filters.begin(), accepted_filters.end(), args.filter) == accepted_filters.end()) {
        cerr << "Invalid filter type. Use --filter=Bloom or --filter=Cuckoo\n";
        return false;
    }
    if (args.ca_only && args.filter != "Bloom" && args.filter != "Cuckoo") {
        cerr << "--CA_only implements cardinality only for --filter=Bloom or --filter=Cuckoo\n";
        return false;
    }
    if (!args.ca_only && CA_COMPOUND_LSH) {
        cerr << "--ca_compound_lsh is only valid together with --CA_only\n";
        return false;
    }

    const int pir_mode_count =
        (USE_PIR_DOUBLE ? 1 : 0) +
        (USE_PIR_SINGLE ? 1 : 0) +
        (USE_PIR_BATCHPIR ? 1 : 0);
    if (pir_mode_count > 1) {
        cerr << "Choose only one PIR mode: --PIR_double, --PIR_single/--PIR_simple, or --PIR_BatchPIR\n";
        return false;
    }
    if (args.ca_only && USE_PIR_SINGLE) {
        cerr << "--CA_only PIR over encrypted FHE ciphertext rows supports --PIR_double"
             << " or --PIR_BatchPIR, not --PIR_single/FrodoPIR\n";
        return false;
    }
    if (args.ca_only && USE_BATCH && !USE_PIR_BATCHPIR) {
        cerr << "--CA_only does not use the standalone --BATCH flag; use --PIR_BatchPIR for BatchPIR\n";
        return false;
    }
    if (!args.ca_only && USE_OPRF && !(USE_PIR_DOUBLE || USE_PIR_SINGLE || USE_PIR_BATCHPIR)) {
        cerr << "OPRF mode is wired for private PIR query paths; use --OPRF together with a PIR mode\n";
        return false;
    }
    if (USE_BATCH && !(USE_PIR_DOUBLE || USE_PIR_SINGLE || USE_PIR_BATCHPIR)) {
        cerr << "Batched PIR mode requires --PIR_double, --PIR_single/--PIR_simple, or --PIR_BatchPIR\n";
        return false;
    }
    if (USE_PIR_BATCHPIR && !fuzzy_pets_batchpir::available()) {
        cerr << "PIR_BatchPIR is unavailable: "
             << fuzzy_pets_batchpir::unavailable_reason()
             << "\n";
        return false;
    }
#if !FUZZY_PETS_HAS_FRODOPIR
    if (USE_PIR_SINGLE) {
        cerr << "PIR_single requires FrodoPIR plus its sha3 and RandomShake headers. "
             << "Populate frodoPIR submodules and rebuild.\n";
        return false;
    }
#endif

    if (args.ca_only && !args.has_all_lsh_params) {
        cerr << "--CA_only requires explicit --L=, --k=, and --w= parameters; grid search is not part of PiCardSum\n";
        return false;
    }
    if (args.ca_only && args.sweep_filter_size) {
        cerr << "--CA_only does only PiCardSum cardinality; do not combine it with filter-size sweep\n";
        return false;
    }
    if (args.calibrate_lkw) {
        if (args.filter != "Cuckoo") {
            cerr << "--CALIBRATE_LKW currently calibrates --filter=Cuckoo only\n";
            return false;
        }
        if (USE_PIR_DOUBLE || USE_PIR_SINGLE || USE_PIR_BATCHPIR || USE_OPRF || USE_BATCH) {
            cerr << "--CALIBRATE_LKW is direct Cuckoo calibration; do not combine it with PIR, OPRF, or batch flags\n";
            return false;
        }
    }

    return true;
}

static bool parse_cli_args(int argc, char* argv[], ParsedCliArgs& args) {
    if (removed_parallel_flag_present(argc, argv)) {
        cerr << "Parallel mode has been removed; run without parallel flags.\n";
        return false;
    }
    if (removed_lowmc_oprf_flag_present(argc, argv)) {
        cerr << "LOWMC/GCLOWMC OPRF has been removed; use GCAES or ECNR.\n";
        return false;
    }
    if (removed_grid_search_flag_present(argc, argv)) {
        cerr << "GRID_SEARCH has been removed; use --CALIBRATE_LKW for parameter calibration.\n";
        return false;
    }

    string requested_db = "gowalla";
    get_string_arg(argc, argv, "--db=", requested_db);
    get_string_arg(argc, argv, "db=", requested_db);
    if (!apply_db_preset(requested_db)) {
        cerr << "Invalid db preset. Use --db=gowalla, --db=random, --db=random_small, --db=MNIST, --db=FashionMNIST, --db=febrl, --db=nf_bot_iot, or --db=acm_dblp\n";
        return false;
    }
    const bool explicit_server_path = get_string_arg(argc, argv, "--server_path=", server_path);
    get_string_arg(argc, argv, "--client_path=", client_path);
    const bool explicit_client_close_path = get_string_arg(argc, argv, "--client_close_path=", CLIENT_CLOSE_PATH);
    const bool explicit_client_far_path = get_string_arg(argc, argv, "--client_far_path=", CLIENT_FAR_PATH);
    get_string_arg(argc, argv, "--raw_client_path=", RAW_CLIENT_PATH);
    const bool explicit_metadata_path = get_string_arg(argc, argv, "--metadata_path=", loc_metadata_path);
    if (explicit_server_path) {
        APPEND_RANDOM_SERVER = false;
        APPEND_AUGMENTED_SERVER = false;
        APPEND_DISTRIBUTION_SERVER = false;
    }
    if (explicit_client_close_path || explicit_client_far_path ||
        has_flag(argc, argv, "--use_stored_client_split") ||
        has_flag(argc, argv, "--stored_client_split") ||
        has_flag(argc, argv, "--no_generate_client_from_server")) {
        GENERATE_CLIENT_FROM_SERVER = false;
    }

    const bool requested_calibrate_lkw =
        has_flag(argc, argv, "--CALIBRATE_LKW") ||
        has_flag(argc, argv, "--calibrate_lkw");
    if (requested_calibrate_lkw &&
        !explicit_server_path &&
        !explicit_client_close_path &&
        !explicit_client_far_path &&
        !explicit_metadata_path) {
        string calibration_prefix;
        if (db_preset == "MNIST") {
            calibration_prefix = "mnist";
        } else if (db_preset == "FashionMNIST") {
            calibration_prefix = "fashionmnist";
        } else if (db_preset == "gowalla") {
            calibration_prefix = "gowalla";
        } else if (db_preset == "febrl") {
            calibration_prefix = "febrl";
        }

        if (!calibration_prefix.empty()) {
            const string calibration_dir = DATASET_DIR + "/calibrations/";
            const string calibration_server_path =
                calibration_dir + calibration_prefix + "_calibration_server.json";
            const string calibration_close_path =
                calibration_dir + calibration_prefix + "_calibration_client_close.json";
            const string calibration_far_path =
                calibration_dir + calibration_prefix + "_calibration_client_far.json";
            const string calibration_metadata_path =
                calibration_dir + calibration_prefix + "_calibration_metadata.txt";
            ifstream server_check(calibration_server_path);
            ifstream close_check(calibration_close_path);
            ifstream far_check(calibration_far_path);
            ifstream metadata_check(calibration_metadata_path);
            if (server_check && close_check && far_check && metadata_check) {
                server_path = calibration_server_path;
                CLIENT_CLOSE_PATH = calibration_close_path;
                CLIENT_FAR_PATH = calibration_far_path;
                loc_metadata_path = calibration_metadata_path;
            }
        }
    }

    int debug_mode = 0;
    get_int_arg(argc, argv, "--debug=", debug_mode);
    get_int_arg(argc, argv, "debug=", debug_mode);
    DEBUG_PHASES = debug_mode != 0 || has_flag(argc, argv, "--debug");

    init_config();

    args.calibrate_lkw = requested_calibrate_lkw;
    args.sweep_filter_size = has_flag(argc, argv, "--SWEEP_FILTER_SIZE") ||
                             has_flag(argc, argv, "--sweep_filter_size");
    args.ca_only = has_flag(argc, argv, "--CA_only") ||
                   has_flag(argc, argv, "--ca_only") ||
                   has_flag(argc, argv, "--CARDINALITY_only") ||
                   has_flag(argc, argv, "--cardinality_only");

    args.calibration_stop_on_first_target =
        !(has_flag(argc, argv, "--calibration_no_early_stop") ||
          has_flag(argc, argv, "--calibration_full_grid"));
    args.calibration_holdout =
        has_flag(argc, argv, "--calibration_holdout") ||
        has_flag(argc, argv, "--CALIBRATION_HOLDOUT") ||
        has_flag(argc, argv, "--calibration_heldout") ||
        has_flag(argc, argv, "--CALIBRATION_HELDOUT");
    get_string_arg(argc, argv, "--calibration_lkw=", args.calibration_lkw_arg);
    get_double_arg(argc, argv, "--calibration_target_fp=", args.calibration_target_fp);
    get_double_arg(argc, argv, "--calibration_target_fn=", args.calibration_target_fn);
    get_double_arg(argc, argv, "--calibration_abort_fail_rate=", args.calibration_abort_fail_rate);
    get_double_arg(argc, argv, "--calibration_abort_fill_ratio=", args.calibration_abort_fill_ratio);
    get_size_arg(argc, argv, "--calibration_abort_min_attempts=", args.calibration_abort_min_attempts);
    get_size_arg(argc, argv, "--calibration_progress_steps=", args.calibration_progress_steps);
    CALIBRATION_HOLDOUT = args.calibration_holdout;
    CALIBRATION_HOLDOUT_USE_HELD_OUT = args.calibrate_lkw && args.calibration_holdout;

    CA_COMPOUND_LSH = has_flag(argc, argv, "--ca_compound_lsh") ||
                      has_flag(argc, argv, "--CA_compound_lsh") ||
                      has_flag(argc, argv, "--ca_use_compound_lsh");
    USE_PIR_DOUBLE = has_flag(argc, argv, "--PIR_double") ||
                     has_flag(argc, argv, "--USE_PIR_double") ||
                     has_flag(argc, argv, "--pir_double") ||
                     has_flag(argc, argv, "--use_pir_double");
    USE_PIR_SINGLE = has_flag(argc, argv, "--PIR_single") ||
                     has_flag(argc, argv, "--USE_PIR_single") ||
                     has_flag(argc, argv, "--pir_single") ||
                     has_flag(argc, argv, "--use_pir_single") ||
                     has_flag(argc, argv, "--PIR_simple") ||
                     has_flag(argc, argv, "--USE_PIR_simple") ||
                     has_flag(argc, argv, "--pir_simple") ||
                     has_flag(argc, argv, "--use_pir_simple");
    USE_PIR_BATCHPIR = has_flag(argc, argv, "--PIR_BatchPIR") ||
                       has_flag(argc, argv, "--USE_PIR_BatchPIR") ||
                       has_flag(argc, argv, "--PIR_batchpir") ||
                       has_flag(argc, argv, "--pir_batchpir") ||
                       has_flag(argc, argv, "--use_pir_batchpir");
    if (db_preset == "acm_dblp" &&
        !requested_calibrate_lkw &&
        !USE_PIR_DOUBLE &&
        !USE_PIR_SINGLE &&
        !USE_PIR_BATCHPIR) {
        USE_PIR_BATCHPIR = true;
    }

    if (!parse_oprf_config(argc, argv)) {
        return false;
    }
    if (db_preset == "acm_dblp" && !requested_calibrate_lkw && !USE_OPRF) {
        USE_OPRF = true;
        disco_oprf_set_mechanism("GCAES");
    }

    const bool ca_no_oprf = has_flag(argc, argv, "--ca_no_oprf") ||
                            has_flag(argc, argv, "--CA_no_oprf");
    CA_PLAIN_LOCAL_PRF =
        has_flag(argc, argv, "--ca_plain_local_prf") ||
        has_flag(argc, argv, "--CA_plain_local_prf") ||
        has_flag(argc, argv, "--ca_local_prf") ||
        has_flag(argc, argv, "--CA_local_prf");
    CA_DISABLE_OPRF_AND_PIR =
        has_flag(argc, argv, "--ca_no_oprf_no_pir") ||
        has_flag(argc, argv, "--CA_no_oprf_no_pir") ||
        has_flag(argc, argv, "--ca_no_oprf_pir") ||
        has_flag(argc, argv, "--ca_plain") ||
        has_flag(argc, argv, "--ca_no_privacy") ||
        CA_PLAIN_LOCAL_PRF;
    USE_BATCH = has_flag(argc, argv, "--BATCH") ||
                has_flag(argc, argv, "--USE_BATCH") ||
                has_flag(argc, argv, "--batch") ||
                has_flag(argc, argv, "--use_batch");

    get_string_arg(argc, argv, "--filter=", args.filter);
    if (db_preset == "acm_dblp" && args.filter.empty()) {
        args.filter = "Cuckoo";
    }
    if (args.ca_only && args.filter.empty()) {
        args.filter = "Bloom";
    }

    get_size_arg(argc, argv, "--filter_size=", filter_size);
    get_size_arg(argc, argv, "--pir_double_max_cached_shards=", PIR_DOUBLE_MAX_CACHED_SHARDS);
    get_size_arg(argc, argv, "--pir_single_max_cached_shards=", PIR_SINGLE_MAX_CACHED_SHARDS);
    get_size_arg(argc, argv, "--pir_batchpir_batch_size=", PIR_BATCHPIR_BATCH_SIZE);
    get_size_arg(argc, argv, "--pir_batchpir_cuckoo_shards=", PIR_BATCHPIR_CUCKOO_SHARDS_OVERRIDE);
    get_size_arg(argc, argv, "--ca_batch_slots=", CA_BATCH_SLOTS);
    get_size_arg(argc, argv, "--ca_pir_shard_rows=", CA_PIR_SHARD_ROWS);
    get_size_arg(argc, argv, "--ca_pir_chunk_cache_limit=", CA_PIR_CHUNK_CACHE_LIMIT);
    get_size_arg(argc, argv, "--ca_bloom_hashes=", CA_BLOOM_HASHES_OVERRIDE);
    get_size_arg(argc, argv, "--ca_cuckoo_fp_bits=", CA_CUCKOO_FP_BITS);
    get_size_arg(argc, argv, "--ca_cuckoo_bucket_tag_bits=", CA_CUCKOO_BUCKET_TAG_BITS);
    get_size_arg(argc, argv, "--ca_cuckoo_tag_hashes=", CA_CUCKOO_TAG_HASHES);
    if (has_flag(argc, argv, "--cuckoo_pow2_buckets") ||
        has_flag(argc, argv, "--cuckoo_power_of_two_buckets") ||
        has_flag(argc, argv, "--Cuckoo_power_of_two_buckets")) {
        CUCKOO_POWER_OF_TWO_BUCKETS = true;
    }
    CA_MATERIALIZE_PIR_DB = has_flag(argc, argv, "--ca_materialize_pir_db") ||
                            has_flag(argc, argv, "--CA_materialize_pir_db");

    get_double_arg(argc, argv, "--synthetic_client_close_radius=", SYNTHETIC_CLIENT_CLOSE_RADIUS_DEG);
    get_double_arg(argc, argv, "--synthetic_client_far_min_radius=", SYNTHETIC_CLIENT_FAR_MIN_RADIUS_DEG);
    get_double_arg(argc, argv, "--augmented_server_jitter=", AUGMENTED_SERVER_JITTER_DEG);
    get_double_arg(argc, argv, "--distribution_server_noise_scale=", DISTRIBUTION_SERVER_NOISE_SCALE);
    get_string_arg(argc, argv, "--append_random_server_path=", APPEND_RANDOM_SERVER_PATH);

    if (has_flag(argc, argv, "--generate_client_from_server")) {
        GENERATE_CLIENT_FROM_SERVER = true;
        CLIENT_CLOSE_PATH = "(generated-from-sampled-server)";
        CLIENT_FAR_PATH = "(generated-far-region)";
    }
    if (has_flag(argc, argv, "--append_random_server")) {
        APPEND_RANDOM_SERVER = true;
    }
    if (has_flag(argc, argv, "--append_augmented_server")) {
        APPEND_AUGMENTED_SERVER = true;
    }
    if (has_flag(argc, argv, "--append_distribution_server") ||
        has_flag(argc, argv, "--append_same_distribution_server") ||
        has_flag(argc, argv, "--append_regression_server") ||
        has_flag(argc, argv, "--append_linear_regression_server")) {
        APPEND_DISTRIBUTION_SERVER = true;
        APPEND_RANDOM_SERVER = false;
        APPEND_AUGMENTED_SERVER = false;
    }
    PORTABLE_DATASET_SAMPLING =
        has_flag(argc, argv, "--portable_sampling") ||
        has_flag(argc, argv, "--portable_dataset_sampling");
    PORTABLE_LSH =
        has_flag(argc, argv, "--portable_lsh") ||
        has_flag(argc, argv, "--portable_e2lsh");
    if (has_flag(argc, argv, "--pir_single_prebuild_shards")) {
        PIR_SINGLE_PREBUILD_SHARDS = true;
    }
    if (has_flag(argc, argv, "--no_pir_single_prebuild_shards")) {
        PIR_SINGLE_PREBUILD_SHARDS = false;
    }

    PIR_DOUBLE_MAX_CACHED_SHARDS = std::max<size_t>(1, PIR_DOUBLE_MAX_CACHED_SHARDS);
    PIR_SINGLE_MAX_CACHED_SHARDS = std::max<size_t>(1, PIR_SINGLE_MAX_CACHED_SHARDS);
    PIR_BATCHPIR_BATCH_SIZE = std::max<size_t>(1, PIR_BATCHPIR_BATCH_SIZE);
    if (PIR_BATCHPIR_CUCKOO_SHARDS_OVERRIDE > 0) {
        PIR_BATCHPIR_CUCKOO_SHARDS_OVERRIDE =
            std::max<size_t>(1, PIR_BATCHPIR_CUCKOO_SHARDS_OVERRIDE);
    }
    CA_BATCH_SLOTS = std::max<size_t>(1, CA_BATCH_SLOTS);
    CA_PIR_SHARD_ROWS = std::max<size_t>(1, CA_PIR_SHARD_ROWS);

    if (args.ca_only) {
        if (CA_DISABLE_OPRF_AND_PIR) {
            USE_PIR_DOUBLE = false;
            USE_PIR_SINGLE = false;
            USE_PIR_BATCHPIR = false;
            USE_BATCH = false;
            CA_USE_OPRF = false;
            USE_OPRF = false;
        } else if (!USE_PIR_DOUBLE && !USE_PIR_SINGLE && !USE_PIR_BATCHPIR) {
            USE_PIR_BATCHPIR = true;
            CA_USE_OPRF = !ca_no_oprf;
            USE_OPRF = CA_USE_OPRF;
        } else {
            CA_USE_OPRF = !ca_no_oprf;
            USE_OPRF = CA_USE_OPRF;
        }
    } else {
        if (ca_no_oprf) {
            cerr << "--ca_no_oprf is only valid together with --CA_only\n";
            return false;
        }
        if (CA_DISABLE_OPRF_AND_PIR) {
            cerr << "--ca_no_oprf_no_pir/--ca_plain is only valid together with --CA_only\n";
            return false;
        }
    }
    if (USE_PIR_BATCHPIR) {
        USE_BATCH = true;
    }

    get_int_arg(argc, argv, "--server_size=", N);
    get_int_arg(argc, argv, "--num_runs=", NUM_RUNS_PSI);
    get_int_arg(argc, argv, "--num_search_runs=", NUM_RUNS_SEARCH);
    get_int_arg(argc, argv, "--dataset_seed=", DATASET_SEED);
    if (DATASET_SEED < 0) {
        cerr << "--dataset_seed must be nonnegative\n";
        return false;
    }
    if (get_int_arg(argc, argv, "--client_size=", CLIENT_SIZE_SEARCH)) {
        CLIENT_SIZES_LIST = {CLIENT_SIZE_SEARCH};
    }
    if (args.calibrate_lkw && !args.calibration_holdout) {
        try {
            N = static_cast<int>(count_server_json_entries(server_path));
        } catch (const std::exception& ex) {
            cerr << "Could not count Server entries for calibration: " << ex.what() << "\n";
            return false;
        }
        if (N <= 0) {
            cerr << "Server dataset is empty; cannot calibrate.\n";
            return false;
        }
    }

    const bool has_L_arg = get_int_arg(argc, argv, "--L=", L);
    const bool has_k_arg = get_int_arg(argc, argv, "--k=", k);
    const bool has_w_arg = get_double_arg(argc, argv, "--w=", w);
    args.has_all_lsh_params =
        (has_L_arg && has_k_arg && has_w_arg) || db_preset == "acm_dblp";

    return validate_parsed_args(args);
}
