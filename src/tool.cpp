#include "tool.hpp"

#include <stdexcept>
#include <utility>

namespace pilite {

void ToolRegistry::add(Tool tool) {
  tools_.push_back(std::move(tool));
}

nlohmann::json ToolRegistry::schemas() const {
  auto schemas = nlohmann::json::array();
  for (const auto& tool : tools_) {
    schemas.push_back({
        {"type", "function"},
        {"function",
         {
             {"name", tool.name},
             {"description", tool.description},
             {"parameters", tool.parameters},
         }},
    });
  }
  return schemas;
}

ToolResult ToolRegistry::execute(const std::string& name, const nlohmann::json& arguments) const {
  for (const auto& tool : tools_) {
    if (tool.name == name) {
      try {
        return tool.execute(arguments);
      } catch (const std::exception& error) {
        return ToolResult{.content = std::string("Tool failed: ") + error.what(), .is_error = true};
      }
    }
  }
  return ToolResult{.content = "Unknown tool: " + name, .is_error = true};
}

}  // namespace pilite
