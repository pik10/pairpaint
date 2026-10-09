# Fuzzing inputs

- `seed-*.pairpaint`, `seed-*.psd`: small files used as fuzzing seeds (together with the PSDs in
  `../psd-tools`). `seed-full` and `seed-small` are version 5 projects; `seed-v7-text`,
  `seed-v7-adjustments` and `seed-adjustments.psd` (Vibrance, Exposure and Color Balance layers) are
  written by the test suite: run it with `PAIRPAINT_KEEP_TEST_FILES=1` to make them again.
  `seed.heic` is a copy of `../heic/sample.heic`.
- `hang-huge-layer-name.psd`: a corrupted PSD found by the fuzzer that made the reader loop for
  minutes (a layer name claiming ~4 billion characters past the end of the file). Kept as a
  regression test.
- `oom-font-qt610.pairpaint`, `ci-seed18-last.psd`: the two inputs saved when the CI fuzzer hit a
  48 GB allocation with Qt 6.10 (most likely the corrupted text-layer font in the project file, whose
  font family list claimed ~2 billion entries). Damaged files must report an error, not run out of memory.

Run the fuzzer (best with a sanitizer build, see the top of `tests/fuzz_files.cpp`):

```sh
cmake -B build-asan -DPAIRPAINT_SANITIZE=ON -DCMAKE_BUILD_TYPE=Debug && cmake --build build-asan
build-asan/tests/pairpaint_fuzz 10000 1 tests/data/psd-tools/*.psd tests/data/fuzz/*
```
