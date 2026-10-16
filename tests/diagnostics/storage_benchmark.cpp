// SPDX-License-Identifier: MPL-2.0
// Production IStorage, Backing and sink; host stand-ins supply only IPC/platform dependencies.
#include "services/fssrv/IStorage.h"
#include "diagnostics/diagnostics.h"
#include <array>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>

using namespace skyline;
class ControlledBacking final : public vfs::Backing {
    std::array<u8, 128> source{};
public:
    uint64_t calls{};
    ControlledBacking() : Backing({true, false, false}, 128) {
        for (size_t i = 0; i < source.size(); ++i) source[i] = static_cast<u8>(i);
    }
    bool HasReliableReadByteCount() const noexcept override { return true; }
    size_t ReadImpl(span<u8> output, size_t offset) override {
        ++calls;
        std::memcpy(output.data(), source.data() + offset, output.size());
        return output.size();
    }
};

int main(int argc, char **argv) {
    if (argc != 5) return 2;
    const std::string mode = argv[1];
    if (mode != "off" && mode != "normal" && mode != "detailed") return 2;
    const uint64_t reads = std::stoull(argv[2]);
    if (!reads) return 2;
    const bool enabled = mode != "off";
    const uint64_t period = mode == "detailed" ? 16 : 128;
    if (enabled) {
        diagnostics::Config config;
        config.directory = argv[3];
        config.sessionId = std::string("host-read-") + argv[4] + '-' + mode;
        config.verbose = mode == "detailed";
        if (!diagnostics::Start(config)) return 3;
    }
    DeviceState state;
    service::ServiceManager manager;
    kernel::type::KSession session;
    kernel::ipc::IpcResponse response;
    auto backing = std::make_shared<ControlledBacking>();
    service::fssrv::IStorage storage(backing, state, manager, 0x0100000000001001);
    std::array<u8, 64> output{};
    kernel::ipc::IpcRequest request{{8, output.size()}, {output}};
    uint64_t checksum{};
    const auto begin = std::chrono::steady_clock::now();
    for (uint64_t i = 0; i < reads; ++i) {
        request.cursor = 0;
        if (!(storage.Read(session, request, response) == Result{})) return 4;
        checksum += output[i % output.size()];
    }
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    const auto drainBegin = std::chrono::steady_clock::now();
    diagnostics::Stop();
    const auto drain = std::chrono::steady_clock::now() - drainBegin;
    const auto stats = diagnostics::Stats();
    const auto attempts = enabled ? (reads + period - 1) / period : 0;
    if (backing->calls != reads || stats.accepted + stats.dropped != attempts ||
        stats.accepted != stats.written || stats.diskErrors != 0) return 5;
    std::cout << "{\"mode\":\"" << mode << "\",\"reads\":" << reads
              << ",\"bytes_per_read\":64,\"calls_ns\":"
              << std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()
              << ",\"stop_drain_ns\":"
              << std::chrono::duration_cast<std::chrono::nanoseconds>(drain).count()
              << ",\"sampling_period\":" << (enabled ? period : 0)
              << ",\"expected_attempts\":" << attempts
              << ",\"accepted\":" << stats.accepted << ",\"written\":" << stats.written
              << ",\"dropped\":" << stats.dropped << ",\"disk_errors\":" << stats.diskErrors
              << ",\"backing_calls\":" << backing->calls << ",\"checksum\":" << checksum << "}\n";
}
