#include "pulseprobe/config.hpp"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

namespace pulseprobe {
namespace {

const char* env_or_null(const char* key) {
  const char* value = std::getenv(key);
  if (value && *value) return value;
  return nullptr;
}

bool parse_bool(const std::string& text, bool* out) {
  if (text == "1" || text == "true" || text == "yes") { *out = true; return true; }
  if (text == "0" || text == "false" || text == "no") { *out = false; return true; }
  return false;
}

bool parse_int(const std::string& text, int* out) {
  char* end = nullptr;
  const long value = std::strtol(text.c_str(), &end, 10);
  if (end == text.c_str() || *end != '\0') return false;
  *out = static_cast<int>(value);
  return true;
}

bool parse_double(const std::string& text, double* out) {
  char* end = nullptr;
  const double value = std::strtod(text.c_str(), &end);
  if (end == text.c_str() || *end != '\0') return false;
  *out = value;
  return true;
}

bool apply(const std::string& key, const std::string& value, Config* config) {
  if (key == "interval") {
    if (!parse_int(value, &config->sampling_interval_seconds)) return false;
    return config->sampling_interval_seconds > 0;
  }
  if (key == "port") {
    if (!parse_int(value, &config->listen_port)) return false;
    return config->listen_port > 0 && config->listen_port < 65536;
  }
  if (key == "host") { config->listen_host = value; return true; }
  if (key == "procfs-root") { config->procfs_root = value; return true; }
  if (key == "cgroup-root") { config->cgroup_root = value; return true; }
  if (key == "job-map-dir") { config->job_map_dir = value; return true; }
  if (key == "node-name") { config->node_name = value; return true; }
  if (key == "cpu-threshold") return parse_double(value, &config->cpu_threshold_percent);
  if (key == "memory-threshold") return parse_double(value, &config->memory_threshold_percent);
  if (key == "gpu") return parse_bool(value, &config->gpu_enabled);
  return false;
}

}  // namespace

std::string Config::usage() {
  return
      "pulseprobe [options]\n"
      "  --interval=<seconds>        sampling interval (default 5)\n"
      "  --port=<port>               listen port (default 9100)\n"
      "  --host=<addr>               listen address (default 0.0.0.0)\n"
      "  --procfs-root=<path>        procfs mount point (default /proc)\n"
      "  --cgroup-root=<path>        cgroup v2 mount point (default /sys/fs/cgroup)\n"
      "  --job-map-dir=<path>        scheduler job mapping directory; empty disables\n"
      "  --node-name=<name>          node label applied to every series\n"
      "  --cpu-threshold=<percent>   structured-log threshold (default 90)\n"
      "  --memory-threshold=<pct>    structured-log threshold (default 90)\n"
      "  --gpu=<true|false>          enable NVML collection (default true)\n"
      "Every option also reads from PULSEPROBE_<OPTION>, e.g. PULSEPROBE_NODE_NAME.\n";
}

bool Config::parse(int argc, char** argv, Config* out) {
  // Environment first so that command-line flags win over it.
  struct EnvBinding { const char* env; const char* key; };
  static const EnvBinding kBindings[] = {
      {"PULSEPROBE_INTERVAL", "interval"},
      {"PULSEPROBE_PORT", "port"},
      {"PULSEPROBE_HOST", "host"},
      {"PULSEPROBE_PROCFS_ROOT", "procfs-root"},
      {"PULSEPROBE_CGROUP_ROOT", "cgroup-root"},
      {"PULSEPROBE_JOB_MAP_DIR", "job-map-dir"},
      {"PULSEPROBE_NODE_NAME", "node-name"},
      {"PULSEPROBE_CPU_THRESHOLD", "cpu-threshold"},
      {"PULSEPROBE_MEMORY_THRESHOLD", "memory-threshold"},
      {"PULSEPROBE_GPU", "gpu"},
  };

  for (const auto& binding : kBindings) {
    if (const char* value = env_or_null(binding.env)) {
      if (!apply(binding.key, value, out)) {
        std::cerr << "invalid value for " << binding.env << ": " << value << "\n";
        return false;
      }
    }
  }

  for (int i = 1; i < argc; ++i) {
    std::string argument = argv[i];
    if (argument == "--help" || argument == "-h") {
      std::cout << usage();
      std::exit(0);
    }
    if (argument.rfind("--", 0) != 0) {
      std::cerr << "unexpected argument: " << argument << "\n" << usage();
      return false;
    }
    argument = argument.substr(2);
    const auto equals = argument.find('=');
    if (equals == std::string::npos) {
      std::cerr << "expected --key=value, got: " << argv[i] << "\n" << usage();
      return false;
    }
    const std::string key = argument.substr(0, equals);
    const std::string value = argument.substr(equals + 1);
    if (!apply(key, value, out)) {
      std::cerr << "invalid option: " << argv[i] << "\n" << usage();
      return false;
    }
  }

  return true;
}

}  // namespace pulseprobe
