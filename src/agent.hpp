#pragma once

#include <string>
#include <vector>

#include "llm_client.hpp"
#include "messages.hpp"
#include "tool.hpp"

namespace pilite {

class Agent {
 public:
  Agent(AppConfig config, LlmClient client, ToolRegistry tools);

  void run(const std::string& prompt);
  void clear();
  const std::vector<Message>& messages() const { return messages_; }

 private:
  std::string system_prompt() const;

  AppConfig config_;
  LlmClient client_;
  ToolRegistry tools_;
  std::vector<Message> messages_;
};

}  // namespace pilite
