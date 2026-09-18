#include "pulseprobe/jobmap.hpp"

#include <cstdlib>
#include <filesystem>
#include <system_error>

#include "pulseprobe/util.hpp"

namespace pulseprobe::jobmap {

std::vector<unsigned> parse_job_file(const std::string& content) {
  std::vector<unsigned> pids;
  for (const auto& raw : util::split_lines(content)) {
    const std::string line = util::trim(raw);
    if (line.empty()) continue;
    char* end = nullptr;
    const unsigned long value = std::strtoul(line.c_str(), &end, 10);
    if (end == line.c_str() || *end != '\0' || value == 0) continue;
    pids.push_back(static_cast<unsigned>(value));
  }
  return pids;
}

PidToJob load_directory(const std::string& dir) {
  PidToJob map;
  if (dir.empty()) return map;

  std::error_code ec;
  std::filesystem::directory_iterator it(dir, ec);
  if (ec) return map;  // no mapping configured on this node

  for (const auto& entry : it) {
    if (!entry.is_regular_file(ec) || ec) continue;
    const std::string job_id = entry.path().filename().string();
    const auto content = util::read_file(entry.path().string());
    if (!content) continue;
    for (const unsigned pid : parse_job_file(*content)) {
      // A PID belongs to exactly one job; first writer wins on the rare
      // overlap seen mid-teardown.
      map.emplace(pid, job_id);
    }
  }
  return map;
}

}  // namespace pulseprobe::jobmap
