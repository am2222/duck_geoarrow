# duck_geoarrow

## Before pushing: run CI's code-quality checks locally

The `Code Quality Check` job (extension-ci-tools `_extension_code_quality.yml`) runs a format
check and a tidy check. Both must pass, so run both before pushing.

### Format check

CI pins its formatter versions, and newer ones produce different output. Install those versions
into a throwaway venv rather than using whatever is on PATH:

```sh
python3 -m venv /tmp/fmt-venv
/tmp/fmt-venv/bin/pip install cmake-format==0.6.13 'black==24.*' 'clang_format==11.0.1' cxxheaderparser pcpp
PATH=/tmp/fmt-venv/bin:$PATH /tmp/fmt-venv/bin/python duckdb/scripts/format.py \
  --all --check --workdir "$PWD" --directories src test
```

Drop `--check` (and add `--fix --noconfirm`) to apply the fixes. It covers more than C++:
clang-format 11 on `src/`, black 24 on `test/python/`, and cmake-format on `CMakeLists.txt`.
The DuckDB v2 `format.py` started checking `CMakeLists.txt`, which v1.5 did not.

### Tidy check

```sh
make tidy-check TIDY_BINARY=/opt/homebrew/opt/llvm/bin/clang-tidy
```

This configures a separate unity-disabled DuckDB build in `build/tidy` and runs clang-tidy over
`src/` with `duckdb/.clang-tidy`. clang-tidy is not on PATH on macOS; Homebrew's `llvm` has it.

## Build notes

- DuckDB v2 requires C++17. A build dir first configured for v1.5 has `CMAKE_CXX_STANDARD=11`
  cached and fails to configure; fix it with `cmake -DCMAKE_CXX_STANDARD=17 build/<dir>`.
- Use `GEN=ninja make` locally only; never default `GEN` to ninja in the Makefile, because the
  WASM CI builds run `emmake make` and need Makefiles.
- Scalar functions that can throw at execution time must be wrapped in `Fallible(...)`, or DuckDB
  v2 reports the error as an INTERNAL error.
