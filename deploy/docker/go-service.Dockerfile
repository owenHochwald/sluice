# Shared, parameterized build for every Go binary in this repo (echod,
# sluice-controller, surge) — they all build the same way out of one
# go.mod, so one Dockerfile with a COMPONENT build-arg keeps base-image and
# CGO settings in one place instead of drifting across three files.
#
# Build from the repo root as context, e.g.:
#   docker build -f deploy/docker/go-service.Dockerfile --build-arg COMPONENT=echod -t sluice/echod:dev .
#
# sluiced (the C++ data plane) is deliberately not built here — it has no
# src/ yet (see sluiced/ROADMAP.md).

FROM golang:1.26-alpine AS build
WORKDIR /src
COPY go.mod go.sum ./
RUN go mod download
COPY . .
ARG COMPONENT
RUN test -n "$COMPONENT" || (echo "COMPONENT build-arg is required" >&2 && exit 1)
RUN CGO_ENABLED=0 GOOS=linux go build -trimpath -o /out/app ./${COMPONENT}

# distroless/static: every binary here is pure Go with CGO disabled, so no
# libc is needed at all. No shell, no package manager — smallest attack
# surface, runs non-root by default. Tradeoff: `kubectl exec -it -- sh`
# won't work; `kubectl debug` is the escape hatch for interactive debugging.
FROM gcr.io/distroless/static-debian12:nonroot
COPY --from=build /out/app /app
USER nonroot:nonroot
ENTRYPOINT ["/app"]
