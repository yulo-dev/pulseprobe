// Runtime loader for NVIDIA's management library.
//
// NVML is opened with dlopen rather than linked at build time. The agent is one
// binary that ships to every node in a cluster, and most nodes have no NVIDIA
// driver. Link-time coupling would mean either a second build or a binary that
// refuses to start on a CPU node; loading at runtime lets the same image run
// everywhere and simply report pulseprobe_gpu_available 0 where there is no
// driver.
//
// The function-pointer table also avoids a build dependency on nvml.h: the few
// structs used here are redeclared with the ABI layout NVML documents.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pulseprobe::nvml {

struct ProcessUsage {
  unsigned pid = 0;
  uint64_t used_gpu_memory_bytes = 0;
};

struct DeviceSample {
  unsigned index = 0;
  std::string uuid;
  std::string name;
  double utilization_percent = 0.0;
  uint64_t memory_used_bytes = 0;
  uint64_t memory_total_bytes = 0;
  double temperature_celsius = 0.0;
  double power_watts = 0.0;
  std::vector<ProcessUsage> processes;
};

class Library {
 public:
  Library();
  ~Library();

  Library(const Library&) = delete;
  Library& operator=(const Library&) = delete;

  // Attempts dlopen + nvmlInit. Safe to call on a host with no driver; returns
  // false and leaves the object usable (but unavailable).
  bool open();

  bool available() const { return available_; }
  const std::string& driver_version() const { return driver_version_; }

  // Reason the library is unavailable, for logging. Empty when available.
  const std::string& last_error() const { return last_error_; }

  std::vector<DeviceSample> sample();

 private:
  struct Impl;
  Impl* impl_ = nullptr;
  bool available_ = false;
  std::string driver_version_;
  std::string last_error_;
};

}  // namespace pulseprobe::nvml
