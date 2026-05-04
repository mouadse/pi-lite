#!/usr/bin/env bash
set -euo pipefail

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build --target pi-lite-memory-bench -j"$(nproc)" >/dev/null
./build/pi-lite-memory-bench
