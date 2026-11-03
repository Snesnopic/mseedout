#include "mseedout/mseedout.hpp"
#include "mseedout/processor.hpp"

namespace mseedout {

bool recompress_mseed(const std::filesystem::path& input_path, const std::filesystem::path& output_path, bool verbose, bool mixed_reclen) {
    return Processor::process(input_path, output_path, verbose, mixed_reclen);
}

} // namespace mseedout
