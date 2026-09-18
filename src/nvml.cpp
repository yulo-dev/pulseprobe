#include "pulseprobe/nvml.hpp"

#include <dlfcn.h>

#include <cstring>

namespace pulseprobe::nvml {
namespace {

// Minimal redeclaration of the NVML ABI. Declaring these here rather than
// including nvml.h keeps the build free of any CUDA toolkit dependency, which
// matters because this binary is built once and shipped to nodes that mostly
// have no NVIDIA software installed at all.
using nvmlReturn_t = int;
constexpr nvmlReturn_t NVML_SUCCESS = 0;

using nvmlDevice_t = void*;

struct nvmlUtilization_t {
  unsigned int gpu;
  unsigned int memory;
};

struct nvmlMemory_t {
  unsigned long long total;
  unsigned long long free;
  unsigned long long used;
};

// v3 layout. v2 lacks the trailing instance IDs; the symbol name encodes which
// layout the driver expects, so the two are never mixed.
struct nvmlProcessInfo_v3_t {
  unsigned int pid;
  unsigned long long usedGpuMemory;
  unsigned int gpuInstanceId;
  unsigned int computeInstanceId;
};

constexpr unsigned NVML_TEMPERATURE_GPU = 0;
constexpr int NVML_DEVICE_NAME_BUFFER_SIZE = 96;
constexpr int NVML_DEVICE_UUID_BUFFER_SIZE = 96;
constexpr int NVML_SYSTEM_DRIVER_VERSION_BUFFER_SIZE = 96;
constexpr unsigned MAX_PROCESSES = 64;

}  // namespace

struct Library::Impl {
  void* handle = nullptr;

  nvmlReturn_t (*init)() = nullptr;
  nvmlReturn_t (*shutdown)() = nullptr;
  nvmlReturn_t (*get_driver_version)(char*, unsigned) = nullptr;
  nvmlReturn_t (*get_device_count)(unsigned*) = nullptr;
  nvmlReturn_t (*get_handle_by_index)(unsigned, nvmlDevice_t*) = nullptr;
  nvmlReturn_t (*get_name)(nvmlDevice_t, char*, unsigned) = nullptr;
  nvmlReturn_t (*get_uuid)(nvmlDevice_t, char*, unsigned) = nullptr;
  nvmlReturn_t (*get_utilization)(nvmlDevice_t, nvmlUtilization_t*) = nullptr;
  nvmlReturn_t (*get_memory_info)(nvmlDevice_t, nvmlMemory_t*) = nullptr;
  nvmlReturn_t (*get_temperature)(nvmlDevice_t, unsigned, unsigned*) = nullptr;
  nvmlReturn_t (*get_power_usage)(nvmlDevice_t, unsigned*) = nullptr;
  nvmlReturn_t (*get_compute_processes)(nvmlDevice_t, unsigned*, nvmlProcessInfo_v3_t*) = nullptr;

  template <typename Fn>
  bool bind(Fn& target, const char* symbol) {
    target = reinterpret_cast<Fn>(dlsym(handle, symbol));
    return target != nullptr;
  }
};

Library::Library() : impl_(new Impl()) {}

Library::~Library() {
  if (impl_) {
    if (available_ && impl_->shutdown) impl_->shutdown();
    if (impl_->handle) dlclose(impl_->handle);
    delete impl_;
  }
}

bool Library::open() {
  // libnvidia-ml.so.1 is the versioned SONAME installed by the driver package.
  // The unversioned .so only exists where a development package is present, so
  // it is a fallback, not the primary.
  static const char* kCandidates[] = {"libnvidia-ml.so.1", "libnvidia-ml.so"};

  for (const char* candidate : kCandidates) {
    impl_->handle = dlopen(candidate, RTLD_LAZY | RTLD_LOCAL);
    if (impl_->handle) break;
  }
  if (!impl_->handle) {
    last_error_ = "libnvidia-ml not present; GPU metrics disabled";
    return false;
  }

  const bool bound =
      impl_->bind(impl_->init, "nvmlInit_v2") &&
      impl_->bind(impl_->shutdown, "nvmlShutdown") &&
      impl_->bind(impl_->get_driver_version, "nvmlSystemGetDriverVersion") &&
      impl_->bind(impl_->get_device_count, "nvmlDeviceGetCount_v2") &&
      impl_->bind(impl_->get_handle_by_index, "nvmlDeviceGetHandleByIndex_v2") &&
      impl_->bind(impl_->get_name, "nvmlDeviceGetName") &&
      impl_->bind(impl_->get_uuid, "nvmlDeviceGetUUID") &&
      impl_->bind(impl_->get_utilization, "nvmlDeviceGetUtilizationRates") &&
      impl_->bind(impl_->get_memory_info, "nvmlDeviceGetMemoryInfo");

  if (!bound) {
    last_error_ = "libnvidia-ml loaded but required symbols are missing";
    dlclose(impl_->handle);
    impl_->handle = nullptr;
    return false;
  }

  // Optional: absent or failing on older drivers and inside some containers.
  // Their metrics are simply omitted rather than failing the collector.
  impl_->bind(impl_->get_temperature, "nvmlDeviceGetTemperature");
  impl_->bind(impl_->get_power_usage, "nvmlDeviceGetPowerUsage");
  impl_->bind(impl_->get_compute_processes, "nvmlDeviceGetComputeRunningProcesses_v3");

  if (impl_->init() != NVML_SUCCESS) {
    last_error_ = "nvmlInit failed; driver present but not usable";
    dlclose(impl_->handle);
    impl_->handle = nullptr;
    return false;
  }

  char driver[NVML_SYSTEM_DRIVER_VERSION_BUFFER_SIZE] = {0};
  if (impl_->get_driver_version(driver, sizeof(driver)) == NVML_SUCCESS) {
    driver_version_ = driver;
  }

  available_ = true;
  last_error_.clear();
  return true;
}

std::vector<DeviceSample> Library::sample() {
  std::vector<DeviceSample> samples;
  if (!available_) return samples;

  unsigned count = 0;
  if (impl_->get_device_count(&count) != NVML_SUCCESS) return samples;

  for (unsigned index = 0; index < count; ++index) {
    nvmlDevice_t device = nullptr;
    if (impl_->get_handle_by_index(index, &device) != NVML_SUCCESS) continue;

    DeviceSample sample;
    sample.index = index;

    char name[NVML_DEVICE_NAME_BUFFER_SIZE] = {0};
    if (impl_->get_name(device, name, sizeof(name)) == NVML_SUCCESS) sample.name = name;

    char uuid[NVML_DEVICE_UUID_BUFFER_SIZE] = {0};
    if (impl_->get_uuid(device, uuid, sizeof(uuid)) == NVML_SUCCESS) sample.uuid = uuid;

    nvmlUtilization_t utilization{};
    if (impl_->get_utilization(device, &utilization) == NVML_SUCCESS) {
      sample.utilization_percent = static_cast<double>(utilization.gpu);
    }

    nvmlMemory_t memory{};
    if (impl_->get_memory_info(device, &memory) == NVML_SUCCESS) {
      sample.memory_used_bytes = memory.used;
      sample.memory_total_bytes = memory.total;
    }

    if (impl_->get_temperature) {
      unsigned celsius = 0;
      if (impl_->get_temperature(device, NVML_TEMPERATURE_GPU, &celsius) == NVML_SUCCESS) {
        sample.temperature_celsius = static_cast<double>(celsius);
      }
    }

    if (impl_->get_power_usage) {
      unsigned milliwatts = 0;
      if (impl_->get_power_usage(device, &milliwatts) == NVML_SUCCESS) {
        sample.power_watts = static_cast<double>(milliwatts) / 1000.0;
      }
    }

    if (impl_->get_compute_processes) {
      nvmlProcessInfo_v3_t processes[MAX_PROCESSES];
      std::memset(processes, 0, sizeof(processes));
      unsigned process_count = MAX_PROCESSES;
      // Returns INSUFFICIENT_SIZE if more processes exist than the buffer
      // holds; the first MAX_PROCESSES are still useful for attribution.
      if (impl_->get_compute_processes(device, &process_count, processes) == NVML_SUCCESS) {
        if (process_count > MAX_PROCESSES) process_count = MAX_PROCESSES;
        for (unsigned i = 0; i < process_count; ++i) {
          ProcessUsage usage;
          usage.pid = processes[i].pid;
          usage.used_gpu_memory_bytes = processes[i].usedGpuMemory;
          sample.processes.push_back(usage);
        }
      }
    }

    samples.push_back(std::move(sample));
  }

  return samples;
}

}  // namespace pulseprobe::nvml
