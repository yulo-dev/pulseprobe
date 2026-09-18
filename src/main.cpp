#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "httplib.h"
#include "pulseprobe/collector.hpp"
#include "pulseprobe/config.hpp"
#include "pulseprobe/exposition.hpp"
#include "pulseprobe/registry.hpp"
#include "pulseprobe/util.hpp"

namespace {

std::atomic<bool> g_stop{false};
std::condition_variable g_wakeup;
std::mutex g_wakeup_mutex;

void handle_signal(int) {
  g_stop.store(true, std::memory_order_relaxed);
  g_wakeup.notify_all();
}

// One line per event, key=value, no interpolated prose. This is the shape a
// log pipeline can parse without a per-message regex.
void log_event(const std::string& level, const std::string& event, const std::string& fields) {
  std::cerr << level << " event=" << event;
  if (!fields.empty()) std::cerr << ' ' << fields;
  std::cerr << '\n';
}

void check_thresholds(const pulseprobe::Snapshot& snapshot, const pulseprobe::Config& config) {
  if (snapshot.cpu.usage_percent && *snapshot.cpu.usage_percent > config.cpu_threshold_percent) {
    log_event("WARN", "high_cpu_usage",
              "value=" + std::to_string(*snapshot.cpu.usage_percent) +
                  " threshold=" + std::to_string(config.cpu_threshold_percent));
  }

  // Prefer the cgroup number when there is one: inside a container the host
  // percentage says nothing about how close this workload is to its limit.
  const double memory_percent = snapshot.cgroup.memory_usage_percent.value_or(
      snapshot.memory.usage_percent);
  const char* scope = snapshot.cgroup.memory_usage_percent ? "cgroup" : "host";
  if (memory_percent > config.memory_threshold_percent) {
    log_event("WARN", "high_memory_usage",
              std::string("scope=") + scope + " value=" + std::to_string(memory_percent) +
                  " threshold=" + std::to_string(config.memory_threshold_percent));
  }
}

}  // namespace

int main(int argc, char** argv) {
  pulseprobe::Config config;
  if (!pulseprobe::Config::parse(argc, argv, &config)) return 2;

  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);
  // A scrape client that disconnects mid-write would otherwise kill the
  // process with SIGPIPE.
  std::signal(SIGPIPE, SIG_IGN);

  pulseprobe::MetricsRegistry registry;

  std::vector<std::unique_ptr<pulseprobe::Collector>> collectors;
  collectors.push_back(pulseprobe::make_cpu_collector(config));
  collectors.push_back(pulseprobe::make_memory_collector(config));
  collectors.push_back(pulseprobe::make_load_collector(config));
  collectors.push_back(pulseprobe::make_cgroup_collector(config));
  if (auto gpu = pulseprobe::make_gpu_collector(config)) collectors.push_back(std::move(gpu));

  log_event("INFO", "startup",
            "port=" + std::to_string(config.listen_port) +
                " interval_seconds=" + std::to_string(config.sampling_interval_seconds) +
                " collectors=" + std::to_string(collectors.size()) +
                " node=" + (config.node_name.empty() ? "<unset>" : config.node_name));

  // Sampling thread. It is the only thread that touches the collectors, so
  // collector state needs no locking; it hands finished snapshots to the
  // registry, which is the single synchronisation point in the process.
  std::thread sampler([&] {
    uint64_t generation = 0;
    while (!g_stop.load(std::memory_order_relaxed)) {
      pulseprobe::Snapshot snapshot;
      snapshot.unix_millis = pulseprobe::util::now_unix_millis();
      snapshot.generation = ++generation;

      for (auto& collector : collectors) collector->collect(&snapshot);

      // Readiness is defined by having a real CPU delta, which by
      // construction takes two cycles. Until then the agent is up but has
      // nothing meaningful to serve, and /readyz says so.
      snapshot.ready = snapshot.cpu.usage_percent.has_value();

      check_thresholds(snapshot, config);
      registry.publish(std::move(snapshot));

      std::unique_lock<std::mutex> lock(g_wakeup_mutex);
      g_wakeup.wait_for(lock, std::chrono::seconds(config.sampling_interval_seconds),
                        [] { return g_stop.load(std::memory_order_relaxed); });
    }
  });

  pulseprobe::ExpositionOptions exposition_options;
  exposition_options.node_name = config.node_name;

  httplib::Server server;

  server.Get("/metrics", [&](const httplib::Request&, httplib::Response& response) {
    // acquire() returns a snapshot that stays valid for this handler's
    // lifetime even if the sampler publishes a new one mid-render.
    const auto snapshot = registry.acquire();
    response.set_content(pulseprobe::render_prometheus(*snapshot, exposition_options),
                         "text/plain; version=0.0.4; charset=utf-8");
  });

  // Liveness: the process is running and can serve. Deliberately does not
  // depend on the sampler, so a stalled collector does not cause a restart
  // loop that would lose the very state needed to debug it.
  server.Get("/healthz", [](const httplib::Request&, httplib::Response& response) {
    response.set_content("ok\n", "text/plain");
  });

  // Readiness: there is a usable snapshot to scrape.
  server.Get("/readyz", [&](const httplib::Request&, httplib::Response& response) {
    const auto snapshot = registry.acquire();
    if (snapshot->ready) {
      response.set_content("ready\n", "text/plain");
    } else {
      response.status = 503;
      response.set_content("warming up: awaiting second CPU sample\n", "text/plain");
    }
  });

  server.Get("/", [](const httplib::Request&, httplib::Response& response) {
    response.set_content(
        "pulseprobe\n\n/metrics  Prometheus exposition\n/healthz  liveness\n/readyz   readiness\n",
        "text/plain");
  });

  std::thread shutdown_watcher([&] {
    std::unique_lock<std::mutex> lock(g_wakeup_mutex);
    g_wakeup.wait(lock, [] { return g_stop.load(std::memory_order_relaxed); });
    server.stop();
  });

  if (!server.listen(config.listen_host.c_str(), config.listen_port)) {
    log_event("ERROR", "listen_failed",
              "host=" + config.listen_host + " port=" + std::to_string(config.listen_port));
    g_stop.store(true, std::memory_order_relaxed);
    g_wakeup.notify_all();
    sampler.join();
    shutdown_watcher.join();
    return 1;
  }

  g_stop.store(true, std::memory_order_relaxed);
  g_wakeup.notify_all();
  sampler.join();
  shutdown_watcher.join();
  log_event("INFO", "shutdown", "");
  return 0;
}
