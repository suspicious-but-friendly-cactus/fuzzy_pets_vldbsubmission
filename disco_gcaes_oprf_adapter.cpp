#include "disco_gcaes_oprf_adapter.h"

#include <cstring>

#include "../psetggm/AES.h"

void disco_gcaes_prf_eval_blocks(
    const std::uint8_t* input_blocks,
    std::size_t block_count,
    std::uint8_t* output_blocks
) {
    for (std::size_t i = 0; i < block_count; ++i) {
        const auto input = toBlock(input_blocks + i * 16);
        block output;
        mAesFixedKey.encryptECB_MMO(input, output);
        std::memcpy(output_blocks + i * 16, &output, 16);
    }
}
