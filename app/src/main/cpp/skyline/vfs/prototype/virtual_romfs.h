// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <stdexcept>
#include <vector>
#include <utility>
#include <vfs/backing.h>

namespace skyline::vfs::prototype {
    struct Limits {
        uint32_t maxEntries{32768};
        uint32_t maxManifestEntries{4096};
        uint32_t maxNameBytes{255};
        uint32_t maxPathBytes{4096};
        uint32_t maxDepth{32};
        uint64_t maxTotalPathBytes{8 * 1024 * 1024};
        uint64_t maxMetadataBytes{16 * 1024 * 1024};
        uint64_t maxPayloadBytes{32 * 1024 * 1024};
        uint64_t maxHashBytes{512 * 1024 * 1024};
        uint64_t maxVirtualBytes{16ULL * 1024 * 1024 * 1024 * 1024};
    };

    // Implementations must own a fixed byte snapshot, expose an in-memory size,
    // return actual counts and throw on I/O failure. Unknown lifetime/mutability
    // contracts are rejected. The immutable-memory helper moves bytes into sole
    // private ownership; callers transfer all write aliases as well. It does
    // not copy payloads during image construction.
    class PayloadSource {
      public:
        virtual ~PayloadSource() = default;
        virtual uint64_t Size() const = 0;
        virtual size_t Read(uint64_t offset, uint8_t *out, size_t size) const = 0;
        virtual bool IsImmutableSnapshot() const noexcept { return false; }
        virtual void CheckUnchanged() const { throw std::runtime_error("payload change guard unavailable"); }
    };
    std::shared_ptr<const PayloadSource> OwnPayload(std::vector<uint8_t> bytes);

    struct Binding {
        uint64_t applicationId{}, programId{};
        std::string programSha256; // complete original effective Program bytes
        std::string romfsSha256;   // complete already-final official RomFS bytes
    };
    struct IdentityObservation {
        uint64_t applicationId{}, programId{};
        std::string computedProgramSha256, computedRomfsSha256;
    };
    enum class IdentityDecision { Unknown, Rejected, Approved };
    class IdentityVerifier {
      public:
        virtual ~IdentityVerifier() = default;
        // Trusted application boundary, not an assertion read from a manifest.
        // The provider must independently establish title/program membership
        // for these computed bytes. No provider is installed by this prototype.
        virtual IdentityDecision Verify(const IdentityObservation &observation) const = 0;
    };
    class SourceGuard {
        const std::shared_ptr<Backing> romfs, program;
      protected:
        SourceGuard(std::shared_ptr<Backing> romfs, std::shared_ptr<Backing> program)
            : romfs(std::move(romfs)), program(std::move(program)) {}
      public:
        virtual ~SourceGuard() = default;
        bool Covers(const std::shared_ptr<Backing> &r, const std::shared_ptr<Backing> &p) const noexcept { return romfs == r && program == p; }
        // Trusted owner implements a fixed-generation/snapshot check, bound to
        // these exact sources. This hot-path check must not hash, scan or stat.
        // A changed/unknown generation must throw. No default is installed.
        virtual void CheckUnchanged() const = 0;
    };

    enum class Operation : uint8_t { Replace = 0, Add = 1 };
    struct Entry {
        std::string path; // canonical relative virtual path; no host paths
        Operation operation{Operation::Replace};
        uint64_t size{};
        std::string sha256;
        std::shared_ptr<const PayloadSource> source;
    };
    struct Manifest {
        uint32_t version{1};
        Binding target;
        std::vector<Entry> entries;
    };
    struct Options {
        bool enabled{false};
        uint64_t applicationId{}, programId{}; // explicit active-context claims
        std::shared_ptr<Backing> effectiveProgram; // owned raw Program, for hash evidence
        const IdentityVerifier *identityVerifier{}; // absent => reject without I/O
        std::shared_ptr<const SourceGuard> sourceGuard; // absent/misbound => reject
        std::string expectedManifestSha256;
        Limits limits;
    };
    enum class Status { Disabled, Rejected, Activated };
    struct Result {
        std::shared_ptr<Backing> view; // exact original pointer on disabled/rejected
        Status status{Status::Disabled};
        std::string reason;
        std::string computedProgramSha256, computedRomfsSha256, computedManifestSha256;
        bool externalIdentityAttested{false};
        bool runtimeIdentityIntegrationImplemented{false};
        uint64_t metadataBytes{}, hashBytes{}, virtualSize{}, segmentCount{};
        uint64_t preservedFiles{}, replacedFiles{}, addedFiles{};
    };

    // These are byte hashes, never signature/title authentication. Manifest hash
    // serialization is documented and bounded; it performs no source reads.
    std::string ManifestSha256(const Manifest &manifest, const Limits &limits = {});
    std::string BytesSha256(const uint8_t *data, size_t size);

    // Call only after official CTR/BKTR selection/resolution has succeeded.
    // No guest/service/loader hook is installed. Base I/O failures propagate;
    // opt-in validation failures return the complete unchanged original view.
    Result CreateVirtualRomfs(const std::shared_ptr<Backing> &effectiveRomfs,
                              const Manifest &manifest, const Options &options = {});
}
