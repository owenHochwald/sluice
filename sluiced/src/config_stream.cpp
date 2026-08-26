#include "sluiced/config_stream.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <thread>

#ifdef SLUICE_HAVE_GRPC
#include <grpcpp/grpcpp.h>

#include "sluice/v1/config.grpc.pb.h"
#include "sluice/v1/config.pb.h"
#endif

namespace sluiced {

namespace {

std::string Trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

}  // namespace

ConfigStreamClient::ConfigStreamClient(std::string bootstrap_file, std::string controller_addr,
                                       BackendSetCallback on_update)
    : bootstrap_file_(std::move(bootstrap_file)),
      controller_addr_(std::move(controller_addr)),
      on_update_(std::move(on_update)) {}

ConfigStreamClient::~ConfigStreamClient() { Stop(); }

void ConfigStreamClient::LoadBootstrap() {
    std::ifstream f(bootstrap_file_);
    if (!f) {
        std::fprintf(stderr, "config: bootstrap file %s not found; starting with no backends\n",
                     bootstrap_file_.c_str());
        return;
    }
    std::vector<std::string> addresses;
    std::string line;
    while (std::getline(f, line)) {
        const std::string a = Trim(line);
        if (a.empty() || a[0] == '#') continue;
        addresses.push_back(a);
    }
    if (addresses.empty()) {
        std::fprintf(stderr, "config: bootstrap file %s empty; starting with no backends\n",
                     bootstrap_file_.c_str());
        return;
    }
    // Bootstrap is version 1 so any later controller publish (which starts at
    // its own version) supersedes it only if strictly newer.
    Apply(1, std::move(addresses));
}

bool ConfigStreamClient::Apply(std::uint64_t version, std::vector<std::string> addresses) {
    if (addresses.empty()) {
        std::fprintf(stderr, "config: rejecting empty backend set (version %llu)\n",
                     static_cast<unsigned long long>(version));
        return false;  // DP-X-06
    }
    if (version <= current_version_.load()) {
        std::fprintf(stderr, "config: rejecting version %llu, not newer than %llu\n",
                     static_cast<unsigned long long>(version),
                     static_cast<unsigned long long>(current_version_.load()));
        return false;  // DP-X-04
    }
    current_version_.store(version);
    on_update_(version, std::move(addresses));
    return true;
}

void ConfigStreamClient::Start() {
#ifdef SLUICE_HAVE_GRPC
    if (controller_addr_.empty()) {
        std::fprintf(stderr, "config: no controller address; serving bootstrap set (fail-static)\n");
        return;
    }
    running_.store(true);
    thread_ = std::thread([this] { WatchLoop(); });
#else
    std::fprintf(stderr,
                 "config: built without gRPC; serving bootstrap set forever (fail-static)\n");
#endif
}

void ConfigStreamClient::Stop() noexcept {
    running_.store(false);
    if (thread_.joinable()) thread_.join();
}

#ifdef SLUICE_HAVE_GRPC
void ConfigStreamClient::WatchLoop() {
    std::random_device rd;
    std::mt19937_64 rng(rd());
    std::uint32_t backoff_ms = 100;
    constexpr std::uint32_t kMaxBackoff = 30000;

    while (running_.load()) {
        auto channel = grpc::CreateChannel(controller_addr_, grpc::InsecureChannelCredentials());
        auto stub = sluice::v1::ConfigStream::NewStub(channel);

        grpc::ClientContext ctx;
        sluice::v1::WatchRequest req;
        auto reader = stub->Watch(&ctx, req);

        bool got_any = false;
        sluice::v1::BackendSet msg;
        while (running_.load() && reader->Read(&msg)) {
            std::vector<std::string> addresses;
            addresses.reserve(msg.backends_size());
            for (const auto& b : msg.backends()) addresses.push_back(b.address());
            Apply(msg.version(), std::move(addresses));
            got_any = true;
            backoff_ms = 100;  // healthy stream resets the backoff
        }
        ctx.TryCancel();
        reader->Finish();

        if (!running_.load()) break;

        // DP-E-11: exponential backoff with full jitter. DP-X-05: we simply
        // keep serving the last-good set; nothing here reduces it.
        std::uniform_int_distribution<std::uint32_t> jitter(0, backoff_ms);
        const std::uint32_t sleep_ms = jitter(rng);
        std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
        if (!got_any) backoff_ms = std::min(backoff_ms * 2, kMaxBackoff);
    }
}
#else
void ConfigStreamClient::WatchLoop() {}
#endif

}  // namespace sluiced
