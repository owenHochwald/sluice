.PHONY: build build-go build-cpp test test-go test-cpp e2e bench leakcheck cluster \
	cluster-up cluster-build cluster-load cluster-apply cluster-verify cluster-down

KIND_CLUSTER ?= sluice
NAMESPACE    ?= sluice-demo
IMG_TAG      ?= dev

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

cluster: cluster-up cluster-build cluster-load cluster-apply cluster-verify

cluster-up:
	kind get clusters | grep -qx $(KIND_CLUSTER) || \
		kind create cluster --name $(KIND_CLUSTER) --config deploy/kind-config.yaml

cluster-build:
	docker build -f deploy/docker/go-service.Dockerfile --build-arg COMPONENT=echod -t sluice/echod:$(IMG_TAG) .
	docker build -f deploy/docker/go-service.Dockerfile --build-arg COMPONENT=sluice-controller -t sluice/sluice-controller:$(IMG_TAG) .
	docker build -f deploy/docker/go-service.Dockerfile --build-arg COMPONENT=surge -t sluice/surge:$(IMG_TAG) .

cluster-load:
	kind load docker-image sluice/echod:$(IMG_TAG) sluice/sluice-controller:$(IMG_TAG) sluice/surge:$(IMG_TAG) --name $(KIND_CLUSTER)

cluster-apply:
	# namespace first and separately: kubectl apply -f <dir> applies files in
	# lexical filename order, and "echod-*.yaml" sorts before "namespace.yaml".
	kubectl apply -f deploy/k8s/namespace.yaml
	kubectl apply -f deploy/k8s/
	kubectl -n $(NAMESPACE) rollout status deployment/echod --timeout=120s
	kubectl -n $(NAMESPACE) rollout status deployment/sluice-controller --timeout=120s
	kubectl -n $(NAMESPACE) rollout status deployment/surge --timeout=180s

cluster-verify:
	kubectl -n $(NAMESPACE) wait --for=condition=Ready pod -l app=echod --timeout=120s
	kubectl -n $(NAMESPACE) wait --for=condition=Ready pod -l app=sluice-controller --timeout=120s
	kubectl -n $(NAMESPACE) wait --for=condition=Ready pod -l app=surge --timeout=180s
	kubectl -n $(NAMESPACE) run verify --rm -i --restart=Never --image=curlimages/curl:8.10.1 -- \
		sh -c 'curl -sf http://sluice-controller:8080/ && curl -sf http://surge:7002/results | grep -q "\"totalIssued\""'

cluster-down:
	kind delete cluster --name $(KIND_CLUSTER)
