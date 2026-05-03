#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace pilite {

struct ToolCall {
  std::string id;
  std::string name;
  nlohmann::json arguments = nlohmann::json::object();
  std::string raw_arguments;
};

struct Usage {
  int prompt_tokens = 0;
  int completion_tokens = 0;
  int total_tokens = 0;
  bool available = false;
};

struct Message {
  std::string role;
  std::string content;
  std::vector<ToolCall> tool_calls;
  std::string tool_call_id;
  std::string name;
  bool is_error = false;
  Usage usage;
};

inline Message user_message(std::string content) {
  Message message;
  message.role = "user";
  message.content = std::move(content);
  return message;
}

inline Message tool_message(const ToolCall& call, std::string content, bool is_error) {
  Message message;
  message.role = "tool";
  message.content = std::move(content);
  message.tool_call_id = call.id;
  message.name = call.name;
  message.is_error = is_error;
  return message;
}

}  // namespace pilite
