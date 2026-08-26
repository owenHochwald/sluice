# sluiced implementation roadmap

## Prerequisites

- `cmake` and a C++20 compiler (already installed on this machine).
- gRPC + Protobuf C++, for `config_stream.hpp` (already installed on this
  machine via Homebrew). On a machine without them, `cmake` still configures
  fine and just skips building `sluiced_configpb` until `brew install grpc
  protobuf` is done and cmake is re-run.

## Suggested build order

Each stage names the header it implements against and the SPEC.md IDs it
satisfies. Do them in order — each one unblocks the next.

1. **`src/maglev.cpp`** — `include/sluiced/maglev.hpp` (DP-U-08/09, DP-E-03/04).
   Pure logic, no sockets. `test/test_maglev.cpp` is already written against
   the header (TST-01 distribution uniformity, TST-02 disruption bound) —
   once this file exists, uncomment its line in `CMakeLists.txt`'s
   `sluiced_test` sources and `make test` starts actually checking it.

2. **`src/connection.cpp` + `src/event_loop.cpp`** — get a minimal one-core
   echo-proxy working end to end: accept, connect upstream, forward bytes
   both ways, handle half-close and backpressure (DP-U-02/03, DP-E-02,
   DP-S-01/02, DP-X-02). This is the state machine the whole project is
   about — see SPEC.md §5.1 for every case, and TST-03 (byte-identical
   proxying) for how it's checked in `make e2e` once `echod` is up as the
   backend.

3. **Multi-core** — spin up N `EventLoop`s, each with its own
   `SO_REUSEPORT` listener and connection table (DP-U-04..07). No shared
   state between them; this is where the "shared-nothing" invariant either
   holds or doesn't.

4. **`src/config_stream.cpp`** — `include/sluiced/config_stream.hpp`
   (DP-E-09/10/11, DP-X-04/05/06). Bootstrap-file loading first (serve
   before the control plane even exists), then the gRPC `Watch()` client
   against `sluice-controller`. This is where "fail static" becomes real —
   TST-06 is the test that proves it.

5. **`src/health_checker.cpp`** — `include/sluiced/health_checker.hpp`
   (DP-U-11..13, DP-E-05..08, DP-X-03 — the panic threshold, worth reading
   the rationale in SPEC.md §5.4 before writing it). TST-04 and TST-08 cover
   this.

6. **`src/stats.cpp`** — `include/sluiced/stats.hpp` (DP-U-15/16/17). Vendor
   an HDR histogram implementation rather than hand-rolling percentile
   bucketing.

7. **`src/admin_socket.cpp`** — `include/sluiced/admin_socket.hpp`
   (DP-U-14, §7). Last on purpose: everything it reports on needs to exist
   first.

## Notes

- `CMakeLists.txt`'s `sluiced` executable target is built automatically the
  moment any `.cpp` lands in `src/` — you don't need to touch the CMake file
  as you go, just add source files.
- `make test` (from the repo root, or `ctest` directly in `sluiced/build`)
  stays green throughout: only `test_smoke.cpp` runs until you wire in
  `test_maglev.cpp` yourself at stage 1.
- `make e2e`, `make bench`, and `make leakcheck` are stubbed at the repo
  root to fail loudly with "blocked on sluiced/src" until stage 2 gets far
  enough to proxy real traffic — that's expected, not a bug.
