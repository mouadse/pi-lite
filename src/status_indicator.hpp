#pragma once

#include <atomic>
#include <chrono>
#include <ostream>
#include <string>
#include <thread>

namespace pilite {

class StatusIndicator {
 public:
  StatusIndicator(std::ostream& out, std::string label, std::string detail = "");
  ~StatusIndicator();

  StatusIndicator(const StatusIndicator&) = delete;
  StatusIndicator& operator=(const StatusIndicator&) = delete;

  void start();
  void stop();

 private:
  void run();
  void render_frame(std::size_t frame);
  std::string elapsed() const;
  std::string style(const std::string& ansi, const std::string& text) const;

  std::ostream& out_;
  std::string label_;
  std::string detail_;
  std::atomic<bool> running_{false};
  std::thread thread_;
  std::chrono::steady_clock::time_point started_;
  bool use_ansi_ = false;
  bool wrote_line_ = false;
};

bool terminal_supports_ansi(int fd);
std::string ansi_fg(int r, int g, int b);
std::string ansi_bg(int r, int g, int b);

}  // namespace pilite
