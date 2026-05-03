#pragma once

#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace pilite {

struct ToolResult {
  std::string content;
  bool is_error = false;
  bool terminate = false;
};

struct Tool {
  std::string name;
  std::string description;
  nlohmann::json parameters;
  std::function<ToolResult(const nlohmann::json&)> execute;
};

class ToolRegistry {
 public:
  void add(Tool tool);
  nlohmann::json schemas() const;
  ToolResult execute(const std::string& name, const nlohmann::json& arguments) const;
  const std::vector<Tool>& tools() const { return tools_; }

 private:
  std::vector<Tool> tools_;
};

}  // namespace pilite
