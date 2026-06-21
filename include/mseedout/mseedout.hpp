/**
 * @file mseedout.hpp
 * @brief Public API for the mseedout library.
 * @author mseedout contributors
 * @date 2026
 *
 * This header defines the main entry points for the miniSEED recompression
 * utility. It interfaces cleanly with libmseed.
 */

#ifndef MSEEDOUT_HPP
#define MSEEDOUT_HPP

#include <filesystem>

namespace mseedout {

/**
 * @brief Recompresses a miniSEED file mathematically optimally.
 *
 * This function parses the input miniSEED file using libmseed, extracts
 * all samples and metadata perfectly, and repacks the data using a DP algorithm
 * for Steim-2 packing and an exhaustive search for the optimal record padding.
 * It strictly preserves all structural metadata, format versions, and blockettes.
 *
 * @param input_path Path to the input miniSEED file.
 * @param output_path Path where the optimized miniSEED file will be saved.
 * @return true if successful, false otherwise.
 */
bool recompress_mseed(const std::filesystem::path& input_path, const std::filesystem::path& output_path);

} // namespace mseedout

#endif // MSEEDOUT_HPP
