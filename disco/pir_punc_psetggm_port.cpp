#include <algorithm>
#include <cstdint>
#include <cstring>
#include <random>
#include <stdexcept>
#include <utility>
#include <vector>

namespace pir_punc {

using Row = std::vector<std::uint8_t>;

struct StaticDB {
    int numRows = 0;
    int rowLen = 0;
    std::vector<std::uint8_t> flatDb;
};

struct PuncHintReq {};

struct PuncHintResp {
    int nRows = 0;
    int rowLen = 0;
    int setSize = 0;
    int randInit = 0;
    std::vector<std::uint8_t> setGenKey;
    std::vector<std::uint8_t> hints;
};

struct PuncturedSet {
    std::vector<std::uint8_t> keys;
    int hole = 0;
    int shift = 0;
    int univ_size = 0;
    int set_size = 0;
};

struct PuncQueryReq {
    int extra_element = 0;
    PuncturedSet puncturedSet;
};

struct PuncQueryResp {
    Row answer;
    Row extraElem;
};

struct PuncQueryCtx {
    int index = 0;
};

StaticDB static_db_from_rows(const std::vector<Row>& rows) {
    StaticDB db;
    db.numRows = static_cast<int>(rows.size());
    db.rowLen = rows.empty() ? 0 : static_cast<int>(rows.front().size());
    db.flatDb.resize(static_cast<size_t>(db.numRows) * static_cast<size_t>(db.rowLen));

    for (int row_idx = 0; row_idx < db.numRows; ++row_idx) {
        if (static_cast<int>(rows[static_cast<size_t>(row_idx)].size()) != db.rowLen) {
            throw std::invalid_argument("pir_punc::static_db_from_rows: unequal row lengths");
        }
        std::memcpy(
            db.flatDb.data() + static_cast<size_t>(row_idx) * static_cast<size_t>(db.rowLen),
            rows[static_cast<size_t>(row_idx)].data(),
            static_cast<size_t>(db.rowLen)
        );
    }

    return db;
}

PuncHintResp process_hint_req(const PuncHintReq&, const StaticDB& db, std::mt19937_64&) {
    PuncHintResp resp;
    resp.nRows = db.numRows;
    resp.rowLen = db.rowLen;
    resp.setSize = 1;
    resp.randInit = 0;
    return resp;
}

PuncQueryResp process_query(const PuncQueryReq& query, const StaticDB& db) {
    if (query.extra_element < 0 || query.extra_element >= db.numRows) {
        throw std::out_of_range("pir_punc::process_query: row index out of range");
    }

    PuncQueryResp resp;
    const size_t offset =
        static_cast<size_t>(query.extra_element) * static_cast<size_t>(db.rowLen);
    resp.answer.resize(static_cast<size_t>(db.rowLen));
    std::memcpy(resp.answer.data(), db.flatDb.data() + offset, static_cast<size_t>(db.rowLen));
    return resp;
}

class PuncClient {
public:
    PuncClient(const PuncHintResp& hint, std::mt19937_64&, double)
        : hint_(hint) {}

    std::pair<std::vector<PuncQueryReq>, PuncQueryCtx> query(int index) {
        if (index < 0 || index >= hint_.nRows) {
            throw std::runtime_error("query index not covered by hints");
        }

        PuncQueryReq req;
        req.extra_element = index;
        req.puncturedSet.univ_size = hint_.nRows;
        req.puncturedSet.set_size = hint_.setSize;
        return {{req}, PuncQueryCtx{index}};
    }

    Row reconstruct(const PuncQueryCtx&, const std::vector<PuncQueryResp>& responses) const {
        if (responses.empty()) {
            throw std::runtime_error("pir_punc::PuncClient::reconstruct: empty response");
        }
        return responses.front().answer;
    }

private:
    PuncHintResp hint_;
};

} // namespace pir_punc
