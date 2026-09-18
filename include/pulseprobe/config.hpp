#pragma once

#include <string>

namespace pulseprobe {

struct Config {
  int sampling_interval_seconds = 5;
  int listen_port = 9100;
  std::string listen_host = "0.0.0.0";

  // Roots are configurable so a DaemonSet can mount the host's procfs and
  // sysfs at a non-standard path, and so tests can point at fixtures.
  std::string procfs_root = "/proc";
  std::string cgroup_root = "/sys/fs/cgroup";

  // Empty disables per-job GPU attribution.
  std::string job_map_dir;

  double cpu_threshold_percent = 90.0;
  double memory_threshold_percent = 90.0;

  bool gpu_enabled = true;

  // Populated from the Kubernetes downward API (spec.nodeName) so every
  // series can be attributed to the node the agent runs on.
  std::string node_name;

  // Parses argv then falls back to PULSEPROBE_* environment variables.
  // Returns false and writes to stderr on malformed input.
  static bool parse(int argc, char** argv, Config* out);
  static std::string usage();
};

}  // namespace pulseprobe
