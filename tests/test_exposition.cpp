#include "pulseprobe/exposition.hpp"

#include <string>

#include "testing.hpp"

using namespace pulseprobe;

namespace {

bool contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

Snapshot sample_snapshot() {
  Snapshot snapshot;
  snapshot.generation = 7;
  snapshot.ready = true;
  snapshot.cpu.usage_percent = 37.5;
  snapshot.cpu.iowait_percent = 2.25;
  snapshot.cpu.online_cpus = 8;
  snapshot.memory.total_bytes = 16000000000ULL;
  snapshot.memory.available_bytes = 8000000000ULL;
  snapshot.memory.used_bytes = 8000000000ULL;
  snapshot.memory.usage_percent = 50.0;
  snapshot.load.load1 = 1.5;
  return snapshot;
}

void test_basic_shape() {
  ExpositionOptions options;
  const std::string output = render_prometheus(sample_snapshot(), options);

  CHECK(contains(output, "# HELP pulseprobe_cpu_usage_percent"));
  CHECK(contains(output, "# TYPE pulseprobe_cpu_usage_percent gauge"));
  CHECK(contains(output, "\npulseprobe_cpu_usage_percent 37.5\n"));
  CHECK(contains(output, "\npulseprobe_memory_used_bytes 8000000000\n"));
  CHECK(contains(output, "pulseprobe_load_average{window=\"1m\"} 1.5"));
  CHECK(contains(output, "\npulseprobe_gpu_available 0\n"));
}

// A gauge with no value yet must be absent, not zero. Publishing 0 would make
// every agent restart look like an idle host in the time series.
void test_first_cycle_omits_unknown_cpu() {
  Snapshot snapshot;  // no usage_percent
  const std::string output = render_prometheus(snapshot, ExpositionOptions{});
  CHECK(!contains(output, "pulseprobe_cpu_usage_percent"));
  CHECK(contains(output, "\npulseprobe_ready 0\n"));
  CHECK(contains(output, "pulseprobe_cpu_online_count"));
}

void test_node_label_applied_to_every_series() {
  ExpositionOptions options;
  options.node_name = "gpu-node-04";
  const std::string output = render_prometheus(sample_snapshot(), options);

  CHECK(contains(output, "pulseprobe_cpu_usage_percent{node=\"gpu-node-04\"} 37.5"));
  CHECK(contains(output, "pulseprobe_load_average{node=\"gpu-node-04\",window=\"1m\"}"));
  CHECK(!contains(output, "\npulseprobe_cpu_usage_percent 37.5"));
}

void test_gpu_series_carry_job_label() {
  Snapshot snapshot = sample_snapshot();
  snapshot.gpu.available = true;
  snapshot.gpu.driver_version = "550.54.15";

  GpuDevice attributed;
  attributed.index = 0;
  attributed.uuid = "GPU-aaaa";
  attributed.name = "NVIDIA A100-SXM4-40GB";
  attributed.utilization_percent = 94.0;
  attributed.memory_used_bytes = 21474836480ULL;
  attributed.memory_total_bytes = 42949672960ULL;
  attributed.power_watts = 312.5;
  attributed.job_id = "88412";
  snapshot.gpu.devices.push_back(attributed);

  GpuDevice idle;
  idle.index = 1;
  idle.uuid = "GPU-bbbb";
  idle.name = "NVIDIA A100-SXM4-40GB";
  snapshot.gpu.devices.push_back(idle);

  const std::string output = render_prometheus(snapshot, ExpositionOptions{});

  CHECK(contains(output, "\npulseprobe_gpu_available 1\n"));
  CHECK(contains(
      output, "pulseprobe_gpu_utilization_percent{gpu=\"0\",uuid=\"GPU-aaaa\",job_id=\"88412\"} 94"));
  CHECK(contains(output,
                 "pulseprobe_gpu_memory_used_bytes{gpu=\"0\",uuid=\"GPU-aaaa\",job_id=\"88412\"} "
                 "21474836480"));
  // An unattributed device keeps the label with an empty value so the series
  // has the same shape whether or not a scheduler mapping exists.
  CHECK(contains(output, "pulseprobe_gpu_utilization_percent{gpu=\"1\",uuid=\"GPU-bbbb\",job_id=\"\"}"));
  CHECK(contains(output, "driver_version=\"550.54.15\""));
}

void test_cgroup_series_only_when_available() {
  Snapshot snapshot = sample_snapshot();
  const std::string without = render_prometheus(snapshot, ExpositionOptions{});
  CHECK(contains(without, "\npulseprobe_cgroup_available 0\n"));
  CHECK(!contains(without, "pulseprobe_cgroup_memory_current_bytes"));

  snapshot.cgroup.available = true;
  snapshot.cgroup.cpu_usage_percent = 61.0;
  snapshot.cgroup.cpu_limit_cores = 2.0;
  snapshot.cgroup.memory_current_bytes = 900000000ULL;
  snapshot.cgroup.memory_max_bytes = 1073741824ULL;
  snapshot.cgroup.memory_usage_percent = 83.8;

  const std::string with = render_prometheus(snapshot, ExpositionOptions{});
  CHECK(contains(with, "\npulseprobe_cgroup_available 1\n"));
  CHECK(contains(with, "\npulseprobe_cgroup_cpu_usage_percent 61\n"));
  CHECK(contains(with, "\npulseprobe_cgroup_memory_max_bytes 1073741824\n"));
}

// Device names arrive from the driver and go straight into a label value, so
// the exposition escaping has to hold up against whatever is in them.
void test_label_escaping() {
  CHECK(escape_label_value("plain") == "plain");
  CHECK(escape_label_value("a\"b") == "a\\\"b");
  CHECK(escape_label_value("a\\b") == "a\\\\b");
  CHECK(escape_label_value("a\nb") == "a\\nb");

  ExpositionOptions options;
  options.node_name = "node\"x";
  const std::string output = render_prometheus(sample_snapshot(), options);
  CHECK(contains(output, "node=\"node\\\"x\""));
}

}  // namespace

int main() {
  test_basic_shape();
  test_first_cycle_omits_unknown_cpu();
  test_node_label_applied_to_every_series();
  test_gpu_series_carry_job_label();
  test_cgroup_series_only_when_available();
  test_label_escaping();
  return testing::summarize("exposition");
}
