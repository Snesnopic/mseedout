#include <iostream>
#include <string>
#include "mseedout/mseedout.hpp"

static void print_usage(const char* prog_name) {
    std::cerr << "Usage: " << prog_name << " <input_file.mseed> <output_file.mseed>\n";
    std::cerr << "Mathematically optimal lossless compression of miniSEED files.\n";
}

int main(int argc, char** argv) {
    if (argc < 3) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    try {
        if (mseedout::recompress_mseed(argv[1], argv[2])) {
            std::cout << "Successfully compressed " << argv[1] << " to " << argv[2] << "\n";
            return EXIT_SUCCESS;
        } else {
            std::cerr << "Failed to compress " << argv[1] << "\n";
            return EXIT_FAILURE;
        }
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return EXIT_FAILURE;
    }
}
