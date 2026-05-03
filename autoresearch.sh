#!/usr/bin/env bash
set -euo pipefail

cmake -S . -B build/autoresearch -DCMAKE_BUILD_TYPE=Release >/tmp/pi-lite-autoresearch-cmake.log
cmake --build build/autoresearch -j2 >/tmp/pi-lite-autoresearch-build.log

python3 - <<'PY'
import os
import statistics
import subprocess
import time

cmd = ["./build/autoresearch/pi-lite", "--self-test"]
times = []
passes = 0
runs = 7
last_output = ""
for _ in range(runs):
    start = time.perf_counter()
    proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    elapsed_ms = (time.perf_counter() - start) * 1000.0
    times.append(elapsed_ms)
    last_output = proc.stdout
    if proc.returncode == 0:
        passes += 1

median_ms = statistics.median(times)
all_pass = 1 if passes == runs else 0
score = (1_000_000.0 - median_ms) if all_pass else 0.0
binary_kb = os.path.getsize("./build/autoresearch/pi-lite") / 1024.0

print(last_output.rstrip())
print(f"METRIC agent_score={score:.3f}")
print(f"METRIC selftest_ms={median_ms:.3f}")
print(f"METRIC selftest_pass={all_pass}")
print(f"METRIC binary_kb={binary_kb:.3f}")
if not all_pass:
    print(f"ASI selftest_passes={passes}/{runs}")
PY
