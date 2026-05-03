#include "tools.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <fstream>
#include <iostream>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
#include <unordered_set>
#include <vector>

#include "util.hpp"

namespace pilite {
namespace {

std::string required_string(const nlohmann::json& args, const char* key) {
  if (!args.contains(key) || !args.at(key).is_string()) {
    throw std::runtime_error(std::string(key) + " must be a string");
  }
  return args.at(key).get<std::string>();
}

std::string optional_string(const nlohmann::json& args, const char* key, std::string fallback) {
  if (!args.contains(key) || args.at(key).is_null()) return fallback;
  if (!args.at(key).is_string()) throw std::runtime_error(std::string(key) + " must be a string");
  return args.at(key).get<std::string>();
}

int optional_int(const nlohmann::json& args, const char* key, int fallback) {
  if (!args.contains(key) || args.at(key).is_null()) return fallback;
  if (!args.at(key).is_number_integer()) throw std::runtime_error(std::string(key) + " must be an integer");
  return args.at(key).get<int>();
}

bool optional_bool(const nlohmann::json& args, const char* key, bool fallback) {
  if (!args.contains(key) || args.at(key).is_null()) return fallback;
  if (!args.at(key).is_boolean()) throw std::runtime_error(std::string(key) + " must be a boolean");
  return args.at(key).get<bool>();
}

nlohmann::json object_schema(nlohmann::json properties, std::vector<std::string> required = {}) {
  return {
      {"type", "object"},
      {"properties", std::move(properties)},
      {"required", std::move(required)},
      {"additionalProperties", false},
  };
}

bool is_sensitive_path(const std::filesystem::path& path);
std::optional<std::vector<std::filesystem::path>> git_file_manifest(const std::filesystem::path& workspace,
                                                                    const std::filesystem::path& path);

ToolResult read_file_tool(const std::filesystem::path& workspace, const nlohmann::json& args) {
  const auto path_arg = required_string(args, "path");
  const auto offset = std::max(1, optional_int(args, "offset", 1));
  const auto limit = std::clamp(optional_int(args, "limit", static_cast<int>(kDefaultMaxLines)), 1, 5000);
  const auto path = resolve_workspace_path(workspace, path_arg);

  if (!std::filesystem::exists(path)) {
    return {.content = "File does not exist: " + path_arg, .is_error = true};
  }
  if (!std::filesystem::is_regular_file(path)) {
    return {.content = "Path is not a regular file: " + path_arg, .is_error = true};
  }
  if (is_sensitive_path(path)) {
    return {.content = "Refusing to read sensitive file: " + relative_to_workspace(workspace, path),
            .is_error = true};
  }

  std::ifstream in(path, std::ios::binary);
  if (!in) return {.content = "Could not open file: " + path_arg, .is_error = true};

  std::ostringstream out;
  out << "File: " << relative_to_workspace(workspace, path) << "\n";

  std::string line;
  int line_no = 0;
  int shown = 0;
  std::size_t bytes = 0;
  bool truncated = false;

  while (std::getline(in, line)) {
    ++line_no;
    if (line.find('\0') != std::string::npos) {
      return {.content = "File appears to be binary; refusing to read as text.", .is_error = true};
    }
    if (line_no < offset) continue;
    if (shown >= limit || bytes >= kDefaultMaxBytes) {
      truncated = true;
      break;
    }

    const auto row = std::to_string(line_no) + ": " + truncate_line(line) + "\n";
    if (bytes + row.size() > kDefaultMaxBytes) {
      truncated = true;
      break;
    }
    out << row;
    bytes += row.size();
    ++shown;
  }

  if (shown == 0) out << "[No lines shown.]\n";
  if (truncated) out << "[Output truncated. Use offset=" << (line_no + 1) << " to continue.]\n";
  return {.content = out.str()};
}

ToolResult write_file_tool(const std::filesystem::path& workspace, const nlohmann::json& args) {
  const auto path_arg = required_string(args, "path");
  const auto content = required_string(args, "content");
  const auto path = resolve_workspace_path(workspace, path_arg);

  if (is_sensitive_path(path)) {
    return {.content = "Refusing to write sensitive file: " + relative_to_workspace(workspace, path),
            .is_error = true};
  }
  if (std::filesystem::exists(path) && std::filesystem::is_directory(path)) {
    return {.content = "Path is a directory: " + path_arg, .is_error = true};
  }

  std::error_code ec;
  const auto parent = path.parent_path();
  if (!parent.empty()) std::filesystem::create_directories(parent, ec);
  if (ec) return {.content = "Could not create parent directories: " + ec.message(), .is_error = true};

  std::ofstream out(path, std::ios::binary);
  if (!out) return {.content = "Could not open file for writing: " + path_arg, .is_error = true};
  out << content;
  if (!out) return {.content = "Failed while writing file: " + path_arg, .is_error = true};

  return {.content = "Wrote " + std::to_string(content.size()) + " bytes to " + relative_to_workspace(workspace, path) + "\n"};
}

ToolResult list_files_tool(const std::filesystem::path& workspace, const nlohmann::json& args) {
  const auto path_arg = optional_string(args, "path", ".");
  const auto limit = std::clamp(optional_int(args, "limit", 200), 1, 1000);
  const auto path = resolve_workspace_path(workspace, path_arg);

  if (!std::filesystem::exists(path)) return {.content = "Path does not exist: " + path_arg, .is_error = true};
  if (!std::filesystem::is_directory(path)) return {.content = "Path is not a directory: " + path_arg, .is_error = true};

  std::vector<std::string> entries;
  if (const auto manifest = git_file_manifest(workspace, path)) {
    std::unordered_set<std::string> seen;
    for (const auto& file : *manifest) {
      std::error_code ec;
      const auto relative = std::filesystem::relative(file, path, ec);
      if (ec || relative.empty()) continue;

      auto part = relative.begin();
      if (part == relative.end()) continue;
      auto name = part->generic_string();
      ++part;
      if (part != relative.end()) name += "/";
      if (is_sensitive_path(file) && name.back() != '/') name += " [sensitive]";
      if (seen.insert(name).second) entries.push_back(name);
    }
  } else {
    for (const auto& entry : std::filesystem::directory_iterator(path)) {
      auto name = entry.path().filename().string();
      if (entry.is_directory()) name += "/";
      if (is_sensitive_path(entry.path())) name += " [sensitive]";
      entries.push_back(name);
    }
  }
  std::sort(entries.begin(), entries.end());

  std::ostringstream out;
  out << "Directory: " << relative_to_workspace(workspace, path) << "\n";
  const auto count = std::min<std::size_t>(entries.size(), static_cast<std::size_t>(limit));
  for (std::size_t i = 0; i < count; ++i) out << entries[i] << "\n";
  if (entries.size() > count) out << "[Output truncated: " << entries.size() - count << " entries omitted.]\n";
  return {.content = out.str()};
}

bool should_skip_directory(const std::filesystem::path& path) {
  static const std::unordered_set<std::string> skipped = {
      ".git", "build", "cmake-build-debug", "cmake-build-release", "node_modules", ".venv", "venv"};
  return skipped.contains(path.filename().string());
}

std::optional<std::vector<std::filesystem::path>> git_file_manifest(const std::filesystem::path& workspace,
                                                                    const std::filesystem::path& path) {
  const auto workspace_root = canonical_workspace(workspace);
  const auto repo_result = run_shell_capture("cd " + shell_quote(workspace_root.string()) +
                                             " && git rev-parse --show-toplevel 2>/dev/null");
  if (repo_result.exit_code != 0) return std::nullopt;

  std::istringstream repo_lines(repo_result.output);
  std::string repo_root_text;
  std::getline(repo_lines, repo_root_text);
  repo_root_text = trim(repo_root_text);
  if (repo_root_text.empty()) return std::nullopt;

  const auto repo_root = canonical_workspace(repo_root_text);
  std::error_code ec;
  auto pathspec = std::filesystem::relative(path, repo_root, ec);
  if (ec || pathspec.empty()) pathspec = ".";
  if (pathspec.is_absolute()) return std::nullopt;
  for (const auto& part : pathspec) {
    if (part == "..") return std::nullopt;
  }

  const auto command = "cd " + shell_quote(repo_root.string()) +
                       " && git ls-files -co --exclude-standard -- " + shell_quote(pathspec.generic_string()) +
                       " 2>/dev/null";
  const auto result = run_shell_capture(command);
  if (result.exit_code != 0) return std::nullopt;

  std::vector<std::filesystem::path> files;
  std::istringstream lines(result.output);
  std::string line;
  while (std::getline(lines, line)) {
    line = trim(line);
    if (line.empty()) continue;
    auto resolved = (repo_root / line).lexically_normal();
    std::error_code canonical_ec;
    auto canonical = std::filesystem::weakly_canonical(resolved, canonical_ec);
    if (canonical_ec) canonical = std::filesystem::absolute(resolved).lexically_normal();

    const auto relative_to_workspace_path = std::filesystem::relative(canonical, workspace_root, canonical_ec);
    if (canonical_ec || relative_to_workspace_path.is_absolute()) continue;
    if (std::any_of(relative_to_workspace_path.begin(), relative_to_workspace_path.end(), [](const auto& part) {
          return part == "..";
        })) {
      continue;
    }
    if (std::filesystem::exists(canonical) && std::filesystem::is_regular_file(canonical)) files.push_back(canonical);
  }

  std::sort(files.begin(), files.end());
  files.erase(std::unique(files.begin(), files.end()), files.end());
  return files;
}

bool is_sensitive_path(const std::filesystem::path& path) {
  const auto filename = to_lower(path.filename().string());
  static const std::unordered_set<std::string> sensitive_names = {
      ".env",          ".env.local",    ".env.production", ".env.development",
      "credentials.json", "secrets.json", "id_rsa",          "id_ed25519",
      "known_hosts"};
  if (sensitive_names.contains(filename)) return true;

  const auto full = to_lower(path.generic_string());
  return full.find("/.ssh/") != std::string::npos || full.find("secret") != std::string::npos ||
         full.find("credential") != std::string::npos || full.find("private_key") != std::string::npos;
}

bool looks_binary(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return true;
  std::array<char, 512> buffer{};
  in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
  const auto count = in.gcount();
  return std::find(buffer.begin(), buffer.begin() + count, '\0') != buffer.begin() + count;
}

ToolResult grep_files_tool(const std::filesystem::path& workspace, const nlohmann::json& args) {
  const auto pattern = required_string(args, "pattern");
  const auto path_arg = optional_string(args, "path", ".");
  const auto max_results = std::clamp(optional_int(args, "max_results", 100), 1, 1000);
  const auto case_sensitive = optional_bool(args, "case_sensitive", true);
  const auto path = resolve_workspace_path(workspace, path_arg);

  if (!std::filesystem::exists(path)) return {.content = "Path does not exist: " + path_arg, .is_error = true};

  std::regex regex;
  try {
    regex = std::regex(pattern, case_sensitive ? std::regex::ECMAScript : std::regex::icase);
  } catch (const std::regex_error& error) {
    return {.content = std::string("Invalid regex: ") + error.what(), .is_error = true};
  }

  std::vector<std::filesystem::path> files;
  if (std::filesystem::is_regular_file(path)) {
    files.push_back(path);
  } else if (std::filesystem::is_directory(path)) {
    if (const auto manifest = git_file_manifest(workspace, path)) {
      files = *manifest;
    } else {
      std::filesystem::recursive_directory_iterator it(path), end;
      while (it != end) {
        if (it->is_directory() && should_skip_directory(it->path())) {
          it.disable_recursion_pending();
        } else if (it->is_regular_file()) {
          files.push_back(it->path());
        }
        std::error_code ec;
        it.increment(ec);
        if (ec) break;
      }
    }
  }

  std::ostringstream out;
  int matches = 0;
  std::size_t bytes = 0;

  for (const auto& file : files) {
    std::error_code ec;
    if (std::filesystem::file_size(file, ec) > 2 * 1024 * 1024) continue;
    if (is_sensitive_path(file)) continue;
    if (looks_binary(file)) continue;

    std::ifstream in(file);
    std::string line;
    int line_no = 0;
    while (std::getline(in, line)) {
      ++line_no;
      if (!std::regex_search(line, regex)) continue;
      const auto row = relative_to_workspace(workspace, file) + ":" + std::to_string(line_no) + ": " +
                       truncate_line(line) + "\n";
      if (bytes + row.size() > kDefaultMaxBytes || matches >= max_results) {
        out << "[Output truncated. Refine the search or increase max_results.]\n";
        return {.content = out.str()};
      }
      out << row;
      bytes += row.size();
      ++matches;
    }
  }

  if (matches == 0) out << "No matches.\n";
  return {.content = out.str()};
}

std::string strip_patch_path_prefix(std::string path) {
  path = trim(path);
  const auto tab = path.find('\t');
  if (tab != std::string::npos) path = path.substr(0, tab);
  const auto space = path.find(' ');
  if (space != std::string::npos) path = path.substr(0, space);
  if (path == "/dev/null") return path;
  if (path.rfind("a/", 0) == 0 || path.rfind("b/", 0) == 0) return path.substr(2);
  return path;
}

std::vector<std::string> extract_patch_paths(const std::string& patch) {
  std::vector<std::string> paths;
  std::istringstream in(patch);
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind("--- ", 0) == 0 || line.rfind("+++ ", 0) == 0) {
      auto path = strip_patch_path_prefix(line.substr(4));
      if (!path.empty() && path != "/dev/null") paths.push_back(path);
    } else if (line.rfind("diff --git ", 0) == 0) {
      std::istringstream parts(line);
      std::string diff, git, left, right;
      parts >> diff >> git >> left >> right;
      left = strip_patch_path_prefix(left);
      right = strip_patch_path_prefix(right);
      if (!left.empty() && left != "/dev/null") paths.push_back(left);
      if (!right.empty() && right != "/dev/null") paths.push_back(right);
    }
  }
  std::sort(paths.begin(), paths.end());
  paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
  return paths;
}

void validate_patch_paths(const std::filesystem::path& workspace, const std::vector<std::string>& paths) {
  if (paths.empty()) throw std::runtime_error("patch does not contain any file headers");
  for (const auto& path : paths) {
    if (path.empty() || path[0] == '/') throw std::runtime_error("unsafe patch path: " + path);
    std::filesystem::path parsed(path);
    for (const auto& part : parsed) {
      if (part == "..") throw std::runtime_error("unsafe patch path: " + path);
    }
    const auto resolved = resolve_workspace_path(workspace, path);
    if (is_sensitive_path(resolved)) throw std::runtime_error("refusing to patch sensitive file: " + path);
  }
}

ToolResult apply_patch_tool(const std::filesystem::path& workspace, const nlohmann::json& args) {
  const auto patch = required_string(args, "patch");
  if (patch.find("*** Begin Patch") != std::string::npos) {
    return {.content = "apply_patch expects a standard unified diff, not the OpenCode patch envelope.", .is_error = true};
  }

  try {
    validate_patch_paths(workspace, extract_patch_paths(patch));
  } catch (const std::exception& error) {
    return {.content = error.what(), .is_error = true};
  }

  const auto temp = std::filesystem::temp_directory_path() /
                    ("pi-lite-patch-" + std::to_string(static_cast<long long>(getpid())) + ".diff");
  {
    std::ofstream out(temp, std::ios::binary);
    out << patch;
  }

  const auto root = canonical_workspace(workspace).string();
  const auto dry_p1 = run_shell_capture("cd " + shell_quote(root) +
                                        " && patch --dry-run --batch --forward -p1 < " +
                                        shell_quote(temp.string()) + " 2>&1");

  int strip = 1;
  CommandResult dry = dry_p1;
  if (dry_p1.exit_code != 0) {
    dry = run_shell_capture("cd " + shell_quote(root) +
                            " && patch --dry-run --batch --forward -p0 < " +
                            shell_quote(temp.string()) + " 2>&1");
    strip = 0;
  }

  if (dry.exit_code != 0) {
    std::filesystem::remove(temp);
    return {.content = "Patch dry run failed:\n" + truncate_tail(dry.output), .is_error = true};
  }

  const auto applied = run_shell_capture("cd " + shell_quote(root) + " && patch --batch --forward -p" +
                                         std::to_string(strip) + " < " + shell_quote(temp.string()) + " 2>&1");
  std::filesystem::remove(temp);

  if (applied.exit_code != 0) {
    return {.content = "Patch failed:\n" + truncate_tail(applied.output), .is_error = true};
  }

  auto output = truncate_tail(applied.output);
  if (trim(output).empty()) output = "Patch applied.\n";
  return {.content = output};
}

bool command_is_dangerous(const std::string& command) {
  const auto lower = to_lower(command);
  static const std::vector<std::string> blocked = {
      "rm -rf",       "sudo ",       "mkfs",      "dd if=",       "git reset --hard",
      "git clean -fd", "shutdown",    "reboot",    "poweroff",     ":(){",
      "chmod -r 777 /", "chown -r ",   "> /dev/sd", "wipefs",       "fdisk"};
  return std::any_of(blocked.begin(), blocked.end(), [&](const std::string& needle) {
    return lower.find(needle) != std::string::npos;
  });
}

ToolResult bash_tool(const std::filesystem::path& workspace, bool allow_bash, const nlohmann::json& args) {
  if (!allow_bash) {
    return {.content = "bash is disabled. Restart pi-lite with --allow-bash to enable shell execution.",
            .is_error = true};
  }

  const auto command = required_string(args, "command");
  if (command_is_dangerous(command)) {
    return {.content = "Blocked dangerous shell command.", .is_error = true};
  }

  const auto root = canonical_workspace(workspace).string();
  const auto wrapped = "cd " + shell_quote(root) + " && PI_CODING_AGENT=true timeout 30s bash -lc " +
                       shell_quote(command) + " 2>&1";

  CommandResult result;
  std::atomic<bool> running{true};
  std::atomic<bool> printed{false};
  std::thread progress;
  if (isatty(STDERR_FILENO)) {
    progress = std::thread([&]() {
      for (int tick = 0; running.load(); ++tick) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (!running.load()) break;
        if (tick > 0 && tick % 10 == 0) {
          std::cerr << "." << std::flush;
          printed = true;
        }
      }
    });
  }

  try {
    result = run_shell_capture(wrapped);
  } catch (...) {
    running = false;
    if (progress.joinable()) progress.join();
    if (printed.load()) std::cerr << "\n";
    throw;
  }
  running = false;
  if (progress.joinable()) progress.join();
  if (printed.load()) std::cerr << "\n";

  std::ostringstream out;
  out << "Exit code: " << result.exit_code << "\n";
  const auto truncated = truncate_tail(result.output);
  out << (trim(truncated).empty() ? "(no output)\n" : truncated);
  return {.content = out.str(), .is_error = result.exit_code != 0};
}

}  // namespace

void register_default_tools(ToolRegistry& registry,
                            const std::filesystem::path& workspace,
                            bool allow_bash) {
  registry.add(Tool{
      .name = "read_file",
      .description = "Read a non-secret text file from the workspace. Output is line-numbered and capped.",
      .parameters = object_schema({
          {"path", {{"type", "string"}, {"description", "Workspace-relative file path."}}},
          {"offset", {{"type", "integer"}, {"description", "1-based starting line."}, {"default", 1}}},
          {"limit", {{"type", "integer"}, {"description", "Maximum lines to return."}, {"default", 2000}}},
      }, {"path"}),
      .execute = [workspace](const nlohmann::json& args) { return read_file_tool(workspace, args); },
  });

  registry.add(Tool{
      .name = "write_file",
      .description = "Create or overwrite a non-secret text file in the workspace. Prefer apply_patch for small edits.",
      .parameters = object_schema({
          {"path", {{"type", "string"}, {"description", "Workspace-relative file path."}}},
          {"content", {{"type", "string"}, {"description", "Complete file content to write."}}},
      }, {"path", "content"}),
      .execute = [workspace](const nlohmann::json& args) { return write_file_tool(workspace, args); },
  });

  registry.add(Tool{
      .name = "list_files",
      .description = "List files in a workspace directory.",
      .parameters = object_schema({
          {"path", {{"type", "string"}, {"description", "Workspace-relative directory."}, {"default", "."}}},
          {"limit", {{"type", "integer"}, {"description", "Maximum entries to return."}, {"default", 200}}},
      }),
      .execute = [workspace](const nlohmann::json& args) { return list_files_tool(workspace, args); },
  });

  registry.add(Tool{
      .name = "grep_files",
      .description = "Search non-secret text files with an ECMAScript regular expression.",
      .parameters = object_schema({
          {"pattern", {{"type", "string"}, {"description", "Regular expression to search for."}}},
          {"path", {{"type", "string"}, {"description", "File or directory to search."}, {"default", "."}}},
          {"max_results", {{"type", "integer"}, {"description", "Maximum matches."}, {"default", 100}}},
          {"case_sensitive", {{"type", "boolean"}, {"description", "Use case-sensitive matching."}, {"default", true}}},
      }, {"pattern"}),
      .execute = [workspace](const nlohmann::json& args) { return grep_files_tool(workspace, args); },
  });

  registry.add(Tool{
      .name = "apply_patch",
      .description = "Apply a standard unified diff to non-secret workspace files. Use git-style a/ and b/ paths when possible.",
      .parameters = object_schema({
          {"patch", {{"type", "string"}, {"description", "Unified diff patch text."}}},
      }, {"patch"}),
      .execute = [workspace](const nlohmann::json& args) { return apply_patch_tool(workspace, args); },
  });

  registry.add(Tool{
      .name = "bash",
      .description = "Run a shell command in the workspace. Disabled unless pi-lite starts with --allow-bash.",
      .parameters = object_schema({
          {"command", {{"type", "string"}, {"description", "Shell command to run."}}},
      }, {"command"}),
      .execute = [workspace, allow_bash](const nlohmann::json& args) { return bash_tool(workspace, allow_bash, args); },
  });
}

}  // namespace pilite
