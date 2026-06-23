#include "mseedout/dp_packer.hpp"
#include <iostream>
#include <algorithm>
#include <cmath>

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

void DPPacker::precompute_bit_widths(const int32_t* samples, const std::size_t count, std::vector<uint8_t>& bit_widths) {
    if (count <= 1) return;
    bit_widths.resize(count, 0);
    int32_t prev = samples[0];
    for (std::size_t i = 1; i < count; ++i) {
        const int64_t diff = static_cast<int64_t>(samples[i]) - prev;
        prev = samples[i];
        if (diff >= -8 && diff <= 7) bit_widths[i] = 4;
        else if (diff >= -16 && diff <= 15) bit_widths[i] = 5;
        else if (diff >= -32 && diff <= 31) bit_widths[i] = 6;
        else if (diff >= -128 && diff <= 127) bit_widths[i] = 8;
        else if (diff >= -512 && diff <= 511) bit_widths[i] = 10;
        else if (diff >= -16384 && diff <= 16383) bit_widths[i] = 15;
        else if (diff >= -536870912LL && diff <= 536870911LL) bit_widths[i] = 30;
        else bit_widths[i] = 31; // overflow: diff does not fit in Steim-2 30-bit
    }
}

std::vector<std::vector<DPPacker::State>> DPPacker::run_dp(const int32_t* samples, std::size_t count, int W_max, const std::vector<uint8_t>& bit_widths) {
    (void)samples;
    constexpr int INF = 1000000000;
    const std::size_t W_max_sz = static_cast<std::size_t>(W_max);
    std::vector<std::vector<State>> DP(count + 1, std::vector<State>(W_max_sz + 1));
    for (std::size_t i = 0; i <= count; ++i) {
        for (std::size_t r_w = 0; r_w <= W_max_sz; ++r_w) {
            DP[i][r_w] = {INF, -1, -1, 0};
        }
    }

    DP[1][0] = {0, -1, -1, 0};

    for (std::size_t i = 1; i < count; ++i) {
        for (int r_w = 0; r_w <= W_max; ++r_w) {
            const std::size_t r_w_sz = static_cast<std::size_t>(r_w);
            if (DP[i][r_w_sz].c == INF) continue;
            int c = DP[i][r_w_sz].c;

            if (r_w < W_max) {
                for (int k = 1; k <= 7; ++k) {
                    int max_b = 0;
                    if (k == 1) max_b = 30;
                    else if (k == 2) max_b = 15;
                    else if (k == 3) max_b = 10;
                    else if (k == 4) max_b = 8;
                    else if (k == 5) max_b = 6;
                    else if (k == 6) max_b = 5;
                    else if (k == 7) max_b = 4;

                    const int samples_packed = (r_w == 0) ? (k - 1) : k;
                    const std::size_t next_i = i + static_cast<std::size_t>(samples_packed);

                    if (next_i <= count) {
                        bool fits = true;
                        for (int j = 0; j < samples_packed; ++j) {
                            if (bit_widths[i + static_cast<std::size_t>(j)] > max_b) {
                                fits = false;
                                break;
                            }
                        }

                        if (fits) {
                            const std::size_t new_r_w_sz = r_w_sz + 1;
                            if (new_r_w_sz <= W_max_sz) {
                                if (DP[next_i][new_r_w_sz].c > c) {
                                    DP[next_i][new_r_w_sz] = {c, static_cast<int>(i), r_w, k};
                                }
                            }
                            if (next_i < count) {
                                const std::size_t start_i = next_i + 1;
                                if (DP[start_i][0].c > c + 1) {
                                    DP[start_i][0] = {c + 1, static_cast<int>(i), r_w, k};
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    return DP;
}

int DPPacker::evaluate_cost(const int32_t* samples, std::size_t count, int max_frames_per_record) {
    if (count == 0) return 0;
    if (count == 1) return 1;
    int W_max = 13 + ((max_frames_per_record - 1) * 15);
    std::vector<uint8_t> bit_widths;
    precompute_bit_widths(samples, count, bit_widths);
    auto DP = run_dp(samples, count, W_max, bit_widths);

    constexpr int INF = 1000000000;
    int min_c = INF;
    for (int r_w = 1; r_w <= W_max; ++r_w) {
        if (DP[count][static_cast<std::size_t>(r_w)].c == INF) continue;
        int total_c = DP[count][static_cast<std::size_t>(r_w)].c + 1;
        if (total_c < min_c) min_c = total_c;
    }
    return min_c;
}

std::vector<PackedRecord> DPPacker::pack_steim2(const int32_t* samples, std::size_t count, int max_frames_per_record) {
    std::vector<PackedRecord> records;
    if (count == 0) return records;
    if (count == 1) {
        PackedRecord rec;
        rec.payload.assign(static_cast<std::size_t>(max_frames_per_record) * 64, 0);
        rec.sample_count = 1;
        auto* frame0 = reinterpret_cast<uint32_t*>(rec.payload.data());
        frame0[1] = htonl(static_cast<uint32_t>(samples[0]));
        frame0[2] = htonl(static_cast<uint32_t>(samples[0]));
        records.push_back(rec);
        return records;
    }

    int W_max = 13 + ((max_frames_per_record - 1) * 15);
    std::vector<uint8_t> bit_widths;
    precompute_bit_widths(samples, count, bit_widths);
    auto DP = run_dp(samples, count, W_max, bit_widths);

    constexpr int INF = 1000000000;
    int best_r_w = -1;
    int min_c = INF;
    for (int r_w = 1; r_w <= W_max; ++r_w) {
        if (DP[count][static_cast<std::size_t>(r_w)].c == INF) continue;
        int total_c = DP[count][static_cast<std::size_t>(r_w)].c + 1;
        if (total_c < min_c) {
            min_c = total_c;
            best_r_w = r_w;
        }
    }
    if (min_c == INF) return records;

    struct Step { int i, r_w, k; };
    std::vector<Step> path;
    int curr_i = static_cast<int>(count);
    int curr_r_w = best_r_w;

    while (curr_i > 1 || curr_r_w > 0) {
        const auto& state = DP[static_cast<std::size_t>(curr_i)][static_cast<std::size_t>(curr_r_w)];
        path.push_back({state.prev_i, state.prev_r_w, state.k});
        curr_i = state.prev_i;
        curr_r_w = state.prev_r_w;
    }
    std::reverse(path.begin(), path.end());

    PackedRecord current_rec;
    int32_t current_X0 = samples[0];

    auto get_frame_and_word = [](const int r_w) -> std::pair<int, int> {
        if (r_w == 0) return {0, 0};
        if (r_w <= 13) return {0, r_w + 2};
        int r_w_rem = r_w - 14;
        return {(r_w_rem / 15) + 1, (r_w_rem % 15) + 1};
    };

    auto set_word = [&](std::vector<uint8_t>& payload, const int f, const int w, uint32_t val) {
        auto* const frame_words = reinterpret_cast<uint32_t*>(payload.data() + static_cast<std::size_t>(f) * 64);
        frame_words[static_cast<std::size_t>(w)] = htonl(val);
    };

    auto set_nibble = [&](std::vector<uint8_t>& payload, int f, int w, uint8_t nibble) {
        auto* frame_words = reinterpret_cast<uint32_t*>(payload.data() + static_cast<std::size_t>(f) * 64);
        uint32_t header = ntohl(frame_words[0]);
        const int shift = 30 - (w * 2);
        header &= ~(0x3U << static_cast<unsigned>(shift));
        header |= (static_cast<uint32_t>(nibble) << static_cast<unsigned>(shift));
        frame_words[0] = htonl(header);
    };

    auto init_record = [&]() {
        current_rec.payload.assign(static_cast<std::size_t>(max_frames_per_record) * 64, 0);
        current_rec.sample_count = 1;
        auto* frame0 = reinterpret_cast<uint32_t*>(current_rec.payload.data());
        frame0[1] = htonl(static_cast<uint32_t>(current_X0));
    };
    init_record();

    for (std::size_t step_idx = 0; step_idx < path.size(); ++step_idx) {
        const auto& step = path[step_idx];
        int samples_packed = (step.r_w == 0) ? (step.k - 1) : step.k;
        current_rec.sample_count += samples_packed;

        std::vector<int32_t> d(static_cast<std::size_t>(step.k), 0);
        if (step.r_w == 0) {
            d[0] = 0;
            for (int j = 0; j < samples_packed; ++j) {
                d[static_cast<std::size_t>(j) + 1] = samples[step.i + j] - samples[step.i + j - 1];
            }
        } else {
            for (int j = 0; j < samples_packed; ++j) {
                d[static_cast<std::size_t>(j)] = samples[step.i + j] - samples[step.i + j - 1];
            }
        }

        int target_r_w = step.r_w + 1;
        auto [f, w] = get_frame_and_word(target_r_w);
        uint32_t val = 0; uint8_t nibble = 0;

        auto dj = [&](int j) { return static_cast<uint32_t>(d[static_cast<std::size_t>(j)]); };
        if (step.k == 7) { for (int j=0; j<7; ++j) val |= ((dj(j) & 0xFU) << static_cast<unsigned>(24 - j*4)); val |= (0x2U << 30U); nibble = 3; }
        else if (step.k == 6) { for (int j=0; j<6; ++j) val |= ((dj(j) & 0x1FU) << static_cast<unsigned>(25 - j*5)); val |= (0x1U << 30U); nibble = 3; }
        else if (step.k == 5) { for (int j=0; j<5; ++j) val |= ((dj(j) & 0x3FU) << static_cast<unsigned>(24 - j*6)); val |= (0x0U << 30U); nibble = 3; }
        else if (step.k == 4) { for (int j=0; j<4; ++j) val |= ((dj(j) & 0xFFU) << static_cast<unsigned>(24 - j*8)); nibble = 1; }
        else if (step.k == 3) { for (int j=0; j<3; ++j) val |= ((dj(j) & 0x3FFU) << static_cast<unsigned>(20 - j*10)); val |= (0x3U << 30U); nibble = 2; }
        else if (step.k == 2) { for (int j=0; j<2; ++j) val |= ((dj(j) & 0x7FFFU) << static_cast<unsigned>(15 - j*15)); val |= (0x2U << 30U); nibble = 2; }
        else if (step.k == 1) { val |= (dj(0) & 0x3FFFFFFFU); val |= (0x1U << 30U); nibble = 2; }

        set_word(current_rec.payload, f, w, val);
        set_nibble(current_rec.payload, f, w, nibble);

        bool record_finished = false;
        if (step_idx + 1 < path.size() && path[step_idx + 1].r_w == 0) {
            record_finished = true;
        }

        if (record_finished) {
            int next_i = step.i + samples_packed;
            auto* frame0 = reinterpret_cast<uint32_t*>(current_rec.payload.data());
            frame0[2] = htonl(static_cast<uint32_t>(samples[next_i - 1])); // Xn = last sample of this record
            records.push_back(current_rec);
            current_rec.sample_count = 0;

            current_X0 = samples[next_i];
            init_record();
        }
    }

    if (current_rec.sample_count > 0) {
        auto* frame0 = reinterpret_cast<uint32_t*>(current_rec.payload.data());
        frame0[2] = htonl(static_cast<uint32_t>(samples[count - 1]));
        records.push_back(current_rec);
    }

    return records;
}

} // namespace mseedout
