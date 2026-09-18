#include "pulseprobe/registry.hpp"

#include <utility>

namespace pulseprobe {

MetricsRegistry::MetricsRegistry() : current_(std::make_shared<const Snapshot>()) {}

void MetricsRegistry::publish(Snapshot snapshot) {
  // The snapshot is fully built before the lock is taken. The critical section
  // is a single pointer assignment.
  auto next = std::make_shared<const Snapshot>(std::move(snapshot));
  std::lock_guard<std::mutex> guard(mutex_);
  current_ = std::move(next);
}

std::shared_ptr<const Snapshot> MetricsRegistry::acquire() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return current_;  // refcount bump; rendering happens outside the lock
}

}  // namespace pulseprobe
