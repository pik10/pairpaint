# Fuzzing inputs

- `seed-*.pairpaint`: small projects using every project-file feature, used as fuzzing seeds
  (together with the PSDs in `../psd-tools`).
- `hang-huge-layer-name.psd`: a corrupted PSD found by the fuzzer that made the reader loop for
  minutes (a layer name claiming ~4 billion characters past the end of the file). Kept as a
  regression test.

Run the fuzzer (best with a sanitizer build, see the top of `tests/fuzz_files.cpp`):

```sh
cmake -B build-asan -DPAIRPAINT_SANITIZE=ON -DCMAKE_BUILD_TYPE=Debug && cmake --build build-asan
build-asan/tests/pairpaint_fuzz 10000 1 tests/data/psd-tools/*.psd tests/data/fuzz/*
```
