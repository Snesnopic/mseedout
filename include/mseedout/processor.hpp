#ifndef MSEEDOUT_PROCESSOR_HPP
#define MSEEDOUT_PROCESSOR_HPP

#include <filesystem>

namespace mseedout {

/**
 * @brief Processor class for handling the miniSEED recompression.
 */
class Processor {
public:
    Processor() = default;

    /**
     * @brief Processes a miniSEED file, running the DP optimization.
     * 
     * @param input_path Source file path.
     * @param output_path Target file path.
     * @return true if successful.
     */
    static bool process(const std::filesystem::path& input_path, const std::filesystem::path& output_path);
};

} // namespace mseedout

#endif // MSEEDOUT_PROCESSOR_HPP
