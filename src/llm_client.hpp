#pragma once

#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "config.hpp"
#include "messages.hpp"

namespace pilite {

using TextDeltaCallback = std::function<void(const std::string&)>;

class LlmClient {
 public:
  explicit LlmClient(AppConfig config);

  Message complete(const std::string& system_prompt,
                   const std::vector<Message>& messages,
                   const nlohmann::json& tools,
                   const TextDeltaCallback& on_text_delta = {}) const;

 private:
  AppConfig config_;
};

}  // namespace pilite
