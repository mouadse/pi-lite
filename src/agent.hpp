#pragma once

#include <string>
#include <memory>
#include <vector>

#include "llm_client.hpp"
#include "memory.hpp"
#include "messages.hpp"
#include "tool.hpp"

namespace pilite {

class Agent {
 public:
  Agent(AppConfig config, LlmClient client, ToolRegistry tools, std::shared_ptr<MemoryManager> memory = nullptr);

  void run(const std::string& prompt);
  void clear();
  void set_model(const std::string& model);
  const std::vector<Message>& messages() const { return messages_; }

 private:
  std::string system_prompt(const std::string& current_prompt) const;

  AppConfig config_;
  LlmClient client_;
  ToolRegistry tools_;
  std::shared_ptr<MemoryManager> memory_;
  std::vector<Message> messages_;
};

}  // namespace pilite
