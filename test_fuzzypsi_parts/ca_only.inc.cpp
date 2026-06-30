// ============================================================
// =========== FHE FUZZY CARDINALITY, BLOOM ONLY ==============
// ============================================================

[[maybe_unused]] static constexpr std::uint64_t CA_PLAINTEXT_MODULUS = 786433;

[[maybe_unused]] static uint64_t ca_mix64(uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

[[maybe_unused]] static std::vector<size_t> ca_bloom_indices(
    uint64_t key,
    size_t m_bits,
    size_t hash_count
) {
    if (m_bits == 0) {
        throw std::invalid_argument("CA_only Bloom filter size must be > 0");
    }
    if (hash_count == 0) {
        throw std::invalid_argument("CA_only Bloom hash count must be > 0");
    }

    const uint64_t h1 = ca_mix64(key);
    const uint64_t h2 = ca_mix64(key ^ 0xD1B54A32D192ED03ULL);

    std::vector<size_t> indices;
    indices.reserve(hash_count);
    for (size_t i = 0; i < hash_count; ++i) {
        const uint64_t combined = h1 + i * h2;
        indices.push_back(static_cast<size_t>(combined % m_bits));
    }
    return indices;
}

[[maybe_unused]] static size_t ca_bloom_hash_count_for(size_t m_bits, size_t inserted_keys) {
    if (CA_BLOOM_HASHES_OVERRIDE > 0) {
        return CA_BLOOM_HASHES_OVERRIDE;
    }
    return bloom_hash_count_for(m_bits, inserted_keys);
}

struct CaCuckooEncoded {
    uint64_t bucket;
    uint32_t tag;
};

[[maybe_unused]] static uint32_t ca_cuckoo_reduce_tag(uint32_t raw_tag) {
    if (CA_CUCKOO_FP_BITS == 0 || CA_CUCKOO_FP_BITS > 32) {
        throw std::invalid_argument("CA_only Cuckoo fingerprint bits must be in [1,32]");
    }

    uint32_t tag = raw_tag;
    if (CA_CUCKOO_FP_BITS < 32) {
        const uint32_t mask = static_cast<uint32_t>((uint64_t{1} << CA_CUCKOO_FP_BITS) - 1);
        tag &= mask;
    }
    return tag == 0 ? 1 : tag;
}

[[maybe_unused]] static uint32_t ca_cuckoo_tag_from_key(uint64_t key) {
    return ca_cuckoo_reduce_tag(static_cast<uint32_t>(ca_mix64(key) & 0xffffffffULL));
}

[[maybe_unused]] static std::vector<size_t> ca_cuckoo_tag_indices(uint32_t tag) {
    if (CA_CUCKOO_BUCKET_TAG_BITS == 0 || CA_CUCKOO_TAG_HASHES == 0) {
        throw std::invalid_argument("CA_only Cuckoo tag Bloom parameters must be > 0");
    }

    const uint64_t h1 = ca_mix64(static_cast<uint64_t>(tag));
    const uint64_t h2 = ca_mix64(static_cast<uint64_t>(tag) ^ 0xC2B2AE3D27D4EB4FULL);
    std::vector<size_t> indices;
    indices.reserve(CA_CUCKOO_TAG_HASHES);
    for (size_t i = 0; i < CA_CUCKOO_TAG_HASHES; ++i) {
        indices.push_back(static_cast<size_t>((h1 + i * h2) % CA_CUCKOO_BUCKET_TAG_BITS));
    }
    return indices;
}

[[maybe_unused]] static size_t ca_cuckoo_bucket_tag_bit_index(size_t bucket, size_t tag_idx) {
    return bucket * CA_CUCKOO_BUCKET_TAG_BITS + tag_idx;
}

[[maybe_unused]] static std::vector<std::vector<uint64_t>> ca_lsh_keys_for_item(
    const E2LSH& lsh,
    const std::vector<double>& item,
    bool use_compound_lsh,
    bool use_oprf
) {
    std::vector<std::vector<uint64_t>> out;
    if (!use_compound_lsh) {
        out = lsh.hash_keys_by_table(item);
    } else {
        const auto table_keys = lsh.table_keys(item);
        out.resize(table_keys.size());
        for (size_t l = 0; l < table_keys.size(); ++l) {
            out[l].push_back(table_keys[l]);
        }
    }

    if (use_oprf) {
        for (size_t l = 0; l < out.size(); ++l) {
            for (size_t kk = 0; kk < out[l].size(); ++kk) {
                const size_t oprf_table_idx = l * out[l].size() + kk;
                out[l][kk] = oprf_bucket_value(oprf_table_idx, out[l][kk]);
            }
        }
    }
    return out;
}

[[maybe_unused]] static std::vector<std::vector<uint64_t>> ca_lsh_keys_for_item_local_prf(
    const E2LSH& lsh,
    const std::vector<double>& item,
    bool use_compound_lsh
) {
    std::vector<std::vector<uint64_t>> out;
    if (!use_compound_lsh) {
        out = lsh.hash_keys_by_table(item);
    } else {
        const auto table_keys = lsh.table_keys(item);
        out.resize(table_keys.size());
        for (size_t l = 0; l < table_keys.size(); ++l) {
            out[l].push_back(table_keys[l]);
        }
    }

    std::vector<std::array<uint8_t, 16>> inputs;
    size_t total_keys = 0;
    for (const auto& table_keys : out) {
        total_keys += table_keys.size();
    }
    inputs.reserve(total_keys);
    for (size_t l = 0; l < out.size(); ++l) {
        for (size_t kk = 0; kk < out[l].size(); ++kk) {
            const size_t table_idx = l * out[l].size() + kk;
            inputs.push_back(disco_oprf_input_block(
                OPRF_BUCKET_DOMAIN,
                table_idx,
                out[l][kk]
            ));
        }
    }

    const auto outputs = disco_prf_blocks_for_role(inputs, false);
    size_t output_idx = 0;
    for (size_t l = 0; l < out.size(); ++l) {
        for (size_t kk = 0; kk < out[l].size(); ++kk) {
            out[l][kk] = first_u64(outputs[output_idx++]);
        }
    }
    return out;
}

[[maybe_unused]] static std::vector<std::vector<CaCuckooEncoded>> ca_cuckoo_keys_for_item(
    const E2LSH& lsh,
    const std::vector<double>& item,
    bool use_compound_lsh,
    bool use_oprf
) {
    std::vector<std::vector<CaCuckooEncoded>> out;
    if (!use_compound_lsh) {
        const auto keys_by_table = lsh.hash_keys_by_table(item);
        out.resize(keys_by_table.size());
        for (size_t l = 0; l < keys_by_table.size(); ++l) {
            out[l].reserve(keys_by_table[l].size());
            for (size_t kk = 0; kk < keys_by_table[l].size(); ++kk) {
                const size_t table_idx = l * keys_by_table[l].size() + kk;
                const uint64_t key = keys_by_table[l][kk];
                const uint64_t bucket = use_oprf ? oprf_bucket_value(table_idx, key) : key;
                const uint32_t raw_tag = use_oprf
                    ? oprf_tag_value(table_idx, key)
                    : static_cast<uint32_t>(ca_mix64(key) & 0xffffffffULL);
                out[l].push_back(CaCuckooEncoded{bucket, ca_cuckoo_reduce_tag(raw_tag)});
            }
        }
    } else {
        const auto table_keys = lsh.table_keys(item);
        out.resize(table_keys.size());
        for (size_t l = 0; l < table_keys.size(); ++l) {
            const uint64_t key = table_keys[l];
            const uint64_t bucket = use_oprf ? oprf_bucket_value(l, key) : key;
            const uint32_t raw_tag = use_oprf
                ? oprf_tag_value(l, key)
                : static_cast<uint32_t>(ca_mix64(key) & 0xffffffffULL);
            out[l].push_back(CaCuckooEncoded{bucket, ca_cuckoo_reduce_tag(raw_tag)});
        }
    }
    return out;
}

[[maybe_unused]] static std::vector<std::vector<CaCuckooEncoded>> ca_cuckoo_keys_for_item_local_prf(
    const E2LSH& lsh,
    const std::vector<double>& item,
    bool use_compound_lsh
) {
    std::vector<std::vector<uint64_t>> raw;
    if (!use_compound_lsh) {
        raw = lsh.hash_keys_by_table(item);
    } else {
        const auto table_keys = lsh.table_keys(item);
        raw.resize(table_keys.size());
        for (size_t l = 0; l < table_keys.size(); ++l) {
            raw[l].push_back(table_keys[l]);
        }
    }

    size_t total_keys = 0;
    for (const auto& table_keys : raw) {
        total_keys += table_keys.size();
    }
    std::vector<std::array<uint8_t, 16>> inputs;
    inputs.reserve(total_keys * 2);
    for (size_t l = 0; l < raw.size(); ++l) {
        for (size_t kk = 0; kk < raw[l].size(); ++kk) {
            const size_t table_idx = l * raw[l].size() + kk;
            inputs.push_back(disco_oprf_input_block(
                OPRF_BUCKET_DOMAIN,
                table_idx,
                raw[l][kk]
            ));
            inputs.push_back(disco_oprf_input_block(
                OPRF_TAG_DOMAIN,
                table_idx,
                raw[l][kk]
            ));
        }
    }

    const auto outputs = disco_prf_blocks_for_role(inputs, false);
    std::vector<std::vector<CaCuckooEncoded>> out(raw.size());
    size_t output_idx = 0;
    for (size_t l = 0; l < raw.size(); ++l) {
        out[l].reserve(raw[l].size());
        for (size_t kk = 0; kk < raw[l].size(); ++kk) {
            out[l].push_back(CaCuckooEncoded{
                first_u64(outputs[output_idx++]),
                ca_cuckoo_reduce_tag(first_u32_nonzero(outputs[output_idx++]))
            });
        }
    }
    return out;
}

#if FUZZY_PETS_HAS_OPENFHE

struct CaThresholdFhe {
    lbcrypto::CryptoContext<lbcrypto::DCRTPoly> cc;
    lbcrypto::KeyPair<lbcrypto::DCRTPoly> client_keys;
    lbcrypto::KeyPair<lbcrypto::DCRTPoly> server_keys;
    size_t batch_slots = 0;
    std::vector<lbcrypto::Plaintext> slot_masks;
};

static CaThresholdFhe ca_threshold_keygen(size_t multiplicative_depth, size_t requested_batch_slots) {
    lbcrypto::CCParams<lbcrypto::CryptoContextBFVRNS> parameters;
    parameters.SetPlaintextModulus(CA_PLAINTEXT_MODULUS);
    parameters.SetMultiplicativeDepth(static_cast<uint32_t>(multiplicative_depth));
    parameters.SetBatchSize(static_cast<uint32_t>(requested_batch_slots));
    parameters.SetMultipartyMode(lbcrypto::NOISE_FLOODING_MULTIPARTY);

    auto cc = lbcrypto::GenCryptoContext(parameters);
    cc->Enable(lbcrypto::PKE);
    cc->Enable(lbcrypto::KEYSWITCH);
    cc->Enable(lbcrypto::LEVELEDSHE);
    cc->Enable(lbcrypto::MULTIPARTY);

    CaThresholdFhe fhe;
    fhe.cc = cc;
    fhe.batch_slots = cc->GetEncodingParams()->GetBatchSize();
    if (fhe.batch_slots == 0) {
        throw std::runtime_error("CA_only OpenFHE returned batch size 0");
    }

    fhe.client_keys = cc->KeyGen();
    if (!fhe.client_keys.good()) {
        throw std::runtime_error("CA_only OpenFHE client key generation failed");
    }

    auto eval_mult_client = cc->KeySwitchGen(
        fhe.client_keys.secretKey,
        fhe.client_keys.secretKey
    );

    std::vector<int32_t> rotation_indices;
    rotation_indices.reserve(fhe.batch_slots > 0 ? fhe.batch_slots - 1 : 0);
    for (size_t slot = 1; slot < fhe.batch_slots; ++slot) {
        if (slot > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
            throw std::runtime_error("CA_only BFV batch slot count exceeds rotation index range");
        }
        const int32_t slot_i = static_cast<int32_t>(slot);
        rotation_indices.push_back(slot_i);
        rotation_indices.push_back(-slot_i);
    }

    std::shared_ptr<std::map<uint32_t, lbcrypto::EvalKey<lbcrypto::DCRTPoly>>> eval_rotate_client;
    if (!rotation_indices.empty()) {
        cc->EvalRotateKeyGen(fhe.client_keys.secretKey, rotation_indices);
        eval_rotate_client =
            std::make_shared<std::map<uint32_t, lbcrypto::EvalKey<lbcrypto::DCRTPoly>>>(
                cc->GetEvalAutomorphismKeyMap(fhe.client_keys.secretKey->GetKeyTag())
            );
    }

    fhe.server_keys = cc->MultipartyKeyGen(fhe.client_keys.publicKey);
    if (!fhe.server_keys.good()) {
        throw std::runtime_error("CA_only OpenFHE server key generation failed");
    }

    auto eval_mult_server = cc->MultiKeySwitchGen(
        fhe.server_keys.secretKey,
        fhe.server_keys.secretKey,
        eval_mult_client
    );
    auto eval_mult_joint = cc->MultiAddEvalKeys(
        eval_mult_client,
        eval_mult_server,
        fhe.server_keys.publicKey->GetKeyTag()
    );
    auto eval_mult_server_joint = cc->MultiMultEvalKey(
        fhe.server_keys.secretKey,
        eval_mult_joint,
        fhe.server_keys.publicKey->GetKeyTag()
    );
    auto eval_mult_client_joint = cc->MultiMultEvalKey(
        fhe.client_keys.secretKey,
        eval_mult_joint,
        fhe.server_keys.publicKey->GetKeyTag()
    );
    auto eval_mult_final = cc->MultiAddEvalMultKeys(
        eval_mult_client_joint,
        eval_mult_server_joint,
        eval_mult_joint->GetKeyTag()
    );
    cc->InsertEvalMultKey({eval_mult_final});

    if (!rotation_indices.empty()) {
        auto eval_rotate_server = cc->MultiEvalAtIndexKeyGen(
            fhe.server_keys.secretKey,
            eval_rotate_client,
            rotation_indices,
            fhe.server_keys.publicKey->GetKeyTag()
        );
        auto eval_rotate_joint = cc->MultiAddEvalAutomorphismKeys(
            eval_rotate_client,
            eval_rotate_server,
            fhe.server_keys.publicKey->GetKeyTag()
        );
        cc->InsertEvalAutomorphismKey(eval_rotate_joint);
    }

    fhe.slot_masks.reserve(fhe.batch_slots);
    for (size_t slot = 0; slot < fhe.batch_slots; ++slot) {
        std::vector<int64_t> mask(fhe.batch_slots, 0);
        mask[slot] = 1;
        fhe.slot_masks.push_back(cc->MakePackedPlaintext(mask));
    }

    return fhe;
}

static lbcrypto::Ciphertext<lbcrypto::DCRTPoly> ca_encrypt_slots(
    const CaThresholdFhe& fhe,
    std::vector<int64_t> slots
) {
    if (slots.size() > fhe.batch_slots) {
        throw std::invalid_argument("CA_only packed plaintext exceeds BFV batch slots");
    }
    slots.resize(fhe.batch_slots, 0);
    auto plaintext = fhe.cc->MakePackedPlaintext(slots);
    return fhe.cc->Encrypt(fhe.server_keys.publicKey, plaintext);
}

static lbcrypto::Ciphertext<lbcrypto::DCRTPoly> ca_encrypt_scalar(
    const CaThresholdFhe& fhe,
    int64_t value
) {
    return ca_encrypt_slots(fhe, {value});
}

[[maybe_unused]] static lbcrypto::Ciphertext<lbcrypto::DCRTPoly> ca_extract_packed_bit(
    const CaThresholdFhe& fhe,
    const lbcrypto::Ciphertext<lbcrypto::DCRTPoly>& packed_ciphertext,
    size_t slot
) {
    if (slot >= fhe.batch_slots) {
        throw std::out_of_range("CA_only packed Bloom slot out of range");
    }
    auto selected = fhe.cc->EvalMult(packed_ciphertext, fhe.slot_masks[slot]);
    if (slot != 0) {
        selected = fhe.cc->EvalRotate(selected, static_cast<int32_t>(slot));
    }
    return selected;
}

[[maybe_unused]] static lbcrypto::Ciphertext<lbcrypto::DCRTPoly> ca_place_extracted_bit(
    const CaThresholdFhe& fhe,
    const lbcrypto::Ciphertext<lbcrypto::DCRTPoly>& extracted_bit,
    size_t target_slot
) {
    if (target_slot >= fhe.batch_slots) {
        throw std::out_of_range("CA_only target slot out of range");
    }
    if (target_slot == 0) {
        return extracted_bit;
    }
    return fhe.cc->EvalRotate(extracted_bit, -static_cast<int32_t>(target_slot));
}

static lbcrypto::Ciphertext<lbcrypto::DCRTPoly> ca_route_packed_bit(
    const CaThresholdFhe& fhe,
    const lbcrypto::Ciphertext<lbcrypto::DCRTPoly>& packed_ciphertext,
    size_t source_slot,
    size_t target_slot
) {
    if (source_slot >= fhe.batch_slots || target_slot >= fhe.batch_slots) {
        throw std::out_of_range("CA_only packed Bloom route slot out of range");
    }

    auto selected = fhe.cc->EvalMult(packed_ciphertext, fhe.slot_masks[source_slot]);
    const int32_t rotation =
        static_cast<int32_t>(source_slot) - static_cast<int32_t>(target_slot);
    if (rotation != 0) {
        selected = fhe.cc->EvalRotate(selected, rotation);
    }
    return selected;
}

static lbcrypto::Ciphertext<lbcrypto::DCRTPoly> ca_sum_slots_to_scalar(
    const CaThresholdFhe& fhe,
    const lbcrypto::Ciphertext<lbcrypto::DCRTPoly>& packed_values,
    size_t used_slots
) {
    if (used_slots == 0 || used_slots > fhe.batch_slots) {
        throw std::invalid_argument("CA_only invalid packed slot sum length");
    }

    auto sum = packed_values;
    for (size_t step = 1; step < used_slots; step <<= 1) {
        sum = fhe.cc->EvalAdd(sum, fhe.cc->EvalRotate(sum, static_cast<int32_t>(step)));
    }
    return sum;
}

static int64_t ca_threshold_decrypt_scalar(
    const CaThresholdFhe& fhe,
    const lbcrypto::Ciphertext<lbcrypto::DCRTPoly>& ciphertext
) {
    auto partial_client = fhe.cc->MultipartyDecryptLead(
        {ciphertext},
        fhe.client_keys.secretKey
    );
    auto partial_server = fhe.cc->MultipartyDecryptMain(
        {ciphertext},
        fhe.server_keys.secretKey
    );

    std::vector<lbcrypto::Ciphertext<lbcrypto::DCRTPoly>> partials;
    partials.push_back(partial_client[0]);
    partials.push_back(partial_server[0]);

    lbcrypto::Plaintext plaintext;
    fhe.cc->MultipartyDecryptFusion(partials, &plaintext);
    plaintext->SetLength(1);

    const auto values = plaintext->GetPackedValue();
    if (values.empty()) {
        throw std::runtime_error("CA_only OpenFHE decrypted empty plaintext");
    }
    return values[0];
}

static std::vector<uint8_t> ca_serialize_ciphertext(
    const lbcrypto::Ciphertext<lbcrypto::DCRTPoly>& ciphertext
) {
    std::stringstream stream;
    lbcrypto::Serial::Serialize(ciphertext, stream, lbcrypto::SerType::BINARY);
    const std::string bytes = stream.str();
    return std::vector<uint8_t>(bytes.begin(), bytes.end());
}

static lbcrypto::Ciphertext<lbcrypto::DCRTPoly> ca_deserialize_ciphertext(
    const std::vector<uint8_t>& bytes
) {
    std::string serialized(bytes.begin(), bytes.end());
    std::stringstream stream(serialized);
    lbcrypto::Ciphertext<lbcrypto::DCRTPoly> ciphertext;
    lbcrypto::Serial::Deserialize(ciphertext, stream, lbcrypto::SerType::BINARY);
    if (!ciphertext) {
        throw std::runtime_error("CA_only PIR returned an empty serialized FHE ciphertext");
    }
    return ciphertext;
}

static void ca_write_u64_le(std::vector<uint8_t>& row, uint64_t value) {
    if (row.size() < sizeof(value)) {
        throw std::invalid_argument("CA_only PIR row too short for length prefix");
    }
    for (size_t i = 0; i < sizeof(value); ++i) {
        row[i] = static_cast<uint8_t>((value >> (8 * i)) & 0xff);
    }
}

static uint64_t ca_read_u64_le(const std::vector<uint8_t>& row) {
    if (row.size() < sizeof(uint64_t)) {
        throw std::runtime_error("CA_only PIR row is missing ciphertext length prefix");
    }
    uint64_t value = 0;
    for (size_t i = 0; i < sizeof(value); ++i) {
        value |= static_cast<uint64_t>(row[i]) << (8 * i);
    }
    return value;
}

static std::vector<uint8_t> ca_pack_ciphertext_pir_row(
    const std::vector<uint8_t>& serialized,
    size_t row_bytes
) {
    if (row_bytes < sizeof(uint64_t) || serialized.size() > row_bytes - sizeof(uint64_t)) {
        throw std::invalid_argument("CA_only serialized ciphertext exceeds PIR row size");
    }
    std::vector<uint8_t> row(row_bytes, 0);
    ca_write_u64_le(row, static_cast<uint64_t>(serialized.size()));
    std::copy(serialized.begin(), serialized.end(), row.begin() + sizeof(uint64_t));
    return row;
}

static lbcrypto::Ciphertext<lbcrypto::DCRTPoly> ca_unpack_ciphertext_pir_row(
    const std::vector<uint8_t>& row
) {
    const uint64_t serialized_len = ca_read_u64_le(row);
    if (serialized_len > row.size() - sizeof(uint64_t)) {
        throw std::runtime_error("CA_only PIR row has invalid serialized ciphertext length");
    }
    std::vector<uint8_t> serialized(
        row.begin() + sizeof(uint64_t),
        row.begin() + sizeof(uint64_t) + static_cast<std::ptrdiff_t>(serialized_len)
    );
    return ca_deserialize_ciphertext(serialized);
}

static std::string ca_temp_pir_db_path(size_t client_size, int run) {
    const auto ticks = Clock::now().time_since_epoch().count();
    std::ostringstream path;
    path << "/tmp/fuzzy_pets_ca_pir_"
         << ticks
         << "_client" << client_size
         << "_run" << run
         << ".bin";
    return path.str();
}

static void ca_write_pir_row_to_stream(
    std::ofstream& out,
    const pir_punc::Row& row
) {
    out.write(reinterpret_cast<const char*>(row.data()), static_cast<std::streamsize>(row.size()));
    if (!out) {
        throw std::runtime_error("CA_only failed while writing disk-backed PIR DB row");
    }
}

static pir_punc::Row ca_read_pir_row_from_stream(
    std::ifstream& in,
    const std::string& path,
    size_t row_bytes,
    size_t row_idx
) {
    pir_punc::Row row(row_bytes);
    const auto offset = static_cast<std::streamoff>(row_idx * row_bytes);
    in.clear();
    in.seekg(offset, std::ios::beg);
    if (!in) {
        throw std::runtime_error("CA_only failed to seek disk-backed PIR DB: " + path);
    }
    in.read(reinterpret_cast<char*>(row.data()), static_cast<std::streamsize>(row.size()));
    if (in.gcount() != static_cast<std::streamsize>(row.size())) {
        throw std::runtime_error("CA_only failed to read full disk-backed PIR DB row: " + path);
    }
    return row;
}

static uint32_t ca_read_u32_le_at(const std::vector<uint8_t>& row, size_t off) {
    if (off + sizeof(uint32_t) > row.size()) {
        throw std::runtime_error("CA_only Cuckoo row too short for fingerprint");
    }
    return static_cast<uint32_t>(row[off + 0]) |
           (static_cast<uint32_t>(row[off + 1]) << 8) |
           (static_cast<uint32_t>(row[off + 2]) << 16) |
           (static_cast<uint32_t>(row[off + 3]) << 24);
}

static bool ca_cuckoo_bucket_tag_maybe_contains(
    const std::vector<unsigned char>& bucket_tag_bits,
    size_t bucket_count,
    size_t bucket,
    uint32_t tag
) {
    if (bucket >= bucket_count) {
        throw std::out_of_range("CA_only Cuckoo bucket out of range");
    }
    for (size_t tag_idx : ca_cuckoo_tag_indices(tag)) {
        const size_t bit_idx = ca_cuckoo_bucket_tag_bit_index(bucket, tag_idx);
        if (bit_idx >= bucket_tag_bits.size() || !bucket_tag_bits[bit_idx]) {
            return false;
        }
    }
    return true;
}

static void ca_progress(
    const std::string& phase,
    size_t done,
    size_t total,
    Clock::time_point start
) {
    const double elapsed_s = elapsed_ms(start, Clock::now()) / 1000.0;
    cout << "[CA_only progress] " << phase
         << " " << done << "/" << total;
    if (total > 0) {
        const double pct = 100.0 * static_cast<double>(done) / static_cast<double>(total);
        cout << " (" << pct << "%)";
    }
    cout << " elapsed=" << elapsed_s << "s";
    if (done > 0 && done < total) {
        const double eta_s = elapsed_s * (static_cast<double>(total - done) / static_cast<double>(done));
        cout << " eta=" << eta_s << "s";
    }
    cout << "\n";
}

static bool ca_should_print_progress(size_t done, size_t total, size_t stride) {
    if (done == total) {
        return true;
    }
    return stride > 0 && done % stride == 0;
}

static size_t ca_ceil_log2(size_t value) {
    if (value <= 1) {
        return 0;
    }

    size_t result = 0;
    size_t power = 1;
    while (power < value) {
        power <<= 1;
        ++result;
    }
    return result;
}

static lbcrypto::Ciphertext<lbcrypto::DCRTPoly> ca_or_tree(
    const CaThresholdFhe& fhe,
    std::vector<lbcrypto::Ciphertext<lbcrypto::DCRTPoly>> terms,
    const lbcrypto::Ciphertext<lbcrypto::DCRTPoly>& encrypted_zero
) {
    if (terms.empty()) {
        return encrypted_zero;
    }

    while (terms.size() > 1) {
        std::vector<lbcrypto::Ciphertext<lbcrypto::DCRTPoly>> next;
        next.reserve((terms.size() + 1) / 2);
        for (size_t i = 0; i < terms.size(); i += 2) {
            if (i + 1 == terms.size()) {
                next.push_back(terms[i]);
                continue;
            }

            auto term_or = fhe.cc->EvalAdd(terms[i], terms[i + 1]);
            auto term_and = fhe.cc->EvalMult(terms[i], terms[i + 1]);
            next.push_back(fhe.cc->EvalSub(term_or, term_and));
        }
        terms = std::move(next);
    }

    return terms[0];
}

static lbcrypto::Ciphertext<lbcrypto::DCRTPoly> ca_and_tree(
    const CaThresholdFhe& fhe,
    std::vector<lbcrypto::Ciphertext<lbcrypto::DCRTPoly>> terms
) {
    if (terms.empty()) {
        throw std::invalid_argument("CA_only AND tree requires at least one term");
    }

    while (terms.size() > 1) {
        std::vector<lbcrypto::Ciphertext<lbcrypto::DCRTPoly>> next;
        next.reserve((terms.size() + 1) / 2);
        for (size_t i = 0; i < terms.size(); i += 2) {
            if (i + 1 == terms.size()) {
                next.push_back(terms[i]);
                continue;
            }
            next.push_back(fhe.cc->EvalMult(terms[i], terms[i + 1]));
        }
        terms = std::move(next);
    }

    return terms[0];
}

static int fuzzypsi_cardinality_only(
    int L,
    int K,
    double w,
    const string& mode
) {
    if (filter_size == 0) {
        throw std::invalid_argument("CA_only requires --filter_size > 0");
    }
    if (L <= 0 || K <= 0) {
        throw std::invalid_argument("CA_only requires L and k to be > 0");
    }

    const size_t l_count = static_cast<size_t>(L);
    const size_t k_count = static_cast<size_t>(K);
    const size_t terms_per_filter = CA_COMPOUND_LSH ? 1 : k_count;
    const size_t membership_terms_per_item = l_count * terms_per_filter;
    const size_t ca_bloom_kstar_inserted_keys =
        std::max<size_t>(1, l_count * terms_per_filter * static_cast<size_t>(N));
    const size_t ca_bloom_hash_count =
        ca_bloom_hash_count_for(filter_size, ca_bloom_kstar_inserted_keys);
    const size_t fhe_depth = std::max<size_t>(
        3,
        ca_ceil_log2(ca_bloom_hash_count) + ca_ceil_log2(membership_terms_per_item)
    );
    E2LSH lsh(dim, L, K, w);

    cout << "[CA_only] PiCardSum(X,Y) with X=Client, Y=Server"
         << " L=" << L
         << " K=" << K
         << " ca_lsh_mode=" << (CA_COMPOUND_LSH ? "compound_table_keys" : "page12_individual_Hk")
         << " ca_oprf=" << (CA_USE_OPRF ? "true" : "false")
         << " bloom_filters=" << l_count
         << " bloom_bits_each=" << filter_size
         << " bloom_hashes=" << ca_bloom_hash_count
         << " bloom_hash_mode="
         << (CA_BLOOM_HASHES_OVERRIDE > 0 ? "override" : "k_star")
         << " bloom_hash_kstar_inserted_keys=" << ca_bloom_kstar_inserted_keys
         << " plaintext_modulus=" << CA_PLAINTEXT_MODULUS
         << " requested_batch_slots=" << CA_BATCH_SLOTS
         << " membership_terms_per_item=" << membership_terms_per_item
         << " fhe_depth=" << fhe_depth
         << "\n";

    vector<string> server_ids;
    vector<vector<double>> server_imgs, client_imgs;
    vector<bool> client_is_close;

    for (int client_size : CLIENT_SIZES_LIST) {
        if (static_cast<std::uint64_t>(client_size) >= CA_PLAINTEXT_MODULUS) {
            throw std::runtime_error("CA_only client_size exceeds FHE plaintext modulus");
        }

        long double cardinality_sum = 0.0L;
        double keygen_runtime_ms_sum = 0.0;
        double server_bloom_runtime_ms_sum = 0.0;
        double server_encrypt_runtime_ms_sum = 0.0;
        double ca_pir_db_runtime_ms_sum = 0.0;
        double ca_pir_client_runtime_ms_sum = 0.0;
        double ca_pir_server_runtime_ms_sum = 0.0;
        double packed_route_planning_runtime_ms_sum = 0.0;
        double client_eval_runtime_ms_sum = 0.0;
        double decrypt_runtime_ms_sum = 0.0;
        double total_runtime_ms_sum = 0.0;
        double ca_oprf_runtime_ms_sum = 0.0;
        long double ca_pir_setup_communication_bytes_sum = 0.0L;
        long double ca_pir_query_communication_bytes_sum = 0.0L;
        long double ca_pir_response_communication_bytes_sum = 0.0L;
        long double ca_oprf_communication_bytes_sum = 0.0L;
        long double ca_oprf_client_to_server_bytes_sum = 0.0L;
        long double ca_oprf_server_to_client_bytes_sum = 0.0L;
        long double ca_oprf_calls_sum = 0.0L;
        long double ca_oprf_blocks_sum = 0.0L;

        for (int run = 0; run < NUM_RUNS_PSI; ++run) {
            const uint32_t run_seed = dataset_seed_for_run(run);
            mt19937 run_rng(run_seed);
            cout << "[CA_only dataset seed] client_size=" << client_size
                 << " run=" << (run + 1) << "/" << NUM_RUNS_PSI
                 << " seed=" << run_seed << "\n";

            cout << "[CA_only phase] Loading/sampling Server and Client datasets...\n";
            load_server_from_json(server_path, N, run_rng, server_ids, server_imgs);
            load_client_from_json_for_server(
                server_ids,
                server_imgs,
                client_size,
                run_rng,
                client_imgs,
                client_is_close
            );
            cout << "[CA_only phase] Dataset ready: server=" << server_imgs.size()
                 << " client=" << client_imgs.size() << "\n";
            const size_t client_close_total = static_cast<size_t>(
                std::count(client_is_close.begin(), client_is_close.end(), true)
            );
            const size_t client_far_total = client_imgs.size() - client_close_total;
            cout << "[CA_only diagnostic] Client ground truth split"
                 << " close=" << client_close_total
                 << " far=" << client_far_total
                 << "\n";

            const auto total_start = Clock::now();
            const DiscoGcaesOprfStats ca_oprf_stats_before_run = disco_gcaes_oprf_stats();

            if (CA_DISABLE_OPRF_AND_PIR) {
                cout << "[CA_only phase] Plain Bloom cardinality start; "
                     << "OPRF/PIR/THE disabled for low-memory validation\n";
                cout << "[CA_only phase] Server Bloom build start"
                     << " server=" << server_imgs.size()
                     << " filters=" << l_count
                     << " terms_per_filter=" << terms_per_filter
                     << " total_terms_per_item=" << membership_terms_per_item
                     << "\n";
                const auto plain_bloom_start = Clock::now();
                const auto server_bloom_start = Clock::now();
                std::vector<std::vector<unsigned char>> bloom_filters(
                    l_count,
                    std::vector<unsigned char>(filter_size, 0)
                );
                const size_t bloom_build_stride = std::max<size_t>(1, server_imgs.size() / 10);
                for (size_t y_idx = 0; y_idx < server_imgs.size(); ++y_idx) {
                    const auto keys_by_table = CA_PLAIN_LOCAL_PRF
                        ? ca_lsh_keys_for_item_local_prf(lsh, server_imgs[y_idx], CA_COMPOUND_LSH)
                        : ca_lsh_keys_for_item(lsh, server_imgs[y_idx], CA_COMPOUND_LSH, false);
                    for (size_t l = 0; l < l_count; ++l) {
                        for (size_t kk = 0; kk < keys_by_table[l].size(); ++kk) {
                            const auto indices = ca_bloom_indices(
                                keys_by_table[l][kk],
                                filter_size,
                                ca_bloom_hash_count
                            );
                            for (size_t idx : indices) {
                                bloom_filters[l][idx] = 1;
                            }
                        }
                    }
                    const size_t done = y_idx + 1;
                    if (ca_should_print_progress(done, server_imgs.size(), bloom_build_stride)) {
                        ca_progress("plain server Bloom build", done, server_imgs.size(), server_bloom_start);
                    }
                }
                const auto server_bloom_end = Clock::now();
                cout << "[CA_only phase] Plain server Bloom build done in "
                     << elapsed_ms(server_bloom_start, server_bloom_end) / 1000.0 << "s\n";

                size_t total_bloom_ones = 0;
                double min_fill_ratio = 1.0;
                double max_fill_ratio = 0.0;
                double fill_ratio_sum = 0.0;
                for (size_t l = 0; l < l_count; ++l) {
                    size_t ones = 0;
                    for (unsigned char bit : bloom_filters[l]) {
                        if (bit) {
                            ++ones;
                        }
                    }
                    const double fill_ratio =
                        static_cast<double>(ones) / static_cast<double>(filter_size);
                    total_bloom_ones += ones;
                    min_fill_ratio = std::min(min_fill_ratio, fill_ratio);
                    max_fill_ratio = std::max(max_fill_ratio, fill_ratio);
                    fill_ratio_sum += fill_ratio;
                }
                const double avg_fill_ratio = fill_ratio_sum / static_cast<double>(l_count);
                cout << "[CA_only diagnostic] Plain Bloom fill"
                     << " total_ones=" << total_bloom_ones
                     << "/" << (l_count * filter_size)
                     << " avg=" << avg_fill_ratio
                     << " min=" << min_fill_ratio
                     << " max=" << max_fill_ratio
                     << "\n";

                const auto plain_client_start = Clock::now();
                int64_t plaintext_cardinality_check = 0;
                size_t plaintext_close_matches = 0;
                size_t plaintext_far_matches = 0;
                for (size_t x_idx = 0; x_idx < client_imgs.size(); ++x_idx) {
                    const auto& x = client_imgs[x_idx];
                    bool in_x_plain = false;
                    const auto keys_by_table = CA_PLAIN_LOCAL_PRF
                        ? ca_lsh_keys_for_item_local_prf(lsh, x, CA_COMPOUND_LSH)
                        : ca_lsh_keys_for_item(lsh, x, CA_COMPOUND_LSH, false);
                    for (size_t l = 0; l < l_count && !in_x_plain; ++l) {
                        for (size_t kk = 0; kk < keys_by_table[l].size(); ++kk) {
                            const auto indices = ca_bloom_indices(
                                keys_by_table[l][kk],
                                filter_size,
                                ca_bloom_hash_count
                            );
                            bool member = true;
                            for (size_t idx : indices) {
                                if (!bloom_filters[l][idx]) {
                                    member = false;
                                    break;
                                }
                            }
                            in_x_plain = in_x_plain || member;
                            if (in_x_plain) {
                                break;
                            }
                        }
                    }
                    if (in_x_plain) {
                        ++plaintext_cardinality_check;
                        if (x_idx < client_is_close.size() && client_is_close[x_idx]) {
                            ++plaintext_close_matches;
                        } else {
                            ++plaintext_far_matches;
                        }
                    }
                }
                const auto plain_client_end = Clock::now();
                const auto total_end = Clock::now();

                cout << "[CA_only diagnostic] Plaintext Bloom split"
                     << " close_matched=" << plaintext_close_matches
                     << "/" << client_close_total
                     << " close_missed=" << (client_close_total - plaintext_close_matches)
                     << " far_matched=" << plaintext_far_matches
                     << "/" << client_far_total
                     << " far_rejected=" << (client_far_total - plaintext_far_matches)
                     << "\n";
                cout << "[CA_only phase] Plain Bloom Client cardinality done in "
                     << elapsed_ms(plain_client_start, plain_client_end) / 1000.0 << "s\n";

                cardinality_sum += static_cast<long double>(plaintext_cardinality_check);
                server_bloom_runtime_ms_sum += elapsed_ms(server_bloom_start, server_bloom_end);
                client_eval_runtime_ms_sum += elapsed_ms(plain_client_start, plain_client_end);
                total_runtime_ms_sum += elapsed_ms(total_start, total_end);

                cout << "[CA_only run] client_size=" << client_size
                     << " cardinality=" << plaintext_cardinality_check
                     << " total_runtime_s=" << elapsed_ms(plain_bloom_start, total_end) / 1000.0
                     << "\n";
                continue;
            }

            // Lines 1-4: THE key generation; pk/skC go to C, pk/skS go to S.
            cout << "[CA_only phase] THE key generation start"
                 << " depth=" << fhe_depth
                 << " requested_batch_slots=" << CA_BATCH_SLOTS
                 << "\n";
            const auto keygen_start = Clock::now();
            CaThresholdFhe fhe = ca_threshold_keygen(fhe_depth, CA_BATCH_SLOTS);
            const auto keygen_end = Clock::now();
            cout << "[CA_only phase] THE key generation done in "
                 << elapsed_ms(keygen_start, keygen_end) / 1000.0
                 << "s actual_batch_slots=" << fhe.batch_slots
                 << "\n";

            // Lines 6-12: server initializes L Bloom filters and sets H_i(H_k(y)).
            cout << "[CA_only phase] Server Bloom build start"
                 << " server=" << server_imgs.size()
                 << " filters=" << l_count
                 << " terms_per_filter=" << terms_per_filter
                 << " total_terms_per_item=" << membership_terms_per_item
                 << "\n";
            const auto server_bloom_start = Clock::now();
            std::vector<std::vector<unsigned char>> bloom_filters(
                l_count,
                std::vector<unsigned char>(filter_size, 0)
            );
            const size_t bloom_build_stride = std::max<size_t>(1, server_imgs.size() / 10);
            for (size_t y_idx = 0; y_idx < server_imgs.size(); ++y_idx) {
                const auto& y = server_imgs[y_idx];
                const auto keys_by_table = ca_lsh_keys_for_item(lsh, y, CA_COMPOUND_LSH, CA_USE_OPRF);
                for (size_t l = 0; l < l_count; ++l) {
                    for (size_t kk = 0; kk < keys_by_table[l].size(); ++kk) {
                        const auto indices = ca_bloom_indices(
                            keys_by_table[l][kk],
                            filter_size,
                            ca_bloom_hash_count
                        );
                        for (size_t idx : indices) {
                            bloom_filters[l][idx] = 1;
                        }
                    }
                }
                const size_t done = y_idx + 1;
                if (ca_should_print_progress(done, server_imgs.size(), bloom_build_stride)) {
                    ca_progress("server Bloom build", done, server_imgs.size(), server_bloom_start);
                }
            }
            const auto server_bloom_end = Clock::now();
            cout << "[CA_only phase] Server Bloom build done in "
                 << elapsed_ms(server_bloom_start, server_bloom_end) / 1000.0 << "s\n";

            size_t total_bloom_ones = 0;
            double min_fill_ratio = 1.0;
            double max_fill_ratio = 0.0;
            double fill_ratio_sum = 0.0;
            double estimated_random_far_accept_complement = 1.0;
            for (size_t l = 0; l < l_count; ++l) {
                size_t ones = 0;
                for (unsigned char bit : bloom_filters[l]) {
                    if (bit) {
                        ++ones;
                    }
                }
                const double fill_ratio =
                    static_cast<double>(ones) / static_cast<double>(filter_size);
                total_bloom_ones += ones;
                min_fill_ratio = std::min(min_fill_ratio, fill_ratio);
                max_fill_ratio = std::max(max_fill_ratio, fill_ratio);
                fill_ratio_sum += fill_ratio;

                const double one_key_fp =
                    std::pow(fill_ratio, static_cast<double>(ca_bloom_hash_count));
                estimated_random_far_accept_complement *=
                    std::pow(1.0 - one_key_fp, static_cast<double>(terms_per_filter));
            }
            const double avg_fill_ratio = fill_ratio_sum / static_cast<double>(l_count);
            const double estimated_random_far_accept =
                1.0 - estimated_random_far_accept_complement;
            cout << "[CA_only diagnostic] Bloom fill"
                 << " total_ones=" << total_bloom_ones
                 << "/" << (l_count * filter_size)
                 << " avg=" << avg_fill_ratio
                 << " min=" << min_fill_ratio
                 << " max=" << max_fill_ratio
                 << " estimated_random_far_accept=" << estimated_random_far_accept
                 << "\n";
            if (estimated_random_far_accept > 0.25) {
                cout << "[CA_only warning] Bloom filter is likely too full for CA_only; "
                     << "far Client items will be counted as fuzzy matches. Increase --filter_size";
                if (!CA_COMPOUND_LSH) {
                    cout << " or use --ca_compound_lsh to compare against the normal Bloom LSH keying";
                }
                cout << ".\n";
            }

            int64_t plaintext_cardinality_check = 0;
            size_t plaintext_close_matches = 0;
            size_t plaintext_far_matches = 0;
            for (size_t x_idx = 0; x_idx < client_imgs.size(); ++x_idx) {
                const auto& x = client_imgs[x_idx];
                bool in_x_plain = false;
                const auto keys_by_table = ca_lsh_keys_for_item(lsh, x, CA_COMPOUND_LSH, CA_USE_OPRF);
                for (size_t l = 0; l < l_count && !in_x_plain; ++l) {
                    for (size_t kk = 0; kk < keys_by_table[l].size(); ++kk) {
                        const auto indices = ca_bloom_indices(
                            keys_by_table[l][kk],
                            filter_size,
                            ca_bloom_hash_count
                        );
                        bool member = true;
                        for (size_t idx : indices) {
                            if (!bloom_filters[l][idx]) {
                                member = false;
                                break;
                            }
                        }
                        in_x_plain = in_x_plain || member;
                        if (in_x_plain) {
                            break;
                        }
                    }
                }
                if (in_x_plain) {
                    ++plaintext_cardinality_check;
                    if (x_idx < client_is_close.size() && client_is_close[x_idx]) {
                        ++plaintext_close_matches;
                    } else {
                        ++plaintext_far_matches;
                    }
                }
            }
            cout << "[CA_only diagnostic] Plaintext Bloom split"
                 << " close_matched=" << plaintext_close_matches
                 << "/" << client_close_total
                 << " close_missed=" << (client_close_total - plaintext_close_matches)
                 << " far_matched=" << plaintext_far_matches
                 << "/" << client_far_total
                 << " far_rejected=" << (client_far_total - plaintext_far_matches)
                 << "\n";
            if (client_far_total > 0 && plaintext_far_matches * 2 > client_far_total) {
                cout << "[CA_only warning] More than half of far Client items pass the plaintext Bloom check; "
                     << "the decrypted cardinality will be much larger than the true close count";
                if (!CA_COMPOUND_LSH) {
                    cout << ". This is the page-12 per-H_k Bloom predicate, not the compound-table "
                         << "predicate used by normal mode";
                }
                cout << ".\n";
            }

            // Line 13: encrypt each value in each D_l with pk. BFV batching packs
            // consecutive Bloom bits into vector slots, so one ciphertext covers
            // ca_batch_slots Bloom positions.
            const size_t chunks_per_filter =
                (filter_size + fhe.batch_slots - 1) / fhe.batch_slots;
            const size_t encrypted_chunk_total = l_count * chunks_per_filter;
            const bool ca_direct_encrypted_rows = CA_DISABLE_OPRF_AND_PIR;
            const bool ca_store_pir_rows_in_memory =
                ca_direct_encrypted_rows || CA_MATERIALIZE_PIR_DB || USE_PIR_BATCHPIR;
            const bool ca_use_disk_pir_rows = !ca_store_pir_rows_in_memory;
            std::string ca_pir_db_path;
            std::ofstream ca_pir_disk_out;
            std::vector<pir_punc::Row> ca_pir_rows;
            if (ca_store_pir_rows_in_memory) {
                ca_pir_rows.resize(encrypted_chunk_total);
            } else {
                ca_pir_db_path = ca_temp_pir_db_path(static_cast<size_t>(client_size), run + 1);
                ca_pir_disk_out.open(ca_pir_db_path, std::ios::binary | std::ios::trunc);
                if (!ca_pir_disk_out) {
                    throw std::runtime_error("CA_only failed to create disk-backed PIR DB: " + ca_pir_db_path);
                }
            }
            cout << "[CA_only phase] Server encrypt packed Bloom chunks start"
                 << " total_chunks=" << encrypted_chunk_total
                 << " chunks_each_filter=" << chunks_per_filter
                 << " filters=" << l_count
                 << " bits_each=" << filter_size
                 << " slots_per_ciphertext=" << fhe.batch_slots
                 << " pir_row_storage="
                 << (ca_direct_encrypted_rows
                         ? "direct_memory"
                         : (ca_use_disk_pir_rows ? "disk" : "memory"))
                 << " pir_chunk_cache_limit=" << CA_PIR_CHUNK_CACHE_LIMIT
                 << "\n";
            const auto server_encrypt_start = Clock::now();
            const size_t encrypt_stride = std::max<size_t>(1, encrypted_chunk_total / 20);
            size_t encrypted_chunks_done = 0;
            size_t ca_pir_row_bytes = 0;
            size_t max_serialized_chunk_bytes = 0;
            for (size_t l = 0; l < l_count; ++l) {
                cout << "[CA_only phase] Encrypting Bloom filter " << (l + 1)
                     << "/" << l_count << "\n";
                for (size_t chunk = 0; chunk < chunks_per_filter; ++chunk) {
                    std::vector<int64_t> slots(fhe.batch_slots, 0);
                    const size_t base = chunk * fhe.batch_slots;
                    const size_t count = std::min(fhe.batch_slots, filter_size - base);
                    for (size_t offset = 0; offset < count; ++offset) {
                        slots[offset] = bloom_filters[l][base + offset] ? 1 : 0;
                    }
                    auto encrypted_chunk = ca_encrypt_slots(fhe, std::move(slots));
                    auto serialized_chunk = ca_serialize_ciphertext(encrypted_chunk);
                    if (ca_pir_row_bytes == 0) {
                        ca_pir_row_bytes = sizeof(uint64_t) + serialized_chunk.size();
                        const double estimated_logical_db_mb =
                            static_cast<double>(ca_pir_row_bytes) *
                            static_cast<double>(encrypted_chunk_total) / 1'000'000.0;
                        cout << "[CA_only diagnostic] First encrypted Bloom chunk"
                             << " row_bytes=" << ca_pir_row_bytes
                             << " estimated_logical_db_mb=" << estimated_logical_db_mb
                             << " storage="
                             << (ca_direct_encrypted_rows
                                     ? "direct_memory"
                                     : (ca_use_disk_pir_rows ? "disk_sharded" : "memory"))
                             << "\n";
                        if (estimated_logical_db_mb > 10'000.0) {
                            cout << "[CA_only warning] Encrypted Bloom PIR DB is very large; "
                                 << "increase --ca_batch_slots if key generation can handle it, "
                                 << "or reduce --filter_size/L.\n";
                        }
                    }
                    if (serialized_chunk.size() > ca_pir_row_bytes - sizeof(uint64_t)) {
                        throw std::runtime_error(
                            "CA_only encrypted Bloom ciphertext row grew after the first row; "
                            "fixed-row PIR storage cannot pack it"
                        );
                    }
                    max_serialized_chunk_bytes = std::max(
                        max_serialized_chunk_bytes,
                        serialized_chunk.size()
                    );
                    const size_t row_idx = l * chunks_per_filter + chunk;
                    auto pir_row = ca_pack_ciphertext_pir_row(
                        serialized_chunk,
                        ca_pir_row_bytes
                    );
                    if (ca_store_pir_rows_in_memory) {
                        ca_pir_rows[row_idx] = std::move(pir_row);
                    } else {
                        ca_write_pir_row_to_stream(ca_pir_disk_out, pir_row);
                    }
                    ++encrypted_chunks_done;
                    if (ca_should_print_progress(encrypted_chunks_done, encrypted_chunk_total, encrypt_stride)) {
                        ca_progress(
                            "server packed Bloom encryption",
                            encrypted_chunks_done,
                            encrypted_chunk_total,
                            server_encrypt_start
                        );
                    }
                }
            }
            if (ca_use_disk_pir_rows) {
                ca_pir_disk_out.close();
                if (!ca_pir_disk_out) {
                    throw std::runtime_error("CA_only failed to finalize disk-backed PIR DB: " + ca_pir_db_path);
                }
            }
            const auto server_encrypt_end = Clock::now();
            cout << "[CA_only phase] Server packed Bloom encryption done in "
                 << elapsed_ms(server_encrypt_start, server_encrypt_end) / 1000.0
                 << "s row_bytes=" << ca_pir_row_bytes
                 << " logical_db_mb="
                 << (static_cast<double>(ca_pir_row_bytes) *
                     static_cast<double>(encrypted_chunk_total) / 1'000'000.0);
            if (ca_use_disk_pir_rows) {
                cout << " disk_db=" << ca_pir_db_path;
            }
            cout << "\n";

            const auto ca_pir_db_start = Clock::now();
            cout << "[CA_only phase] "
                 << (ca_direct_encrypted_rows
                         ? "Preparing direct encrypted Bloom chunk access"
                         : "Building PIR reader over encrypted Bloom chunks")
                 << " rows=" << encrypted_chunk_total
                 << " pir_mode=" << active_pir_label()
                 << " storage="
                 << (ca_direct_encrypted_rows
                         ? "direct_memory"
                         : (ca_use_disk_pir_rows ? "disk_sharded" : "memory"))
                 << "\n";

            std::unique_ptr<PirDoubleRowReaderBase> ca_pir_double_reader;
            std::unique_ptr<fuzzy_pets_batchpir::RowReader> ca_batchpir_reader;
            std::shared_ptr<std::ifstream> ca_pir_disk_in;
            std::function<pir_punc::Row(size_t)> ca_pir_row_builder;
            if (ca_use_disk_pir_rows) {
                ca_pir_disk_in = std::make_shared<std::ifstream>(ca_pir_db_path, std::ios::binary);
                if (!*ca_pir_disk_in) {
                    throw std::runtime_error("CA_only failed to open disk-backed PIR DB: " + ca_pir_db_path);
                }
                ca_pir_row_builder = [ca_pir_disk_in, ca_pir_db_path, ca_pir_row_bytes](size_t row) {
                    return ca_read_pir_row_from_stream(
                        *ca_pir_disk_in,
                        ca_pir_db_path,
                        ca_pir_row_bytes,
                        row
                    );
                };
            } else {
                ca_pir_row_builder = [&ca_pir_rows](size_t row) {
                    return ca_pir_rows.at(row);
                };
            }

            if (ca_direct_encrypted_rows) {
                cout << "[CA_only phase] Direct encrypted Bloom row access selected; PIR disabled\n";
            } else if (USE_PIR_BATCHPIR) {
                if (!CA_MATERIALIZE_PIR_DB) {
                    cout << "[CA_only warning] PIR_BatchPIR still builds its raw DB in memory; "
                         << "large encrypted Bloom rows may need --PIR_double instead.\n";
                }
                ca_batchpir_reader = fuzzy_pets_batchpir::make_row_reader(
                    encrypted_chunk_total,
                    ca_pir_row_bytes,
                    ca_pir_row_builder,
                    PIR_BATCHPIR_BATCH_SIZE
                );
            } else if (CA_MATERIALIZE_PIR_DB) {
                ca_pir_double_reader = std::make_unique<PirDoubleRowReader>(ca_pir_rows);
            } else {
                const size_t effective_shard_rows =
                    std::max<size_t>(1, std::min(CA_PIR_SHARD_ROWS, encrypted_chunk_total));
                cout << "[CA_only phase] PIR_double sharded reader"
                     << " shard_rows=" << effective_shard_rows
                     << " estimated_shard_mb="
                     << (static_cast<double>(effective_shard_rows) *
                         static_cast<double>(ca_pir_row_bytes) / 1'000'000.0)
                     << "\n";
                ca_pir_double_reader = std::make_unique<ShardedPirDoubleRowReader>(
                    encrypted_chunk_total,
                    effective_shard_rows,
                    ca_pir_row_builder
                );
            }
            const auto ca_pir_db_end = Clock::now();
            cout << "[CA_only phase] "
                 << (ca_direct_encrypted_rows
                         ? "Direct encrypted Bloom access ready in "
                         : "Encrypted Bloom PIR reader ready in ")
                 << elapsed_ms(ca_pir_db_start, ca_pir_db_end) / 1000.0
                 << "s row_bytes=" << ca_pir_row_bytes
                 << " max_serialized_chunk_bytes=" << max_serialized_chunk_bytes
                 << " logical_db_mb="
                 << (static_cast<double>(ca_pir_row_bytes) *
                     static_cast<double>(encrypted_chunk_total) / 1'000'000.0)
                 << "\n";

            std::unordered_map<size_t, lbcrypto::Ciphertext<lbcrypto::DCRTPoly>> ca_pir_chunk_cache;
            std::deque<size_t> ca_pir_chunk_cache_fifo;
            size_t ca_pir_chunk_reads = 0;
            size_t ca_pir_cache_hits = 0;
            auto ca_read_pir_row = [&](size_t row_idx) -> std::vector<uint8_t> {
                if (ca_direct_encrypted_rows) {
                    return ca_pir_rows.at(row_idx);
                }
                if (USE_PIR_BATCHPIR) {
                    return ca_batchpir_reader->read_row(row_idx);
                }
                return ca_pir_double_reader->read_row(row_idx);
            };
            auto ca_get_encrypted_chunk = [&](size_t l, size_t chunk)
                -> lbcrypto::Ciphertext<lbcrypto::DCRTPoly> {
                const size_t row_idx = l * chunks_per_filter + chunk;
                if (CA_PIR_CHUNK_CACHE_LIMIT > 0) {
                    auto cached = ca_pir_chunk_cache.find(row_idx);
                    if (cached != ca_pir_chunk_cache.end()) {
                        ++ca_pir_cache_hits;
                        return cached->second;
                    }
                }
                auto loaded = ca_unpack_ciphertext_pir_row(ca_read_pir_row(row_idx));
                ++ca_pir_chunk_reads;
                if (CA_PIR_CHUNK_CACHE_LIMIT > 0) {
                    while (ca_pir_chunk_cache.size() >= CA_PIR_CHUNK_CACHE_LIMIT &&
                           !ca_pir_chunk_cache_fifo.empty()) {
                        ca_pir_chunk_cache.erase(ca_pir_chunk_cache_fifo.front());
                        ca_pir_chunk_cache_fifo.pop_front();
                    }
                    ca_pir_chunk_cache_fifo.push_back(row_idx);
                    ca_pir_chunk_cache.emplace(row_idx, loaded);
                }
                return loaded;
            };

            cout << "[CA_only phase] Planning packed bit routes for Client queries...\n";
            const auto route_plan_start = Clock::now();
            std::vector<std::vector<unsigned char>> needed_bloom_bits(
                l_count,
                std::vector<unsigned char>(filter_size, 0)
            );
            size_t queried_bloom_bit_accesses = 0;
            size_t unique_queried_bloom_bits = 0;
            for (const auto& x : client_imgs) {
                const auto keys_by_table = ca_lsh_keys_for_item(lsh, x, CA_COMPOUND_LSH, CA_USE_OPRF);
                for (size_t l = 0; l < l_count; ++l) {
                    for (size_t kk = 0; kk < keys_by_table[l].size(); ++kk) {
                        const auto indices = ca_bloom_indices(
                            keys_by_table[l][kk],
                            filter_size,
                            ca_bloom_hash_count
                        );
                        for (size_t idx : indices) {
                            ++queried_bloom_bit_accesses;
                            if (!needed_bloom_bits[l][idx]) {
                                needed_bloom_bits[l][idx] = 1;
                                ++unique_queried_bloom_bits;
                            }
                        }
                    }
                }
            }
            const auto route_plan_end = Clock::now();
            cout << "[CA_only phase] Packed bit routing plan"
                 << " total_accesses=" << queried_bloom_bit_accesses
                 << " unique_bits=" << unique_queried_bloom_bits
                 << " reuse_saved=" << (queried_bloom_bit_accesses - unique_queried_bloom_bits)
                 << " planning_runtime_s=" << elapsed_ms(route_plan_start, route_plan_end) / 1000.0
                 << "\n";
            cout << "[CA_only phase] "
                 << (ca_direct_encrypted_rows
                         ? "Direct packed bit routing selected; PIR disabled"
                         : "PIR-backed packed bit routing selected; no pre-extraction cache needed")
                 << "\n";

            // Lines 14-26: client retrieves encrypted Bloom values and sums in_x.
            cout << "[CA_only phase] Client homomorphic cardinality start"
                 << " client=" << client_imgs.size()
                 << " encrypted_membership_checks=" << (client_imgs.size() * membership_terms_per_item)
                 << " encrypted_bloom_bit_reads="
                 << (client_imgs.size() * membership_terms_per_item * ca_bloom_hash_count)
                 << " client_batch_slots=" << fhe.batch_slots
                 << "\n";
            const auto client_eval_start = Clock::now();
            auto encrypted_zero = ca_encrypt_scalar(fhe, 0);
            auto encrypted_sum = encrypted_zero;

            const size_t client_batch_size = fhe.batch_slots;
            const size_t client_batch_count =
                (client_imgs.size() + client_batch_size - 1) / client_batch_size;
            const size_t client_stride = std::max<size_t>(1, client_batch_count / 10);
            for (size_t batch = 0; batch < client_batch_count; ++batch) {
                const size_t batch_start = batch * client_batch_size;
                const size_t batch_count = std::min(client_batch_size, client_imgs.size() - batch_start);

                std::vector<std::vector<std::vector<uint64_t>>> batch_keys;
                batch_keys.reserve(batch_count);
                for (size_t offset = 0; offset < batch_count; ++offset) {
                    batch_keys.push_back(ca_lsh_keys_for_item(
                        lsh,
                        client_imgs[batch_start + offset],
                        CA_COMPOUND_LSH,
                        CA_USE_OPRF
                    ));
                }

                std::vector<lbcrypto::Ciphertext<lbcrypto::DCRTPoly>> member_terms;
                member_terms.reserve(membership_terms_per_item);
                for (size_t l = 0; l < l_count; ++l) {
                    for (size_t kk = 0; kk < batch_keys[0][l].size(); ++kk) {
                        std::vector<lbcrypto::Ciphertext<lbcrypto::DCRTPoly>> hash_bit_batches(
                            ca_bloom_hash_count,
                            encrypted_zero
                        );

                        for (size_t offset = 0; offset < batch_count; ++offset) {
                            const auto indices = ca_bloom_indices(
                                batch_keys[offset][l][kk],
                                filter_size,
                                ca_bloom_hash_count
                            );

                            for (size_t hash_idx = 0; hash_idx < indices.size(); ++hash_idx) {
                                const size_t chunk = indices[hash_idx] / fhe.batch_slots;
                                const size_t slot = indices[hash_idx] % fhe.batch_slots;
                                hash_bit_batches[hash_idx] = fhe.cc->EvalAdd(
                                    hash_bit_batches[hash_idx],
                                    ca_route_packed_bit(
                                        fhe,
                                        ca_get_encrypted_chunk(l, chunk),
                                        slot,
                                        offset
                                    )
                                );
                            }
                        }

                        member_terms.push_back(ca_and_tree(fhe, std::move(hash_bit_batches)));
                    }
                }

                auto in_batch = ca_or_tree(fhe, std::move(member_terms), encrypted_zero);
                auto batch_sum = ca_sum_slots_to_scalar(fhe, in_batch, batch_count);
                encrypted_sum = fhe.cc->EvalAdd(encrypted_sum, batch_sum);

                const size_t done_batches = batch + 1;
                if (ca_should_print_progress(done_batches, client_batch_count, client_stride)) {
                    const size_t done_items = std::min(client_imgs.size(), batch_start + batch_count);
                    ca_progress("client homomorphic Client batches", done_batches, client_batch_count, client_eval_start);
                    cout << "[CA_only progress] client homomorphic Client items "
                         << done_items << "/" << client_imgs.size() << "\n";
                }
            }
            const auto client_eval_end = Clock::now();
            cout << "[CA_only phase] Client homomorphic cardinality done in "
                 << elapsed_ms(client_eval_start, client_eval_end) / 1000.0 << "s\n";
            cout << "[CA_only phase] Encrypted Bloom "
                 << (ca_direct_encrypted_rows ? "direct" : "PIR")
                 << " chunk reads"
                 << " reads=" << ca_pir_chunk_reads
                 << " cache_hits=" << ca_pir_cache_hits
                 << " cache_size=" << ca_pir_chunk_cache.size()
                 << "/" << CA_PIR_CHUNK_CACHE_LIMIT
                 << " total_chunks=" << encrypted_chunk_total
                 << "\n";

            // Line 27: client and server jointly decrypt the encrypted sum.
            cout << "[CA_only phase] Joint decryption start\n";
            const auto decrypt_start = Clock::now();
            const int64_t cardinality = ca_threshold_decrypt_scalar(fhe, encrypted_sum);
            const auto decrypt_end = Clock::now();
            const auto total_end = Clock::now();
            cout << "[CA_only phase] Joint decryption done in "
                 << elapsed_ms(decrypt_start, decrypt_end) / 1000.0 << "s\n";
            if (cardinality == plaintext_cardinality_check) {
                cout << "[CA_only check] decrypted cardinality matches plaintext Bloom count: "
                     << cardinality << "\n";
            } else {
                cout << "[CA_only check] WARNING decrypted cardinality=" << cardinality
                     << " plaintext Bloom count=" << plaintext_cardinality_check
                     << "\n";
            }
            const DiscoGcaesOprfStats ca_oprf_stats_for_run =
                disco_gcaes_oprf_stats_delta(ca_oprf_stats_before_run, disco_gcaes_oprf_stats());
            const std::uint64_t ca_oprf_communication_bytes_for_run =
                ca_oprf_stats_for_run.bytes_sent + ca_oprf_stats_for_run.bytes_recv;
            if (CA_USE_OPRF) {
                cout << "[CA_only phase] " << disco_oprf_mechanism()
                     << " OPRF measured"
                     << " calls=" << ca_oprf_stats_for_run.calls
                     << " blocks=" << ca_oprf_stats_for_run.blocks
                     << " runtime_s=" << (ca_oprf_stats_for_run.runtime_ms / 1000.0)
                     << " comm_mb="
                     << (static_cast<double>(ca_oprf_communication_bytes_for_run) / 1'000'000.0)
                     << "\n";
            }

            cardinality_sum += static_cast<long double>(cardinality);
            keygen_runtime_ms_sum += elapsed_ms(keygen_start, keygen_end);
            server_bloom_runtime_ms_sum += elapsed_ms(server_bloom_start, server_bloom_end);
            server_encrypt_runtime_ms_sum += elapsed_ms(server_encrypt_start, server_encrypt_end);
            ca_pir_db_runtime_ms_sum += elapsed_ms(ca_pir_db_start, ca_pir_db_end);
            if (ca_direct_encrypted_rows) {
                // Direct encrypted-row access intentionally has no PIR communication/runtime.
            } else if (USE_PIR_BATCHPIR) {
                ca_pir_client_runtime_ms_sum += ca_batchpir_reader->client_runtime_ms();
                ca_pir_server_runtime_ms_sum += ca_batchpir_reader->server_runtime_ms();
                ca_pir_setup_communication_bytes_sum +=
                    static_cast<long double>(ca_batchpir_reader->setup_communication_bytes());
                ca_pir_query_communication_bytes_sum +=
                    static_cast<long double>(ca_batchpir_reader->query_communication_bytes());
                ca_pir_response_communication_bytes_sum +=
                    static_cast<long double>(ca_batchpir_reader->response_communication_bytes());
            } else {
                ca_pir_client_runtime_ms_sum += ca_pir_double_reader->client_runtime_ms();
                ca_pir_server_runtime_ms_sum += ca_pir_double_reader->server_runtime_ms();
                ca_pir_setup_communication_bytes_sum +=
                    static_cast<long double>(ca_pir_double_reader->setup_communication_bytes());
                ca_pir_query_communication_bytes_sum +=
                    static_cast<long double>(ca_pir_double_reader->query_communication_bytes());
                ca_pir_response_communication_bytes_sum +=
                    static_cast<long double>(ca_pir_double_reader->response_communication_bytes());
            }
            packed_route_planning_runtime_ms_sum += elapsed_ms(route_plan_start, route_plan_end);
            client_eval_runtime_ms_sum += elapsed_ms(client_eval_start, client_eval_end);
            decrypt_runtime_ms_sum += elapsed_ms(decrypt_start, decrypt_end);
            total_runtime_ms_sum += elapsed_ms(total_start, total_end);
            ca_oprf_runtime_ms_sum += ca_oprf_stats_for_run.runtime_ms;
            ca_oprf_communication_bytes_sum +=
                static_cast<long double>(ca_oprf_communication_bytes_for_run);
            ca_oprf_client_to_server_bytes_sum +=
                static_cast<long double>(ca_oprf_stats_for_run.bytes_sent);
            ca_oprf_server_to_client_bytes_sum +=
                static_cast<long double>(ca_oprf_stats_for_run.bytes_recv);
            ca_oprf_calls_sum += static_cast<long double>(ca_oprf_stats_for_run.calls);
            ca_oprf_blocks_sum += static_cast<long double>(ca_oprf_stats_for_run.blocks);

            if (ca_use_disk_pir_rows) {
                ca_pir_double_reader.reset();
                ca_batchpir_reader.reset();
                ca_pir_row_builder = nullptr;
                ca_pir_disk_in.reset();
                if (std::remove(ca_pir_db_path.c_str()) != 0) {
                    cout << "[CA_only warning] Could not remove temporary PIR DB "
                         << ca_pir_db_path << "\n";
                }
            }

            cout << "[CA_only run] client_size=" << client_size
                 << " cardinality=" << cardinality
                 << " total_runtime_s=" << elapsed_ms(total_start, total_end) / 1000.0
                 << "\n";
        }

        cout << "\n[CA_ONLY AVG] mode=" << mode
             << " filter=Bloom"
             << " server_size=" << N
             << " client_size=" << client_size
             << "\n"
             << "  Fuzzy Cardinality: "
             << static_cast<double>(cardinality_sum / NUM_RUNS_PSI)
             << "\n"
             << "  Total Runtime: "
             << (total_runtime_ms_sum / NUM_RUNS_PSI) / 1000.0 << " s"
             << "\n"
             << "  Total Communication Cost"
             << (CA_USE_OPRF
                     ? (std::string(" (PIR + ") + disco_oprf_mechanism() + " OPRF)")
                     : "")
             << ": "
             << static_cast<double>((
                    ca_pir_setup_communication_bytes_sum +
                    ca_pir_query_communication_bytes_sum +
                    ca_pir_response_communication_bytes_sum +
                    ca_oprf_communication_bytes_sum
                ) / NUM_RUNS_PSI / 1'000'000.0L)
             << " MB"
             << "\n"
             << "  THE KeyGen Runtime: "
             << (keygen_runtime_ms_sum / NUM_RUNS_PSI) / 1000.0 << " s"
             << "\n"
             << "  Server Bloom Build Runtime: "
             << (server_bloom_runtime_ms_sum / NUM_RUNS_PSI) / 1000.0 << " s"
             << "\n"
             << "  Server Bloom Encryption Runtime: "
             << (server_encrypt_runtime_ms_sum / NUM_RUNS_PSI) / 1000.0 << " s"
             << "\n"
             << "  Encrypted Bloom PIR DB Runtime: "
             << (ca_pir_db_runtime_ms_sum / NUM_RUNS_PSI) / 1000.0 << " s"
             << "\n"
             << "  Encrypted Bloom PIR Client Runtime: "
             << (ca_pir_client_runtime_ms_sum / NUM_RUNS_PSI) / 1000.0 << " s"
             << "\n"
             << "  Encrypted Bloom PIR Server Runtime: "
             << (ca_pir_server_runtime_ms_sum / NUM_RUNS_PSI) / 1000.0 << " s"
             << "\n"
             << "  " << disco_oprf_mechanism() << " OPRF Runtime (included in total): "
             << (ca_oprf_runtime_ms_sum / NUM_RUNS_PSI) / 1000.0 << " s"
             << "\n"
             << "  " << disco_oprf_mechanism() << " OPRF Communication Cost (included in total): "
             << static_cast<double>((ca_oprf_communication_bytes_sum / NUM_RUNS_PSI) / 1'000'000.0L)
             << " MB"
             << "\n"
             << "  " << disco_oprf_mechanism() << " OPRF Client -> Server Communication: "
             << static_cast<double>((ca_oprf_client_to_server_bytes_sum / NUM_RUNS_PSI) / 1'000'000.0L)
             << " MB"
             << "\n"
             << "  " << disco_oprf_mechanism() << " OPRF Server -> Client Communication: "
             << static_cast<double>((ca_oprf_server_to_client_bytes_sum / NUM_RUNS_PSI) / 1'000'000.0L)
             << " MB"
             << "\n"
             << "  " << disco_oprf_mechanism() << " OPRF Calls: "
             << static_cast<double>(ca_oprf_calls_sum / NUM_RUNS_PSI)
             << "\n"
             << "  " << disco_oprf_mechanism() << " OPRF Blocks: "
             << static_cast<double>(ca_oprf_blocks_sum / NUM_RUNS_PSI)
             << "\n"
             << "  Encrypted Bloom PIR Setup Communication: "
             << static_cast<double>((ca_pir_setup_communication_bytes_sum / NUM_RUNS_PSI) / 1'000'000.0L)
             << " MB"
             << "\n"
             << "  Encrypted Bloom PIR Query Communication: "
             << static_cast<double>((ca_pir_query_communication_bytes_sum / NUM_RUNS_PSI) / 1'000'000.0L)
             << " MB"
             << "\n"
             << "  Encrypted Bloom PIR Response Communication: "
             << static_cast<double>((ca_pir_response_communication_bytes_sum / NUM_RUNS_PSI) / 1'000'000.0L)
             << " MB"
             << "\n"
             << "  Packed Query Planning Runtime: "
             << (packed_route_planning_runtime_ms_sum / NUM_RUNS_PSI) / 1000.0 << " s"
             << "\n"
             << "  Client Homomorphic Cardinality Runtime: "
             << (client_eval_runtime_ms_sum / NUM_RUNS_PSI) / 1000.0 << " s"
             << "\n"
             << "  Joint Decryption Runtime: "
             << (decrypt_runtime_ms_sum / NUM_RUNS_PSI) / 1000.0 << " s"
             << "\n";
    }

    return 0;
}

static int fuzzypsi_cardinality_only_cuckoo(
    int L,
    int K,
    double w,
    const string& mode
) {
    if (filter_size == 0) {
        throw std::invalid_argument("CA_only Cuckoo requires --filter_size > 0");
    }
    if (L <= 0 || K <= 0) {
        throw std::invalid_argument("CA_only Cuckoo requires L and k to be > 0");
    }
    if (CA_CUCKOO_FP_BITS == 0 || CA_CUCKOO_FP_BITS > 32) {
        throw std::invalid_argument("--ca_cuckoo_fp_bits must be in [1,32]");
    }
    if (CA_CUCKOO_BUCKET_TAG_BITS == 0 || CA_CUCKOO_TAG_HASHES == 0) {
        throw std::invalid_argument("--ca_cuckoo_bucket_tag_bits and --ca_cuckoo_tag_hashes must be > 0");
    }

    const size_t l_count = static_cast<size_t>(L);
    const size_t k_count = static_cast<size_t>(K);
    const size_t terms_per_filter = CA_COMPOUND_LSH ? 1 : k_count;
    const size_t membership_terms_per_item = l_count * terms_per_filter;
    const size_t fhe_depth = std::max<size_t>(
        3,
        ca_ceil_log2(CA_CUCKOO_TAG_HASHES) + 1 + ca_ceil_log2(membership_terms_per_item)
    );
    E2LSH lsh(dim, L, K, w);

    cout << "[CA_only] Cuckoo fuzzy cardinality with X=Client, Y=Server"
         << " L=" << L
         << " K=" << K
         << " ca_lsh_mode=" << (CA_COMPOUND_LSH ? "compound_table_keys" : "page12_individual_Hk")
         << " ca_oprf=" << (CA_USE_OPRF ? "true" : "false")
         << " cuckoo_slots=" << filter_size
         << " cuckoo_bucket_size=4"
         << " ca_cuckoo_fp_bits=" << CA_CUCKOO_FP_BITS
         << " ca_cuckoo_bucket_tag_bits=" << CA_CUCKOO_BUCKET_TAG_BITS
         << " ca_cuckoo_tag_hashes=" << CA_CUCKOO_TAG_HASHES
         << " plaintext_modulus=" << CA_PLAINTEXT_MODULUS
         << " requested_batch_slots=" << CA_BATCH_SLOTS
         << " membership_terms_per_item=" << membership_terms_per_item
         << " fhe_depth=" << fhe_depth
         << "\n";

    vector<string> server_ids;
    vector<vector<double>> server_imgs, client_imgs;
    vector<bool> client_is_close;

    for (int client_size : CLIENT_SIZES_LIST) {
        if (static_cast<std::uint64_t>(client_size) >= CA_PLAINTEXT_MODULUS) {
            throw std::runtime_error("CA_only Cuckoo client_size exceeds FHE plaintext modulus");
        }

        long double cardinality_sum = 0.0L;
        double keygen_runtime_ms_sum = 0.0;
        double server_cuckoo_runtime_ms_sum = 0.0;
        double server_encrypt_runtime_ms_sum = 0.0;
        double ca_pir_db_runtime_ms_sum = 0.0;
        double ca_pir_client_runtime_ms_sum = 0.0;
        double ca_pir_server_runtime_ms_sum = 0.0;
        double packed_route_planning_runtime_ms_sum = 0.0;
        double client_eval_runtime_ms_sum = 0.0;
        double decrypt_runtime_ms_sum = 0.0;
        double total_runtime_ms_sum = 0.0;
        double ca_oprf_runtime_ms_sum = 0.0;
        long double ca_pir_setup_communication_bytes_sum = 0.0L;
        long double ca_pir_query_communication_bytes_sum = 0.0L;
        long double ca_pir_response_communication_bytes_sum = 0.0L;
        long double ca_oprf_communication_bytes_sum = 0.0L;
        long double ca_oprf_client_to_server_bytes_sum = 0.0L;
        long double ca_oprf_server_to_client_bytes_sum = 0.0L;
        long double ca_oprf_calls_sum = 0.0L;
        long double ca_oprf_blocks_sum = 0.0L;

        for (int run = 0; run < NUM_RUNS_PSI; ++run) {
            const uint32_t run_seed = dataset_seed_for_run(run);
            mt19937 run_rng(run_seed);
            cout << "[CA_only dataset seed] client_size=" << client_size
                 << " run=" << (run + 1) << "/" << NUM_RUNS_PSI
                 << " seed=" << run_seed << "\n";

            cout << "[CA_only phase] Loading/sampling Server and Client datasets...\n";
            load_server_from_json(server_path, N, run_rng, server_ids, server_imgs);
            load_client_from_json_for_server(
                server_ids,
                server_imgs,
                client_size,
                run_rng,
                client_imgs,
                client_is_close
            );
            cout << "[CA_only phase] Dataset ready: server=" << server_imgs.size()
                 << " client=" << client_imgs.size() << "\n";
            const size_t client_close_total = static_cast<size_t>(
                std::count(client_is_close.begin(), client_is_close.end(), true)
            );
            const size_t client_far_total = client_imgs.size() - client_close_total;
            cout << "[CA_only diagnostic] Client ground truth split"
                 << " close=" << client_close_total
                 << " far=" << client_far_total
                 << "\n";

            const auto total_start = Clock::now();
            const DiscoGcaesOprfStats ca_oprf_stats_before_run = disco_gcaes_oprf_stats();

            if (CA_DISABLE_OPRF_AND_PIR) {
                cout << "[CA_only phase] Plain Cuckoo cardinality start; "
                     << "OPRF/PIR/THE disabled for low-memory validation\n";
                cout << "[CA_only phase] Server Cuckoo build start"
                     << " server=" << server_imgs.size()
                     << " terms_per_filter=" << terms_per_filter
                     << " total_terms_per_item=" << membership_terms_per_item
                     << "\n";
                const auto plain_cuckoo_start = Clock::now();
                const auto server_cuckoo_start = Clock::now();
                CuckooFilter cuckoo(filter_size);
                const size_t cuckoo_build_stride = std::max<size_t>(1, server_imgs.size() / 10);
                for (size_t y_idx = 0; y_idx < server_imgs.size(); ++y_idx) {
                    const auto keys_by_table = CA_PLAIN_LOCAL_PRF
                        ? ca_cuckoo_keys_for_item_local_prf(lsh, server_imgs[y_idx], CA_COMPOUND_LSH)
                        : ca_cuckoo_keys_for_item(lsh, server_imgs[y_idx], CA_COMPOUND_LSH, false);
                    for (size_t l = 0; l < keys_by_table.size(); ++l) {
                        for (const auto& encoded : keys_by_table[l]) {
                            cuckoo.insert_encoded(encoded.bucket, encoded.tag);
                        }
                    }
                    const size_t done = y_idx + 1;
                    if (ca_should_print_progress(done, server_imgs.size(), cuckoo_build_stride)) {
                        ca_progress("plain server Cuckoo build", done, server_imgs.size(), server_cuckoo_start);
                    }
                }

                const size_t cuckoo_bucket_count = cuckoo.bucket_count();
                const size_t cuckoo_bucket_size = cuckoo.slots_per_bucket();
                const size_t cuckoo_tag_bit_count = cuckoo_bucket_count * CA_CUCKOO_BUCKET_TAG_BITS;
                std::vector<unsigned char> cuckoo_tag_bits(cuckoo_tag_bit_count, 0);
                size_t cuckoo_tag_bit_ones = 0;
                for (size_t bucket = 0; bucket < cuckoo_bucket_count; ++bucket) {
                    const auto row = cuckoo.bucket_row(bucket);
                    for (size_t slot_idx = 0; slot_idx < cuckoo_bucket_size; ++slot_idx) {
                        const uint32_t tag = ca_read_u32_le_at(row, slot_idx * sizeof(uint32_t));
                        if (tag == 0) {
                            continue;
                        }
                        for (size_t tag_idx : ca_cuckoo_tag_indices(tag)) {
                            const size_t bit_idx = ca_cuckoo_bucket_tag_bit_index(bucket, tag_idx);
                            if (!cuckoo_tag_bits[bit_idx]) {
                                cuckoo_tag_bits[bit_idx] = 1;
                                ++cuckoo_tag_bit_ones;
                            }
                        }
                    }
                }
                const auto server_cuckoo_end = Clock::now();
                cout << "[CA_only phase] Plain server Cuckoo build done in "
                     << elapsed_ms(server_cuckoo_start, server_cuckoo_end) / 1000.0
                     << "s buckets=" << cuckoo_bucket_count
                     << " slots=" << cuckoo.slot_count()
                     << " fill=" << cuckoo.fill_ratio()
                     << " failed_inserts=" << cuckoo.failed_insert_count()
                     << "/" << cuckoo.attempted_insert_count()
                     << " tag_bit_fill="
                     << (cuckoo_tag_bit_count
                             ? static_cast<double>(cuckoo_tag_bit_ones) /
                                   static_cast<double>(cuckoo_tag_bit_count)
                             : 0.0)
                     << "\n";

                const auto plain_client_start = Clock::now();
                int64_t plaintext_cardinality_check = 0;
                size_t plaintext_close_matches = 0;
                size_t plaintext_far_matches = 0;
                for (size_t x_idx = 0; x_idx < client_imgs.size(); ++x_idx) {
                    bool in_x_plain = false;
                    const auto keys_by_table = CA_PLAIN_LOCAL_PRF
                        ? ca_cuckoo_keys_for_item_local_prf(lsh, client_imgs[x_idx], CA_COMPOUND_LSH)
                        : ca_cuckoo_keys_for_item(lsh, client_imgs[x_idx], CA_COMPOUND_LSH, false);
                    for (size_t l = 0; l < keys_by_table.size() && !in_x_plain; ++l) {
                        for (const auto& encoded : keys_by_table[l]) {
                            const auto lookup = cuckoo.lookup_encoded(encoded.bucket, encoded.tag);
                            const bool member =
                                ca_cuckoo_bucket_tag_maybe_contains(
                                    cuckoo_tag_bits,
                                    cuckoo_bucket_count,
                                    lookup.i1,
                                    lookup.fp
                                ) ||
                                ca_cuckoo_bucket_tag_maybe_contains(
                                    cuckoo_tag_bits,
                                    cuckoo_bucket_count,
                                    lookup.i2,
                                    lookup.fp
                                );
                            in_x_plain = in_x_plain || member;
                            if (in_x_plain) {
                                break;
                            }
                        }
                    }
                    if (in_x_plain) {
                        ++plaintext_cardinality_check;
                        if (x_idx < client_is_close.size() && client_is_close[x_idx]) {
                            ++plaintext_close_matches;
                        } else {
                            ++plaintext_far_matches;
                        }
                    }
                }
                const auto plain_client_end = Clock::now();
                const auto total_end = Clock::now();

                cout << "[CA_only diagnostic] Plaintext Cuckoo split"
                     << " close_matched=" << plaintext_close_matches
                     << "/" << client_close_total
                     << " close_missed=" << (client_close_total - plaintext_close_matches)
                     << " far_matched=" << plaintext_far_matches
                     << "/" << client_far_total
                     << " far_rejected=" << (client_far_total - plaintext_far_matches)
                     << "\n";
                cout << "[CA_only phase] Plain Cuckoo Client cardinality done in "
                     << elapsed_ms(plain_client_start, plain_client_end) / 1000.0 << "s\n";

                cardinality_sum += static_cast<long double>(plaintext_cardinality_check);
                server_cuckoo_runtime_ms_sum += elapsed_ms(server_cuckoo_start, server_cuckoo_end);
                client_eval_runtime_ms_sum += elapsed_ms(plain_client_start, plain_client_end);
                total_runtime_ms_sum += elapsed_ms(total_start, total_end);

                cout << "[CA_only run] client_size=" << client_size
                     << " cardinality=" << plaintext_cardinality_check
                     << " total_runtime_s=" << elapsed_ms(plain_cuckoo_start, total_end) / 1000.0
                     << "\n";
                continue;
            }

            cout << "[CA_only phase] THE key generation start"
                 << " depth=" << fhe_depth
                 << " requested_batch_slots=" << CA_BATCH_SLOTS
                 << "\n";
            const auto keygen_start = Clock::now();
            CaThresholdFhe fhe = ca_threshold_keygen(fhe_depth, CA_BATCH_SLOTS);
            const auto keygen_end = Clock::now();
            cout << "[CA_only phase] THE key generation done in "
                 << elapsed_ms(keygen_start, keygen_end) / 1000.0
                 << "s actual_batch_slots=" << fhe.batch_slots
                 << "\n";

            cout << "[CA_only phase] Server Cuckoo build start"
                 << " server=" << server_imgs.size()
                 << " terms_per_filter=" << terms_per_filter
                 << " total_terms_per_item=" << membership_terms_per_item
                 << "\n";
            const auto server_cuckoo_start = Clock::now();
            CuckooFilter cuckoo(filter_size);
            const size_t cuckoo_build_stride = std::max<size_t>(1, server_imgs.size() / 10);
            for (size_t y_idx = 0; y_idx < server_imgs.size(); ++y_idx) {
                const auto keys_by_table =
                    ca_cuckoo_keys_for_item(lsh, server_imgs[y_idx], CA_COMPOUND_LSH, CA_USE_OPRF);
                for (size_t l = 0; l < keys_by_table.size(); ++l) {
                    for (const auto& encoded : keys_by_table[l]) {
                        cuckoo.insert_encoded(encoded.bucket, encoded.tag);
                    }
                }
                const size_t done = y_idx + 1;
                if (ca_should_print_progress(done, server_imgs.size(), cuckoo_build_stride)) {
                    ca_progress("server Cuckoo build", done, server_imgs.size(), server_cuckoo_start);
                }
            }

            const size_t cuckoo_bucket_count = cuckoo.bucket_count();
            const size_t cuckoo_bucket_size = cuckoo.slots_per_bucket();
            const size_t cuckoo_tag_bit_count = cuckoo_bucket_count * CA_CUCKOO_BUCKET_TAG_BITS;
            std::vector<unsigned char> cuckoo_tag_bits(cuckoo_tag_bit_count, 0);
            size_t cuckoo_tag_bit_ones = 0;
            for (size_t bucket = 0; bucket < cuckoo_bucket_count; ++bucket) {
                const auto row = cuckoo.bucket_row(bucket);
                for (size_t slot_idx = 0; slot_idx < cuckoo_bucket_size; ++slot_idx) {
                    const uint32_t tag = ca_read_u32_le_at(row, slot_idx * sizeof(uint32_t));
                    if (tag == 0) {
                        continue;
                    }
                    for (size_t tag_idx : ca_cuckoo_tag_indices(tag)) {
                        const size_t bit_idx = ca_cuckoo_bucket_tag_bit_index(bucket, tag_idx);
                        if (!cuckoo_tag_bits[bit_idx]) {
                            cuckoo_tag_bits[bit_idx] = 1;
                            ++cuckoo_tag_bit_ones;
                        }
                    }
                }
            }
            const auto server_cuckoo_end = Clock::now();
            cout << "[CA_only phase] Server Cuckoo build done in "
                 << elapsed_ms(server_cuckoo_start, server_cuckoo_end) / 1000.0
                 << "s buckets=" << cuckoo_bucket_count
                 << " slots=" << cuckoo.slot_count()
                 << " fill=" << cuckoo.fill_ratio()
                 << " failed_inserts=" << cuckoo.failed_insert_count()
                 << "/" << cuckoo.attempted_insert_count()
                 << " tag_bit_fill="
                 << (cuckoo_tag_bit_count
                         ? static_cast<double>(cuckoo_tag_bit_ones) /
                               static_cast<double>(cuckoo_tag_bit_count)
                         : 0.0)
                 << "\n";

            int64_t plaintext_cardinality_check = 0;
            size_t plaintext_close_matches = 0;
            size_t plaintext_far_matches = 0;
            for (size_t x_idx = 0; x_idx < client_imgs.size(); ++x_idx) {
                bool in_x_plain = false;
                const auto keys_by_table =
                    ca_cuckoo_keys_for_item(lsh, client_imgs[x_idx], CA_COMPOUND_LSH, CA_USE_OPRF);
                for (size_t l = 0; l < keys_by_table.size() && !in_x_plain; ++l) {
                    for (const auto& encoded : keys_by_table[l]) {
                        const auto lookup = cuckoo.lookup_encoded(encoded.bucket, encoded.tag);
                        const bool member =
                            ca_cuckoo_bucket_tag_maybe_contains(
                                cuckoo_tag_bits,
                                cuckoo_bucket_count,
                                lookup.i1,
                                lookup.fp
                            ) ||
                            ca_cuckoo_bucket_tag_maybe_contains(
                                cuckoo_tag_bits,
                                cuckoo_bucket_count,
                                lookup.i2,
                                lookup.fp
                            );
                        in_x_plain = in_x_plain || member;
                        if (in_x_plain) {
                            break;
                        }
                    }
                }
                if (in_x_plain) {
                    ++plaintext_cardinality_check;
                    if (x_idx < client_is_close.size() && client_is_close[x_idx]) {
                        ++plaintext_close_matches;
                    } else {
                        ++plaintext_far_matches;
                    }
                }
            }
            cout << "[CA_only diagnostic] Plaintext Cuckoo split"
                 << " close_matched=" << plaintext_close_matches
                 << "/" << client_close_total
                 << " close_missed=" << (client_close_total - plaintext_close_matches)
                 << " far_matched=" << plaintext_far_matches
                 << "/" << client_far_total
                 << " far_rejected=" << (client_far_total - plaintext_far_matches)
                 << "\n";

            const size_t chunks_per_table =
                (cuckoo_tag_bit_count + fhe.batch_slots - 1) / fhe.batch_slots;
            const size_t encrypted_chunk_total = chunks_per_table;
            const bool ca_direct_encrypted_rows = CA_DISABLE_OPRF_AND_PIR;
            const bool ca_store_pir_rows_in_memory =
                ca_direct_encrypted_rows || CA_MATERIALIZE_PIR_DB || USE_PIR_BATCHPIR;
            const bool ca_use_disk_pir_rows = !ca_store_pir_rows_in_memory;
            std::string ca_pir_db_path;
            std::ofstream ca_pir_disk_out;
            std::vector<pir_punc::Row> ca_pir_rows;
            if (ca_store_pir_rows_in_memory) {
                ca_pir_rows.resize(encrypted_chunk_total);
            } else {
                ca_pir_db_path = ca_temp_pir_db_path(static_cast<size_t>(client_size), run + 1);
                ca_pir_disk_out.open(ca_pir_db_path, std::ios::binary | std::ios::trunc);
                if (!ca_pir_disk_out) {
                    throw std::runtime_error("CA_only failed to create disk-backed Cuckoo PIR DB: " + ca_pir_db_path);
                }
            }

            cout << "[CA_only phase] Server encrypt packed Cuckoo tag bits start"
                 << " total_chunks=" << encrypted_chunk_total
                 << " tag_bits=" << cuckoo_tag_bit_count
                 << " slots_per_ciphertext=" << fhe.batch_slots
                 << " pir_row_storage="
                 << (ca_direct_encrypted_rows
                         ? "direct_memory"
                         : (ca_use_disk_pir_rows ? "disk" : "memory"))
                 << " pir_chunk_cache_limit=" << CA_PIR_CHUNK_CACHE_LIMIT
                 << "\n";
            const auto server_encrypt_start = Clock::now();
            const size_t encrypt_stride = std::max<size_t>(1, encrypted_chunk_total / 20);
            size_t encrypted_chunks_done = 0;
            size_t ca_pir_row_bytes = 0;
            size_t max_serialized_chunk_bytes = 0;
            for (size_t chunk = 0; chunk < chunks_per_table; ++chunk) {
                std::vector<int64_t> slots(fhe.batch_slots, 0);
                const size_t base = chunk * fhe.batch_slots;
                const size_t count = std::min(fhe.batch_slots, cuckoo_tag_bit_count - base);
                for (size_t offset = 0; offset < count; ++offset) {
                    slots[offset] = cuckoo_tag_bits[base + offset] ? 1 : 0;
                }
                auto encrypted_chunk = ca_encrypt_slots(fhe, std::move(slots));
                auto serialized_chunk = ca_serialize_ciphertext(encrypted_chunk);
                if (ca_pir_row_bytes == 0) {
                    ca_pir_row_bytes = sizeof(uint64_t) + serialized_chunk.size();
                    const double estimated_logical_db_mb =
                        static_cast<double>(ca_pir_row_bytes) *
                        static_cast<double>(encrypted_chunk_total) / 1'000'000.0;
                    cout << "[CA_only diagnostic] First encrypted Cuckoo chunk"
                         << " row_bytes=" << ca_pir_row_bytes
                         << " estimated_logical_db_mb=" << estimated_logical_db_mb
                         << " storage="
                         << (ca_direct_encrypted_rows
                                 ? "direct_memory"
                                 : (ca_use_disk_pir_rows ? "disk_sharded" : "memory"))
                         << "\n";
                }
                if (serialized_chunk.size() > ca_pir_row_bytes - sizeof(uint64_t)) {
                    throw std::runtime_error(
                        "CA_only encrypted Cuckoo ciphertext row grew after the first row"
                    );
                }
                max_serialized_chunk_bytes = std::max(max_serialized_chunk_bytes, serialized_chunk.size());
                auto pir_row = ca_pack_ciphertext_pir_row(serialized_chunk, ca_pir_row_bytes);
                if (ca_store_pir_rows_in_memory) {
                    ca_pir_rows[chunk] = std::move(pir_row);
                } else {
                    ca_write_pir_row_to_stream(ca_pir_disk_out, pir_row);
                }
                ++encrypted_chunks_done;
                if (ca_should_print_progress(encrypted_chunks_done, encrypted_chunk_total, encrypt_stride)) {
                    ca_progress(
                        "server packed Cuckoo encryption",
                        encrypted_chunks_done,
                        encrypted_chunk_total,
                        server_encrypt_start
                    );
                }
            }
            if (ca_use_disk_pir_rows) {
                ca_pir_disk_out.close();
                if (!ca_pir_disk_out) {
                    throw std::runtime_error("CA_only failed to finalize disk-backed Cuckoo PIR DB: " + ca_pir_db_path);
                }
            }
            const auto server_encrypt_end = Clock::now();
            cout << "[CA_only phase] Server packed Cuckoo encryption done in "
                 << elapsed_ms(server_encrypt_start, server_encrypt_end) / 1000.0
                 << "s row_bytes=" << ca_pir_row_bytes
                 << " logical_db_mb="
                 << (static_cast<double>(ca_pir_row_bytes) *
                     static_cast<double>(encrypted_chunk_total) / 1'000'000.0);
            if (ca_use_disk_pir_rows) {
                cout << " disk_db=" << ca_pir_db_path;
            }
            cout << "\n";

            const auto ca_pir_db_start = Clock::now();
            cout << "[CA_only phase] "
                 << (ca_direct_encrypted_rows
                         ? "Preparing direct encrypted Cuckoo tag-bit access"
                         : "Building PIR reader over encrypted Cuckoo tag bits")
                 << " rows=" << encrypted_chunk_total
                 << " pir_mode=" << active_pir_label()
                 << " storage="
                 << (ca_direct_encrypted_rows
                         ? "direct_memory"
                         : (ca_use_disk_pir_rows ? "disk_sharded" : "memory"))
                 << "\n";

            std::unique_ptr<PirDoubleRowReaderBase> ca_pir_double_reader;
            std::unique_ptr<fuzzy_pets_batchpir::RowReader> ca_batchpir_reader;
            std::shared_ptr<std::ifstream> ca_pir_disk_in;
            std::function<pir_punc::Row(size_t)> ca_pir_row_builder;
            if (ca_use_disk_pir_rows) {
                ca_pir_disk_in = std::make_shared<std::ifstream>(ca_pir_db_path, std::ios::binary);
                if (!*ca_pir_disk_in) {
                    throw std::runtime_error("CA_only failed to open disk-backed Cuckoo PIR DB: " + ca_pir_db_path);
                }
                ca_pir_row_builder = [ca_pir_disk_in, ca_pir_db_path, ca_pir_row_bytes](size_t row) {
                    return ca_read_pir_row_from_stream(
                        *ca_pir_disk_in,
                        ca_pir_db_path,
                        ca_pir_row_bytes,
                        row
                    );
                };
            } else {
                ca_pir_row_builder = [&ca_pir_rows](size_t row) {
                    return ca_pir_rows.at(row);
                };
            }

            if (ca_direct_encrypted_rows) {
                cout << "[CA_only phase] Direct encrypted Cuckoo row access selected; PIR disabled\n";
            } else if (USE_PIR_BATCHPIR) {
                ca_batchpir_reader = fuzzy_pets_batchpir::make_row_reader(
                    encrypted_chunk_total,
                    ca_pir_row_bytes,
                    ca_pir_row_builder,
                    PIR_BATCHPIR_BATCH_SIZE
                );
            } else if (CA_MATERIALIZE_PIR_DB) {
                ca_pir_double_reader = std::make_unique<PirDoubleRowReader>(ca_pir_rows);
            } else {
                const size_t effective_shard_rows =
                    std::max<size_t>(1, std::min(CA_PIR_SHARD_ROWS, encrypted_chunk_total));
                cout << "[CA_only phase] PIR_double sharded reader"
                     << " shard_rows=" << effective_shard_rows
                     << " estimated_shard_mb="
                     << (static_cast<double>(effective_shard_rows) *
                         static_cast<double>(ca_pir_row_bytes) / 1'000'000.0)
                     << "\n";
                ca_pir_double_reader = std::make_unique<ShardedPirDoubleRowReader>(
                    encrypted_chunk_total,
                    effective_shard_rows,
                    ca_pir_row_builder
                );
            }
            const auto ca_pir_db_end = Clock::now();
            cout << "[CA_only phase] "
                 << (ca_direct_encrypted_rows
                         ? "Direct encrypted Cuckoo access ready in "
                         : "Encrypted Cuckoo PIR reader ready in ")
                 << elapsed_ms(ca_pir_db_start, ca_pir_db_end) / 1000.0
                 << "s row_bytes=" << ca_pir_row_bytes
                 << " max_serialized_chunk_bytes=" << max_serialized_chunk_bytes
                 << " logical_db_mb="
                 << (static_cast<double>(ca_pir_row_bytes) *
                     static_cast<double>(encrypted_chunk_total) / 1'000'000.0)
                 << "\n";

            std::unordered_map<size_t, lbcrypto::Ciphertext<lbcrypto::DCRTPoly>> ca_pir_chunk_cache;
            std::deque<size_t> ca_pir_chunk_cache_fifo;
            size_t ca_pir_chunk_reads = 0;
            size_t ca_pir_cache_hits = 0;
            auto ca_read_pir_row = [&](size_t row_idx) -> std::vector<uint8_t> {
                if (ca_direct_encrypted_rows) {
                    return ca_pir_rows.at(row_idx);
                }
                if (USE_PIR_BATCHPIR) {
                    return ca_batchpir_reader->read_row(row_idx);
                }
                return ca_pir_double_reader->read_row(row_idx);
            };
            auto ca_get_encrypted_chunk = [&](size_t chunk)
                -> lbcrypto::Ciphertext<lbcrypto::DCRTPoly> {
                if (CA_PIR_CHUNK_CACHE_LIMIT > 0) {
                    auto cached = ca_pir_chunk_cache.find(chunk);
                    if (cached != ca_pir_chunk_cache.end()) {
                        ++ca_pir_cache_hits;
                        return cached->second;
                    }
                }
                auto loaded = ca_unpack_ciphertext_pir_row(ca_read_pir_row(chunk));
                ++ca_pir_chunk_reads;
                if (CA_PIR_CHUNK_CACHE_LIMIT > 0) {
                    while (ca_pir_chunk_cache.size() >= CA_PIR_CHUNK_CACHE_LIMIT &&
                           !ca_pir_chunk_cache_fifo.empty()) {
                        ca_pir_chunk_cache.erase(ca_pir_chunk_cache_fifo.front());
                        ca_pir_chunk_cache_fifo.pop_front();
                    }
                    ca_pir_chunk_cache_fifo.push_back(chunk);
                    ca_pir_chunk_cache.emplace(chunk, loaded);
                }
                return loaded;
            };

            cout << "[CA_only phase] Planning Cuckoo packed bit routes for Client queries...\n";
            const auto route_plan_start = Clock::now();
            std::vector<unsigned char> needed_cuckoo_bits(cuckoo_tag_bit_count, 0);
            size_t queried_cuckoo_bit_accesses = 0;
            size_t unique_queried_cuckoo_bits = 0;
            for (const auto& x : client_imgs) {
                const auto keys_by_table = ca_cuckoo_keys_for_item(lsh, x, CA_COMPOUND_LSH, CA_USE_OPRF);
                for (size_t l = 0; l < keys_by_table.size(); ++l) {
                    for (const auto& encoded : keys_by_table[l]) {
                        const auto lookup = cuckoo.lookup_encoded(encoded.bucket, encoded.tag);
                        const std::array<size_t, 2> buckets{lookup.i1, lookup.i2};
                        for (size_t bucket : buckets) {
                            for (size_t tag_idx : ca_cuckoo_tag_indices(lookup.fp)) {
                                const size_t bit_idx = ca_cuckoo_bucket_tag_bit_index(bucket, tag_idx);
                                ++queried_cuckoo_bit_accesses;
                                if (!needed_cuckoo_bits[bit_idx]) {
                                    needed_cuckoo_bits[bit_idx] = 1;
                                    ++unique_queried_cuckoo_bits;
                                }
                            }
                        }
                    }
                }
            }
            const auto route_plan_end = Clock::now();
            cout << "[CA_only phase] Cuckoo packed bit routing plan"
                 << " total_accesses=" << queried_cuckoo_bit_accesses
                 << " unique_bits=" << unique_queried_cuckoo_bits
                 << " reuse_saved=" << (queried_cuckoo_bit_accesses - unique_queried_cuckoo_bits)
                 << " planning_runtime_s=" << elapsed_ms(route_plan_start, route_plan_end) / 1000.0
                 << "\n";

            cout << "[CA_only phase] Client homomorphic Cuckoo cardinality start"
                 << " client=" << client_imgs.size()
                 << " encrypted_membership_checks=" << (client_imgs.size() * membership_terms_per_item)
                 << " encrypted_tag_bit_reads="
                 << (client_imgs.size() * membership_terms_per_item * 2 * CA_CUCKOO_TAG_HASHES)
                 << " client_batch_slots=" << fhe.batch_slots
                 << "\n";
            const auto client_eval_start = Clock::now();
            auto encrypted_zero = ca_encrypt_scalar(fhe, 0);
            auto encrypted_sum = encrypted_zero;

            const size_t client_batch_size = fhe.batch_slots;
            const size_t client_batch_count =
                (client_imgs.size() + client_batch_size - 1) / client_batch_size;
            const size_t client_stride = std::max<size_t>(1, client_batch_count / 10);
            for (size_t batch = 0; batch < client_batch_count; ++batch) {
                const size_t batch_start = batch * client_batch_size;
                const size_t batch_count = std::min(client_batch_size, client_imgs.size() - batch_start);

                std::vector<std::vector<std::vector<CaCuckooEncoded>>> batch_keys;
                batch_keys.reserve(batch_count);
                for (size_t offset = 0; offset < batch_count; ++offset) {
                    batch_keys.push_back(ca_cuckoo_keys_for_item(
                        lsh,
                        client_imgs[batch_start + offset],
                        CA_COMPOUND_LSH,
                        CA_USE_OPRF
                    ));
                }

                std::vector<lbcrypto::Ciphertext<lbcrypto::DCRTPoly>> member_terms;
                member_terms.reserve(membership_terms_per_item);
                for (size_t l = 0; l < l_count; ++l) {
                    for (size_t kk = 0; kk < batch_keys[0][l].size(); ++kk) {
                        std::vector<lbcrypto::Ciphertext<lbcrypto::DCRTPoly>> bucket_terms;
                        bucket_terms.reserve(2);
                        for (size_t candidate = 0; candidate < 2; ++candidate) {
                            std::vector<lbcrypto::Ciphertext<lbcrypto::DCRTPoly>> tag_bit_batches(
                                CA_CUCKOO_TAG_HASHES,
                                encrypted_zero
                            );
                            for (size_t offset = 0; offset < batch_count; ++offset) {
                                const auto lookup = cuckoo.lookup_encoded(
                                    batch_keys[offset][l][kk].bucket,
                                    batch_keys[offset][l][kk].tag
                                );
                                const size_t bucket = candidate == 0 ? lookup.i1 : lookup.i2;
                                const auto tag_indices = ca_cuckoo_tag_indices(lookup.fp);
                                for (size_t tag_hash_idx = 0; tag_hash_idx < tag_indices.size(); ++tag_hash_idx) {
                                    const size_t bit_idx =
                                        ca_cuckoo_bucket_tag_bit_index(bucket, tag_indices[tag_hash_idx]);
                                    const size_t chunk = bit_idx / fhe.batch_slots;
                                    const size_t slot = bit_idx % fhe.batch_slots;
                                    tag_bit_batches[tag_hash_idx] = fhe.cc->EvalAdd(
                                        tag_bit_batches[tag_hash_idx],
                                        ca_route_packed_bit(
                                            fhe,
                                            ca_get_encrypted_chunk(chunk),
                                            slot,
                                            offset
                                        )
                                    );
                                }
                            }
                            bucket_terms.push_back(ca_and_tree(fhe, std::move(tag_bit_batches)));
                        }
                        member_terms.push_back(ca_or_tree(fhe, std::move(bucket_terms), encrypted_zero));
                    }
                }

                auto in_batch = ca_or_tree(fhe, std::move(member_terms), encrypted_zero);
                auto batch_sum = ca_sum_slots_to_scalar(fhe, in_batch, batch_count);
                encrypted_sum = fhe.cc->EvalAdd(encrypted_sum, batch_sum);

                const size_t done_batches = batch + 1;
                if (ca_should_print_progress(done_batches, client_batch_count, client_stride)) {
                    const size_t done_items = std::min(client_imgs.size(), batch_start + batch_count);
                    ca_progress("client homomorphic Cuckoo Client batches", done_batches, client_batch_count, client_eval_start);
                    cout << "[CA_only progress] client homomorphic Client items "
                         << done_items << "/" << client_imgs.size() << "\n";
                }
            }
            const auto client_eval_end = Clock::now();
            cout << "[CA_only phase] Client homomorphic Cuckoo cardinality done in "
                 << elapsed_ms(client_eval_start, client_eval_end) / 1000.0 << "s\n";
            cout << "[CA_only phase] Encrypted Cuckoo "
                 << (ca_direct_encrypted_rows ? "direct" : "PIR")
                 << " chunk reads"
                 << " reads=" << ca_pir_chunk_reads
                 << " cache_hits=" << ca_pir_cache_hits
                 << " cache_size=" << ca_pir_chunk_cache.size()
                 << "/" << CA_PIR_CHUNK_CACHE_LIMIT
                 << " total_chunks=" << encrypted_chunk_total
                 << "\n";

            cout << "[CA_only phase] Joint decryption start\n";
            const auto decrypt_start = Clock::now();
            const int64_t cardinality = ca_threshold_decrypt_scalar(fhe, encrypted_sum);
            const auto decrypt_end = Clock::now();
            const auto total_end = Clock::now();
            cout << "[CA_only phase] Joint decryption done in "
                 << elapsed_ms(decrypt_start, decrypt_end) / 1000.0 << "s\n";
            if (cardinality == plaintext_cardinality_check) {
                cout << "[CA_only check] decrypted cardinality matches plaintext Cuckoo count: "
                     << cardinality << "\n";
            } else {
                cout << "[CA_only check] WARNING decrypted cardinality=" << cardinality
                     << " plaintext Cuckoo count=" << plaintext_cardinality_check
                     << "\n";
            }
            const DiscoGcaesOprfStats ca_oprf_stats_for_run =
                disco_gcaes_oprf_stats_delta(ca_oprf_stats_before_run, disco_gcaes_oprf_stats());
            const std::uint64_t ca_oprf_communication_bytes_for_run =
                ca_oprf_stats_for_run.bytes_sent + ca_oprf_stats_for_run.bytes_recv;
            if (CA_USE_OPRF) {
                cout << "[CA_only phase] " << disco_oprf_mechanism()
                     << " OPRF measured"
                     << " calls=" << ca_oprf_stats_for_run.calls
                     << " blocks=" << ca_oprf_stats_for_run.blocks
                     << " runtime_s=" << (ca_oprf_stats_for_run.runtime_ms / 1000.0)
                     << " comm_mb="
                     << (static_cast<double>(ca_oprf_communication_bytes_for_run) / 1'000'000.0)
                     << "\n";
            }

            cardinality_sum += static_cast<long double>(cardinality);
            keygen_runtime_ms_sum += elapsed_ms(keygen_start, keygen_end);
            server_cuckoo_runtime_ms_sum += elapsed_ms(server_cuckoo_start, server_cuckoo_end);
            server_encrypt_runtime_ms_sum += elapsed_ms(server_encrypt_start, server_encrypt_end);
            ca_pir_db_runtime_ms_sum += elapsed_ms(ca_pir_db_start, ca_pir_db_end);
            if (ca_direct_encrypted_rows) {
                // Direct encrypted-row access intentionally has no PIR communication/runtime.
            } else if (USE_PIR_BATCHPIR) {
                ca_pir_client_runtime_ms_sum += ca_batchpir_reader->client_runtime_ms();
                ca_pir_server_runtime_ms_sum += ca_batchpir_reader->server_runtime_ms();
                ca_pir_setup_communication_bytes_sum +=
                    static_cast<long double>(ca_batchpir_reader->setup_communication_bytes());
                ca_pir_query_communication_bytes_sum +=
                    static_cast<long double>(ca_batchpir_reader->query_communication_bytes());
                ca_pir_response_communication_bytes_sum +=
                    static_cast<long double>(ca_batchpir_reader->response_communication_bytes());
            } else {
                ca_pir_client_runtime_ms_sum += ca_pir_double_reader->client_runtime_ms();
                ca_pir_server_runtime_ms_sum += ca_pir_double_reader->server_runtime_ms();
                ca_pir_setup_communication_bytes_sum +=
                    static_cast<long double>(ca_pir_double_reader->setup_communication_bytes());
                ca_pir_query_communication_bytes_sum +=
                    static_cast<long double>(ca_pir_double_reader->query_communication_bytes());
                ca_pir_response_communication_bytes_sum +=
                    static_cast<long double>(ca_pir_double_reader->response_communication_bytes());
            }
            packed_route_planning_runtime_ms_sum += elapsed_ms(route_plan_start, route_plan_end);
            client_eval_runtime_ms_sum += elapsed_ms(client_eval_start, client_eval_end);
            decrypt_runtime_ms_sum += elapsed_ms(decrypt_start, decrypt_end);
            total_runtime_ms_sum += elapsed_ms(total_start, total_end);
            ca_oprf_runtime_ms_sum += ca_oprf_stats_for_run.runtime_ms;
            ca_oprf_communication_bytes_sum +=
                static_cast<long double>(ca_oprf_communication_bytes_for_run);
            ca_oprf_client_to_server_bytes_sum +=
                static_cast<long double>(ca_oprf_stats_for_run.bytes_sent);
            ca_oprf_server_to_client_bytes_sum +=
                static_cast<long double>(ca_oprf_stats_for_run.bytes_recv);
            ca_oprf_calls_sum += static_cast<long double>(ca_oprf_stats_for_run.calls);
            ca_oprf_blocks_sum += static_cast<long double>(ca_oprf_stats_for_run.blocks);

            if (ca_use_disk_pir_rows) {
                ca_pir_double_reader.reset();
                ca_batchpir_reader.reset();
                ca_pir_row_builder = nullptr;
                ca_pir_disk_in.reset();
                if (std::remove(ca_pir_db_path.c_str()) != 0) {
                    cout << "[CA_only warning] Could not remove temporary PIR DB "
                         << ca_pir_db_path << "\n";
                }
            }

            cout << "[CA_only run] client_size=" << client_size
                 << " cardinality=" << cardinality
                 << " total_runtime_s=" << elapsed_ms(total_start, total_end) / 1000.0
                 << "\n";
        }

        cout << "\n[CA_ONLY AVG] mode=" << mode
             << " filter=Cuckoo"
             << " server_size=" << N
             << " client_size=" << client_size
             << "\n"
             << "  Fuzzy Cardinality: "
             << static_cast<double>(cardinality_sum / NUM_RUNS_PSI)
             << "\n"
             << "  Total Runtime: "
             << (total_runtime_ms_sum / NUM_RUNS_PSI) / 1000.0 << " s"
             << "\n"
             << "  Total Communication Cost"
             << (CA_USE_OPRF
                     ? (std::string(" (PIR + ") + disco_oprf_mechanism() + " OPRF)")
                     : "")
             << ": "
             << static_cast<double>((
                    ca_pir_setup_communication_bytes_sum +
                    ca_pir_query_communication_bytes_sum +
                    ca_pir_response_communication_bytes_sum +
                    ca_oprf_communication_bytes_sum
                ) / NUM_RUNS_PSI / 1'000'000.0L)
             << " MB"
             << "\n"
             << "  THE KeyGen Runtime: "
             << (keygen_runtime_ms_sum / NUM_RUNS_PSI) / 1000.0 << " s"
             << "\n"
             << "  Server Cuckoo Build Runtime: "
             << (server_cuckoo_runtime_ms_sum / NUM_RUNS_PSI) / 1000.0 << " s"
             << "\n"
             << "  Server Cuckoo Encryption Runtime: "
             << (server_encrypt_runtime_ms_sum / NUM_RUNS_PSI) / 1000.0 << " s"
             << "\n"
             << "  Encrypted Cuckoo PIR DB Runtime: "
             << (ca_pir_db_runtime_ms_sum / NUM_RUNS_PSI) / 1000.0 << " s"
             << "\n"
             << "  Encrypted Cuckoo PIR Client Runtime: "
             << (ca_pir_client_runtime_ms_sum / NUM_RUNS_PSI) / 1000.0 << " s"
             << "\n"
             << "  Encrypted Cuckoo PIR Server Runtime: "
             << (ca_pir_server_runtime_ms_sum / NUM_RUNS_PSI) / 1000.0 << " s"
             << "\n"
             << "  " << disco_oprf_mechanism() << " OPRF Runtime (included in total): "
             << (ca_oprf_runtime_ms_sum / NUM_RUNS_PSI) / 1000.0 << " s"
             << "\n"
             << "  " << disco_oprf_mechanism() << " OPRF Communication Cost (included in total): "
             << static_cast<double>((ca_oprf_communication_bytes_sum / NUM_RUNS_PSI) / 1'000'000.0L)
             << " MB"
             << "\n"
             << "  " << disco_oprf_mechanism() << " OPRF Client -> Server Communication: "
             << static_cast<double>((ca_oprf_client_to_server_bytes_sum / NUM_RUNS_PSI) / 1'000'000.0L)
             << " MB"
             << "\n"
             << "  " << disco_oprf_mechanism() << " OPRF Server -> Client Communication: "
             << static_cast<double>((ca_oprf_server_to_client_bytes_sum / NUM_RUNS_PSI) / 1'000'000.0L)
             << " MB"
             << "\n"
             << "  " << disco_oprf_mechanism() << " OPRF Calls: "
             << static_cast<double>(ca_oprf_calls_sum / NUM_RUNS_PSI)
             << "\n"
             << "  " << disco_oprf_mechanism() << " OPRF Blocks: "
             << static_cast<double>(ca_oprf_blocks_sum / NUM_RUNS_PSI)
             << "\n"
             << "  Encrypted Cuckoo PIR Setup Communication: "
             << static_cast<double>((ca_pir_setup_communication_bytes_sum / NUM_RUNS_PSI) / 1'000'000.0L)
             << " MB"
             << "\n"
             << "  Encrypted Cuckoo PIR Query Communication: "
             << static_cast<double>((ca_pir_query_communication_bytes_sum / NUM_RUNS_PSI) / 1'000'000.0L)
             << " MB"
             << "\n"
             << "  Encrypted Cuckoo PIR Response Communication: "
             << static_cast<double>((ca_pir_response_communication_bytes_sum / NUM_RUNS_PSI) / 1'000'000.0L)
             << " MB"
             << "\n"
             << "  Packed Query Planning Runtime: "
             << (packed_route_planning_runtime_ms_sum / NUM_RUNS_PSI) / 1000.0 << " s"
             << "\n"
             << "  Client Homomorphic Cardinality Runtime: "
             << (client_eval_runtime_ms_sum / NUM_RUNS_PSI) / 1000.0 << " s"
             << "\n"
             << "  Joint Decryption Runtime: "
             << (decrypt_runtime_ms_sum / NUM_RUNS_PSI) / 1000.0 << " s"
             << "\n";
    }

    return 0;
}

#else

static int fuzzypsi_cardinality_only(
    int,
    int,
    double,
    const string&
) {
    cerr << "--CA_only requires OpenFHE support. Build OpenFHE under openfhe/build "
         << "and rebuild this target with USE_OPENFHE=1.\n";
    return -1;
}

static int fuzzypsi_cardinality_only_cuckoo(
    int,
    int,
    double,
    const string&
) {
    cerr << "--CA_only requires OpenFHE support. Build OpenFHE under openfhe/build "
         << "and rebuild this target with USE_OPENFHE=1.\n";
    return -1;
}

#endif
