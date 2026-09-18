#include "pulseprobe/cgroup.hpp"

#include <cstdlib>

#include "pulseprobe/util.hpp"

namespace pulseprobe::cgroup {
namespace {

std::optional<uint64_t> to_u64(const std::string& token) {
  if (token.empty()) return std::nullopt;
  char* end = nullptr;
  const unsigned long long value = std::strtoull(token.c_str(), &end, 10);
  if (end == token.c_str() || *end != '\0') return std::nullopt;
  return static_cast<uint64_t>(value);
}

}  // namespace

std::optional<CpuStat> parse_cpu_stat(const std::string& content) {
  CpuStat stat;
  bool saw_usage = false;

  for (const auto& line : util::split_lines(content)) {
    const auto fields = util::split_whitespace(line);
    if (fields.size() < 2) continue;
    const auto value = to_u64(fields[1]);
    if (!value) continue;

    if (fields[0] == "usage_usec") {
      stat.usage_usec = *value;
      saw_usage = true;
    } else if (fields[0] == "user_usec") {
      stat.user_usec = *value;
    } else if (fields[0] == "system_usec") {
      stat.system_usec = *value;
    }
  }

  if (!saw_usage) return std::nullopt;
  return stat;
}

std::optional<uint64_t> parse_scalar(const std::string& content) {
  const std::string trimmed = util::trim(content);
  if (trimmed.empty()) return std::nullopt;
  if (trimmed == "max") return std::nullopt;  // unlimited
  return to_u64(trimmed);
}

std::optional<CpuMax> parse_cpu_max(const std::string& content) {
  const auto fields = util::split_whitespace(content);
  if (fields.empty()) return std::nullopt;

  CpuMax result;
  if (fields[0] == "max") return result;  // no quota: cores stays empty

  if (fields.size() < 2) return std::nullopt;
  const auto quota = to_u64(fields[0]);
  const auto period = to_u64(fields[1]);
  if (!quota || !period || *period == 0) return std::nullopt;

  result.cores = static_cast<double>(*quota) / static_cast<double>(*period);
  return result;
}

std::optional<std::string> parse_self_cgroup_path(const std::string& content) {
  for (const auto& line : util::split_lines(content)) {
    // Format is "<hierarchy-id>:<controllers>:<path>". The v2 hierarchy is
    // always id 0 with no controller list.
    if (line.rfind("0::", 0) != 0) continue;
    const std::string path = line.substr(3);
    if (path.empty() || path[0] != '/') return std::nullopt;
    return path;
  }
  return std::nullopt;
}

}  // namespace pulseprobe::cgroup
