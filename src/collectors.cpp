#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

#include "pulseprobe/cgroup.hpp"
#include "pulseprobe/collector.hpp"
#include "pulseprobe/jobmap.hpp"
#include "pulseprobe/nvml.hpp"
#include "pulseprobe/procfs.hpp"
#include "pulseprobe/util.hpp"

namespace pulseprobe {
namespace {

class CpuCollector : public Collector {
 public:
  explicit CpuCollector(std::string stat_path) : stat_path_(std::move(stat_path)) {}

  const char* name() const override { return "cpu"; }

  void collect(Snapshot* out) override {
    const auto content = util::read_file(stat_path_);
    if (!content) return;

    out->cpu.online_cpus = procfs::count_online_cpus(*content);

    const auto times = procfs::parse_stat(*content);
    if (!times) return;

    if (previous_) {
      const auto usage = procfs::compute_utilization(*previous_, *times);
      if (usage) {
        out->cpu.usage_percent = usage->busy_percent;
        out->cpu.iowait_percent = usage->iowait_percent;
      }
    }
    // First cycle records a baseline only. Reporting a number here would mean
    // reporting utilisation since boot, which is not what the metric says.
    previous_ = times;
  }

 private:
  std::string stat_path_;
  std::optional<procfs::CpuTimes> previous_;
};

class MemoryCollector : public Collector {
 public:
  explicit MemoryCollector(std::string meminfo_path) : meminfo_path_(std::move(meminfo_path)) {}

  const char* name() const override { return "memory"; }

  void collect(Snapshot* out) override {
    const auto content = util::read_file(meminfo_path_);
    if (!content) return;
    const auto metrics = procfs::parse_meminfo(*content);
    if (metrics) out->memory = *metrics;
  }

 private:
  std::string meminfo_path_;
};

class LoadCollector : public Collector {
 public:
  LoadCollector(std::string loadavg_path, std::string uptime_path)
      : loadavg_path_(std::move(loadavg_path)), uptime_path_(std::move(uptime_path)) {}

  const char* name() const override { return "load"; }

  void collect(Snapshot* out) override {
    if (const auto content = util::read_file(loadavg_path_)) {
      if (const auto metrics = procfs::parse_loadavg(*content)) {
        out->load.load1 = metrics->load1;
        out->load.load5 = metrics->load5;
        out->load.load15 = metrics->load15;
      }
    }
    if (const auto content = util::read_file(uptime_path_)) {
      if (const auto uptime = procfs::parse_uptime(*content)) out->load.uptime_seconds = *uptime;
    }
  }

 private:
  std::string loadavg_path_;
  std::string uptime_path_;
};

// Reads the cgroup v2 unified hierarchy. This is what makes the agent honest
// inside a container: /proc/stat and /proc/meminfo are not namespaced, so a
// container sees the host's CPUs and the host's memory. Anything that alerts
// on container headroom has to read the cgroup instead.
class CgroupCollector : public Collector {
 public:
  CgroupCollector(std::string root, const std::string& procfs_root)
      : root_(resolve_own_cgroup(root, procfs_root)) {}

  const char* name() const override { return "cgroup"; }

  void collect(Snapshot* out) override {
    const auto cpu_stat_content = util::read_file(root_ + "/cpu.stat");
    const auto memory_current_content = util::read_file(root_ + "/memory.current");
    if (!cpu_stat_content && !memory_current_content) return;  // not cgroup v2

    out->cgroup.available = true;

    std::optional<double> limit_cores;
    if (const auto cpu_max_content = util::read_file(root_ + "/cpu.max")) {
      if (const auto cpu_max = cgroup::parse_cpu_max(*cpu_max_content)) {
        limit_cores = cpu_max->cores;
        out->cgroup.cpu_limit_cores = cpu_max->cores;
      }
    }

    if (cpu_stat_content) {
      if (const auto stat = cgroup::parse_cpu_stat(*cpu_stat_content)) {
        const int64_t now_us = util::monotonic_micros();
        if (previous_usage_usec_ && previous_monotonic_us_ && stat->usage_usec >= *previous_usage_usec_) {
          const double elapsed_us = static_cast<double>(now_us - *previous_monotonic_us_);
          const double busy_us = static_cast<double>(stat->usage_usec - *previous_usage_usec_);
          // Denominator is the cgroup's allowance, not one CPU: a container
          // limited to 2 cores and using both reads 100%, not 200%.
          const double cores = limit_cores.value_or(
              out->cpu.online_cpus > 0 ? static_cast<double>(out->cpu.online_cpus) : 1.0);
          if (elapsed_us > 0.0 && cores > 0.0) {
            double percent = 100.0 * busy_us / (elapsed_us * cores);
            percent = std::clamp(percent, 0.0, 100.0);
            out->cgroup.cpu_usage_percent = percent;
          }
        }
        previous_usage_usec_ = stat->usage_usec;
        previous_monotonic_us_ = now_us;
      }
    }

    if (memory_current_content) {
      if (const auto current = cgroup::parse_scalar(*memory_current_content)) {
        out->cgroup.memory_current_bytes = *current;
        if (const auto max_content = util::read_file(root_ + "/memory.max")) {
          const auto max = cgroup::parse_scalar(*max_content);  // empty == "max"
          out->cgroup.memory_max_bytes = max;
          if (max && *max > 0) {
            out->cgroup.memory_usage_percent =
                100.0 * static_cast<double>(*current) / static_cast<double>(*max);
          }
        }
      }
    }
  }

 private:
  // Resolves the directory that actually describes *this* process, rather than
  // the hierarchy root. With a private cgroup namespace (the usual container
  // case) /proc/self/cgroup reports "0::/" and the two are the same; on a host,
  // or under a shared cgroup namespace, they are not, and reading the root
  // silently reports the whole machine's numbers as if they were the
  // container's.
  static std::string resolve_own_cgroup(const std::string& root, const std::string& procfs_root) {
    const auto content = util::read_file(procfs_root + "/self/cgroup");
    if (!content) return root;
    const auto relative = cgroup::parse_self_cgroup_path(*content);
    if (!relative || *relative == "/") return root;

    const std::string candidate = root + *relative;
    // Fall back to the root when the delegated path is not visible, which
    // happens when only a subtree is bind-mounted into the container.
    if (util::read_file(candidate + "/cpu.stat")) return candidate;
    return root;
  }

  std::string root_;
  std::optional<uint64_t> previous_usage_usec_;
  std::optional<int64_t> previous_monotonic_us_;
};

class GpuCollector : public Collector {
 public:
  explicit GpuCollector(std::string job_map_dir) : job_map_dir_(std::move(job_map_dir)) {
    library_.open();
  }

  const char* name() const override { return "gpu"; }

  void collect(Snapshot* out) override {
    out->gpu.available = library_.available();
    if (!library_.available()) return;

    out->gpu.driver_version = library_.driver_version();
    const auto job_map = jobmap::load_directory(job_map_dir_);

    for (const auto& sample : library_.sample()) {
      GpuDevice device;
      device.index = sample.index;
      device.uuid = sample.uuid;
      device.name = sample.name;
      device.utilization_percent = sample.utilization_percent;
      device.memory_used_bytes = sample.memory_used_bytes;
      device.memory_total_bytes = sample.memory_total_bytes;
      device.temperature_celsius = sample.temperature_celsius;
      device.power_watts = sample.power_watts;

      // Attribute the device to the job holding the most GPU memory on it.
      // A device shared by two jobs is reported against the larger consumer;
      // per-process series would be the finer-grained alternative but PIDs
      // churn far too fast to make good label values.
      uint64_t best = 0;
      for (const auto& process : sample.processes) {
        const auto it = job_map.find(process.pid);
        if (it == job_map.end()) continue;
        if (process.used_gpu_memory_bytes >= best) {
          best = process.used_gpu_memory_bytes;
          device.job_id = it->second;
        }
      }

      out->gpu.devices.push_back(std::move(device));
    }
  }

 private:
  nvml::Library library_;
  std::string job_map_dir_;
};

}  // namespace

std::unique_ptr<Collector> make_cpu_collector(const Config& config) {
  return std::make_unique<CpuCollector>(config.procfs_root + "/stat");
}

std::unique_ptr<Collector> make_memory_collector(const Config& config) {
  return std::make_unique<MemoryCollector>(config.procfs_root + "/meminfo");
}

std::unique_ptr<Collector> make_load_collector(const Config& config) {
  return std::make_unique<LoadCollector>(config.procfs_root + "/loadavg",
                                         config.procfs_root + "/uptime");
}

std::unique_ptr<Collector> make_cgroup_collector(const Config& config) {
  return std::make_unique<CgroupCollector>(config.cgroup_root, config.procfs_root);
}

std::unique_ptr<Collector> make_gpu_collector(const Config& config) {
  if (!config.gpu_enabled) return nullptr;
  return std::make_unique<GpuCollector>(config.job_map_dir);
}

}  // namespace pulseprobe
