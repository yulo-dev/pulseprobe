#include "pulseprobe/procfs.hpp"

#include <cstdlib>

#include "pulseprobe/util.hpp"

namespace pulseprobe::procfs {
namespace {

std::optional<uint64_t> to_u64(const std::string& token) {
  if (token.empty()) return std::nullopt;
  char* end = nullptr;
  const unsigned long long value = std::strtoull(token.c_str(), &end, 10);
  if (end == token.c_str() || *end != '\0') return std::nullopt;
  return static_cast<uint64_t>(value);
}

std::optional<double> to_double(const std::string& token) {
  if (token.empty()) return std::nullopt;
  char* end = nullptr;
  const double value = std::strtod(token.c_str(), &end);
  if (end == token.c_str() || *end != '\0') return std::nullopt;
  return value;
}

}  // namespace

uint64_t CpuTimes::total() const {
  return user + nice + system + idle + iowait + irq + softirq + steal;
}

uint64_t CpuTimes::idle_all() const { return idle + iowait; }

std::optional<CpuTimes> parse_stat(const std::string& content) {
  for (const auto& line : util::split_lines(content)) {
    const auto fields = util::split_whitespace(line);
    if (fields.empty() || fields[0] != "cpu") continue;

    // Aggregate line: "cpu user nice system idle iowait irq softirq steal ..."
    // Kernels since 2.6.24 append guest and guest_nice; newer ones may append
    // more. Require the eight we use and ignore anything beyond them.
    if (fields.size() < 9) return std::nullopt;

    uint64_t values[8];
    for (int i = 0; i < 8; ++i) {
      const auto parsed = to_u64(fields[i + 1]);
      if (!parsed) return std::nullopt;
      values[i] = *parsed;
    }

    CpuTimes times;
    times.user = values[0];
    times.nice = values[1];
    times.system = values[2];
    times.idle = values[3];
    times.iowait = values[4];
    times.irq = values[5];
    times.softirq = values[6];
    times.steal = values[7];
    return times;
  }
  return std::nullopt;
}

unsigned count_online_cpus(const std::string& content) {
  unsigned count = 0;
  for (const auto& line : util::split_lines(content)) {
    if (line.rfind("cpu", 0) != 0) continue;
    if (line.size() < 4) continue;
    if (line[3] >= '0' && line[3] <= '9') ++count;
  }
  return count;
}

std::optional<MemoryMetrics> parse_meminfo(const std::string& content) {
  std::optional<uint64_t> total_kb;
  std::optional<uint64_t> available_kb;

  for (const auto& line : util::split_lines(content)) {
    const auto colon = line.find(':');
    if (colon == std::string::npos) continue;
    const std::string key = line.substr(0, colon);
    const auto fields = util::split_whitespace(line.substr(colon + 1));
    if (fields.empty()) continue;
    const auto value = to_u64(fields[0]);
    if (!value) continue;

    if (key == "MemTotal") total_kb = value;
    else if (key == "MemAvailable") available_kb = value;
    if (total_kb && available_kb) break;
  }

  if (!total_kb || *total_kb == 0) return std::nullopt;
  if (!available_kb) return std::nullopt;

  MemoryMetrics metrics;
  metrics.total_bytes = *total_kb * 1024ULL;
  metrics.available_bytes = *available_kb * 1024ULL;
  // MemAvailable can momentarily exceed MemTotal on some kernels; clamp rather
  // than underflow the unsigned subtraction.
  metrics.used_bytes = metrics.available_bytes >= metrics.total_bytes
                           ? 0
                           : metrics.total_bytes - metrics.available_bytes;
  metrics.usage_percent =
      100.0 * static_cast<double>(metrics.used_bytes) / static_cast<double>(metrics.total_bytes);
  return metrics;
}

std::optional<LoadMetrics> parse_loadavg(const std::string& content) {
  const auto fields = util::split_whitespace(content);
  if (fields.size() < 3) return std::nullopt;
  const auto one = to_double(fields[0]);
  const auto five = to_double(fields[1]);
  const auto fifteen = to_double(fields[2]);
  if (!one || !five || !fifteen) return std::nullopt;

  LoadMetrics metrics;
  metrics.load1 = *one;
  metrics.load5 = *five;
  metrics.load15 = *fifteen;
  return metrics;
}

std::optional<double> parse_uptime(const std::string& content) {
  const auto fields = util::split_whitespace(content);
  if (fields.empty()) return std::nullopt;
  return to_double(fields[0]);
}

std::optional<CpuUtilization> compute_utilization(const CpuTimes& prev, const CpuTimes& cur) {
  const uint64_t prev_total = prev.total();
  const uint64_t cur_total = cur.total();

  // Counters are monotonic in normal operation. Going backwards means the set
  // of CPUs changed underneath us (hotplug) or the counters reset; either way
  // the delta is meaningless and this cycle is dropped.
  if (cur_total <= prev_total) return std::nullopt;
  if (cur.idle_all() < prev.idle_all()) return std::nullopt;
  if (cur.iowait < prev.iowait) return std::nullopt;

  const double delta_total = static_cast<double>(cur_total - prev_total);
  const double delta_idle = static_cast<double>(cur.idle_all() - prev.idle_all());
  const double delta_iowait = static_cast<double>(cur.iowait - prev.iowait);

  CpuUtilization result;
  result.busy_percent = 100.0 * (delta_total - delta_idle) / delta_total;
  result.iowait_percent = 100.0 * delta_iowait / delta_total;

  if (result.busy_percent < 0.0) result.busy_percent = 0.0;
  if (result.busy_percent > 100.0) result.busy_percent = 100.0;
  return result;
}

}  // namespace pulseprobe::procfs
