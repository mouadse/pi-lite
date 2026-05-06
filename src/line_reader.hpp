#pragma once

#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

namespace pilite {

struct SlashCommandCompletion {
  std::string value;
  std::string label;
  std::string description;
};

struct SlashCommandSpec {
  std::string name;
  std::string description;
  std::string argument_hint = "";
  std::vector<SlashCommandCompletion> argument_completions = {};
};

struct LineHistory {
  std::vector<std::string> entries;
  std::optional<std::size_t> pos;
  std::string stash;
  static constexpr std::size_t max_size = 100;
};

bool read_interactive_line(const std::string& prompt, std::string& line, LineHistory& history);
bool read_interactive_line(const std::string& prompt,
                           std::string& line,
                           LineHistory& history,
                           const std::vector<SlashCommandSpec>& slash_commands);

}  // namespace pilite
