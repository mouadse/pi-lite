#include "util.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <sstream>
#include <stdexcept>
#include <sys/wait.h>
#include <vector>

namespace pilite {

std::string trim(std::string value) {
  const auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
  value.erase(value.begin(), std::find_if(value.begin(), value.end(), [&](char c) {
                return !is_space(static_cast<unsigned char>(c));
              }));
  value.erase(std::find_if(value.rbegin(), value.rend(), [&](char c) {
                return !is_space(static_cast<unsigned char>(c));
              }).base(),
              value.end());
  return value;
}

std::string to_lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

std::string shell_quote(const std::string& value) {
  std::string quoted = "'";
  for (const char c : value) {
    if (c == '\'') {
      quoted += "'\\''";
    } else {
      quoted += c;
    }
  }
  quoted += "'";
  return quoted;
}

std::string truncate_line(const std::string& value, std::size_t max_bytes) {
  if (value.size() <= max_bytes) return value;
  if (max_bytes <= 16) return value.substr(0, max_bytes);
  return value.substr(0, max_bytes - 16) + "... [truncated]";
}

static std::vector<std::string> split_lines(const std::string& value) {
  std::vector<std::string> lines;
  std::istringstream stream(value);
  std::string line;
  while (std::getline(stream, line)) lines.push_back(line);
  if (!value.empty() && value.back() == '\n') lines.emplace_back();
  return lines;
}

std::string truncate_head(const std::string& value, std::size_t max_bytes, std::size_t max_lines) {
  const auto lines = split_lines(value);
  std::ostringstream out;
  std::size_t bytes = 0;
  std::size_t emitted = 0;

  for (const auto& line : lines) {
    if (emitted >= max_lines) break;
    const auto row = truncate_line(line) + "\n";
    if (bytes + row.size() > max_bytes) break;
    out << row;
    bytes += row.size();
    ++emitted;
  }

  if (emitted < lines.size() || value.size() > bytes) {
    out << "[Output truncated: showing first " << emitted << " lines / " << bytes << " bytes.]\n";
  }
  return out.str();
}

std::string truncate_tail(const std::string& value, std::size_t max_bytes, std::size_t max_lines) {
  const auto lines = split_lines(value);
  std::vector<std::string> kept;
  std::size_t bytes = 0;

  for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
    if (kept.size() >= max_lines) break;
    const auto row = truncate_line(*it) + "\n";
    if (bytes + row.size() > max_bytes) break;
    kept.push_back(row);
    bytes += row.size();
  }

  std::ostringstream out;
  if (kept.size() < lines.size() || value.size() > bytes) {
    out << "[Output truncated: showing last " << kept.size() << " lines / " << bytes << " bytes.]\n";
  }
  for (auto it = kept.rbegin(); it != kept.rend(); ++it) out << *it;
  return out.str();
}

std::filesystem::path canonical_workspace(const std::filesystem::path& workspace) {
  std::error_code ec;
  auto root = std::filesystem::weakly_canonical(workspace, ec);
  if (ec) root = std::filesystem::absolute(workspace).lexically_normal();
  return root;
}

static bool is_inside_workspace(const std::filesystem::path& workspace,
                                const std::filesystem::path& path) {
  std::error_code ec;
  const auto relative = std::filesystem::relative(path, workspace, ec);
  if (ec || relative.empty()) return path == workspace;
  if (relative.is_absolute()) return false;
  for (const auto& part : relative) {
    if (part == "..") return false;
  }
  return true;
}

std::filesystem::path resolve_workspace_path(const std::filesystem::path& workspace,
                                             const std::string& requested_path) {
  if (trim(requested_path).empty()) throw std::runtime_error("path is required");

  const auto root = canonical_workspace(workspace);
  std::filesystem::path requested(requested_path);
  auto candidate = requested.is_absolute() ? requested : root / requested;
  candidate = candidate.lexically_normal();

  std::error_code ec;
  auto resolved = std::filesystem::weakly_canonical(candidate, ec);
  if (ec) resolved = std::filesystem::absolute(candidate).lexically_normal();

  if (!is_inside_workspace(root, resolved)) {
    throw std::runtime_error("path escapes workspace: " + requested_path);
  }

  return candidate;
}

std::string relative_to_workspace(const std::filesystem::path& workspace,
                                  const std::filesystem::path& path) {
  std::error_code ec;
  auto relative = std::filesystem::relative(path, canonical_workspace(workspace), ec);
  if (ec) return path.string();
  return relative.empty() ? "." : relative.generic_string();
}

CommandResult run_shell_capture(const std::string& command) {
  std::array<char, 4096> buffer{};
  CommandResult result;

  FILE* pipe = popen(command.c_str(), "r");
  if (!pipe) throw std::runtime_error("failed to start command");

  while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
    result.output += buffer.data();
  }

  const int status = pclose(pipe);
  if (WIFEXITED(status)) {
    result.exit_code = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    result.exit_code = 128 + WTERMSIG(status);
  } else {
    result.exit_code = status;
  }

  return result;
}

}  // namespace pilite
