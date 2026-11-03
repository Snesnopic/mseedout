#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#endif

#include "mseedout/processor.hpp"
#include "mseedout/dp_packer.hpp"
#include "mseedout/mseedout.hpp"
#include <libmseed.h>
#include <iostream>
#include <fstream>
#include <cstring>
#include <filesystem>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace mseedout {

namespace {

// header fields every output record of a run shares; flags and extra headers only exist in miniSEED 3
struct RecordMeta {
    uint8_t version{2};
    uint8_t flags{0};
    std::string extra;
};

MS3Record* make_record_template(const MS3TraceID* id, const MS3TraceSeg* seg, const RecordMeta& meta) {
    MS3Record* msr = msr3_init(nullptr);
    if (!msr) return nullptr;
    strncpy(msr->sid, id->sid, sizeof(msr->sid) - 1);
    msr->samprate = seg->samprate;
    msr->formatversion = meta.version; // output keeps the input format version
    msr->pubversion = id->pubversion;
    if (meta.version == 3) {
        msr->flags = meta.flags;
        if (!meta.extra.empty()) {
            msr->extra = const_cast<char*>(meta.extra.data());
            msr->extralength = static_cast<uint16_t>(meta.extra.size());
        }
    }
    return msr;
}

// the samples belong to the trace list and the extra headers to the RecordMeta
void free_record_template(MS3Record*& msr) {
    msr->datasamples = nullptr;
    msr->extra = nullptr;
    msr->extralength = 0;
    msr3_free(&msr);
}

struct Encoded {
    std::vector<char> bytes;
    int records{-1};
    std::string layout;
};

bool better(const Encoded& candidate, const Encoded& best) {
    if (candidate.records < 0) return false;
    if (best.records < 0 || candidate.bytes.size() < best.bytes.size()) return true;
    return candidate.bytes.size() == best.bytes.size() && candidate.records < best.records; // e.g. do not split a log line
}

std::string reclen_layout(const std::vector<int>& reclens) {
    if (reclens.empty()) return "";
    if (std::all_of(reclens.begin(), reclens.end(), [&](const int r) { return r == reclens.front(); })) return "record length " + std::to_string(reclens.front());
    return "mixed record lengths";
}

Encoded encode_steim2(const MS3TraceID* id, const MS3TraceSeg* seg, const bool mixed) {
    Encoded enc;
    const auto* samples = static_cast<const int32_t*>(seg->datasamples);
    const auto records = DPPacker::pack_steim2_smallest(samples, static_cast<std::size_t>(seg->numsamples), mixed);

    MS3Record* msr = make_record_template(id, seg, RecordMeta{});
    if (!msr) return enc;
    msr->sampletype = 'i';
    msr->encoding = DE_STEIM2;

    std::vector<int> reclens;
    int64_t offset = 0;
    for (const PackedRecord& rec : records) {
        const std::size_t base = enc.bytes.size();
        enc.bytes.resize(base + static_cast<std::size_t>(rec.reclen), 0);
        msr->reclen = rec.reclen;
        msr->starttime = ms_sampletime(seg->starttime, offset, seg->samprate);
        msr->samplecnt = rec.sample_count;
        msr->numsamples = rec.sample_count;

        const int header_len = msr3_pack_header2(msr, &enc.bytes[base], static_cast<uint32_t>(rec.reclen), 0);
        if (header_len < 0 || header_len > DPPacker::HEADER_BYTES) {
            std::cerr << "Failed to pack header for " << id->sid << "\n";
            free_record_template(msr);
            enc = Encoded{};
            return enc;
        }

        const uint16_t dataoffset = htons(DPPacker::HEADER_BYTES);
        memcpy(&enc.bytes[base + 44], &dataoffset, 2);
        const uint16_t numsamples = htons(static_cast<uint16_t>(rec.sample_count));
        memcpy(&enc.bytes[base + 30], &numsamples, 2);
        enc.bytes[base + 52] = DE_STEIM2; // blockette 1000 encoding
        memcpy(&enc.bytes[base + DPPacker::HEADER_BYTES], rec.payload.data(), rec.payload.size());

        reclens.push_back(rec.reclen);
        offset += rec.sample_count;
    }
    free_record_template(msr);
    enc.records = static_cast<int>(records.size());
    enc.layout = "Steim-2, " + reclen_layout(reclens);
    return enc;
}

void append_record(char* record, int reclen, void* handlerdata) {
    auto* sink = static_cast<std::vector<char>*>(handlerdata);
    sink->insert(sink->end(), record, record + reclen);
}

// samples [offset, offset + count) packed by libmseed's own encoder; without flush only complete records are packed
Encoded pack_libmseed(const MS3TraceID* id, const MS3TraceSeg* seg, const RecordMeta& meta, const int16_t encoding, const int reclen,
                      const int64_t offset, const int64_t count, const bool flush, int64_t& packed) {
    Encoded enc;
    packed = 0;
    MS3Record* msr = make_record_template(id, seg, meta);
    if (!msr) return enc;
    msr->sampletype = seg->sampletype;
    msr->encoding = encoding;
    msr->reclen = reclen;
    msr->starttime = ms_sampletime(seg->starttime, offset, seg->samprate);
    msr->datasamples = static_cast<char*>(seg->datasamples) + (offset * ms_samplesize(seg->sampletype));
    msr->numsamples = count;
    msr->samplecnt = count;
    const uint32_t flags = static_cast<uint32_t>(flush ? MSF_FLUSHDATA : 0) | static_cast<uint32_t>(meta.version == 2 ? MSF_PACKVER2 : 0);
    enc.records = msr3_pack(msr, append_record, &enc.bytes, &packed, flags, 0);
    free_record_template(msr);
    return enc;
}

// fixed-size encodings: best uniform record length, or full records of one length plus a shorter tail
Encoded encode_libmseed(const MS3TraceID* id, const MS3TraceSeg* seg, const int16_t encoding, const std::string& name, const bool mixed) {
    const int64_t n = seg->numsamples;
    Encoded best;
    for (int r = DPPacker::MIN_RECLEN; r <= DPPacker::MAX_RECLEN; r *= 2) {
        int64_t packed = 0;
        Encoded enc = pack_libmseed(id, seg, RecordMeta{}, encoding, r, 0, n, true, packed);
        if (packed != n) continue;
        enc.layout = name + ", record length " + std::to_string(r);
        if (better(enc, best)) best = std::move(enc);
    }
    if (!mixed) return best;
    for (int r = DPPacker::MIN_RECLEN * 2; r <= DPPacker::MAX_RECLEN; r *= 2) {
        int64_t bulk_packed = 0;
        const Encoded bulk = pack_libmseed(id, seg, RecordMeta{}, encoding, r, 0, n, false, bulk_packed);
        if (bulk.records <= 0 || bulk_packed <= 0 || bulk_packed >= n) continue;
        for (int t = DPPacker::MIN_RECLEN; t < r; t *= 2) {
            int64_t tail_packed = 0;
            Encoded tail = pack_libmseed(id, seg, RecordMeta{}, encoding, t, bulk_packed, n - bulk_packed, true, tail_packed);
            if (tail.records < 0 || tail_packed != n - bulk_packed) continue;
            Encoded enc;
            enc.bytes = bulk.bytes;
            enc.bytes.insert(enc.bytes.end(), tail.bytes.begin(), tail.bytes.end());
            enc.records = bulk.records + tail.records;
            enc.layout = name + ", mixed record lengths";
            if (better(enc, best)) best = std::move(enc);
        }
    }
    return best;
}

Encoded encode_segment(const MS3TraceID* id, const MS3TraceSeg* seg, const bool mixed) {
    switch (seg->sampletype) {
        case 'f': return encode_libmseed(id, seg, DE_FLOAT32, "FLOAT32", mixed);
        case 'd': return encode_libmseed(id, seg, DE_FLOAT64, "FLOAT64", mixed);
        case 't': return encode_libmseed(id, seg, DE_TEXT, "TEXT", mixed);
        case 'i': break;
        default: return {};
    }
    Encoded best = encode_steim2(id, seg, mixed);
    // high-entropy channels can be smaller as plain integers, which cost at least 2 or 4 bytes per sample
    const auto n = static_cast<std::size_t>(seg->numsamples);
    const auto* samples = static_cast<const int32_t*>(seg->datasamples);
    if (best.records < 0 || best.bytes.size() > 2 * n) {
        const bool fits16 = std::all_of(samples, samples + n, [](const int32_t v) { return v >= -32768 && v <= 32767; });
        if (fits16) {
            Encoded enc = encode_libmseed(id, seg, DE_INT16, "INT16", mixed);
            if (better(enc, best)) best = std::move(enc);
        }
    }
    if (best.records < 0 || best.bytes.size() > 4 * n) {
        Encoded enc = encode_libmseed(id, seg, DE_INT32, "INT32", mixed);
        if (better(enc, best)) best = std::move(enc);
    }
    return best;
}

void put_le32(char* p, const uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<char>((v >> (8U * static_cast<unsigned>(i))) & 0xFFU);
}

// miniSEED 3 records hold exactly their data, so Steim-2 records are cut as large as allowed and as short as needed
Encoded encode_steim2_v3(const MS3TraceID* id, const MS3TraceSeg* seg, const RecordMeta& meta, const int64_t start, const int64_t count) {
    Encoded enc;
    const std::size_t header = 40 + std::strlen(id->sid) + meta.extra.size();
    const int frames = static_cast<int>((static_cast<std::size_t>(DPPacker::MAX_RECLEN) - header) / 64);
    const auto* samples = static_cast<const int32_t*>(seg->datasamples) + start;
    const auto records = DPPacker::pack_steim2_variable(samples, static_cast<std::size_t>(count), frames);

    MS3Record* msr = make_record_template(id, seg, meta);
    if (!msr) return enc;
    msr->sampletype = 'i';
    msr->encoding = DE_STEIM2;

    std::vector<uint64_t> scratch; // libmseed stores header fields through typed pointers, so build each record aligned
    int64_t offset = start;
    for (const PackedRecord& rec : records) {
        const std::size_t total = header + rec.payload.size();
        scratch.assign((total + 7) / 8, 0);
        char* record = reinterpret_cast<char*>(scratch.data());
        msr->reclen = static_cast<int32_t>(total);
        msr->starttime = ms_sampletime(seg->starttime, offset, seg->samprate);
        msr->samplecnt = rec.sample_count;

        if (msr3_pack_header3(msr, record, static_cast<uint32_t>(total), 0) != static_cast<int>(header)) {
            std::cerr << "Failed to pack header for " << id->sid << "\n";
            free_record_template(msr);
            enc = Encoded{};
            return enc;
        }
        put_le32(record + 24, static_cast<uint32_t>(rec.sample_count));
        put_le32(record + 36, static_cast<uint32_t>(rec.payload.size()));
        memcpy(record + header, rec.payload.data(), rec.payload.size());
        put_le32(record + 28, 0);
        put_le32(record + 28, ms_crc32c(reinterpret_cast<const uint8_t*>(record), static_cast<int>(total), 0));
        enc.bytes.insert(enc.bytes.end(), record, record + total);
        offset += rec.sample_count;
    }
    free_record_template(msr);
    enc.records = static_cast<int>(records.size());
    enc.layout = "Steim-2";
    return enc;
}

Encoded encode_run_v3(const MS3TraceID* id, const MS3TraceSeg* seg, const RecordMeta& meta, const int64_t start, const int64_t count) {
    int64_t packed = 0;
    auto native = [&](const int16_t encoding, const char* name) {
        Encoded enc = pack_libmseed(id, seg, meta, encoding, DPPacker::MAX_RECLEN, start, count, true, packed);
        if (packed != count) enc = Encoded{};
        enc.layout = name;
        return enc;
    };
    Encoded best;
    switch (seg->sampletype) {
        case 'f': best = native(DE_FLOAT32, "FLOAT32"); return best;
        case 'd': best = native(DE_FLOAT64, "FLOAT64"); return best;
        case 't': best = native(DE_TEXT, "TEXT"); return best;
        case 'i': break;
        default: return best;
    }
    best = encode_steim2_v3(id, seg, meta, start, count);
    const auto n = static_cast<std::size_t>(count);
    const auto* samples = static_cast<const int32_t*>(seg->datasamples) + start;
    if (best.records < 0 || best.bytes.size() > 2 * n) {
        if (std::all_of(samples, samples + n, [](const int32_t v) { return v >= -32768 && v <= 32767; })) {
            Encoded enc = native(DE_INT16, "INT16");
            if (better(enc, best)) best = std::move(enc);
        }
    }
    if (best.records < 0 || best.bytes.size() > 4 * n) {
        Encoded enc = native(DE_INT32, "INT32");
        if (better(enc, best)) best = std::move(enc);
    }
    return best;
}

struct Run {
    int64_t start{0};
    RecordMeta meta;
};

// consecutive input records with identical flags and extra headers form a run; output records never cross runs
bool record_runs(const MS3TraceSeg* seg, std::vector<Run>& runs) {
    runs.clear();
    if (!seg->recordlist || !seg->recordlist->first) {
        runs.push_back({0, RecordMeta{3, 0, {}}});
        return true;
    }
    std::vector<std::pair<int64_t, const MS3Record*>> records;
    int64_t cumulative = 0;
    for (const MS3RecordPtr* r = seg->recordlist->first; r != nullptr; r = r->next) {
        const double elapsed = static_cast<double>(r->msr->starttime - seg->starttime) / NSTMODULUS;
        const int64_t offset = seg->samprate > 0.0 ? std::llround(elapsed * seg->samprate) : cumulative;
        cumulative += r->msr->samplecnt;
        records.emplace_back(offset, r->msr);
    }
    std::stable_sort(records.begin(), records.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
    for (const auto& [offset, msr] : records) {
        RecordMeta meta{3, msr->flags, (msr->extra && msr->extralength) ? std::string(msr->extra, msr->extralength) : std::string()};
        if (runs.empty()) {
            runs.push_back({0, std::move(meta)});
        } else if (meta.flags != runs.back().meta.flags || meta.extra != runs.back().meta.extra) {
            if (offset <= runs.back().start || offset >= seg->numsamples) return false;
            runs.push_back({offset, std::move(meta)});
        }
    }
    return true;
}

Encoded encode_segment_v3(const MS3TraceID* id, const MS3TraceSeg* seg) {
    Encoded total;
    std::vector<Run> runs;
    if (!record_runs(seg, runs)) return total;
    total.records = 0;
    for (std::size_t i = 0; i < runs.size(); ++i) {
        const int64_t end = (i + 1 < runs.size()) ? runs[i + 1].start : seg->numsamples;
        const Encoded enc = encode_run_v3(id, seg, runs[i].meta, runs[i].start, end - runs[i].start);
        if (enc.records < 0) {
            total = Encoded{};
            return total;
        }
        total.bytes.insert(total.bytes.end(), enc.bytes.begin(), enc.bytes.end());
        total.records += enc.records;
        if (total.layout.find(enc.layout) == std::string::npos) total.layout += (total.layout.empty() ? "" : "+") + enc.layout;
    }
    total.layout += ", miniSEED 3, " + std::to_string(runs.size()) + (runs.size() == 1 ? " metadata run" : " metadata runs");
    return total;
}

struct InputScan {
    bool v2{false};
    bool v3{false};
    std::vector<char> sampleless; // records without samples (detections, timing or log headers), kept verbatim
};

bool scan_input(const std::filesystem::path& input_path, InputScan& scan) {
    MS3FileParam* msfp = nullptr;
    MS3Record* msr = nullptr;
    const std::string path = input_path.string();
    int rv = MS_NOERROR;
    bool ok = true;
    while ((rv = ms3_readmsr_r(&msfp, &msr, path.c_str(), 0, 0)) == MS_NOERROR) {
        (msr->formatversion == 3 ? scan.v3 : scan.v2) = true;
        if (msr->samplecnt != 0) continue;
        if (!msr->record) {
            std::cerr << "Raw record unavailable for " << msr->sid << "\n";
            ok = false;
            break;
        }
        scan.sampleless.insert(scan.sampleless.end(), msr->record, msr->record + msr->reclen);
    }
    if (ok && rv != MS_ENDOFFILE) {
        std::cerr << "Failed to read input records.\n";
        ok = false;
    }
    ms3_readmsr_r(&msfp, &msr, nullptr, 0, 0);
    if (ok && scan.v2 && scan.v3) {
        std::cerr << "Input mixes miniSEED 2 and 3 records.\n";
        ok = false;
    }
    return ok;
}

} // namespace

bool Processor::process(const std::filesystem::path& input_path, const std::filesystem::path& output_path, bool verbose, bool mixed_reclen) {
    if (verbose) std::cout << "Starting optimization for " << input_path << "\n";

    InputScan scan;
    if (!scan_input(input_path, scan)) return false;
    const bool v3 = scan.v3;

    MS3TraceList* mstl = nullptr;
    const uint32_t read_flags = static_cast<uint32_t>(MSF_UNPACKDATA) | static_cast<uint32_t>(v3 ? MSF_RECORDLIST : 0);
    const int ret = ms3_readtracelist(&mstl, input_path.string().c_str(), nullptr, 0, read_flags, 0);

    if (ret != MS_NOERROR || !mstl) {
        std::cerr << "Failed to read input trace list.\n";
        return false;
    }

    std::ofstream outfile(output_path, std::ios::binary);
    if (!outfile) {
        std::cerr << "Failed to open output file.\n";
        mstl3_free(&mstl, 0);
        return false;
    }

    bool ok = true;
    for (const MS3TraceID* id = mstl->traces.next[0]; ok && id != nullptr; id = id->next[0]) {
        for (const MS3TraceSeg* seg = id->first; ok && seg != nullptr; seg = seg->next) {
            if (seg->samplecnt <= 0) continue;
            if (seg->numsamples != seg->samplecnt) {
                std::cerr << "Incomplete decode of " << id->sid << "\n";
                ok = false;
            } else {
                const Encoded enc = v3 ? encode_segment_v3(id, seg) : encode_segment(id, seg, mixed_reclen);
                if (enc.records < 0) {
                    std::cerr << "Failed to encode " << id->sid << " (sample type '" << seg->sampletype << "')\n";
                    ok = false;
                } else {
                    outfile.write(enc.bytes.data(), static_cast<std::streamsize>(enc.bytes.size()));
                    if (verbose) std::cout << id->sid << ": " << seg->numsamples << " samples, " << enc.records << " records, " << enc.bytes.size() << " bytes (" << enc.layout << ")\n";
                }
            }
        }
    }

    mstl3_free(&mstl, 0);
    if (ok) outfile.write(scan.sampleless.data(), static_cast<std::streamsize>(scan.sampleless.size()));
    outfile.close();
    if (!outfile) {
        std::cerr << "Failed to write output file.\n";
        ok = false;
    }
    return ok;
}

} // namespace mseedout
