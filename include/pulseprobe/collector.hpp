#pragma once

#include <memory>
#include <string>

#include "pulseprobe/config.hpp"
#include "pulseprobe/snapshot.hpp"

namespace pulseprobe {

// A collector fills in its own slice of the snapshot. Collectors are only ever
// touched by the sampling thread, so they may hold mutable state (previous
// counter readings) without synchronisation.
class Collector {
 public:
  virtual ~Collector() = default;
  virtual const char* name() const = 0;

  // A collector that cannot read its source leaves the snapshot's
  // corresponding fields at their defaults rather than throwing. One missing
  // source must not take the whole agent down.
  virtual void collect(Snapshot* out) = 0;
};

std::unique_ptr<Collector> make_cpu_collector(const Config& config);
std::unique_ptr<Collector> make_memory_collector(const Config& config);
std::unique_ptr<Collector> make_load_collector(const Config& config);
std::unique_ptr<Collector> make_cgroup_collector(const Config& config);

// Returns nullptr when GPU collection is disabled in config. When enabled but
// the driver is absent, returns a collector that reports gpu.available = 0.
std::unique_ptr<Collector> make_gpu_collector(const Config& config);

}  // namespace pulseprobe
