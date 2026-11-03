
# clang.llvm-mca

** LLVM MCA module **

Multi phase module to analyse binary instructions
Puts output file in bin/$profile/res

## Prerequisites
  [maiken](https://github.com/mkn/mkn)

## Usage

```yaml
mod:
- name: clang.llvm-mca
  link:
    bin: $str      # [optional] llvm-mca binary, default: "$compiler -print-prog-name=llvm-mca"
                   #   (e.g. clang++-19 -> /usr/lib/llvm-19/bin/llvm-mca), else "llvm-mca"
                   #   launchers (ccache, sccache, distcc, icecc) are skipped, MPI wrappers
                   #   forward to their compiler
    mcpu: $str     # [optional] default: -march=/-mcpu= from compile flags, else "native"
    args: $str     # [optional] extra llvm-mca args, e.g. "-bottleneck-analysis -timeline"
    types: $str    # [optional] colon separated file extensions to analyse, e.g. "cpp:cc"
    regions: only  # [optional] skip translation units without LLVM-MCA-BEGIN markers
    strip: $str    # [optional] colon separated flags dropped from the -S compile, trailing "*"
                   #   matches a prefix, "" keeps all
                   #   default: "-pg:-g*:-fsave-optimization-record*:-foptimization-record-*"
    threads: $int  # [optional] default: maiken's thread count
    allow_failure: $bool  # [optional] report failures as warnings, default: false
                          #   or set env MKN_LLVM_MCA_ALLOW_FAILURE=1
```

Each translation unit is recompiled with `-S` using its real compile flags. The chosen llvm-mca
and `-mcpu` are printed.

Mark regions with the macros from `mkn/mca.hpp`, which the module always puts on the include path
(`bin/$profile/llvm-mca/inc`):

```cpp
#include "mkn/mca.hpp"

for (int i = 0; i < n; ++i) {
  MKN_MCA_BEGIN(name);
  // hot code
  MKN_MCA_END(name);
}
```

A region is built from the loop structure clang annotates the assembly with, not from the text
between the markers, which optimised loops scramble (tail duplication, loop rotation):

- `BEGIN` inside a loop: that loop's blocks, including nested loops, as region `name`, and each
  child loop on its own as `name.<BBx_y>`.
- `BEGIN` outside a loop: the blocks from the marker to the end of the function as `name`, and
  each loop in that range as `name.<BBx_y>`.

A region spanning nested loops counts the inner loop body once, so its cycle counts are not per
outer iteration - use the `name.<BBx_y>` regions for per-loop numbers.

Each region is analysed separately:

    bin/$profile/res/<hash>-<file>/<region>.llvm-mca.txt
    bin/$profile/tmp/<hash>-<file>/<region>.s
    bin/$profile/tmp/<hash>-<file>.s                      # full -S output

Without markers the whole translation unit is analysed as one region named after the file, which is
rarely meaningful, so a warning is printed.

A failed `-S` compile or region analysis fails the build once all translation units are done,
listing every failure. Failed regions leave no result file.

## Building

  Windows cl:

    mkn clean build -tSa -EHsc -d


  *nix gcc:

    mkn clean build -tSa "-O2 -fPIC" -d -l "-pthread -ldl"


## Testing

  Windows cl:

    mkn clean build -tSa -EHsc -dp test run


  *nix gcc:

    mkn clean build -tSa "-O2 -fPIC" -dp test -l "-pthread -ldl" run

