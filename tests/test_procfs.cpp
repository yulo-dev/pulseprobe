#include "pulseprobe/procfs.hpp"

#include <string>

#include "pulseprobe/util.hpp"
#include "testing.hpp"

using namespace pulseprobe;

namespace {

std::string fixture(const std::string& name) {
  const auto content = util::read_file(std::string(PULSEPROBE_FIXTURE_DIR) + "/" + name);
  CHECK(content.has_value());
  return content.value_or("");
}

void test_parse_stat_fixture() {
  const auto times = procfs::parse_stat(fixture("stat.txt"));
  CHECK(times.has_value());
  if (!times) return;
  CHECK(times->user == 120000);
  CHECK(times->nice == 500);
  CHECK(times->system == 40000);
  CHECK(times->idle == 800000);
  CHECK(times->iowait == 12000);
  CHECK(times->steal == 30);
  CHECK(times->total() == 120000 + 500 + 40000 + 800000 + 12000 + 900 + 1500 + 30);
  CHECK(times->idle_all() == 800000 + 12000);
}

// Kernels append columns over time (guest, guest_nice, and more since). A
// parser that requires exactly eight breaks on a newer kernel, which is a
// silent failure in production.
void test_parse_stat_tolerates_extra_columns() {
  const auto base = procfs::parse_stat("cpu 1 2 3 4 5 6 7 8\n");
  const auto extended = procfs::parse_stat("cpu 1 2 3 4 5 6 7 8 9 10 11\n");
  CHECK(base.has_value());
  CHECK(extended.has_value());
  if (base && extended) CHECK(base->total() == extended->total());
}

void test_parse_stat_rejects_truncated_line() {
  CHECK(!procfs::parse_stat("cpu 1 2 3\n").has_value());
  CHECK(!procfs::parse_stat("cpu\n").has_value());
  CHECK(!procfs::parse_stat("").has_value());
  CHECK(!procfs::parse_stat("cpu 1 2 3 4 five 6 7 8\n").has_value());
}

// The aggregate line must win over the per-core lines that follow it.
void test_parse_stat_picks_aggregate_line() {
  const std::string content =
      "cpu  10 0 0 90 0 0 0 0\n"
      "cpu0 5 0 0 45 0 0 0 0\n"
      "cpu1 5 0 0 45 0 0 0 0\n";
  const auto times = procfs::parse_stat(content);
  CHECK(times.has_value());
  if (times) CHECK(times->total() == 100);
  CHECK(procfs::count_online_cpus(content) == 2);
  CHECK(procfs::count_online_cpus(fixture("stat.txt")) == 4);
}

void test_utilization_basic() {
  procfs::CpuTimes prev;
  prev.idle = 100;
  prev.user = 100;  // total 200

  procfs::CpuTimes cur;
  cur.idle = 150;
  cur.user = 250;  // total 400, delta 200, delta_idle 50

  const auto usage = procfs::compute_utilization(prev, cur);
  CHECK(usage.has_value());
  if (usage) CHECK_NEAR(usage->busy_percent, 75.0, 1e-9);
}

// iowait is idle time: the CPU is blocked, not working. Counting it as busy
// reports an I/O-bound host as saturated when it has CPU headroom to spare.
void test_utilization_counts_iowait_as_idle() {
  procfs::CpuTimes prev;
  procfs::CpuTimes cur;
  cur.iowait = 100;  // the entire interval was spent waiting on I/O

  const auto usage = procfs::compute_utilization(prev, cur);
  CHECK(usage.has_value());
  if (!usage) return;
  CHECK_NEAR(usage->busy_percent, 0.0, 1e-9);
  CHECK_NEAR(usage->iowait_percent, 100.0, 1e-9);
}

void test_utilization_rejects_non_advancing_counters() {
  procfs::CpuTimes prev;
  prev.user = 100;
  CHECK(!procfs::compute_utilization(prev, prev).has_value());

  // Counters going backwards (hotplug, counter reset) must not produce a
  // nonsense reading from an unsigned underflow.
  procfs::CpuTimes lower;
  lower.user = 10;
  CHECK(!procfs::compute_utilization(prev, lower).has_value());
}

void test_parse_meminfo() {
  const auto memory = procfs::parse_meminfo(fixture("meminfo.txt"));
  CHECK(memory.has_value());
  if (!memory) return;
  CHECK(memory->total_bytes == 16324476ULL * 1024ULL);
  CHECK(memory->available_bytes == 9873220ULL * 1024ULL);
  CHECK(memory->used_bytes == (16324476ULL - 9873220ULL) * 1024ULL);
  CHECK_NEAR(memory->usage_percent, 39.51, 0.01);
}

// MemFree excludes reclaimable page cache, so a MemFree-based "used" reading
// shows a healthy host at 95% memory. MemAvailable is the correct basis.
void test_parse_meminfo_uses_available_not_free() {
  const std::string content =
      "MemTotal:       1000 kB\n"
      "MemFree:          50 kB\n"
      "MemAvailable:    800 kB\n";
  const auto memory = procfs::parse_meminfo(content);
  CHECK(memory.has_value());
  if (memory) CHECK_NEAR(memory->usage_percent, 20.0, 1e-9);
}

void test_parse_meminfo_requires_fields() {
  CHECK(!procfs::parse_meminfo("MemTotal: 1000 kB\n").has_value());
  CHECK(!procfs::parse_meminfo("MemAvailable: 800 kB\n").has_value());
  CHECK(!procfs::parse_meminfo("MemTotal: 0 kB\nMemAvailable: 0 kB\n").has_value());
}

void test_parse_loadavg_and_uptime() {
  const auto load = procfs::parse_loadavg(fixture("loadavg.txt"));
  CHECK(load.has_value());
  if (load) {
    CHECK_NEAR(load->load1, 1.32, 1e-9);
    CHECK_NEAR(load->load5, 0.94, 1e-9);
    CHECK_NEAR(load->load15, 0.71, 1e-9);
  }

  const auto uptime = procfs::parse_uptime(fixture("uptime.txt"));
  CHECK(uptime.has_value());
  if (uptime) CHECK_NEAR(*uptime, 92837.41, 1e-9);

  CHECK(!procfs::parse_loadavg("1.0 2.0\n").has_value());
  CHECK(!procfs::parse_uptime("\n").has_value());
}

}  // namespace

int main() {
  test_parse_stat_fixture();
  test_parse_stat_tolerates_extra_columns();
  test_parse_stat_rejects_truncated_line();
  test_parse_stat_picks_aggregate_line();
  test_utilization_basic();
  test_utilization_counts_iowait_as_idle();
  test_utilization_rejects_non_advancing_counters();
  test_parse_meminfo();
  test_parse_meminfo_uses_available_not_free();
  test_parse_meminfo_requires_fields();
  test_parse_loadavg_and_uptime();
  return testing::summarize("procfs");
}
