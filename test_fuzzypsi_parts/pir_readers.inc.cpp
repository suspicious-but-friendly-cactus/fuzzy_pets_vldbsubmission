// The Disco C++ port uses structs directly, so count the schema payload fields
// without FlatBuffers table/framing padding.
static std::uint64_t punc_hint_req_wire_bytes(const pir_punc::PuncHintReq& req) {
    (void)req;
    return WIRE_U16_BYTES;
}

static std::uint64_t punc_hint_resp_wire_bytes(const pir_punc::PuncHintResp& resp) {
    return WIRE_U32_BYTES + // n_rows
           WIRE_U32_BYTES + // row_len
           WIRE_U32_BYTES + // set_size
           WIRE_U32_BYTES + // rand_init
           wire_vector_bytes(resp.setGenKey.size()) +
           wire_vector_bytes(
               static_cast<size_t>(resp.hints.size()) *
               static_cast<size_t>(resp.rowLen)
           );
}

static std::uint64_t punc_query_req_wire_bytes(const pir_punc::PuncQueryReq& query) {
    return WIRE_U32_BYTES + // extra_element
           wire_vector_bytes(query.puncturedSet.keys.size()) +
           WIRE_U32_BYTES + // hole
           WIRE_U32_BYTES + // shift
           WIRE_U32_BYTES + // univ_size
           WIRE_U32_BYTES;  // set_size
}

static std::uint64_t punc_query_list_wire_bytes(
    const std::vector<pir_punc::PuncQueryReq>& queries
) {
    std::uint64_t bytes = 0;
    for (const auto& query : queries) {
        bytes += punc_query_req_wire_bytes(query);
    }
    return bytes;
}

static std::uint64_t punc_response_wire_bytes(const pir_punc::PuncQueryResp& response) {
    return wire_vector_bytes(response.answer.size()) +
           wire_vector_bytes(response.extraElem.size());
}

static std::uint64_t punc_response_list_wire_bytes(
    const std::vector<pir_punc::PuncQueryResp>& responses
) {
    std::uint64_t bytes = 0;
    for (const auto& response : responses) {
        bytes += punc_response_wire_bytes(response);
    }
    return bytes;
}

class PirDoubleRowReaderBase {
public:
    virtual ~PirDoubleRowReaderBase() = default;
    virtual pir_punc::Row read_row(size_t idx) = 0;
    virtual std::unique_ptr<PirDoubleRowReaderBase> clone() const = 0;
    virtual double client_runtime_ms() const { return 0.0; }
    virtual double server_runtime_ms() const { return 0.0; }
    virtual std::uint64_t communication_bytes() const { return 0; }
    virtual std::uint64_t setup_communication_bytes() const { return 0; }
    virtual std::uint64_t query_communication_bytes() const { return 0; }
    virtual std::uint64_t response_communication_bytes() const { return 0; }
    virtual std::vector<pir_punc::Row> read_rows(const std::vector<size_t>& indices) {
        std::vector<pir_punc::Row> rows;
        rows.reserve(indices.size());
        for (size_t idx : indices) {
            rows.push_back(read_row(idx));
        }
        return rows;
    }
};

class PirDoubleRowReader : public PirDoubleRowReaderBase {
public:
    explicit PirDoubleRowReader(const std::vector<pir_punc::Row>& rows)
        : rng_(12345),
          db_(pir_punc::static_db_from_rows(rows)) {
        rebuild_client();
    }

    explicit PirDoubleRowReader(const pir_punc::StaticDB& db)
        : rng_(12345),
          db_(db) {
        rebuild_client();
    }

    explicit PirDoubleRowReader(pir_punc::StaticDB&& db)
        : rng_(12345),
          db_(std::move(db)) {
        rebuild_client();
    }

    std::unique_ptr<PirDoubleRowReaderBase> clone() const override {
        return std::make_unique<PirDoubleRowReader>(db_);
    }

    pir_punc::Row read_row(size_t idx) override {
        if (idx > static_cast<size_t>(std::numeric_limits<int>::max())) {
            throw std::out_of_range("PirDoubleRowReader: row index exceeds Disco PIR int limit");
        }

        static constexpr int max_attempts = 8;
        for (int attempt = 0; attempt < max_attempts; ++attempt) {
            try {
                return read_row_once(idx);
            } catch (const std::runtime_error& e) {
                if (std::string(e.what()) != "query index not covered by hints" ||
                    attempt + 1 == max_attempts) {
                    throw;
                }
                rebuild_client();
            }
        }

        throw std::runtime_error("PirDoubleRowReader: unreachable retry state");
    }

    std::vector<pir_punc::Row> read_rows(const std::vector<size_t>& indices) override {
        for (size_t idx : indices) {
            if (idx > static_cast<size_t>(std::numeric_limits<int>::max())) {
                throw std::out_of_range("PirDoubleRowReader: row index exceeds Disco PIR int limit");
            }
        }

        static constexpr int max_attempts = 8;
        for (int attempt = 0; attempt < max_attempts; ++attempt) {
            try {
                return read_rows_once(indices);
            } catch (const std::runtime_error& e) {
                if (std::string(e.what()) != "query index not covered by hints" ||
                    attempt + 1 == max_attempts) {
                    throw;
                }
                rebuild_client();
            }
        }

        throw std::runtime_error("PirDoubleRowReader: unreachable batch retry state");
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

private:
    void rebuild_client() {
        pir_punc::PuncHintReq hint_req{};
        setup_communication_bytes_ += punc_hint_req_wire_bytes(hint_req);
        hintResp_ = pir_punc::process_hint_req(hint_req, db_, rng_);
        setup_communication_bytes_ += punc_hint_resp_wire_bytes(hintResp_);
        client_ = std::make_unique<pir_punc::PuncClient>(hintResp_, rng_, 1.0);
    }

    pir_punc::Row read_row_once(size_t idx) {
        auto client_start = Clock::now();
        auto [queries, ctx] = client_->query(static_cast<int>(idx));
        query_communication_bytes_ += punc_query_list_wire_bytes(queries);
        client_runtime_ms_ += elapsed_ms(client_start, Clock::now());

        std::vector<pir_punc::PuncQueryResp> responses;
        responses.reserve(queries.size());
        auto server_start = Clock::now();
        for (const auto& query : queries) {
            responses.push_back(pir_punc::process_query(query, db_));
        }
        response_communication_bytes_ += punc_response_list_wire_bytes(responses);
        server_runtime_ms_ += elapsed_ms(server_start, Clock::now());

        client_start = Clock::now();
        pir_punc::Row row = client_->reconstruct(ctx, responses);
        client_runtime_ms_ += elapsed_ms(client_start, Clock::now());
        return row;
    }

    std::vector<pir_punc::Row> read_rows_once(const std::vector<size_t>& indices) {
        std::vector<std::vector<pir_punc::PuncQueryReq>> query_batches;
        std::vector<pir_punc::PuncQueryCtx> contexts;
        query_batches.reserve(indices.size());
        contexts.reserve(indices.size());

        auto client_start = Clock::now();
        for (size_t idx : indices) {
            auto [queries, ctx] = client_->query(static_cast<int>(idx));
            query_communication_bytes_ += punc_query_list_wire_bytes(queries);
            query_batches.push_back(std::move(queries));
            contexts.push_back(ctx);
        }
        client_runtime_ms_ += elapsed_ms(client_start, Clock::now());

        std::vector<std::vector<pir_punc::PuncQueryResp>> response_batches(query_batches.size());
        auto server_start = Clock::now();
        for (size_t batch_idx = 0; batch_idx < query_batches.size(); ++batch_idx) {
            auto& responses = response_batches[batch_idx];
            responses.reserve(query_batches[batch_idx].size());
            for (const auto& query : query_batches[batch_idx]) {
                responses.push_back(pir_punc::process_query(query, db_));
            }
            response_communication_bytes_ += punc_response_list_wire_bytes(responses);
        }
        server_runtime_ms_ += elapsed_ms(server_start, Clock::now());

        std::vector<pir_punc::Row> rows;
        rows.reserve(indices.size());
        client_start = Clock::now();
        for (size_t batch_idx = 0; batch_idx < contexts.size(); ++batch_idx) {
            rows.push_back(client_->reconstruct(contexts[batch_idx], response_batches[batch_idx]));
        }
        client_runtime_ms_ += elapsed_ms(client_start, Clock::now());
        return rows;
    }

    std::mt19937_64 rng_;
    pir_punc::StaticDB db_;
    pir_punc::PuncHintResp hintResp_;
    std::unique_ptr<pir_punc::PuncClient> client_;
    double client_runtime_ms_ = 0.0;
    double server_runtime_ms_ = 0.0;
    std::uint64_t setup_communication_bytes_ = 0;
    std::uint64_t query_communication_bytes_ = 0;
    std::uint64_t response_communication_bytes_ = 0;
};

class ShardedPirDoubleRowReader : public PirDoubleRowReaderBase {
public:
    using RowBuilder = std::function<pir_punc::Row(size_t)>;

    ShardedPirDoubleRowReader(size_t total_rows, size_t shard_rows, RowBuilder row_builder)
        : total_rows_(total_rows),
          shard_rows_(shard_rows),
          row_builder_(std::move(row_builder)) {
        if (total_rows_ == 0) {
            throw std::invalid_argument("ShardedPirDoubleRowReader: total_rows must be > 0");
        }
        if (shard_rows_ == 0 || shard_rows_ > static_cast<size_t>(std::numeric_limits<int>::max())) {
            throw std::invalid_argument("ShardedPirDoubleRowReader: shard_rows must fit in int");
        }

        if (PIR_DOUBLE_PREBUILD_SHARDS) {
            const size_t shard_count = (total_rows_ + shard_rows_ - 1) / shard_rows_;
            prebuild_shards_parallel(shard_count);
        }
    }

    pir_punc::Row read_row(size_t idx) override {
        if (idx >= total_rows_) {
            throw std::out_of_range("ShardedPirDoubleRowReader: row index out of range");
        }

        const size_t shard_idx = idx / shard_rows_;
        const size_t local_idx = idx % shard_rows_;
        auto server_start = Clock::now();
        PirDoubleRowReader& reader = reader_for_shard(shard_idx);
        server_runtime_ms_ += elapsed_ms(server_start, Clock::now());
        const double client_before = reader.client_runtime_ms();
        const double server_before = reader.server_runtime_ms();
        const std::uint64_t setup_before = reader.setup_communication_bytes();
        const std::uint64_t query_before = reader.query_communication_bytes();
        const std::uint64_t response_before = reader.response_communication_bytes();
        pir_punc::Row row = reader.read_row(local_idx);
        client_runtime_ms_ += reader.client_runtime_ms() - client_before;
        server_runtime_ms_ += reader.server_runtime_ms() - server_before;
        setup_communication_bytes_ += reader.setup_communication_bytes() - setup_before;
        query_communication_bytes_ += reader.query_communication_bytes() - query_before;
        response_communication_bytes_ +=
            reader.response_communication_bytes() - response_before;
        return row;
    }

    std::unique_ptr<PirDoubleRowReaderBase> clone() const override {
        return std::make_unique<ShardedPirDoubleRowReader>(
            total_rows_,
            shard_rows_,
            row_builder_
        );
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

    std::vector<pir_punc::Row> read_rows(const std::vector<size_t>& indices) override {
        std::vector<pir_punc::Row> rows(indices.size());
        std::unordered_map<size_t, std::vector<std::pair<size_t, size_t>>> by_shard;
        std::vector<size_t> shard_order;

        for (size_t pos = 0; pos < indices.size(); ++pos) {
            const size_t idx = indices[pos];
            if (idx >= total_rows_) {
                throw std::out_of_range("ShardedPirDoubleRowReader: row index out of range");
            }

            const size_t shard_idx = idx / shard_rows_;
            const size_t local_idx = idx % shard_rows_;
            if (by_shard.find(shard_idx) == by_shard.end()) {
                shard_order.push_back(shard_idx);
            }
            by_shard[shard_idx].push_back({pos, local_idx});
        }

        for (size_t shard_idx : shard_order) {
            const auto& requests = by_shard[shard_idx];
            std::vector<size_t> local_indices;
            local_indices.reserve(requests.size());
            for (const auto& request : requests) {
                local_indices.push_back(request.second);
            }

            auto server_start = Clock::now();
            PirDoubleRowReader& reader = reader_for_shard(shard_idx);
            server_runtime_ms_ += elapsed_ms(server_start, Clock::now());
            const double client_before = reader.client_runtime_ms();
            const double server_before = reader.server_runtime_ms();
            const std::uint64_t setup_before = reader.setup_communication_bytes();
            const std::uint64_t query_before = reader.query_communication_bytes();
            const std::uint64_t response_before = reader.response_communication_bytes();
            std::vector<pir_punc::Row> shard_rows_vec = reader.read_rows(local_indices);
            client_runtime_ms_ += reader.client_runtime_ms() - client_before;
            server_runtime_ms_ += reader.server_runtime_ms() - server_before;
            setup_communication_bytes_ += reader.setup_communication_bytes() - setup_before;
            query_communication_bytes_ += reader.query_communication_bytes() - query_before;
            response_communication_bytes_ +=
                reader.response_communication_bytes() - response_before;
            if (shard_rows_vec.size() != requests.size()) {
                throw std::runtime_error("ShardedPirDoubleRowReader: batch response size mismatch");
            }

            for (size_t i = 0; i < requests.size(); ++i) {
                rows[requests[i].first] = std::move(shard_rows_vec[i]);
            }
        }

        return rows;
    }

private:
    std::unique_ptr<PirDoubleRowReader> build_reader_for_shard(
        size_t shard_idx,
        const RowBuilder& row_builder
    ) const {
        const size_t start = shard_idx * shard_rows_;
        const size_t count = std::min(shard_rows_, total_rows_ - start);
        if (count > static_cast<size_t>(std::numeric_limits<int>::max())) {
            throw std::out_of_range("ShardedPirDoubleRowReader: shard size exceeds Disco PIR int limit");
        }

        pir_punc::StaticDB db;
        db.numRows = static_cast<int>(count);
        pir_punc::Row first_row = row_builder(start);
        db.rowLen = static_cast<int>(first_row.size());
        db.flatDb.resize(count * first_row.size());
        std::memcpy(db.flatDb.data(), first_row.data(), first_row.size());
        for (size_t i = 1; i < count; ++i) {
            pir_punc::Row row = row_builder(start + i);
            if (row.size() != first_row.size()) {
                throw std::runtime_error("ShardedPirDoubleRowReader: DB rows have unequal length");
            }
            std::memcpy(
                db.flatDb.data() + i * first_row.size(),
                row.data(),
                row.size()
            );
        }

        return std::make_unique<PirDoubleRowReader>(std::move(db));
    }

    void prebuild_shards_parallel(size_t shard_count) {
        std::vector<std::unique_ptr<PirDoubleRowReader>> built(shard_count);
        const size_t workers = std::min(
            parallel_worker_count(shard_count),
            PIR_SHARD_PREBUILD_MAX_WORKERS
        );
        if (workers <= 1) {
            for (size_t shard_idx = 0; shard_idx < shard_count; ++shard_idx) {
                built[shard_idx] = build_reader_for_shard(shard_idx, row_builder_);
            }
        } else {
            std::atomic<size_t> next{0};
            std::atomic<bool> stop{false};
            std::mutex exception_mutex;
            std::exception_ptr first_exception = nullptr;
            std::vector<std::thread> threads;
            threads.reserve(workers);

            for (size_t worker = 0; worker < workers; ++worker) {
                RowBuilder row_builder = row_builder_;
                threads.emplace_back([&, row_builder]() {
                    try {
                        while (!stop.load(std::memory_order_relaxed)) {
                            const size_t shard_idx = next.fetch_add(1, std::memory_order_relaxed);
                            if (shard_idx >= shard_count) {
                                break;
                            }
                            built[shard_idx] = build_reader_for_shard(shard_idx, row_builder);
                        }
                    } catch (...) {
                        {
                            std::lock_guard<std::mutex> lock(exception_mutex);
                            if (!first_exception) {
                                first_exception = std::current_exception();
                            }
                        }
                        stop.store(true, std::memory_order_relaxed);
                    }
                });
            }

            for (auto& thread : threads) {
                thread.join();
            }
            if (first_exception) {
                std::rethrow_exception(first_exception);
            }
        }

        for (size_t shard_idx = 0; shard_idx < shard_count; ++shard_idx) {
            lru_.push_back(shard_idx);
            setup_communication_bytes_ += built[shard_idx]->setup_communication_bytes();
            readers_.emplace(shard_idx, std::move(built[shard_idx]));
        }
    }

    PirDoubleRowReader& reader_for_shard(size_t shard_idx) {
        auto cached = readers_.find(shard_idx);
        if (cached != readers_.end()) {
            touch(shard_idx);
            return *cached->second;
        }

        if (!PIR_DOUBLE_PREBUILD_SHARDS) {
            evict_if_needed();
        }
        lru_.push_back(shard_idx);
        std::unique_ptr<PirDoubleRowReader> reader =
            build_reader_for_shard(shard_idx, row_builder_);
        setup_communication_bytes_ += reader->setup_communication_bytes();
        auto inserted = readers_.emplace(
            shard_idx,
            std::move(reader)
        );
        return *inserted.first->second;
    }

    void touch(size_t shard_idx) {
        auto it = std::find(lru_.begin(), lru_.end(), shard_idx);
        if (it != lru_.end()) {
            lru_.erase(it);
            lru_.push_back(shard_idx);
        }
    }

    void evict_if_needed() {
        while (readers_.size() >= PIR_DOUBLE_MAX_CACHED_SHARDS && !lru_.empty()) {
            readers_.erase(lru_.front());
            lru_.pop_front();
        }
    }

    size_t total_rows_;
    size_t shard_rows_;
    RowBuilder row_builder_;
    std::unordered_map<size_t, std::unique_ptr<PirDoubleRowReader>> readers_;
    std::deque<size_t> lru_;
    double client_runtime_ms_ = 0.0;
    double server_runtime_ms_ = 0.0;
    std::uint64_t setup_communication_bytes_ = 0;
    std::uint64_t query_communication_bytes_ = 0;
    std::uint64_t response_communication_bytes_ = 0;
};

static std::unique_ptr<PirDoubleRowReaderBase> build_bloom_pir_double_reader(const BloomFilter& bloom) {
    return std::make_unique<ShardedPirDoubleRowReader>(
        bloom.bucket_count(),
        PIR_DOUBLE_SHARD_ROWS,
        [&bloom](size_t i) { return pir_punc::Row{bloom.bit_at(i)}; }
    );
}

static std::unique_ptr<PirDoubleRowReaderBase> build_cuckoo_pir_double_reader(const CuckooFilter& cuckoo) {
    return std::make_unique<ShardedPirDoubleRowReader>(
        cuckoo.bucket_count(),
        PIR_DOUBLE_SHARD_ROWS,
        [&cuckoo](size_t i) { return cuckoo.bucket_row(i); }
    );
}

[[maybe_unused]] static std::vector<std::unique_ptr<PirDoubleRowReaderBase>> build_pir_double_reader_copies(
    const PirDoubleRowReaderBase& primary,
    size_t total_reader_count
) {
    std::vector<std::unique_ptr<PirDoubleRowReaderBase>> copies;
    if (total_reader_count <= 1) {
        return copies;
    }

    copies.resize(total_reader_count - 1);
    parallel_for_indices(copies.size(), [&](size_t i) {
        copies[i] = primary.clone();
    }, "PIR_double reader copy construction");
    return copies;
}

class PirSingleRowReaderBase {
public:
    virtual ~PirSingleRowReaderBase() = default;
    virtual pir_punc::Row read_row(size_t idx) = 0;
    virtual double client_runtime_ms() const { return 0.0; }
    virtual double server_runtime_ms() const { return 0.0; }
    virtual std::uint64_t communication_bytes() const { return 0; }
    virtual std::uint64_t setup_communication_bytes() const { return 0; }
    virtual std::uint64_t query_communication_bytes() const { return 0; }
    virtual std::uint64_t response_communication_bytes() const { return 0; }
    virtual std::vector<pir_punc::Row> read_rows(const std::vector<size_t>& indices) {
        std::vector<pir_punc::Row> rows;
        rows.reserve(indices.size());
        for (size_t idx : indices) {
            rows.push_back(read_row(idx));
        }
        return rows;
    }
};

#if FUZZY_PETS_HAS_FRODOPIR
template<size_t DB_ROWS, size_t ROW_BYTES, size_t MAT_ELEMENT_BITS>
class FrodoPirSingleRowReader : public PirSingleRowReaderBase {
public:
    explicit FrodoPirSingleRowReader(const std::vector<pir_punc::Row>& rows) {
        if (rows.size() > DB_ROWS) {
            throw std::invalid_argument("FrodoPirSingleRowReader: too many rows for shard");
        }

        std::array<uint8_t, frodoPIR_server::SEED_BYTE_LEN> seed_mu{};
        csprng_.generate(seed_mu);

        std::vector<uint8_t> db_bytes(DB_BYTE_LEN, 0);
        for (size_t row_idx = 0; row_idx < rows.size(); ++row_idx) {
            if (rows[row_idx].size() > ROW_BYTES) {
                throw std::invalid_argument("FrodoPirSingleRowReader: row is larger than configured row size");
            }
            std::copy(
                rows[row_idx].begin(),
                rows[row_idx].end(),
                db_bytes.begin() + row_idx * ROW_BYTES
            );
        }

        auto db_span = std::span<const uint8_t, DB_BYTE_LEN>(db_bytes.data(), DB_BYTE_LEN);
        auto [server, public_m] = Server::setup(seed_span(seed_mu), db_span);
        server_ = std::make_unique<Server>(std::move(server));

        std::vector<uint8_t> public_m_bytes(Client::PUBLIC_MATRIX_M_BYTE_LEN, 0);
        auto public_m_span = std::span<uint8_t, Client::PUBLIC_MATRIX_M_BYTE_LEN>(
            public_m_bytes.data(),
            Client::PUBLIC_MATRIX_M_BYTE_LEN
        );
        public_m.to_le_bytes(public_m_span);
        setup_communication_bytes_ +=
            frodoPIR_server::SEED_BYTE_LEN + Client::PUBLIC_MATRIX_M_BYTE_LEN;
        client_ = std::make_unique<Client>(
            Client::setup(seed_span(seed_mu), const_public_m_span(public_m_bytes))
        );

        query_bytes_.resize(Client::QUERY_BYTE_LEN);
        response_bytes_.resize(Client::RESPONSE_BYTE_LEN);
        row_bytes_.resize(ROW_BYTES);
    }

    pir_punc::Row read_row(size_t idx) override {
        if (idx >= DB_ROWS) {
            throw std::out_of_range("FrodoPirSingleRowReader: row index out of range");
        }

        auto client_start = Clock::now();
        const bool prepared = client_->prepare_query(idx, csprng_);
        if (!prepared) {
            throw std::runtime_error("FrodoPirSingleRowReader: failed to prepare FrodoPIR query");
        }

        auto query_span = std::span<uint8_t, Client::QUERY_BYTE_LEN>(
            query_bytes_.data(),
            Client::QUERY_BYTE_LEN
        );
        if (!client_->query(idx, query_span)) {
            throw std::runtime_error("FrodoPirSingleRowReader: failed to finalize FrodoPIR query");
        }
        query_communication_bytes_ += Client::QUERY_BYTE_LEN;
        client_runtime_ms_ += elapsed_ms(client_start, Clock::now());

        auto response_span = std::span<uint8_t, Client::RESPONSE_BYTE_LEN>(
            response_bytes_.data(),
            Client::RESPONSE_BYTE_LEN
        );
        auto server_start = Clock::now();
        server_->respond(query_span, response_span);
        response_communication_bytes_ += Client::RESPONSE_BYTE_LEN;
        server_runtime_ms_ += elapsed_ms(server_start, Clock::now());

        std::fill(row_bytes_.begin(), row_bytes_.end(), 0);
        auto row_span = std::span<uint8_t, ROW_BYTES>(row_bytes_.data(), ROW_BYTES);
        client_start = Clock::now();
        if (!client_->process_response(idx, response_span, row_span)) {
            throw std::runtime_error("FrodoPirSingleRowReader: failed to decode FrodoPIR response");
        }
        client_runtime_ms_ += elapsed_ms(client_start, Clock::now());

        return pir_punc::Row(row_bytes_.begin(), row_bytes_.end());
    }

    std::vector<pir_punc::Row> read_rows(const std::vector<size_t>& indices) override {
        std::vector<pir_punc::Row> rows(indices.size());
        std::vector<size_t> unique_indices;
        std::vector<std::vector<size_t>> output_positions;
        std::unordered_map<size_t, size_t> unique_pos_by_idx;

        for (size_t pos = 0; pos < indices.size(); ++pos) {
            const size_t idx = indices[pos];
            if (idx >= DB_ROWS) {
                throw std::out_of_range("FrodoPirSingleRowReader: row index out of range");
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

        auto client_start = Clock::now();
        auto prep_status = client_->prepare_query(
            std::span<const size_t>(unique_indices.data(), unique_indices.size()),
            csprng_
        );
        for (bool prepared : prep_status) {
            if (!prepared) {
                throw std::runtime_error("FrodoPirSingleRowReader: failed to prepare FrodoPIR batch query");
            }
        }
        client_runtime_ms_ += elapsed_ms(client_start, Clock::now());

        std::vector<pir_punc::Row> unique_rows(unique_indices.size());

        for (size_t i = 0; i < unique_indices.size(); ++i) {
            auto query_span = std::span<uint8_t, Client::QUERY_BYTE_LEN>(
                query_bytes_.data(),
                Client::QUERY_BYTE_LEN
            );
            client_start = Clock::now();
            if (!client_->query(unique_indices[i], query_span)) {
                throw std::runtime_error("FrodoPirSingleRowReader: failed to finalize FrodoPIR batch query");
            }
            query_communication_bytes_ += Client::QUERY_BYTE_LEN;
            client_runtime_ms_ += elapsed_ms(client_start, Clock::now());

            auto response_span = std::span<uint8_t, Client::RESPONSE_BYTE_LEN>(
                response_bytes_.data(),
                Client::RESPONSE_BYTE_LEN
            );
            auto server_start = Clock::now();
            server_->respond(query_span, response_span);
            response_communication_bytes_ += Client::RESPONSE_BYTE_LEN;
            server_runtime_ms_ += elapsed_ms(server_start, Clock::now());

            std::vector<uint8_t> decoded_row(ROW_BYTES, 0);
            auto row_span = std::span<uint8_t, ROW_BYTES>(
                decoded_row.data(),
                ROW_BYTES
            );
            client_start = Clock::now();
            if (!client_->process_response(unique_indices[i], response_span, row_span)) {
                throw std::runtime_error("FrodoPirSingleRowReader: failed to decode FrodoPIR batch response");
            }
            client_runtime_ms_ += elapsed_ms(client_start, Clock::now());
            unique_rows[i] = pir_punc::Row(decoded_row.begin(), decoded_row.end());
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

private:
    using Server = frodoPIR_server::server_t<DB_ROWS, ROW_BYTES, MAT_ELEMENT_BITS>;
    using Client = frodoPIR_client::client_t<DB_ROWS, ROW_BYTES, MAT_ELEMENT_BITS>;
    static constexpr size_t DB_BYTE_LEN = DB_ROWS * ROW_BYTES;

    static std::span<const uint8_t, frodoPIR_server::SEED_BYTE_LEN> seed_span(
        const std::array<uint8_t, frodoPIR_server::SEED_BYTE_LEN>& seed
    ) {
        return std::span<const uint8_t, frodoPIR_server::SEED_BYTE_LEN>(
            seed.data(),
            frodoPIR_server::SEED_BYTE_LEN
        );
    }

    static std::span<const uint8_t, Client::PUBLIC_MATRIX_M_BYTE_LEN> const_public_m_span(
        const std::vector<uint8_t>& public_m_bytes
    ) {
        return std::span<const uint8_t, Client::PUBLIC_MATRIX_M_BYTE_LEN>(
            public_m_bytes.data(),
            Client::PUBLIC_MATRIX_M_BYTE_LEN
        );
    }

    csprng::csprng_t csprng_{};
    std::unique_ptr<Server> server_;
    std::unique_ptr<Client> client_;
    std::vector<uint8_t> query_bytes_;
    std::vector<uint8_t> response_bytes_;
    std::vector<uint8_t> row_bytes_;
    double client_runtime_ms_ = 0.0;
    double server_runtime_ms_ = 0.0;
    std::uint64_t setup_communication_bytes_ = 0;
    std::uint64_t query_communication_bytes_ = 0;
    std::uint64_t response_communication_bytes_ = 0;
};

template<size_t DB_ROWS, size_t ROW_BYTES>
class ShardedFrodoPirSingleRowReader : public PirSingleRowReaderBase {
public:
    using RowBuilder = std::function<pir_punc::Row(size_t)>;

    ShardedFrodoPirSingleRowReader(size_t total_rows, RowBuilder row_builder)
        : total_rows_(total_rows),
          row_builder_(std::move(row_builder)) {
        if (total_rows_ == 0) {
            throw std::invalid_argument("ShardedFrodoPirSingleRowReader: total_rows must be > 0");
        }

        if (PIR_SINGLE_PREBUILD_SHARDS) {
            const size_t shard_count = (total_rows_ + DB_ROWS - 1) / DB_ROWS;
            prebuild_shards_parallel(shard_count);
        }
    }

    pir_punc::Row read_row(size_t idx) override {
        if (idx >= total_rows_) {
            throw std::out_of_range("ShardedFrodoPirSingleRowReader: row index out of range");
        }

        const size_t shard_idx = idx / DB_ROWS;
        const size_t local_idx = idx % DB_ROWS;
        auto server_start = Clock::now();
        auto& reader = reader_for_shard(shard_idx);
        server_runtime_ms_ += elapsed_ms(server_start, Clock::now());
        const double client_before = reader.client_runtime_ms();
        const double server_before = reader.server_runtime_ms();
        const std::uint64_t setup_before = reader.setup_communication_bytes();
        const std::uint64_t query_before = reader.query_communication_bytes();
        const std::uint64_t response_before = reader.response_communication_bytes();
        pir_punc::Row row = reader.read_row(local_idx);
        client_runtime_ms_ += reader.client_runtime_ms() - client_before;
        server_runtime_ms_ += reader.server_runtime_ms() - server_before;
        setup_communication_bytes_ += reader.setup_communication_bytes() - setup_before;
        query_communication_bytes_ += reader.query_communication_bytes() - query_before;
        response_communication_bytes_ +=
            reader.response_communication_bytes() - response_before;
        return row;
    }

    std::vector<pir_punc::Row> read_rows(const std::vector<size_t>& indices) override {
        std::vector<pir_punc::Row> rows(indices.size());
        std::unordered_map<size_t, std::vector<std::pair<size_t, size_t>>> by_shard;
        std::vector<size_t> shard_order;

        for (size_t pos = 0; pos < indices.size(); ++pos) {
            const size_t idx = indices[pos];
            if (idx >= total_rows_) {
                throw std::out_of_range("ShardedFrodoPirSingleRowReader: row index out of range");
            }

            const size_t shard_idx = idx / DB_ROWS;
            const size_t local_idx = idx % DB_ROWS;
            if (by_shard.find(shard_idx) == by_shard.end()) {
                shard_order.push_back(shard_idx);
            }
            by_shard[shard_idx].push_back({pos, local_idx});
        }

        for (size_t shard_idx : shard_order) {
            const auto& requests = by_shard[shard_idx];
            std::vector<size_t> local_indices;
            local_indices.reserve(requests.size());
            for (const auto& request : requests) {
                local_indices.push_back(request.second);
            }

            auto server_start = Clock::now();
            auto& reader = reader_for_shard(shard_idx);
            server_runtime_ms_ += elapsed_ms(server_start, Clock::now());
            const double client_before = reader.client_runtime_ms();
            const double server_before = reader.server_runtime_ms();
            const std::uint64_t setup_before = reader.setup_communication_bytes();
            const std::uint64_t query_before = reader.query_communication_bytes();
            const std::uint64_t response_before = reader.response_communication_bytes();
            std::vector<pir_punc::Row> shard_rows_vec = reader.read_rows(local_indices);
            client_runtime_ms_ += reader.client_runtime_ms() - client_before;
            server_runtime_ms_ += reader.server_runtime_ms() - server_before;
            setup_communication_bytes_ += reader.setup_communication_bytes() - setup_before;
            query_communication_bytes_ += reader.query_communication_bytes() - query_before;
            response_communication_bytes_ +=
                reader.response_communication_bytes() - response_before;
            if (shard_rows_vec.size() != requests.size()) {
                throw std::runtime_error("ShardedFrodoPirSingleRowReader: batch response size mismatch");
            }

            for (size_t i = 0; i < requests.size(); ++i) {
                rows[requests[i].first] = std::move(shard_rows_vec[i]);
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

private:
    using Reader = FrodoPirSingleRowReader<
        DB_ROWS,
        ROW_BYTES,
        PIR_SINGLE_MAT_ELEMENT_BITS
    >;

    std::unique_ptr<Reader> build_reader_for_shard(
        size_t shard_idx,
        const RowBuilder& row_builder
    ) const {
        const size_t start = shard_idx * DB_ROWS;
        const size_t count = std::min(DB_ROWS, total_rows_ - start);

        std::vector<pir_punc::Row> rows;
        rows.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            rows.push_back(row_builder(start + i));
        }

        return std::make_unique<Reader>(rows);
    }

    void prebuild_shards_parallel(size_t shard_count) {
        std::vector<std::unique_ptr<Reader>> built(shard_count);
        const size_t workers = std::min(
            parallel_worker_count(shard_count),
            PIR_SHARD_PREBUILD_MAX_WORKERS
        );
        if (workers <= 1) {
            for (size_t shard_idx = 0; shard_idx < shard_count; ++shard_idx) {
                built[shard_idx] = build_reader_for_shard(shard_idx, row_builder_);
            }
        } else {
            std::atomic<size_t> next{0};
            std::atomic<bool> stop{false};
            std::mutex exception_mutex;
            std::exception_ptr first_exception = nullptr;
            std::vector<std::thread> threads;
            threads.reserve(workers);

            for (size_t worker = 0; worker < workers; ++worker) {
                RowBuilder row_builder = row_builder_;
                threads.emplace_back([&, row_builder]() {
                    try {
                        while (!stop.load(std::memory_order_relaxed)) {
                            const size_t shard_idx = next.fetch_add(1, std::memory_order_relaxed);
                            if (shard_idx >= shard_count) {
                                break;
                            }
                            built[shard_idx] = build_reader_for_shard(shard_idx, row_builder);
                        }
                    } catch (...) {
                        {
                            std::lock_guard<std::mutex> lock(exception_mutex);
                            if (!first_exception) {
                                first_exception = std::current_exception();
                            }
                        }
                        stop.store(true, std::memory_order_relaxed);
                    }
                });
            }

            for (auto& thread : threads) {
                thread.join();
            }
            if (first_exception) {
                std::rethrow_exception(first_exception);
            }
        }

        for (size_t shard_idx = 0; shard_idx < shard_count; ++shard_idx) {
            lru_.push_back(shard_idx);
            setup_communication_bytes_ += built[shard_idx]->setup_communication_bytes();
            readers_.emplace(shard_idx, std::move(built[shard_idx]));
        }
    }

    Reader& reader_for_shard(size_t shard_idx) {
        auto cached = readers_.find(shard_idx);
        if (cached != readers_.end()) {
            touch(shard_idx);
            return *cached->second;
        }

        if (!PIR_SINGLE_PREBUILD_SHARDS) {
            evict_if_needed();
        }
        lru_.push_back(shard_idx);
        std::unique_ptr<Reader> reader = build_reader_for_shard(shard_idx, row_builder_);
        setup_communication_bytes_ += reader->setup_communication_bytes();
        auto inserted = readers_.emplace(
            shard_idx,
            std::move(reader)
        );
        return *inserted.first->second;
    }

    void touch(size_t shard_idx) {
        auto it = std::find(lru_.begin(), lru_.end(), shard_idx);
        if (it != lru_.end()) {
            lru_.erase(it);
            lru_.push_back(shard_idx);
        }
    }

    void evict_if_needed() {
        while (readers_.size() >= PIR_SINGLE_MAX_CACHED_SHARDS && !lru_.empty()) {
            readers_.erase(lru_.front());
            lru_.pop_front();
        }
    }

    size_t total_rows_;
    RowBuilder row_builder_;
    std::unordered_map<size_t, std::unique_ptr<Reader>> readers_;
    std::deque<size_t> lru_;
    double client_runtime_ms_ = 0.0;
    double server_runtime_ms_ = 0.0;
    std::uint64_t setup_communication_bytes_ = 0;
    std::uint64_t query_communication_bytes_ = 0;
    std::uint64_t response_communication_bytes_ = 0;
};

static size_t pir_single_adaptive_db_rows(size_t total_rows) {
    if (total_rows <= (1ULL << 10)) return 1ULL << 10;
    if (total_rows <= (1ULL << 12)) return 1ULL << 12;
    if (total_rows <= (1ULL << 14)) return 1ULL << 14;
    return PIR_SINGLE_MAX_SHARD_ROWS;
}

template<size_t ROW_BYTES>
static std::unique_ptr<PirSingleRowReaderBase> build_adaptive_pir_single_reader(
    size_t total_rows,
    std::function<pir_punc::Row(size_t)> row_builder
) {
    const size_t db_rows = pir_single_adaptive_db_rows(total_rows);
    switch (db_rows) {
        case (1ULL << 10):
            return std::make_unique<ShardedFrodoPirSingleRowReader<(1ULL << 10), ROW_BYTES>>(
                total_rows,
                std::move(row_builder)
            );
        case (1ULL << 12):
            return std::make_unique<ShardedFrodoPirSingleRowReader<(1ULL << 12), ROW_BYTES>>(
                total_rows,
                std::move(row_builder)
            );
        case (1ULL << 14):
            return std::make_unique<ShardedFrodoPirSingleRowReader<(1ULL << 14), ROW_BYTES>>(
                total_rows,
                std::move(row_builder)
            );
        default:
            return std::make_unique<ShardedFrodoPirSingleRowReader<PIR_SINGLE_MAX_SHARD_ROWS, ROW_BYTES>>(
                total_rows,
                std::move(row_builder)
            );
    }
}

static std::unique_ptr<PirSingleRowReaderBase> build_bloom_pir_single_reader(const BloomFilter& bloom) {
    return build_adaptive_pir_single_reader<1>(
        bloom.bucket_count(),
        [&bloom](size_t i) { return pir_punc::Row{bloom.bit_at(i)}; }
    );
}

static std::unique_ptr<PirSingleRowReaderBase> build_cuckoo_pir_single_reader(const CuckooFilter& cuckoo) {
    const size_t row_bytes = cuckoo.slots_per_bucket() * sizeof(uint32_t);
    if (row_bytes != 16) {
        throw std::invalid_argument("PIR_single FrodoPIR Cuckoo rows currently expect 16-byte buckets");
    }

    return build_adaptive_pir_single_reader<16>(
        cuckoo.bucket_count(),
        [&cuckoo](size_t i) { return cuckoo.bucket_row(i); }
    );
}

[[maybe_unused]] static std::vector<std::unique_ptr<PirSingleRowReaderBase>> build_bloom_pir_single_reader_copies(
    const BloomFilter& bloom,
    size_t total_reader_count
) {
    std::vector<std::unique_ptr<PirSingleRowReaderBase>> copies;
    if (total_reader_count <= 1) {
        return copies;
    }

    copies.resize(total_reader_count - 1);
    parallel_for_indices(copies.size(), [&](size_t i) {
        copies[i] = build_adaptive_pir_single_reader<1>(
            bloom.bucket_count(),
            [&bloom](size_t row) { return pir_punc::Row{bloom.bit_at(row)}; }
        );
    }, "PIR_single reader copy construction");
    return copies;
}

[[maybe_unused]] static std::vector<std::unique_ptr<PirSingleRowReaderBase>> build_cuckoo_pir_single_reader_copies(
    const CuckooFilter& cuckoo,
    size_t total_reader_count
) {
    std::vector<std::unique_ptr<PirSingleRowReaderBase>> copies;
    if (total_reader_count <= 1) {
        return copies;
    }

    const size_t row_bytes = cuckoo.slots_per_bucket() * sizeof(uint32_t);
    if (row_bytes != 16) {
        throw std::invalid_argument("PIR_single FrodoPIR Cuckoo rows currently expect 16-byte buckets");
    }

    copies.resize(total_reader_count - 1);
    parallel_for_indices(copies.size(), [&](size_t i) {
        copies[i] = build_adaptive_pir_single_reader<16>(
            cuckoo.bucket_count(),
            [&cuckoo](size_t row) { return cuckoo.bucket_row(row); }
        );
    }, "PIR_single reader copy construction");
    return copies;
}
#else
static std::unique_ptr<PirSingleRowReaderBase> build_bloom_pir_single_reader(const BloomFilter&) {
    throw std::runtime_error(
        "PIR_single requires FrodoPIR plus its sha3 and RandomShake headers; "
        "populate frodoPIR submodules and rebuild"
    );
}

static std::unique_ptr<PirSingleRowReaderBase> build_cuckoo_pir_single_reader(const CuckooFilter&) {
    throw std::runtime_error(
        "PIR_single requires FrodoPIR plus its sha3 and RandomShake headers; "
        "populate frodoPIR submodules and rebuild"
    );
}

static std::vector<std::unique_ptr<PirSingleRowReaderBase>> build_bloom_pir_single_reader_copies(
    const BloomFilter&,
    size_t
) {
    throw std::runtime_error(
        "PIR_single requires FrodoPIR plus its sha3 and RandomShake headers; "
        "populate frodoPIR submodules and rebuild"
    );
}

static std::vector<std::unique_ptr<PirSingleRowReaderBase>> build_cuckoo_pir_single_reader_copies(
    const CuckooFilter&,
    size_t
) {
    throw std::runtime_error(
        "PIR_single requires FrodoPIR plus its sha3 and RandomShake headers; "
        "populate frodoPIR submodules and rebuild"
    );
}
#endif

static std::unique_ptr<fuzzy_pets_batchpir::RowReader> build_bloom_batchpir_reader(
    const BloomFilter& bloom
) {
    return fuzzy_pets_batchpir::make_row_reader(
        bloom.bucket_count(),
        1,
        [&bloom](size_t i) { return pir_punc::Row{bloom.bit_at(i)}; },
        PIR_BATCHPIR_BATCH_SIZE
    );
}

static std::unique_ptr<fuzzy_pets_batchpir::RowReader> build_cuckoo_batchpir_reader(
    const CuckooFilter& cuckoo
) {
    const size_t row_bytes = cuckoo.slots_per_bucket() * sizeof(uint32_t);
    return fuzzy_pets_batchpir::make_row_reader(
        cuckoo.bucket_count(),
        row_bytes,
        [&cuckoo](size_t i) { return cuckoo.bucket_row(i); },
        PIR_BATCHPIR_BATCH_SIZE
    );
}

static const char* active_pir_label() {
    if (USE_PIR_DOUBLE) {
        return "PIR_double";
    }
    if (USE_PIR_SINGLE) {
        return "PIR_single";
    }
    if (USE_PIR_BATCHPIR) {
        return "PIR_BatchPIR";
    }
    return "none";
}

static void print_pir_scheme_for_run(int run, int total_runs) {
    cout << "[PIR] run=" << (run + 1)
         << "/" << total_runs
         << " scheme=" << active_pir_label()
         << "\n";
}

static double pir_double_server_runtime_ms(
    const std::unique_ptr<PirDoubleRowReaderBase>& primary,
    const std::vector<std::unique_ptr<PirDoubleRowReaderBase>>& copies
) {
    double total = primary ? primary->server_runtime_ms() : 0.0;
    for (const auto& copy : copies) {
        if (copy) {
            total += copy->server_runtime_ms();
        }
    }
    return total;
}

static double pir_single_server_runtime_ms(
    const std::unique_ptr<PirSingleRowReaderBase>& primary,
    const std::vector<std::unique_ptr<PirSingleRowReaderBase>>& copies
) {
    double total = primary ? primary->server_runtime_ms() : 0.0;
    for (const auto& copy : copies) {
        if (copy) {
            total += copy->server_runtime_ms();
        }
    }
    return total;
}

template <typename ReaderBase>
static std::uint64_t pir_reader_communication_bytes(
    const std::unique_ptr<ReaderBase>& primary,
    const std::vector<std::unique_ptr<ReaderBase>>& copies
) {
    std::uint64_t total = primary ? primary->communication_bytes() : 0;
    for (const auto& copy : copies) {
        if (copy) {
            total += copy->communication_bytes();
        }
    }
    return total;
}

template <typename ReaderBase>
static std::uint64_t pir_reader_setup_communication_bytes(
    const std::unique_ptr<ReaderBase>& primary,
    const std::vector<std::unique_ptr<ReaderBase>>& copies
) {
    std::uint64_t total = primary ? primary->setup_communication_bytes() : 0;
    for (const auto& copy : copies) {
        if (copy) {
            total += copy->setup_communication_bytes();
        }
    }
    return total;
}

template <typename ReaderBase>
static std::uint64_t pir_reader_query_communication_bytes(
    const std::unique_ptr<ReaderBase>& primary,
    const std::vector<std::unique_ptr<ReaderBase>>& copies
) {
    std::uint64_t total = primary ? primary->query_communication_bytes() : 0;
    for (const auto& copy : copies) {
        if (copy) {
            total += copy->query_communication_bytes();
        }
    }
    return total;
}

template <typename ReaderBase>
static std::uint64_t pir_reader_response_communication_bytes(
    const std::unique_ptr<ReaderBase>& primary,
    const std::vector<std::unique_ptr<ReaderBase>>& copies
) {
    std::uint64_t total = primary ? primary->response_communication_bytes() : 0;
    for (const auto& copy : copies) {
        if (copy) {
            total += copy->response_communication_bytes();
        }
    }
    return total;
}
