#include "line_reader.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <poll.h>
#include <sstream>
#include <string>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#include <vector>

namespace pilite {
namespace {

struct CompletionItem {
  std::string value;
  std::string label;
  std::string description;
};

struct CompletionState {
  bool active = false;
  std::vector<CompletionItem> items;
  std::size_t selected = 0;
  std::string replacement_prefix;
  bool argument_completion = false;
};

class RawTerminalMode {
 public:
  RawTerminalMode() {
    if (!isatty(STDIN_FILENO)) return;
    if (tcgetattr(STDIN_FILENO, &original_) != 0) return;

    auto raw = original_;
    raw.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO | ISIG));
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == 0) enabled_ = true;
  }

  ~RawTerminalMode() {
    if (enabled_) tcsetattr(STDIN_FILENO, TCSAFLUSH, &original_);
  }

  bool enabled() const { return enabled_; }

 private:
  termios original_{};
  bool enabled_ = false;
};

enum class KeyType {
  Character,
  Enter,
  Backspace,
  CtrlC,
  CtrlD,
  CtrlL,
  Escape,
  Up,
  Down,
  Tab,
  Unknown,
};

struct KeyPress {
  KeyType type = KeyType::Unknown;
  char c = 0;
};

std::string lower_ascii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

std::string ltrim(std::string value) {
  value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char c) {
                return std::isspace(c) == 0;
              }));
  return value;
}

std::string single_line(std::string value) {
  for (char& c : value) {
    if (c == '\n' || c == '\r' || c == '\t') c = ' ';
  }
  return value;
}

std::string truncate_bytes(const std::string& value, std::size_t max_bytes) {
  if (value.size() <= max_bytes) return value;
  if (max_bytes <= 1) return value.substr(0, max_bytes);
  return value.substr(0, max_bytes - 1) + "…";
}

int terminal_width() {
  winsize size{};
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col > 0) {
    return static_cast<int>(size.ws_col);
  }
  return 80;
}

std::optional<int> fuzzy_score(const std::string& candidate, const std::string& prefix) {
  const auto haystack = lower_ascii(candidate);
  const auto needle = lower_ascii(prefix);
  if (needle.empty()) return 0;
  if (haystack.rfind(needle, 0) == 0) return 1;

  const auto found = haystack.find(needle);
  if (found != std::string::npos) return 20 + static_cast<int>(found);

  std::size_t pos = 0;
  int gaps = 0;
  for (char c : needle) {
    const auto next = haystack.find(c, pos);
    if (next == std::string::npos) return std::nullopt;
    gaps += static_cast<int>(next - pos);
    pos = next + 1;
  }
  return 100 + gaps;
}

std::string command_description(const SlashCommandSpec& command) {
  if (command.argument_hint.empty()) return command.description;
  if (command.description.empty()) return command.argument_hint;
  return command.argument_hint + " — " + command.description;
}

CompletionState build_completion_state(const std::string& line,
                                       const std::vector<SlashCommandSpec>& commands,
                                       const CompletionState& previous) {
  CompletionState state;
  if (commands.empty()) return state;

  const auto trimmed_left = ltrim(line);
  const auto leading_spaces = line.size() - trimmed_left.size();
  if (trimmed_left.empty() || trimmed_left.front() != '/') return state;
  if (leading_spaces > 0 && line.substr(0, leading_spaces).find_first_not_of(" \t") != std::string::npos) {
    return state;
  }

  const auto text = trimmed_left;
  const auto space = text.find(' ');

  if (space == std::string::npos) {
    const auto prefix = text.substr(1);
    struct Ranked {
      int score;
      CompletionItem item;
    };
    std::vector<Ranked> ranked;
    for (const auto& command : commands) {
      if (auto score = fuzzy_score(command.name, prefix)) {
        ranked.push_back({*score,
                          CompletionItem{command.name, "/" + command.name,
                                         single_line(command_description(command))}});
      }
    }
    std::sort(ranked.begin(), ranked.end(), [](const Ranked& a, const Ranked& b) {
      if (a.score != b.score) return a.score < b.score;
      return a.item.label < b.item.label;
    });
    for (const auto& entry : ranked) state.items.push_back(entry.item);
    state.active = !state.items.empty();
    state.replacement_prefix = text;
  } else {
    const auto command_name = text.substr(1, space - 1);
    const auto argument_prefix = text.substr(space + 1);
    const auto command = std::find_if(commands.begin(), commands.end(), [&](const SlashCommandSpec& item) {
      return item.name == command_name;
    });
    if (command == commands.end() || command->argument_completions.empty()) return state;

    struct Ranked {
      int score;
      CompletionItem item;
    };
    std::vector<Ranked> ranked;
    for (const auto& completion : command->argument_completions) {
      const auto search_text = completion.value + " " + completion.label + " " + completion.description;
      if (auto score = fuzzy_score(search_text, argument_prefix)) {
        ranked.push_back({*score,
                          CompletionItem{completion.value,
                                         completion.label.empty() ? completion.value : completion.label,
                                         single_line(completion.description)}});
      }
    }
    std::sort(ranked.begin(), ranked.end(), [](const Ranked& a, const Ranked& b) {
      if (a.score != b.score) return a.score < b.score;
      return a.item.label < b.item.label;
    });
    for (const auto& entry : ranked) state.items.push_back(entry.item);
    state.active = !state.items.empty();
    state.replacement_prefix = argument_prefix;
    state.argument_completion = true;
  }

  if (state.active && previous.active && previous.replacement_prefix == state.replacement_prefix &&
      previous.argument_completion == state.argument_completion && !previous.items.empty()) {
    const auto previous_value = previous.items[std::min(previous.selected, previous.items.size() - 1)].value;
    const auto match = std::find_if(state.items.begin(), state.items.end(), [&](const CompletionItem& item) {
      return item.value == previous_value;
    });
    if (match != state.items.end()) state.selected = static_cast<std::size_t>(match - state.items.begin());
  }

  return state;
}

void apply_completion(std::string& line, const CompletionState& state) {
  if (!state.active || state.items.empty()) return;
  const auto& item = state.items[std::min(state.selected, state.items.size() - 1)];

  auto trimmed_left = ltrim(line);
  const auto leading = line.size() - trimmed_left.size();
  if (state.argument_completion) {
    const auto space = trimmed_left.find(' ');
    if (space == std::string::npos) return;
    line = line.substr(0, leading) + trimmed_left.substr(0, space + 1) + item.value;
  } else {
    line = line.substr(0, leading) + "/" + item.value;
  }
}

std::string render_item(const CompletionItem& item,
                        bool selected,
                        std::size_t primary_width,
                        std::size_t width) {
  const std::string marker = selected ? "→ " : "  ";
  std::ostringstream row;
  row << marker << item.label;

  if (!item.description.empty() && width > marker.size() + primary_width + 3) {
    const auto label_padding = primary_width > item.label.size() ? primary_width - item.label.size() : 0;
    row << std::string(label_padding, ' ') << "  ";
    const auto used = marker.size() + primary_width + 2;
    row << truncate_bytes(item.description, width > used ? width - used : 0);
  }

  return truncate_bytes(row.str(), width);
}

std::vector<std::string> render_completion_rows(const CompletionState& state) {
  std::vector<std::string> rows;
  if (!state.active || state.items.empty()) return rows;

  constexpr std::size_t max_visible = 5;
  const auto width = static_cast<std::size_t>(std::max(20, terminal_width()));
  std::size_t primary_width = 0;
  for (const auto& item : state.items) primary_width = std::max(primary_width, item.label.size());
  primary_width = std::min<std::size_t>(std::max<std::size_t>(primary_width, 8), 28);

  const auto selected = std::min(state.selected, state.items.size() - 1);
  const auto half = max_visible / 2;
  std::size_t start = selected > half ? selected - half : 0;
  if (start + max_visible > state.items.size()) {
    start = state.items.size() > max_visible ? state.items.size() - max_visible : 0;
  }
  const auto end = std::min(start + max_visible, state.items.size());

  for (std::size_t i = start; i < end; ++i) {
    rows.push_back(render_item(state.items[i], i == selected, primary_width, width));
  }

  if (start > 0 || end < state.items.size()) {
    std::ostringstream info;
    info << "  (" << (selected + 1) << "/" << state.items.size() << ")";
    rows.push_back(truncate_bytes(info.str(), width));
  }

  return rows;
}

void clear_rendered_area(std::size_t old_rows) {
  std::cout << "\r\033[2K";
  for (std::size_t i = 0; i < old_rows; ++i) {
    std::cout << "\033[B\r\033[2K";
  }
  if (old_rows > 0) std::cout << "\033[" << old_rows << "A";
  std::cout << "\r";
}

void redraw_line(const std::string& prompt,
                 const std::string& line,
                 const CompletionState& completion,
                 std::size_t& rendered_completion_rows) {
  const auto rows = render_completion_rows(completion);
  clear_rendered_area(rendered_completion_rows);

  std::cout << prompt << line;
  for (const auto& row : rows) {
    std::cout << "\n\r\033[2K" << row;
  }
  if (!rows.empty()) {
    std::cout << "\033[" << rows.size() << "A\r" << prompt << line;
  }
  std::cout << std::flush;
  rendered_completion_rows = rows.size();
}

bool read_byte_with_timeout(char& c, int timeout_ms) {
  pollfd descriptor{STDIN_FILENO, POLLIN, 0};
  const int ready = poll(&descriptor, 1, timeout_ms);
  if (ready <= 0 || (descriptor.revents & POLLIN) == 0) return false;
  return read(STDIN_FILENO, &c, 1) == 1;
}

KeyPress read_key() {
  char c = 0;
  const auto bytes = read(STDIN_FILENO, &c, 1);
  if (bytes <= 0) return {KeyType::Unknown, 0};

  switch (c) {
    case '\n':
    case '\r':
      return {KeyType::Enter, c};
    case '\x04':
      return {KeyType::CtrlD, c};
    case '\x03':
      return {KeyType::CtrlC, c};
    case '\x0c':
      return {KeyType::CtrlL, c};
    case '\x7f':
    case '\b':
      return {KeyType::Backspace, c};
    case '\t':
      return {KeyType::Tab, c};
    case '\x1b': {
      char first = 0;
      if (!read_byte_with_timeout(first, 25)) return {KeyType::Escape, c};
      if (first != '[' && first != 'O') return {KeyType::Escape, c};

      char second = 0;
      if (!read_byte_with_timeout(second, 25)) return {KeyType::Escape, c};
      if (second == 'A') return {KeyType::Up, c};
      if (second == 'B') return {KeyType::Down, c};

      while ((second < '@' || second > '~') && read_byte_with_timeout(second, 25)) {
      }
      return {KeyType::Unknown, c};
    }
    default:
      if (std::isprint(static_cast<unsigned char>(c)) || static_cast<unsigned char>(c) >= 128) {
        return {KeyType::Character, c};
      }
      return {KeyType::Unknown, c};
  }
}

std::string trim(std::string value) {
  value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char c) {
                return std::isspace(c) == 0;
              }));
  value.erase(std::find_if(value.rbegin(), value.rend(), [](unsigned char c) {
                return std::isspace(c) == 0;
              }).base(), value.end());
  return value;
}

void add_to_history(LineHistory& history, const std::string& text) {
  auto trimmed = trim(text);
  if (trimmed.empty()) return;
  if (!history.entries.empty() && history.entries.back() == trimmed) return;
  if (history.entries.size() >= LineHistory::max_size) {
    history.entries.erase(history.entries.begin());
  }
  history.entries.push_back(std::move(trimmed));
}

void history_up(LineHistory& history, std::string& line) {
  if (history.entries.empty()) return;
  if (!history.pos.has_value()) {
    history.stash = line;
    history.pos = history.entries.size() - 1;
  } else if (*history.pos > 0) {
    --*history.pos;
  }
  line = history.entries[*history.pos];
}

void history_down(LineHistory& history, std::string& line) {
  if (!history.pos.has_value()) return;
  if (*history.pos + 1 < history.entries.size()) {
    ++*history.pos;
    line = history.entries[*history.pos];
  } else {
    history.pos = std::nullopt;
    line = history.stash;
  }
}

}  // namespace

bool read_interactive_line(const std::string& prompt, std::string& line, LineHistory& history) {
  static const std::vector<SlashCommandSpec> no_commands;
  return read_interactive_line(prompt, line, history, no_commands);
}

bool read_interactive_line(const std::string& prompt,
                           std::string& line,
                           LineHistory& history,
                           const std::vector<SlashCommandSpec>& slash_commands) {
  line.clear();
  history.pos = std::nullopt;

  if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
    std::cout << prompt << std::flush;
    return static_cast<bool>(std::getline(std::cin, line));
  }

  RawTerminalMode raw;
  if (!raw.enabled()) {
    std::cout << prompt << std::flush;
    return static_cast<bool>(std::getline(std::cin, line));
  }

  CompletionState completion;
  std::size_t rendered_completion_rows = 0;
  std::cout << prompt << std::flush;

  const auto refresh_completion = [&]() {
    completion = build_completion_state(line, slash_commands, completion);
    redraw_line(prompt, line, completion, rendered_completion_rows);
  };

  while (true) {
    const auto key = read_key();

    switch (key.type) {
      case KeyType::Enter:
        if (completion.active) {
          apply_completion(line, completion);
          completion = {};
          redraw_line(prompt, line, completion, rendered_completion_rows);
        }
        add_to_history(history, line);
        clear_rendered_area(rendered_completion_rows);
        rendered_completion_rows = 0;
        std::cout << prompt << line << "\n" << std::flush;
        return true;

      case KeyType::CtrlD:
        if (line.empty()) {
          clear_rendered_area(rendered_completion_rows);
          std::cout << "\n" << std::flush;
          return false;
        }
        break;

      case KeyType::CtrlC:
        line.clear();
        completion = {};
        clear_rendered_area(rendered_completion_rows);
        rendered_completion_rows = 0;
        std::cout << "^C\n" << std::flush;
        return true;

      case KeyType::CtrlL:
        std::cout << "\033[2J\033[H" << std::flush;
        rendered_completion_rows = 0;
        redraw_line(prompt, line, completion, rendered_completion_rows);
        break;

      case KeyType::Escape:
        completion = {};
        redraw_line(prompt, line, completion, rendered_completion_rows);
        break;

      case KeyType::Up:
        if (completion.active && !completion.items.empty()) {
          completion.selected = completion.selected == 0 ? completion.items.size() - 1 : completion.selected - 1;
          redraw_line(prompt, line, completion, rendered_completion_rows);
        } else if (!history.entries.empty()) {
          history_up(history, line);
          completion = {};
          redraw_line(prompt, line, completion, rendered_completion_rows);
        }
        break;

      case KeyType::Down:
        if (completion.active && !completion.items.empty()) {
          completion.selected = (completion.selected + 1) % completion.items.size();
          redraw_line(prompt, line, completion, rendered_completion_rows);
        } else if (history.pos.has_value()) {
          history_down(history, line);
          completion = {};
          redraw_line(prompt, line, completion, rendered_completion_rows);
        }
        break;

      case KeyType::Tab:
        if (completion.active) {
          apply_completion(line, completion);
          completion = build_completion_state(line, slash_commands, {});
          redraw_line(prompt, line, completion, rendered_completion_rows);
        }
        break;

      case KeyType::Backspace:
        if (!line.empty()) {
          line.pop_back();
          if (history.pos.has_value()) history.pos = std::nullopt;
          refresh_completion();
        }
        break;

      case KeyType::Character:
        if (history.pos.has_value()) history.pos = std::nullopt;
        line.push_back(key.c);
        refresh_completion();
        break;

      case KeyType::Unknown:
        break;
    }
  }
}

}  // namespace pilite
