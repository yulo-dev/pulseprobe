#include "pulseprobe/exposition.hpp"

#include <cstdio>
#include <sstream>
#include <vector>

namespace pulseprobe {
namespace {

using Labels = std::vector<std::pair<std::string, std::string>>;

std::string format_double(double value) {
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%.6g", value);
  return buffer;
}

std::string format_u64(uint64_t value) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%llu", static_cast<unsigned long long>(value));
  return buffer;
}

class Writer {
 public:
  explicit Writer(Labels base) : base_(std::move(base)) {}

  void help(const std::string& metric, const std::string& text, const std::string& type) {
    out_ << "# HELP " << metric << ' ' << text << '\n';
    out_ << "# TYPE " << metric << ' ' << type << '\n';
  }

  void sample(const std::string& metric, const std::string& value, const Labels& extra = {}) {
    out_ << metric;
    Labels labels = base_;
    labels.insert(labels.end(), extra.begin(), extra.end());
    if (!labels.empty()) {
      out_ << '{';
      for (size_t i = 0; i < labels.size(); ++i) {
        if (i > 0) out_ << ',';
        out_ << labels[i].first << "=\"" << escape_label_value(labels[i].second) << '"';
      }
      out_ << '}';
    }
    out_ << ' ' << value << '\n';
  }

  void gauge(const std::string& metric, const std::string& text, double value) {
    help(metric, text, "gauge");
    sample(metric, format_double(value));
  }

  void gauge(const std::string& metric, const std::string& text, uint64_t value) {
    help(metric, text, "gauge");
    sample(metric, format_u64(value));
  }

  void blank() { out_ << '\n'; }

  std::string str() const { return out_.str(); }

 private:
  Labels base_;
  std::ostringstream out_;
};

}  // namespace

std::string escape_label_value(const std::string& value) {
  std::string escaped;
  escaped.reserve(value.size());
  for (const char c : value) {
    switch (c) {
      case '\\': escaped += "\\\\"; break;
      case '"': escaped += "\\\""; break;
      case '\n': escaped += "\\n"; break;
      default: escaped += c;
    }
  }
  return escaped;
}

std::string render_prometheus(const Snapshot& snapshot, const ExpositionOptions& options) {
  Labels base;
  if (!options.node_name.empty()) base.emplace_back("node", options.node_name);

  Writer w(base);

  w.help("pulseprobe_build_info", "Agent build metadata.", "gauge");
  w.sample("pulseprobe_build_info", "1", {{"version", options.version}});
  w.blank();

  w.gauge("pulseprobe_ready", "1 once a CPU delta is available, 0 during the first sampling cycle.",
          static_cast<double>(snapshot.ready ? 1 : 0));
  w.gauge("pulseprobe_sampling_generation", "Sampling cycles completed since start.",
          snapshot.generation);
  w.blank();

  // ---- host CPU ----------------------------------------------------------
  // Absent rather than zero on the first cycle: a gauge that is not yet known
  // must not be published as 0, or every agent restart shows a utilisation
  // trough that never happened.
  if (snapshot.cpu.usage_percent) {
    w.gauge("pulseprobe_cpu_usage_percent",
            "Host CPU busy time over the last sampling interval, excluding idle and iowait.",
            *snapshot.cpu.usage_percent);
  }
  if (snapshot.cpu.iowait_percent) {
    w.gauge("pulseprobe_cpu_iowait_percent",
            "Host CPU time blocked on I/O over the last sampling interval.",
            *snapshot.cpu.iowait_percent);
  }
  w.gauge("pulseprobe_cpu_online_count", "Online logical CPUs visible in procfs.",
          static_cast<uint64_t>(snapshot.cpu.online_cpus));
  w.blank();

  // ---- host memory -------------------------------------------------------
  w.gauge("pulseprobe_memory_total_bytes", "MemTotal.", snapshot.memory.total_bytes);
  w.gauge("pulseprobe_memory_available_bytes", "MemAvailable.", snapshot.memory.available_bytes);
  w.gauge("pulseprobe_memory_used_bytes", "MemTotal minus MemAvailable.",
          snapshot.memory.used_bytes);
  w.gauge("pulseprobe_memory_usage_percent", "Used memory as a share of MemTotal.",
          snapshot.memory.usage_percent);
  w.blank();

  // ---- load and uptime ---------------------------------------------------
  w.help("pulseprobe_load_average", "Kernel load average.", "gauge");
  w.sample("pulseprobe_load_average", format_double(snapshot.load.load1), {{"window", "1m"}});
  w.sample("pulseprobe_load_average", format_double(snapshot.load.load5), {{"window", "5m"}});
  w.sample("pulseprobe_load_average", format_double(snapshot.load.load15), {{"window", "15m"}});
  w.gauge("pulseprobe_uptime_seconds", "Seconds since boot.", snapshot.load.uptime_seconds);
  w.blank();

  // ---- cgroup ------------------------------------------------------------
  w.gauge("pulseprobe_cgroup_available",
          "1 when a cgroup v2 hierarchy is readable, in which case the cgroup series below "
          "describe this container rather than the host.",
          static_cast<double>(snapshot.cgroup.available ? 1 : 0));
  if (snapshot.cgroup.available) {
    if (snapshot.cgroup.cpu_usage_percent) {
      w.gauge("pulseprobe_cgroup_cpu_usage_percent",
              "CPU used by this cgroup as a share of its own allowance.",
              *snapshot.cgroup.cpu_usage_percent);
    }
    if (snapshot.cgroup.cpu_limit_cores) {
      w.gauge("pulseprobe_cgroup_cpu_limit_cores", "CPU quota from cpu.max, in cores.",
              *snapshot.cgroup.cpu_limit_cores);
    }
    if (snapshot.cgroup.memory_current_bytes) {
      w.gauge("pulseprobe_cgroup_memory_current_bytes", "memory.current.",
              *snapshot.cgroup.memory_current_bytes);
    }
    if (snapshot.cgroup.memory_max_bytes) {
      w.gauge("pulseprobe_cgroup_memory_max_bytes", "memory.max, omitted when unlimited.",
              *snapshot.cgroup.memory_max_bytes);
    }
    if (snapshot.cgroup.memory_usage_percent) {
      w.gauge("pulseprobe_cgroup_memory_usage_percent",
              "memory.current as a share of memory.max.", *snapshot.cgroup.memory_usage_percent);
    }
  }
  w.blank();

  // ---- GPU ---------------------------------------------------------------
  w.gauge("pulseprobe_gpu_available",
          "1 when the NVIDIA management library loaded on this node, 0 otherwise.",
          static_cast<double>(snapshot.gpu.available ? 1 : 0));

  if (snapshot.gpu.available && !snapshot.gpu.devices.empty()) {
    w.help("pulseprobe_gpu_info", "Per-device identity.", "gauge");
    for (const auto& device : snapshot.gpu.devices) {
      w.sample("pulseprobe_gpu_info", "1",
               {{"gpu", format_u64(device.index)},
                {"uuid", device.uuid},
                {"name", device.name},
                {"driver_version", snapshot.gpu.driver_version}});
    }

    // job_id is carried on every device series so a scrape answers "which job
    // is holding this GPU", not just "this GPU is busy". Unattributed devices
    // keep the label with an empty value so the series shape stays stable.
    const auto device_labels = [](const GpuDevice& device) {
      return Labels{{"gpu", format_u64(device.index)},
                    {"uuid", device.uuid},
                    {"job_id", device.job_id}};
    };

    w.help("pulseprobe_gpu_utilization_percent", "Share of the last sampling period during "
                                                 "which any kernel was executing.", "gauge");
    for (const auto& device : snapshot.gpu.devices) {
      w.sample("pulseprobe_gpu_utilization_percent", format_double(device.utilization_percent),
               device_labels(device));
    }

    w.help("pulseprobe_gpu_memory_used_bytes", "Device memory in use.", "gauge");
    for (const auto& device : snapshot.gpu.devices) {
      w.sample("pulseprobe_gpu_memory_used_bytes", format_u64(device.memory_used_bytes),
               device_labels(device));
    }

    w.help("pulseprobe_gpu_memory_total_bytes", "Total device memory.", "gauge");
    for (const auto& device : snapshot.gpu.devices) {
      w.sample("pulseprobe_gpu_memory_total_bytes", format_u64(device.memory_total_bytes),
               device_labels(device));
    }

    w.help("pulseprobe_gpu_temperature_celsius", "Device temperature.", "gauge");
    for (const auto& device : snapshot.gpu.devices) {
      w.sample("pulseprobe_gpu_temperature_celsius", format_double(device.temperature_celsius),
               device_labels(device));
    }

    w.help("pulseprobe_gpu_power_watts", "Instantaneous board power draw.", "gauge");
    for (const auto& device : snapshot.gpu.devices) {
      w.sample("pulseprobe_gpu_power_watts", format_double(device.power_watts),
               device_labels(device));
    }
  }

  return w.str();
}

}  // namespace pulseprobe
