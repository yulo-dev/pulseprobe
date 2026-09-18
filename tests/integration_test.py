#!/usr/bin/env python3
"""End-to-end test: start the agent, wait for readiness, scrape it.

The unit tests drive the parsers from fixtures. This one runs the real binary
against the real /proc so that the wiring is covered too: config parsing,
collector registration, the sampling thread, the HTTP surface, and the shape of
what Prometheus would actually scrape.
"""

import re
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request

READY_TIMEOUT_SECONDS = 30
SAMPLING_INTERVAL_SECONDS = 1
NODE_NAME = "integration-test-node"

failures = []


def check(condition, message):
    if not condition:
        failures.append(message)


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def get(url):
    try:
        with urllib.request.urlopen(url, timeout=5) as response:
            return response.status, response.read().decode()
    except urllib.error.HTTPError as error:
        return error.code, error.read().decode()


def wait_for_ready(base, process):
    deadline = time.time() + READY_TIMEOUT_SECONDS
    while time.time() < deadline:
        if process.poll() is not None:
            raise SystemExit(f"agent exited early with code {process.returncode}")
        try:
            status, _ = get(f"{base}/readyz")
            if status == 200:
                return True
        except urllib.error.URLError:
            pass
        time.sleep(0.2)
    return False


def metric_value(body, name, labels=""):
    """Returns the float value of a single sample, or None."""
    pattern = rf"^{re.escape(name)}{re.escape(labels)}\s+(\S+)$"
    match = re.search(pattern, body, re.MULTILINE)
    return float(match.group(1)) if match else None


def main():
    if len(sys.argv) < 2:
        raise SystemExit("usage: integration_test.py <path-to-pulseprobe>")

    binary = sys.argv[1]
    port = free_port()
    base = f"http://127.0.0.1:{port}"

    process = subprocess.Popen(
        [
            binary,
            f"--port={port}",
            "--host=127.0.0.1",
            f"--interval={SAMPLING_INTERVAL_SECONDS}",
            f"--node-name={NODE_NAME}",
            # Thresholds are pushed out of the way so the run does not emit
            # warnings just because CI is busy.
            "--cpu-threshold=100",
            "--memory-threshold=100",
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )

    try:
        # Liveness must answer before the first sample completes: it reports
        # that the process is serving, not that data is ready.
        deadline = time.time() + 10
        healthy = False
        while time.time() < deadline and not healthy:
            if process.poll() is not None:
                raise SystemExit(f"agent exited early with code {process.returncode}")
            try:
                status, _ = get(f"{base}/healthz")
                healthy = status == 200
            except urllib.error.URLError:
                time.sleep(0.1)
        check(healthy, "/healthz did not become available")

        # Readiness takes two sampling cycles, because CPU utilisation is a
        # delta and one reading is not enough to compute it.
        check(wait_for_ready(base, process), "/readyz never reported ready")

        status, body = get(f"{base}/metrics")
        check(status == 200, f"/metrics returned {status}")

        for name in [
            "pulseprobe_build_info",
            "pulseprobe_ready",
            "pulseprobe_cpu_usage_percent",
            "pulseprobe_cpu_online_count",
            "pulseprobe_memory_total_bytes",
            "pulseprobe_memory_usage_percent",
            "pulseprobe_load_average",
            "pulseprobe_uptime_seconds",
            "pulseprobe_cgroup_available",
            "pulseprobe_gpu_available",
        ]:
            check(f"# TYPE {name} " in body, f"missing TYPE line for {name}")

        labels = f'{{node="{NODE_NAME}"}}'
        cpu = metric_value(body, "pulseprobe_cpu_usage_percent", labels)
        check(cpu is not None, "pulseprobe_cpu_usage_percent missing the node label")
        if cpu is not None:
            check(0.0 <= cpu <= 100.0, f"CPU utilisation out of range: {cpu}")

        memory_percent = metric_value(body, "pulseprobe_memory_usage_percent", labels)
        check(memory_percent is not None, "pulseprobe_memory_usage_percent missing")
        if memory_percent is not None:
            check(0.0 < memory_percent < 100.0, f"memory usage out of range: {memory_percent}")

        total = metric_value(body, "pulseprobe_memory_total_bytes", labels)
        check(total is not None and total > 0, "pulseprobe_memory_total_bytes should be positive")

        cpus = metric_value(body, "pulseprobe_cpu_online_count", labels)
        check(cpus is not None and cpus >= 1, "pulseprobe_cpu_online_count should be at least 1")

        # Every sample line must carry the node label, or a multi-node scrape
        # cannot tell two agents apart.
        for line in body.splitlines():
            if not line or line.startswith("#"):
                continue
            check(f'node="{NODE_NAME}"' in line, f"sample without node label: {line}")

        # Consecutive scrapes must advance the generation counter, proving the
        # sampling thread is still running behind the HTTP surface.
        first = metric_value(body, "pulseprobe_sampling_generation", labels)
        time.sleep(SAMPLING_INTERVAL_SECONDS * 2.5)
        _, body2 = get(f"{base}/metrics")
        second = metric_value(body2, "pulseprobe_sampling_generation", labels)
        check(
            first is not None and second is not None and second > first,
            f"sampling generation did not advance: {first} -> {second}",
        )

        status, _ = get(f"{base}/nope")
        check(status == 404, f"unknown path returned {status}, want 404")

    finally:
        process.terminate()
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            failures.append("agent did not exit within 10s of SIGTERM")

    if failures:
        for failure in failures:
            print(f"FAIL {failure}", file=sys.stderr)
        return 1

    print("integration: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
