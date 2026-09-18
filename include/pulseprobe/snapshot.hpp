// Immutable metrics snapshot. Produced by the sampling thread, consumed by
// HTTP scrapes. Nothing in here is mutated after publication.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pulseprobe {

struct CpuMetrics {
  // Empty on the very first sample: CPU utilisation is a delta between two
  // readings of a monotonic counter, so one reading tells us nothing.
  std::optional<double> usage_percent;
  std::optional<double> iowait_percent;
  unsigned online_cpus = 0;
};

struct MemoryMetrics {
  uint64_t total_bytes = 0;
  uint64_t available_bytes = 0;
  uint64_t used_bytes = 0;
  double usage_percent = 0.0;
};

struct LoadMetrics {
  double load1 = 0.0;
  double load5 = 0.0;
  double load15 = 0.0;
  double uptime_seconds = 0.0;
};

// cgroup v2 view. Inside a container /proc/stat and /proc/meminfo report the
// host (or the VM hosting the container runtime), not the container's own
// limits. These fields carry the cgroup-scoped numbers instead.
struct CgroupMetrics {
  bool available = false;
  std::optional<double> cpu_usage_percent;   // relative to the cgroup's CPU allowance
  std::optional<double> cpu_limit_cores;     // from cpu.max; empty means unlimited
  std::optional<uint64_t> memory_current_bytes;
  std::optional<uint64_t> memory_max_bytes;  // empty means "max" (unlimited)
  std::optional<double> memory_usage_percent;
};

struct GpuDevice {
  unsigned index = 0;
  std::string uuid;
  std::string name;
  double utilization_percent = 0.0;
  uint64_t memory_used_bytes = 0;
  uint64_t memory_total_bytes = 0;
  double temperature_celsius = 0.0;
  double power_watts = 0.0;
  // Scheduler job that owns the compute processes on this device, when a job
  // mapping is configured. Empty string means unattributed.
  std::string job_id;
};

struct GpuMetrics {
  bool available = false;
  std::string driver_version;
  std::vector<GpuDevice> devices;
};

struct Snapshot {
  int64_t unix_millis = 0;
  uint64_t generation = 0;  // increments once per sampling cycle
  bool ready = false;       // false until a usable CPU delta exists
  CpuMetrics cpu;
  MemoryMetrics memory;
  LoadMetrics load;
  CgroupMetrics cgroup;
  GpuMetrics gpu;
};

}  // namespace pulseprobe
