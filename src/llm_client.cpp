#include "llm_client.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

#include "util.hpp"

namespace pilite {
namespace {

struct CurlGlobal {
  CurlGlobal() { curl_global_init(CURL_GLOBAL_DEFAULT); }
  ~CurlGlobal() { curl_global_cleanup(); }
};

struct StreamParseState {
  std::string raw_body;
  std::string buffer;
  std::string error;
  Message assistant;
  TextDeltaCallback on_text_delta;
  bool done = false;
};

std::string chat_url(std::string base_url) {
  while (!base_url.empty() && base_url.back() == '/') base_url.pop_back();
  return base_url + "/chat/completions";
}

nlohmann::json message_to_openai_json(const Message& message) {
  nlohmann::json out;
  out["role"] = message.role;

  if (message.role == "tool") {
    out["tool_call_id"] = message.tool_call_id;
    out["content"] = message.content;
    return out;
  }

  if (message.role == "assistant" && !message.tool_calls.empty()) {
    out["content"] = message.content.empty() ? nlohmann::json(nullptr) : nlohmann::json(message.content);
    out["tool_calls"] = nlohmann::json::array();
    for (const auto& call : message.tool_calls) {
      out["tool_calls"].push_back({
          {"id", call.id},
          {"type", "function"},
          {"function",
           {
               {"name", call.name},
               {"arguments", call.raw_arguments.empty() ? call.arguments.dump() : call.raw_arguments},
           }},
      });
    }
    return out;
  }

  out["content"] = message.content;
  return out;
}

std::string normalize_newlines(std::string chunk) {
  chunk.erase(std::remove(chunk.begin(), chunk.end(), '\r'), chunk.end());
  return chunk;
}

void merge_tool_call_delta(Message& assistant, const nlohmann::json& item) {
  const auto index = item.value("index", static_cast<int>(assistant.tool_calls.size()));
  if (index < 0) return;
  if (assistant.tool_calls.size() <= static_cast<std::size_t>(index)) {
    assistant.tool_calls.resize(static_cast<std::size_t>(index) + 1);
  }

  auto& call = assistant.tool_calls.at(static_cast<std::size_t>(index));
  if (item.contains("id") && item.at("id").is_string()) call.id = item.at("id").get<std::string>();

  if (item.contains("function") && item.at("function").is_object()) {
    const auto& fn = item.at("function");
    if (fn.contains("name") && fn.at("name").is_string()) call.name += fn.at("name").get<std::string>();
    if (fn.contains("arguments") && fn.at("arguments").is_string()) {
      call.raw_arguments += fn.at("arguments").get<std::string>();
      try {
        call.arguments = nlohmann::json::parse(call.raw_arguments.empty() ? "{}" : call.raw_arguments);
      } catch (const nlohmann::json::parse_error&) {
        call.arguments = nlohmann::json::object();
      }
    }
  }
}

void process_sse_data(StreamParseState& state, const std::string& data) {
  if (data == "[DONE]") {
    state.done = true;
    return;
  }

  nlohmann::json event;
  try {
    event = nlohmann::json::parse(data);
  } catch (const nlohmann::json::parse_error&) {
    return;
  }

  if (event.contains("usage") && event.at("usage").is_object()) {
    const auto& usage = event.at("usage");
    state.assistant.usage.prompt_tokens = usage.value("prompt_tokens", 0);
    state.assistant.usage.completion_tokens = usage.value("completion_tokens", 0);
    state.assistant.usage.total_tokens = usage.value("total_tokens", 0);
    state.assistant.usage.available = true;
  }

  if (event.contains("error")) {
    throw std::runtime_error("LLM stream error: " + event.at("error").dump());
  }
  if (!event.contains("choices") || !event.at("choices").is_array() || event.at("choices").empty()) return;

  const auto& choice = event.at("choices").at(0);
  if (!choice.contains("delta") || !choice.at("delta").is_object()) return;
  const auto& delta = choice.at("delta");

  if (delta.contains("content") && delta.at("content").is_string()) {
    const auto text = delta.at("content").get<std::string>();
    state.assistant.content += text;
    if (state.on_text_delta && !text.empty()) state.on_text_delta(text);
  }

  if (delta.contains("tool_calls") && delta.at("tool_calls").is_array()) {
    for (const auto& item : delta.at("tool_calls")) merge_tool_call_delta(state.assistant, item);
  }
}

void process_sse_event(StreamParseState& state, const std::string& event_text) {
  std::istringstream lines(event_text);
  std::string line;
  std::string data;
  while (std::getline(lines, line)) {
    if (line.rfind("data:", 0) != 0) continue;
    auto value = line.substr(5);
    if (!value.empty() && value.front() == ' ') value.erase(value.begin());
    if (!data.empty()) data += '\n';
    data += value;
  }
  if (!data.empty()) process_sse_data(state, data);
}

std::size_t stream_callback(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
  auto* state = static_cast<StreamParseState*>(userdata);
  const auto bytes = size * nmemb;
  std::string chunk(ptr, bytes);
  state->raw_body += chunk;
  state->buffer += normalize_newlines(std::move(chunk));

  std::size_t delimiter = 0;
  while ((delimiter = state->buffer.find("\n\n")) != std::string::npos) {
    const auto event_text = state->buffer.substr(0, delimiter);
    state->buffer.erase(0, delimiter + 2);
    try {
      process_sse_event(*state, event_text);
    } catch (const std::exception& error) {
      state->error = error.what();
      return 0;
    }
  }

  return bytes;
}

void finalize_streamed_tool_calls(Message& assistant) {
  int generated_id = 0;
  std::vector<ToolCall> calls;
  for (auto& call : assistant.tool_calls) {
    if (call.name.empty()) continue;
    if (call.id.empty()) call.id = "call_" + std::to_string(++generated_id);
    if (call.raw_arguments.empty()) call.raw_arguments = "{}";
    try {
      call.arguments = nlohmann::json::parse(call.raw_arguments);
    } catch (const nlohmann::json::parse_error&) {
      call.arguments = {{"_raw", call.raw_arguments}};
    }
    calls.push_back(std::move(call));
  }
  assistant.tool_calls = std::move(calls);
}

bool retryable_curl_code(CURLcode code) {
  switch (code) {
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_CONNECT:
    case CURLE_OPERATION_TIMEDOUT:
    case CURLE_RECV_ERROR:
    case CURLE_SEND_ERROR:
    case CURLE_GOT_NOTHING:
    case CURLE_PARTIAL_FILE:
    case CURLE_SSL_CONNECT_ERROR:
      return true;
    default:
      return false;
  }
}

}  // namespace

LlmClient::LlmClient(AppConfig config) : config_(std::move(config)) {}

Message LlmClient::complete(const std::string& system_prompt,
                            const std::vector<Message>& messages,
                            const nlohmann::json& tools,
                            const TextDeltaCallback& on_text_delta) const {
  static CurlGlobal curl_global;

  nlohmann::json payload;
  payload["model"] = config_.model;
  payload["temperature"] = config_.temperature;
  payload["max_tokens"] = config_.max_tokens;
  payload["stream"] = true;
  if (config_.base_url.find("openai.com") != std::string::npos ||
      config_.base_url.find("openrouter.ai") != std::string::npos) {
    payload["stream_options"] = {{"include_usage", true}};
  }
  payload["messages"] = nlohmann::json::array();

  if (!system_prompt.empty()) payload["messages"].push_back({{"role", "system"}, {"content", system_prompt}});
  for (const auto& message : messages) payload["messages"].push_back(message_to_openai_json(message));

  if (tools.is_array() && !tools.empty()) {
    payload["tools"] = tools;
    payload["tool_choice"] = "auto";
  }

  const auto payload_text = payload.dump();
  if (config_.verbose) {
    std::cerr << "--- request ---\n" << payload.dump(2) << "\n";
  }

  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  if (!config_.api_key.empty()) {
    headers = curl_slist_append(headers, ("Authorization: Bearer " + config_.api_key).c_str());
  }
  if (config_.base_url.find("openrouter.ai") != std::string::npos) {
    headers = curl_slist_append(headers, "HTTP-Referer: https://github.com/badlogic/pi-mono");
    headers = curl_slist_append(headers, "X-OpenRouter-Title: pi-lite");
    headers = curl_slist_append(headers, "X-OpenRouter-Categories: cli-agent");
  }

  StreamParseState stream_state;
  CURLcode code = CURLE_OK;
  long http_status = 0;
  constexpr int kMaxAttempts = 3;

  for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
    stream_state = StreamParseState{};
    stream_state.assistant.role = "assistant";
    stream_state.on_text_delta = on_text_delta;
    http_status = 0;

    CURL* curl = curl_easy_init();
    if (!curl) {
      curl_slist_free_all(headers);
      throw std::runtime_error("failed to initialize curl");
    }

    curl_easy_setopt(curl, CURLOPT_URL, chat_url(config_.base_url).c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload_text.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(payload_text.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, stream_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &stream_state);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    code = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
    curl_easy_cleanup(curl);

    const bool no_partial_output = stream_state.assistant.content.empty() && stream_state.assistant.tool_calls.empty();
    const bool retryable_http = code == CURLE_OK && no_partial_output && (http_status == 429 || http_status >= 500);
    const bool retryable_curl = code != CURLE_OK && no_partial_output && stream_state.error.empty() &&
                                retryable_curl_code(code);
    if ((!retryable_http && !retryable_curl) || attempt + 1 >= kMaxAttempts) break;

    if (config_.verbose) {
      std::cerr << "Retrying LLM request after attempt " << (attempt + 1) << " failed"
                << " (curl=" << curl_easy_strerror(code) << ", http=" << http_status << ")\n";
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500 * (1 << attempt)));
  }

  curl_slist_free_all(headers);

  if (code != CURLE_OK) {
    if (!stream_state.error.empty()) throw std::runtime_error(stream_state.error);
    throw std::runtime_error(std::string("curl failed: ") + curl_easy_strerror(code));
  }
  if (config_.verbose) std::cerr << "--- response ---\n" << stream_state.raw_body << "\n";
  if (http_status >= 400) {
    throw std::runtime_error("LLM HTTP " + std::to_string(http_status) + ": " + truncate_head(stream_state.raw_body));
  }

  if (!stream_state.buffer.empty()) process_sse_event(stream_state, stream_state.buffer);
  finalize_streamed_tool_calls(stream_state.assistant);
  return stream_state.assistant;
}

}  // namespace pilite
