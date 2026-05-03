#pragma once

#include <ostream>
#include <string>

namespace pilite {

class MarkdownRenderer {
 public:
  explicit MarkdownRenderer(std::ostream& out);

  void write(const std::string& chunk);
  void flush();

 private:
  std::string render_line(const std::string& line);
  std::string render_inline(const std::string& text) const;
  std::string style(const std::string& code, const std::string& text) const;
  void emit_line(const std::string& line);

  std::ostream& out_;
  std::string pending_;
  bool in_code_block_ = false;
  bool use_ansi_ = false;
};

}  // namespace pilite
