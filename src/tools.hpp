#pragma once

#include <filesystem>

#include "tool.hpp"

namespace pilite {

void register_default_tools(ToolRegistry& registry,
                            const std::filesystem::path& workspace,
                            bool allow_bash);

}  // namespace pilite
