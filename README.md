# PulseProbe

A C++17 node telemetry agent. It samples CPU, memory, load, cgroup v2 and
NVIDIA GPU metrics, and exposes them on a Prometheus-compatible endpoint. It
deploys as a Kubernetes DaemonSet so that every node in a cluster is covered by
exactly one agent.

```
 host sources                    agent                         consumers
 ─────────────                   ─────                         ─────────
 /proc/stat        ─┐
 /proc/meminfo      │
 /proc/loadavg      ├─► Collectors ─► Snapshot ─► Registry ─┬─► GET /metrics ─► Prometheus
 /proc/uptime       │   (sampling      (value)    (double   │
 cgroup v2 files    │    thread)                   buffer)  ├─► GET /readyz  ─► kubelet
 libnvidia-ml.so.1 ─┘                                       └─► GET /healthz ─► kubelet
        │                                                   
        └─ job mapping ─► per-job GPU attribution           threshold breaches ─► structured logs
```

## Quick start

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure

./build/pulseprobe --port=9100 --interval=5
curl -s localhost:9100/metrics
```

In Docker:

```bash
docker build -t pulseprobe:dev .
docker run --rm -p 9100:9100 pulseprobe:dev --interval=5
```

On a cluster:

```bash
kubectl create namespace monitoring
kubectl apply -f deploy/daemonset.yaml
kubectl apply -f deploy/servicemonitor.yaml   # Prometheus Operator only
```

procfs is Linux-only. On macOS, develop in a Linux container.

## Design decisions

### Scrapes never contend with sampling

The sampling thread owns the collectors and builds a complete `Snapshot` off to
the side. Publishing it is a single pointer assignment into
`MetricsRegistry`; a scrape takes a `shared_ptr` copy and renders from it. The
mutex covers the pointer swap and nothing else, so it is never held across file
I/O, text formatting, or a socket write.

The consequence that matters operationally: a slow or stalled HTTP client
cannot delay the next sample, and a sample landing mid-render cannot tear the
response. A reader that acquired generation N keeps reading a coherent
generation N even after 99 newer snapshots have been published and dropped.

This is *not* lock-free, and the code says so. It is a short critical section
around a pointer swap. `std::atomic<std::shared_ptr<T>>` would remove the mutex
outright but is C++20. `tests/test_registry.cpp` runs one writer against four
readers over 20,000 publications, checking redundantly-encoded fields for
tearing, and CI runs it under both ThreadSanitizer and AddressSanitizer.

### iowait counts as idle

CPU utilisation is `(Δtotal − Δidle) / Δtotal`, where idle is `idle + iowait`.
During iowait the CPU is blocked on I/O, not executing. Folding iowait into
busy time reports an I/O-bound host as CPU-saturated when it has headroom to
spare — a common enough mistake to be worth a dedicated test.

`iowait` is still exported separately, because "blocked on I/O" is the useful
signal in its own right.

### The first sample publishes no CPU number

Utilisation is a delta between two readings of a monotonic counter, so one
reading says nothing. Until the second cycle, `pulseprobe_cpu_usage_percent`
is **absent from the response** rather than present as `0`. A zero would put a
utilisation trough into the time series after every agent restart — an artefact
that looks exactly like a real event.

`/readyz` returns 503 for the same reason, which is what stops the kubelet from
routing scrapes to a pod with nothing worth reporting.

### Counters that go backwards are dropped

CPU hotplug and counter resets both make `/proc/stat` values decrease. Naive
unsigned subtraction turns that into an enormous positive delta and a
nonsensical percentage. Non-advancing counters produce no reading for that
cycle.

### Inside a container, procfs lies

`/proc/stat` and `/proc/meminfo` are not namespaced. A container reads the
host's CPUs and the host's memory, so anything alerting on container headroom
from procfs alone is alerting on the wrong machine. The cgroup v2 collector
reads `cpu.stat`, `cpu.max`, `memory.current` and `memory.max` to report the
numbers that actually bound the workload, and
`pulseprobe_cgroup_cpu_usage_percent` is scaled by the cgroup's own CPU
allowance — a container limited to 2 cores and using both reads 100%, not 200%.

There is a second trap underneath the first. The cgroup files at the hierarchy
*root* describe the whole machine. The agent resolves its own path from
`/proc/self/cgroup` (the `0::` line) and reads from there. With a private
cgroup namespace the two happen to coincide, which is precisely why reading the
root passes local testing and then silently reports node-level numbers as
container-level ones in production.

### GPU support loads at runtime, and the same image runs everywhere

NVML is opened with `dlopen("libnvidia-ml.so.1")` and bound through a function
pointer table, not linked at build time. There is no CUDA toolkit dependency
and no `nvml.h` include; the handful of ABI structs are redeclared locally.

One image therefore ships to every node in a mixed fleet. On a CPU node the
`dlopen` fails, the agent reports `pulseprobe_gpu_available 0`, and everything
else keeps working. Link-time coupling would mean either maintaining two images
or shipping a binary that refuses to start on most of the cluster.

Optional symbols (temperature, power, per-process compute) are bound
separately, because older drivers and some container configurations do not
provide them. Their absence drops those metrics rather than the collector.

### GPU metrics carry the owning job

Utilisation alone answers "is this GPU busy". On a shared cluster the question
people actually have is "which job is holding this GPU". So every GPU series
carries a `job_id` label, joined from NVML's per-process memory usage against a
scheduler job-mapping directory (one file per job, named for the job ID,
containing its PIDs — the convention HPC prologs already write):

```
pulseprobe_gpu_utilization_percent{gpu="0",uuid="GPU-aaaa",job_id="88412"} 94.2
pulseprobe_gpu_memory_used_bytes{gpu="0",uuid="GPU-aaaa",job_id="88412"} 21474836480
```

That label is what makes `GpuReservedButIdle` in `prometheus/alerts.yml`
expressible: a job holding a device the scheduler cannot reallocate while
running nothing on it. Devices with no mapping keep the label with an empty
value so the series shape stays stable whether or not a scheduler is present.

Attribution is best-effort by design. PIDs are reused, job files are written
while the agent reads them, and a device can be shared by two jobs (reported
against the larger memory consumer). A malformed line is skipped rather than
failing the whole map, and a missing mapping directory is not an error.

### Liveness and readiness check different things

`/healthz` deliberately does not depend on the sampler. A wedged collector
should not cause a restart loop that destroys the state needed to debug it.
`/readyz` is the one that depends on having a usable snapshot. The DaemonSet
also sets no CPU limit: throttling a monitoring agent under node pressure loses
data at exactly the moment it matters.

## Metrics

| Metric | Type | Notes |
| --- | --- | --- |
| `pulseprobe_build_info` | gauge | version label |
| `pulseprobe_ready` | gauge | 0 during the first sampling cycle |
| `pulseprobe_sampling_generation` | gauge | cycles completed since start |
| `pulseprobe_cpu_usage_percent` | gauge | absent until the second cycle |
| `pulseprobe_cpu_iowait_percent` | gauge | |
| `pulseprobe_cpu_online_count` | gauge | |
| `pulseprobe_memory_{total,available,used}_bytes` | gauge | used = total − available |
| `pulseprobe_memory_usage_percent` | gauge | |
| `pulseprobe_load_average` | gauge | `window="1m"\|"5m"\|"15m"` |
| `pulseprobe_uptime_seconds` | gauge | |
| `pulseprobe_cgroup_available` | gauge | 0 when there is no cgroup v2 hierarchy |
| `pulseprobe_cgroup_cpu_usage_percent` | gauge | share of the cgroup's own allowance |
| `pulseprobe_cgroup_cpu_limit_cores` | gauge | from `cpu.max`; absent when unlimited |
| `pulseprobe_cgroup_memory_{current,max}_bytes` | gauge | `max` absent when unlimited |
| `pulseprobe_cgroup_memory_usage_percent` | gauge | |
| `pulseprobe_gpu_available` | gauge | 0 when no driver is present |
| `pulseprobe_gpu_info` | gauge | `gpu`, `uuid`, `name`, `driver_version` |
| `pulseprobe_gpu_utilization_percent` | gauge | `gpu`, `uuid`, `job_id` |
| `pulseprobe_gpu_memory_{used,total}_bytes` | gauge | `gpu`, `uuid`, `job_id` |
| `pulseprobe_gpu_temperature_celsius` | gauge | `gpu`, `uuid`, `job_id` |
| `pulseprobe_gpu_power_watts` | gauge | `gpu`, `uuid`, `job_id` |

A `node` label is applied to every series when `--node-name` is set. The
DaemonSet fills it from the downward API (`spec.nodeName`).

## Configuration

| Flag | Environment variable | Default |
| --- | --- | --- |
| `--interval=<seconds>` | `PULSEPROBE_INTERVAL` | `5` |
| `--port=<port>` | `PULSEPROBE_PORT` | `9100` |
| `--host=<addr>` | `PULSEPROBE_HOST` | `0.0.0.0` |
| `--procfs-root=<path>` | `PULSEPROBE_PROCFS_ROOT` | `/proc` |
| `--cgroup-root=<path>` | `PULSEPROBE_CGROUP_ROOT` | `/sys/fs/cgroup` |
| `--job-map-dir=<path>` | `PULSEPROBE_JOB_MAP_DIR` | unset (attribution off) |
| `--node-name=<name>` | `PULSEPROBE_NODE_NAME` | unset |
| `--cpu-threshold=<pct>` | `PULSEPROBE_CPU_THRESHOLD` | `90` |
| `--memory-threshold=<pct>` | `PULSEPROBE_MEMORY_THRESHOLD` | `90` |
| `--gpu=<true\|false>` | `PULSEPROBE_GPU` | `true` |

Flags take precedence over the environment. Threshold breaches are emitted as
one structured line per event, parseable without a per-message regex:

```
WARN event=high_memory_usage scope=cgroup value=93.210000 threshold=90.000000
```

On a hybrid cgroup host, where v1 controllers are mounted alongside a v2
hierarchy at `/sys/fs/cgroup/unified`, point `--cgroup-root` at the v2 mount.

## Testing

| Suite | Covers |
| --- | --- |
| `test_procfs` | counter deltas, iowait handling, backwards counters, kernel-version column drift, `MemAvailable` vs `MemFree` |
| `test_cgroup` | `cpu.stat`, `cpu.max` quota maths, `max` sentinel, self-path resolution across private/shared/hybrid namespaces |
| `test_jobmap` | PID parsing, partial writes, missing directory |
| `test_exposition` | exposition format, label escaping, absent-vs-zero gauges, GPU job labels |
| `test_registry` | 1 writer × 4 readers × 20,000 publications, tearing and monotonicity |
| `integration_test.py` | the real binary against the real `/proc`: readiness gate, metric ranges, node labels on every sample, sampling progress across scrapes, shutdown on SIGTERM |

Parsers take file *contents* rather than paths, so they are driven from
fixtures in `tests/fixtures/` and need no Linux host.

CI builds with GCC and Clang, Debug and Release, warnings as errors; runs the
suite under ThreadSanitizer and AddressSanitizer; builds the image and scrapes a
running container; and deploys the DaemonSet to a kind cluster to confirm the
rollout passes its own readiness gate.

## Layout

```
include/pulseprobe/   public headers
src/                  collectors, registry, exposition, NVML loader, main
tests/                unit suites, fixtures, HTTP integration test
deploy/               DaemonSet, headless Service, ServiceMonitor
prometheus/           scrape config and alerting rules
third_party/          cpp-httplib (single header, vendored)
```

The HTTP server is `cpp-httplib`, vendored as a single header. The interesting
part of this project is Linux telemetry and concurrency, not reimplementing
HTTP, and vendoring keeps the build hermetic.
