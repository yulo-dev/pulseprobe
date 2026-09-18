// Double-buffered snapshot holder.
//
// The sampling thread builds a fresh Snapshot off to the side and swaps it in
// atomically; scrape threads take a shared_ptr copy and read from it for as
// long as they need. The mutex guards only the pointer assignment, so it is
// held for a handful of instructions and never across file I/O, formatting, or
// socket writes. A scrape can therefore never stall a sample, and a sample can
// never tear a scrape's view of the data.
//
// Note this is not lock-free; it is a short critical section around a pointer
// swap. The C++20 std::atomic<std::shared_ptr<T>> specialisation would remove
// the mutex outright, but it is not available in C++17.
#pragma once

#include <memory>
#include <mutex>

#include "pulseprobe/snapshot.hpp"

namespace pulseprobe {

class MetricsRegistry {
 public:
  MetricsRegistry();

  // Called by the sampling thread, once per cycle.
  void publish(Snapshot snapshot);

  // Called by scrape handlers. Never returns null.
  std::shared_ptr<const Snapshot> acquire() const;

 private:
  mutable std::mutex mutex_;
  std::shared_ptr<const Snapshot> current_;
};

}  // namespace pulseprobe
