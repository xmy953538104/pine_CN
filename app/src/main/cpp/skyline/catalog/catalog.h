// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "wire.h"
#include <optional>

namespace skyline::catalog {
    enum class ContentInfoProfile { Unknown, Legacy48, Modern40 };
    std::string ProfileName(ContentInfoProfile profile);
    struct Options {
        ContentInfoProfile profile{ContentInfoProfile::Unknown};
        bool verifyHashes{false};
        Limits limits;
    };
    struct Issue { std::string code, detail; };
    struct Section {
        uint32_t index{};
        uint16_t version{};
        uint8_t fsType{}, hashType{}, encryptionType{}, metadataHashType{};
        uint64_t offset{}, size{};
        bool sparse{}, compressed{}, requiresBase{};
        uint64_t patchIndirectOffset{}, patchIndirectSize{}, patchCtrExOffset{}, patchCtrExSize{};
        std::string support{"unknown"};
        std::string headerHash{"unverified"};
        std::optional<Source> pfs;
    };
    struct NcaDescriptor {
        std::string entry;
        uint64_t offset{}, size{};
        std::optional<uint8_t> contentType;
        std::optional<uint64_t> programId;
        std::string discovery{"discovered"}, readability{"unknown"}, support{"unknown"};
        std::string signature{"not_verified"};
        std::string contentIdHint; // filename only; never a verified content id
        std::string actualSha256;
        std::vector<Section> sections;
        std::vector<Issue> issues;
        Source source;
    };
    struct ContentReference {
        std::string contentId, expectedSha256, rawSize;
        std::optional<uint64_t> size;
        std::optional<uint8_t> attributes;
        uint8_t type{}, idOffset{};
        std::vector<size_t> candidates;
        std::optional<size_t> nca;
        std::string resolution{"unresolved"}, consistency{"unknown"}, hash{"unverified"};
    };
    struct MetaReference {
        uint64_t titleId{};
        uint32_t version{};
        uint8_t type{}, attributes{};
        std::vector<size_t> candidates;
        std::string state{"unresolved"};
    };
    struct ProgramDescriptor {
        size_t reference{};
        uint8_t idOffset{};
        std::optional<uint8_t> programIndex;
        std::string indexEvidence{"not_derived_from_id_offset"};
        std::optional<uint64_t> programId;
        std::string capability{"unknown"};
        bool requiresBase{};
    };
    struct Group {
        size_t nca{};
        std::string cnmtEntry;
        std::string id; // SHA256 of the complete CNMT, independent of location
        std::optional<uint64_t> titleId, applicationId, patchId, dataPatchId, dataId;
        std::optional<uint32_t> version;
        std::optional<uint8_t> type;
        std::string discovery{"discovered"}, parsing{"unknown"}, digest{"unverified"};
        std::string parentState{"not_applicable"}, referenceConsistency{"unknown"}, hashVerification{"unverified"};
        std::string contentSupport{"unknown"}, runnable{"unknown"};
        std::string extendedData{"none"};
        uint32_t extendedDataSize{};
        uint16_t extendedHeaderSize{};
        uint64_t contentTableOffset{}, metaTableOffset{}, extendedDataOffset{};
        std::vector<ContentReference> contents;
        std::vector<MetaReference> metaReferences;
        std::vector<ProgramDescriptor> programs;
        std::vector<size_t> applicationCandidates;
        std::vector<Issue> issues;
    };
    struct Catalog {
        Options options;
        uint64_t containerSize{}, hashBytes{};
        std::string containerSnapshot;
        Source source;
        bool complete{true}; // Bounded scan work only; use AllMetadataEnumerated for metadata completeness.
        std::string runtimeIntegration{"not_implemented"};
        std::string inspectorName{"unknown"};
        std::vector<NcaDescriptor> ncas;
        std::vector<Group> groups;
        std::vector<std::string> otherEntries;
        std::vector<Issue> issues;
        void CheckUnchanged() const { if (source) source->CheckUnchanged(); }
        bool AllMetadataEnumerated() const;
    };

    // A legal decoder can replace the plaintext inspector without changing the
    // association core. The decoder owns its key/signature policy; its output
    // must preserve raw ciphertext as `source`, actual byte counts, and all
    // unsupported/encrypted members. P0 never silently substitutes a decoder.
    class NcaInspector {
      public:
        virtual ~NcaInspector() = default;
        virtual std::string Name() const { return "custom"; }
        virtual NcaDescriptor Inspect(const PfsEntry &entry, const Options &options) const = 0;
    };
    class PlaintextNcaInspector final : public NcaInspector {
      public:
        std::string Name() const override { return "plaintext_nca3"; }
        NcaDescriptor Inspect(const PfsEntry &entry, const Options &options) const override;
    };
    Group ParseCnmt(Source source, size_t nca, const std::string &name, const Options &options);
    Catalog Scan(Source container, const Options &options, const NcaInspector &inspector);
    Catalog Scan(Source container, const Options &options = {});
    std::string ToJson(const Catalog &catalog);
    std::string MetaTypeName(uint8_t type);
    std::string ContentTypeName(uint8_t type);
    std::string NcaTypeName(uint8_t type);
}
