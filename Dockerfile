# Build stage -----------------------------------------------------------------
FROM debian:bookworm-slim AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt ./
COPY include/ ./include/
COPY src/ ./src/
COPY tests/ ./tests/
COPY third_party/ ./third_party/

RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DPULSEPROBE_WERROR=ON \
    && cmake --build build -j"$(nproc)"

# The unit tests run inside the image build, so an image can never be published
# from a tree whose parsers are broken. The HTTP integration test is left to CI,
# where it can bind a port.
RUN ctest --test-dir build --output-on-failure -E integration

# Runtime stage ---------------------------------------------------------------
FROM debian:bookworm-slim

# No CUDA toolkit and no NVIDIA packages. libnvidia-ml.so.1 is injected by the
# NVIDIA container runtime on GPU nodes and dlopen'd at startup; on every other
# node the agent runs unchanged and reports pulseprobe_gpu_available 0.
RUN useradd --system --uid 65532 --no-create-home pulseprobe

COPY --from=build /src/build/pulseprobe /usr/local/bin/pulseprobe

USER 65532:65532
EXPOSE 9100

ENTRYPOINT ["/usr/local/bin/pulseprobe"]
