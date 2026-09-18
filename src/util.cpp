#include "pulseprobe/util.hpp"

#include <chrono>
#include <fstream>
#include <sstream>

namespace pulseprobe::util {

std::optional<std::string> read_file(const std::string& path) {
  std::ifstream in(path, std::ios::in | std::ios::binary);
  if (!in) return std::nullopt;
  std::ostringstream buffer;
  buffer << in.rdbuf();
  if (in.bad()) return std::nullopt;
  return buffer.str();
}

std::vector<std::string> split_whitespace(const std::string& line) {
  std::vector<std::string> out;
  std::istringstream stream(line);
  std::string token;
  while (stream >> token) out.push_back(token);
  return out;
}

std::vector<std::string> split_lines(const std::string& content) {
  std::vector<std::string> out;
  std::istringstream stream(content);
  std::string line;
  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    out.push_back(line);
  }
  return out;
}

std::string trim(const std::string& s) {
  const char* ws = " \t\n\r\f\v";
  const auto begin = s.find_first_not_of(ws);
  if (begin == std::string::npos) return {};
  const auto end = s.find_last_not_of(ws);
  return s.substr(begin, end - begin + 1);
}

int64_t now_unix_millis() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

int64_t monotonic_micros() {
  using namespace std::chrono;
  return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

}  // namespace pulseprobe::util
