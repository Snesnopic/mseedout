# mseedout

mseedout is a tool for the absolute mathematical optimal lossless compression of miniSEED files. 
It uses dynamic programming to generate the smallest possible Steim-2 payload and optimal record padding, without altering the format version or any metadata.

## Building

```bash
git clone --recursive https://github.com/yourusername/mseedout.git
cd mseedout
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build .
```

## Usage

```bash
./mseedout <input.mseed> <output.mseed>
```
