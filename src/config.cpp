#include "config.hpp"

#include <cstdlib>
#include <fstream>
#include <string>

#include "util.hpp"

namespace pilite {

std::unordered_map<std::string, std::string> load_dotenv(const std::filesystem::path& path) {
  std::unordered_map<std::string, std::string> values;
  std::ifstream in(path);
  if (!in) return values;

  std::string line;
  while (std::getline(in, line)) {
    line = trim(line);
    if (line.empty() || line[0] == '#') continue;
    if (line.rfind("export ", 0) == 0) line = trim(line.substr(7));

    const auto equals = line.find('=');
    if (equals == std::string::npos) continue;

    auto key = trim(line.substr(0, equals));
    auto value = trim(line.substr(equals + 1));
    if (key.empty()) continue;

    if (value.size() >= 2 &&
        ((value.front() == '"' && value.back() == '"') ||
         (value.front() == '\'' && value.back() == '\''))) {
      value = value.substr(1, value.size() - 2);
    }

    values[key] = value;
  }

  return values;
}

std::optional<std::string> lookup_config_value(
    const std::unordered_map<std::string, std::string>& dotenv,
    const std::string& key) {
  if (const char* value = std::getenv(key.c_str()); value && *value) return std::string(value);
  if (const auto it = dotenv.find(key); it != dotenv.end() && !it->second.empty()) return it->second;
  return std::nullopt;
}

}  // namespace pilite
