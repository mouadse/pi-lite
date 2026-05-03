#include "agent.hpp"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "markdown_renderer.hpp"
#include "status_indicator.hpp"
#include "util.hpp"

namespace pilite {
namespace {

std::string paint_stderr(const std::string& ansi, const std::string& text) {
  if (!terminal_supports_ansi(STDERR_FILENO)) return text;
  return ansi + text + "\033[0m";
}

std::string tool_badge(const std::string& label, bool is_error = false, bool is_done = false) {
  if (!terminal_supports_ansi(STDERR_FILENO)) return "[" + label + "]";
  const auto bg = is_error ? ansi_bg(60, 40, 40) : (is_done ? ansi_bg(40, 50, 40) : ansi_bg(40, 40, 50));
  return bg + ansi_fg(230, 230, 230) + "[" + label + "]\033[0m";
}

std::string elapsed_ms(std::chrono::steady_clock::time_point started) {
  const auto now = std::chrono::steady_clock::now();
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - started).count();
  return std::to_string(ms) + "ms";
}

std::size_t estimate_text_tokens(const std::string& text) {
  return (text.size() + 3) / 4 + 1;
}

std::size_t estimate_message_tokens(const Message& message) {
  std::size_t total = estimate_text_tokens(message.role) + estimate_text_tokens(message.content) + 4;
  total += estimate_text_tokens(message.name) + estimate_text_tokens(message.tool_call_id);
  for (const auto& call : message.tool_calls) {
    total += estimate_text_tokens(call.name) + estimate_text_tokens(call.raw_arguments.empty() ? call.arguments.dump() : call.raw_arguments) + 8;
  }
  return total;
}

std::size_t estimate_request_tokens(const std::string& system_prompt,
                                    const std::vector<Message>& messages,
                                    const nlohmann::json& tool_schemas) {
  std::size_t total = estimate_text_tokens(system_prompt) + estimate_text_tokens(tool_schemas.dump()) + 16;
  for (const auto& message : messages) total += estimate_message_tokens(message);
  return total;
}

std::size_t estimate_completion_tokens(const Message& assistant) {
  std::size_t total = estimate_text_tokens(assistant.content);
  for (const auto& call : assistant.tool_calls) {
    total += estimate_text_tokens(call.name) + estimate_text_tokens(call.raw_arguments.empty() ? call.arguments.dump() : call.raw_arguments);
  }
  return total;
}

std::string one_line(std::string value, std::size_t max_bytes = 700) {
  std::replace(value.begin(), value.end(), '\n', ' ');
  std::replace(value.begin(), value.end(), '\r', ' ');
  return truncate_line(trim(std::move(value)), max_bytes);
}

std::string compact_message_line(const Message& message) {
  std::ostringstream out;
  out << "- " << message.role;
  if (!message.name.empty()) out << " " << message.name;
  if (message.is_error) out << " error";

  if (message.role == "tool") {
    out << " (" << message.content.size() << " bytes): " << one_line(message.content, 500);
    return out.str();
  }

  if (!message.tool_calls.empty()) {
    out << " requested tools:";
    for (const auto& call : message.tool_calls) out << " " << call.name;
  }

  const auto content = one_line(message.content, 800);
  if (!content.empty()) out << ": " << content;
  return out.str();
}

bool maybe_compact_messages(std::vector<Message>& messages,
                            const std::string& system_prompt,
                            const nlohmann::json& tool_schemas,
                            int max_context_tokens) {
  if (max_context_tokens <= 0) return false;
  if (estimate_request_tokens(system_prompt, messages, tool_schemas) <= static_cast<std::size_t>(max_context_tokens)) {
    return false;
  }

  bool changed = false;
  for (auto& message : messages) {
    if (message.role == "tool" && message.content.size() > 8000) {
      const auto original_size = message.content.size();
      message.content = truncate_head(message.content, 8000, 220);
      message.content += "[Tool output compacted from " + std::to_string(original_size) +
                         " bytes. Ask for a narrower read/search or rerun the tool if needed.]\n";
      changed = true;
    }
  }

  if (estimate_request_tokens(system_prompt, messages, tool_schemas) <= static_cast<std::size_t>(max_context_tokens)) {
    return changed;
  }

  std::size_t keep_start = messages.size();
  int users_seen = 0;
  for (std::size_t i = messages.size(); i > 0; --i) {
    const auto index = i - 1;
    if (messages[index].role == "user" && ++users_seen >= 2) {
      keep_start = index;
      break;
    }
  }

  if (keep_start == 0 || keep_start >= messages.size()) return changed;

  std::ostringstream summary;
  summary << "Previous conversation compacted to save context. Preserve decisions and constraints below; "
             "do not treat this as a fresh user request.\n";
  std::size_t summary_bytes = static_cast<std::size_t>(summary.tellp());
  for (std::size_t i = 0; i < keep_start; ++i) {
    const auto line = compact_message_line(messages[i]) + "\n";
    if (summary_bytes + line.size() > 12000) {
      summary << "[Earlier compacted transcript omitted.]\n";
      break;
    }
    summary << line;
    summary_bytes += line.size();
  }

  Message memory;
  memory.role = "assistant";
  memory.content = truncate_head(summary.str(), 12000, 260);

  std::vector<Message> compacted;
  compacted.reserve(messages.size() - keep_start + 1);
  compacted.push_back(std::move(memory));
  compacted.insert(compacted.end(), messages.begin() + static_cast<std::ptrdiff_t>(keep_start), messages.end());
  messages = std::move(compacted);
  return true;
}

struct ModelRate {
  const char* needle;
  double input_per_million;
  double output_per_million;
};

std::optional<double> estimate_cost_usd(const std::string& model,
                                        std::size_t prompt_tokens,
                                        std::size_t completion_tokens) {
  static constexpr ModelRate rates[] = {
      {"gpt-4.1-mini", 0.40, 1.60},
      {"gpt-4.1-nano", 0.10, 0.40},
      {"gpt-4o-mini", 0.15, 0.60},
      {"gpt-4.1", 2.00, 8.00},
      {"gemini-3-flash-preview", 0.50, 3.00},
  };

  const auto lower = to_lower(model);
  for (const auto& rate : rates) {
    if (lower.find(rate.needle) != std::string::npos) {
      return (static_cast<double>(prompt_tokens) * rate.input_per_million +
              static_cast<double>(completion_tokens) * rate.output_per_million) /
             1'000'000.0;
    }
  }
  return std::nullopt;
}

void print_usage_stats(const std::string& model, std::size_t prompt_estimate, const Message& assistant) {
  const bool actual = assistant.usage.available;
  const auto prompt_tokens = actual ? static_cast<std::size_t>(std::max(0, assistant.usage.prompt_tokens)) : prompt_estimate;
  const auto completion_tokens = actual ? static_cast<std::size_t>(std::max(0, assistant.usage.completion_tokens))
                                        : estimate_completion_tokens(assistant);
  const auto total_tokens = actual && assistant.usage.total_tokens > 0
                                ? static_cast<std::size_t>(assistant.usage.total_tokens)
                                : prompt_tokens + completion_tokens;

  std::ostringstream line;
  line << "[stats] " << (actual ? "actual" : "approx") << " prompt=" << prompt_tokens
       << " completion=" << completion_tokens << " total=" << total_tokens;
  if (const auto cost = estimate_cost_usd(model, prompt_tokens, completion_tokens)) {
    line << " cost=$" << std::fixed << std::setprecision(4) << *cost;
  }
  std::cerr << paint_stderr("\033[2m", line.str()) << "\n";
}

}  // namespace

Agent::Agent(AppConfig config, LlmClient client, ToolRegistry tools)
    : config_(std::move(config)), client_(std::move(client)), tools_(std::move(tools)) {}

void Agent::clear() {
  messages_.clear();
}

void Agent::set_model(const std::string& model) {
  config_.model = model;
  client_ = LlmClient(config_);
}

std::string Agent::system_prompt() const {
  std::ostringstream prompt;
  prompt << "You are pi-lite, a small general-purpose coding agent for learning purposes.\n";
  prompt << "Help with any programming language or project type present in the workspace.\n";
  prompt << "Workspace: " << canonical_workspace(config_.workspace).string() << "\n";
  prompt << "Use tools to inspect files before editing. Prefer minimal changes.\n";
  prompt << "Use apply_patch for focused edits and write_file for creating or replacing whole files. Do not invent file contents you have not read.\n";
  prompt << "Never inspect secret files such as .env, credentials, SSH keys, or private keys.\n";
  prompt << "Do not run destructive shell commands. Explain the final result briefly.\n";
  prompt << "Tool outputs are capped; ask for narrower reads or searches if needed.\n";
  return prompt.str();
}

void Agent::run(const std::string& prompt) {
  auto previous_messages = messages_;
  messages_.push_back(user_message(prompt));

  const auto tool_schemas = tools_.schemas();

  for (int iteration = 1; iteration <= config_.max_iterations; ++iteration) {
    const auto system = system_prompt();
    if (maybe_compact_messages(messages_, system, tool_schemas, config_.max_context_tokens)) {
      std::cerr << paint_stderr("\033[2m", "[memory] compacted conversation context") << "\n";
    }
    const auto prompt_tokens_estimate = estimate_request_tokens(system, messages_, tool_schemas);

    Message assistant;
    MarkdownRenderer renderer(std::cout);
    bool streamed_text = false;
    StatusIndicator status(std::cerr, "thinking", config_.model);
    status.start();

    try {
      assistant = client_.complete(system, messages_, tool_schemas, [&](const std::string& delta) {
        status.stop();
        streamed_text = true;
        renderer.write(delta);
      });
    } catch (const std::exception& error) {
      status.stop();
      std::cerr << "LLM request failed: " << error.what() << "\n";
      if (iteration == 1) messages_ = std::move(previous_messages);
      return;
    }
    status.stop();

    if (streamed_text) {
      renderer.flush();
    } else if (!trim(assistant.content).empty()) {
      renderer.write(assistant.content);
      renderer.flush();
    }

    messages_.push_back(assistant);
    print_usage_stats(config_.model, prompt_tokens_estimate, assistant);

    if (assistant.tool_calls.empty()) return;

    for (const auto& call : assistant.tool_calls) {
      const auto started = std::chrono::steady_clock::now();
      std::cerr << tool_badge("tool") << " " << paint_stderr(ansi_fg(138, 190, 183), call.name)
                << paint_stderr("\033[2m", " running") << "\n";
      const auto result = tools_.execute(call.name, call.arguments);
      std::cerr << tool_badge(result.is_error ? "error" : "ok", result.is_error, true) << " "
                << paint_stderr(ansi_fg(138, 190, 183), call.name) << paint_stderr("\033[2m", " " + elapsed_ms(started))
                << "\n";

      messages_.push_back(tool_message(call, result.content, result.is_error));
      if (result.terminate) return;
    }
  }

  std::cerr << "Stopped after max iterations (" << config_.max_iterations << ").\n";
}

}  // namespace pilite
