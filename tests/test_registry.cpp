// Concurrency test for the double-buffered registry.
//
// One writer publishes continuously while several readers scrape. Each
// snapshot is internally consistent by construction (generation N carries
// exactly N in every field that mirrors it), so a torn read shows up as a
// mismatch. Readers also hold their snapshot across a delay to prove that a
// publication during a scrape cannot pull data out from under the reader.
#include "pulseprobe/registry.hpp"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "testing.hpp"

using namespace pulseprobe;

namespace {

constexpr int kPublications = 20000;
constexpr int kReaders = 4;

Snapshot make_snapshot(uint64_t generation) {
  Snapshot snapshot;
  snapshot.generation = generation;
  snapshot.ready = true;
  // Redundant encodings of the same generation. If a reader ever observes a
  // snapshot whose fields disagree, the publication was not atomic.
  snapshot.unix_millis = static_cast<int64_t>(generation);
  snapshot.cpu.usage_percent = static_cast<double>(generation);
  snapshot.memory.total_bytes = generation;

  GpuDevice device;
  device.index = static_cast<unsigned>(generation % 8);
  device.memory_used_bytes = generation;
  device.job_id = std::to_string(generation);
  snapshot.gpu.devices.push_back(device);
  snapshot.gpu.available = true;
  return snapshot;
}

void test_initial_snapshot_is_never_null() {
  MetricsRegistry registry;
  const auto snapshot = registry.acquire();
  CHECK(snapshot != nullptr);
  if (snapshot) {
    CHECK(snapshot->generation == 0);
    CHECK(!snapshot->ready);
  }
}

void test_concurrent_publish_and_acquire() {
  MetricsRegistry registry;
  std::atomic<bool> stop{false};
  std::atomic<int> torn{0};
  std::atomic<long long> observed{0};
  std::atomic<uint64_t> highest_seen{0};

  std::thread writer([&] {
    for (uint64_t generation = 1; generation <= kPublications; ++generation) {
      registry.publish(make_snapshot(generation));
    }
    stop.store(true);
  });

  std::vector<std::thread> readers;
  for (int i = 0; i < kReaders; ++i) {
    readers.emplace_back([&] {
      uint64_t local_highest = 0;
      while (!stop.load()) {
        const auto snapshot = registry.acquire();
        if (!snapshot) { ++torn; continue; }

        const uint64_t generation = snapshot->generation;
        if (generation == 0) continue;  // pre-first-publish snapshot

        // Hold the snapshot while the writer keeps going, then re-read it.
        // A reader that lost its data to a concurrent publish fails here.
        std::this_thread::yield();

        const bool consistent =
            snapshot->unix_millis == static_cast<int64_t>(generation) &&
            snapshot->cpu.usage_percent.has_value() &&
            *snapshot->cpu.usage_percent == static_cast<double>(generation) &&
            snapshot->memory.total_bytes == generation &&
            snapshot->gpu.devices.size() == 1 &&
            snapshot->gpu.devices[0].memory_used_bytes == generation &&
            snapshot->gpu.devices[0].job_id == std::to_string(generation) &&
            snapshot->generation == generation;

        if (!consistent) ++torn;

        // Snapshots must never go backwards for a given reader.
        if (generation < local_highest) ++torn;
        local_highest = generation;
        ++observed;
      }
      uint64_t previous = highest_seen.load();
      while (local_highest > previous && !highest_seen.compare_exchange_weak(previous, local_highest)) {
      }
    });
  }

  writer.join();
  for (auto& reader : readers) reader.join();

  CHECK(torn.load() == 0);
  // Guard against the test silently passing because the readers never ran.
  CHECK(observed.load() > 0);
  CHECK(highest_seen.load() > 0);
}

void test_reader_keeps_snapshot_alive_after_republish() {
  MetricsRegistry registry;
  registry.publish(make_snapshot(1));

  const auto held = registry.acquire();
  for (uint64_t generation = 2; generation <= 100; ++generation) {
    registry.publish(make_snapshot(generation));
  }

  // The held snapshot still reads as generation 1 even though 99 newer ones
  // have been published and dropped in the meantime.
  CHECK(held->generation == 1);
  CHECK(held->memory.total_bytes == 1);
  CHECK(registry.acquire()->generation == 100);
}

}  // namespace

int main() {
  test_initial_snapshot_is_never_null();
  test_concurrent_publish_and_acquire();
  test_reader_keeps_snapshot_alive_after_republish();
  return testing::summarize("registry");
}
