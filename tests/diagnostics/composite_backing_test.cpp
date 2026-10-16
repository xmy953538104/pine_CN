// SPDX-License-Identifier: MPL-2.0
// Real BKTR, RegionBacking, CTR, OsBacking and IStorage; only IPC/platform/cipher APIs are stubbed.
#include "vfs/bktr.h"
#include "vfs/region_backing.h"
#include "vfs/ctr_encrypted_backing.h"
#include "vfs/os_backing.h"
#include "services/fssrv/IStorage.h"
#include "diagnostics/content.h"
#include <cassert>
#include <fcntl.h>
#include <map>
#include <unistd.h>

namespace skyline::diagnostics {
bool enabled{};
struct Observation {
    std::string event;
    std::map<std::string, std::string> fields;
};
std::vector<Observation> observations;
bool Enabled() noexcept { return enabled; }
bool Verbose() noexcept { return false; }
void Emit(std::string_view category, std::string_view event,
          std::initializer_list<std::pair<std::string_view, std::string_view>> fields,
          std::string_view) noexcept {
    assert(category == "storage");
    Observation observation{std::string(event), {}};
    for (const auto &[key, value] : fields) observation.fields.emplace(key, value);
    observations.push_back(std::move(observation));
}
}
using namespace skyline;
struct ControlledBacking : vfs::Backing {
    size_t maxBytes{std::numeric_limits<size_t>::max()}, calls{};
    ControlledBacking() : Backing({true, false, false}, 64) {}
    bool HasReliableReadByteCount() const noexcept override { return true; }
    size_t ReadImpl(span<u8> output, size_t) override {
        ++calls;
        const auto amount = std::min(output.size(), maxBytes);
        std::fill_n(output.data(), amount, 0x5A);
        return amount;
    }
};
std::shared_ptr<vfs::BKTR> makeBktr(const std::shared_ptr<ControlledBacking> &base,
                                  const std::shared_ptr<ControlledBacking> &patch, bool fromPatch, bool split = false) {
    vfs::RelocationBlock relocation{};
    relocation.numberBuckets = 1;
    relocation.size = 64;
    vfs::RelocationBucket relocations{split ? 2U : 1U, 64, {{0, 0, fromPatch ? 1U : 0U}}};
    if (split) relocations.entries.push_back({2, 2, 1});
    vfs::SubsectionBlock subsection{};
    subsection.numberBuckets = 1;
    subsection.size = 64;
    vfs::SubsectionBucket subsections{1, 64, {{0, {0}, 0}, {64, {0}, 0}}};
    return std::make_shared<vfs::BKTR>(base, patch, relocation, std::vector{relocations}, subsection,
        std::vector{subsections}, false, std::array<u8, 16>{}, 0, 0, std::array<u8, 8>{});
}
int main() {
    using namespace diagnostics;
    auto base{std::make_shared<ControlledBacking>()}, patch{std::make_shared<ControlledBacking>()};
    base->maxBytes = 2;
    patch->maxBytes = 2;
    std::array<u8, 4> output{};
    vfs::ReadObservation observation{999};

    auto region{std::make_shared<vfs::RegionBacking>(base, 0, 64)};
    assert(region->Read(output, 0, &observation) == 64 && observation.bytes == 2 && base->calls == 1);
    auto bktr{makeBktr(base, patch, false)};
    output.fill(0);
    const auto beforeLegacy = base->calls;
    assert(bktr->ReadUnchecked(output) == output.size()); // Existing BKTR hides the source's 2-byte short read.
    assert(base->calls == beforeLegacy + 1 && output[0] == 0x5A && output[2] == 0);
    auto bktrRegion{std::make_shared<vfs::RegionBacking>(bktr, 0, 64)};
    const auto beforeObserved = base->calls;
    output.fill(0);
    assert(bktrRegion->Read(output, 0, &observation) == 64);
    assert(!observation.bytes && base->calls == beforeObserved + 1 && output[2] == 0);
    assert(!bktrRegion->HasReliableReadByteCount());

    // Service behavior remains successful, but its diagnostics cannot claim a complete read.
    DeviceState state;
    service::ServiceManager manager;
    kernel::type::KSession session;
    kernel::ipc::IpcResponse response;
    service::fssrv::IStorage storage(bktrRegion, state, manager, 0x0100000000001001);
    auto read = [&] {
        kernel::ipc::IpcRequest request{{0, 4}, {output}};
        return storage.Read(session, request, response);
    };
    const auto beforeDisabled = base->calls;
    enabled = false;
    output.fill(0);
    assert(read() == Result{} && base->calls == beforeDisabled + 1 && observations.empty());
    const auto disabledOutput = output;
    const auto disabledWarnings = backingWarnings;
    enabled = true;
    output.fill(0);
    assert(read() == Result{} && base->calls == beforeDisabled + 2 && output == disabledOutput);
    assert(backingWarnings == disabledWarnings + 1); // Same underlying short-read warning.
    assert(observations.size() == 1 && observations.back().event == "DLC_DATA_READ");
    assert(observations.back().fields.at("actual_bytes") == "unknown");
    assert(observations.back().fields.at("status") == "unknown");
    assert(observations.back().fields.at("byte_count_evidence") == "unavailable_for_backing");

    auto patchBktr{makeBktr(base, patch, true)};
    const auto beforePatch = patch->calls;
    assert(patchBktr->ReadUnchecked(output) == patch->size); // Preserve the other legacy total-size return.
    assert(patch->calls == beforePatch + 1);
    vfs::RegionBacking patchRegion(patchBktr, 0, 64);
    assert(patchRegion.Read(output, 0, &observation) == 64 && !observation.bytes);
    assert(patch->calls == beforePatch + 2);

    base->maxBytes = patch->maxBytes = 1;
    auto partitioned{makeBktr(base, patch, false, true)};
    const auto beforeBasePartition = base->calls, beforePatchPartition = patch->calls;
    assert(partitioned->ReadUnchecked(output) == 66); // 2-byte Region total + 64-byte patch total, unchanged.
    assert(base->calls == beforeBasePartition + 1 && patch->calls == beforePatchPartition + 1);
    vfs::RegionBacking partitionRegion(partitioned, 0, 64);
    assert(partitionRegion.Read(output, 0, &observation) == 64 && !observation.bytes);
    assert(base->calls == beforeBasePartition + 2 && patch->calls == beforePatchPartition + 2);

    crypto::KeyStore::Key128 zero{};
    base->maxBytes = 2;
    vfs::CtrEncryptedBacking ctrOverBktr(zero, zero, bktrRegion, 0);
    const auto beforeCtr = base->calls;
    assert(ctrOverBktr.Read(output, 0, &observation) == 64 && !observation.bytes);
    assert(base->calls == beforeCtr + 1); // No extra read is performed to discover evidence.
    vfs::CtrEncryptedBacking ctrOverLeaf(zero, zero, region, 0);
    assert(ctrOverLeaf.Read(output, 0, &observation) == 64 && observation.bytes == 0);
    base->maxBytes = 64;
    assert(ctrOverLeaf.Read(output, 0, &observation) == 64 && observation.bytes == 4);
    assert(ctrOverLeaf.Read(output, 1, &observation) == 64 && observation.bytes == 4);

    // A real OS leaf observes an EOF short read even though its cached backing size is larger.
    char filename[]{"/tmp/pine-backing-XXXXXX"};
    const int fd = mkstemp(filename);
    assert(fd >= 0 && unlink(filename) == 0 && ftruncate(fd, 64) == 0);
    vfs::OsBacking osBacking(fd, true);
    assert(osBacking.Read(output, 0, &observation) == 64 && observation.bytes == 4);
    assert(ftruncate(fd, 2) == 0);
    assert(osBacking.Read(output, 0, &observation) == 64 && observation.bytes == 2);
}
