.PHONY: build build-go build-cpp test test-go test-cpp cpp-build-image e2e bench leakcheck cluster \
	bench-go-bins bench-prereqs \
	cluster-up cluster-build cluster-load cluster-apply cluster-verify cluster-down

KIND_CLUSTER   ?= sluice
NAMESPACE      ?= sluice-demo
IMG_TAG        ?= dev
CPP_BUILD_IMAGE ?= sluice/cpp-build:dev

build: build-go build-cpp

build-go:
	go build ./...

# sluiced targets Linux's epoll (see sluiced/event_loop.hpp) and ships in
# Linux containers anyway (see deploy/), so it's built and tested inside
# sluiced/Dockerfile.dev rather than natively — that keeps this working the
# same way on a Linux host or a macOS one. Source is bind-mounted, not
# baked into the image, so a normal edit/rebuild loop doesn't rebuild it.
DOCKER_RUN_CPP = docker run --rm -u $$(id -u):$$(id -g) -v $(CURDIR):/workspace -w /workspace $(CPP_BUILD_IMAGE)

cpp-build-image:
	docker build -f sluiced/Dockerfile.dev -t $(CPP_BUILD_IMAGE) sluiced

build-cpp: cpp-build-image
	$(DOCKER_RUN_CPP) sh -c "cmake -S sluiced -B sluiced/build -DCMAKE_BUILD_TYPE=Debug && cmake --build sluiced/build"

test: test-go test-cpp

test-go:
	go test ./...

# Builds+runs only the sluiced_test target, not the full build-cpp. The
# `sluiced` binary itself has no main() yet (that lands with event_loop.cpp
# — see ROADMAP.md) and won't link; that's an expected, staged gap, not a
# reason for `make test` to fail on work that's actually testable today.
test-cpp: cpp-build-image
	$(DOCKER_RUN_CPP) sh -c "cmake -S sluiced -B sluiced/build -DCMAKE_BUILD_TYPE=Debug && cmake --build sluiced/build --target sluiced_test && ctest --test-dir sluiced/build --output-on-failure"

# The benchmark harness runs echod + sluiced + surge together in one Linux
# container so everything talks over localhost. sluiced is a Linux/epoll binary
# built by build-cpp; the Go tools are cross-compiled to Linux to match the
# container arch, then all four binaries run inside the cpp-build image.
GOARCH_LINUX ?= $(shell uname -m | sed 's/x86_64/amd64/;s/aarch64/arm64/')

bench-go-bins:
	CGO_ENABLED=0 GOOS=linux GOARCH=$(GOARCH_LINUX) go build -o bench/bin/echod ./echod
	CGO_ENABLED=0 GOOS=linux GOARCH=$(GOARCH_LINUX) go build -o bench/bin/surge ./surge
	CGO_ENABLED=0 GOOS=linux GOARCH=$(GOARCH_LINUX) go build -o bench/bin/adminget ./bench/adminget

# Shared prerequisites for every harness target: the C++ binary and the Go
# tools, both built for Linux.
bench-prereqs: build-cpp bench-go-bins

BENCH_RUN = docker run --rm -u $$(id -u):$$(id -g) -v $(CURDIR):/workspace -w /workspace $(CPP_BUILD_IMAGE) bash

e2e: bench-prereqs
	$(BENCH_RUN) bench/e2e.sh

bench: bench-prereqs
	$(BENCH_RUN) bench/run.sh

leakcheck: bench-prereqs
	$(BENCH_RUN) bench/leakcheck.sh

cluster: cluster-up cluster-build cluster-load cluster-apply cluster-verify

cluster-up:
	kind get clusters | grep -qx $(KIND_CLUSTER) || \
		kind create cluster --name $(KIND_CLUSTER) --config deploy/kind-config.yaml

cluster-build:
	docker build -f deploy/docker/go-service.Dockerfile --build-arg COMPONENT=echod -t sluice/echod:$(IMG_TAG) .
	docker build -f deploy/docker/go-service.Dockerfile --build-arg COMPONENT=sluice-controller -t sluice/sluice-controller:$(IMG_TAG) .
	docker build -f deploy/docker/go-service.Dockerfile --build-arg COMPONENT=surge -t sluice/surge:$(IMG_TAG) .
	docker build -f deploy/docker/sluiced.Dockerfile -t sluice/sluiced:$(IMG_TAG) .

cluster-load:
	kind load docker-image sluice/echod:$(IMG_TAG) sluice/sluice-controller:$(IMG_TAG) sluice/surge:$(IMG_TAG) sluice/sluiced:$(IMG_TAG) --name $(KIND_CLUSTER)

cluster-apply:
	# namespace first and separately: kubectl apply -f <dir> applies files in
	# lexical filename order, and "echod-*.yaml" sorts before "namespace.yaml".
	kubectl apply -f deploy/k8s/namespace.yaml
	kubectl apply -f deploy/k8s/
	kubectl -n $(NAMESPACE) rollout status deployment/echod --timeout=120s
	kubectl -n $(NAMESPACE) rollout status deployment/sluice-controller --timeout=120s
	# sluiced before surge: surge targets the sluiced Service.
	kubectl -n $(NAMESPACE) rollout status deployment/sluiced --timeout=120s
	kubectl -n $(NAMESPACE) rollout status deployment/surge --timeout=180s

cluster-verify:
	kubectl -n $(NAMESPACE) wait --for=condition=Ready pod -l app=echod --timeout=120s
	kubectl -n $(NAMESPACE) wait --for=condition=Ready pod -l app=sluice-controller --timeout=120s
	kubectl -n $(NAMESPACE) wait --for=condition=Ready pod -l app=sluiced --timeout=120s
	kubectl -n $(NAMESPACE) wait --for=condition=Ready pod -l app=surge --timeout=180s
	kubectl -n $(NAMESPACE) run verify --rm -i --restart=Never --image=curlimages/curl:8.10.1 -- \
		sh -c 'curl -sf http://sluice-controller:8080/ && curl -sf http://surge:7002/results | grep -q "\"totalIssued\""'

cluster-down:
	kind delete cluster --name $(KIND_CLUSTER)
