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
 * @brief Optimally recompresses a miniSEED file.
 *
 * This function parses the input miniSEED file using libmseed, extracts
 * all samples and metadata, and repacks the data using a DP algorithm
 * for Steim-2 packing and an exhaustive search for the optimal record padding.
 * It preserves all structural metadata, format versions and blockettes.
 *
 * @param input_path Path to the input miniSEED file.
 * @param output_path Path where the optimized miniSEED file will be saved.
 * @param verbose If true, prints progress info to stdout. Warnings and
 *        errors are always printed to stderr regardless of this flag.
 * @param mixed_reclen If true, each record may use its own record length.
 * @return true if successful, false otherwise.
 */
bool recompress_mseed(const std::filesystem::path& input_path, const std::filesystem::path& output_path, bool verbose = false, bool mixed_reclen = false);

} // namespace mseedout

#endif // MSEEDOUT_HPP
