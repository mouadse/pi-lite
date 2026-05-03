#include "status_indicator.hpp"

#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <utility>
#include <unistd.h>

namespace pilite {
namespace {

constexpr const char* kReset = "\033[0m";
constexpr const char* kBold = "\033[1m";
constexpr const char* kDim = "\033[2m";
constexpr const char* kProgressActive = "\033]9;4;3;0\007";
constexpr const char* kProgressDone = "\033]9;4;0\007";

}  // namespace

bool terminal_supports_ansi(int fd) {
  if (std::getenv("NO_COLOR")) return false;
  if (!isatty(fd)) return false;
  const char* term = std::getenv("TERM");
  return term && std::string(term) != "dumb";
}

std::string ansi_fg(int r, int g, int b) {
  return "\033[38;2;" + std::to_string(r) + ";" + std::to_string(g) + ";" + std::to_string(b) + "m";
}

std::string ansi_bg(int r, int g, int b) {
  return "\033[48;2;" + std::to_string(r) + ";" + std::to_string(g) + ";" + std::to_string(b) + "m";
}

StatusIndicator::StatusIndicator(std::ostream& out, std::string label, std::string detail)
    : out_(out), label_(std::move(label)), detail_(std::move(detail)), use_ansi_(terminal_supports_ansi(STDERR_FILENO)) {}

StatusIndicator::~StatusIndicator() {
  stop();
}

void StatusIndicator::start() {
  if (running_) return;
  started_ = std::chrono::steady_clock::now();
  running_ = true;

  if (!use_ansi_) {
    out_ << "[" << label_ << "] " << (detail_.empty() ? "working" : detail_) << "...\n" << std::flush;
    wrote_line_ = true;
    return;
  }

  out_ << kProgressActive << std::flush;
  thread_ = std::thread(&StatusIndicator::run, this);
}

void StatusIndicator::stop() {
  if (!running_) return;
  running_ = false;
  if (thread_.joinable()) thread_.join();

  if (use_ansi_) {
    out_ << "\r\033[2K" << kProgressDone << std::flush;
  }
}

void StatusIndicator::run() {
  for (std::size_t frame = 0; running_; ++frame) {
    render_frame(frame);
    std::this_thread::sleep_for(std::chrono::milliseconds(90));
  }
}

void StatusIndicator::render_frame(std::size_t frame) {
  static constexpr const char* frames[] = {"|", "/", "-", "\\"};
  out_ << "\r\033[2K";
  out_ << style(ansi_fg(138, 190, 183), frames[frame % 4]);
  out_ << " " << style(std::string(kBold) + ansi_fg(240, 198, 116), label_);
  if (!detail_.empty()) out_ << style(kDim, " " + detail_);
  out_ << style(kDim, " " + elapsed()) << std::flush;
  wrote_line_ = true;
}

std::string StatusIndicator::elapsed() const {
  const auto now = std::chrono::steady_clock::now();
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - started_).count();
  std::ostringstream out;
  out << std::fixed << std::setprecision(1) << (static_cast<double>(ms) / 1000.0) << "s";
  return out.str();
}

std::string StatusIndicator::style(const std::string& ansi, const std::string& text) const {
  if (!use_ansi_) return text;
  return ansi + text + kReset;
}

}  // namespace pilite
