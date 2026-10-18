// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "catalog.h"

namespace skyline::crypto { class KeyStore; }
namespace skyline::vfs { class Backing; class NCA; }

namespace skyline::catalog {
    class NativeInspectionBudget;
    // Holds a reliable-count Backing by shared ownership; never adopts an integer
    // FD again. Reads use ReadUnchecked's actual count. Backing exposes no
    // opened-file identity, so this source's snapshot token is unavailable.
    Source MakeBackingSource(std::shared_ptr<vfs::Backing> backing);
    // Exact, read-only bridge. Every legacy read must return all requested bytes
    // or throw; its own logical extent remains enforced by ReadUnchecked too.
    std::shared_ptr<vfs::Backing> MakeSourceBacking(Source source);
    struct OpenedNativeNca {
        std::shared_ptr<vfs::NCA> nca;
        std::shared_ptr<const NcaDescriptor> descriptor;
    };
    // Explicit runtime preparation reuses the same strict profile as inspection.
    // It adds no BKTR/plaintext support and never downgrades existing checks.
    OpenedNativeNca OpenNativeNca(const PfsEntry &entry, std::shared_ptr<crypto::KeyStore> keyStore,
                                 const Options &options);

    // Explicit Pine-internal inspection. Keys must already be legally provided
    // by the caller. This API never searches for, creates or logs keys.
    // The existing vfs::NCA performs decryption; additive validators protect the
    // catalog from legacy section/PFS parsing before those parsers run.
    class NativeNcaInspector final : public NcaInspector {
        std::shared_ptr<crypto::KeyStore> keyStore;
        std::shared_ptr<NativeInspectionBudget> budget;
      public:
        // One inspector instance belongs to one scan, including all NCA members.
        // Construct a new instance for a new scan to reset its metadata budget.
        explicit NativeNcaInspector(std::shared_ptr<crypto::KeyStore> keyStore);
        std::string Name() const override { return "pine_existing_nca"; }
        NcaDescriptor Inspect(const PfsEntry &entry, const Options &options) const override;
    };

    // No JNI, UI, loader selection or runtime registration is installed by this
    // function. The application must opt into both compilation and this call.
    Catalog ScanNative(std::shared_ptr<vfs::Backing> container,
                       std::shared_ptr<crypto::KeyStore> keyStore,
                       const Options &options = {});
    // Prefer this overload when identity checks are required: one Source owns
    // the ciphertext bytes and its opened-file snapshot/change policy together.
    Catalog ScanNative(Source container, std::shared_ptr<crypto::KeyStore> keyStore,
                       const Options &options = {});
}
