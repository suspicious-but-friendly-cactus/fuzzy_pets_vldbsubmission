//Our fuzzy PSI Protocol
int fuzzypsi(
    int L,
    int k,
    double w,
    string mode,
    string filter
) {
    double fp_rate_sum = 0.0, fn_rate_sum = 0.0;

    vector<double> fp_rates, fn_rates;
    vector<int> client_sizes;

    cout << "Running for " << NUM_RUNS_PSI
         << " runs per |client| with dataset_seed=" << DATASET_SEED
         << " (run seed = dataset_seed + run_index)" << endl;
    // LSH & filter
    cout << "USING DATA DIMENSIONS: " << dim << "\n";


     //initialize a family of L E2LSH functions
    E2LSH lsh(dim, L, k, w);


    //initialize filters
    if (filter == "Bloom") {
        warn_large_bloom_private_pir(filter_size, static_cast<size_t>(L) * static_cast<size_t>(N));
    }
    BloomFilter bloom(filter_size,L*N);
    CuckooFilter cuckoo(filter_size, 4, 5, CUCKOO_POWER_OF_TWO_BUCKETS);
    const size_t cuckoo_batchpir_shard_count =
        (filter == "Cuckoo" && USE_PIR_BATCHPIR)
            ? batchpir_cuckoo_shard_count_for_server_size(static_cast<size_t>(std::max(0, N)))
            : 1;
    const size_t cuckoo_batchpir_shard_filter_size =
        batchpir_cuckoo_shard_filter_size(cuckoo_batchpir_shard_count);
    std::vector<CuckooFilter> cuckoo_batchpir_shards;

    size_t filter_total_size_bits = 0;

    if (filter == "Bloom") {
        filter_total_size_bits = bloom.total_size_bits();
    }
    else if (filter == "Cuckoo") {
        if (cuckoo_batchpir_shard_count > 1) {
            CuckooFilter shard_shape(
                cuckoo_batchpir_shard_filter_size,
                4,
                5,
                CUCKOO_POWER_OF_TWO_BUCKETS
            );
            filter_total_size_bits =
                cuckoo_batchpir_shard_count * shard_shape.total_size_bits();
            cout << "[PIR_BatchPIR] Cuckoo dataset sharding enabled"
                 << " server_size=" << N
                 << " shards=" << cuckoo_batchpir_shard_count
                 << " shard_filter_size=" << cuckoo_batchpir_shard_filter_size
                 << " total_filter_bits=" << filter_total_size_bits
                 << "\n";
        } else {
            filter_total_size_bits = cuckoo.total_size_bits();
        }
    }

    vector<string> server_ids;
    vector<vector<double>> server_imgs, client_imgs;
    vector<bool> client_is_close;

    // Run for each given client size (we usually test |client|=1000 but we can sweep over different sizes as well e.g. 10000)
    for (int client_size : CLIENT_SIZES_LIST) 
    {
        fp_rate_sum = 0.0;
        fn_rate_sum = 0.0;
        double total_runtime_ms_sum = 0.0;
        double client_runtime_ms_sum = 0.0;
        double server_runtime_ms_sum = 0.0;
        double server_offline_runtime_ms_sum = 0.0;
        double server_online_runtime_ms_sum = 0.0;
        double filter_build_runtime_ms_sum = 0.0;
        double pir_setup_runtime_ms_sum = 0.0;
        double oprf_runtime_ms_sum = 0.0;
        long double communication_bytes_sum = 0.0L;
        long double pir_setup_communication_bytes_sum = 0.0L;
        long double pir_query_communication_bytes_sum = 0.0L;
        long double pir_response_communication_bytes_sum = 0.0L;
        long double oprf_communication_bytes_sum = 0.0L;
        long double oprf_client_to_server_bytes_sum = 0.0L;
        long double oprf_server_to_client_bytes_sum = 0.0L;
        long double oprf_calls_sum = 0.0L;
        long double oprf_blocks_sum = 0.0L;
        double local_oprf_server_prf_runtime_ms_sum = 0.0;
        long double local_oprf_server_prf_calls_sum = 0.0L;
        long double local_oprf_server_prf_blocks_sum = 0.0L;
        long double batchpir_num_buckets_sum = 0.0L;
        long double batchpir_max_bucket_size_sum = 0.0L;
        long double batchpir_first_dimension_size_sum = 0.0L;
        long double batchpir_server_count_sum = 0.0L;
        long double fp_count_sum = 0.0L;
        long double tn_count_sum = 0.0L;
        long double tp_count_sum = 0.0L;
        long double fn_count_sum = 0.0L;
        long double cuckoo_failed_sum = 0.0L;
        long double cuckoo_fail_rate_sum = 0.0L;

        for (int run = 0; run < NUM_RUNS_PSI; run++) {
            const uint32_t run_seed = dataset_seed_for_run(run);
            mt19937 run_rng(run_seed);
            cout << "[dataset seed] client_size=" << client_size
                 << " run=" << (run + 1) << "/" << NUM_RUNS_PSI
                 << " seed=" << run_seed << "\n" << std::flush;

            /************************
            Step 1: Load Databases
            ************************/
            if (REUSE_DATASET_BETWEEN_GRID_AND_TEST == 1) { //in case we want to run the exact same sampling of datasets for parameter optimization and experiment run (less privacy but better utility)
            PHASE_LOG << "[phase] Dataset reuse start\n" << std::flush;
            // Build once and reuse it for this run.
            server_ids = fixed_ds.server_ids;
            server_imgs = fixed_ds.server_imgs;
            client_imgs = fixed_ds.client_imgs;
            client_is_close = fixed_ds.client_is_close;
            PHASE_LOG << "[phase] Dataset reuse done server=" << server_imgs.size()
                 << " client=" << client_imgs.size() << "\n" << std::flush;
            }
            else{
            // load new db
            PHASE_LOG << "[phase] Dataset load start server_size=" << N
                 << " client_size=" << client_size
                 << " server_path=" << server_path
                 << "\n" << std::flush;
            PHASE_LOG << "[phase] load_server_from_json call start\n" << std::flush;
            load_server_from_json(server_path, N, run_rng, server_ids, server_imgs);
            PHASE_LOG << "[phase] load_server_from_json call done server=" << server_imgs.size()
                 << " ids=" << server_ids.size() << "\n" << std::flush;
            /*
            std::cout << "\n[Sanity] Server samples:\n";
            for (int i = 0; i < std::min(5, (int)server_imgs.size()); i++) {
                std::cout << "Server[" << i << "]: ";
                for (auto v : server_imgs[i]) std::cout << v << " ";
                std::cout << "\n";
            }
            */
            

            PHASE_LOG << "[phase] load_client_from_json_for_server call start\n" << std::flush;
            load_client_from_json_for_server(
                server_ids,
                server_imgs,
                client_size,
                run_rng,
                client_imgs,
                client_is_close
            );
            PHASE_LOG << "[phase] load_client_from_json_for_server call done client=" << client_imgs.size()
                 << " labels=" << client_is_close.size() << "\n" << std::flush;
            
            /*
            std::cout << "\n[Sanity] Client samples:\n";
            for (int i = 0; i < std::min(10, (int)client_imgs.size()); i++) {
                std::cout << "Client[" << i << "] ("
                          << (client_is_close[i] ? "close" : "far")
                          << "): ";

                for (auto v : client_imgs[i]) std::cout << v << " ";
                std::cout << "\n";
            
            }
            */
           }

            PHASE_LOG << "[phase] Dataset ready server=" << server_imgs.size()
                 << " client=" << client_imgs.size()
                 << " filter_size=" << filter_size
                 << "\n" << std::flush;
            PHASE_LOG << "[phase] Timed runtime starts after dataset sampling; sampling is excluded\n"
                 << std::flush;

            //debug_print_dataset(server_ids, server_imgs, client_imgs, client_is_close, client_relates_to, 10);
                
                //if (run == 0 && client_size == 10) {
                    //    print_images("Server images (from server.bin)", server);
                    //}


             /************************
            Step 2:  Create Filter
            ************************/
            const DiscoGcaesOprfStats oprf_stats_before_run = disco_gcaes_oprf_stats();
            const LocalOprfServerPrfStats local_oprf_server_prf_stats_before_run = local_oprf_server_prf_stats();
            auto t_start = chrono::high_resolution_clock::now();
            std::unique_ptr<PirDoubleRowReaderBase> pir_double_reader;
            std::vector<std::unique_ptr<PirDoubleRowReaderBase>> pir_double_reader_copies;
            std::unique_ptr<PirSingleRowReaderBase> pir_single_reader;
            std::vector<std::unique_ptr<PirSingleRowReaderBase>> pir_single_reader_copies;
            std::unique_ptr<fuzzy_pets_batchpir::RowReader> pir_batchpir_reader;
            std::vector<std::unique_ptr<fuzzy_pets_batchpir::RowReader>> pir_batchpir_shard_readers;
            double pir_setup_time_ms_for_run = 0.0;
            print_pir_scheme_for_run(run, NUM_RUNS_PSI);

            if (filter == "Bloom") {
                PHASE_LOG << "[phase] Filter build start filter=Bloom server=" << server_imgs.size()
                     << "\n" << std::flush;
                bloom.clear();
                if (USE_OPRF) {
                    bloom_insert_oprf_parallel_wrapped(bloom, server_imgs, lsh);
                } else {
                    for (auto& img : server_imgs) {
                        bloom.insert(img, lsh);
                    }
                }
            }
            else if (filter == "Cuckoo") {
                PHASE_LOG << "[phase] Filter build start filter=Cuckoo server=" << server_imgs.size()
                     << "\n" << std::flush;
                if (USE_PIR_BATCHPIR && cuckoo_batchpir_shard_count > 1) {
                    cuckoo_batchpir_shards.clear();
                    cuckoo_batchpir_shards.reserve(cuckoo_batchpir_shard_count);
                    for (size_t shard_idx = 0; shard_idx < cuckoo_batchpir_shard_count; ++shard_idx) {
                        cuckoo_batchpir_shards.emplace_back(
                            cuckoo_batchpir_shard_filter_size,
                            4,
                            5,
                            CUCKOO_POWER_OF_TWO_BUCKETS
                        );
                    }
                    for (size_t shard_idx = 0; shard_idx < cuckoo_batchpir_shard_count; ++shard_idx) {
                        const size_t shard_start =
                            (server_imgs.size() * shard_idx) / cuckoo_batchpir_shard_count;
                        const size_t shard_end =
                            (server_imgs.size() * (shard_idx + 1)) / cuckoo_batchpir_shard_count;
                        const size_t shard_count = shard_end - shard_start;
                        if (USE_OPRF) {
                            cuckoo_insert_oprf_range_wrapped(
                                cuckoo_batchpir_shards[shard_idx],
                                server_imgs,
                                lsh,
                                shard_start,
                                shard_count
                            );
                        } else {
                            cuckoo_insert_range(
                                cuckoo_batchpir_shards[shard_idx],
                                server_imgs,
                                lsh,
                                shard_start,
                                shard_count
                            );
                        }
                    }
                    PHASE_LOG << "[phase] Cuckoo sharded build done shards="
                         << cuckoo_batchpir_shard_count
                         << " shard_filter_size=" << cuckoo_batchpir_shard_filter_size
                         << "\n" << std::flush;
                } else {
                    cuckoo.clear();
                    if (USE_OPRF) {
                        cuckoo_insert_oprf_parallel_wrapped(cuckoo, server_imgs, lsh);
                    } else {
                        for (auto& img : server_imgs) {
                            cuckoo.insert(img, lsh);
                        }
                    }
                }
            }
            auto t_end = chrono::high_resolution_clock::now();
            chrono::duration<double, milli> dt = t_end - t_start;
            PHASE_LOG << "[phase] Filter build done seconds=" << (dt.count() / 1000.0)
                 << "\n" << std::flush;


            //PIR setup: creates PIR readers
            if (USE_PIR_DOUBLE) {
                PHASE_LOG << "[phase] PIR setup start mode=PIR_double filter=" << filter << "\n" << std::flush;
                auto pir_double_setup_start = chrono::high_resolution_clock::now();
                if (filter == "Bloom") {
                    pir_double_reader = build_bloom_pir_double_reader(bloom);
                } else if (filter == "Cuckoo") {
                    pir_double_reader = build_cuckoo_pir_double_reader(cuckoo);
                }
                auto pir_double_setup_end = chrono::high_resolution_clock::now();
                chrono::duration<double, milli> pir_double_setup_dt = pir_double_setup_end - pir_double_setup_start;
                pir_setup_time_ms_for_run += pir_double_setup_dt.count();
                PHASE_LOG << "[phase] PIR setup done mode=PIR_double seconds="
                     << (pir_double_setup_dt.count() / 1000.0) << "\n" << std::flush;
            }
            else if (USE_PIR_SINGLE) {
                PHASE_LOG << "[phase] PIR setup start mode=PIR_single filter=" << filter << "\n" << std::flush;
                auto pir_single_setup_start = chrono::high_resolution_clock::now();
                if (filter == "Bloom") {
                    pir_single_reader = build_bloom_pir_single_reader(bloom);
                } else if (filter == "Cuckoo") {
                    pir_single_reader = build_cuckoo_pir_single_reader(cuckoo);
                }
                auto pir_single_setup_end = chrono::high_resolution_clock::now();
                chrono::duration<double, milli> pir_single_setup_dt = pir_single_setup_end - pir_single_setup_start;
                pir_setup_time_ms_for_run += pir_single_setup_dt.count();
                PHASE_LOG << "[phase] PIR setup done mode=PIR_single seconds="
                     << (pir_single_setup_dt.count() / 1000.0) << "\n" << std::flush;
            }
            else if (USE_PIR_BATCHPIR) {
                PHASE_LOG << "[phase] PIR setup start mode=PIR_BatchPIR filter=" << filter << "\n" << std::flush;
                auto pir_batchpir_setup_start = chrono::high_resolution_clock::now();
                if (filter == "Bloom") {
                    pir_batchpir_reader = build_bloom_batchpir_reader(bloom);
                } else if (filter == "Cuckoo") {
                    if (cuckoo_batchpir_shard_count > 1) {
                        pir_batchpir_shard_readers.reserve(cuckoo_batchpir_shards.size());
                        for (size_t shard_idx = 0; shard_idx < cuckoo_batchpir_shards.size(); ++shard_idx) {
                            PHASE_LOG << "[phase] PIR setup shard start mode=PIR_BatchPIR"
                                 << " shard=" << (shard_idx + 1)
                                 << "/" << cuckoo_batchpir_shards.size()
                                 << " rows=" << cuckoo_batchpir_shards[shard_idx].bucket_count()
                                 << "\n" << std::flush;
                            pir_batchpir_shard_readers.push_back(
                                build_cuckoo_batchpir_reader(cuckoo_batchpir_shards[shard_idx])
                            );
                        }
                    } else {
                        pir_batchpir_reader = build_cuckoo_batchpir_reader(cuckoo);
                    }
                }
                auto pir_batchpir_setup_end = chrono::high_resolution_clock::now();
                chrono::duration<double, milli> pir_batchpir_setup_dt =
                    pir_batchpir_setup_end - pir_batchpir_setup_start;
                pir_setup_time_ms_for_run += pir_batchpir_setup_dt.count();
                PHASE_LOG << "[phase] PIR setup done mode=PIR_BatchPIR seconds="
                     << (pir_batchpir_setup_dt.count() / 1000.0) << "\n" << std::flush;
            }

            //if (run == NUM_RUNS_PSI-1) {
            //    debug_print_dataset(server_ids, server_imgs, client_imgs, client_is_close, client_relates_to, 10);
            //}


            int FP = 0, TN = 0, TP = 0, FN = 0;

            auto q_start = chrono::high_resolution_clock::now();
            std::vector<std::vector<uint64_t>> client_oprf_bucket_keys;
            std::vector<std::vector<CuckooFilter::Lookup>> client_oprf_cuckoo_lookups;

            // OPRF setup
            if (USE_OPRF) {
                PHASE_LOG << "[phase] Client OPRF lookup precompute start client=" << client_imgs.size()
                     << "\n" << std::flush;
                const char* pir_label = active_pir_label();
                if (filter == "Bloom") {
                    print_bloom_oprf_query_sample(client_imgs, lsh, pir_label);
                    client_oprf_bucket_keys =
                        parallel_oprf_bucket_key_batches(client_imgs, lsh);
                } else if (filter == "Cuckoo") {
                    if (USE_PIR_BATCHPIR && cuckoo_batchpir_shard_count > 1) {
                        print_cuckoo_oprf_query_sample(
                            cuckoo_batchpir_shards.front(),
                            client_imgs,
                            lsh,
                            pir_label
                        );
                    } else {
                        print_cuckoo_oprf_query_sample(cuckoo, client_imgs, lsh, pir_label);
                        client_oprf_cuckoo_lookups =
                            parallel_cuckoo_oprf_lookup_batches(cuckoo, client_imgs, lsh);
                    }
                }
                PHASE_LOG << "[phase] Client OPRF lookup precompute done\n" << std::flush;
            }

             /************************
            Step 3: Membership Query 
            ************************/
            std::vector<bool> batched_reported;

            //PIR modes
            if (USE_BATCH && USE_PIR_DOUBLE) {
                PHASE_LOG << "[phase] Batched membership query start mode=PIR_double client="
                     << client_imgs.size() << "\n" << std::flush;
                batched_reported = batched_pir_memberships_for_reader(
                    filter,
                    bloom,
                    cuckoo,
                    client_imgs,
                    lsh,
                    client_oprf_bucket_keys,
                    client_oprf_cuckoo_lookups,
                    *pir_double_reader
                );
                PHASE_LOG << "[phase] Batched membership query done mode=PIR_double results="
                     << batched_reported.size() << "\n" << std::flush;
            } else if (USE_BATCH && USE_PIR_SINGLE) {
                PHASE_LOG << "[phase] Batched membership query start mode=PIR_single client="
                     << client_imgs.size() << "\n" << std::flush;
                batched_reported = batched_pir_memberships_for_reader(
                    filter,
                    bloom,
                    cuckoo,
                    client_imgs,
                    lsh,
                    client_oprf_bucket_keys,
                    client_oprf_cuckoo_lookups,
                    *pir_single_reader
                );
                PHASE_LOG << "[phase] Batched membership query done mode=PIR_single results="
                     << batched_reported.size() << "\n" << std::flush;
            } else if (USE_BATCH && USE_PIR_BATCHPIR) {
                PHASE_LOG << "[phase] Batched membership query start mode=PIR_BatchPIR client="
                     << client_imgs.size() << "\n" << std::flush;
                if (filter == "Cuckoo" && cuckoo_batchpir_shard_count > 1) {
                    batched_reported = sharded_cuckoo_batchpir_memberships(
                        cuckoo_batchpir_shards,
                        pir_batchpir_shard_readers,
                        client_imgs,
                        lsh
                    );
                } else {
                    batched_reported = batched_pir_memberships_for_reader(
                        filter,
                        bloom,
                        cuckoo,
                        client_imgs,
                        lsh,
                        client_oprf_bucket_keys,
                        client_oprf_cuckoo_lookups,
                        *pir_batchpir_reader
                    );
                }
                PHASE_LOG << "[phase] Batched membership query done mode=PIR_BatchPIR results="
                     << batched_reported.size() << "\n" << std::flush;
            }
             /************************
            Step 4: Membership Evaluation 
            ************************/


            PHASE_LOG << "[phase] Membership evaluation start client=" << client_imgs.size()
                 << " use_batch=" << (USE_BATCH ? "true" : "false")
                 << "\n" << std::flush;


            for (size_t i = 0; i < client_imgs.size(); i++) {
                bool is_close = client_is_close[i]; //take gorund truth
                bool is_far = !is_close;

                bool reported = false;
                if (filter == "Bloom") {
                    if (USE_BATCH && (USE_PIR_DOUBLE || USE_PIR_SINGLE || USE_PIR_BATCHPIR)) {
                        reported = batched_reported[i];
                    } else if (USE_PIR_DOUBLE) {
                        reported = USE_OPRF
                            ? bloom_membership_pir_double_keys_parallel(
                                  bloom,
                                  client_oprf_bucket_keys[i],
                                  *pir_double_reader,
                                  pir_double_reader_copies
                              )
                            : bloom_membership_pir_double_parallel(
                                  bloom,
                                  client_imgs[i],
                                  lsh,
                                  *pir_double_reader,
                                  pir_double_reader_copies
                              );
                    } else if (USE_PIR_SINGLE) {
                        reported = USE_OPRF
                            ? bloom_membership_pir_single_keys_parallel(
                                  bloom,
                                  client_oprf_bucket_keys[i],
                                  *pir_single_reader,
                                  pir_single_reader_copies
                              )
                            : bloom_membership_pir_single_parallel(
                                  bloom,
                                  client_imgs[i],
                                  lsh,
                                  *pir_single_reader,
                                  pir_single_reader_copies
                              );
                    } else {
                        reported = bloom.membership(client_imgs[i], lsh);
                    }
                }
                else if (filter == "Cuckoo") {
                    if (USE_BATCH && (USE_PIR_DOUBLE || USE_PIR_SINGLE || USE_PIR_BATCHPIR)) {
                        reported = batched_reported[i]; //take reported value
                    } else if (USE_PIR_DOUBLE) {
                        const auto lookups = USE_OPRF
                            ? client_oprf_cuckoo_lookups[i]
                            : cuckoo.lookup_buckets(client_imgs[i], lsh);
                        reported = cuckoo_membership_pir_double_lookups_parallel(
                            cuckoo,
                            lookups,
                            *pir_double_reader,
                            pir_double_reader_copies
                        );
                    } else if (USE_PIR_SINGLE) {
                        const auto lookups = USE_OPRF
                            ? client_oprf_cuckoo_lookups[i]
                            : cuckoo.lookup_buckets(client_imgs[i], lsh);
                        reported = cuckoo_membership_pir_single_lookups_parallel(
                            cuckoo,
                            lookups,
                            *pir_single_reader,
                            pir_single_reader_copies
                        );
                    } else {
                        reported = cuckoo.membership(client_imgs[i], lsh);
                    }
                }
                //comapare and form statistics
                if (is_far) {
                    if (reported) FP++;
                    else TN++;
                }
                if (is_close) {
                    if (reported) TP++;
                    else FN++;
                }
            }

            /************************
            Step 5: Timing
            ************************/
            auto q_end = chrono::high_resolution_clock::now();
            chrono::duration<double, milli> q_dt = q_end - q_start;
            PHASE_LOG << "[phase] Membership evaluation done seconds=" << (q_dt.count() / 1000.0)
                 << " FP=" << FP
                 << " TN=" << TN
                 << " TP=" << TP
                 << " FN=" << FN
                 << "\n" << std::flush;
            double pir_server_answer_time_ms_for_run = 0.0;
            std::uint64_t pir_setup_communication_bytes_for_run = 0;
            std::uint64_t pir_query_communication_bytes_for_run = 0;
            std::uint64_t pir_response_communication_bytes_for_run = 0;
            if (USE_PIR_DOUBLE) {
                pir_server_answer_time_ms_for_run = pir_double_server_runtime_ms(
                    pir_double_reader,
                    pir_double_reader_copies
                );
                pir_setup_communication_bytes_for_run =
                    pir_reader_setup_communication_bytes(
                        pir_double_reader,
                        pir_double_reader_copies
                    );
                pir_query_communication_bytes_for_run =
                    pir_reader_query_communication_bytes(
                        pir_double_reader,
                        pir_double_reader_copies
                    );
                pir_response_communication_bytes_for_run =
                    pir_reader_response_communication_bytes(
                        pir_double_reader,
                        pir_double_reader_copies
                    );
            } else if (USE_PIR_SINGLE) {
                pir_server_answer_time_ms_for_run = pir_single_server_runtime_ms(
                    pir_single_reader,
                    pir_single_reader_copies
                );
                pir_setup_communication_bytes_for_run =
                    pir_reader_setup_communication_bytes(
                        pir_single_reader,
                        pir_single_reader_copies
                    );
                pir_query_communication_bytes_for_run =
                    pir_reader_query_communication_bytes(
                        pir_single_reader,
                        pir_single_reader_copies
                    );
                pir_response_communication_bytes_for_run =
                    pir_reader_response_communication_bytes(
                        pir_single_reader,
                        pir_single_reader_copies
                    );
            } else if (USE_PIR_BATCHPIR) {
                if (!pir_batchpir_shard_readers.empty()) {
                    size_t max_bucket_size_for_run = 0;
                    size_t max_first_dimension_for_run = 0;
                    for (const auto& reader : pir_batchpir_shard_readers) {
                        if (!reader) {
                            continue;
                        }
                        pir_server_answer_time_ms_for_run += reader->server_runtime_ms();
                        pir_setup_communication_bytes_for_run +=
                            reader->setup_communication_bytes();
                        pir_query_communication_bytes_for_run +=
                            reader->query_communication_bytes();
                        pir_response_communication_bytes_for_run +=
                            reader->response_communication_bytes();
                        batchpir_num_buckets_sum +=
                            static_cast<long double>(reader->batchpir_num_buckets());
                        max_bucket_size_for_run = std::max(
                            max_bucket_size_for_run,
                            reader->batchpir_max_bucket_size()
                        );
                        max_first_dimension_for_run = std::max(
                            max_first_dimension_for_run,
                            reader->batchpir_first_dimension_size()
                        );
                        batchpir_server_count_sum +=
                            static_cast<long double>(reader->batchpir_server_count());
                    }
                    batchpir_max_bucket_size_sum +=
                        static_cast<long double>(max_bucket_size_for_run);
                    batchpir_first_dimension_size_sum +=
                        static_cast<long double>(max_first_dimension_for_run);
                } else if (pir_batchpir_reader) {
                    pir_server_answer_time_ms_for_run = pir_batchpir_reader->server_runtime_ms();
                    pir_setup_communication_bytes_for_run =
                        pir_batchpir_reader->setup_communication_bytes();
                    pir_query_communication_bytes_for_run =
                        pir_batchpir_reader->query_communication_bytes();
                    pir_response_communication_bytes_for_run =
                        pir_batchpir_reader->response_communication_bytes();
                    batchpir_num_buckets_sum +=
                        static_cast<long double>(pir_batchpir_reader->batchpir_num_buckets());
                    batchpir_max_bucket_size_sum +=
                        static_cast<long double>(pir_batchpir_reader->batchpir_max_bucket_size());
                    batchpir_first_dimension_size_sum +=
                        static_cast<long double>(pir_batchpir_reader->batchpir_first_dimension_size());
                    batchpir_server_count_sum +=
                        static_cast<long double>(pir_batchpir_reader->batchpir_server_count());
                }
            }
            pir_server_answer_time_ms_for_run = std::max(
                0.0,
                std::min(pir_server_answer_time_ms_for_run, q_dt.count())
            );
            const DiscoGcaesOprfStats oprf_stats_for_run =
                disco_gcaes_oprf_stats_delta(oprf_stats_before_run, disco_gcaes_oprf_stats());
            const LocalOprfServerPrfStats local_oprf_server_prf_stats_for_run =
                local_oprf_server_prf_stats_delta(
                    local_oprf_server_prf_stats_before_run,
                    local_oprf_server_prf_stats()
                );
            const std::uint64_t oprf_communication_bytes_for_run =
                oprf_stats_for_run.bytes_sent + oprf_stats_for_run.bytes_recv;
            if (USE_OPRF) {
                PHASE_LOG << "[phase] Interactive " << disco_oprf_mechanism()
                     << " OPRF measured calls="
                     << oprf_stats_for_run.calls
                     << " blocks=" << oprf_stats_for_run.blocks
                     << " runtime_s=" << (oprf_stats_for_run.runtime_ms / 1000.0)
                     << " comm_mb="
                     << (static_cast<double>(oprf_communication_bytes_for_run) / 1'000'000.0)
                     << "\n" << std::flush;
                if (!OPRF_SERVER_INTERACTIVE) {
                    PHASE_LOG << "[phase] Server local " << disco_oprf_mechanism()
                         << " PRF measured calls="
                         << local_oprf_server_prf_stats_for_run.calls
                         << " blocks=" << local_oprf_server_prf_stats_for_run.blocks
                         << " runtime_s="
                         << (local_oprf_server_prf_stats_for_run.runtime_ms / 1000.0)
                         << "\n" << std::flush;
                }
            }

            const double total_runtime_ms_for_run =
                dt.count() + pir_setup_time_ms_for_run + q_dt.count();
            const double client_runtime_ms_for_run =
                std::max(0.0, q_dt.count() - pir_server_answer_time_ms_for_run);
            const double server_runtime_ms_for_run =
                dt.count() + pir_setup_time_ms_for_run + pir_server_answer_time_ms_for_run;
            const double server_offline_runtime_ms_for_run =
                dt.count() + pir_setup_time_ms_for_run;
            const double server_online_runtime_ms_for_run =
                pir_server_answer_time_ms_for_run;
            const std::uint64_t total_communication_bytes_for_run =
                pir_setup_communication_bytes_for_run +
                pir_query_communication_bytes_for_run +
                pir_response_communication_bytes_for_run +
                oprf_communication_bytes_for_run;

            total_runtime_ms_sum += total_runtime_ms_for_run;
            client_runtime_ms_sum += client_runtime_ms_for_run;
            server_runtime_ms_sum += server_runtime_ms_for_run;
            server_offline_runtime_ms_sum += server_offline_runtime_ms_for_run;
            server_online_runtime_ms_sum += server_online_runtime_ms_for_run;
            filter_build_runtime_ms_sum += dt.count();
            pir_setup_runtime_ms_sum += pir_setup_time_ms_for_run;
            oprf_runtime_ms_sum += oprf_stats_for_run.runtime_ms;
            communication_bytes_sum += static_cast<long double>(total_communication_bytes_for_run);
            pir_setup_communication_bytes_sum +=
                static_cast<long double>(pir_setup_communication_bytes_for_run);
            pir_query_communication_bytes_sum +=
                static_cast<long double>(pir_query_communication_bytes_for_run);
            pir_response_communication_bytes_sum +=
                static_cast<long double>(pir_response_communication_bytes_for_run);
            oprf_communication_bytes_sum +=
                static_cast<long double>(oprf_communication_bytes_for_run);
            oprf_client_to_server_bytes_sum +=
                static_cast<long double>(oprf_stats_for_run.bytes_sent);
            oprf_server_to_client_bytes_sum +=
                static_cast<long double>(oprf_stats_for_run.bytes_recv);
            oprf_calls_sum += static_cast<long double>(oprf_stats_for_run.calls);
            oprf_blocks_sum += static_cast<long double>(oprf_stats_for_run.blocks);
            local_oprf_server_prf_runtime_ms_sum +=
                local_oprf_server_prf_stats_for_run.runtime_ms;
            local_oprf_server_prf_calls_sum +=
                static_cast<long double>(local_oprf_server_prf_stats_for_run.calls);
            local_oprf_server_prf_blocks_sum +=
                static_cast<long double>(local_oprf_server_prf_stats_for_run.blocks);

            const double fp_rate = (FP + TN) ? double(FP) / double(FP + TN) : 0.0;
            const double fn_rate = (TP + FN) ? double(FN) / double(TP + FN) : 0.0;

            fp_rate_sum += fp_rate;
            fn_rate_sum += fn_rate;
            fp_count_sum += static_cast<long double>(FP);
            tn_count_sum += static_cast<long double>(TN);
            tp_count_sum += static_cast<long double>(TP);
            fn_count_sum += static_cast<long double>(FN);
            if (filter == "Cuckoo") {
                if (USE_PIR_BATCHPIR && cuckoo_batchpir_shard_count > 1) {
                    size_t attempted_inserts = 0;
                    size_t failed_inserts = 0;
                    for (const auto& shard : cuckoo_batchpir_shards) {
                        attempted_inserts += shard.attempted_insert_count();
                        failed_inserts += shard.failed_insert_count();
                    }
                    cuckoo_failed_sum += static_cast<long double>(failed_inserts);
                    cuckoo_fail_rate_sum += attempted_inserts
                        ? static_cast<long double>(failed_inserts) /
                              static_cast<long double>(attempted_inserts)
                        : 0.0L;
                } else {
                    cuckoo_failed_sum += static_cast<long double>(cuckoo.failed_insert_count());
                    cuckoo_fail_rate_sum += static_cast<long double>(cuckoo.failed_insert_rate());
                }
            }
           /*
            cout << "[run] client_size=" << client_size
                 << " | FP=" << FP << " TN=" << TN
                 << " TP=" << TP << " FN=" << FN
                 << " | fp_rate=" << fp_rate
                 << " fn_rate=" << fn_rate
                 << "\n";
           */
        }

        const double avg_fp = fp_rate_sum / NUM_RUNS_PSI;
        const double avg_fn = fn_rate_sum / NUM_RUNS_PSI;
        const double avg_total_runtime_s = (total_runtime_ms_sum / NUM_RUNS_PSI) / 1000.0;
        const double avg_client_runtime_s = (client_runtime_ms_sum / NUM_RUNS_PSI) / 1000.0;
        const double avg_server_runtime_s = (server_runtime_ms_sum / NUM_RUNS_PSI) / 1000.0;
        const double avg_server_offline_runtime_s =
            (server_offline_runtime_ms_sum / NUM_RUNS_PSI) / 1000.0;
        const double avg_server_online_runtime_s =
            (server_online_runtime_ms_sum / NUM_RUNS_PSI) / 1000.0;
        const double avg_filter_build_runtime_s =
            (filter_build_runtime_ms_sum / NUM_RUNS_PSI) / 1000.0;
        const double avg_pir_setup_runtime_s =
            (pir_setup_runtime_ms_sum / NUM_RUNS_PSI) / 1000.0;
        const double avg_oprf_runtime_s =
            (oprf_runtime_ms_sum / NUM_RUNS_PSI) / 1000.0;
        const double filter_size_mb = static_cast<double>(filter_total_size_bits) / 8.0 / 1'000'000.0;
        const double avg_communication_mb =
            static_cast<double>((communication_bytes_sum / NUM_RUNS_PSI) / 1'000'000.0L);
        const double avg_pir_setup_communication_mb =
            static_cast<double>((pir_setup_communication_bytes_sum / NUM_RUNS_PSI) / 1'000'000.0L);
        const double avg_pir_query_communication_mb =
            static_cast<double>((pir_query_communication_bytes_sum / NUM_RUNS_PSI) / 1'000'000.0L);
        const double avg_pir_response_communication_mb =
            static_cast<double>((pir_response_communication_bytes_sum / NUM_RUNS_PSI) / 1'000'000.0L);
        const double avg_oprf_communication_mb =
            static_cast<double>((oprf_communication_bytes_sum / NUM_RUNS_PSI) / 1'000'000.0L);
        const double avg_oprf_client_to_server_mb =
            static_cast<double>((oprf_client_to_server_bytes_sum / NUM_RUNS_PSI) / 1'000'000.0L);
        const double avg_oprf_server_to_client_mb =
            static_cast<double>((oprf_server_to_client_bytes_sum / NUM_RUNS_PSI) / 1'000'000.0L);
        const double avg_oprf_calls =
            static_cast<double>(oprf_calls_sum / NUM_RUNS_PSI);
        const double avg_oprf_blocks =
            static_cast<double>(oprf_blocks_sum / NUM_RUNS_PSI);
        const double avg_local_oprf_server_prf_runtime_s =
            (local_oprf_server_prf_runtime_ms_sum / NUM_RUNS_PSI) / 1000.0;
        const double avg_local_oprf_server_prf_calls =
            static_cast<double>(local_oprf_server_prf_calls_sum / NUM_RUNS_PSI);
        const double avg_local_oprf_server_prf_blocks =
            static_cast<double>(local_oprf_server_prf_blocks_sum / NUM_RUNS_PSI);
        const double avg_batchpir_num_buckets =
            static_cast<double>(batchpir_num_buckets_sum / NUM_RUNS_PSI);
        const double avg_batchpir_max_bucket_size =
            static_cast<double>(batchpir_max_bucket_size_sum / NUM_RUNS_PSI);
        const double avg_batchpir_first_dimension_size =
            static_cast<double>(batchpir_first_dimension_size_sum / NUM_RUNS_PSI);
        const double avg_batchpir_server_count =
            static_cast<double>(batchpir_server_count_sum / NUM_RUNS_PSI);
        const double avg_fp_count = static_cast<double>(fp_count_sum / NUM_RUNS_PSI);
        const double avg_tn_count = static_cast<double>(tn_count_sum / NUM_RUNS_PSI);
        const double avg_tp_count = static_cast<double>(tp_count_sum / NUM_RUNS_PSI);
        const double avg_fn_count = static_cast<double>(fn_count_sum / NUM_RUNS_PSI);
        const double avg_cuckoo_failed = static_cast<double>(cuckoo_failed_sum / NUM_RUNS_PSI);
        const double avg_cuckoo_fail_rate = static_cast<double>(cuckoo_fail_rate_sum / NUM_RUNS_PSI);
        const double avg_num_correct = avg_tp_count + avg_fn_count;
        const double avg_num_matched = avg_tp_count + avg_fp_count;
        const double avg_cardinality_abs_error =
            std::abs(avg_num_correct - avg_num_matched);

        cout << "\n[RUNTIME AVG] mode=" << mode
             << " filter=" << filter
             << " server_size=" << N
             << " client_size=" << client_size
             << "\n"
             << "  Filter Size: "
             << filter_size_mb << " MB"
             << "\n"
             << "  FP Rate: "
             << avg_fp
             << "\n"
             << "  FN Rate: "
             << avg_fn
             << "\n"
             << "  Avg FP Count: "
             << avg_fp_count
             << "\n"
             << "  Avg TN Count: "
             << avg_tn_count
             << "\n"
             << "  Avg TP Count: "
             << avg_tp_count
             << "\n"
             << "  Avg FN Count: "
             << avg_fn_count
             << "\n"
             << "  Avg Cuckoo Failed Inserts: "
             << avg_cuckoo_failed
             << "\n"
             << "  Avg Cuckoo Fail Rate: "
             << avg_cuckoo_fail_rate
             << "\n"
             << "  Num Correct: "
             << avg_num_correct
             << "\n"
             << "  Num Matched: "
             << avg_num_matched
             << "\n"
             << "  Cardinality Abs Error: "
             << avg_cardinality_abs_error
             << "\n"
             << "  Total Runtime (build"
             << (USE_OPRF
                     ? (OPRF_SERVER_INTERACTIVE
                            ? (std::string(" + ") + disco_oprf_mechanism() + " OPRF")
                            : (std::string(" + Client ") + disco_oprf_mechanism() + " OPRF"))
                     : "")
             << " + PIR setup + Client query/results): "
             << avg_total_runtime_s << " s"
             << "\n"
             << "  Client Runtime (query generation + decode/results): "
             << avg_client_runtime_s << " s"
             << "\n"
             << "  Server Runtime (filter construction + PIR setup/answers): "
             << avg_server_runtime_s << " s"
             << "\n"
             << "  Server Offline Runtime (filter construction + PIR setup): "
             << avg_server_offline_runtime_s << " s"
             << "\n"
             << "  Server Online Runtime (PIR answers): "
             << avg_server_online_runtime_s << " s"
             << "\n"
             << "  Filter Build Runtime: "
             << avg_filter_build_runtime_s << " s"
             << "\n"
             << "  PIR Setup Runtime: "
             << avg_pir_setup_runtime_s << " s"
             << "\n";
        if (USE_OPRF) {
            cout << "  Interactive " << disco_oprf_mechanism()
                 << " OPRF Runtime (included in total): "
                 << avg_oprf_runtime_s << " s"
                 << "\n"
                 << "  Interactive " << disco_oprf_mechanism()
                 << " OPRF Communication Cost (included in total): "
                 << avg_oprf_communication_mb << " MB"
                 << "\n"
                 << "  Interactive " << disco_oprf_mechanism()
                 << " OPRF Client -> Server Communication: "
                 << avg_oprf_client_to_server_mb << " MB"
                 << "\n"
                 << "  Interactive " << disco_oprf_mechanism()
                 << " OPRF Server -> Client Communication: "
                 << avg_oprf_server_to_client_mb << " MB"
                 << "\n"
                 << "  Interactive " << disco_oprf_mechanism()
                 << " OPRF Calls: "
                 << avg_oprf_calls
                 << "\n"
                 << "  Interactive " << disco_oprf_mechanism()
                 << " OPRF Blocks: "
                 << avg_oprf_blocks
                 << "\n";
            if (!OPRF_SERVER_INTERACTIVE) {
                cout << "  Server Local " << disco_oprf_mechanism()
                     << " PRF Runtime (included in filter build): "
                     << avg_local_oprf_server_prf_runtime_s << " s"
                     << "\n"
                     << "  Server Local " << disco_oprf_mechanism()
                     << " PRF Communication Cost: 0 MB"
                     << "\n"
                     << "  Server Local " << disco_oprf_mechanism()
                     << " PRF Calls: "
                     << avg_local_oprf_server_prf_calls
                     << "\n"
                     << "  Server Local " << disco_oprf_mechanism()
                     << " PRF Blocks: "
                     << avg_local_oprf_server_prf_blocks
                     << "\n";
            }
        }
        if (USE_PIR_DOUBLE || USE_PIR_SINGLE || USE_PIR_BATCHPIR || USE_OPRF) {
            cout << "  Total Communication Cost"
                 << (USE_OPRF
                         ? (std::string(" (PIR + interactive ") +
                            disco_oprf_mechanism() + " OPRF)")
                         : "")
                 << ": "
                 << avg_communication_mb << " MB"
                 << "\n"
                 << "  PIR Query Communication Cost (Client -> Server): "
                 << avg_pir_query_communication_mb << " MB"
                 << "\n"
                 << "  PIR Answer Communication Cost (Server -> Client): "
                 << avg_pir_response_communication_mb << " MB"
                 << "\n"
                 << "  PIR Setup Exchange Cost (metadata/keys, not DB build): "
                 << avg_pir_setup_communication_mb << " MB"
                 << "\n";
        }
        if (USE_PIR_BATCHPIR) {
            cout << "  BatchPIR Num Buckets: "
                 << avg_batchpir_num_buckets
                 << "\n"
                 << "  BatchPIR Max Bucket Size: "
                 << avg_batchpir_max_bucket_size
                 << "\n"
                 << "  BatchPIR First Dimension Size: "
                 << avg_batchpir_first_dimension_size
                 << "\n"
                 << "  BatchPIR Server Count: "
                 << avg_batchpir_server_count
                 << "\n";
        }
    }

    return 0;
}
