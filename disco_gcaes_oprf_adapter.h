#pragma once

#include <cstddef>
#include <cstdint>

void disco_gcaes_prf_eval_blocks(
    const std::uint8_t* input_blocks,
    std::size_t block_count,
    std::uint8_t* output_blocks
);
