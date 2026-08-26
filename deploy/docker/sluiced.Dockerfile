# Production image for sluiced, the C++ data plane. Built WITH gRPC so it can
# receive the backend set from sluice-controller's ConfigStream and route to
# echod pod IPs directly (the kube-proxy-bypass thesis). Unlike the Go
# services, sluiced targets Linux/epoll, so it only ever runs in a container.
#
# Build from the repo root (proto/ and sluiced/ must both be in context):
#   docker build -f deploy/docker/sluiced.Dockerfile -t sluice/sluiced:dev .

# --- build stage: full toolchain + gRPC C++ ---------------------------------
FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake git ca-certificates \
    protobuf-compiler protobuf-compiler-grpc libprotobuf-dev libgrpc++-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY proto ./proto
COPY sluiced ./sluiced
# SLUICE_BUILD_TESTS=OFF keeps the image build hermetic (no Catch2 fetch). With
# gRPC present, CMake generates the proto stubs and defines SLUICE_HAVE_GRPC.
RUN cmake -S sluiced -B /build -DCMAKE_BUILD_TYPE=Release -DSLUICE_BUILD_TESTS=OFF \
    && cmake --build /build --target sluiced -j"$(nproc)"

# --- runtime stage: just the shared libs the binary needs -------------------
FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
    libgrpc++1.51 ca-certificates \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /build/sluiced /usr/local/bin/sluiced
# Backends arrive from the controller over gRPC; no bootstrap file is baked in
# (pod IPs aren't known at build time). sluiced starts with an empty set and
# fills it within a second of the controller connecting.
ENTRYPOINT ["/usr/local/bin/sluiced"]
