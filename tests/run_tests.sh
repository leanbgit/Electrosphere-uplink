#!/usr/bin/env bash
# Native unit tests for the runtime (Linux/macOS clang). Builds each test
# against runtime/ + runtime/hle/ (no game code) and runs it.
set -euo pipefail
cd "$(dirname "$0")/.."
B=build-tests; mkdir -p $B
src=$(ls runtime/*.c runtime/hle/*.c | grep -v ac3_oracle_frame.c)
for t in $(ls tests/test_*.c | grep -v test_stubs.c); do
  exe=$B/$(basename "$t" .c)
  clang -O1 -w -DAC3_INTERP_ONLY -Iruntime -Iruntime/hle $src platform/web/interp_only_stubs.c tests/test_stubs.c "$t" -o "$exe" -lm 2>&1 | grep -v '^$' || true
  "$exe"
done
