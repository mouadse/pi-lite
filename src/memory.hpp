#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "llm_client.hpp"
#include "tool.hpp"

struct sqlite3;

namespace pilite {

struct MemoryRecord {
  std::string id;
  std::string memory;
  std::string hash;
  std::vector<float> vector;
  std::string user_id;
  std::string agent_id;
  std::string run_id;
  std::vector<std::string> categories;
  nlohmann::json metadata = nlohmann::json::object();
  std::string created_at;
  std::string updated_at;
  double score = 0.0;
};

class HashEmbedder {
 public:
  explicit HashEmbedder(std::size_t dimensions = 512);

  std::vector<float> embed(const std::string& text) const;
  std::size_t dimensions() const { return dimensions_; }

 private:
  std::size_t dimensions_;
};

class MemoryStore {
 public:
  explicit MemoryStore(std::filesystem::path path);
  ~MemoryStore();

  MemoryStore(const MemoryStore&) = delete;
  MemoryStore& operator=(const MemoryStore&) = delete;

  bool add(MemoryRecord record);
  bool update(const std::string& id,
              const std::string& memory,
              const std::vector<float>& vector,
              const std::vector<std::string>& categories,
              const nlohmann::json& metadata);
  bool remove(const std::string& id);
  std::optional<MemoryRecord> get(const std::string& id) const;
  std::vector<MemoryRecord> list(const std::string& user_id,
                                 const std::string& agent_id,
                                 const std::string& run_id,
                                 const std::vector<std::string>& categories,
                                 int limit) const;
  std::vector<MemoryRecord> search(const std::vector<float>& query,
                                   const std::string& user_id,
                                   const std::string& agent_id,
                                   const std::string& run_id,
                                   const std::vector<std::string>& categories,
                                   int top_k,
                                   double threshold) const;

 private:
  void initialize();
  void exec(const std::string& sql) const;
  void log_event(const std::string& memory_id,
                 const std::string& event,
                 const std::string& old_memory,
                 const std::string& new_memory) const;
  const std::vector<MemoryRecord>& cached_records() const;
  void upsert_cached_record(MemoryRecord record) const;
  void erase_cached_record(const std::string& id) const;

  std::filesystem::path path_;
  sqlite3* db_ = nullptr;
  mutable bool cache_loaded_ = false;
  mutable std::vector<MemoryRecord> records_cache_;
};

class MemoryManager {
 public:
  explicit MemoryManager(AppConfig config);

  bool enabled() const { return config_.memory_enabled; }
  bool auto_capture_enabled() const { return config_.memory_auto_capture; }
  const std::string& user_id() const { return config_.memory_user_id; }
  const std::string& agent_id() const { return config_.memory_agent_id; }
  const std::string& run_id() const { return config_.memory_run_id; }

  std::vector<MemoryRecord> search(const std::string& query,
                                   const std::vector<std::string>& categories = {},
                                   int top_k = 5,
                                   double threshold = 0.10) const;
  std::vector<MemoryRecord> list(const std::vector<std::string>& categories = {}, int limit = 50) const;
  std::optional<MemoryRecord> get(const std::string& id) const;
  bool save(const std::string& memory,
            const std::vector<std::string>& categories = {},
            const nlohmann::json& metadata = nlohmann::json::object());
  bool update(const std::string& id,
              const std::string& memory,
              const std::optional<std::vector<std::string>>& categories = std::nullopt,
              const std::optional<nlohmann::json>& metadata = std::nullopt);
  bool remove(const std::string& id);

  std::string prompt_context(const std::string& query) const;
  int capture_turn(const std::string& user_prompt,
                   const std::string& assistant_content,
                   const LlmClient& client);

 private:
  bool is_valid_scope() const;
  bool is_safe_memory(const std::string& memory) const;
  bool owns_record(const MemoryRecord& record) const;
  std::vector<MemoryRecord> near_duplicates(const std::string& memory, double threshold) const;

  AppConfig config_;
  HashEmbedder embedder_;
  MemoryStore store_;
};

void register_memory_tools(ToolRegistry& registry, std::shared_ptr<MemoryManager> memory);

}  // namespace pilite
