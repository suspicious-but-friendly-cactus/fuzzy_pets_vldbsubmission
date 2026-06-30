#pragma once

#include <cstddef>
#include <cstdint>

void disco_gcaes_oprf_set_server(const char* host, int port);
const char* disco_gcaes_oprf_server_host();
int disco_gcaes_oprf_server_port();
void disco_oprf_set_mechanism(const char* mechanism);
const char* disco_oprf_mechanism();

struct DiscoGcaesOprfStats {
    std::uint64_t bytes_sent = 0;
    std::uint64_t bytes_recv = 0;
    double runtime_ms = 0.0;
    std::size_t calls = 0;
    std::size_t blocks = 0;
};

void disco_gcaes_oprf_reset_stats();
DiscoGcaesOprfStats disco_gcaes_oprf_stats();
std::uint64_t disco_gcaes_oprf_total_communication_bytes();
double disco_gcaes_oprf_runtime_ms();

void disco_gcaes_prf_eval_blocks(
    const std::uint8_t* input_blocks,
    std::size_t block_count,
    std::uint8_t* output_blocks
);

void disco_oprf_eval_blocks(
    const std::uint8_t* input_blocks,
    std::size_t block_count,
    std::uint8_t* output_blocks
);

void disco_oprf_server_prf_eval_blocks(
    const std::uint8_t* input_blocks,
    std::size_t block_count,
    std::uint8_t* output_blocks
);
