#!/usr/bin/env bash
set -euo pipefail

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build --target pi-lite pi-lite-memory-bench -j"$(nproc)" >/dev/null
rm -f build/autoresearch-self-test.sqlite3 build/autoresearch-self-test.sqlite3-wal build/autoresearch-self-test.sqlite3-shm
PI_LITE_BASE_URL=http://localhost:11434/v1 \
PI_LITE_MEMORY=1 \
PI_LITE_MEMORY_PATH=build/autoresearch-self-test.sqlite3 \
./build/pi-lite --self-test --memory >/dev/null
