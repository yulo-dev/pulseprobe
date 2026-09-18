#include "pulseprobe/cgroup.hpp"

#include "pulseprobe/util.hpp"
#include "testing.hpp"

using namespace pulseprobe;

namespace {

std::string fixture(const std::string& name) {
  const auto content = util::read_file(std::string(PULSEPROBE_FIXTURE_DIR) + "/" + name);
  CHECK(content.has_value());
  return content.value_or("");
}

void test_parse_cpu_stat() {
  const auto stat = cgroup::parse_cpu_stat(fixture("cgroup_cpu.stat"));
  CHECK(stat.has_value());
  if (!stat) return;
  CHECK(stat->usage_usec == 4210000);
  CHECK(stat->user_usec == 3100000);
  CHECK(stat->system_usec == 1110000);
}

// cpu.stat gained throttling fields over time and their presence varies by
// kernel; only usage_usec is required.
void test_parse_cpu_stat_requires_usage_only() {
  CHECK(cgroup::parse_cpu_stat("usage_usec 42\n").has_value());
  CHECK(!cgroup::parse_cpu_stat("user_usec 42\nsystem_usec 1\n").has_value());
  CHECK(!cgroup::parse_cpu_stat("").has_value());
}

void test_parse_scalar() {
  CHECK(cgroup::parse_scalar("2147483648\n").value_or(0) == 2147483648ULL);
  // "max" means unlimited, which is a different thing from "unreadable" but
  // shares the empty-optional representation; callers treat both as no limit.
  CHECK(!cgroup::parse_scalar("max\n").has_value());
  CHECK(!cgroup::parse_scalar("   \n").has_value());
  CHECK(!cgroup::parse_scalar("not-a-number\n").has_value());
}

void test_parse_cpu_max() {
  const auto quota = cgroup::parse_cpu_max("200000 100000\n");
  CHECK(quota.has_value());
  if (quota) {
    CHECK(quota->cores.has_value());
    if (quota->cores) CHECK_NEAR(*quota->cores, 2.0, 1e-9);
  }

  const auto unlimited = cgroup::parse_cpu_max("max 100000\n");
  CHECK(unlimited.has_value());
  if (unlimited) CHECK(!unlimited->cores.has_value());

  const auto fractional = cgroup::parse_cpu_max("50000 100000\n");
  CHECK(fractional.has_value());
  if (fractional && fractional->cores) CHECK_NEAR(*fractional->cores, 0.5, 1e-9);

  CHECK(!cgroup::parse_cpu_max("200000\n").has_value());
  CHECK(!cgroup::parse_cpu_max("200000 0\n").has_value());
  CHECK(!cgroup::parse_cpu_max("").has_value());
}

void test_parse_self_cgroup_path() {
  // Private cgroup namespace: the container is its own root.
  const auto container = cgroup::parse_self_cgroup_path("0::/\n");
  CHECK(container.value_or("") == "/");

  // Shared namespace: the v2 line carries the full delegated path, and reading
  // the hierarchy root here would report the node instead of the pod.
  const std::string kubernetes =
      "0::/kubepods.slice/kubepods-besteffort.slice/"
      "kubepods-besteffort-pod9f2a.slice/cri-containerd-7c1d.scope\n";
  const auto pod = cgroup::parse_self_cgroup_path(kubernetes);
  CHECK(pod.has_value());
  if (pod) CHECK(pod->rfind("/kubepods.slice/", 0) == 0);

  // Hybrid hierarchy: v1 controller lines are present alongside the v2 line
  // and must not be mistaken for it.
  const std::string hybrid =
      "9:name=systemd:/user.slice\n"
      "4:memory:/some/v1/path\n"
      "1:cpu:/\n"
      "0::/user.slice/user-1000.slice\n";
  CHECK(cgroup::parse_self_cgroup_path(hybrid).value_or("") == "/user.slice/user-1000.slice");

  // cgroup v1 only: no v2 line at all.
  CHECK(!cgroup::parse_self_cgroup_path("4:memory:/docker/abc\n1:cpu:/\n").has_value());
  CHECK(!cgroup::parse_self_cgroup_path("").has_value());
  CHECK(!cgroup::parse_self_cgroup_path("0::\n").has_value());
}

}  // namespace

int main() {
  test_parse_cpu_stat();
  test_parse_cpu_stat_requires_usage_only();
  test_parse_scalar();
  test_parse_cpu_max();
  test_parse_self_cgroup_path();
  return testing::summarize("cgroup");
}
