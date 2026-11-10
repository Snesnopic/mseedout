#include <libmseed.h>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <tuple>
#include <vector>

// lossless check: every sample keeps its value, its time (record start + i / rate, per record) and the header
// fields of the record that holds it; records without samples must match one to one

namespace {

struct Meta {
    std::string sid;
    int pubversion{0};
    int version{0};
    int flags{0};
    std::string extra;
    double rate{0.0};
    char type{0};
    bool operator<(const Meta& o) const {
        return std::tie(sid, pubversion, version, flags, extra, rate, type) < std::tie(o.sid, o.pubversion, o.version, o.flags, o.extra, o.rate, o.type);
    }
};

struct Sample {
    uint32_t meta;
    int64_t time;
    uint64_t value;
    bool operator<(const Sample& o) const { return std::tie(meta, time, value) < std::tie(o.meta, o.time, o.value); }
    bool operator==(const Sample& o) const { return meta == o.meta && time == o.time && value == o.value; }
};

struct Contents {
    std::vector<Sample> samples;
    std::vector<Sample> empty_records; // value unused
    std::size_t records{0};
};

// header fields shared by both files, so samples compare by a small id
struct MetaTable {
    std::map<Meta, uint32_t> ids;
    std::vector<Meta> metas;

    uint32_t id(const MS3Record* msr) {
        Meta m{msr->sid, msr->pubversion, msr->formatversion, msr->flags,
               std::string(msr->extra ? msr->extra : "", msr->extra ? msr->extralength : 0), msr3_sampratehz(msr), msr->sampletype};
        const auto [it, inserted] = ids.emplace(m, static_cast<uint32_t>(metas.size()));
        if (inserted) metas.push_back(m);
        return it->second;
    }
};

bool load(const std::filesystem::path& path, MetaTable& table, Contents& out) {
    MS3FileParam* msfp = nullptr;
    MS3Record* msr = nullptr;
    const std::string p = path.string();
    int rv = MS_NOERROR;
    while ((rv = ms3_readmsr_r(&msfp, &msr, p.c_str(), static_cast<uint32_t>(MSF_UNPACKDATA) | static_cast<uint32_t>(MSF_VALIDATECRC), 0)) == MS_NOERROR) {
        ++out.records;
        const uint32_t id = table.id(msr);
        if (msr->samplecnt == 0) {
            out.empty_records.push_back({id, msr->starttime, 0});
            continue;
        }
        if (msr->numsamples != msr->samplecnt) {
            std::cerr << path << ": incomplete decode of " << msr->sid << "\n";
            ms3_readmsr_r(&msfp, &msr, nullptr, 0, 0);
            return false;
        }
        const double rate = msr3_sampratehz(msr);
        const auto* data = static_cast<const char*>(msr->datasamples);
        for (int64_t i = 0; i < msr->numsamples; ++i) {
            uint64_t value = 0;
            switch (msr->sampletype) {
                case 'd': memcpy(&value, data + (i * 8), 8); break;
                case 't': value = static_cast<uint8_t>(data[i]); break;
                default: {
                    uint32_t w = 0;
                    memcpy(&w, data + (i * 4), 4);
                    value = w;
                }
            }
            out.samples.push_back({id, ms_sampletime(msr->starttime, i, rate), value});
        }
    }
    ms3_readmsr_r(&msfp, &msr, nullptr, 0, 0);
    if (rv != MS_ENDOFFILE) {
        std::cerr << "Failed to read " << path << ": " << ms_errorstr(rv) << "\n";
        return false;
    }
    std::sort(out.samples.begin(), out.samples.end());
    std::sort(out.empty_records.begin(), out.empty_records.end());
    return true;
}

void describe(const MetaTable& table, const char* label, const Sample& s) {
    const Meta& m = table.metas[s.meta];
    char time[40];
    ms_nstime2timestr_n(s.time, time, sizeof(time), ISOMONTHDAY_Z, NANO);
    std::cerr << "  " << label << ": " << m.sid << " pubversion=" << m.pubversion << " version=" << m.version << " flags=" << m.flags
              << " rate=" << m.rate << " type=" << m.type << " time=" << time << " value=" << s.value << " extra=" << (m.extra.empty() ? "{}" : m.extra) << "\n";
}

bool compare(const MetaTable& table, const char* what, const std::vector<Sample>& a, const std::vector<Sample>& b) {
    if (a == b) return true;
    const auto [ia, ib] = std::mismatch(a.begin(), a.end(), b.begin(), b.end());
    std::cerr << what << " differ (" << a.size() << " vs " << b.size() << "), first difference:\n";
    if (ia != a.end()) describe(table, "file1", *ia);
    if (ib != b.end()) describe(table, "file2", *ib);
    return false;
}

int run(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <file1.mseed> <file2.mseed>\n";
        std::cerr << "Checks that both files hold the same samples, sample times and header fields.\n";
        return EXIT_FAILURE;
    }

    MetaTable table;
    Contents a, b;
    if (!load(argv[1], table, a) || !load(argv[2], table, b)) return EXIT_FAILURE;

    const bool same_samples = compare(table, "Samples", a.samples, b.samples);
    const bool same_empty = compare(table, "Records without samples", a.empty_records, b.empty_records);
    if (!same_samples || !same_empty) return EXIT_FAILURE;

    std::cout << "SUCCESS! Both files contain exactly the same " << a.samples.size() << " samples, with identical times and headers ("
              << a.records << " vs " << b.records << " records).\n";
    return EXIT_SUCCESS;
}

} // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return EXIT_FAILURE;
    }
}
