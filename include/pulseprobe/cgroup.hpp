// cgroup v2 parsers. Same contract as procfs.hpp: content in, values out.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace pulseprobe::cgroup {

struct CpuStat {
  uint64_t usage_usec = 0;
  uint64_t user_usec = 0;
  uint64_t system_usec = 0;
};

std::optional<CpuStat> parse_cpu_stat(const std::string& content);

// memory.current / memory.max style files: a single scalar, or the literal
// "max" meaning unlimited, which maps to an empty optional.
std::optional<uint64_t> parse_scalar(const std::string& content);

struct CpuMax {
  std::optional<double> cores;  // empty means unlimited
};

// cpu.max is "<quota> <period>" in microseconds, or "max <period>".
std::optional<CpuMax> parse_cpu_max(const std::string& content);

// Extracts this process's path within the cgroup v2 hierarchy from
// /proc/self/cgroup. The v2 entry is the line with hierarchy ID 0 and an empty
// controller field ("0::/kubepods/besteffort/pod.../abc").
//
// Reading the hierarchy root instead of this path is a common mistake, and a
// quiet one: on a host, or in a container sharing the host's cgroup namespace,
// the root reports the whole machine while claiming to report the container.
// Returns empty when the file has no v2 entry, i.e. cgroup v1 only.
std::optional<std::string> parse_self_cgroup_path(const std::string& content);

}  // namespace pulseprobe::cgroup
