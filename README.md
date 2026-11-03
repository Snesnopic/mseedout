# mseedout

mseedout recompresses miniSEED 2 and miniSEED 3 files to the smallest possible valid output. It finds the provably optimal Steim-2 packing and the record layout that minimizes total file size, in linear time, without changing any sample. The output keeps the format version of the input.

## Building

```bash
git clone --recursive https://github.com/yourusername/mseedout.git
cd mseedout
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build .
```

Requires a C++20 compiler. libmseed is included as a submodule.

## CLI usage

```bash
./mseedout [-v|--verbose] [--mixed-reclen] <input.mseed> <output.mseed>
```

- `-v`, `--verbose`: print the chosen encoding and record layout for every trace segment.
- `--mixed-reclen`: let each record pick its own record length within a trace (see below). miniSEED 3 records always have their own length, so the option only affects miniSEED 2.

A tool for verifying correctness is also built:

```bash
./mseedverify <file1.mseed> <file2.mseed>
```

## How it works

### Steim-2 recap

Steim-2 stores integer samples as first differences. Consecutive differences are packed together into 32-bit data words using one of seven encodings, each trading word count for bit width:

| k (diffs/word) | bits/diff | max \|diff\| |
|:--------------:|:---------:|:------------:|
|       7        |     4     |      8       |
|       6        |     5     |      16      |
|       5        |     6     |      32      |
|       4        |     8     |     128      |
|       3        |    10     |     512      |
|       2        |    15     |    16 384    |
|       1        |    30     | 536 870 911  |

Each record is made of 64-byte frames. Frame 0 reserves two words for X0 (first sample) and Xn (last sample, used as an integrity check). The remaining words are data words. The total number of available data words per record (`W_max`) is:

```
W_max = 13 + (max_frames - 1) × 15
```

Decoders ignore the first difference of every record (X0 is stored explicitly), so the first data word of a record spends one slot on a dummy difference. A miniSEED 2 record can also hold at most 65 535 samples, since the sample count field is 16 bits.

### Optimal packing in linear time

The goal is the minimum number of records for a given record length. Two properties make a greedy strategy optimal:

1. **A sub-window of a valid word is a valid word.** Packing fewer differences allows a smaller k, which never has fewer bits per difference. So if a word can cover differences `u+1..v`, a word starting later can cover `x+1..v` for any `u ≤ x < v`.
2. **Reach is therefore monotone in the starting point.** Starting later never ends earlier, for any number of words. Packing every word with as many differences as fit is optimal, and the same argument one level up makes extending every record as far as it fits optimal.

For each segment mseedout precomputes, for every sample, the widest word that can start there (one byte per sample). Counting the records for a record length is then a single forward walk. All ten power-of-two record lengths from 256 to 131 072 bytes are evaluated, and the smallest total wins. This yields exactly the same record count as an exhaustive dynamic program, in O(n) time and memory instead of O(n · W_max).

Differences outside the 30-bit range cannot be stored in any data word, but the jump can always be absorbed by starting a new record, whose X0 is a full 32-bit sample. Every integer series is therefore encodable.

### Other encodings

For each trace segment the smallest of these standard encodings is written:

- **Steim-2**, packed as above. This wins for practically all seismic data.
- **INT16** (when every sample fits) or **INT32**, which can win for high-entropy channels such as state-of-health counters. They are only tried when Steim-2 needs more than 2 or 4 bytes per sample, so normal data pays nothing for the check.
- Float and text channels have no Steim form and are repacked in their own encoding (FLOAT32, FLOAT64, TEXT) with the record length that wastes the least space.

Records that carry no samples, such as event detections or timing blockettes, are copied verbatim.

### miniSEED 3

miniSEED 3 input is written back as miniSEED 3. The format has no power-of-two record lengths and a 32-bit sample count, so each Steim-2 record is exactly as long as its data, up to 131 072 bytes, with no padding and no 65 535-sample limit. Start times keep their nanosecond precision and every record carries a CRC-32C, as the format requires.

Flags and extra headers (such as `FDSN.Time.Quality` or timing exceptions) belong to individual records. Consecutive input records with identical flags and extra headers form a run: samples are repacked freely within a run, output records never cross a run boundary, and every output record carries its run's flags and extra headers. An identical extra header repeated over many input records may therefore appear in a different number of output records, which is how libmseed itself packs from a template. A file that mixes miniSEED 2 and 3 records is rejected.

### Mixed record lengths (`--mixed-reclen`)

With a single record length per segment, the last record is usually partly empty. With `--mixed-reclen` every record may use its own power-of-two length, and mseedout finds the exact minimum total size: `covered[c]` is the longest prefix of the segment encodable in `c × 256` bytes, and extending from the longest prefix is optimal because record ends are monotone in their start. A table of 64-word jumps keeps each lookup cheap.

The output is only used when it is strictly smaller than the best single-length layout. Savings are typically 0.3% on long continuous files, a few percent on medium files and up to 20% on small ones. Varying record lengths are valid miniSEED, and libmseed has auto-detected the length of each record independently since at least version 2.6, which covers ObsPy, SeisComP, pyrocko and the IRIS tools. The option stays off by default for readers that might assume a fixed length.

## Using mseedout as a library

The build produces `libmseedout_lib` alongside the CLI. To embed it in another CMake project:

```cmake
add_subdirectory(path/to/mseedout)

target_link_libraries(your_target PRIVATE mseedout_lib)
target_include_directories(your_target PRIVATE path/to/mseedout/include)
```

The public API is a single function declared in `include/mseedout/mseedout.hpp`:

```cpp
#include <mseedout/mseedout.hpp>

bool ok = mseedout::recompress_mseed("input.mseed", "output.mseed");
bool ok_mixed = mseedout::recompress_mseed("input.mseed", "output.mseed", /*verbose=*/false, /*mixed_reclen=*/true);
```

`recompress_mseed` reads the input file with libmseed, recompresses every trace segment and writes a file of the same miniSEED version to the output path. It returns `false`, after reporting the reason on stderr, when the input cannot be read or a segment cannot be reproduced exactly, so that callers can keep the original file.

### Lower-level access

If you need direct access to the packer (e.g. to integrate it into a custom miniSEED writer), `DPPacker` is declared in `include/mseedout/dp_packer.hpp`:

```cpp
#include <mseedout/dp_packer.hpp>

// smallest Steim-2 encoding over all record lengths
auto records = mseedout::DPPacker::pack_steim2_smallest(samples, count, /*mixed=*/false);

// miniSEED 3: records only as long as their data, at most max_frames frames each
auto variable = mseedout::DPPacker::pack_steim2_variable(samples, count, max_frames);

// or a fixed record length: max_frames = (record_length - 64) / 64
int n_records = mseedout::DPPacker::evaluate_cost(samples, count, max_frames);
auto fixed = mseedout::DPPacker::pack_steim2(samples, count, max_frames);

for (const auto& rec : records) {
    // rec.payload      — vector<uint8_t>, Steim-2 frames filling rec.reclen - 64 bytes
    // rec.sample_count — number of samples in this record (including X0)
    // rec.reclen       — record length this payload belongs to
}
```

The payload is meant to follow a 64-byte miniSEED 2 header; the caller is responsible for writing the fixed header and blockettes.
