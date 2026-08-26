#pragma once
// Client side of the gRPC ConfigStream service (../../proto/sluice/v1/config.proto)
// plus the bootstrap-file loader. docs/SPEC.md §5.5 (DP-E-09/10/11, DP-X-04/05/06).
//
// Contract — this is the header that makes "fail static" real:
//  - On startup, load a bootstrap backend set from a local file and begin
//    serving *before* any control-plane connection exists (DP-E-09).
//  - Apply a received BackendSet only if its version is strictly greater
//    than the current one; reject (log, keep serving current) anything
//    malformed or not strictly newer (DP-X-04).
//  - Reject an empty BackendSet outright — never let the live set shrink to
//    zero because of a bad or empty publish (DP-X-06).
//  - If the stream disconnects, keep serving the last known-good set
//    indefinitely, and reconnect with exponential backoff + full jitter
//    (DP-X-05, DP-E-11). Losing the control plane must never look like
//    losing backends — that invariant is the reason this project exists.
//
// TODO(owen): implement in src/config_stream.cpp. The generated gRPC C++
// stubs land in the CMake build dir at genproto/sluice/v1/config.pb.h and
// config.grpc.pb.h — see ../CMakeLists.txt, which builds a
// `sluiced_configpb` target whenever gRPC C++ is found (`brew install grpc
// protobuf` if it isn't); link against it. Suggested shape: a background
// thread running the Watch() client loop, publishing a new MaglevTable via
// the same atomic-swap mechanism discussed in maglev.hpp.

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace sluiced {

// Invoked with a new backend set every time the data plane accepts one — from
// the bootstrap file, or from a Watch() message that passed validation
// (DP-X-04/06). The set is delivered as plain "ip:port" strings: Backend
// carries atomics and cannot live in a movable/copyable vector, and the Router
// is what owns turning addresses into stable Backend objects.
using BackendSetCallback =
    std::function<void(std::uint64_t version, std::vector<std::string> addresses)>;

class ConfigStreamClient {
public:
    ConfigStreamClient(std::string bootstrap_file, std::string controller_addr, BackendSetCallback on_update);
    ~ConfigStreamClient();

    ConfigStreamClient(const ConfigStreamClient&) = delete;
    ConfigStreamClient& operator=(const ConfigStreamClient&) = delete;

    // Loads the bootstrap file synchronously and invokes on_update once
    // before returning (DP-E-09) — call this before serving any traffic. The
    // file is one "ip:port" per line; blank lines and '#' comments ignored.
    void LoadBootstrap();

    // Starts the background Watch() loop against controller_addr. Never
    // blocks; reconnects internally with backoff + jitter on failure
    // (DP-E-11) and never reduces the backend set while doing so (DP-X-05).
    // When built without gRPC C++ (no `sluiced_configpb` target), this logs
    // that no control-plane transport is compiled in and returns — the data
    // plane then serves the bootstrap set forever, which is fail-static by
    // construction.
    void Start();
    void Stop() noexcept;

private:
    void WatchLoop();
    // Validate and apply one received set; returns true if applied (DP-X-04/06).
    bool Apply(std::uint64_t version, std::vector<std::string> addresses);

    std::string bootstrap_file_;
    std::string controller_addr_;
    BackendSetCallback on_update_;
    std::atomic<std::uint64_t> current_version_{0};
    std::atomic<bool> running_{false};
    std::thread thread_;
};

}  // namespace sluiced
