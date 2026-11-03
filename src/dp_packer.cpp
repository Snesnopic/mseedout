#include "mseedout/dp_packer.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#endif

namespace mseedout {

namespace {

constexpr std::array<uint8_t, 8> MAX_BITS = {0, 30, 15, 10, 8, 6, 5, 4}; // indexed by differences per word
constexpr uint8_t OVERFLOW_BITS = 31;
constexpr uint32_t JUMP_WORDS = 64;
constexpr int NUM_RECLENS = 10; // 256 .. 131072

uint8_t width_class(const int64_t d) {
    if (d >= -8 && d <= 7) return 4;
    if (d >= -16 && d <= 15) return 5;
    if (d >= -32 && d <= 31) return 6;
    if (d >= -128 && d <= 127) return 8;
    if (d >= -512 && d <= 511) return 10;
    if (d >= -16384 && d <= 16383) return 15;
    if (d >= -536870912LL && d <= 536870911LL) return 30;
    return OVERFLOW_BITS; // only representable as the X0 of a new record
}

int words_for_frames(const int frames) { return 13 + ((frames - 1) * 15); }

int frames_for_reclen(const int reclen) { return (reclen - DPPacker::HEADER_BYTES) / 64; }

constexpr std::array<int, NUM_RECLENS> RECLENS = {256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536, 131072};
static_assert(RECLENS.front() == DPPacker::MIN_RECLEN && RECLENS.back() == DPPacker::MAX_RECLEN, "mixed_plan counts in MIN_RECLEN units of 2^i");

int reclen_at(const int i) { return RECLENS[static_cast<std::size_t>(i)]; }

// nodes are sample indices: being at node t means samples up to t are covered by the current record
class Planner {
public:
    Planner(const int32_t* samples, const std::size_t count, const uint32_t max_samples)
        : s_(samples), n_(static_cast<uint32_t>(count)), max_samples_(max_samples) {
        if (count >= UINT32_MAX) throw std::length_error("segment too long");
        bw_.assign(n_, 0);
        for (uint32_t t = 1; t < n_; ++t) bw_[t] = width_class(static_cast<int64_t>(s_[t]) - s_[t - 1]);
        kfar_.assign(n_, 0);
        for (uint32_t t = 0; t + 1 < n_; ++t) {
            uint8_t m = 0;
            uint8_t best = 0;
            for (uint8_t k = 1; k <= 7 && t + k < n_; ++k) {
                m = std::max(m, bw_[t + k]);
                if (m > MAX_BITS[k]) break; // wider window and narrower slots: no larger k can fit either
                best = k;
            }
            kfar_[t] = best;
        }
    }

    uint32_t size() const { return n_; }

    // after the first word of a record starting at a, which spends one slot on the skipped difference
    uint32_t first_node(const uint32_t a) const {
        uint8_t m = 0;
        uint32_t best = 1;
        for (uint32_t k = 2; k <= 7 && a + k - 1 < n_; ++k) {
            m = std::max(m, bw_[a + k - 1]);
            if (m > MAX_BITS[k]) break;
            best = k;
        }
        return a + best - 1;
    }

    void build_jumps() {
        jump_.resize(n_);
        for (uint32_t t = 0; t < n_; ++t) jump_[t] = t + kfar_[t];
        std::vector<uint32_t> next(n_);
        for (uint32_t span = 1; span < JUMP_WORDS; span *= 2) {
            for (uint32_t t = 0; t < n_; ++t) next[t] = jump_[jump_[t]];
            jump_.swap(next);
        }
    }

    // last sample index of the longest record starting at a, for each ascending word budget
    template <std::size_t N>
    void record_ends(const uint32_t a, const std::array<int, N>& words, std::array<uint32_t, N>& out) const {
        const auto limit = static_cast<uint32_t>(std::min<uint64_t>(n_ - 1, static_cast<uint64_t>(a) + max_samples_ - 1));
        uint32_t x = first_node(a);
        int used = 1;
        bool done = false;
        for (std::size_t i = 0; i < N; ++i) {
            while (!done && !jump_.empty() && used + static_cast<int>(JUMP_WORDS) <= words[i]) {
                const uint32_t y = jump_[x];
                if (y > limit || y == x) break;
                x = y;
                used += JUMP_WORDS;
            }
            while (!done && used < words[i]) {
                const uint32_t y = x + kfar_[x];
                if (y == x) { done = true; break; }
                if (y > limit) { x = limit; done = true; break; } // truncated word: fewer differences still fit
                x = y;
                ++used;
            }
            out[i] = x;
        }
    }

    uint32_t record_end(const uint32_t a, const int words) const {
        std::array<uint32_t, 1> out{};
        record_ends(a, std::array<int, 1>{words}, out);
        return out[0];
    }

    // tight drops the unused trailing frames, for formats without fixed record lengths
    PackedRecord emit(const uint32_t a, const uint32_t b, const int frames, const int reclen, const bool tight) const {
        PackedRecord rec;
        rec.reclen = reclen;
        rec.sample_count = static_cast<int>(b - a + 1);
        const int capacity = words_for_frames(frames);
        rec.payload.assign(static_cast<std::size_t>(frames) * 64, 0);

        put(rec.payload, 1, static_cast<uint32_t>(s_[a]));
        put(rec.payload, 2, static_cast<uint32_t>(s_[b]));

        std::array<int32_t, 7> d{};
        int wi = 0;
        uint32_t x = std::min(first_node(a), b);
        const int k_first = static_cast<int>(x - a) + 1;
        d[0] = 0; // ignored by decoders: the first difference of a record
        for (int j = 1; j < k_first; ++j) d[static_cast<std::size_t>(j)] = diff(a + static_cast<uint32_t>(j));
        write_word(rec.payload, wi++, k_first, d);

        while (x < b) {
            const uint32_t y = std::min(x + kfar_[x], b);
            if (y == x) throw std::logic_error("unreachable record end");
            const int k = static_cast<int>(y - x);
            for (int j = 0; j < k; ++j) d[static_cast<std::size_t>(j)] = diff(x + 1 + static_cast<uint32_t>(j));
            if (wi >= capacity) throw std::logic_error("record overflow");
            write_word(rec.payload, wi++, k, d);
            x = y;
        }
        if (tight) rec.payload.resize(static_cast<std::size_t>(wi <= 13 ? 1 : 1 + ((wi - 13 + 14) / 15)) * 64);
        return rec;
    }

private:
    int32_t diff(const uint32_t t) const { return static_cast<int32_t>(static_cast<uint32_t>(s_[t]) - static_cast<uint32_t>(s_[t - 1])); }

    static void put(std::vector<uint8_t>& payload, const std::size_t word_index, const uint32_t value) {
        const uint32_t be = htonl(value);
        std::memcpy(payload.data() + (word_index * 4), &be, 4);
    }

    static uint32_t get(const std::vector<uint8_t>& payload, const std::size_t word_index) {
        uint32_t be = 0;
        std::memcpy(&be, payload.data() + (word_index * 4), 4);
        return ntohl(be);
    }

    static void write_word(std::vector<uint8_t>& payload, const int wi, const int k, const std::array<int32_t, 7>& d) {
        auto dj = [&](const int j) { return static_cast<uint32_t>(d[static_cast<std::size_t>(j)]); };
        uint32_t val = 0;
        uint32_t nibble = 0;
        switch (k) {
            case 7: for (int j = 0; j < 7; ++j) val |= (dj(j) & 0xFU) << static_cast<unsigned>(24 - (j * 4)); val |= 0x2U << 30U; nibble = 3; break;
            case 6: for (int j = 0; j < 6; ++j) val |= (dj(j) & 0x1FU) << static_cast<unsigned>(25 - (j * 5)); val |= 0x1U << 30U; nibble = 3; break;
            case 5: for (int j = 0; j < 5; ++j) val |= (dj(j) & 0x3FU) << static_cast<unsigned>(24 - (j * 6)); nibble = 3; break;
            case 4: for (int j = 0; j < 4; ++j) val |= (dj(j) & 0xFFU) << static_cast<unsigned>(24 - (j * 8)); nibble = 1; break;
            case 3: for (int j = 0; j < 3; ++j) val |= (dj(j) & 0x3FFU) << static_cast<unsigned>(20 - (j * 10)); val |= 0x3U << 30U; nibble = 2; break;
            case 2: for (int j = 0; j < 2; ++j) val |= (dj(j) & 0x7FFFU) << static_cast<unsigned>(15 - (j * 15)); val |= 0x2U << 30U; nibble = 2; break;
            default: val = (dj(0) & 0x3FFFFFFFU) | (0x1U << 30U); nibble = 2; break;
        }
        const int f = (wi < 13) ? 0 : 1 + ((wi - 13) / 15);
        const int w = (wi < 13) ? wi + 3 : 1 + ((wi - 13) % 15);
        const std::size_t frame_base = static_cast<std::size_t>(f) * 16;
        put(payload, frame_base + static_cast<std::size_t>(w), val);
        const unsigned shift = static_cast<unsigned>(30 - (w * 2));
        put(payload, frame_base, (get(payload, frame_base) & ~(0x3U << shift)) | (nibble << shift));
    }

    const int32_t* s_;
    uint32_t n_;
    uint32_t max_samples_;
    std::vector<uint8_t> bw_;   // bit class of the difference ending at t
    std::vector<uint8_t> kfar_; // differences packed by the widest word starting at node t (0: record must end here)
    std::vector<uint32_t> jump_; // node reached after JUMP_WORDS greedy words
};

struct Span {
    uint32_t a;
    uint32_t b;
    int reclen;
};

std::vector<Span> fixed_plan(const Planner& p, const int frames, const int reclen) {
    std::vector<Span> plan;
    const int words = words_for_frames(frames);
    for (uint32_t a = 0; a < p.size();) {
        const uint32_t b = p.record_end(a, words);
        plan.push_back({a, b, reclen});
        a = b + 1;
    }
    return plan;
}

std::size_t fixed_records(const Planner& p, const int frames) {
    std::size_t records = 0;
    const int words = words_for_frames(frames);
    for (uint32_t a = 0; a < p.size(); ++records) a = p.record_end(a, words) + 1;
    return records;
}

// exact minimum over per-record lengths: covered[c] is the longest prefix encodable in c * MIN_RECLEN bytes
std::vector<Span> mixed_plan(Planner& p, const std::size_t budget_units) {
    p.build_jumps();
    std::array<int, NUM_RECLENS> words{};
    for (int i = 0; i < NUM_RECLENS; ++i) words[static_cast<std::size_t>(i)] = words_for_frames(frames_for_reclen(reclen_at(i)));

    const uint32_t n = p.size();
    std::vector<uint32_t> covered{0};
    std::vector<int8_t> choice{-1};
    std::vector<uint32_t> ends_of{0};
    std::vector<std::array<uint32_t, NUM_RECLENS>> ends(1);
    p.record_ends(0, words, ends[0]);

    std::size_t c = 0;
    while (covered[c] < n && c < budget_units) {
        ++c;
        uint32_t best = covered[c - 1];
        int8_t pick = -1;
        for (int i = 0; i < NUM_RECLENS; ++i) {
            const std::size_t units = std::size_t{1} << static_cast<unsigned>(i);
            if (units > c) break;
            const std::size_t from = c - units;
            const uint32_t e = ends[ends_of[from]][static_cast<std::size_t>(i)] + 1;
            if (e > best) { best = e; pick = static_cast<int8_t>(i); }
        }
        covered.push_back(best);
        choice.push_back(pick);
        if (best != covered[c - 1] && best < n) {
            ends.emplace_back();
            p.record_ends(best, words, ends.back());
            ends_of.push_back(static_cast<uint32_t>(ends.size() - 1));
        } else {
            ends_of.push_back(ends_of[c - 1]);
        }
    }
    if (covered[c] < n) return {};

    std::vector<Span> plan;
    while (c > 0) {
        if (choice[c] < 0) { --c; continue; }
        const std::size_t from = c - (std::size_t{1} << static_cast<unsigned>(choice[c]));
        plan.push_back({covered[from], covered[c] - 1, reclen_at(choice[c])});
        c = from;
    }
    std::reverse(plan.begin(), plan.end());
    return plan;
}

std::vector<PackedRecord> emit_plan(const Planner& p, const std::vector<Span>& plan) {
    std::vector<PackedRecord> records;
    records.reserve(plan.size());
    for (const Span& sp : plan) records.push_back(p.emit(sp.a, sp.b, frames_for_reclen(sp.reclen), sp.reclen, false));
    return records;
}

} // namespace

std::vector<PackedRecord> DPPacker::pack_steim2(const int32_t* samples, const std::size_t count, const int max_frames_per_record) {
    if (count == 0) return {};
    const Planner p(samples, count, MAX_SAMPLES_PER_RECORD);
    return emit_plan(p, fixed_plan(p, max_frames_per_record, (max_frames_per_record + 1) * 64));
}

int DPPacker::evaluate_cost(const int32_t* samples, const std::size_t count, const int max_frames_per_record) {
    if (count == 0) return 0;
    const Planner p(samples, count, MAX_SAMPLES_PER_RECORD);
    return static_cast<int>(fixed_records(p, max_frames_per_record));
}

std::vector<PackedRecord> DPPacker::pack_steim2_smallest(const int32_t* samples, const std::size_t count, const bool mixed) {
    if (count == 0) return {};
    Planner p(samples, count, MAX_SAMPLES_PER_RECORD);

    int best_reclen = MIN_RECLEN;
    std::size_t best_bytes = SIZE_MAX;
    for (int i = 0; i < NUM_RECLENS; ++i) {
        const std::size_t bytes = fixed_records(p, frames_for_reclen(reclen_at(i))) * static_cast<std::size_t>(reclen_at(i));
        if (bytes < best_bytes) { best_bytes = bytes; best_reclen = reclen_at(i); }
    }

    if (mixed) {
        // strictly smaller only: a uniform record length is the more conservative layout
        const std::vector<Span> plan = mixed_plan(p, (best_bytes / MIN_RECLEN) - 1);
        if (!plan.empty()) return emit_plan(p, plan);
    }
    return emit_plan(p, fixed_plan(p, frames_for_reclen(best_reclen), best_reclen));
}

std::vector<PackedRecord> DPPacker::pack_steim2_variable(const int32_t* samples, const std::size_t count, const int max_frames_per_record) {
    if (count == 0) return {};
    const Planner p(samples, count, UINT32_MAX);
    std::vector<PackedRecord> records;
    for (const Span& sp : fixed_plan(p, max_frames_per_record, 0)) records.push_back(p.emit(sp.a, sp.b, max_frames_per_record, 0, true));
    return records;
}

} // namespace mseedout
