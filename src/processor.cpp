#include "mseedout/processor.hpp"
#include "mseedout/dp_packer.hpp"
#include "mseedout/mseedout.hpp"
#include <libmseed.h>
#ifdef _WIN32
#undef min // libmseed.h includes windows.h
#undef max
#endif
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <numeric>
#include <optional>
#include <string>
#include <vector>

namespace mseedout {

namespace {

// one decoded input record
struct Rec {
    std::string sid;
    uint8_t version{2};
    uint8_t pubversion{0};
    uint8_t flags{0};
    std::string extra;
    double rate{0.0}; // Hz
    char type{0};
    nstime_t start{0};
    int64_t count{0};
    std::vector<char> samples;
    std::vector<char> raw;
    std::string signature; // miniSEED 2 header bytes and blockette types that must come out unchanged
    bool verbatim{false};  // carries blockettes libmseed cannot write back
    bool mergeable{true};  // false for records carrying events, which keep their own record boundaries
};

uint16_t read_u16(const char* p, const bool big) {
    const auto* u = reinterpret_cast<const uint8_t*>(p);
    const unsigned hi = big ? u[0] : u[1];
    const unsigned lo = big ? u[1] : u[0];
    return static_cast<uint16_t>((hi << 8U) | lo);
}

uint32_t read_u32(const char* p, const bool big) {
    const auto* u = reinterpret_cast<const uint8_t*>(p);
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v = (v << 8U) | u[big ? i : 3 - i];
    return v;
}

void put_be16(char* p, const uint16_t v) {
    p[0] = static_cast<char>(v >> 8U);
    p[1] = static_cast<char>(v & 0xFFU);
}

void put_le32(char* p, const uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<char>((v >> (8U * static_cast<unsigned>(i))) & 0xFFU);
}

bool header_big_endian(const MS3Record* msr) {
    const bool swapped = (static_cast<unsigned>(msr->swapflag) & static_cast<unsigned>(MSSWAP_HEADER)) != 0;
    return ms_bigendianhost() ? !swapped : swapped;
}

// blockettes libmseed both decodes into extra headers and writes back
bool round_trips(const uint16_t type) {
    switch (type) {
        case 100: case 200: case 201: case 300: case 310: case 320: case 390: case 395: case 500: case 1000: case 1001: return true;
        default: return false;
    }
}

bool is_event(const uint16_t type) { return type != 100 && type != 1000 && type != 1001; }

// quality, identifiers, flag bytes, time correction and blockette types: what a rewritten header must reproduce
std::string v2_signature(const char* record, const int reclen, const bool big, bool& unsupported, bool& event) {
    std::string sig;
    sig.push_back(record[6]);
    sig.append(record + 8, 12);
    sig.append(record + 36, 3);
    const uint32_t correction = read_u32(record + 40, big);
    sig.append(reinterpret_cast<const char*>(&correction), 4);
    uint16_t offset = read_u16(record + 46, big);
    for (int guard = 0; offset != 0; ++guard) {
        if (guard > 64 || offset + 4 > reclen) { unsupported = true; break; }
        const uint16_t type = read_u16(record + offset, big);
        unsupported |= !round_trips(type);
        event |= is_event(type);
        if (type != 1000 && type != 1001) {
            sig.push_back(static_cast<char>(type >> 8U));
            sig.push_back(static_cast<char>(type & 0xFFU));
        }
        const uint16_t next = read_u16(record + offset + 2, big);
        if (next != 0 && next <= offset) { unsupported = true; break; }
        offset = next;
    }
    return sig;
}

bool v3_event(const std::string& extra) {
    return extra.find("\"Detection\"") != std::string::npos || extra.find("\"Calibration\"") != std::string::npos ||
           extra.find("\"Exception\"") != std::string::npos;
}

struct Input {
    std::vector<Rec> recs;
    std::vector<char> sampleless; // records without samples, kept verbatim
    bool v2{false};
    bool v3{false};
};

bool read_input(const std::filesystem::path& input_path, Input& in) {
    MS3FileParam* msfp = nullptr;
    MS3Record* msr = nullptr;
    const std::string path = input_path.string();
    int rv = MS_NOERROR;
    bool ok = true;
    while ((rv = ms3_readmsr_r(&msfp, &msr, path.c_str(), MSF_UNPACKDATA, 0)) == MS_NOERROR) {
        (msr->formatversion == 3 ? in.v3 : in.v2) = true;
        if (!msr->record) {
            std::cerr << "Raw record unavailable for " << msr->sid << "\n";
            ok = false;
            break;
        }
        if (msr->samplecnt == 0) {
            in.sampleless.insert(in.sampleless.end(), msr->record, msr->record + msr->reclen);
            continue;
        }
        if (msr->numsamples != msr->samplecnt) {
            std::cerr << "Incomplete decode of " << msr->sid << "\n";
            ok = false;
            break;
        }
        Rec r;
        r.sid = msr->sid;
        r.version = msr->formatversion;
        r.pubversion = msr->pubversion;
        r.flags = msr->flags;
        if (msr->extra && msr->extralength) r.extra.assign(msr->extra, msr->extralength);
        r.rate = msr3_sampratehz(msr);
        r.type = msr->sampletype;
        r.start = msr->starttime;
        r.count = msr->numsamples;
        const auto* data = static_cast<const char*>(msr->datasamples);
        r.samples.assign(data, data + (static_cast<std::size_t>(msr->numsamples) * ms_samplesize(msr->sampletype)));
        r.raw.assign(msr->record, msr->record + msr->reclen);
        bool event = false;
        if (r.version == 2) r.signature = v2_signature(msr->record, msr->reclen, header_big_endian(msr), r.verbatim, event);
        else event = v3_event(r.extra);
        r.mergeable = !r.verbatim && !event;
        in.recs.push_back(std::move(r));
    }
    if (ok && rv != MS_ENDOFFILE) {
        std::cerr << "Failed to read input records.\n";
        ok = false;
    }
    ms3_readmsr_r(&msfp, &msr, nullptr, 0, 0);
    if (ok && in.v2 && in.v3) {
        std::cerr << "Input mixes miniSEED 2 and 3 records.\n";
        ok = false;
    }
    return ok;
}

// exact equality is intended: a rate that changes at all is a different stream
bool same_rate(const double a, const double b) { return std::bit_cast<uint64_t>(a) == std::bit_cast<uint64_t>(b); }

// 0 for series without a sample rate, nullopt when the period is not a whole number of nanoseconds
std::optional<int64_t> period_ns(const double rate) {
    if (rate == 0.0) return 0;
    if (rate < 0.0) return std::nullopt;
    const double p = 1e9 / rate;
    const auto whole = std::llround(p);
    if (whole <= 0 || std::fabs(p - static_cast<double>(whole)) > 1e-6) return std::nullopt;
    return whole;
}

// consecutive records whose samples lie exactly on one time line and whose headers are identical
struct Block {
    std::vector<const Rec*> recs;
    std::vector<int64_t> offsets;
    std::vector<char> samples;
    int64_t count{0};
    std::optional<int64_t> period;
    uint32_t grid{1}; // output records may only start at sample indices divisible by grid

    const Rec& first() const { return *recs.front(); }
    std::size_t sample_size() const { return ms_samplesize(first().type); }
    nstime_t start_of(const int64_t k) const { return first().start + (period ? k * *period : 0); }
    std::size_t verbatim_bytes() const {
        std::size_t n = 0;
        for (const Rec* r : recs) n += r->raw.size();
        return n;
    }
};

bool extends(const Block& b, const Rec& r) {
    const Rec& last = *b.recs.back();
    if (!last.mergeable || !r.mergeable) return false;
    if (last.sid != r.sid || last.pubversion != r.pubversion || last.flags != r.flags || last.extra != r.extra || !same_rate(last.rate, r.rate) ||
        last.type != r.type || last.signature != r.signature)
        return false;
    const auto p = period_ns(r.rate);
    return p && r.start == last.start + (last.count * *p);
}

std::vector<Block> make_blocks(const Input& in) {
    std::vector<const Rec*> sorted;
    sorted.reserve(in.recs.size());
    for (const Rec& r : in.recs) sorted.push_back(&r);
    std::stable_sort(sorted.begin(), sorted.end(), [](const Rec* a, const Rec* b) {
        if (a->sid != b->sid) return a->sid < b->sid;
        if (a->pubversion != b->pubversion) return a->pubversion < b->pubversion;
        return a->start < b->start;
    });

    std::vector<Block> blocks;
    for (const Rec* r : sorted) {
        if (blocks.empty() || !extends(blocks.back(), *r)) blocks.emplace_back();
        Block& b = blocks.back();
        b.offsets.push_back(b.count);
        b.recs.push_back(r);
        b.samples.insert(b.samples.end(), r->samples.begin(), r->samples.end());
        b.count += r->count;
    }
    for (Block& b : blocks) {
        b.period = period_ns(b.first().rate);
        if (!b.period || (b.recs.size() == 1 && b.first().version == 2 && b.first().start % 1000 != 0)) {
            b.grid = static_cast<uint32_t>(std::min<int64_t>(b.count, UINT32_MAX));
        } else if (b.first().version == 2 && *b.period % 1000 != 0) {
            // miniSEED 2 start times have microsecond resolution
            b.grid = static_cast<uint32_t>(1000 / std::gcd<int64_t>(*b.period % 1000, 1000));
        }
    }
    return blocks;
}

// header fields every output record of a block shares
MS3Record* make_template(const Block& b, const int16_t encoding, const char type) {
    MS3Record* msr = msr3_init(nullptr);
    if (!msr) return nullptr;
    const Rec& r = b.first();
    strncpy(msr->sid, r.sid.c_str(), sizeof(msr->sid) - 1);
    msr->formatversion = r.version;
    msr->pubversion = r.pubversion;
    msr->flags = r.flags;
    msr->samprate = r.rate;
    msr->encoding = encoding;
    msr->sampletype = type;
    if (!r.extra.empty()) {
        msr->extra = const_cast<char*>(r.extra.data());
        msr->extralength = static_cast<uint16_t>(r.extra.size());
    }
    return msr;
}

// the extra headers belong to the input record
void free_template(MS3Record*& msr) {
    msr->extra = nullptr;
    msr->extralength = 0;
    msr->datasamples = nullptr;
    msr3_free(&msr);
}

struct Candidate {
    std::vector<char> bytes;
    std::vector<std::size_t> lengths;
    std::string layout;
};

// libmseed stores header fields through typed pointers, so every record is built in aligned scratch space
class RecordWriter {
public:
    RecordWriter(const Block& b, const int16_t encoding, const char type) : block_(b), msr_(make_template(b, encoding, type)) {}
    ~RecordWriter() { if (msr_) free_template(msr_); }
    RecordWriter(const RecordWriter&) = delete;
    RecordWriter& operator=(const RecordWriter&) = delete;

    bool ok() const { return msr_ != nullptr; }

    int v2_header_length() {
        scratch_.assign(4096 / 8, 0);
        msr_->reclen = 4096;
        msr_->starttime = block_.first().start;
        msr_->samplecnt = 1;
        return msr3_pack_header2(msr_, reinterpret_cast<char*>(scratch_.data()), 4096, 0);
    }

    std::size_t v3_header_length() const { return 40 + block_.first().sid.size() + block_.first().extra.size(); }

    bool append_v2(Candidate& c, const int64_t first, const int64_t n, const int reclen, const int dataoffset, const char* payload, const std::size_t len) {
        if (n > DPPacker::MAX_SAMPLES_PER_RECORD || static_cast<std::size_t>(dataoffset) + len > static_cast<std::size_t>(reclen)) return false;
        scratch_.assign((static_cast<std::size_t>(reclen) + 7) / 8, 0);
        char* record = reinterpret_cast<char*>(scratch_.data());
        msr_->reclen = reclen;
        msr_->starttime = block_.start_of(first);
        msr_->samplecnt = n;
        const int header = msr3_pack_header2(msr_, record, static_cast<uint32_t>(reclen), 0);
        if (header < 0 || header > dataoffset) return false;
        put_be16(record + 30, static_cast<uint16_t>(n));
        put_be16(record + 44, static_cast<uint16_t>(dataoffset));
        memcpy(record + dataoffset, payload, len);
        c.bytes.insert(c.bytes.end(), record, record + reclen);
        c.lengths.push_back(static_cast<std::size_t>(reclen));
        return true;
    }

    bool append_v3(Candidate& c, const int64_t first, const int64_t n, const char* payload, const std::size_t len) {
        const std::size_t header = v3_header_length();
        const std::size_t total = header + len;
        scratch_.assign((total + 7) / 8, 0);
        char* record = reinterpret_cast<char*>(scratch_.data());
        msr_->reclen = static_cast<int32_t>(total);
        msr_->starttime = block_.start_of(first);
        msr_->samplecnt = n;
        if (msr3_pack_header3(msr_, record, static_cast<uint32_t>(total), 0) != static_cast<int>(header)) return false;
        put_le32(record + 24, static_cast<uint32_t>(n));
        put_le32(record + 36, static_cast<uint32_t>(len));
        memcpy(record + header, payload, len);
        put_le32(record + 28, 0);
        put_le32(record + 28, ms_crc32c(reinterpret_cast<const uint8_t*>(record), static_cast<int>(total), 0));
        c.bytes.insert(c.bytes.end(), record, record + total);
        c.lengths.push_back(total);
        return true;
    }

private:
    const Block& block_;
    MS3Record* msr_;
    std::vector<uint64_t> scratch_;
};

std::string reclen_layout(const std::vector<int>& reclens) {
    if (std::all_of(reclens.begin(), reclens.end(), [&](const int r) { return r == reclens.front(); })) return "record length " + std::to_string(reclens.front());
    return "mixed record lengths";
}

std::optional<Candidate> encode_steim2(const Block& b, const bool mixed) {
    RecordWriter w(b, DE_STEIM2, 'i');
    if (!w.ok()) return std::nullopt;
    const auto* samples = reinterpret_cast<const int32_t*>(b.samples.data());
    const auto n = static_cast<std::size_t>(b.count);
    Candidate c;
    int64_t k = 0;
    if (b.first().version == 2) {
        const int header = w.v2_header_length();
        if (header < 0) return std::nullopt;
        const int dataoffset = ((header + 63) / 64) * 64; // Steim frames start on a 64-byte boundary
        const auto records = DPPacker::pack_steim2_smallest(samples, n, mixed, dataoffset, b.grid);
        if (records.empty()) return std::nullopt;
        std::vector<int> reclens;
        for (const PackedRecord& rec : records) {
            if (!w.append_v2(c, k, rec.sample_count, rec.reclen, dataoffset, reinterpret_cast<const char*>(rec.payload.data()), rec.payload.size())) return std::nullopt;
            reclens.push_back(rec.reclen);
            k += rec.sample_count;
        }
        c.layout = "Steim-2, " + reclen_layout(reclens);
    } else {
        const int frames = static_cast<int>((static_cast<std::size_t>(DPPacker::MAX_RECLEN) - w.v3_header_length()) / 64);
        const auto records = DPPacker::pack_steim2_variable(samples, n, frames, b.grid);
        if (records.empty()) return std::nullopt;
        for (const PackedRecord& rec : records) {
            if (!w.append_v3(c, k, rec.sample_count, reinterpret_cast<const char*>(rec.payload.data()), rec.payload.size())) return std::nullopt;
            k += rec.sample_count;
        }
        c.layout = "Steim-2";
    }
    return c;
}

// last sample of a record that may hold up to capacity samples from a, so that the next one starts on the grid
std::optional<int64_t> grid_end(const Block& b, const int64_t a, const int64_t capacity) {
    const int64_t e = std::min(a + capacity - 1, b.count - 1);
    if (e + 1 >= b.count) return e;
    const int64_t next = ((e + 1) / b.grid) * b.grid;
    if (next <= a) return std::nullopt;
    return next - 1;
}

// fixed-size encodings: miniSEED 2 big-endian, miniSEED 3 little-endian
std::vector<char> serialize(const Block& b, const int16_t encoding, const int64_t first, const int64_t n) {
    const bool big = b.first().version == 2;
    const int size = encoding == DE_INT16 ? 2 : encoding == DE_TEXT ? 1 : encoding == DE_FLOAT64 ? 8 : 4;
    std::vector<char> out(static_cast<std::size_t>(n * size));
    const char* base = b.samples.data();
    for (int64_t i = 0; i < n; ++i) {
        const auto at = static_cast<std::size_t>(first + i);
        uint64_t v = 0;
        if (b.first().type == 'd') {
            memcpy(&v, base + (at * 8), 8);
        } else if (b.first().type == 't') {
            v = static_cast<uint8_t>(base[at]);
        } else {
            uint32_t w = 0;
            memcpy(&w, base + (at * 4), 4);
            v = encoding == DE_INT16 ? static_cast<uint16_t>(static_cast<int16_t>(static_cast<int32_t>(w))) : w;
        }
        for (int j = 0; j < size; ++j) {
            const int shift = 8 * (big ? size - 1 - j : j);
            out[static_cast<std::size_t>((i * size) + j)] = static_cast<char>((v >> static_cast<unsigned>(shift)) & 0xFFU);
        }
    }
    return out;
}

std::optional<Candidate> encode_fixed(const Block& b, const int16_t encoding, const char* name, const bool mixed) {
    const int size = encoding == DE_INT16 ? 2 : encoding == DE_TEXT ? 1 : encoding == DE_FLOAT64 ? 8 : 4;
    RecordWriter w(b, encoding, b.first().type);
    if (!w.ok()) return std::nullopt;
    struct Span { int64_t a, n; int reclen; };
    std::vector<Span> plan;
    const int v2_header = b.first().version == 2 ? w.v2_header_length() : 0;
    if (v2_header < 0) return std::nullopt;

    if (b.first().version == 3) {
        const auto capacity = static_cast<int64_t>((static_cast<std::size_t>(DPPacker::MAX_RECLEN) - w.v3_header_length()) / static_cast<std::size_t>(size));
        for (int64_t a = 0; a < b.count;) {
            const auto e = grid_end(b, a, capacity);
            if (!e) return std::nullopt;
            plan.push_back({a, *e - a + 1, 0});
            a = *e + 1;
        }
    } else {
        auto capacity = [&](const int reclen) { return std::min<int64_t>(DPPacker::MAX_SAMPLES_PER_RECORD, (reclen - v2_header) / size); };
        std::size_t best_bytes = SIZE_MAX;
        for (int r = DPPacker::MIN_RECLEN; r <= DPPacker::MAX_RECLEN; r *= 2) {
            if (capacity(r) < 1) continue;
            std::vector<Span> p;
            bool feasible = true;
            for (int64_t a = 0; a < b.count && feasible;) {
                const auto e = grid_end(b, a, capacity(r));
                if (!e) { feasible = false; break; }
                p.push_back({a, *e - a + 1, r});
                a = *e + 1;
            }
            if (!feasible) continue;
            if (mixed) {
                // the last record only needs to hold what is left
                for (int t = DPPacker::MIN_RECLEN; t < p.back().reclen; t *= 2) {
                    if (capacity(t) >= p.back().n) { p.back().reclen = t; break; }
                }
            }
            std::size_t bytes = 0;
            for (const Span& s : p) bytes += static_cast<std::size_t>(s.reclen);
            if (bytes < best_bytes) { best_bytes = bytes; plan = std::move(p); }
        }
        if (plan.empty()) return std::nullopt;
    }

    Candidate c;
    std::vector<int> reclens;
    for (const Span& s : plan) {
        const auto payload = serialize(b, encoding, s.a, s.n);
        const bool written = b.first().version == 2 ? w.append_v2(c, s.a, s.n, s.reclen, v2_header, payload.data(), payload.size())
                                                     : w.append_v3(c, s.a, s.n, payload.data(), payload.size());
        if (!written) return std::nullopt;
        reclens.push_back(s.reclen);
    }
    c.layout = std::string(name) + (b.first().version == 2 ? ", " + reclen_layout(reclens) : "");
    return c;
}

// decode the candidate and require every sample value, every sample time and every header field to match the input
bool verify(const Block& b, const Candidate& c) {
    const Rec& ref = b.first();
    const std::size_t size = b.sample_size();
    const uint32_t flags = static_cast<uint32_t>(MSF_UNPACKDATA) | static_cast<uint32_t>(ref.version == 3 ? MSF_VALIDATECRC : 0);
    std::vector<uint64_t> scratch;
    std::size_t pos = 0;
    std::size_t j = 0;
    int64_t k = 0;
    for (const std::size_t len : c.lengths) {
        scratch.assign((len + 7) / 8, 0);
        memcpy(scratch.data(), c.bytes.data() + pos, len);
        pos += len;
        MS3Record* m = nullptr;
        if (msr3_parse(reinterpret_cast<const char*>(scratch.data()), len, &m, flags, 0) != MS_NOERROR || !m) {
            msr3_free(&m);
            return false;
        }
        bool ok = static_cast<std::size_t>(m->reclen) == len && ref.sid == m->sid && m->pubversion == ref.pubversion &&
                  m->formatversion == ref.version && m->flags == ref.flags && same_rate(msr3_sampratehz(m), ref.rate) && m->sampletype == ref.type &&
                  m->numsamples == m->samplecnt && k + m->numsamples <= b.count &&
                  std::string(m->extra ? m->extra : "", m->extra ? m->extralength : 0) == ref.extra &&
                  memcmp(m->datasamples, b.samples.data() + (static_cast<std::size_t>(k) * size), static_cast<std::size_t>(m->numsamples) * size) == 0;
        if (ok && ref.version == 2) {
            bool unsupported = false;
            bool event = false;
            ok = v2_signature(reinterpret_cast<const char*>(scratch.data()), static_cast<int>(len), header_big_endian(m), unsupported, event) == ref.signature && !unsupported;
        }
        for (int64_t i = 0; ok && i < m->numsamples; ++i, ++k) {
            while (j + 1 < b.recs.size() && b.offsets[j + 1] <= k) ++j;
            ok = ms_sampletime(m->starttime, i, ref.rate) == ms_sampletime(b.recs[j]->start, k - b.offsets[j], ref.rate);
        }
        msr3_free(&m);
        if (!ok) return false;
    }
    return k == b.count && pos == c.bytes.size();
}

struct Choice {
    std::vector<char> bytes;
    std::string layout;
    bool verbatim{false};
};

Choice encode_block(const Block& b, const bool mixed) {
    std::vector<Candidate> candidates;
    auto add = [&](std::optional<Candidate> c) { if (c) candidates.push_back(std::move(*c)); };
    const auto n = static_cast<std::size_t>(b.count);
    if (b.first().verbatim) {
        // nothing to try
    } else if (b.first().type == 'i') {
        add(encode_steim2(b, mixed));
        const std::size_t steim = candidates.empty() ? SIZE_MAX : candidates.front().bytes.size();
        // high-entropy channels can be smaller as plain integers, which cost at least 2 or 4 bytes per sample
        const auto* samples = reinterpret_cast<const int32_t*>(b.samples.data());
        if (steim > 2 * n && std::all_of(samples, samples + n, [](const int32_t v) { return v >= -32768 && v <= 32767; })) add(encode_fixed(b, DE_INT16, "INT16", mixed));
        if (steim > 4 * n) add(encode_fixed(b, DE_INT32, "INT32", mixed));
    } else if (b.first().type == 'f') {
        add(encode_fixed(b, DE_FLOAT32, "FLOAT32", mixed));
    } else if (b.first().type == 'd') {
        add(encode_fixed(b, DE_FLOAT64, "FLOAT64", mixed));
    } else if (b.first().type == 't') {
        add(encode_fixed(b, DE_TEXT, "TEXT", mixed));
    }
    std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate& x, const Candidate& y) { return x.bytes.size() < y.bytes.size(); });

    const std::size_t original = b.verbatim_bytes();
    for (Candidate& c : candidates) {
        if (c.bytes.size() >= original) break;
        if (verify(b, c)) return {std::move(c.bytes), std::move(c.layout), false};
    }
    Choice keep;
    for (const Rec* r : b.recs) keep.bytes.insert(keep.bytes.end(), r->raw.begin(), r->raw.end());
    keep.layout = "original records";
    keep.verbatim = true;
    return keep;
}

} // namespace

bool Processor::process(const std::filesystem::path& input_path, const std::filesystem::path& output_path, bool verbose, bool mixed_reclen) {
    if (verbose) std::cout << "Starting optimization for " << input_path << "\n";

    Input in;
    if (!read_input(input_path, in)) return false;
    const std::vector<Block> blocks = make_blocks(in);

    std::ofstream outfile(output_path, std::ios::binary);
    if (!outfile) {
        std::cerr << "Failed to open output file.\n";
        return false;
    }

    struct Summary { int64_t samples{0}; std::size_t blocks{0}, kept{0}, bytes{0}; std::map<std::string, int> layouts; };
    std::map<std::string, Summary> summary;
    for (const Block& b : blocks) {
        const Choice choice = encode_block(b, mixed_reclen);
        outfile.write(choice.bytes.data(), static_cast<std::streamsize>(choice.bytes.size()));
        Summary& s = summary[b.first().sid];
        s.samples += b.count;
        s.blocks += 1;
        s.kept += choice.verbatim ? 1 : 0;
        s.bytes += choice.bytes.size();
        s.layouts[choice.layout] += 1;
    }
    outfile.write(in.sampleless.data(), static_cast<std::streamsize>(in.sampleless.size()));

    if (verbose) {
        for (const auto& [sid, s] : summary) {
            std::cout << sid << ": " << s.samples << " samples in " << s.blocks << (s.blocks == 1 ? " block" : " blocks") << ", " << s.bytes << " bytes (";
            bool first = true;
            for (const auto& [layout, count] : s.layouts) {
                std::cout << (first ? "" : "; ") << layout << (s.blocks > 1 ? " x" + std::to_string(count) : "");
                first = false;
            }
            std::cout << ")\n";
        }
    }

    outfile.close();
    if (!outfile) {
        std::cerr << "Failed to write output file.\n";
        return false;
    }
    return true;
}

} // namespace mseedout
