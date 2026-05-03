#pragma once

#include <iosfwd>
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

bool read_interactive_line(const std::string& prompt, std::string& line);
bool read_interactive_line(const std::string& prompt,
                           std::string& line,
                           const std::vector<SlashCommandSpec>& slash_commands);

}  // namespace pilite
