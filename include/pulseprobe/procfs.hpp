// Pure parsers over procfs file *contents*. They take strings rather than
// paths so the unit tests can drive them from fixtures without a Linux host.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "pulseprobe/snapshot.hpp"

namespace pulseprobe::procfs {

struct CpuTimes {
  uint64_t user = 0;
  uint64_t nice = 0;
  uint64_t system = 0;
  uint64_t idle = 0;
  uint64_t iowait = 0;
  uint64_t irq = 0;
  uint64_t softirq = 0;
  uint64_t steal = 0;

  uint64_t total() const;

  // iowait counts as idle: the CPU is not executing anything during it.
  // Folding iowait into busy time inflates utilisation on I/O-heavy hosts.
  uint64_t idle_all() const;
};

// Parses the aggregate "cpu" line of /proc/stat. Field count varies by kernel
// version (guest and guest_nice were added later), so trailing fields are
// tolerated and anything short of 8 columns is rejected.
std::optional<CpuTimes> parse_stat(const std::string& content);

// Counts the per-core "cpuN" lines.
unsigned count_online_cpus(const std::string& content);

// MemAvailable is what the kernel estimates is obtainable without swapping.
// It is the right basis for "used"; MemFree is not, because it excludes
// reclaimable page cache.
std::optional<MemoryMetrics> parse_meminfo(const std::string& content);

std::optional<LoadMetrics> parse_loadavg(const std::string& content);
std::optional<double> parse_uptime(const std::string& content);

struct CpuUtilization {
  double busy_percent = 0.0;
  double iowait_percent = 0.0;
};

// Returns empty when the counters did not advance, or when they went
// backwards (CPU hotplug and counter resets both do this).
std::optional<CpuUtilization> compute_utilization(const CpuTimes& prev, const CpuTimes& cur);

}  // namespace pulseprobe::procfs
