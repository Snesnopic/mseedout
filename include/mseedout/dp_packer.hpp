#ifndef MSEEDOUT_DP_PACKER_HPP
#define MSEEDOUT_DP_PACKER_HPP

#include <vector>
#include <cstdint>
#include <cstddef>

namespace mseedout {

struct PackedRecord {
    std::vector<uint8_t> payload; // Steim-2 frames, reclen - HEADER_BYTES bytes (only the frames used if reclen is 0)
    int sample_count{0};
    int reclen{0}; // 0 for variable-length records (miniSEED 3)
};

// minimum-size Steim-2 packer: greedy words and records are optimal since a sub-window of a valid word is valid
class DPPacker {
public:
    static constexpr int HEADER_BYTES = 64;
    static constexpr int MIN_RECLEN = 256;
    static constexpr int MAX_RECLEN = 131072;
    static constexpr int MAX_SAMPLES_PER_RECORD = 65535; // 16-bit numsamples field of miniSEED 2

    DPPacker() = default;

    // every record uses (max_frames_per_record + 1) * 64 bytes
    static std::vector<PackedRecord> pack_steim2(const int32_t* samples, std::size_t count, int max_frames_per_record);

    // number of records pack_steim2 would produce
    static int evaluate_cost(const int32_t* samples, std::size_t count, int max_frames_per_record);

    // smallest output over all power-of-two record lengths; with mixed, each record picks its own length
    static std::vector<PackedRecord> pack_steim2_smallest(const int32_t* samples, std::size_t count, bool mixed);

    // miniSEED 3: records as short as their data, at most max_frames_per_record frames, no sample limit
    static std::vector<PackedRecord> pack_steim2_variable(const int32_t* samples, std::size_t count, int max_frames_per_record);
};

} // namespace mseedout

#endif // MSEEDOUT_DP_PACKER_HPP
