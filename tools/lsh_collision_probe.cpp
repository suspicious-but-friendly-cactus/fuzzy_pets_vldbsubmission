#include "lsh_e2lsh.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

static std::vector<double> parse_vector(const json& value) {
    if (!value.is_array()) {
        throw std::runtime_error("Expected vector JSON array");
    }
    std::vector<double> out;
    out.reserve(value.size());
    for (const auto& x : value) {
        out.push_back(x.get<double>());
    }
    return out;
}

static json read_json(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("Cannot open " + path);
    }
    json value;
    in >> value;
    return value;
}

static bool any_key_in_set(
    const std::vector<uint64_t>& keys,
    const std::unordered_set<uint64_t>& set
) {
    for (uint64_t key : keys) {
        if (set.find(key) != set.end()) {
            return true;
        }
    }
    return false;
}

static bool any_key_overlap(
    const std::vector<uint64_t>& a,
    const std::vector<uint64_t>& b
) {
    for (uint64_t x : a) {
        for (uint64_t y : b) {
            if (x == y) {
                return true;
            }
        }
    }
    return false;
}

int main(int argc, char** argv) {
    if (argc < 6) {
        std::cerr
            << "usage: " << argv[0]
            << " SERVER_JSON CLOSE_JSON FAR_JSON L:k:w[,L:k:w...] MAX_CLIENT_ROWS\n";
        return 2;
    }

    const std::string server_path = argv[1];
    const std::string close_path = argv[2];
    const std::string far_path = argv[3];
    const std::string candidates_arg = argv[4];
    const size_t max_client_rows = static_cast<size_t>(std::stoull(argv[5]));

    json server_json = read_json(server_path);
    json close_json = read_json(close_path);
    json far_json = read_json(far_path);

    std::unordered_map<std::string, std::vector<double>> server;
    server.reserve(server_json.size());
    size_t dim = 0;
    for (auto it = server_json.begin(); it != server_json.end(); ++it) {
        auto vec = parse_vector(it.value());
        if (dim == 0) {
            dim = vec.size();
        } else if (vec.size() != dim) {
            throw std::runtime_error("Server dimension mismatch");
        }
        server.emplace(it.key(), std::move(vec));
    }
    if (dim == 0) {
        throw std::runtime_error("Empty Server JSON");
    }

    struct CloseRow {
        std::string server_id;
        std::vector<double> pixels;
    };
    struct FarRow {
        std::vector<double> pixels;
    };

    std::vector<CloseRow> close_rows;
    std::vector<FarRow> far_rows;

    for (auto it = close_json.begin(); it != close_json.end(); ++it) {
        if (server.find(it.key()) == server.end()) {
            continue;
        }
        const auto& arr = it.value().at("close");
        for (const auto& row : arr) {
            auto vec = parse_vector(row.at("pixels"));
            if (vec.size() == dim) {
                close_rows.push_back({it.key(), std::move(vec)});
                if (close_rows.size() >= max_client_rows / 2) {
                    break;
                }
            }
        }
        if (close_rows.size() >= max_client_rows / 2) {
            break;
        }
    }

    for (auto it = far_json.begin(); it != far_json.end(); ++it) {
        const json& value = it.value();
        if (value.is_array()) {
            for (const auto& row : value) {
                auto vec = parse_vector(row.at("pixels"));
                if (vec.size() == dim) {
                    far_rows.push_back({std::move(vec)});
                    if (far_rows.size() >= max_client_rows / 2) {
                        break;
                    }
                }
            }
        } else {
            auto vec = parse_vector(value.at("pixels"));
            if (vec.size() == dim) {
                far_rows.push_back({std::move(vec)});
            }
        }
        if (far_rows.size() >= max_client_rows / 2) {
            break;
        }
    }

    std::cout << "server_rows=" << server.size()
              << " close_rows_used=" << close_rows.size()
              << " far_rows_used=" << far_rows.size()
              << " dim=" << dim << "\n";

    size_t start = 0;
    while (start < candidates_arg.size()) {
        size_t end = candidates_arg.find(',', start);
        if (end == std::string::npos) {
            end = candidates_arg.size();
        }
        std::string raw = candidates_arg.substr(start, end - start);
        std::replace(raw.begin(), raw.end(), '/', ':');
        size_t p1 = raw.find(':');
        size_t p2 = raw.find(':', p1 == std::string::npos ? 0 : p1 + 1);
        if (p1 == std::string::npos || p2 == std::string::npos) {
            throw std::runtime_error("Invalid candidate " + raw);
        }
        const int L = std::stoi(raw.substr(0, p1));
        const int k = std::stoi(raw.substr(p1 + 1, p2 - p1 - 1));
        const double w = std::stod(raw.substr(p2 + 1));

        E2LSH lsh(static_cast<int>(dim), L, k, w);
        std::unordered_map<std::string, std::vector<uint64_t>> server_keys_by_id;
        std::unordered_set<uint64_t> server_key_union;
        server_keys_by_id.reserve(server.size());
        server_key_union.reserve(server.size() * static_cast<size_t>(L) * 2);

        for (const auto& [id, vec] : server) {
            auto keys = lsh.table_keys(vec);
            for (uint64_t key : keys) {
                server_key_union.insert(key);
            }
            server_keys_by_id.emplace(id, std::move(keys));
        }

        size_t close_related_hits = 0;
        size_t close_union_hits = 0;
        for (const auto& row : close_rows) {
            auto client_keys = lsh.table_keys(row.pixels);
            const auto related_it = server_keys_by_id.find(row.server_id);
            if (related_it != server_keys_by_id.end() &&
                any_key_overlap(client_keys, related_it->second)) {
                close_related_hits++;
            }
            if (any_key_in_set(client_keys, server_key_union)) {
                close_union_hits++;
            }
        }

        size_t far_union_hits = 0;
        for (const auto& row : far_rows) {
            auto client_keys = lsh.table_keys(row.pixels);
            if (any_key_in_set(client_keys, server_key_union)) {
                far_union_hits++;
            }
        }

        const double close_related_rate = close_rows.empty()
            ? 0.0
            : static_cast<double>(close_related_hits) / static_cast<double>(close_rows.size());
        const double close_union_rate = close_rows.empty()
            ? 0.0
            : static_cast<double>(close_union_hits) / static_cast<double>(close_rows.size());
        const double far_union_rate = far_rows.empty()
            ? 0.0
            : static_cast<double>(far_union_hits) / static_cast<double>(far_rows.size());

        std::cout << "L=" << L
                  << " k=" << k
                  << " w=" << w
                  << " server_unique_keys=" << server_key_union.size()
                  << " close_related_hit_rate=" << close_related_rate
                  << " close_union_hit_rate=" << close_union_rate
                  << " implied_fn=" << (1.0 - close_union_rate)
                  << " far_union_hit_rate=" << far_union_rate
                  << "\n";

        start = end + 1;
    }

    return 0;
}
