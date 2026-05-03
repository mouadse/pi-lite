#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "agent.hpp"
#include "config.hpp"
#include "line_reader.hpp"
#include "llm_client.hpp"
#include "tool.hpp"
#include "tools.hpp"
#include "util.hpp"

namespace {

std::vector<pilite::SlashCommandSpec> interactive_slash_commands() {
  return {
      {.name = "exit", .description = "quit pi-lite"},
      {.name = "quit", .description = "quit pi-lite"},
      {.name = "clear", .description = "clear the terminal"},
      {.name = "reset", .description = "clear conversation history"},
      {.name = "history", .description = "print stored conversation/tool history"},
      {.name = "edit", .description = "open $EDITOR for a multi-line prompt"},
      {.name = "model", .description = "show or switch the model (/model <name>)"},
      {.name = "help", .description = "show interactive commands"},
  };
}

void print_help() {
  std::cout << R"(pi-lite: a tiny coding agent inspired by pi-mono

Usage:
  pi-lite [options] "prompt"
  pi-lite [options]

Options:
  --model MODEL           Model name. Defaults from PI_LITE_MODEL, OPENAI_MODEL, or OPENROUTER_MODEL.
  --base-url URL          OpenAI-compatible base URL. Defaults to OpenRouter if OPENROUTER_API_KEY exists.
  --api-key KEY           API key. Prefer environment variables over this flag.
  --workspace PATH        Workspace root. Defaults to current directory.
  --allow-bash            Enable the bash tool. Disabled by default.
  --max-iterations N      Maximum tool/LLM loop iterations. Default: 8.
  --max-tokens N          Maximum assistant output tokens. Default: 4096.
  --max-context-tokens N  Approximate context budget before compaction. Default: 24000. Use 0 to disable.
  --temperature N         Sampling temperature. Default: 0.2.
  --verbose               Print raw JSON requests and responses.
  --self-test             Run local tool smoke tests without calling an LLM.
  --help                  Show this help.

Environment:
  PI_LITE_BASE_URL, PI_LITE_API_KEY, PI_LITE_MODEL
  OPENAI_BASE_URL, OPENAI_API_KEY, OPENAI_MODEL
  OPENROUTER_API_KEY, OPENROUTER_MODEL

The program also reads a .env file from the workspace, without printing secrets.
Interactive commands: /exit, /clear, /reset, /history, /edit, /model, /help, !<shell command>.
Type / in interactive mode to open slash command autocomplete.
)";
}

std::string join_prompt(const std::vector<std::string>& parts) {
  std::ostringstream out;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i) out << ' ';
    out << parts[i];
  }
  return out.str();
}

bool is_sensitive_file_ref(const std::filesystem::path& path) {
  const auto filename = pilite::to_lower(path.filename().string());
  static const std::vector<std::string> sensitive_names = {
      ".env", ".env.local", ".env.production", ".env.development",
      "credentials.json", "secrets.json", "id_rsa", "id_ed25519", "known_hosts"};
  if (std::find(sensitive_names.begin(), sensitive_names.end(), filename) != sensitive_names.end()) return true;

  const auto full = pilite::to_lower(path.generic_string());
  return full.find("/.ssh/") != std::string::npos || full.find("secret") != std::string::npos ||
         full.find("credential") != std::string::npos || full.find("private_key") != std::string::npos;
}

std::string expand_file_refs(const std::string& prompt, const std::filesystem::path& workspace) {
  std::string out;
  std::size_t pos = 0;

  while (pos < prompt.size()) {
    const auto at = prompt.find('@', pos);
    if (at == std::string::npos) {
      out += prompt.substr(pos);
      break;
    }

    out += prompt.substr(pos, at - pos);

    const bool at_start = at == 0;
    const bool after_space = at > 0 && std::isspace(static_cast<unsigned char>(prompt[at - 1]));
    if (!at_start && !after_space) {
      out += '@';
      pos = at + 1;
      continue;
    }

    auto end = at + 1;
    while (end < prompt.size() && !std::isspace(static_cast<unsigned char>(prompt[end]))) ++end;

    const std::string filepath = prompt.substr(at + 1, end - at - 1);
    if (filepath.empty()) {
      out += '@';
      pos = at + 1;
      continue;
    }

    std::filesystem::path resolved;
    try {
      resolved = pilite::resolve_workspace_path(workspace, filepath);
    } catch (const std::exception&) {
      out += '@' + filepath;
      pos = end;
      continue;
    }

    std::error_code ec;
    auto read_path = std::filesystem::weakly_canonical(resolved, ec);
    if (ec) read_path = std::filesystem::absolute(resolved).lexically_normal();

    if (!std::filesystem::exists(read_path, ec) || !std::filesystem::is_regular_file(read_path, ec) ||
        is_sensitive_file_ref(resolved) || is_sensitive_file_ref(read_path)) {
      out += '@' + filepath;
      pos = end;
      continue;
    }

    std::ifstream in(read_path, std::ios::binary);
    if (!in) {
      out += '@' + filepath;
      pos = end;
      continue;
    }

    std::ostringstream buffer;
    buffer << in.rdbuf();
    auto text = buffer.str();
    if (text.find('\0') != std::string::npos) {
      out += '@' + filepath;
      pos = end;
      continue;
    }
    if (text.size() > 100 * 1024) {
      text = text.substr(0, 100 * 1024) + "\n[... truncated at 100KB]";
    }

    const auto relative = pilite::relative_to_workspace(workspace, read_path);
    out += "\n```" + relative + "\n" + text;
    if (!text.empty() && text.back() != '\n') out += '\n';
    out += "```\n";

    pos = end;
  }

  return out;
}

std::string config_value(const std::unordered_map<std::string, std::string>& dotenv,
                         std::initializer_list<const char*> keys) {
  for (const char* key : keys) {
    if (auto value = pilite::lookup_config_value(dotenv, key)) return *value;
  }
  return "";
}

bool looks_local_url(const std::string& url) {
  return url.find("localhost") != std::string::npos || url.find("127.0.0.1") != std::string::npos ||
         url.find("0.0.0.0") != std::string::npos;
}

bool command_is_dangerous(const std::string& command) {
  const auto lower = pilite::to_lower(command);
  static const std::vector<std::string> blocked = {
      "rm -rf",        "sudo ",     "mkfs",     "dd if=",        "git reset --hard",
      "git clean -fd", "shutdown",  "reboot",   "poweroff",      ":(){",
      "chmod -r 777 /", "chown -r ", "> /dev/sd", "wipefs",        "fdisk"};
  return std::any_of(blocked.begin(), blocked.end(), [&](const std::string& needle) {
    return lower.find(needle) != std::string::npos;
  });
}

void run_local_shell_command(const std::filesystem::path& workspace, const std::string& command) {
  if (pilite::trim(command).empty()) {
    std::cerr << "Usage: !<shell command>\n";
    return;
  }

  if (command_is_dangerous(command)) {
    std::cerr << "Blocked dangerous local shell command.\n";
    return;
  }

  const auto root = pilite::canonical_workspace(workspace).string();
  const auto wrapped = "cd " + pilite::shell_quote(root) + " && timeout 30s bash -lc " +
                       pilite::shell_quote(command) + " 2>&1";
  const auto result = pilite::run_shell_capture(wrapped);

  if (result.exit_code != 0) std::cerr << "[exit " << result.exit_code << "]\n";
  const auto output = pilite::truncate_tail(result.output);
  if (!pilite::trim(output).empty()) std::cout << output;
}

void print_history(const pilite::Agent& agent) {
  const auto& messages = agent.messages();
  if (messages.empty()) {
    std::cout << "No conversation history.\n";
    return;
  }

  for (std::size_t i = 0; i < messages.size(); ++i) {
    const auto& message = messages[i];
    std::cout << (i + 1) << ". [" << message.role;
    if (!message.name.empty()) std::cout << ":" << message.name;
    if (message.is_error) std::cout << ":error";
    std::cout << "]\n";

    std::string preview = pilite::trim(message.content);
    if (preview.empty() && !message.tool_calls.empty()) {
      std::ostringstream calls;
      calls << "tool calls:";
      for (const auto& call : message.tool_calls) calls << " " << call.name;
      preview = calls.str();
    }
    if (!preview.empty()) {
      preview = pilite::truncate_head(preview, 1200, 24);
      std::cout << preview;
      if (preview.back() != '\n') std::cout << "\n";
    }
  }
}

std::optional<std::string> read_prompt_from_editor() {
  const auto temp = std::filesystem::temp_directory_path() /
                    ("pi-lite-prompt-" + std::to_string(static_cast<long long>(getpid())) + ".md");
  {
    std::ofstream out(temp);
    out << "";
  }

  const char* editor_env = std::getenv("EDITOR");
  const std::string editor = editor_env && *editor_env ? editor_env : "vi";
  const auto command = editor + " " + pilite::shell_quote(temp.string());
  const auto code = std::system(command.c_str());
  if (code != 0) {
    std::filesystem::remove(temp);
    std::cerr << "Editor exited with status " << code << ".\n";
    return std::nullopt;
  }

  std::ifstream in(temp);
  std::ostringstream buffer;
  buffer << in.rdbuf();
  std::filesystem::remove(temp);

  auto prompt = pilite::trim(buffer.str());
  if (prompt.empty()) return std::nullopt;
  return prompt;
}

std::string read_all_stdin() {
  std::ostringstream buffer;
  buffer << std::cin.rdbuf();
  return pilite::trim(buffer.str());
}

}  // namespace

int main(int argc, char** argv) {
  pilite::AppConfig config;
  config.workspace = std::filesystem::current_path();
  std::vector<std::string> prompt_parts;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const auto need_value = [&](const char* name) -> std::string {
      if (i + 1 >= argc) throw std::runtime_error(std::string(name) + " needs a value");
      return argv[++i];
    };

    try {
      if (arg == "--help" || arg == "-h") {
        print_help();
        return 0;
      } else if (arg == "--model") {
        config.model = need_value("--model");
      } else if (arg == "--base-url") {
        config.base_url = need_value("--base-url");
      } else if (arg == "--api-key") {
        config.api_key = need_value("--api-key");
      } else if (arg == "--workspace") {
        config.workspace = need_value("--workspace");
      } else if (arg == "--allow-bash") {
        config.allow_bash = true;
      } else if (arg == "--verbose") {
        config.verbose = true;
      } else if (arg == "--self-test") {
        config.self_test = true;
      } else if (arg == "--max-iterations") {
        config.max_iterations = std::stoi(need_value("--max-iterations"));
      } else if (arg == "--max-tokens") {
        config.max_tokens = std::stoi(need_value("--max-tokens"));
      } else if (arg == "--max-context-tokens") {
        config.max_context_tokens = std::stoi(need_value("--max-context-tokens"));
      } else if (arg == "--temperature") {
        config.temperature = std::stod(need_value("--temperature"));
      } else if (arg.rfind("--", 0) == 0) {
        throw std::runtime_error("unknown option: " + arg);
      } else {
        prompt_parts.push_back(arg);
      }
    } catch (const std::exception& error) {
      std::cerr << error.what() << "\n";
      return 2;
    }
  }

  config.workspace = pilite::canonical_workspace(config.workspace);
  const auto dotenv = pilite::load_dotenv(config.workspace / ".env");
  const auto openrouter_key = config_value(dotenv, {"OPENROUTER_API_KEY"});

  if (config.base_url.empty()) {
    config.base_url = config_value(dotenv, {"PI_LITE_BASE_URL", "OPENAI_BASE_URL", "OPENROUTER_BASE_URL"});
  }
  if (config.api_key.empty()) {
    config.api_key = config_value(dotenv, {"PI_LITE_API_KEY", "OPENAI_API_KEY", "OPENROUTER_API_KEY"});
  }
  if (config.model.empty()) {
    config.model = config_value(dotenv, {"PI_LITE_MODEL", "OPENAI_MODEL", "OPENROUTER_MODEL"});
  }

  const bool using_openrouter = !openrouter_key.empty() &&
                                config_value(dotenv, {"PI_LITE_BASE_URL", "OPENAI_BASE_URL"}).empty();

  if (config.base_url.empty()) {
    config.base_url = using_openrouter ? "https://openrouter.ai/api/v1" : "https://api.openai.com/v1";
  }
  if (config.model.empty()) {
    config.model = using_openrouter ? "deepseek/deepseek-v4-flash" : "gpt-4.1-mini";
  }

  if (config.api_key.empty() && !looks_local_url(config.base_url)) {
    std::cerr << "No API key found. Set PI_LITE_API_KEY, OPENAI_API_KEY, or OPENROUTER_API_KEY.\n";
    return 2;
  }

  pilite::ToolRegistry tools;
  pilite::register_default_tools(tools, config.workspace, config.allow_bash);

  if (config.self_test) {
    std::cout << "Workspace: " << config.workspace << "\n";
    std::cout << "Registered tools: " << tools.tools().size() << "\n";

    bool ok = true;
    const auto check = [&](const std::string& name, const pilite::ToolResult& result) {
      std::cout << name << ": " << (result.is_error ? "error" : "ok") << "\n";
      if (result.is_error) {
        std::cout << result.content;
        ok = false;
      }
    };
    const auto check_contains = [&](const std::string& name,
                                    const pilite::ToolResult& result,
                                    const std::string& expected) {
      check(name, result);
      if (!result.is_error && result.content.find(expected) == std::string::npos) {
        std::cout << name << ": missing expected text: " << expected << "\n";
        ok = false;
      }
    };

    const std::string test_path = ".pi-lite-self-test.tmp";
    check("write_file", tools.execute("write_file", {{"path", test_path}, {"content", "pi-lite self test marker\n"}}));
    check_contains("read_file", tools.execute("read_file", {{"path", test_path}, {"limit", 5}}), "pi-lite self test marker");
    check_contains("grep_files", tools.execute("grep_files", {{"path", test_path}, {"pattern", "self test marker"}}), test_path);
    std::filesystem::remove(config.workspace / test_path);

    const auto result = tools.execute("list_files", {{"path", "."}, {"limit", 20}});
    std::cout << result.content;
    return ok && !result.is_error ? 0 : 1;
  }

  pilite::Agent agent(config, pilite::LlmClient(config), std::move(tools));
  const auto one_shot_prompt = join_prompt(prompt_parts);
  const auto stdin_prompt = !isatty(STDIN_FILENO) ? read_all_stdin() : std::string();
  if (!one_shot_prompt.empty()) {
    if (stdin_prompt.empty()) {
      agent.run(expand_file_refs(one_shot_prompt, config.workspace));
    } else {
      agent.run(expand_file_refs(one_shot_prompt + "\n\nPiped stdin:\n" + stdin_prompt, config.workspace));
    }
    return 0;
  }

  if (!isatty(STDIN_FILENO)) {
    if (!stdin_prompt.empty()) agent.run(expand_file_refs(stdin_prompt, config.workspace));
    return 0;
  }

  std::cout << "pi-lite coding agent. Type /exit to quit, /help for commands, !cmd for local shell.\n";
  const auto slash_commands = interactive_slash_commands();
  std::string line;
  while (true) {
    if (!pilite::read_interactive_line("> ", line, slash_commands)) break;
    line = pilite::trim(line);
    if (line == "/exit" || line == "/quit") break;
    if (line == "/clear" || line == "clear") {
      std::cout << "\033[2J\033[H" << std::flush;
      continue;
    }
    if (line == "/reset") {
      agent.clear();
      std::cout << "Conversation history cleared.\n";
      continue;
    }
    if (line == "/history") {
      print_history(agent);
      continue;
    }
    if (line == "/edit") {
      if (auto edited = read_prompt_from_editor()) agent.run(expand_file_refs(*edited, config.workspace));
      continue;
    }
    if (line == "/model" || line.rfind("/model ", 0) == 0) {
      auto arg = line == "/model" ? std::string() : pilite::trim(line.substr(7));
      if (arg.empty()) {
        std::cout << "Current model: " << config.model << "\n";
      } else {
        config.model = arg;
        agent.set_model(config.model);
        std::cout << "Switched to model: " << config.model << "\n";
      }
      continue;
    }
    if (line == "/help") {
      std::cout << "Commands: /exit, /clear, /reset, /history, /edit, /model, /help, !<shell command>. Type / for autocomplete; prefix shell commands with ! to run locally.\n";
      continue;
    }
    if (line.empty()) continue;
    if (line.front() == '!') {
      run_local_shell_command(config.workspace, pilite::trim(line.substr(1)));
      continue;
    }
    agent.run(expand_file_refs(line, config.workspace));
  }

  return 0;
}
