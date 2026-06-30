#include "disco_gcaes_oprf_adapter.h"

#include <dlfcn.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct OprfCallStats {
    std::uint64_t bytes_sent;
    std::uint64_t bytes_recv;
    double runtime_ms;
};

using OprfBlocksStatsFn = void (*)(
    const std::uint8_t*,
    int,
    const char*,
    int,
    std::uint8_t*,
    OprfCallStats*
);

using ServerPrfBlocksStatsFn = void (*)(
    const std::uint8_t*,
    int,
    std::uint8_t*,
    OprfCallStats*
);

constexpr const char* kDefaultHost = "127.0.0.1";
constexpr int kDefaultPort = 50051;
constexpr const char* kGcaesMechanism = "GCAES";
constexpr const char* kEcnrMechanism = "ECNR";
constexpr const char* kGclowmcMechanism = "GCLOWMC";

std::mutex config_mutex;
std::mutex oprf_call_mutex;
std::mutex stats_mutex;
bool env_loaded = false;
std::string configured_host = kDefaultHost;
int configured_port = kDefaultPort;
std::string configured_mechanism = kGcaesMechanism;
std::string configured_library_path;
void* droidcrypto_handle = nullptr;
OprfBlocksStatsFn gcaes_oprf_blocks_stats = nullptr;
OprfBlocksStatsFn ecnr_oprf_blocks_stats = nullptr;
OprfBlocksStatsFn gclowmc_oprf_blocks_stats = nullptr;
ServerPrfBlocksStatsFn ecnr_server_prf_blocks_stats = nullptr;
ServerPrfBlocksStatsFn gclowmc_server_prf_blocks_stats = nullptr;
DiscoGcaesOprfStats accumulated_stats;

std::string normalize_mechanism(const char* mechanism) {
    if (mechanism == nullptr || std::strlen(mechanism) == 0) {
        throw std::invalid_argument("OPRF mechanism cannot be empty");
    }

    std::string normalized;
    for (const unsigned char ch : std::string(mechanism)) {
        if (ch == '_' || ch == '-') {
            continue;
        }
        normalized.push_back(static_cast<char>(std::toupper(ch)));
    }

    if (normalized == "GCAES" || normalized == "GCAESOPRF") {
        return kGcaesMechanism;
    }
    if (normalized == "ECNR" || normalized == "ECNROPRF") {
        return kEcnrMechanism;
    }
    if (normalized == "GCLOWMC" || normalized == "GCLOWMCOPRF" ||
        normalized == "LOWMC" || normalized == "LOWMCOPRF") {
        return kGclowmcMechanism;
    }

    throw std::invalid_argument("OPRF mechanism must be GCAES, ECNR, or GCLOWMC");
}

void parse_server_addr(const std::string& addr, std::string& host, int& port) {
    const size_t colon = addr.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= addr.size()) {
        throw std::invalid_argument("OPRF address must be host:port");
    }

    const int parsed_port = std::stoi(addr.substr(colon + 1));
    if (parsed_port <= 0 || parsed_port > 65535) {
        throw std::invalid_argument("OPRF port must be in 1..65535");
    }

    host = addr.substr(0, colon);
    port = parsed_port;
}

void apply_host_env(const char* name) {
    if (const char* host = std::getenv(name)) {
        if (std::strlen(host) == 0) {
            throw std::invalid_argument(std::string(name) + " cannot be empty");
        }
        configured_host = host;
    }
}

void apply_port_env(const char* name) {
    if (const char* port = std::getenv(name)) {
        const int parsed_port = std::stoi(port);
        if (parsed_port <= 0 || parsed_port > 65535) {
            throw std::invalid_argument(std::string(name) + " must be in 1..65535");
        }
        configured_port = parsed_port;
    }
}

void apply_addr_env(const char* name) {
    if (const char* addr = std::getenv(name)) {
        parse_server_addr(addr, configured_host, configured_port);
    }
}

void load_env_locked() {
    if (env_loaded) {
        return;
    }
    env_loaded = true;

    if (const char* mechanism = std::getenv("FUZZY_PETS_OPRF_MECHANISM")) {
        configured_mechanism = normalize_mechanism(mechanism);
    }
    if (const char* mechanism = std::getenv("FUZZY_PETS_OPRF_TYPE")) {
        configured_mechanism = normalize_mechanism(mechanism);
    }

    apply_addr_env("FUZZY_PETS_OPRF_ADDR");
    apply_addr_env("FUZZY_PETS_GCAES_OPRF_ADDR");
    apply_addr_env("FUZZY_PETS_ECNR_OPRF_ADDR");
    apply_addr_env("FUZZY_PETS_GCLOWMC_OPRF_ADDR");

    apply_host_env("FUZZY_PETS_OPRF_HOST");
    apply_host_env("FUZZY_PETS_GCAES_OPRF_HOST");
    apply_host_env("FUZZY_PETS_ECNR_OPRF_HOST");
    apply_host_env("FUZZY_PETS_GCLOWMC_OPRF_HOST");

    apply_port_env("FUZZY_PETS_OPRF_PORT");
    apply_port_env("FUZZY_PETS_GCAES_OPRF_PORT");
    apply_port_env("FUZZY_PETS_ECNR_OPRF_PORT");
    apply_port_env("FUZZY_PETS_GCLOWMC_OPRF_PORT");

    if (const char* library_path = std::getenv("FUZZY_PETS_DROIDCRYPTO_LIB")) {
        configured_library_path = library_path;
    }
}

std::vector<std::string> droidcrypto_library_candidates_locked() {
#if defined(__APPLE__)
    constexpr const char* lib_name = "libdroidcrypto.dylib";
#else
    constexpr const char* lib_name = "libdroidcrypto.so";
#endif

    std::vector<std::string> candidates;
    if (!configured_library_path.empty()) {
        candidates.push_back(configured_library_path);
    }
    candidates.push_back(std::string("disco/mobile_psi_cpp/build-mac/droidCrypto/") + lib_name);
    candidates.push_back(std::string("disco/mobile_psi_cpp/build-linux/droidCrypto/") + lib_name);
    candidates.push_back(std::string("disco/mobile_psi_cpp/build/droidCrypto/") + lib_name);
    candidates.push_back(std::string("disco/contact-discovery/") + lib_name);
    candidates.push_back(lib_name);
    return candidates;
}

void* load_symbol_locked(const char* symbol_name, const char* description) {
    load_env_locked();

    if (droidcrypto_handle != nullptr) {
        dlerror();
        void* symbol = dlsym(droidcrypto_handle, symbol_name);
        const char* symbol_error = dlerror();
        if (symbol_error == nullptr) {
            return symbol;
        }
    }

    std::string errors;
    for (const auto& path : droidcrypto_library_candidates_locked()) {
        dlerror();
        void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (handle == nullptr) {
            const char* open_error = dlerror();
            if (!errors.empty()) {
                errors += "; ";
            }
            errors += path + ": " + (open_error ? open_error : "unknown dlopen error");
            continue;
        }

        dlerror();
        void* symbol = dlsym(handle, symbol_name);
        const char* symbol_error = dlerror();
        if (symbol_error != nullptr) {
            dlclose(handle);
            if (!errors.empty()) {
                errors += "; ";
            }
            errors += path + ": " + symbol_error;
            continue;
        }

        droidcrypto_handle = handle;
        return symbol;
    }

    throw std::runtime_error(
        std::string("Measured real ") + description +
        " requires a built libdroidcrypto exposing " + symbol_name + ". "
        "Build it with `cmake -S disco/mobile_psi_cpp -B disco/mobile_psi_cpp/build-linux && "
        "cmake --build disco/mobile_psi_cpp/build-linux --target droidcrypto oprf_server`, "
        "or set FUZZY_PETS_DROIDCRYPTO_LIB. "
        "dlopen details: " + errors
    );
}

OprfBlocksStatsFn interactive_symbol_locked() {
    if (configured_mechanism == kGcaesMechanism) {
        if (gcaes_oprf_blocks_stats == nullptr) {
            gcaes_oprf_blocks_stats = reinterpret_cast<OprfBlocksStatsFn>(
                load_symbol_locked("doGCAES_OPRF_Blocks_Stats", "GCAES OPRF")
            );
        }
        return gcaes_oprf_blocks_stats;
    }

    if (configured_mechanism == kEcnrMechanism) {
        if (ecnr_oprf_blocks_stats == nullptr) {
            ecnr_oprf_blocks_stats = reinterpret_cast<OprfBlocksStatsFn>(
                load_symbol_locked("doECNR_OPRF_Blocks_Stats", "ECNR OPRF")
            );
        }
        return ecnr_oprf_blocks_stats;
    }

    if (configured_mechanism == kGclowmcMechanism) {
        if (gclowmc_oprf_blocks_stats == nullptr) {
            gclowmc_oprf_blocks_stats = reinterpret_cast<OprfBlocksStatsFn>(
                load_symbol_locked("doGCLOWMC_OPRF_Blocks_Stats", "GCLOWMC OPRF")
            );
        }
        return gclowmc_oprf_blocks_stats;
    }

    throw std::runtime_error("Unsupported OPRF mechanism");
}

ServerPrfBlocksStatsFn ecnr_server_prf_symbol_locked() {
    if (ecnr_server_prf_blocks_stats == nullptr) {
        ecnr_server_prf_blocks_stats = reinterpret_cast<ServerPrfBlocksStatsFn>(
            load_symbol_locked("doECNR_PRF_Blocks_Stats", "ECNR server PRF")
        );
    }
    return ecnr_server_prf_blocks_stats;
}

ServerPrfBlocksStatsFn gclowmc_server_prf_symbol_locked() {
    if (gclowmc_server_prf_blocks_stats == nullptr) {
        gclowmc_server_prf_blocks_stats = reinterpret_cast<ServerPrfBlocksStatsFn>(
            load_symbol_locked("doGCLOWMC_PRF_Blocks_Stats", "GCLOWMC server PRF")
        );
    }
    return gclowmc_server_prf_blocks_stats;
}

}  // namespace

void disco_gcaes_oprf_set_server(const char* host, int port) {
    if (host == nullptr || std::strlen(host) == 0) {
        throw std::invalid_argument("OPRF host cannot be empty");
    }
    if (port <= 0 || port > 65535) {
        throw std::invalid_argument("OPRF port must be in 1..65535");
    }

    std::lock_guard<std::mutex> lock(config_mutex);
    load_env_locked();
    configured_host = host;
    configured_port = port;
}

const char* disco_gcaes_oprf_server_host() {
    std::lock_guard<std::mutex> lock(config_mutex);
    load_env_locked();
    return configured_host.c_str();
}

int disco_gcaes_oprf_server_port() {
    std::lock_guard<std::mutex> lock(config_mutex);
    load_env_locked();
    return configured_port;
}

void disco_oprf_set_mechanism(const char* mechanism) {
    std::lock_guard<std::mutex> lock(config_mutex);
    load_env_locked();
    configured_mechanism = normalize_mechanism(mechanism);
}

const char* disco_oprf_mechanism() {
    std::lock_guard<std::mutex> lock(config_mutex);
    load_env_locked();
    return configured_mechanism.c_str();
}

void disco_gcaes_oprf_reset_stats() {
    std::lock_guard<std::mutex> lock(stats_mutex);
    accumulated_stats = {};
}

DiscoGcaesOprfStats disco_gcaes_oprf_stats() {
    std::lock_guard<std::mutex> lock(stats_mutex);
    return accumulated_stats;
}

std::uint64_t disco_gcaes_oprf_total_communication_bytes() {
    const DiscoGcaesOprfStats stats = disco_gcaes_oprf_stats();
    return stats.bytes_sent + stats.bytes_recv;
}

double disco_gcaes_oprf_runtime_ms() {
    return disco_gcaes_oprf_stats().runtime_ms;
}

void disco_gcaes_prf_eval_blocks(
    const std::uint8_t* input_blocks,
    std::size_t block_count,
    std::uint8_t* output_blocks
) {
    disco_oprf_eval_blocks(input_blocks, block_count, output_blocks);
}

void disco_oprf_eval_blocks(
    const std::uint8_t* input_blocks,
    std::size_t block_count,
    std::uint8_t* output_blocks
) {
    if (block_count == 0) {
        return;
    }
    if (input_blocks == nullptr || output_blocks == nullptr) {
        throw std::invalid_argument("OPRF input/output buffers cannot be null");
    }
    if (block_count > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::invalid_argument("OPRF batch is too large");
    }

    OprfBlocksStatsFn fn = nullptr;
    std::string host;
    int port = 0;
    {
        std::lock_guard<std::mutex> lock(config_mutex);
        load_env_locked();
        fn = interactive_symbol_locked();
        host = configured_host;
        port = configured_port;
    }

    OprfCallStats call_stats{};
    std::lock_guard<std::mutex> call_lock(oprf_call_mutex);
    fn(
        input_blocks,
        static_cast<int>(block_count),
        host.c_str(),
        port,
        output_blocks,
        &call_stats
    );

    {
        std::lock_guard<std::mutex> lock(stats_mutex);
        accumulated_stats.bytes_sent += call_stats.bytes_sent;
        accumulated_stats.bytes_recv += call_stats.bytes_recv;
        accumulated_stats.runtime_ms += call_stats.runtime_ms;
        accumulated_stats.calls += 1;
        accumulated_stats.blocks += block_count;
    }
}

void disco_oprf_server_prf_eval_blocks(
    const std::uint8_t* input_blocks,
    std::size_t block_count,
    std::uint8_t* output_blocks
) {
    if (block_count == 0) {
        return;
    }
    if (input_blocks == nullptr || output_blocks == nullptr) {
        throw std::invalid_argument("server PRF input/output buffers cannot be null");
    }
    if (block_count > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::invalid_argument("server PRF batch is too large");
    }

    ServerPrfBlocksStatsFn fn = nullptr;
    {
        std::lock_guard<std::mutex> lock(config_mutex);
        load_env_locked();
        if (configured_mechanism != kEcnrMechanism &&
            configured_mechanism != kGclowmcMechanism) {
            throw std::runtime_error(
                "adapter server-side PRF is only needed for ECNR/GCLOWMC; GCAES is evaluated locally"
            );
        }
        fn = configured_mechanism == kEcnrMechanism
            ? ecnr_server_prf_symbol_locked()
            : gclowmc_server_prf_symbol_locked();
    }

    std::lock_guard<std::mutex> call_lock(oprf_call_mutex);
    fn(
        input_blocks,
        static_cast<int>(block_count),
        output_blocks,
        nullptr
    );
}
