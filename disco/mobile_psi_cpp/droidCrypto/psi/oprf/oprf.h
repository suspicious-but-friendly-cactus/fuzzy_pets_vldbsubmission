#pragma once

#ifdef __cplusplus
#include <cstdint>
#else
#include <stdbool.h>
#include <stdint.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gcaes_oprf_stats_t {
    uint64_t bytes_sent;
    uint64_t bytes_recv;
    double runtime_ms;
} gcaes_oprf_stats_t;

typedef gcaes_oprf_stats_t ecnr_oprf_stats_t;
typedef gcaes_oprf_stats_t gclowmc_oprf_stats_t;

//void SayHi();
void doECNR_OPRF(int num_elements, bool first, char* s_addr, int s_port, uint8_t * ptr);
void doECNR_OPRF_Blocks(const uint8_t* input_blocks, int num_elements, const char* s_addr, int s_port, uint8_t* ptr);
void doECNR_OPRF_Blocks_Stats(
    const uint8_t* input_blocks,
    int num_elements,
    const char* s_addr,
    int s_port,
    uint8_t* ptr,
    ecnr_oprf_stats_t* stats
);
void doECNR_PRF_Blocks_Stats(
    const uint8_t* input_blocks,
    int num_elements,
    uint8_t* ptr,
    ecnr_oprf_stats_t* stats
);
void doGCAES_OPRF(int num_elements, bool first, char* s_addr, int s_port, uint8_t * ptr);
void doGCAES_OPRF_Blocks(const uint8_t* input_blocks, int num_elements, const char* s_addr, int s_port, uint8_t* ptr);
void doGCAES_OPRF_Blocks_Stats(
    const uint8_t* input_blocks,
    int num_elements,
    const char* s_addr,
    int s_port,
    uint8_t* ptr,
    gcaes_oprf_stats_t* stats
);
void doGCLowMC_OPRF(int num_element, bool first, char* s_addr, int s_port, uint8_t * ptr);
void doGCLOWMC_OPRF_Blocks(const uint8_t* input_blocks, int num_elements, const char* s_addr, int s_port, uint8_t* ptr);
void doGCLOWMC_OPRF_Blocks_Stats(
    const uint8_t* input_blocks,
    int num_elements,
    const char* s_addr,
    int s_port,
    uint8_t* ptr,
    gclowmc_oprf_stats_t* stats
);
void doGCLOWMC_PRF_Blocks_Stats(
    const uint8_t* input_blocks,
    int num_elements,
    uint8_t* ptr,
    gclowmc_oprf_stats_t* stats
);


#ifdef __cplusplus
}
#endif
