// ============================================================
// ===================== GRID SEARCH ==========================
// ============================================================

struct GridResult {
    int L = 0;
    int k = 0;
    double w = 0.0;
    double fp = 1.0;
    double fn = 1.0;
    double score = numeric_limits<double>::infinity();
    double fill = 0.0;
};

struct LkwCandidate {
    int L = 0;
    int k = 0;
    double w = 0.0;
};

static vector<LkwCandidate> parse_lkw_candidates(const string& raw) {
    vector<LkwCandidate> candidates;
    stringstream ss(raw);
    string item;

    while (getline(ss, item, ',')) {
        if (item.empty()) {
            continue;
        }
        replace(item.begin(), item.end(), '/', ':');
        stringstream item_ss(item);
        string L_part;
        string k_part;
        string w_part;
        if (!getline(item_ss, L_part, ':') ||
            !getline(item_ss, k_part, ':') ||
            !getline(item_ss, w_part, ':')) {
            throw invalid_argument("Invalid --calibration_lkw entry: " + item);
        }
        string extra;
        if (getline(item_ss, extra, ':')) {
            throw invalid_argument("Invalid --calibration_lkw entry: " + item);
        }
        candidates.push_back({stoi(L_part), stoi(k_part), stod(w_part)});
    }

    return candidates;
}

static vector<LkwCandidate> default_lkw_candidates() {
    vector<LkwCandidate> candidates;
    candidates.reserve(L_vals.size() * k_vals.size() * w_vals.size());
    for (int L_candidate : L_vals) {
        for (int k_candidate : k_vals) {
            for (double w_candidate : w_vals) {
                candidates.push_back({L_candidate, k_candidate, w_candidate});
            }
        }
    }
    return candidates;
}

static int run_cuckoo_lkw_calibration_once(
    const string& server_path,
    const string& client_path,
    int dim,
    int server_N,
    int client_size,
    size_t filter_size,
    const vector<LkwCandidate>& candidates,
    double target_fp,
    double target_fn,
    double abort_fail_rate,
    double abort_fill_ratio,
    size_t abort_min_attempts,
    size_t progress_steps,
    bool stop_on_first_target,
    bool use_holdout_dataset
) {
    if (candidates.empty()) {
        cerr << "--CALIBRATE_LKW needs at least one --calibration_lkw=L:k:w candidate\n";
        return -1;
    }

    const uint32_t calibration_dataset_seed = dataset_seed_for_run(0);
    mt19937 run_rng(calibration_dataset_seed);
    FixedDataset ds = build_fixed_dataset_once(
        server_path,
        client_path,
        server_N,
        client_size,
        run_rng
    );

    cout << "[calibration dataset] seed=" << calibration_dataset_seed
         << " mode=" << (use_holdout_dataset ? "holdout" : "legacy")
         << " server_size=" << ds.server_imgs.size()
         << " client_size=" << ds.client_imgs.size()
         << " filter_size=" << filter_size
         << " candidates=" << candidates.size()
         << " selection=" << (use_holdout_dataset ? "min_fp" : "target_fp_fn")
         << " abort_fail_rate=" << abort_fail_rate
         << " abort_fill_ratio=" << abort_fill_ratio
         << " abort_min_attempts=" << abort_min_attempts
         << " stop_on_first_target=" << (stop_on_first_target ? "true" : "false")
         << "\n" << std::flush;

    GridResult best;
    bool have_best = false;
    double best_cuckoo_failed = 0.0;
    double best_cuckoo_fail_rate = 0.0;
    string best_status = "no_candidate_met_target";
    static constexpr double alpha = 0.7;
    size_t candidate_index = 0;

    for (const LkwCandidate& candidate : candidates) {
        candidate_index++;
        const auto candidate_start = Clock::now();
        cout << "[calibration start] "
             << "candidate=" << candidate_index << "/" << candidates.size()
             << " client_size=" << client_size
             << " server_size=" << server_N
             << " filter_size=" << filter_size
             << " L=" << candidate.L
             << " k=" << candidate.k
             << " w=" << candidate.w
             << "\n" << std::flush;

        E2LSH lsh(dim, candidate.L, candidate.k, candidate.w, 42, PORTABLE_LSH);
        CuckooFilter cuckoo(filter_size, 4, 5, CUCKOO_POWER_OF_TWO_BUCKETS);

        bool aborted = false;
        string abort_reason;
        size_t inserted_server = 0;
        const size_t progress_stride =
            progress_steps == 0 ? 0 : std::max<size_t>(1, ds.server_imgs.size() / progress_steps);
        int FP = 0;
        int TN = 0;
        int TP = 0;
        int FN = 0;
        double fp_rate = 1.0;
        double fn_rate = 1.0;
        double score = 1.0;
        string status = "fail";

        for (const auto& img : ds.server_imgs) {
            cuckoo.insert(img, lsh);
            inserted_server++;

            const double cuckoo_fail_rate = cuckoo.failed_insert_rate();
            const double fill = cuckoo.fill_ratio();
            const bool can_abort = cuckoo.attempted_insert_count() >= abort_min_attempts;
            if (can_abort && abort_fail_rate > 0.0 && cuckoo_fail_rate > abort_fail_rate) {
                aborted = true;
                abort_reason = "cuckoo_fail_rate";
            } else if (can_abort && abort_fill_ratio > 0.0 && fill >= abort_fill_ratio) {
                aborted = true;
                abort_reason = "cuckoo_fill_ratio";
            }

            const bool progress_due =
                progress_stride != 0 &&
                (inserted_server == ds.server_imgs.size() || inserted_server % progress_stride == 0);
            if (progress_due || aborted) {
                cout << "[calibration progress] "
                     << "candidate=" << candidate_index << "/" << candidates.size()
                     << " L=" << candidate.L
                     << " k=" << candidate.k
                     << " w=" << candidate.w
                     << " inserted_server=" << inserted_server
                     << "/" << ds.server_imgs.size()
                     << " attempted=" << cuckoo.attempted_insert_count()
                     << " cuckoo_failed=" << cuckoo.failed_insert_count()
                     << " cuckoo_fail_rate=" << cuckoo_fail_rate
                     << " fill=" << fill
                     << "\n" << std::flush;
            }

            if (aborted) {
                break;
            }
        }

        if (!aborted) {
            for (size_t i = 0; i < ds.client_imgs.size(); ++i) {
                const bool reported = cuckoo.membership(ds.client_imgs[i], lsh);
                if (ds.client_is_close[i]) {
                    if (reported) {
                        TP++;
                    } else {
                        FN++;
                    }
                } else {
                    if (reported) {
                        FP++;
                    } else {
                        TN++;
                    }
                }
            }

            fp_rate = (FP + TN) ? double(FP) / double(FP + TN) : 0.0;
            fn_rate = (TP + FN) ? double(FN) / double(TP + FN) : 0.0;
            score = use_holdout_dataset
                ? fp_rate
                : alpha * fn_rate + (1.0 - alpha) * fp_rate;
            status = use_holdout_dataset
                ? "measured"
                : ((fp_rate <= target_fp && fn_rate <= target_fn) ? "selected" : "fail");
        } else {
            status = "aborted_" + abort_reason;
        }
        const double cuckoo_failed = static_cast<double>(cuckoo.failed_insert_count());
        const double cuckoo_attempted = static_cast<double>(cuckoo.attempted_insert_count());
        const double cuckoo_fail_rate = cuckoo_attempted ? cuckoo_failed / cuckoo_attempted : 0.0;
        const double candidate_seconds =
            elapsed_ms(candidate_start, Clock::now()) / 1000.0;

        cout << "[calibration] "
             << "client_size=" << client_size
             << " server_size=" << server_N
             << " filter_size=" << filter_size
             << " candidate=" << candidate_index << "/" << candidates.size()
             << " L=" << candidate.L
             << " k=" << candidate.k
             << " w=" << candidate.w
             << " fp=" << fp_rate
             << " fn=" << fn_rate
             << " FP_count=" << FP
             << " TN_count=" << TN
             << " TP_count=" << TP
             << " FN_count=" << FN
             << " score=" << score
             << " fill=" << cuckoo.fill_ratio()
             << " inserted_server=" << inserted_server
             << " seconds=" << candidate_seconds
             << " cuckoo_failed=" << cuckoo_failed
             << " cuckoo_fail_rate=" << cuckoo_fail_rate
             << " status=" << status
             << "\n" << std::flush;

        const bool best_is_selected = best_status == "selected";
        const bool candidate_is_selected = status == "selected";
        const bool better = use_holdout_dataset
            ? (!have_best ||
               fp_rate < best.fp ||
               (fp_rate == best.fp && fn_rate < best.fn) ||
               (fp_rate == best.fp && fn_rate == best.fn && candidate.L < best.L) ||
               (fp_rate == best.fp && fn_rate == best.fn && candidate.L == best.L && candidate.k < best.k) ||
               (fp_rate == best.fp && fn_rate == best.fn && candidate.L == best.L && candidate.k == best.k && candidate.w < best.w))
            : (!have_best ||
               (candidate_is_selected && !best_is_selected) ||
               (candidate_is_selected == best_is_selected &&
                   (score < best.score ||
                    (score == best.score && fn_rate < best.fn) ||
                    (score == best.score && fn_rate == best.fn && fp_rate < best.fp))));
        if (better) {
            best = {candidate.L, candidate.k, candidate.w, fp_rate, fn_rate, score, cuckoo.fill_ratio()};
            best_cuckoo_failed = cuckoo_failed;
            best_cuckoo_fail_rate = cuckoo_fail_rate;
            best_status = use_holdout_dataset ? "selected" : status;
            have_best = true;
        }

        if (!use_holdout_dataset && status == "selected" && stop_on_first_target) {
            break;
        }
    }

    if (best_status != "selected") {
        best_status = "no_candidate_met_target";
    }

    cout << "[calibration best] "
         << "client_size=" << client_size
         << " server_size=" << server_N
         << " filter_size=" << filter_size
         << " L=" << best.L
         << " k=" << best.k
         << " w=" << best.w
         << " fp=" << best.fp
         << " fn=" << best.fn
         << " score=" << best.score
             << " cuckoo_failed=" << best_cuckoo_failed
             << " cuckoo_fail_rate=" << best_cuckoo_fail_rate
             << " status=" << best_status
         << "\n" << std::flush;

    return 0;
}
