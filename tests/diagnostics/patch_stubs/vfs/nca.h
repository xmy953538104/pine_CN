// NCA construction is controlled here; cryptographic NCA parsing is outside this host test.
#pragma once
#include <common.h>
#include <vfs/backing.h>
namespace skyline::crypto { struct KeyStore {}; }
namespace skyline::vfs {
struct FileSystem {};
struct ControlledPatchFailure : exception { ControlledPatchFailure() : exception("controlled patch error") {} };
struct NCA {
    struct Header { u64 titleId{}; } header;
    std::shared_ptr<Backing> romFs;
    std::shared_ptr<FileSystem> exeFs;
    static inline bool fail{};
    static inline Backing *receivedBase{};
    static inline crypto::KeyStore *receivedKeyStore{};
    static inline u64 receivedOffset{};
    NCA() = default;
    NCA(std::optional<NCA> update, std::shared_ptr<crypto::KeyStore> keyStore,
        std::shared_ptr<Backing> base, u64 offset) {
        receivedBase = base.get();
        receivedKeyStore = keyStore.get();
        receivedOffset = offset;
        if (fail) throw ControlledPatchFailure{};
        header = update->header;
        romFs = update->romFs;
    }
};
}
