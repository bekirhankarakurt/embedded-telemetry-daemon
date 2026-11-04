# Contributing

Thanks for your interest! Bug reports, fixes and focused improvements are welcome.

## Build and test

```bash
cmake -S . -B build -DETD_WERROR=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
scripts/smoke_test.sh build
```

Before opening a pull request, also run the sanitizer builds:

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DETD_SANITIZE_ADDRESS=ON && cmake --build build-asan -j && ctest --test-dir build-asan
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DETD_SANITIZE_THREAD=ON && cmake --build build-tsan -j && ctest --test-dir build-tsan
```

## Style and quality bar

- C++17, no extensions, **no external dependencies** (STL and Linux/POSIX only).
- The code must build without warnings on GCC and Clang (`-DETD_WERROR=ON`).
- Formatting: `clang-format -i` with the committed `.clang-format`.
- Static analysis must pass:
  ```bash
  cppcheck --enable=warning,style,performance,portability --error-exitcode=1 --inline-suppr \
      --std=c++17 -I include --suppress=missingIncludeSystem --quiet src tools tests
  cmake -S . -B build-tidy -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DCMAKE_CXX_COMPILER=clang++
  clang-tidy -p build-tidy --quiet src/*.cpp tools/*.cpp
  ```
- RAII everywhere (no raw `new`/`delete`), `const`-correctness, `[[nodiscard]]` on results that must not
  be ignored, `enum class`, and concise Doxygen comments on public APIs (`doxygen Doxyfile` must be warning-free).
- New behaviour needs a test in `tests/`; wire-format changes must bump `wire::kVersion` and update the
  table in the README.

## Commit conventions

- Small, logical commits with an imperative subject of at most 72 characters
  (`Add ...`, `Fix ...`, `Refactor ...`); explain *why* in the body when it is not obvious.
- One topic per pull request; keep unrelated formatting changes out.

## Regenerating the demo GIF

```bash
pip install asciinema   # plus the `agg` binary from https://github.com/asciinema/agg
asciinema rec --cols 132 --rows 24 -c "bash docs/demo.sh $PWD/build" docs/demo.cast
agg --font-size 14 --theme monokai --speed 1.3 docs/demo.cast docs/demo.gif
```

## Reporting security issues

See [SECURITY.md](SECURITY.md).
