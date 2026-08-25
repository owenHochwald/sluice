.PHONY: build build-go build-cpp test test-go test-cpp e2e bench leakcheck cluster

build: build-go build-cpp

build-go:
	go build ./...

build-cpp:
	cmake -S sluiced -B sluiced/build -DCMAKE_BUILD_TYPE=Debug
	cmake --build sluiced/build

test: test-go test-cpp

test-go:
	go test ./...

test-cpp: build-cpp
	ctest --test-dir sluiced/build --output-on-failure

e2e bench leakcheck:
	@echo "$@: blocked on sluiced/src implementation — see sluiced/ROADMAP.md" >&2
	@exit 1

cluster:
	@echo "cluster: deploy/ (kind config, k8s manifests) not yet added" >&2
	@exit 1
