#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

namespace pilite {

constexpr std::size_t kDefaultMaxBytes = 50 * 1024;
constexpr std::size_t kDefaultMaxLines = 2000;
constexpr std::size_t kMaxLineBytes = 2000;

struct CommandResult {
  int exit_code = -1;
  std::string output;
};

std::string trim(std::string value);
std::string to_lower(std::string value);
std::string shell_quote(const std::string& value);
std::string truncate_line(const std::string& value, std::size_t max_bytes = kMaxLineBytes);
std::string truncate_head(const std::string& value,
                          std::size_t max_bytes = kDefaultMaxBytes,
                          std::size_t max_lines = kDefaultMaxLines);
std::string truncate_tail(const std::string& value,
                          std::size_t max_bytes = kDefaultMaxBytes,
                          std::size_t max_lines = kDefaultMaxLines);

std::filesystem::path canonical_workspace(const std::filesystem::path& workspace);
std::filesystem::path resolve_workspace_path(const std::filesystem::path& workspace,
                                             const std::string& requested_path);
std::string relative_to_workspace(const std::filesystem::path& workspace,
                                  const std::filesystem::path& path);

CommandResult run_shell_capture(const std::string& command);

}  // namespace pilite
