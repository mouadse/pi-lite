# Autoresearch: faster local memory operations

## Objective
Optimize pi-lite's local long-term memory so saving and retrieving memories causes less UX latency. The target workload exercises the current SQLite-backed `MemoryManager` under a realistic steady-state agent profile: 1000 existing memories, repeated natural-language searches, prompt-context retrievals before model calls, category-filtered listing, and new memory saves that must still dedupe and persist events.

## Metrics
- **Primary**: `total_us` (µs, lower is better) — median steady-state memory operation time across 5 trials; sum of search, prompt-context, save, and list phases after seeding the database.
- **Secondary**: `search_us`, `prompt_us`, `save_us`, `list_us`, `seed_us`, `records` — phase timings and correctness guardrails.

## How to Run
`./autoresearch.sh` — builds the benchmark target and outputs `METRIC name=value` lines.

## Files in Scope
- `src/memory.cpp` — embedding, SQLite storage, search/list/save/update/delete, prompt memory context, memory tools.
- `src/memory.hpp` — memory data structures and public/private interfaces.
- `CMakeLists.txt` — benchmark target wiring only; avoid product build regressions.
- `benchmarks/memory_ops_bench.cpp` — representative benchmark harness; may be refined for better signal but must not be gamed.
- `autoresearch.sh`, `autoresearch.checks.sh`, `autoresearch.md`, `autoresearch.ideas.md` — experiment infrastructure and notes.

## Off Limits
- Do not remove safety filtering, dedupe behavior, event logging, scope isolation, category filtering, or prompt-context relevance.
- Do not special-case benchmark text, benchmark user IDs, paths, categories, or counts.
- Do not alter the benchmark to hide real regressions or reduce coverage just to improve the metric.
- Do not add new external dependencies.

## Constraints
- Product binary must still build with CMake.
- `autoresearch.checks.sh` runs the self-test with memory enabled after each benchmark; kept experiments must pass it.
- Preserve public memory tool behavior and output shape unless there is a clear compatible improvement.
- Prefer general data-structure, SQL, allocation, and algorithmic wins over benchmark-specific tuning.

## What's Been Tried
- Initial source read shows current search loads up to 5000 rows via `list()`, parses categories/metadata, converts vector BLOBs into `std::vector<float>`, computes cosine, then fully sorts all candidates. Save also performs a near-duplicate search before inserting, so save latency grows with database size.
- First baseline attempt measured `total_us=1,756,188`, but checks failed because the full product target exposed a pre-existing missing `#include <unistd.h>` for `STDERR_FILENO` in `src/agent.cpp`. Next run includes that build fix.
- Kept `std::partial_sort` for memory top-k ranking: `total_us=1,690,740` (~3.7% faster) with same comparator and candidate scan.
- Kept `records.reserve(requested_limit)` in list/search candidate collection: tiny additional win (`total_us=1,690,302`), probably near noise but allocation-free and semantically neutral.
- Discarded dot-product-only cosine despite normalized embeddings: theoretically lower arithmetic but measured worse than the partial-sort best; SQLite/materialization dominated at that point.
- Kept an in-process `MemoryStore` cache synced on add/update/delete: `total_us=169,003` (~90% faster than baseline), with search/prompt/list avoiding repeated SQLite reads and JSON/vector materialization. Main semantic risk: external DB modifications after cache load are not observed until restart.
