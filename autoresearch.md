# Autoresearch: Improve pi-lite coding agent

## Objective
Improve `pi-lite`, a tiny C++ coding agent. The workload is local and deterministic: build the Release binary, run its built-in tool smoke test, and measure the agent's local startup/tool-path responsiveness. Correctness matters first: a failing smoke test gets score 0; passing runs are ranked by lower median self-test latency.

## Metrics
- **Primary**: `agent_score` (unitless, higher is better) — `1,000,000 - median_selftest_ms` when the built-in self-test passes, otherwise `0`.
- **Secondary**:
  - `selftest_ms` — median wall-clock milliseconds for `./build/autoresearch/pi-lite --self-test` across repeated runs (lower is better).
  - `selftest_pass` — 1 if every repeated self-test run exits 0, else 0.
  - `binary_kb` — Release binary size in KiB.

## How to Run
`./autoresearch.sh` — builds Release in `build/autoresearch`, runs repeated self-tests, and outputs `METRIC name=value` lines.

## Files in Scope
- `src/*.cpp`, `src/*.hpp` — agent, LLM client, renderer, tools, config, utility code.
- `CMakeLists.txt` — build flags and target wiring if needed for safe performance/quality changes.
- `README.md` — update only if behavior changes.
- `autoresearch.sh`, `autoresearch.md`, `autoresearch.ideas.md` — experiment harness and notes.

## Off Limits
- Do not read or depend on `.env` secrets except through the program's existing config behavior.
- Do not remove or weaken safety checks around sensitive files, workspace boundaries, or destructive shell commands.
- Do not game `autoresearch.sh`, hard-code benchmark outputs, skip meaningful work, or make the self-test falsely pass.
- Do not add network-dependent benchmark steps; keep experiments local and deterministic.

## Constraints
- Preserve intended CLI behavior from `README.md`.
- Keep dependencies unchanged unless there is a strong, documented reason.
- Prefer simple, maintainable changes over tiny wins with complexity.
- The built-in self-test must pass for a useful improvement.

## What's Been Tried
- Baseline score was 0 because `--self-test` failed: `grep_files` could not find its explicit `.pi-lite-self-test.tmp` path since the git manifest excludes ignored `*.tmp` files.
- Kept fix: explicit regular-file `grep_files` searches now bypass git manifest filtering while directory searches still use git-aware manifests. Self-test passes 7/7.
