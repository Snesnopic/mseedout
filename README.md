# mseedout

mseedout recompresses miniSEED files to the smallest possible valid output. It uses dynamic programming to find the optimal Steim-2 word assignment and exhaustive search over valid record lengths to minimize total file size, without altering the format version or any metadata.

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
./mseedout <input.mseed> <output.mseed>
```

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

### Dynamic programming

The core problem is: given N samples and W_max data words per record, assign consecutive differences to words to minimize the total number of records. This is a shortest-path problem on a DAG.

**State:** `DP[i][r_w]` — "we have processed samples `[0..i-1]`, and have used `r_w` data words in the current open record."

**Initial state:** `DP[1][0] = 0 records completed`. Sample 0 is always X0 and is never encoded as a diff.

**Transitions from `(i, r_w)`:** for each k ∈ {1..7}, check whether the next k differences (or k−1 if r_w=0, because the first word of a record carries a dummy zero diff required by the decoder) all fit within the bit width for k. If yes:
- **Same record:** advance to `DP[i + samples_packed][r_w + 1]` with the same cost.
- **New record:** complete the current record and start the next one at `DP[i + samples_packed + 1][0]` with cost + 1.

**Terminal:** the minimum of `DP[N][r_w].cost + 1` for `r_w ≥ 1` is the total number of records. `r_w = 0` is excluded because it represents a record that was opened but has no data words yet — a boundary artifact, not a valid end state.

**Bit-width pre-computation:** before the DP, each difference is classified into the smallest fitting encoding. Differences outside ±536 870 911 (the 30-bit signed range) are marked as unencodable; any segment containing one cannot be Steim-2 encoded and is skipped with a warning.

### Record length selection

miniSEED record lengths must be powers of two between 256 and 131 072 bytes. For each candidate length, `evaluate_cost` runs the DP and computes the projected total output size. The length that yields the smallest output is selected. The final `pack_steim2` call then runs the DP again for that length and backtracks through the matrix to build the actual Steim-2 frame payloads.

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
```

`recompress_mseed` reads the input file with libmseed, optimally recompresses every integer-typed trace segment, and writes a valid miniSEED file to the output path. Non-integer channels (float32, float64) are silently skipped. It returns `false` on I/O errors; segments that cannot be Steim-2 encoded emit a warning to stderr and are omitted from the output.

### Lower-level access

If you need direct access to the packer (e.g. to integrate the DP into a custom miniSEED writer), `DPPacker` is declared in `include/mseedout/dp_packer.hpp`:

```cpp
#include <mseedout/dp_packer.hpp>

// Estimate the number of records needed for a given record length
// without building the payload — used to pick the optimal length.
int max_frames = (record_length - 64) / 64;
int n_records = mseedout::DPPacker::evaluate_cost(samples, count, max_frames);

// Pack and retrieve the raw Steim-2 frame payloads.
auto records = mseedout::DPPacker::pack_steim2(samples, count, max_frames);
for (const auto& rec : records) {
    // rec.payload      — vector<uint8_t>, multiple of 64 bytes (Steim-2 frames)
    // rec.sample_count — number of samples in this record (including X0)
}
```

`max_frames` is `(record_length - 64) / 64`, where 64 bytes is the fixed miniSEED v2 header overhead. The payload returned by `pack_steim2` can be copied directly after the record header; the caller is responsible for writing the miniSEED fixed header and blockettes.
