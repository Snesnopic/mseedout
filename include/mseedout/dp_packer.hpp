/**
 * @file dp_packer.hpp
 * @brief Dynamic Programming core for optimal Steim-2 packing.
 */

#ifndef MSEEDOUT_DP_PACKER_HPP
#define MSEEDOUT_DP_PACKER_HPP

#include <vector>
#include <cstdint>

namespace mseedout {

struct PackedRecord {
    std::vector<uint8_t> payload; // Exact multiple of 64 bytes (Steim-2 frames)
    int sample_count{0};          // Number of samples stored in this record
};

/**
 * @brief Steim-2 Dynamic Programming packer for miniSEED records.
 */
class DPPacker {
public:
    DPPacker() = default;

    /**
     * @brief Computes the optimal Steim-2 packing for a specific record length.
     * 
     * @param samples Pointer to the array of 32-bit integer samples.
     * @param count Number of samples.
     * @param max_frames_per_record Maximum number of 64-byte frames that fit in the payload.
     * @return A vector of PackedRecords, each containing the formatted Steim-2 frames.
     */
    static std::vector<PackedRecord> pack_steim2(const int32_t* samples, size_t count, int max_frames_per_record);

    /**
     * @brief Evaluates the total number of records needed for a specific W_max without building payload.
     */
    static int evaluate_cost(const int32_t* samples, size_t count, int max_frames_per_record);

private:
    static void precompute_bit_widths(const int32_t* samples, size_t count, std::vector<uint8_t>& bit_widths);
    
    struct State {
        int c;        // cost (number of records)
        int prev_i;   // backtracking: previous sample index
        int prev_r_w; // backtracking: previous r_w
        int k;        // backtracking: number of samples packed (0 if padded)
    };
    
    // Internal generic DP runner. 
    // Returns the full DP matrix so backtracking can be performed if requested.
    // If backtrack is false, it can free memory or just return the minimum cost.
    static std::vector<std::vector<State>> run_dp(const int32_t* samples, size_t count, int W_max, const std::vector<uint8_t>& bit_widths);
};

} // namespace mseedout

#endif // MSEEDOUT_DP_PACKER_HPP
