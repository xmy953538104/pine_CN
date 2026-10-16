// SPDX-License-Identifier: MPL-2.0
// Compiles and invokes production IStorage.cpp + Backing; only IPC/platform APIs are stubbed.
#include "services/fssrv/IStorage.h"
#include "diagnostics/content.h"
#include <cassert>
#include <map>
#include <string>
#include <vector>

namespace skyline::diagnostics {
bool enabled{}, verbose{};
struct Observation {
    std::string category, event, level;
    std::map<std::string, std::string> fields;
};
std::vector<Observation> observations;
bool Enabled() noexcept { return enabled; }
bool Verbose() noexcept { return verbose; }
void Emit(std::string_view category, std::string_view event,
          std::initializer_list<std::pair<std::string_view, std::string_view>> fields,
          std::string_view level) noexcept {
    Observation observation{std::string(category), std::string(event), std::string(level), {}};
    for (const auto &[key, value] : fields) observation.fields.emplace(key, value);
    observations.push_back(std::move(observation));
}
}

using namespace skyline;
struct ControlledFailure : exception { ControlledFailure() : exception("controlled IO error") {} };
struct ControlledBacking : vfs::Backing {
    size_t result{4}, calls{}, lastOffset{}, lastSize{};
    bool fail{};
    ControlledBacking() : Backing({true, false, false}, 64) {}
    bool HasReliableReadByteCount() const noexcept override { return true; }
    size_t ReadImpl(span<u8> output, size_t offset) override {
        ++calls;
        lastOffset = offset;
        lastSize = output.size();
        if (fail) throw ControlledFailure{};
        for (size_t i = 0; i < std::min(result, output.size()); ++i) output[i] = static_cast<u8>(i + 1);
        return result;
    }
};

int main() {
    using namespace diagnostics;
    using service::fssrv::IStorage;
    DeviceState state;
    service::ServiceManager manager;
    kernel::type::KSession session;
    kernel::ipc::IpcResponse response;
    auto backing{std::make_shared<ControlledBacking>()};
    std::array<u8, 4> buffer{};
    auto read = [&](IStorage &storage, u64 offset = 8, u64 requestedSize = 999) {
        kernel::ipc::IpcRequest request{{offset, requestedSize}, {buffer}};
        assert(storage.Read(session, request, response) == Result{});
    };

    IStorage dlc(backing, state, manager, 0x0100000000001001);
    enabled = false;
    read(dlc);
    assert(observations.empty() && backing->calls == 1);
    assert(backing->lastOffset == 8 && backing->lastSize == 4 && buffer[3] == 4);

    enabled = true;
    read(dlc);
    assert(observations.size() == 1);
    const auto &first = observations.back();
    assert(first.category == "storage" && first.event == "DLC_DATA_READ" && first.level == "INFO");
    assert(first.fields.at("title_id") == "0100000000001001");
    assert(first.fields.at("request_size") == "999" && first.fields.at("buffer_size") == "4");
    assert(first.fields.at("actual_bytes") == "4" && first.fields.at("offset") == "8");
    assert(first.fields.at("read_ordinal") == "1" && first.fields.at("success_sampling") == "first_then_1_in_128");
    assert(first.fields.at("sampling_period") == "128");
    // The guest still receives the whole output buffer; the historically ignored size stays ignored.
    assert(backing->lastSize == 4 && backing->lastOffset == 8);
    for (unsigned i = 0; i < 127; ++i) read(dlc);
    assert(observations.size() == 1);
    read(dlc);
    assert(observations.size() == 2 && observations.back().fields.at("read_ordinal") == "129");

    verbose = true;
    for (unsigned i = 0; i < 15; ++i) read(dlc);
    assert(observations.size() == 2);
    read(dlc);
    assert(observations.size() == 3 && observations.back().fields.at("read_ordinal") == "145");
    for (unsigned i = 0; i < 15; ++i) read(dlc);
    assert(observations.size() == 3);
    read(dlc);
    assert(observations.size() == 4 && observations.back().fields.at("read_ordinal") == "161");
    assert(observations.back().fields.at("success_sampling") == "first_then_1_in_16");
    assert(observations.back().fields.at("sampling_period") == "16");
    verbose = false;
    backing->result = 2;
    read(dlc);
    verbose = true;
    read(dlc);
    assert(observations.size() == 6); // Every short read is retained at either level.
    assert(observations.back().event == "DLC_RUNTIME_ERROR" && observations.back().level == "ERROR");
    assert(observations.back().fields.at("actual_bytes") == "2" && observations.back().fields.at("status") == "short_read");
    assert(observations.back().fields.at("sampling_period") == "1");
    assert(backingWarnings == 2);

    verbose = false;
    backing->fail = true;
    try { read(dlc); assert(false); } catch (const ControlledFailure &) {}
    assert(observations.size() == 7 && observations.back().fields.at("status") == "exception");
    assert(observations.back().fields.at("operation") == "read" && observations.back().fields.at("actual_bytes") == "unknown");
    assert(observations.back().fields.at("sampling_period") == "1");
    assert(observations.back().fields.at("offset") == "8" && observations.back().fields.at("request_size") == "999");
    verbose = true;
    try { read(dlc); assert(false); } catch (const ControlledFailure &) {}
    assert(observations.size() == 8 && observations.back().fields.at("sampling_period") == "1");
    const auto calls = backing->calls;
    try { read(dlc, 65); assert(false); } catch (const exception &) {}
    assert(backing->calls == calls && observations.size() == 9);
    assert(observations.back().fields.at("offset") == "65");
    kernel::ipc::IpcRequest missingBuffer{{8, 4}, {}};
    try { dlc.Read(session, missingBuffer, response); assert(false); } catch (const std::out_of_range &) {}
    assert(backing->calls == calls && observations.back().fields.at("buffer_size") == "unknown");

    backing->fail = false;
    backing->result = 0;
    buffer.fill(0);
    read(dlc);
    assert(observations.back().fields.at("actual_bytes") == "0" && buffer[0] == 0);
    backing->result = 4;
    IStorage detailed(backing, state, manager, 0x0100000000001002);
    const auto detailedBegin = observations.size();
    read(detailed);
    assert(observations.size() == detailedBegin + 1 && observations.back().fields.at("read_ordinal") == "1");
    assert(observations.back().fields.at("sampling_period") == "16");
    for (unsigned i = 0; i < 15; ++i) read(detailed);
    assert(observations.size() == detailedBegin + 1);
    read(detailed);
    assert(observations.size() == detailedBegin + 2 && observations.back().fields.at("read_ordinal") == "17");
    const auto observed = observations.size();
    IStorage base(backing, state, manager);
    read(base);
    assert(observations.size() == observed); // Only proven DLC backings carry DLC events.
    enabled = false;
    backing->fail = true;
    try { read(dlc); assert(false); } catch (const ControlledFailure &) {}
    assert(observations.size() == observed); // Disabled read keeps original exception and emits nothing.
    dlc.GetSize(session, missingBuffer, response);
    assert(response.value == backing->size);
}
