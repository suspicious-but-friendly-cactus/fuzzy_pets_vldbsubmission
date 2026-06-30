template <typename RowReader>
static bool bloom_membership_pir_keys_all(
    const BloomFilter& bloom,
    const std::vector<uint64_t>& keys,
    RowReader& reader
) {
    std::vector<std::vector<size_t>> key_row_positions;
    std::vector<size_t> row_indices;
    key_row_positions.reserve(keys.size());

    for (uint64_t key : keys) {
        std::vector<size_t> indices = bloom.indices_for_key(key);
        std::vector<size_t> positions;
        positions.reserve(indices.size());
        for (size_t idx : indices) {
            positions.push_back(row_indices.size());
            row_indices.push_back(idx);
        }
        key_row_positions.push_back(std::move(positions));
    }

    std::vector<pir_punc::Row> rows = reader.read_rows(row_indices);
    if (rows.size() != row_indices.size()) {
        throw std::runtime_error("Bloom PIR response size mismatch");
    }

    for (const auto& positions : key_row_positions) {
        bool present = true;
        for (size_t row_pos : positions) {
            const pir_punc::Row& row = rows[row_pos];
            if (row.empty() || row[0] == 0) {
                present = false;
                break;
            }
        }

        if (present) return true;
    }

    return false;
}

template <typename RowReader>
static bool cuckoo_membership_pir_lookups_all(
    const CuckooFilter& cuckoo,
    const std::vector<CuckooFilter::Lookup>& lookups,
    RowReader& reader
) {
    std::vector<size_t> row_indices;
    row_indices.reserve(lookups.size() * 2);
    for (const auto& lookup : lookups) {
        row_indices.push_back(lookup.i1);
        row_indices.push_back(lookup.i2);
    }

    std::vector<pir_punc::Row> rows = reader.read_rows(row_indices);
    if (rows.size() != row_indices.size()) {
        throw std::runtime_error("Cuckoo PIR response size mismatch");
    }

    for (size_t lookup_idx = 0; lookup_idx < lookups.size(); ++lookup_idx) {
        const CuckooFilter::Lookup& lookup = lookups[lookup_idx];
        const pir_punc::Row& row1 = rows[2 * lookup_idx];
        const pir_punc::Row& row2 = rows[2 * lookup_idx + 1];
        if (cuckoo.row_contains_fingerprint(row1, lookup.fp) ||
            cuckoo.row_contains_fingerprint(row2, lookup.fp)) {
            return true;
        }
    }

    return false;
}

static bool bloom_membership_pir_double_keys(
    const BloomFilter& bloom,
    const std::vector<uint64_t>& keys,
    PirDoubleRowReaderBase& pir_double
) {
    return bloom_membership_pir_keys_all(bloom, keys, pir_double);
}

static PirDoubleRowReaderBase& pir_double_reader_for_worker(
    PirDoubleRowReaderBase& primary,
    std::vector<std::unique_ptr<PirDoubleRowReaderBase>>& copies,
    size_t worker_idx
) {
    if (worker_idx == 0) {
        return primary;
    }
    return *copies.at(worker_idx - 1);
}

static bool bloom_key_present_pir_double_parallel(
    const std::vector<size_t>& indices,
    PirDoubleRowReaderBase& pir_double,
    std::vector<std::unique_ptr<PirDoubleRowReaderBase>>& copies
) {
    const size_t available_readers = copies.size() + 1;
    const size_t workers = std::min(indices.size(), available_readers);
    print_parallel_launch("Bloom PIR_double k_star row checks", indices.size(), workers);

    if (workers <= 1) {
        for (size_t idx : indices) {
            pir_punc::Row row = pir_double.read_row(idx);
            if (row.empty() || row[0] == 0) {
                return false;
            }
        }
        return true;
    }

    std::atomic<size_t> next{0};
    std::atomic<bool> present{true};
    std::mutex exception_mutex;
    std::exception_ptr first_exception = nullptr;
    std::vector<std::thread> threads;
    threads.reserve(workers);

    for (size_t worker = 0; worker < workers; ++worker) {
        threads.emplace_back([&, worker]() {
            try {
                PirDoubleRowReaderBase& reader =
                    pir_double_reader_for_worker(pir_double, copies, worker);
                while (present.load(std::memory_order_relaxed)) {
                    const size_t pos = next.fetch_add(1, std::memory_order_relaxed);
                    if (pos >= indices.size()) {
                        break;
                    }

                    pir_punc::Row row = reader.read_row(indices[pos]);
                    if (row.empty() || row[0] == 0) {
                        present.store(false, std::memory_order_relaxed);
                        break;
                    }
                }
            } catch (...) {
                {
                    std::lock_guard<std::mutex> lock(exception_mutex);
                    if (!first_exception) {
                        first_exception = std::current_exception();
                    }
                }
                present.store(false, std::memory_order_relaxed);
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }
    if (first_exception) {
        std::rethrow_exception(first_exception);
    }

    return present.load(std::memory_order_relaxed);
}

static bool bloom_membership_pir_double_keys_parallel(
    const BloomFilter& bloom,
    const std::vector<uint64_t>& keys,
    PirDoubleRowReaderBase& pir_double,
    std::vector<std::unique_ptr<PirDoubleRowReaderBase>>& copies
) {
    if (!USE_PARALLEL || copies.empty()) {
        return bloom_membership_pir_double_keys(bloom, keys, pir_double);
    }

    for (uint64_t key : keys) {
        std::vector<size_t> indices = bloom.indices_for_key(key);
        if (bloom_key_present_pir_double_parallel(indices, pir_double, copies)) {
            return true;
        }
    }

    return false;
}

[[maybe_unused]] static bool bloom_membership_pir_double(
    const BloomFilter& bloom,
    const std::vector<double>& img,
    const E2LSH& lsh,
    PirDoubleRowReaderBase& pir_double
) {
    return bloom_membership_pir_double_keys(bloom, lsh.table_keys(img), pir_double);
}

static bool bloom_membership_pir_double_parallel(
    const BloomFilter& bloom,
    const std::vector<double>& img,
    const E2LSH& lsh,
    PirDoubleRowReaderBase& pir_double,
    std::vector<std::unique_ptr<PirDoubleRowReaderBase>>& copies
) {
    return bloom_membership_pir_double_keys_parallel(
        bloom,
        lsh.table_keys(img),
        pir_double,
        copies
    );
}

[[maybe_unused]] static bool bloom_membership_pir_double_oprf(
    const BloomFilter& bloom,
    const std::vector<double>& img,
    const E2LSH& lsh,
    PirDoubleRowReaderBase& pir_double
) {
    if (!printed_bloom_oprf_query) {
        printed_bloom_oprf_query = true;
    }

    return bloom_membership_pir_double_keys(bloom, oprf_bucket_keys(img, lsh), pir_double);
}

static bool cuckoo_membership_pir_double_lookups(
    const CuckooFilter& cuckoo,
    const std::vector<CuckooFilter::Lookup>& lookups,
    PirDoubleRowReaderBase& pir_double
) {
    return cuckoo_membership_pir_lookups_all(cuckoo, lookups, pir_double);
}

static bool cuckoo_membership_pir_double_lookups_parallel(
    const CuckooFilter& cuckoo,
    const std::vector<CuckooFilter::Lookup>& lookups,
    PirDoubleRowReaderBase& pir_double,
    std::vector<std::unique_ptr<PirDoubleRowReaderBase>>& copies
) {
    if (!USE_PARALLEL || copies.empty()) {
        return cuckoo_membership_pir_double_lookups(cuckoo, lookups, pir_double);
    }

    const size_t candidate_count = lookups.size() * 2;
    if (candidate_count == 0) {
        return false;
    }

    const size_t available_readers = copies.size() + 1;
    const size_t workers = std::min(candidate_count, available_readers);
    print_parallel_launch("Cuckoo PIR_double candidate row checks", candidate_count, workers);

    if (workers <= 1) {
        return cuckoo_membership_pir_double_lookups(cuckoo, lookups, pir_double);
    }

    std::atomic<size_t> next{0};
    std::atomic<bool> found{false};
    std::mutex exception_mutex;
    std::exception_ptr first_exception = nullptr;
    std::vector<std::thread> threads;
    threads.reserve(workers);

    for (size_t worker = 0; worker < workers; ++worker) {
        threads.emplace_back([&, worker]() {
            try {
                PirDoubleRowReaderBase& reader =
                    pir_double_reader_for_worker(pir_double, copies, worker);
                while (!found.load(std::memory_order_relaxed)) {
                    const size_t candidate = next.fetch_add(1, std::memory_order_relaxed);
                    if (candidate >= candidate_count) {
                        break;
                    }

                    const CuckooFilter::Lookup& lookup = lookups[candidate / 2];
                    const size_t row_idx = (candidate % 2 == 0) ? lookup.i1 : lookup.i2;
                    pir_punc::Row row = reader.read_row(row_idx);
                    if (cuckoo.row_contains_fingerprint(row, lookup.fp)) {
                        found.store(true, std::memory_order_relaxed);
                        break;
                    }
                }
            } catch (...) {
                {
                    std::lock_guard<std::mutex> lock(exception_mutex);
                    if (!first_exception) {
                        first_exception = std::current_exception();
                    }
                }
                found.store(true, std::memory_order_relaxed);
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }
    if (first_exception) {
        std::rethrow_exception(first_exception);
    }

    return found.load(std::memory_order_relaxed);
}

[[maybe_unused]] static bool cuckoo_membership_pir_double(
    const CuckooFilter& cuckoo,
    const std::vector<double>& img,
    E2LSH& lsh,
    PirDoubleRowReaderBase& pir_double
) {
    return cuckoo_membership_pir_double_lookups(cuckoo, cuckoo.lookup_buckets(img, lsh), pir_double);
}

[[maybe_unused]] static bool cuckoo_membership_pir_double_oprf(
    const CuckooFilter& cuckoo,
    const std::vector<double>& img,
    const E2LSH& lsh,
    PirDoubleRowReaderBase& pir_double
) {
    if (!printed_cuckoo_oprf_query) {
        printed_cuckoo_oprf_query = true;
    }

    return cuckoo_membership_pir_double_lookups(cuckoo, cuckoo_oprf_lookups(cuckoo, img, lsh), pir_double);
}

static bool bloom_membership_pir_single_keys(
    const BloomFilter& bloom,
    const std::vector<uint64_t>& keys,
    PirSingleRowReaderBase& pir_single
) {
    return bloom_membership_pir_keys_all(bloom, keys, pir_single);
}

static PirSingleRowReaderBase& pir_single_reader_for_worker(
    PirSingleRowReaderBase& primary,
    std::vector<std::unique_ptr<PirSingleRowReaderBase>>& copies,
    size_t worker_idx
) {
    if (worker_idx == 0) {
        return primary;
    }
    return *copies.at(worker_idx - 1);
}

static bool bloom_key_present_pir_single_parallel(
    const std::vector<size_t>& indices,
    PirSingleRowReaderBase& pir_single,
    std::vector<std::unique_ptr<PirSingleRowReaderBase>>& copies
) {
    const size_t available_readers = copies.size() + 1;
    const size_t workers = std::min(indices.size(), available_readers);
    print_parallel_launch("Bloom PIR_single k_star row checks", indices.size(), workers);

    if (workers <= 1) {
        for (size_t idx : indices) {
            pir_punc::Row row = pir_single.read_row(idx);
            if (row.empty() || row[0] == 0) {
                return false;
            }
        }
        return true;
    }

    std::atomic<size_t> next{0};
    std::atomic<bool> present{true};
    std::mutex exception_mutex;
    std::exception_ptr first_exception = nullptr;
    std::vector<std::thread> threads;
    threads.reserve(workers);

    for (size_t worker = 0; worker < workers; ++worker) {
        threads.emplace_back([&, worker]() {
            try {
                PirSingleRowReaderBase& reader =
                    pir_single_reader_for_worker(pir_single, copies, worker);
                while (present.load(std::memory_order_relaxed)) {
                    const size_t pos = next.fetch_add(1, std::memory_order_relaxed);
                    if (pos >= indices.size()) {
                        break;
                    }

                    pir_punc::Row row = reader.read_row(indices[pos]);
                    if (row.empty() || row[0] == 0) {
                        present.store(false, std::memory_order_relaxed);
                        break;
                    }
                }
            } catch (...) {
                {
                    std::lock_guard<std::mutex> lock(exception_mutex);
                    if (!first_exception) {
                        first_exception = std::current_exception();
                    }
                }
                present.store(false, std::memory_order_relaxed);
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }
    if (first_exception) {
        std::rethrow_exception(first_exception);
    }

    return present.load(std::memory_order_relaxed);
}

static bool bloom_membership_pir_single_keys_parallel(
    const BloomFilter& bloom,
    const std::vector<uint64_t>& keys,
    PirSingleRowReaderBase& pir_single,
    std::vector<std::unique_ptr<PirSingleRowReaderBase>>& copies
) {
    if (!USE_PARALLEL || copies.empty()) {
        return bloom_membership_pir_single_keys(bloom, keys, pir_single);
    }

    for (uint64_t key : keys) {
        std::vector<size_t> indices = bloom.indices_for_key(key);
        if (bloom_key_present_pir_single_parallel(indices, pir_single, copies)) {
            return true;
        }
    }

    return false;
}

[[maybe_unused]] static bool bloom_membership_pir_single(
    const BloomFilter& bloom,
    const std::vector<double>& img,
    const E2LSH& lsh,
    PirSingleRowReaderBase& pir_single
) {
    return bloom_membership_pir_single_keys(bloom, lsh.table_keys(img), pir_single);
}

static bool bloom_membership_pir_single_parallel(
    const BloomFilter& bloom,
    const std::vector<double>& img,
    const E2LSH& lsh,
    PirSingleRowReaderBase& pir_single,
    std::vector<std::unique_ptr<PirSingleRowReaderBase>>& copies
) {
    return bloom_membership_pir_single_keys_parallel(
        bloom,
        lsh.table_keys(img),
        pir_single,
        copies
    );
}

[[maybe_unused]] static bool bloom_membership_pir_single_oprf(
    const BloomFilter& bloom,
    const std::vector<double>& img,
    const E2LSH& lsh,
    PirSingleRowReaderBase& pir_single
) {
    if (!printed_bloom_oprf_query) {
        printed_bloom_oprf_query = true;
    }

    return bloom_membership_pir_single_keys(bloom, oprf_bucket_keys(img, lsh), pir_single);
}

static bool cuckoo_membership_pir_single_lookups(
    const CuckooFilter& cuckoo,
    const std::vector<CuckooFilter::Lookup>& lookups,
    PirSingleRowReaderBase& pir_single
) {
    return cuckoo_membership_pir_lookups_all(cuckoo, lookups, pir_single);
}

static bool cuckoo_membership_pir_single_lookups_parallel(
    const CuckooFilter& cuckoo,
    const std::vector<CuckooFilter::Lookup>& lookups,
    PirSingleRowReaderBase& pir_single,
    std::vector<std::unique_ptr<PirSingleRowReaderBase>>& copies
) {
    if (!USE_PARALLEL || copies.empty()) {
        return cuckoo_membership_pir_single_lookups(cuckoo, lookups, pir_single);
    }

    const size_t candidate_count = lookups.size() * 2;
    if (candidate_count == 0) {
        return false;
    }

    const size_t available_readers = copies.size() + 1;
    const size_t workers = std::min(candidate_count, available_readers);
    print_parallel_launch("Cuckoo PIR_single candidate row checks", candidate_count, workers);

    if (workers <= 1) {
        return cuckoo_membership_pir_single_lookups(cuckoo, lookups, pir_single);
    }

    std::atomic<size_t> next{0};
    std::atomic<bool> found{false};
    std::mutex exception_mutex;
    std::exception_ptr first_exception = nullptr;
    std::vector<std::thread> threads;
    threads.reserve(workers);

    for (size_t worker = 0; worker < workers; ++worker) {
        threads.emplace_back([&, worker]() {
            try {
                PirSingleRowReaderBase& reader =
                    pir_single_reader_for_worker(pir_single, copies, worker);
                while (!found.load(std::memory_order_relaxed)) {
                    const size_t candidate = next.fetch_add(1, std::memory_order_relaxed);
                    if (candidate >= candidate_count) {
                        break;
                    }

                    const CuckooFilter::Lookup& lookup = lookups[candidate / 2];
                    const size_t row_idx = (candidate % 2 == 0) ? lookup.i1 : lookup.i2;
                    pir_punc::Row row = reader.read_row(row_idx);
                    if (cuckoo.row_contains_fingerprint(row, lookup.fp)) {
                        found.store(true, std::memory_order_relaxed);
                        break;
                    }
                }
            } catch (...) {
                {
                    std::lock_guard<std::mutex> lock(exception_mutex);
                    if (!first_exception) {
                        first_exception = std::current_exception();
                    }
                }
                found.store(true, std::memory_order_relaxed);
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }
    if (first_exception) {
        std::rethrow_exception(first_exception);
    }

    return found.load(std::memory_order_relaxed);
}

[[maybe_unused]] static bool cuckoo_membership_pir_single(
    const CuckooFilter& cuckoo,
    const std::vector<double>& img,
    E2LSH& lsh,
    PirSingleRowReaderBase& pir_single
) {
    return cuckoo_membership_pir_single_lookups(cuckoo, cuckoo.lookup_buckets(img, lsh), pir_single);
}

[[maybe_unused]] static bool cuckoo_membership_pir_single_oprf(
    const CuckooFilter& cuckoo,
    const std::vector<double>& img,
    const E2LSH& lsh,
    PirSingleRowReaderBase& pir_single
) {
    if (!printed_cuckoo_oprf_query) {
        printed_cuckoo_oprf_query = true;
    }

    return cuckoo_membership_pir_single_lookups(cuckoo, cuckoo_oprf_lookups(cuckoo, img, lsh), pir_single);
}

static size_t remember_unique_pir_row(
    size_t row_idx,
    std::vector<size_t>& unique_rows,
    std::unordered_map<size_t, size_t>& unique_pos_by_row
) {
    auto found = unique_pos_by_row.find(row_idx);
    if (found != unique_pos_by_row.end()) {
        return found->second;
    }

    const size_t pos = unique_rows.size();
    unique_pos_by_row.emplace(row_idx, pos);
    unique_rows.push_back(row_idx);
    return pos;
}

static std::vector<std::vector<uint64_t>> lsh_key_batches(
    const std::vector<std::vector<double>>& imgs,
    const E2LSH& lsh
) {
    std::vector<std::vector<uint64_t>> out(imgs.size());
    parallel_for_indices(imgs.size(), [&](size_t i) {
        out[i] = lsh.table_keys(imgs[i]);
    }, "Bloom query LSH bucket lookup");
    return out;
}

static std::vector<std::vector<CuckooFilter::Lookup>> cuckoo_lookup_batches(
    const CuckooFilter& cuckoo,
    const std::vector<std::vector<double>>& imgs,
    E2LSH& lsh
) {
    std::vector<std::vector<CuckooFilter::Lookup>> out(imgs.size());
    parallel_for_indices(imgs.size(), [&](size_t i) {
        out[i] = cuckoo.lookup_buckets(imgs[i], lsh);
    }, "Cuckoo query bucket lookup");
    return out;
}

template <typename T>
static void require_batch_count(
    const std::vector<std::vector<T>>& batches,
    size_t expected,
    const char* label
) {
    if (batches.size() != expected) {
        throw std::runtime_error(std::string(label) + ": batch count does not match Client item count");
    }
}

template <typename RowReader>
static std::vector<bool> bloom_membership_pir_batch_keys(
    const BloomFilter& bloom,
    const std::vector<std::vector<uint64_t>>& key_batches,
    RowReader& reader
) {
    std::vector<std::vector<std::vector<size_t>>> key_row_positions(key_batches.size());
    std::vector<size_t> unique_row_indices;
    std::unordered_map<size_t, size_t> unique_pos_by_row;

    for (size_t item_idx = 0; item_idx < key_batches.size(); ++item_idx) {
        key_row_positions[item_idx].reserve(key_batches[item_idx].size());
        for (uint64_t key : key_batches[item_idx]) {
            std::vector<size_t> positions;
            std::vector<size_t> indices = bloom.indices_for_key(key);
            positions.reserve(indices.size());
            for (size_t row_idx : indices) {
                positions.push_back(
                    remember_unique_pir_row(row_idx, unique_row_indices, unique_pos_by_row)
                );
            }
            key_row_positions[item_idx].push_back(std::move(positions));
        }
    }

    std::vector<pir_punc::Row> rows = reader.read_rows(unique_row_indices);
    if (rows.size() != unique_row_indices.size()) {
        throw std::runtime_error("Bloom PIR batch response size mismatch");
    }

    std::vector<bool> reported(key_batches.size(), false);
    for (size_t item_idx = 0; item_idx < key_row_positions.size(); ++item_idx) {
        for (const auto& positions : key_row_positions[item_idx]) {
            bool present = true;
            for (size_t row_pos : positions) {
                const pir_punc::Row& row = rows[row_pos];
                if (row.empty() || row[0] == 0) {
                    present = false;
                    break;
                }
            }
            if (present) {
                reported[item_idx] = true;
                break;
            }
        }
    }

    return reported;
}

struct BatchedCuckooLookup {
    uint32_t fp = 0;
    size_t row1_pos = 0;
    size_t row2_pos = 0;
};

template <typename RowReader>
static std::vector<bool> cuckoo_membership_pir_batch_lookups(
    const CuckooFilter& cuckoo,
    const std::vector<std::vector<CuckooFilter::Lookup>>& lookup_batches,
    RowReader& reader
) {
    std::vector<std::vector<BatchedCuckooLookup>> planned_lookups(lookup_batches.size());
    std::vector<size_t> unique_row_indices;
    std::unordered_map<size_t, size_t> unique_pos_by_row;

    for (size_t item_idx = 0; item_idx < lookup_batches.size(); ++item_idx) {
        planned_lookups[item_idx].reserve(lookup_batches[item_idx].size());
        for (const auto& lookup : lookup_batches[item_idx]) {
            planned_lookups[item_idx].push_back(BatchedCuckooLookup{
                lookup.fp,
                remember_unique_pir_row(lookup.i1, unique_row_indices, unique_pos_by_row),
                remember_unique_pir_row(lookup.i2, unique_row_indices, unique_pos_by_row)
            });
        }
    }

    std::vector<pir_punc::Row> rows = reader.read_rows(unique_row_indices);
    if (rows.size() != unique_row_indices.size()) {
        throw std::runtime_error("Cuckoo PIR batch response size mismatch");
    }

    std::vector<bool> reported(lookup_batches.size(), false);
    for (size_t item_idx = 0; item_idx < planned_lookups.size(); ++item_idx) {
        for (const auto& lookup : planned_lookups[item_idx]) {
            if (cuckoo.row_contains_fingerprint(rows[lookup.row1_pos], lookup.fp) ||
                cuckoo.row_contains_fingerprint(rows[lookup.row2_pos], lookup.fp)) {
                reported[item_idx] = true;
                break;
            }
        }
    }

    return reported;
}

template <typename RowReader>
static std::vector<bool> batched_pir_memberships_for_reader(
    const std::string& filter,
    const BloomFilter& bloom,
    const CuckooFilter& cuckoo,
    const std::vector<std::vector<double>>& client_imgs,
    E2LSH& lsh,
    const std::vector<std::vector<uint64_t>>& client_oprf_bucket_keys,
    const std::vector<std::vector<CuckooFilter::Lookup>>& client_oprf_cuckoo_lookups,
    RowReader& reader
) {
    if (filter == "Bloom") {
        if (USE_OPRF) {
            require_batch_count(client_oprf_bucket_keys, client_imgs.size(), "Bloom OPRF PIR batch");
            return bloom_membership_pir_batch_keys(bloom, client_oprf_bucket_keys, reader);
        }
        std::vector<std::vector<uint64_t>> key_batches = lsh_key_batches(client_imgs, lsh);
        return bloom_membership_pir_batch_keys(bloom, key_batches, reader);
    }

    if (filter == "Cuckoo") {
        if (USE_OPRF) {
            require_batch_count(client_oprf_cuckoo_lookups, client_imgs.size(), "Cuckoo OPRF PIR batch");
            return cuckoo_membership_pir_batch_lookups(cuckoo, client_oprf_cuckoo_lookups, reader);
        }
        std::vector<std::vector<CuckooFilter::Lookup>> lookup_batches =
            cuckoo_lookup_batches(cuckoo, client_imgs, lsh);
        return cuckoo_membership_pir_batch_lookups(cuckoo, lookup_batches, reader);
    }

    throw std::invalid_argument("Batched PIR membership is only implemented for Bloom and Cuckoo");
}

static std::vector<bool> sharded_cuckoo_batchpir_memberships(
    const std::vector<CuckooFilter>& cuckoo_shards,
    const std::vector<std::unique_ptr<fuzzy_pets_batchpir::RowReader>>& readers,
    const std::vector<std::vector<double>>& client_imgs,
    E2LSH& lsh
) {
    if (cuckoo_shards.size() != readers.size()) {
        throw std::runtime_error("Sharded Cuckoo BatchPIR filter/reader count mismatch");
    }
    if (cuckoo_shards.empty()) {
        throw std::runtime_error("Sharded Cuckoo BatchPIR requires at least one shard");
    }

    std::vector<bool> reported(client_imgs.size(), false);
    for (size_t shard_idx = 0; shard_idx < cuckoo_shards.size(); ++shard_idx) {
        if (!readers[shard_idx]) {
            throw std::runtime_error("Sharded Cuckoo BatchPIR reader is not initialized");
        }

        std::vector<std::vector<CuckooFilter::Lookup>> lookup_batches =
            USE_OPRF
                ? parallel_cuckoo_oprf_lookup_batches(cuckoo_shards[shard_idx], client_imgs, lsh)
                : cuckoo_lookup_batches(cuckoo_shards[shard_idx], client_imgs, lsh);
        std::vector<bool> shard_reported = cuckoo_membership_pir_batch_lookups(
            cuckoo_shards[shard_idx],
            lookup_batches,
            *readers[shard_idx]
        );
        if (shard_reported.size() != reported.size()) {
            throw std::runtime_error("Sharded Cuckoo BatchPIR result count mismatch");
        }

        for (size_t i = 0; i < reported.size(); ++i) {
            reported[i] = reported[i] || shard_reported[i];
        }
    }

    return reported;
}
