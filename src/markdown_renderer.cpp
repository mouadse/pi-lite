#include "markdown_renderer.hpp"

#include <algorithm>
#include <sstream>
#include <string>
#include <unistd.h>

#include "status_indicator.hpp"
#include "util.hpp"

namespace pilite {
namespace {

constexpr const char* kReset = "\033[0m";
constexpr const char* kBold = "\033[1m";
constexpr const char* kDim = "\033[2m";
constexpr const char* kUnderline = "\033[4m";
constexpr const char* kItalic = "\033[3m";

bool starts_with(const std::string& text, const std::string& prefix) {
  return text.rfind(prefix, 0) == 0;
}

std::string strip_prefix_hashes(const std::string& line, int& level) {
  level = 0;
  while (level < static_cast<int>(line.size()) && line[static_cast<std::size_t>(level)] == '#') ++level;
  if (level == 0 || level > 6) return line;
  if (line.size() <= static_cast<std::size_t>(level) || line[static_cast<std::size_t>(level)] != ' ') {
    level = 0;
    return line;
  }
  return trim(line.substr(static_cast<std::size_t>(level) + 1));
}

bool is_ordered_list(const std::string& line, std::size_t& marker_end) {
  std::size_t i = 0;
  while (i < line.size() && std::isdigit(static_cast<unsigned char>(line[i]))) ++i;
  if (i == 0 || i + 1 >= line.size()) return false;
  if (line[i] != '.' || line[i + 1] != ' ') return false;
  marker_end = i + 2;
  return true;
}

bool is_unordered_list(const std::string& line, std::size_t& marker_end) {
  if (line.empty() || (line[0] != '-' && line[0] != '*')) return false;
  if (line.size() < 2 || !std::isspace(static_cast<unsigned char>(line[1]))) return false;
  marker_end = 1;
  while (marker_end < line.size() && std::isspace(static_cast<unsigned char>(line[marker_end]))) ++marker_end;
  return marker_end < line.size();
}

bool is_table_separator(const std::string& line) {
  auto stripped = trim(line);
  if (stripped.size() < 3) return false;
  return std::all_of(stripped.begin(), stripped.end(), [](char c) {
    return c == '|' || c == '-' || c == ':' || std::isspace(static_cast<unsigned char>(c));
  }) && stripped.find('-') != std::string::npos && stripped.find('|') != std::string::npos;
}

std::string plain_inline(std::string text) {
  for (const auto marker : {"**", "__"}) {
    std::size_t found = 0;
    while ((found = text.find(marker, found)) != std::string::npos) text.erase(found, 2);
  }
  text.erase(std::remove(text.begin(), text.end(), '`'), text.end());
  return text;
}

std::string code_block_border(const std::string& label) {
  std::string title = label.empty() ? " code " : " code: " + label + " ";
  if (title.size() > 50) title = title.substr(0, 47) + "...";
  const auto width = static_cast<std::size_t>(72);
  if (title.size() + 4 >= width) return "+--" + title;
  return "+--" + title + std::string(width - title.size() - 4, '-') + "+";
}

}  // namespace

MarkdownRenderer::MarkdownRenderer(std::ostream& out) : out_(out), use_ansi_(terminal_supports_ansi(STDOUT_FILENO)) {}

void MarkdownRenderer::write(const std::string& chunk) {
  pending_ += chunk;

  std::size_t newline = 0;
  while ((newline = pending_.find('\n')) != std::string::npos) {
    auto line = pending_.substr(0, newline);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    pending_.erase(0, newline + 1);
    emit_line(line);
  }
}

void MarkdownRenderer::flush() {
  if (!pending_.empty()) {
    emit_line(pending_);
    pending_.clear();
  }
}

void MarkdownRenderer::emit_line(const std::string& line) {
  out_ << render_line(line) << '\n' << std::flush;
}

std::string MarkdownRenderer::style(const std::string& code, const std::string& text) const {
  if (!use_ansi_ || text.empty()) return text;
  return code + text + kReset;
}

std::string MarkdownRenderer::render_inline(const std::string& text) const {
  std::string out;

  for (std::size_t i = 0; i < text.size();) {
    if (text[i] == '`') {
      const auto end = text.find('`', i + 1);
      if (end != std::string::npos) {
        const auto code = text.substr(i + 1, end - i - 1);
        out += use_ansi_ ? style(ansi_bg(35, 43, 43) + ansi_fg(138, 190, 183), " " + code + " ") : code;
        i = end + 1;
        continue;
      }
    }

    if (text[i] == '[') {
      const auto close = text.find("](", i + 1);
      if (close != std::string::npos) {
        const auto end = text.find(')', close + 2);
        if (end != std::string::npos) {
          const auto label = text.substr(i + 1, close - i - 1);
          const auto url = text.substr(close + 2, end - close - 2);
          out += style(ansi_fg(138, 190, 183) + std::string(kUnderline), label);
          out += style(kDim, " (" + url + ")");
          i = end + 1;
          continue;
        }
      }
    }

    if (starts_with(text.substr(i), "**")) {
      const auto end = text.find("**", i + 2);
      if (end != std::string::npos) {
        out += style(std::string(kBold) + ansi_fg(230, 230, 230), text.substr(i + 2, end - i - 2));
        i = end + 2;
        continue;
      }
    }

    if (text[i] == '*' && i + 1 < text.size() && text[i + 1] != ' ') {
      const auto end = text.find('*', i + 1);
      if (end != std::string::npos) {
        out += style(std::string(kItalic) + ansi_fg(210, 210, 210), text.substr(i + 1, end - i - 1));
        i = end + 1;
        continue;
      }
    }

    out += text[i++];
  }

  out.erase(std::remove(out.begin(), out.end(), '`'), out.end());
  out.erase(std::remove(out.begin(), out.end(), '*'), out.end());
  return out;
}

std::string MarkdownRenderer::render_line(const std::string& line) {
  const auto stripped = trim(line);

  if (starts_with(stripped, "```")) {
    const auto language = trim(stripped.substr(3));
    in_code_block_ = !in_code_block_;
    if (in_code_block_) {
      return style(std::string(kDim) + ansi_fg(138, 190, 183), code_block_border(language));
    }
    return style(std::string(kDim) + ansi_fg(138, 190, 183), "+" + std::string(70, '-') + "+");
  }

  if (in_code_block_) return style(ansi_fg(170, 170, 170), "  " + line);
  if (stripped.empty()) return "";

  int heading_level = 0;
  const auto heading = strip_prefix_hashes(stripped, heading_level);
  if (heading_level > 0) {
    std::string prefix = heading_level == 1 ? "\n" : "";
    const auto ansi = std::string(kBold) + (heading_level == 1 ? kUnderline : "") + ansi_fg(240, 198, 116);
    return prefix + style(ansi, plain_inline(heading));
  }

  std::size_t unordered_end = 0;
  if (is_unordered_list(stripped, unordered_end)) {
    return "  " + style(ansi_fg(138, 190, 183), "* ") + render_inline(stripped.substr(unordered_end));
  }

  std::size_t ordered_end = 0;
  if (is_ordered_list(stripped, ordered_end)) {
    return "  " + style(ansi_fg(138, 190, 183), stripped.substr(0, ordered_end)) + render_inline(stripped.substr(ordered_end));
  }

  if (starts_with(stripped, "> ")) {
    return style(ansi_fg(181, 189, 104), "| ") + style(std::string(kItalic) + ansi_fg(170, 170, 170), render_inline(stripped.substr(2)));
  }

  if (stripped == "---" || stripped == "***" || stripped == "___") {
    return style(kDim, std::string(72, '-'));
  }

  if (is_table_separator(stripped)) return style(kDim, "  " + stripped);
  if (starts_with(stripped, "|") && stripped.ends_with("|")) return style(ansi_fg(170, 170, 170), "  " + render_inline(stripped));

  return render_inline(line);
}

}  // namespace pilite
