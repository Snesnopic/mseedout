#include <libmseed.h>
#include <iostream>
#include <vector>
#include <string>
#include <filesystem>

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <file1.mseed> <file2.mseed>\n";
        return EXIT_FAILURE;
    }

    const std::filesystem::path file1 = argv[1];
    const std::filesystem::path file2 = argv[2];

    MS3TraceList* mstl1 = nullptr;
    MS3TraceList* mstl2 = nullptr;

    if (ms3_readtracelist(&mstl1, file1.string().c_str(), nullptr, 0, MSF_UNPACKDATA, 3) != MS_NOERROR) {
        std::cerr << "Failed to read " << file1 << "\n";
        return EXIT_FAILURE;
    }
    if (ms3_readtracelist(&mstl2, file2.string().c_str(), nullptr, 0, MSF_UNPACKDATA, 3) != MS_NOERROR) {
        std::cerr << "Failed to read " << file2 << "\n";
        return EXIT_FAILURE;
    }

    // Extract all samples from both
    std::vector<int32_t> samples1;
    std::vector<int32_t> samples2;

    if (mstl1->traces.next[0]) {
        for (MS3TraceID *id = mstl1->traces.next[0]; id != nullptr; id = id->next[0]) {
            for (MS3TraceSeg *seg = id->first; seg != nullptr; seg = seg->next) {
                if (seg->samplecnt > 0 && seg->sampletype == 'i') {
                    const int32_t* s = static_cast<int32_t*>(seg->datasamples);
                    samples1.insert(samples1.end(), s, s + seg->samplecnt);
                }
            }
        }
    }

    if (mstl2->traces.next[0]) {
        for (MS3TraceID *id = mstl2->traces.next[0]; id != nullptr; id = id->next[0]) {
            for (MS3TraceSeg *seg = id->first; seg != nullptr; seg = seg->next) {
                std::cout << "File2 Trace: " << id->sid << " seg samples: " << seg->samplecnt << " type: " << seg->sampletype << "\n";
                if (seg->samplecnt > 0 && seg->sampletype == 'i') {
                    const int32_t* s = static_cast<int32_t*>(seg->datasamples);
                    samples2.insert(samples2.end(), s, s + seg->samplecnt);
                }
            }
        }
    }

    if (samples1.size() != samples2.size()) {
        std::cerr << "Sample count mismatch! File1: " << samples1.size() << " vs File2: " << samples2.size() << "\n";
        return EXIT_FAILURE;
    }

    for (size_t i = 0; i < samples1.size(); ++i) {
        if (samples1[i] != samples2[i]) {
            std::cerr << "Sample mismatch at index " << i << "! File1: " << samples1[i] << " vs File2: " << samples2[i] << "\n";
            return EXIT_FAILURE;
        }
    }

    std::cout << "SUCCESS! Both files contain exactly the same " << samples1.size() << " samples.\n";

    mstl3_free(&mstl1, 0);
    mstl3_free(&mstl2, 0);
    return EXIT_SUCCESS;
}
