#include <iostream>
#include <string>
#include "mseedout/mseedout.hpp"

static void print_usage(const char* prog_name) {
    std::cerr << "Usage: " << prog_name << " [-v|--verbose] <input_file.mseed> <output_file.mseed>\n";
    std::cerr << "Optimal lossless compression of miniSEED files.\n";
}

int main(int argc, char** argv) {
    bool verbose = false;
    std::string input_path, output_path;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--version") {
            std::cout << MSEEDOUT_VERSION << "\n";
            return EXIT_SUCCESS;
        } else if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return EXIT_SUCCESS;
        } else if (arg == "-v" || arg == "--verbose") {
            verbose = true;
        } else if (input_path.empty()) {
            input_path = arg;
        } else if (output_path.empty()) {
            output_path = arg;
        } else {
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    if (input_path.empty() || output_path.empty()) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    try {
        if (mseedout::recompress_mseed(input_path, output_path, verbose)) {
            if (verbose) std::cout << "Successfully compressed " << input_path << " to " << output_path << "\n";
            return EXIT_SUCCESS;
        } else {
            std::cerr << "Failed to compress " << input_path << "\n";
            return EXIT_FAILURE;
        }
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return EXIT_FAILURE;
    }
}
