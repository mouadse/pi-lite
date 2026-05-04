#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "config.hpp"
#include "memory.hpp"

namespace {
using Clock = std::chrono::steady_clock;

long long micros_since(Clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count();
}

long long median(std::vector<long long> values) {
  std::sort(values.begin(), values.end());
  return values[values.size() / 2];
}

std::string memory_text(int i) {
  static const std::vector<std::string> areas = {
      "build system", "SQLite storage", "terminal UX", "tool routing", "project rules",
      "model configuration", "context compaction", "shell safety", "markdown rendering", "testing workflow"};
  static const std::vector<std::string> actions = {
      "prefers short status updates", "uses deterministic local paths", "keeps durable technical decisions",
      "avoids storing transient logs", "summarizes relevant implementation constraints", "remembers user workflow preferences",
      "captures repo-specific conventions", "tracks performance-sensitive code paths"};
  return "Memory " + std::to_string(i) + ": In " + areas[static_cast<std::size_t>(i) % areas.size()] +
         ", the agent " + actions[static_cast<std::size_t>(i * 7) % actions.size()] +
         " for component " + std::to_string((i * 37) % 997) +
         " with priority " + std::to_string((i * 13) % 29) + ".";
}

std::vector<std::string> categories_for(int i) {
  static const std::vector<std::string> cats = {"technical", "project", "preference", "rule", "configuration", "decision"};
  return {cats[static_cast<std::size_t>(i) % cats.size()]};
}

struct TrialMetrics {
  long long total_us = 0;
  long long seed_us = 0;
  long long search_us = 0;
  long long prompt_us = 0;
  long long save_us = 0;
  long long list_us = 0;
  int seed_saved = 0;
  int steady_saved = 0;
  int hits = 0;
  int context_bytes = 0;
};

TrialMetrics run_trial(int trial_index) {
  const std::filesystem::path db_path = std::filesystem::path("build") /
      ("autoresearch-memory-bench-" + std::to_string(trial_index) + ".sqlite3");
  std::filesystem::remove(db_path);
  std::filesystem::remove(db_path.string() + "-wal");
  std::filesystem::remove(db_path.string() + "-shm");

  pilite::AppConfig config;
  config.workspace = std::filesystem::current_path();
  config.memory_enabled = true;
  config.memory_path = db_path;
  config.memory_user_id = "bench-user";
  config.memory_agent_id = "pi-lite-bench";

  pilite::MemoryManager memory(config);
  TrialMetrics m;

  constexpr int seed_count = 1000;
  auto start = Clock::now();
  for (int i = 0; i < seed_count; ++i) {
    if (memory.save(memory_text(i), categories_for(i), {{"source", "bench"}, {"index", i}})) ++m.seed_saved;
  }
  m.seed_us = micros_since(start);

  const std::vector<std::string> queries = {
      "remember sqlite storage performance constraints",
      "what project rules affect shell safety",
      "terminal ux status update preference",
      "context compaction implementation notes",
      "model configuration local paths",
      "durable technical decisions for testing workflow",
      "markdown rendering component priority",
      "tool routing workflow preferences"};

  start = Clock::now();
  for (int i = 0; i < 96; ++i) {
    auto records = memory.search(queries[static_cast<std::size_t>(i) % queries.size()], {}, 5, 0.05);
    m.hits += static_cast<int>(records.size());
  }
  m.search_us = micros_since(start);

  start = Clock::now();
  for (int i = 0; i < 48; ++i) {
    auto context = memory.prompt_context(queries[static_cast<std::size_t>(i * 3) % queries.size()]);
    m.context_bytes += static_cast<int>(context.size());
  }
  m.prompt_us = micros_since(start);

  start = Clock::now();
  for (int i = 0; i < 32; ++i) {
    const int index = seed_count + trial_index * 1000 + i;
    if (memory.save(memory_text(index), categories_for(index), {{"source", "bench-steady"}, {"index", index}})) {
      ++m.steady_saved;
    }
  }
  m.save_us = micros_since(start);

  start = Clock::now();
  for (int i = 0; i < 32; ++i) {
    auto records = memory.list(i % 2 == 0 ? std::vector<std::string>{"technical"} : std::vector<std::string>{}, 50);
    m.hits += static_cast<int>(records.size());
  }
  m.list_us = micros_since(start);

  m.total_us = m.search_us + m.prompt_us + m.save_us + m.list_us;

  if (m.seed_saved != seed_count || m.steady_saved != 32 || m.hits <= 0 || m.context_bytes <= 0) {
    std::cerr << "benchmark correctness failure: seed_saved=" << m.seed_saved
              << " steady_saved=" << m.steady_saved << " hits=" << m.hits
              << " context_bytes=" << m.context_bytes << "\n";
    std::exit(2);
  }
  return m;
}

}  // namespace

int main() {
  std::vector<TrialMetrics> trials;
  for (int i = 0; i < 5; ++i) trials.push_back(run_trial(i));

  std::vector<long long> total, seed, search, prompt, save, list;
  for (const auto& t : trials) {
    total.push_back(t.total_us);
    seed.push_back(t.seed_us);
    search.push_back(t.search_us);
    prompt.push_back(t.prompt_us);
    save.push_back(t.save_us);
    list.push_back(t.list_us);
  }

  std::cout << "METRIC total_us=" << median(total) << "\n";
  std::cout << "METRIC seed_us=" << median(seed) << "\n";
  std::cout << "METRIC search_us=" << median(search) << "\n";
  std::cout << "METRIC prompt_us=" << median(prompt) << "\n";
  std::cout << "METRIC save_us=" << median(save) << "\n";
  std::cout << "METRIC list_us=" << median(list) << "\n";
  std::cout << "METRIC records=1032\n";
  return 0;
}
