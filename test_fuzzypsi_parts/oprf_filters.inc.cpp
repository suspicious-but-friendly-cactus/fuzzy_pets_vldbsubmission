static bool printed_bloom_oprf_insert = false;
static bool printed_bloom_oprf_query = false;
static bool printed_cuckoo_oprf_insert = false;
static bool printed_cuckoo_oprf_query = false;
static constexpr size_t OPRF_INSERT_BATCH_SIZE = 4096;
static constexpr uint64_t OPRF_BUCKET_DOMAIN = 0x4255434b45545f31ULL; // "BUCKET_1"
static constexpr uint64_t OPRF_TAG_DOMAIN = 0x5441475f5f5f5f31ULL; // "TAG____1"
static constexpr uint64_t OPRF_CUCKOO_DOMAIN = 0x4355434b4f4f5f31ULL; // "CUCKOO_1"

struct LocalOprfServerPrfStats {
    double runtime_ms = 0.0;
    std::size_t calls = 0;
    std::size_t blocks = 0;
};

static std::mutex local_oprf_server_prf_stats_mutex;
static LocalOprfServerPrfStats local_oprf_server_prf_accumulated_stats;

static LocalOprfServerPrfStats local_oprf_server_prf_stats() {
    std::lock_guard<std::mutex> lock(local_oprf_server_prf_stats_mutex);
    return local_oprf_server_prf_accumulated_stats;
}

static LocalOprfServerPrfStats local_oprf_server_prf_stats_delta(
    const LocalOprfServerPrfStats& before,
    const LocalOprfServerPrfStats& after
) {
    auto delta_size = [](std::size_t a, std::size_t b) -> std::size_t {
        return a >= b ? a - b : 0;
    };

    LocalOprfServerPrfStats delta;
    delta.runtime_ms = std::max(0.0, after.runtime_ms - before.runtime_ms);
    delta.calls = delta_size(after.calls, before.calls);
    delta.blocks = delta_size(after.blocks, before.blocks);
    return delta;
}

static void add_local_oprf_server_prf_stats(double runtime_ms, std::size_t blocks) {
    std::lock_guard<std::mutex> lock(local_oprf_server_prf_stats_mutex);
    local_oprf_server_prf_accumulated_stats.runtime_ms += runtime_ms;
    local_oprf_server_prf_accumulated_stats.calls += 1;
    local_oprf_server_prf_accumulated_stats.blocks += blocks;
}

static uint64_t mix_domain_table(uint64_t domain, size_t table_idx) {
    uint64_t x = domain ^ (static_cast<uint64_t>(table_idx) + 0x9e3779b97f4a7c15ULL);
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

static std::array<uint8_t, 16> disco_oprf_input_block(
    uint64_t domain,
    size_t table_idx,
    uint64_t lsh_key
) {
    std::array<uint8_t, 16> block{};
    const uint64_t left = mix_domain_table(domain, table_idx);
    const uint64_t right = lsh_key;
    std::memcpy(block.data(), &left, sizeof(left));
    std::memcpy(block.data() + sizeof(left), &right, sizeof(right));
    return block;
}

static std::array<uint8_t, 16> disco_oprf_block(
    uint64_t domain,
    size_t table_idx,
    uint64_t lsh_key
) {
    const auto input = disco_oprf_input_block(domain, table_idx, lsh_key);
    std::array<uint8_t, 16> output{};
    disco_oprf_eval_blocks(input.data(), 1, output.data());
    return output;
}

static std::vector<std::array<uint8_t, 16>> disco_oprf_blocks(
    const std::vector<std::array<uint8_t, 16>>& inputs
) {
    std::vector<std::array<uint8_t, 16>> outputs(inputs.size());
    if (!inputs.empty()) {
        disco_oprf_eval_blocks(
            inputs.front().data(),
            inputs.size(),
            outputs.front().data()
        );
    }
    return outputs;
}

static bool disco_oprf_uses_adapter_server_prf() {
    return std::strcmp(disco_oprf_mechanism(), "GCAES") != 0;
}

static std::vector<std::array<uint8_t, 16>> disco_oprf_server_prf_blocks(
    const std::vector<std::array<uint8_t, 16>>& inputs
) {
    std::vector<std::array<uint8_t, 16>> outputs(inputs.size());
    if (inputs.empty()) {
        return outputs;
    }

    const auto start = Clock::now();
    if (disco_oprf_uses_adapter_server_prf()) {
        disco_oprf_server_prf_eval_blocks(
            inputs.front().data(),
            inputs.size(),
            outputs.front().data()
        );
    } else {
        static const uint8_t zero_key[16] = {};
        static const AES zero_key_aes(zero_key);

        std::vector<block> plaintexts(inputs.size());
        std::vector<block> ciphertexts(inputs.size());
        for (size_t i = 0; i < inputs.size(); ++i) {
            plaintexts[i] = toBlock(inputs[i].data());
        }
        zero_key_aes.encryptECBBlocks(
            plaintexts.data(),
            static_cast<uint64_t>(plaintexts.size()),
            ciphertexts.data()
        );
        for (size_t i = 0; i < outputs.size(); ++i) {
            std::memcpy(outputs[i].data(), &ciphertexts[i], outputs[i].size());
        }
    }

    add_local_oprf_server_prf_stats(elapsed_ms(start, Clock::now()), inputs.size());
    return outputs;
}

static std::vector<std::array<uint8_t, 16>> disco_prf_blocks_for_role(
    const std::vector<std::array<uint8_t, 16>>& inputs,
    bool interactive
) {
    return interactive
        ? disco_oprf_blocks(inputs)
        : disco_oprf_server_prf_blocks(inputs);
}

static uint64_t first_u64(const std::array<uint8_t, 16>& block) {
    uint64_t value = 0;
    std::memcpy(&value, block.data(), sizeof(value));
    return value;
}

static uint32_t first_u32_nonzero(const std::array<uint8_t, 16>& block) {
    uint32_t value = 0;
    std::memcpy(&value, block.data(), sizeof(value));
    if (value == 0) {
        value = 1;
    }
    return value;
}

static uint32_t second_u32_nonzero(const std::array<uint8_t, 16>& block) {
    uint32_t value = 0;
    std::memcpy(&value, block.data() + sizeof(uint64_t), sizeof(value));
    if (value == 0) {
        value = 1;
    }
    return value;
}

static uint64_t oprf_bucket_value(size_t table_idx, uint64_t lsh_key) {
    return first_u64(disco_oprf_block(OPRF_BUCKET_DOMAIN, table_idx, lsh_key));
}

static uint32_t oprf_tag_value(size_t table_idx, uint64_t lsh_key) {
    return first_u32_nonzero(disco_oprf_block(OPRF_TAG_DOMAIN, table_idx, lsh_key));
}

struct CuckooOprfEncoded {
    uint64_t bucket;
    uint32_t tag;
};

static std::vector<uint64_t> oprf_bucket_keys(
    const std::vector<double>& img,
    const E2LSH& lsh
) {
    auto keys = lsh.table_keys(img);
    std::vector<std::array<uint8_t, 16>> inputs;
    inputs.reserve(keys.size());
    for (size_t table_idx = 0; table_idx < keys.size(); ++table_idx) {
        inputs.push_back(disco_oprf_input_block(OPRF_BUCKET_DOMAIN, table_idx, keys[table_idx]));
    }

    const auto outputs = disco_oprf_blocks(inputs);
    for (size_t table_idx = 0; table_idx < keys.size(); ++table_idx) {
        keys[table_idx] = first_u64(outputs[table_idx]);
    }
    return keys;
}

static std::vector<std::vector<uint64_t>> oprf_bucket_key_batches_range(
    const std::vector<std::vector<double>>& imgs,
    const E2LSH& lsh,
    size_t start,
    size_t count,
    const char* label,
    bool interactive = true
) {
    std::vector<std::vector<uint64_t>> raw_batches(count);
    parallel_for_indices(count, [&](size_t offset) {
        raw_batches[offset] = lsh.table_keys(imgs[start + offset]);
    }, label);

    size_t total_keys = 0;
    for (const auto& keys : raw_batches) {
        total_keys += keys.size();
    }

    std::vector<std::array<uint8_t, 16>> inputs;
    inputs.reserve(total_keys);
    for (const auto& keys : raw_batches) {
        for (size_t table_idx = 0; table_idx < keys.size(); ++table_idx) {
            inputs.push_back(disco_oprf_input_block(OPRF_BUCKET_DOMAIN, table_idx, keys[table_idx]));
        }
    }

    const auto outputs = disco_prf_blocks_for_role(inputs, interactive);
    std::vector<std::vector<uint64_t>> out(count);
    size_t output_idx = 0;
    for (size_t offset = 0; offset < count; ++offset) {
        out[offset].resize(raw_batches[offset].size());
        for (size_t table_idx = 0; table_idx < raw_batches[offset].size(); ++table_idx) {
            out[offset][table_idx] = first_u64(outputs[output_idx++]);
        }
    }
    return out;
}

static std::vector<std::vector<uint64_t>> parallel_oprf_bucket_key_batches(
    const std::vector<std::vector<double>>& imgs,
    const E2LSH& lsh
) {
    return oprf_bucket_key_batches_range(
        imgs,
        lsh,
        0,
        imgs.size(),
        "Bloom OPRF query LSH bucket lookup"
    );
}

static std::vector<CuckooOprfEncoded> cuckoo_oprf_encoded_keys(
    const std::vector<double>& img,
    const E2LSH& lsh
) {
    auto keys = lsh.table_keys(img);
    std::vector<CuckooOprfEncoded> encoded;
    encoded.reserve(keys.size());

    std::vector<std::array<uint8_t, 16>> inputs;
    inputs.reserve(keys.size() * (CUCKOO_OPRF_SPLIT_OUTPUT ? 1 : 2));
    for (size_t table_idx = 0; table_idx < keys.size(); ++table_idx) {
        if (CUCKOO_OPRF_SPLIT_OUTPUT) {
            inputs.push_back(disco_oprf_input_block(OPRF_CUCKOO_DOMAIN, table_idx, keys[table_idx]));
        } else {
            inputs.push_back(disco_oprf_input_block(OPRF_BUCKET_DOMAIN, table_idx, keys[table_idx]));
            inputs.push_back(disco_oprf_input_block(OPRF_TAG_DOMAIN, table_idx, keys[table_idx]));
        }
    }

    const auto outputs = disco_oprf_blocks(inputs);
    size_t output_idx = 0;
    for (size_t table_idx = 0; table_idx < keys.size(); ++table_idx) {
        if (CUCKOO_OPRF_SPLIT_OUTPUT) {
            const auto& output = outputs[output_idx++];
            encoded.push_back(CuckooOprfEncoded{
                first_u64(output),
                second_u32_nonzero(output)
            });
        } else {
            encoded.push_back(CuckooOprfEncoded{
                first_u64(outputs[output_idx++]),
                first_u32_nonzero(outputs[output_idx++])
            });
        }
    }

    return encoded;
}

static std::vector<std::vector<CuckooOprfEncoded>> cuckoo_oprf_encoded_key_batches_range(
    const std::vector<std::vector<double>>& imgs,
    const E2LSH& lsh,
    size_t start,
    size_t count,
    const char* label,
    bool interactive = true
) {
    std::vector<std::vector<uint64_t>> raw_batches(count);
    parallel_for_indices(count, [&](size_t offset) {
        raw_batches[offset] = lsh.table_keys(imgs[start + offset]);
    }, label);

    size_t total_keys = 0;
    for (const auto& keys : raw_batches) {
        total_keys += keys.size();
    }

    std::vector<std::array<uint8_t, 16>> inputs;
    inputs.reserve(total_keys * (CUCKOO_OPRF_SPLIT_OUTPUT ? 1 : 2));
    for (const auto& keys : raw_batches) {
        for (size_t table_idx = 0; table_idx < keys.size(); ++table_idx) {
            if (CUCKOO_OPRF_SPLIT_OUTPUT) {
                inputs.push_back(disco_oprf_input_block(OPRF_CUCKOO_DOMAIN, table_idx, keys[table_idx]));
            } else {
                inputs.push_back(disco_oprf_input_block(OPRF_BUCKET_DOMAIN, table_idx, keys[table_idx]));
                inputs.push_back(disco_oprf_input_block(OPRF_TAG_DOMAIN, table_idx, keys[table_idx]));
            }
        }
    }

    const auto outputs = disco_prf_blocks_for_role(inputs, interactive);
    std::vector<std::vector<CuckooOprfEncoded>> out(count);
    size_t output_idx = 0;
    for (size_t offset = 0; offset < count; ++offset) {
        out[offset].reserve(raw_batches[offset].size());
        for (size_t table_idx = 0; table_idx < raw_batches[offset].size(); ++table_idx) {
            if (CUCKOO_OPRF_SPLIT_OUTPUT) {
                const auto& output = outputs[output_idx++];
                out[offset].push_back(CuckooOprfEncoded{
                    first_u64(output),
                    second_u32_nonzero(output)
                });
            } else {
                out[offset].push_back(CuckooOprfEncoded{
                    first_u64(outputs[output_idx++]),
                    first_u32_nonzero(outputs[output_idx++])
                });
            }
        }
    }
    return out;
}

static void print_bloom_oprf_insert_sample(
    const std::vector<std::vector<double>>& imgs,
    const E2LSH& lsh
);

static void print_cuckoo_oprf_insert_sample(
    const std::vector<std::vector<double>>& imgs,
    const E2LSH& lsh
);

static void bloom_insert_oprf_parallel_wrapped(
    BloomFilter& bloom,
    const std::vector<std::vector<double>>& imgs,
    const E2LSH& lsh
) {
    print_bloom_oprf_insert_sample(imgs, lsh);

    for (size_t start = 0; start < imgs.size(); start += OPRF_INSERT_BATCH_SIZE) {
        const size_t count = std::min(OPRF_INSERT_BATCH_SIZE, imgs.size() - start);
        std::vector<std::vector<uint64_t>> wrapped_batches =
            oprf_bucket_key_batches_range(
                imgs,
                lsh,
                start,
                count,
                "Bloom OPRF insert LSH bucket lookup",
                OPRF_SERVER_INTERACTIVE
            );

        for (const auto& keys : wrapped_batches) {
            bloom.insert_table_keys(keys);
        }
    }
}

[[maybe_unused]] static void bloom_insert_parallel(
    BloomFilter& bloom,
    const std::vector<std::vector<double>>& imgs,
    const E2LSH& lsh
) {
    std::vector<std::vector<uint64_t>> key_batches(imgs.size());
    parallel_for_indices(imgs.size(), [&](size_t i) {
        key_batches[i] = lsh.table_keys(imgs[i]);
    }, "Bloom insert LSH bucket lookup");

    for (const auto& keys : key_batches) {
        bloom.insert_table_keys(keys);
    }
}

static void cuckoo_insert_oprf_parallel_wrapped(
    CuckooFilter& cuckoo,
    const std::vector<std::vector<double>>& imgs,
    const E2LSH& lsh
) {
    print_cuckoo_oprf_insert_sample(imgs, lsh);

    for (size_t start = 0; start < imgs.size(); start += OPRF_INSERT_BATCH_SIZE) {
        const size_t count = std::min(OPRF_INSERT_BATCH_SIZE, imgs.size() - start);
        std::vector<std::vector<CuckooOprfEncoded>> encoded_batches =
            cuckoo_oprf_encoded_key_batches_range(
                imgs,
                lsh,
                start,
                count,
                "Cuckoo OPRF insert LSH bucket lookup",
                OPRF_SERVER_INTERACTIVE
            );

        for (const auto& encoded_keys : encoded_batches) {
            for (const auto& encoded : encoded_keys) {
                cuckoo.insert_encoded(encoded.bucket, encoded.tag);
            }
        }
    }
}

static void cuckoo_insert_oprf_range_wrapped(
    CuckooFilter& cuckoo,
    const std::vector<std::vector<double>>& imgs,
    const E2LSH& lsh,
    size_t range_start,
    size_t range_count
) {
    print_cuckoo_oprf_insert_sample(imgs, lsh);

    const size_t range_end = std::min(imgs.size(), range_start + range_count);
    for (size_t start = range_start; start < range_end; start += OPRF_INSERT_BATCH_SIZE) {
        const size_t count = std::min(OPRF_INSERT_BATCH_SIZE, range_end - start);
        std::vector<std::vector<CuckooOprfEncoded>> encoded_batches =
            cuckoo_oprf_encoded_key_batches_range(
                imgs,
                lsh,
                start,
                count,
                "Cuckoo OPRF insert LSH bucket lookup",
                OPRF_SERVER_INTERACTIVE
            );

        for (const auto& encoded_keys : encoded_batches) {
            for (const auto& encoded : encoded_keys) {
                cuckoo.insert_encoded(encoded.bucket, encoded.tag);
            }
        }
    }
}

static void cuckoo_insert_range(
    CuckooFilter& cuckoo,
    const std::vector<std::vector<double>>& imgs,
    E2LSH& lsh,
    size_t range_start,
    size_t range_count
) {
    const size_t range_end = std::min(imgs.size(), range_start + range_count);
    for (size_t i = range_start; i < range_end; ++i) {
        cuckoo.insert(imgs[i], lsh);
    }
}

[[maybe_unused]] static void cuckoo_insert_parallel(
    CuckooFilter& cuckoo,
    const std::vector<std::vector<double>>& imgs,
    const E2LSH& lsh
) {
    std::vector<std::vector<uint64_t>> key_batches(imgs.size());
    parallel_for_indices(imgs.size(), [&](size_t i) {
        key_batches[i] = lsh.table_keys(imgs[i]);
    }, "Cuckoo insert LSH bucket lookup");

    for (const auto& keys : key_batches) {
        for (uint64_t key : keys) {
            cuckoo.insert_key(key);
        }
    }
}

static void print_bloom_oprf_insert_sample(
    const std::vector<std::vector<double>>& imgs,
    const E2LSH& lsh
) {
    (void)imgs;
    (void)lsh;
    if (printed_bloom_oprf_insert || imgs.empty()) {
        return;
    }
    printed_bloom_oprf_insert = true;
}

static void print_bloom_oprf_query_sample(
    const std::vector<std::vector<double>>& imgs,
    const E2LSH& lsh,
    const char* pir_label
) {
    (void)imgs;
    (void)lsh;
    (void)pir_label;
    if (printed_bloom_oprf_query || imgs.empty()) {
        return;
    }
    printed_bloom_oprf_query = true;
}

static void print_cuckoo_oprf_insert_sample(
    const std::vector<std::vector<double>>& imgs,
    const E2LSH& lsh
) {
    (void)imgs;
    (void)lsh;
    if (printed_cuckoo_oprf_insert || imgs.empty()) {
        return;
    }
    printed_cuckoo_oprf_insert = true;
}

static void print_cuckoo_oprf_query_sample(
    const CuckooFilter& cuckoo,
    const std::vector<std::vector<double>>& imgs,
    const E2LSH& lsh,
    const char* pir_label
) {
    (void)cuckoo;
    (void)imgs;
    (void)lsh;
    (void)pir_label;
    if (printed_cuckoo_oprf_query || imgs.empty()) {
        return;
    }
    printed_cuckoo_oprf_query = true;
}

[[maybe_unused]] static void bloom_insert_oprf(
    BloomFilter& bloom,
    const std::vector<double>& img,
    const E2LSH& lsh
) {
    if (!printed_bloom_oprf_insert) {
        printed_bloom_oprf_insert = true;
    }

    bloom.insert_table_keys(oprf_bucket_keys(img, lsh));
}

static std::vector<CuckooFilter::Lookup> cuckoo_oprf_lookups(
    const CuckooFilter& cuckoo,
    const std::vector<double>& img,
    const E2LSH& lsh
) {
    std::vector<CuckooFilter::Lookup> lookups;
    const auto encoded_keys = cuckoo_oprf_encoded_keys(img, lsh);
    lookups.reserve(encoded_keys.size());
    for (const auto& encoded : encoded_keys) {
        const uint64_t bucket = encoded.bucket;
        const uint32_t tag = encoded.tag;
        lookups.push_back(cuckoo.lookup_encoded(bucket, tag));
    }

    return lookups;
}

static std::vector<std::vector<CuckooFilter::Lookup>> parallel_cuckoo_oprf_lookup_batches(
    const CuckooFilter& cuckoo,
    const std::vector<std::vector<double>>& imgs,
    const E2LSH& lsh
) {
    std::vector<std::vector<CuckooFilter::Lookup>> out(imgs.size());
    const auto encoded_batches = cuckoo_oprf_encoded_key_batches_range(
        imgs,
        lsh,
        0,
        imgs.size(),
        "Cuckoo OPRF query LSH bucket lookup"
    );
    parallel_for_indices(imgs.size(), [&](size_t i) {
        out[i].reserve(encoded_batches[i].size());
        for (const auto& encoded : encoded_batches[i]) {
            out[i].push_back(cuckoo.lookup_encoded(encoded.bucket, encoded.tag));
        }
    }, "Cuckoo OPRF query bucket lookup");
    return out;
}

[[maybe_unused]] static void cuckoo_insert_oprf(
    CuckooFilter& cuckoo,
    const std::vector<double>& img,
    const E2LSH& lsh
) {
    auto keys = lsh.table_keys(img);
    if (!printed_cuckoo_oprf_insert && !keys.empty()) {
        printed_cuckoo_oprf_insert = true;
    }

    for (const auto& encoded : cuckoo_oprf_encoded_keys(img, lsh)) {
        cuckoo.insert_encoded(encoded.bucket, encoded.tag);
    }
}
