
# clang.llvm-mca

** LLVM MCA module **

Multi phase module to analyse binary instructions
Puts output file in bin/$profile/res

## Prerequisites
  [maiken](https://github.com/mkn/mkn)

## Usage

```yaml
mod:
- name: clang.format
  link:
    bin: $str  #[optional, default="llvm-mca"]
    out: $str  #[optional, default="llvm-mca.txt"]
```

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

