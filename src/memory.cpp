#include "memory.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>

#include "messages.hpp"
#include "util.hpp"

namespace pilite {
namespace {

std::string now_iso8601() {
  const auto now = std::chrono::system_clock::now();
  const auto time = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
  gmtime_r(&time, &tm);
  std::ostringstream out;
  out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
  return out.str();
}

std::string random_id() {
  static thread_local std::mt19937_64 rng(std::random_device{}());
  std::uniform_int_distribution<unsigned long long> dist;
  std::ostringstream out;
  out << std::hex << std::setfill('0') << std::setw(16) << dist(rng) << std::setw(16) << dist(rng);
  return out.str();
}

std::string normalize_memory_text(std::string value) {
  value = to_lower(trim(std::move(value)));
  std::string out;
  bool was_space = false;
  for (const unsigned char c : value) {
    if (std::isspace(c)) {
      if (!was_space) out += ' ';
      was_space = true;
    } else {
      out += static_cast<char>(c);
      was_space = false;
    }
  }
  return trim(std::move(out));
}

std::string stable_hash(const std::string& value) {
  unsigned long long hash = 14695981039346656037ull;
  for (const unsigned char c : value) {
    hash ^= c;
    hash *= 1099511628211ull;
  }
  std::ostringstream out;
  out << std::hex << std::setfill('0') << std::setw(16) << hash;
  return out.str();
}

std::vector<std::string> tokenize(const std::string& text) {
  std::vector<std::string> tokens;
  std::string token;
  for (const unsigned char c : text) {
    if (std::isalnum(c) || c == '_' || c == '-' || c == '.') {
      token += static_cast<char>(std::tolower(c));
    } else if (!token.empty()) {
      tokens.push_back(std::move(token));
      token.clear();
    }
  }
  if (!token.empty()) tokens.push_back(std::move(token));
  return tokens;
}

std::size_t hash_token(const std::string& token) {
  unsigned long long hash = 14695981039346656037ull;
  for (const unsigned char c : token) {
    hash ^= c;
    hash *= 1099511628211ull;
  }
  return static_cast<std::size_t>(hash);
}

double vector_norm(const std::vector<float>& vector) {
  double norm = 0.0;
  for (const auto value : vector) norm += static_cast<double>(value) * static_cast<double>(value);
  return norm > 0.0 ? std::sqrt(norm) : 0.0;
}

std::vector<MemoryVectorEntry> sparse_vector(const std::vector<float>& vector) {
  std::vector<MemoryVectorEntry> sparse;
  sparse.reserve(64);
  for (std::size_t i = 0; i < vector.size(); ++i) {
    if (vector[i] != 0.0f) sparse.push_back({static_cast<std::uint16_t>(i), vector[i]});
  }
  return sparse;
}

double sparse_cosine_similarity(const std::vector<float>& query,
                                double query_norm,
                                const std::vector<MemoryVectorEntry>& vector,
                                double stored_norm) {
  if (query.empty() || vector.empty() || query_norm <= 0.0 || stored_norm <= 0.0) return 0.0;
  double dot = 0.0;
  for (const auto& entry : vector) {
    dot += static_cast<double>(query[entry.index]) * static_cast<double>(entry.value);
  }
  return dot / (query_norm * stored_norm);
}

std::string vector_to_blob(const std::vector<float>& vector) {
  std::string blob(vector.size() * sizeof(float), '\0');
  if (!vector.empty()) std::memcpy(blob.data(), vector.data(), blob.size());
  return blob;
}

std::vector<float> blob_to_vector(const void* data, int bytes) {
  if (!data || bytes <= 0 || bytes % static_cast<int>(sizeof(float)) != 0) return {};
  std::vector<float> vector(static_cast<std::size_t>(bytes) / sizeof(float));
  std::memcpy(vector.data(), data, static_cast<std::size_t>(bytes));
  return vector;
}

std::string json_array(const std::vector<std::string>& values) {
  return nlohmann::json(values).dump();
}

std::vector<std::string> parse_string_array(const std::string& value) {
  if (trim(value).empty()) return {};
  try {
    auto parsed = nlohmann::json::parse(value);
    std::vector<std::string> out;
    if (!parsed.is_array()) return out;
    for (const auto& item : parsed) {
      if (item.is_string() && !trim(item.get<std::string>()).empty()) out.push_back(trim(item.get<std::string>()));
    }
    return out;
  } catch (const nlohmann::json::parse_error&) {
    return {};
  }
}

nlohmann::json parse_json_object(const std::string& value) {
  if (trim(value).empty()) return nlohmann::json::object();
  try {
    auto parsed = nlohmann::json::parse(value);
    return parsed.is_object() ? parsed : nlohmann::json::object();
  } catch (const nlohmann::json::parse_error&) {
    return nlohmann::json::object();
  }
}

bool categories_match(const std::vector<std::string>& record_categories, const std::vector<std::string>& filters) {
  if (filters.empty()) return true;
  for (const auto& filter : filters) {
    if (std::find(record_categories.begin(), record_categories.end(), filter) != record_categories.end()) return true;
  }
  return false;
}

std::string category_or_default(const std::vector<std::string>& categories) {
  if (categories.empty()) return json_array({"technical"});
  std::vector<std::string> out;
  for (auto category : categories) {
    category = to_lower(trim(std::move(category)));
    if (!category.empty()) out.push_back(category);
  }
  if (out.empty()) out.push_back("technical");
  return json_array(out);
}

std::vector<std::string> normalize_categories(const std::vector<std::string>& categories) {
  return parse_string_array(category_or_default(categories));
}

std::vector<std::string> normalize_category_filter(const std::vector<std::string>& categories) {
  std::vector<std::string> out;
  for (auto category : categories) {
    category = to_lower(trim(std::move(category)));
    if (!category.empty()) out.push_back(category);
  }
  return out;
}

std::string sql_error(sqlite3* db) {
  return sqlite3_errmsg(db) ? sqlite3_errmsg(db) : "unknown sqlite error";
}

void bind_text(sqlite3_stmt* stmt, int index, const std::string& value) {
  sqlite3_bind_text(stmt, index, value.c_str(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
}

MemoryRecord record_from_stmt(sqlite3_stmt* stmt) {
  MemoryRecord record;
  record.id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
  record.memory = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
  record.hash = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
  record.vector = blob_to_vector(sqlite3_column_blob(stmt, 3), sqlite3_column_bytes(stmt, 3));
  record.sparse_vector = sparse_vector(record.vector);
  record.vector_norm = vector_norm(record.vector);
  record.user_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
  record.agent_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
  if (sqlite3_column_text(stmt, 6)) record.run_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
  record.categories = parse_string_array(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7)));
  record.metadata = parse_json_object(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 8)));
  record.created_at = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 9));
  record.updated_at = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 10));
  return record;
}

nlohmann::json object_schema(nlohmann::json properties, std::vector<std::string> required = {}) {
  return {
      {"type", "object"},
      {"properties", std::move(properties)},
      {"required", std::move(required)},
      {"additionalProperties", false},
  };
}

std::string required_string(const nlohmann::json& args, const char* key) {
  if (!args.contains(key) || !args.at(key).is_string()) throw std::runtime_error(std::string(key) + " must be a string");
  return args.at(key).get<std::string>();
}

std::vector<std::string> optional_categories(const nlohmann::json& args) {
  if (!args.contains("categories") || args.at("categories").is_null()) return {};
  if (args.at("categories").is_string()) return parse_string_array(args.at("categories").get<std::string>());
  if (!args.at("categories").is_array()) throw std::runtime_error("categories must be an array of strings");
  std::vector<std::string> categories;
  for (const auto& item : args.at("categories")) {
    if (!item.is_string()) throw std::runtime_error("categories must be an array of strings");
    categories.push_back(item.get<std::string>());
  }
  return categories;
}

int optional_int(const nlohmann::json& args, const char* key, int fallback) {
  if (!args.contains(key) || args.at(key).is_null()) return fallback;
  if (!args.at(key).is_number_integer()) throw std::runtime_error(std::string(key) + " must be an integer");
  return args.at(key).get<int>();
}

nlohmann::json optional_metadata(const nlohmann::json& args) {
  if (!args.contains("metadata") || args.at("metadata").is_null()) return nlohmann::json::object();
  if (!args.at("metadata").is_object()) throw std::runtime_error("metadata must be an object");
  return args.at("metadata");
}

std::optional<std::vector<std::string>> update_categories(const nlohmann::json& args) {
  if (!args.contains("categories") || args.at("categories").is_null()) return std::nullopt;
  return optional_categories(args);
}

std::optional<nlohmann::json> update_metadata(const nlohmann::json& args) {
  if (!args.contains("metadata") || args.at("metadata").is_null()) return std::nullopt;
  return optional_metadata(args);
}

std::string format_memory_records(const std::vector<MemoryRecord>& records) {
  if (records.empty()) return "No memories found.\n";
  std::ostringstream out;
  for (const auto& record : records) {
    out << "- id: " << record.id << "\n";
    out << "  memory: " << record.memory << "\n";
    out << "  score: " << std::fixed << std::setprecision(3) << record.score << "\n";
    out << "  categories: " << json_array(record.categories) << "\n";
    out << "  updated_at: " << record.updated_at << "\n";
  }
  return out.str();
}

std::string strip_code_blocks(std::string text) {
  const std::string fence = "```";
  std::size_t pos = 0;
  while ((pos = text.find(fence, pos)) != std::string::npos) {
    const auto end = text.find(fence, pos + fence.size());
    if (end == std::string::npos) break;
    auto inner = text.substr(pos + fence.size(), end - pos - fence.size());
    const auto newline = inner.find('\n');
    if (newline != std::string::npos && inner.substr(0, newline).find('{') == std::string::npos) {
      inner = inner.substr(newline + 1);
    }
    text.replace(pos, end + fence.size() - pos, inner);
    pos += inner.size();
  }
  while ((pos = text.find("<think>")) != std::string::npos) {
    const auto end = text.find("</think>", pos);
    if (end == std::string::npos) break;
    text.erase(pos, end + 8 - pos);
  }
  return text;
}

std::optional<nlohmann::json> extract_json_object(std::string text) {
  text = strip_code_blocks(std::move(text));
  try {
    auto parsed = nlohmann::json::parse(text);
    if (parsed.is_object()) return parsed;
  } catch (const nlohmann::json::parse_error&) {
  }

  const auto start = text.find('{');
  if (start == std::string::npos) return std::nullopt;
  int depth = 0;
  bool in_string = false;
  bool escape = false;
  for (std::size_t i = start; i < text.size(); ++i) {
    const char c = text[i];
    if (in_string) {
      if (escape) {
        escape = false;
      } else if (c == '\\') {
        escape = true;
      } else if (c == '"') {
        in_string = false;
      }
      continue;
    }
    if (c == '"') {
      in_string = true;
    } else if (c == '{') {
      ++depth;
    } else if (c == '}' && --depth == 0) {
      try {
        auto parsed = nlohmann::json::parse(text.substr(start, i - start + 1));
        if (parsed.is_object()) return parsed;
      } catch (const nlohmann::json::parse_error&) {
        return std::nullopt;
      }
    }
  }
  return std::nullopt;
}

struct ExtractedFact {
  std::string memory;
  std::vector<std::string> categories;
};

std::vector<ExtractedFact> parse_facts(const std::string& response) {
  const auto parsed = extract_json_object(response);
  if (!parsed || !parsed->contains("facts") || !parsed->at("facts").is_array()) return {};
  std::vector<ExtractedFact> facts;
  for (const auto& item : parsed->at("facts")) {
    ExtractedFact fact;
    if (item.is_string()) {
      fact.memory = item.get<std::string>();
    } else if (item.is_object()) {
      for (const auto* key : {"memory", "fact", "text"}) {
        if (item.contains(key) && item.at(key).is_string()) {
          fact.memory = item.at(key).get<std::string>();
          break;
        }
      }
      if (item.contains("categories") && item.at("categories").is_array()) {
        for (const auto& category : item.at("categories")) {
          if (category.is_string()) fact.categories.push_back(category.get<std::string>());
        }
      }
    }
    fact.memory = trim(std::move(fact.memory));
    if (!fact.memory.empty()) facts.push_back(std::move(fact));
  }
  return facts;
}

}  // namespace

HashEmbedder::HashEmbedder(std::size_t dimensions) : dimensions_(std::max<std::size_t>(64, dimensions)) {}

std::vector<float> HashEmbedder::embed(const std::string& text) const {
  std::vector<float> vector(dimensions_, 0.0f);
  const auto tokens = tokenize(text);
  for (std::size_t i = 0; i < tokens.size(); ++i) {
    const auto h = hash_token(tokens[i]);
    vector[h % dimensions_] += (h & 1u) ? 1.0f : -1.0f;
    if (i + 1 < tokens.size()) {
      const auto bh = hash_token(tokens[i] + " " + tokens[i + 1]);
      vector[bh % dimensions_] += (bh & 1u) ? 0.5f : -0.5f;
    }
  }
  double norm = 0.0;
  for (const auto value : vector) norm += static_cast<double>(value) * static_cast<double>(value);
  if (norm <= 0.0) return vector;
  const auto scale = static_cast<float>(1.0 / std::sqrt(norm));
  for (auto& value : vector) value *= scale;
  return vector;
}

MemoryStore::MemoryStore(std::filesystem::path path) : path_(std::move(path)) {
  if (!path_.parent_path().empty()) std::filesystem::create_directories(path_.parent_path());
  if (sqlite3_open(path_.string().c_str(), &db_) != SQLITE_OK) {
    const auto error = sql_error(db_);
    sqlite3_close(db_);
    db_ = nullptr;
    throw std::runtime_error("Could not open memory database: " + error);
  }
  initialize();
}

MemoryStore::~MemoryStore() {
  if (db_) sqlite3_close(db_);
}

void MemoryStore::exec(const std::string& sql) const {
  char* error = nullptr;
  if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
    std::string message = error ? error : "unknown sqlite error";
    sqlite3_free(error);
    throw std::runtime_error(message);
  }
}

void MemoryStore::initialize() {
  exec("PRAGMA journal_mode=WAL");
  exec("PRAGMA synchronous=NORMAL");
  exec("CREATE TABLE IF NOT EXISTS memories("
       "id TEXT PRIMARY KEY,"
       "memory TEXT NOT NULL,"
       "hash TEXT NOT NULL,"
       "vector BLOB NOT NULL,"
       "user_id TEXT NOT NULL,"
       "agent_id TEXT NOT NULL,"
       "run_id TEXT,"
       "categories TEXT NOT NULL,"
       "metadata TEXT NOT NULL,"
       "created_at TEXT NOT NULL,"
       "updated_at TEXT NOT NULL)");
  exec("CREATE TABLE IF NOT EXISTS memory_events("
       "id TEXT PRIMARY KEY,"
       "memory_id TEXT NOT NULL,"
       "event TEXT NOT NULL,"
       "old_memory TEXT,"
       "new_memory TEXT,"
       "actor_id TEXT,"
       "created_at TEXT NOT NULL)");
  exec("DROP INDEX IF EXISTS idx_memories_scope");
  exec("DROP INDEX IF EXISTS idx_memories_hash");
  exec("CREATE INDEX IF NOT EXISTS idx_memories_updated_at ON memories(updated_at)");
}

void MemoryStore::log_event(const std::string& memory_id,
                            const std::string& event,
                            const std::string& old_memory,
                            const std::string& new_memory) const {
  sqlite3_stmt* stmt = nullptr;
  const char* sql = "INSERT INTO memory_events(id,memory_id,event,old_memory,new_memory,actor_id,created_at) VALUES(?,?,?,?,?,?,?)";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) throw std::runtime_error(sql_error(db_));
  bind_text(stmt, 1, random_id());
  bind_text(stmt, 2, memory_id);
  bind_text(stmt, 3, event);
  bind_text(stmt, 4, old_memory);
  bind_text(stmt, 5, new_memory);
  bind_text(stmt, 6, "pi-lite");
  bind_text(stmt, 7, now_iso8601());
  const int code = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (code != SQLITE_DONE) throw std::runtime_error(sql_error(db_));
}

const std::vector<MemoryRecord>& MemoryStore::cached_records() const {
  if (cache_loaded_) return records_cache_;

  sqlite3_stmt* stmt = nullptr;
  const char* sql = "SELECT id,memory,hash,vector,user_id,agent_id,run_id,categories,metadata,created_at,updated_at "
                    "FROM memories ORDER BY updated_at ASC";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) throw std::runtime_error(sql_error(db_));
  records_cache_.clear();
  while (sqlite3_step(stmt) == SQLITE_ROW) records_cache_.push_back(record_from_stmt(stmt));
  sqlite3_finalize(stmt);
  cache_loaded_ = true;
  return records_cache_;
}

void MemoryStore::upsert_cached_record(MemoryRecord record) const {
  if (!cache_loaded_) return;
  erase_cached_record(record.id);
  records_cache_.push_back(std::move(record));
}

void MemoryStore::erase_cached_record(const std::string& id) const {
  if (!cache_loaded_) return;
  records_cache_.erase(std::remove_if(records_cache_.begin(), records_cache_.end(), [&](const MemoryRecord& record) {
                         return record.id == id;
                       }),
                       records_cache_.end());
}

bool MemoryStore::add(MemoryRecord record) {
  sqlite3_stmt* stmt = nullptr;
  const char* sql = "INSERT INTO memories(id,memory,hash,vector,user_id,agent_id,run_id,categories,metadata,created_at,updated_at) "
                    "VALUES(?,?,?,?,?,?,?,?,?,?,?)";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) throw std::runtime_error(sql_error(db_));
  const auto blob = vector_to_blob(record.vector);
  bind_text(stmt, 1, record.id);
  bind_text(stmt, 2, record.memory);
  bind_text(stmt, 3, record.hash);
  sqlite3_bind_blob(stmt, 4, blob.data(), static_cast<int>(blob.size()), SQLITE_TRANSIENT);
  bind_text(stmt, 5, record.user_id);
  bind_text(stmt, 6, record.agent_id);
  bind_text(stmt, 7, record.run_id);
  bind_text(stmt, 8, json_array(record.categories));
  bind_text(stmt, 9, record.metadata.dump());
  bind_text(stmt, 10, record.created_at);
  bind_text(stmt, 11, record.updated_at);
  const int code = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (code == SQLITE_CONSTRAINT) return false;
  if (code != SQLITE_DONE) throw std::runtime_error(sql_error(db_));
  log_event(record.id, "ADD", "", record.memory);
  upsert_cached_record(std::move(record));
  return true;
}

bool MemoryStore::update(const std::string& id,
                         const std::string& memory,
                         const std::vector<float>& vector,
                         const std::vector<std::string>& categories,
                         const nlohmann::json& metadata) {
  const auto existing = get(id);
  if (!existing) return false;
  sqlite3_stmt* stmt = nullptr;
  const char* sql = "UPDATE memories SET memory=?, hash=?, vector=?, categories=?, metadata=?, updated_at=? WHERE id=?";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) throw std::runtime_error(sql_error(db_));
  const auto normalized = normalize_memory_text(memory);
  const auto blob = vector_to_blob(vector);
  bind_text(stmt, 1, memory);
  bind_text(stmt, 2, stable_hash(normalized));
  sqlite3_bind_blob(stmt, 3, blob.data(), static_cast<int>(blob.size()), SQLITE_TRANSIENT);
  bind_text(stmt, 4, json_array(normalize_categories(categories)));
  bind_text(stmt, 5, metadata.is_object() ? metadata.dump() : nlohmann::json::object().dump());
  bind_text(stmt, 6, now_iso8601());
  bind_text(stmt, 7, id);
  const int code = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (code != SQLITE_DONE) throw std::runtime_error(sql_error(db_));
  const bool changed = sqlite3_changes(db_) > 0;
  if (changed) {
    auto updated = *existing;
    updated.memory = memory;
    updated.hash = stable_hash(normalized);
    updated.vector = vector;
    updated.sparse_vector = sparse_vector(updated.vector);
    updated.vector_norm = vector_norm(updated.vector);
    updated.categories = normalize_categories(categories);
    updated.metadata = metadata.is_object() ? metadata : nlohmann::json::object();
    updated.updated_at = now_iso8601();
    upsert_cached_record(std::move(updated));
  }
  log_event(id, "UPDATE", existing->memory, memory);
  return changed;
}

bool MemoryStore::remove(const std::string& id) {
  const auto existing = get(id);
  if (!existing) return false;
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, "DELETE FROM memories WHERE id=?", -1, &stmt, nullptr) != SQLITE_OK) {
    throw std::runtime_error(sql_error(db_));
  }
  bind_text(stmt, 1, id);
  const int code = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (code != SQLITE_DONE) throw std::runtime_error(sql_error(db_));
  const bool changed = sqlite3_changes(db_) > 0;
  if (changed) erase_cached_record(id);
  log_event(id, "DELETE", existing->memory, "");
  return changed;
}

std::optional<MemoryRecord> MemoryStore::get(const std::string& id) const {
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, "SELECT id,memory,hash,vector,user_id,agent_id,run_id,categories,metadata,created_at,updated_at FROM memories WHERE id=?", -1, &stmt, nullptr) != SQLITE_OK) {
    throw std::runtime_error(sql_error(db_));
  }
  bind_text(stmt, 1, id);
  const int code = sqlite3_step(stmt);
  if (code == SQLITE_ROW) {
    auto record = record_from_stmt(stmt);
    sqlite3_finalize(stmt);
    return record;
  }
  sqlite3_finalize(stmt);
  return std::nullopt;
}

std::vector<MemoryRecord> MemoryStore::list(const std::string& user_id,
                                            const std::string& agent_id,
                                            const std::string& run_id,
                                            const std::vector<std::string>& categories,
                                            int limit) const {
  const auto requested_limit = std::clamp(limit, 1, 500);
  const auto scan_limit = categories.empty() ? requested_limit : 5000;
  std::vector<MemoryRecord> records;
  records.reserve(static_cast<std::size_t>(requested_limit));
  int scanned = 0;
  const auto& cache = cached_records();
  for (auto it = cache.rbegin(); it != cache.rend(); ++it) {
    const auto& record = *it;
    if (record.user_id != user_id || record.agent_id != agent_id) continue;
    if (!run_id.empty() && record.run_id != run_id) continue;
    if (++scanned > scan_limit) break;
    if (categories_match(record.categories, categories)) records.push_back(record);
    if (records.size() >= static_cast<std::size_t>(requested_limit)) break;
  }
  return records;
}

std::vector<MemoryRecord> MemoryStore::search(const std::vector<float>& query,
                                              const std::string& user_id,
                                              const std::string& agent_id,
                                              const std::string& run_id,
                                              const std::vector<std::string>& categories,
                                              int top_k,
                                              double threshold) const {
  const auto requested = static_cast<std::size_t>(std::max(1, top_k));
  const auto better = [](const MemoryRecord& left, const MemoryRecord& right) {
    if (left.score == right.score) return left.updated_at > right.updated_at;
    return left.score > right.score;
  };
  std::vector<MemoryRecord> best;
  best.reserve(requested);
  int scoped_seen = 0;
  int category_matches = 0;
  const double query_norm = vector_norm(query);
  const auto& cache = cached_records();
  for (auto it = cache.rbegin(); it != cache.rend(); ++it) {
    const auto& record = *it;
    if (record.user_id != user_id || record.agent_id != agent_id) continue;
    if (!run_id.empty() && record.run_id != run_id) continue;
    if (++scoped_seen > 5000) break;
    if (!categories_match(record.categories, categories)) continue;
    if (++category_matches > 500) break;
    const double score = sparse_cosine_similarity(query, query_norm, record.sparse_vector, record.vector_norm);
    if (score < threshold) continue;

    MemoryRecord candidate;
    if (best.size() < requested) {
      candidate = record;
      candidate.score = score;
      best.push_back(std::move(candidate));
      continue;
    }

    auto worst = best.begin();
    for (auto it = std::next(best.begin()); it != best.end(); ++it) {
      if (better(*worst, *it)) worst = it;
    }
    candidate.score = score;
    candidate.updated_at = record.updated_at;
    if (better(candidate, *worst)) {
      candidate = record;
      candidate.score = score;
      *worst = std::move(candidate);
    }
  }
  std::sort(best.begin(), best.end(), better);
  return best;
}

bool MemoryStore::has_similar(const std::vector<float>& query,
                              const std::string& user_id,
                              const std::string& agent_id,
                              const std::string& run_id,
                              double threshold) const {
  const double query_norm = vector_norm(query);
  for (const auto& record : cached_records()) {
    if (record.user_id != user_id || record.agent_id != agent_id) continue;
    if (!run_id.empty() && record.run_id != run_id) continue;
    if (sparse_cosine_similarity(query, query_norm, record.sparse_vector, record.vector_norm) >= threshold) return true;
  }
  return false;
}

MemoryManager::MemoryManager(AppConfig config)
    : config_(std::move(config)), embedder_(512), store_(config_.memory_path) {}

bool MemoryManager::is_valid_scope() const {
  return !trim(config_.memory_user_id).empty() && !trim(config_.memory_agent_id).empty();
}

bool MemoryManager::is_safe_memory(const std::string& memory) const {
  const auto lower = to_lower(memory);
  if (memory.size() > 2000) return false;
  static const std::vector<std::string> blocked = {
      "api_key",      "apikey",      "password",   "passwd",     "authorization:",
      "bearer ",      "secret",      "token=",     "access_token", "refresh_token",
      "private key",  "-----begin",  "webhook",    "ssh-rsa",     "sk-"};
  return std::none_of(blocked.begin(), blocked.end(), [&](const std::string& needle) {
    return lower.find(needle) != std::string::npos;
  });
}

bool MemoryManager::owns_record(const MemoryRecord& record) const {
  return record.user_id == config_.memory_user_id && record.agent_id == config_.memory_agent_id &&
         (config_.memory_run_id.empty() || record.run_id == config_.memory_run_id);
}

bool MemoryManager::has_near_duplicate(const std::string& memory, double threshold) const {
  return store_.has_similar(embedder_.embed(memory), config_.memory_user_id, config_.memory_agent_id, config_.memory_run_id, threshold);
}

bool MemoryManager::save(const std::string& memory,
                         const std::vector<std::string>& categories,
                         const nlohmann::json& metadata) {
  const auto trimmed = trim(memory);
  if (!enabled() || !is_valid_scope() || trimmed.empty() || !is_safe_memory(trimmed)) return false;
  if (has_near_duplicate(trimmed, 0.92)) return false;

  const auto timestamp = now_iso8601();
  MemoryRecord record;
  record.id = random_id();
  record.memory = trimmed;
  record.hash = stable_hash(normalize_memory_text(trimmed));
  record.vector = embedder_.embed(trimmed);
  record.sparse_vector = sparse_vector(record.vector);
  record.vector_norm = vector_norm(record.vector);
  record.user_id = config_.memory_user_id;
  record.agent_id = config_.memory_agent_id;
  record.run_id = config_.memory_run_id;
  record.categories = normalize_categories(categories);
  record.metadata = metadata.is_object() ? metadata : nlohmann::json::object();
  record.created_at = timestamp;
  record.updated_at = timestamp;
  return store_.add(std::move(record));
}

bool MemoryManager::update(const std::string& id,
                           const std::string& memory,
                           const std::optional<std::vector<std::string>>& categories,
                           const std::optional<nlohmann::json>& metadata) {
  const auto trimmed = trim(memory);
  if (!enabled() || !is_valid_scope() || id.empty() || trimmed.empty() || !is_safe_memory(trimmed)) return false;
  const auto existing = store_.get(id);
  if (!existing || !owns_record(*existing)) return false;
  const auto next_categories = categories ? normalize_categories(*categories) : existing->categories;
  const auto next_metadata =
      metadata ? (metadata->is_object() ? *metadata : nlohmann::json::object()) : existing->metadata;
  return store_.update(id, trimmed, embedder_.embed(trimmed), next_categories, next_metadata);
}

bool MemoryManager::remove(const std::string& id) {
  if (!enabled() || id.empty()) return false;
  const auto existing = store_.get(id);
  if (!existing || !owns_record(*existing)) return false;
  return store_.remove(id);
}

std::optional<MemoryRecord> MemoryManager::get(const std::string& id) const {
  if (!enabled() || id.empty()) return std::nullopt;
  auto record = store_.get(id);
  if (!record || !owns_record(*record)) return std::nullopt;
  return record;
}

std::vector<MemoryRecord> MemoryManager::search(const std::string& query,
                                                const std::vector<std::string>& categories,
                                                int top_k,
                                                double threshold) const {
  if (!enabled() || !is_valid_scope() || trim(query).empty()) return {};
  return store_.search(embedder_.embed(query), config_.memory_user_id, config_.memory_agent_id, config_.memory_run_id,
                       normalize_category_filter(categories), std::clamp(top_k, 1, 20), std::clamp(threshold, 0.0, 1.0));
}

std::vector<MemoryRecord> MemoryManager::list(const std::vector<std::string>& categories, int limit) const {
  if (!enabled() || !is_valid_scope()) return {};
  return store_.list(config_.memory_user_id, config_.memory_agent_id, config_.memory_run_id, normalize_category_filter(categories),
                     std::clamp(limit, 1, 500));
}

std::string MemoryManager::prompt_context(const std::string& query) const {
  const auto records = search(query, {}, 5, 0.10);
  if (records.empty()) return "";
  std::ostringstream out;
  out << "Relevant long-term memory:\n";
  std::size_t bytes = 0;
  for (const auto& record : records) {
    const std::string categories = record.categories.empty() ? "technical" : record.categories.front();
    const auto line = "- [" + categories + "] " + truncate_line(record.memory, 260) + "\n";
    if (bytes + line.size() > 1600) break;
    out << line;
    bytes += line.size();
  }
  return out.str();
}

int MemoryManager::capture_turn(const std::string& user_prompt,
                                const std::string& assistant_content,
                                const LlmClient& client) {
  if (!enabled() || !auto_capture_enabled() || !is_valid_scope() || trim(assistant_content).empty()) return 0;

  const std::string system =
      "You extract long-term memories for a small coding agent. Return only valid JSON.\n"
      "Store only durable facts useful days or weeks later. Candidate facts must pass future utility, novelty, factualness, and safety.\n"
      "Never store secrets, tokens, passwords, raw tool output, raw code, logs, one-off commands, or transient status.\n"
      "Prefer 15-50 word self-contained third-person facts. Use categories from: preference, rule, project, configuration, decision, technical, lesson.\n"
      "Return schema: {\"facts\":[{\"memory\":\"...\",\"categories\":[\"technical\"]}]}. Return {\"facts\":[]} when nothing should be saved.";

  std::ostringstream transcript;
  transcript << "User prompt:\n" << truncate_head(user_prompt, 6000, 80) << "\n\nAssistant answer:\n"
             << truncate_head(assistant_content, 6000, 80);

  Message response;
  try {
    response = client.complete(system, {user_message(transcript.str())}, nlohmann::json::array());
  } catch (const std::exception& error) {
    std::cerr << "[memory] auto-capture failed: " << error.what() << "\n";
    return 0;
  }

  int saved = 0;
  for (const auto& fact : parse_facts(response.content)) {
    if (save(fact.memory, fact.categories, {{"source", "auto_capture"}})) ++saved;
  }
  return saved;
}

void register_memory_tools(ToolRegistry& registry, std::shared_ptr<MemoryManager> memory) {
  registry.add(Tool{
      .name = "memory_save",
      .description = "Save a durable, non-secret long-term memory for future coding-agent sessions.",
      .parameters = object_schema({
          {"memory", {{"type", "string"}, {"description", "Self-contained durable fact to remember."}}},
          {"categories", {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", "Optional categories."}}},
          {"metadata", {{"type", "object"}, {"description", "Optional metadata."}}},
      }, {"memory"}),
      .execute = [memory](const nlohmann::json& args) {
        if (!memory || !memory->enabled()) return ToolResult{.content = "Memory is disabled.", .is_error = true};
        const auto saved = memory->save(required_string(args, "memory"), optional_categories(args), optional_metadata(args));
        return ToolResult{.content = saved ? "Memory saved.\n" : "Memory was not saved. It may be unsafe, duplicate, or invalid.\n", .is_error = !saved};
      },
  });

  registry.add(Tool{
      .name = "memory_search",
      .description = "Search long-term memory by natural language query.",
      .parameters = object_schema({
          {"query", {{"type", "string"}, {"description", "Search query."}}},
          {"categories", {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", "Optional category filters."}}},
          {"top_k", {{"type", "integer"}, {"description", "Maximum memories to return."}, {"default", 5}}},
      }, {"query"}),
      .execute = [memory](const nlohmann::json& args) {
        if (!memory || !memory->enabled()) return ToolResult{.content = "Memory is disabled.", .is_error = true};
        return ToolResult{.content = format_memory_records(memory->search(required_string(args, "query"), optional_categories(args), optional_int(args, "top_k", 5), 0.10))};
      },
  });

  registry.add(Tool{
      .name = "memory_list",
      .description = "List recent long-term memories.",
      .parameters = object_schema({
          {"categories", {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", "Optional category filters."}}},
          {"limit", {{"type", "integer"}, {"description", "Maximum memories to return."}, {"default", 50}}},
      }),
      .execute = [memory](const nlohmann::json& args) {
        if (!memory || !memory->enabled()) return ToolResult{.content = "Memory is disabled.", .is_error = true};
        return ToolResult{.content = format_memory_records(memory->list(optional_categories(args), optional_int(args, "limit", 50)))};
      },
  });

  registry.add(Tool{
      .name = "memory_update",
      .description = "Update an existing memory by ID.",
      .parameters = object_schema({
          {"id", {{"type", "string"}, {"description", "Memory ID."}}},
          {"memory", {{"type", "string"}, {"description", "Updated self-contained durable fact."}}},
          {"categories", {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", "Optional categories."}}},
          {"metadata", {{"type", "object"}, {"description", "Optional metadata."}}},
      }, {"id", "memory"}),
      .execute = [memory](const nlohmann::json& args) {
        if (!memory || !memory->enabled()) return ToolResult{.content = "Memory is disabled.", .is_error = true};
        const auto updated = memory->update(required_string(args, "id"),
                                            required_string(args, "memory"),
                                            update_categories(args),
                                            update_metadata(args));
        return ToolResult{.content = updated ? "Memory updated.\n" : "Memory was not updated.\n", .is_error = !updated};
      },
  });

  registry.add(Tool{
      .name = "memory_delete",
      .description = "Delete a long-term memory by ID.",
      .parameters = object_schema({
          {"id", {{"type", "string"}, {"description", "Memory ID."}}},
      }, {"id"}),
      .execute = [memory](const nlohmann::json& args) {
        if (!memory || !memory->enabled()) return ToolResult{.content = "Memory is disabled.", .is_error = true};
        const auto removed = memory->remove(required_string(args, "id"));
        return ToolResult{.content = removed ? "Memory deleted.\n" : "Memory was not found.\n", .is_error = !removed};
      },
  });
}

}  // namespace pilite
