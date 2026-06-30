#include "batchpir_row_reader.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

#if defined(FUZZY_PETS_ENABLE_BATCHPIR) && __has_include("seal/seal.h")
#define FUZZY_PETS_BATCHPIR_COMPILED 1

#include <bitset>
#include <cmath>
#include <ctime>
#include <iostream>
#include <limits>
#include <map>
#include <string>
#include <utility>

#include "vectorized_batchpir/header/batchpirparams.h"
#include "vectorized_batchpir/header/batchpirserver.h"
#include "vectorized_batchpir/header/batchpirclient.h"

#include "vectorized_batchpir/src/pirparams.cpp"
#include "vectorized_batchpir/src/client.cpp"
#include "vectorized_batchpir/src/server.cpp"
#include "vectorized_batchpir/src/batchpirparams.cpp"
#include "vectorized_batchpir/src/batchpirserver.cpp"
#include "vectorized_batchpir/src/batchpirclient.cpp"
#else
#define FUZZY_PETS_BATCHPIR_COMPILED 0
#endif

namespace fuzzy_pets_batchpir {

bool available() {
    return FUZZY_PETS_BATCHPIR_COMPILED != 0;
}

std::string unavailable_reason() {
#if FUZZY_PETS_BATCHPIR_COMPILED
    return {};
#elif defined(FUZZY_PETS_ENABLE_BATCHPIR)
    return "Microsoft SEAL headers were not found while compiling BatchPIR support";
#else
    return "BatchPIR support was not enabled at build time; rebuild with USE_BATCHPIR=1 and SEAL installed";
#endif
}

#if FUZZY_PETS_BATCHPIR_COMPILED
namespace {

static constexpr uint32_t CLIENT_ID = 0;
using Clock = std::chrono::high_resolution_clock;

class NullStreamBuffer : public std::streambuf {
public:
    int overflow(int c) override {
        return traits_type::not_eof(c);
    }
};

class ScopedCoutSilencer {
public:
    ScopedCoutSilencer()
        : previous_(std::cout.rdbuf(&null_buffer_)) {}

    ~ScopedCoutSilencer() {
        std::cout.rdbuf(previous_);
    }

    ScopedCoutSilencer(const ScopedCoutSilencer&) = delete;
    ScopedCoutSilencer& operator=(const ScopedCoutSilencer&) = delete;

private:
    NullStreamBuffer null_buffer_;
    std::streambuf* previous_;
};

double elapsed_ms(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

RawDB build_raw_db(size_t total_rows, size_t row_bytes, const RowBuilder& row_builder) {
    if (total_rows == 0) {
        throw std::invalid_argument("BatchPIR RowReader: total_rows must be > 0");
    }
    if (row_bytes == 0) {
        throw std::invalid_argument("BatchPIR RowReader: row_bytes must be > 0");
    }

    RawDB rawdb;
    rawdb.reserve(total_rows);
    for (size_t i = 0; i < total_rows; ++i) {
        Row row = row_builder(i);
        if (row.size() != row_bytes) {
            throw std::runtime_error("BatchPIR RowReader: row_builder returned a row with unexpected length");
        }
        rawdb.emplace_back(row.begin(), row.end());
    }
    return rawdb;
}

std::vector<Row> flatten_decoded_rows(
    const std::vector<RawDB>& decoded_chunks,
    size_t expected_count
) {
    std::vector<Row> rows;
    rows.reserve(expected_count);
    for (const RawDB& chunk : decoded_chunks) {
        for (const auto& row : chunk) {
            if (rows.size() == expected_count) {
                return rows;
            }
            rows.emplace_back(row.begin(), row.end());
        }
    }
    if (rows.size() != expected_count) {
        throw std::runtime_error("BatchPIR RowReader: decoded response count mismatch");
    }
    return rows;
}

template <typename SealObject>
std::uint64_t seal_serialized_bytes(const SealObject& object) {
    return static_cast<std::uint64_t>(object.save_size());
}

std::uint64_t ciphertext_list_bytes(const PIRResponseList& ciphertexts) {
    std::uint64_t bytes = 0;
    for (const auto& ciphertext : ciphertexts) {
        bytes += seal_serialized_bytes(ciphertext);
    }
    return bytes;
}

std::uint64_t query_list_bytes(const std::vector<PIRQuery>& queries) {
    std::uint64_t bytes = 0;
    for (const auto& query : queries) {
        bytes += ciphertext_list_bytes(query);
    }
    return bytes;
}

std::uint64_t public_key_bytes(const std::pair<seal::GaloisKeys, seal::RelinKeys>& keys) {
    return seal_serialized_bytes(keys.first) + seal_serialized_bytes(keys.second);
}

std::uint64_t hash_map_wire_bytes(const std::unordered_map<std::string, uint64_t>& map) {
    std::uint64_t bytes = sizeof(std::uint64_t);
    for (const auto& entry : map) {
        bytes += sizeof(std::uint32_t);
        bytes += static_cast<std::uint64_t>(entry.first.size());
        bytes += sizeof(entry.second);
    }
    return bytes;
}

} // namespace

class BatchPirRowReader final : public RowReader {
public:
    BatchPirRowReader(
        size_t total_rows,
        size_t row_bytes,
        RowBuilder row_builder,
        size_t batch_size
    )
        : total_rows_(total_rows),
          row_bytes_(row_bytes),
          batch_size_(batch_size),
          row_builder_(std::move(row_builder)) {
        if (batch_size_ == 0) {
            throw std::invalid_argument("BatchPIR RowReader: batch_size must be > 0");
        }
        if (batch_size_ > static_cast<size_t>(std::numeric_limits<int>::max())) {
            throw std::invalid_argument("BatchPIR RowReader: batch_size exceeds BatchPIR int limit");
        }

        std::cout << "[PIR_BatchPIR] Building server for rows=" << total_rows_
                  << " row_bytes=" << row_bytes_
                  << " batch_size=" << batch_size_
                  << std::endl;

        RawDB rawdb = build_raw_db(total_rows_, row_bytes_, row_builder_);
        {
            ScopedCoutSilencer silence_batchpir_setup;
            auto encryption_params = utils::create_single_server_encryption_parameters(
                batch_size_,
                total_rows_,
                row_bytes_
            );
            params_ = std::make_unique<BatchPirParams>(
                static_cast<int>(batch_size_),
                total_rows_,
                row_bytes_,
                encryption_params
            );
            server_ = std::make_unique<BatchPIRServer>(*params_, std::move(rawdb));
            client_ = std::make_unique<BatchPIRClient>(*params_);
            auto server_map = server_->get_hash_map();
            setup_communication_bytes_ += hash_map_wire_bytes(server_map);
            client_->set_map(std::move(server_map));
            auto public_keys = client_->get_public_keys();
            setup_communication_bytes_ += public_key_bytes(public_keys);
            server_->set_client_keys(CLIENT_ID, std::move(public_keys));
        }
        const auto seal_params = params_->get_seal_parameters();
        std::cout << "[PIR_BatchPIR] SEAL params: PolyDegree="
                  << seal_params.poly_modulus_degree()
                  << " PlaintextModBitss="
                  << seal_params.plain_modulus().bit_count()
                  << " CoeffMods=[";
        for (size_t i = 0; i < seal_params.coeff_modulus().size(); ++i) {
            if (i > 0) {
                std::cout << ",";
            }
            std::cout << seal_params.coeff_modulus()[i].bit_count();
        }
        std::cout << "]" << std::endl;
    }

    Row read_row(size_t idx) override {
        return read_rows(std::vector<size_t>{idx}).front();
    }

    std::vector<Row> read_rows(const std::vector<size_t>& indices) override {
        std::vector<Row> rows(indices.size());
        std::vector<size_t> unique_indices;
        std::vector<std::vector<size_t>> output_positions;
        std::unordered_map<size_t, size_t> unique_pos_by_idx;

        for (size_t pos = 0; pos < indices.size(); ++pos) {
            const size_t idx = indices[pos];
            if (idx >= total_rows_) {
                throw std::out_of_range("BatchPIR RowReader: row index out of range");
            }

            auto found = unique_pos_by_idx.find(idx);
            if (found == unique_pos_by_idx.end()) {
                const size_t unique_pos = unique_indices.size();
                unique_pos_by_idx.emplace(idx, unique_pos);
                unique_indices.push_back(idx);
                output_positions.push_back({pos});
            } else {
                output_positions[found->second].push_back(pos);
            }
        }

        if (unique_indices.empty()) {
            return rows;
        }

        std::vector<Row> unique_rows(unique_indices.size());
        for (size_t start = 0; start < unique_indices.size(); start += batch_size_) {
            const size_t count = std::min(batch_size_, unique_indices.size() - start);
            const size_t batch_number = (start / batch_size_) + 1;
            const size_t batch_total =
                (unique_indices.size() + batch_size_ - 1) / batch_size_;
            std::cout << "[PIR_BatchPIR] Fetch batch "
                      << batch_number << "/" << batch_total
                      << " requested=" << count
                      << " padded=" << batch_size_
                      << std::endl;
            std::vector<uint64_t> batch;
            batch.reserve(batch_size_);

            std::unordered_set<uint64_t> used;
            used.reserve(batch_size_);
            for (size_t i = 0; i < count; ++i) {
                const uint64_t idx = static_cast<uint64_t>(unique_indices[start + i]);
                batch.push_back(idx);
                used.insert(idx);
            }

            uint64_t dummy = 0;
            while (batch.size() < batch_size_ && used.size() < total_rows_) {
                if (used.insert(dummy).second) {
                    batch.push_back(dummy);
                }
                dummy = (dummy + 1) % static_cast<uint64_t>(total_rows_);
            }
            while (batch.size() < batch_size_) {
                batch.push_back(0);
            }

            std::vector<Row> fetched = fetch_batch(batch, count);
            for (size_t i = 0; i < count; ++i) {
                unique_rows[start + i] = std::move(fetched[i]);
            }
            std::cout << "[PIR_BatchPIR] Fetch batch done "
                      << batch_number << "/" << batch_total
                      << " client_s=" << (client_runtime_ms_ / 1000.0)
                      << " server_s=" << (server_runtime_ms_ / 1000.0)
                      << std::endl;
        }

        for (size_t unique_pos = 0; unique_pos < unique_rows.size(); ++unique_pos) {
            for (size_t out_pos : output_positions[unique_pos]) {
                rows[out_pos] = unique_rows[unique_pos];
            }
        }
        return rows;
    }

    double client_runtime_ms() const override {
        return client_runtime_ms_;
    }

    double server_runtime_ms() const override {
        return server_runtime_ms_;
    }

    std::uint64_t communication_bytes() const override {
        return query_communication_bytes_ + response_communication_bytes_;
    }

    std::uint64_t setup_communication_bytes() const override {
        return setup_communication_bytes_;
    }

    std::uint64_t query_communication_bytes() const override {
        return query_communication_bytes_;
    }

    std::uint64_t response_communication_bytes() const override {
        return response_communication_bytes_;
    }

    size_t batchpir_num_buckets() const override {
        if (!params_) {
            return 0;
        }
        return static_cast<size_t>(
            std::ceil(params_->get_batch_size() * params_->get_cuckoo_factor())
        );
    }

    size_t batchpir_max_bucket_size() const override {
        return params_ ? params_->get_max_bucket_size() : 0;
    }

    size_t batchpir_first_dimension_size() const override {
        return params_ ? params_->get_first_dimension_size() : 0;
    }

    size_t batchpir_server_count() const override {
        const size_t dim_size = batchpir_first_dimension_size();
        const size_t num_buckets = batchpir_num_buckets();
        if (!params_ || dim_size == 0) {
            return 0;
        }
        const size_t per_server_capacity =
            params_->get_seal_parameters().poly_modulus_degree() / dim_size;
        if (per_server_capacity == 0) {
            return 0;
        }
        return static_cast<size_t>(
            std::ceil(num_buckets * 1.0 / per_server_capacity)
        );
    }

private:
    std::vector<Row> fetch_batch(const std::vector<uint64_t>& batch, size_t requested_count) {
        std::unordered_map<uint64_t, size_t> requested_pos;
        requested_pos.reserve(requested_count);
        for (size_t i = 0; i < requested_count; ++i) {
            requested_pos.emplace(batch[i], i);
        }

        auto client_start = Clock::now();
        std::vector<PIRQuery> queries;
        std::cout << "[PIR_BatchPIR] Create queries count="
                  << requested_count << std::endl;
        {
            ScopedCoutSilencer silence_batchpir_client;
            queries = client_->create_queries(batch);
        }
        const std::uint64_t seeded_query_wire_bytes = client_->last_query_wire_bytes();
        query_communication_bytes_ += seeded_query_wire_bytes != 0
            ? seeded_query_wire_bytes
            : query_list_bytes(queries);
        client_runtime_ms_ += elapsed_ms(client_start, Clock::now());

        auto server_start = Clock::now();
        PIRResponseList responses;
        std::cout << "[PIR_BatchPIR] Generate response queries="
                  << queries.size() << std::endl;
        {
            ScopedCoutSilencer silence_batchpir_server;
            responses = server_->generate_response(CLIENT_ID, queries);
        }
        response_communication_bytes_ += ciphertext_list_bytes(responses);
        server_runtime_ms_ += elapsed_ms(server_start, Clock::now());
        std::cout << "[PIR_BatchPIR] Generate response done seconds="
                  << (elapsed_ms(server_start, Clock::now()) / 1000.0)
                  << " responses=" << responses.size()
                  << std::endl;

        client_start = Clock::now();
        std::vector<RawDB> decoded_chunks;
        std::vector<uint64_t> request_table;
        std::cout << "[PIR_BatchPIR] Decode responses responses="
                  << responses.size() << std::endl;
        {
            ScopedCoutSilencer silence_batchpir_decode;
            decoded_chunks = client_->decode_responses_chunks(responses);
            request_table = client_->get_request_table();
        }
        std::cout << "[PIR_BatchPIR] Decode responses done chunks="
                  << decoded_chunks.size()
                  << " request_table=" << request_table.size()
                  << std::endl;
        std::vector<Row> decoded_rows = flatten_decoded_rows(decoded_chunks, request_table.size());

        std::vector<Row> fetched(requested_count);
        std::vector<bool> found(requested_count, false);
        const uint64_t default_value = params_->get_default_value();

        for (size_t bucket_idx = 0; bucket_idx < request_table.size(); ++bucket_idx) {
            const uint64_t original_idx = request_table[bucket_idx];
            if (original_idx == default_value) {
                continue;
            }
            auto wanted = requested_pos.find(original_idx);
            if (wanted == requested_pos.end()) {
                continue;
            }
            fetched[wanted->second] = std::move(decoded_rows[bucket_idx]);
            found[wanted->second] = true;
        }

        for (size_t i = 0; i < found.size(); ++i) {
            if (!found[i]) {
                fetched[i] = Row(row_bytes_, 0);
                continue;
            }
            if (fetched[i].size() != row_bytes_) {
                throw std::runtime_error("BatchPIR RowReader: decoded row has unexpected length");
            }
        }
        client_runtime_ms_ += elapsed_ms(client_start, Clock::now());

        return fetched;
    }

    size_t total_rows_;
    size_t row_bytes_;
    size_t batch_size_;
    RowBuilder row_builder_;
    std::unique_ptr<BatchPirParams> params_;
    std::unique_ptr<BatchPIRServer> server_;
    std::unique_ptr<BatchPIRClient> client_;
    double client_runtime_ms_ = 0.0;
    double server_runtime_ms_ = 0.0;
    std::uint64_t setup_communication_bytes_ = 0;
    std::uint64_t query_communication_bytes_ = 0;
    std::uint64_t response_communication_bytes_ = 0;
};
#endif

std::unique_ptr<RowReader> make_row_reader(
    size_t total_rows,
    size_t row_bytes,
    RowBuilder row_builder,
    size_t batch_size
) {
#if FUZZY_PETS_BATCHPIR_COMPILED
    return std::make_unique<BatchPirRowReader>(
        total_rows,
        row_bytes,
        std::move(row_builder),
        batch_size
    );
#else
    (void)total_rows;
    (void)row_bytes;
    (void)row_builder;
    (void)batch_size;
    throw std::runtime_error(unavailable_reason());
#endif
}

} // namespace fuzzy_pets_batchpir
