#include "mseedout/processor.hpp"
#include "mseedout/dp_packer.hpp"
#include <libmseed.h>
#include <iostream>
#include <fstream>
#include <cstring>

namespace mseedout {

bool Processor::process(const std::filesystem::path& input_path, const std::filesystem::path& output_path) {
    std::cout << "Starting optimization for " << input_path << "\n";

    MS3TraceList* mstl = nullptr;
    const int ret = ms3_readtracelist(&mstl, input_path.c_str(), nullptr, 0, MSF_UNPACKDATA, 0);
    
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

    if (mstl->traces.next[0]) {
        for (const MS3TraceID *id = mstl->traces.next[0]; id != nullptr; id = id->next[0]) {
            for (const MS3TraceSeg *seg = id->first; seg != nullptr; seg = seg->next) {
                if (seg->samplecnt <= 0 || seg->sampletype != 'i') continue;

                const int32_t* samples = static_cast<int32_t*>(seg->datasamples);
                
                int best_reclen = 512;
                size_t min_total_size = SIZE_MAX;
                constexpr int header_overhead = 64;

                // Evaluate L
                for (int reclen = 256; reclen <= 131072; reclen *= 2) {
                    const int payload_capacity = reclen - header_overhead;
                    if (payload_capacity <= 0 || payload_capacity % 64 != 0) continue;
                    
                    const int max_frames = payload_capacity / 64;
                    const int records_needed = DPPacker::evaluate_cost(samples, seg->samplecnt, max_frames);
                    
                    if (records_needed > 0 && records_needed < 1000000000) {
                        const std::size_t projected_size = static_cast<std::size_t>(records_needed) * static_cast<std::size_t>(reclen);
                        if (projected_size < min_total_size) {
                            min_total_size = projected_size;
                            best_reclen = reclen;
                        }
                    }
                }
                
                std::cout << "Optimal record length chosen: " << best_reclen << " (Expected size: " << min_total_size << " bytes)\n";
                
                // Pack for optimal L
                const int max_frames = (best_reclen - header_overhead) / 64;
                auto packed_records = DPPacker::pack_steim2(samples, seg->samplecnt, max_frames);
                
                int64_t current_starttime = seg->starttime;
                const auto record_buffer = new char[best_reclen];
                
                MS3Record* msr = msr3_init(nullptr);
                strncpy(msr->sid, id->sid, sizeof(msr->sid));
                msr->samprate = seg->samprate;
                msr->sampletype = 'i';
                msr->encoding = 11; // DE_STEIM2
                msr->formatversion = 2; // Forcing MSEED2 output for compatibility
                msr->pubversion = id->pubversion;
                msr->reclen = best_reclen;
                
                // Write output
                for (size_t i = 0; i < packed_records.size(); ++i) {
                    const auto& prec = packed_records[i];
                    
                    msr->starttime = current_starttime;
                    msr->numsamples = prec.sample_count;
                    
                    memset(record_buffer, 0, static_cast<std::size_t>(best_reclen));
                    const int header_len = msr3_pack_header2(msr, record_buffer, best_reclen, 0);
                    
                    if (header_len < 0) {
                        std::cerr << "Failed to pack header for record " << i << "\n";
                        continue;
                    }
                    
                    // Force dataoffset to 64 for Steim-2 alignment
                    int dataoffset = 64;
                    while (dataoffset < header_len) {
                        dataoffset += 64;
                    }
                    
                    // Patch FSDH dataoffset (byte 44) if format is MS2
                    uint16_t offset_u16 = htons((uint16_t)dataoffset);
                    memcpy(record_buffer + 44, &offset_u16, 2);
                    
                    // Patch FSDH numsamples (bytes 30-31)
                    uint16_t numsamples_u16 = htons((uint16_t)prec.sample_count);
                    memcpy(record_buffer + 30, &numsamples_u16, 2);
                    
                    // Patch B1000 encoding to DE_STEIM2 (byte 52)
                    record_buffer[52] = 11;
                    
                    // Copy Steim-2 payload
                    size_t payload_size = prec.payload.size();
                    if (static_cast<std::size_t>(dataoffset) + payload_size > static_cast<std::size_t>(best_reclen)) {
                        payload_size = static_cast<std::size_t>(best_reclen) - static_cast<std::size_t>(dataoffset);
                    }
                    memcpy(record_buffer + dataoffset, prec.payload.data(), payload_size);
                    
                    // Write to file
                    outfile.write(record_buffer, best_reclen);
                    
                    // Advance time
                    current_starttime = ms_sampletime(current_starttime, prec.sample_count, seg->samprate);
                }
                
                msr3_free(&msr);
                delete[] record_buffer;
            }
        }
    }

    mstl3_free(&mstl, 0);
    return true;
}

} // namespace mseedout
