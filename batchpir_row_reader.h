#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace fuzzy_pets_batchpir {

using Row = std::vector<std::uint8_t>;
using RowBuilder = std::function<Row(size_t)>;

bool available();
std::string unavailable_reason();

class RowReader {
public:
    virtual ~RowReader() = default;
    virtual Row read_row(size_t idx) = 0;
    virtual std::vector<Row> read_rows(const std::vector<size_t>& indices) = 0;
    virtual double client_runtime_ms() const { return 0.0; }
    virtual double server_runtime_ms() const { return 0.0; }
    // Online traffic: Client's PIR queries plus Server's PIR answers.
    virtual std::uint64_t communication_bytes() const { return 0; }
    // One-time protocol exchange, separate from Server's local DB construction.
    virtual std::uint64_t setup_communication_bytes() const { return 0; }
    virtual std::uint64_t query_communication_bytes() const { return 0; }
    virtual std::uint64_t response_communication_bytes() const { return 0; }
    virtual size_t batchpir_num_buckets() const { return 0; }
    virtual size_t batchpir_max_bucket_size() const { return 0; }
    virtual size_t batchpir_first_dimension_size() const { return 0; }
    virtual size_t batchpir_server_count() const { return 0; }
};

std::unique_ptr<RowReader> make_row_reader(
    size_t total_rows,
    size_t row_bytes,
    RowBuilder row_builder,
    size_t batch_size
);

} // namespace fuzzy_pets_batchpir
