#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>

namespace pilite {

struct AppConfig {
  std::filesystem::path workspace;
  std::string base_url;
  std::string api_key;
  std::string model;
  bool allow_bash = false;
  bool verbose = false;
  bool self_test = false;
  bool memory_enabled = false;
  bool memory_auto_capture = false;
  int max_iterations = 8;
  int max_tokens = 4096;
  int max_context_tokens = 24000;
  double temperature = 0.2;
  std::filesystem::path memory_path;
  std::string memory_user_id;
  std::string memory_agent_id = "pi-lite";
  std::string memory_run_id;
};

std::unordered_map<std::string, std::string> load_dotenv(const std::filesystem::path& path);

std::optional<std::string> lookup_config_value(
    const std::unordered_map<std::string, std::string>& dotenv,
    const std::string& key);

}  // namespace pilite
