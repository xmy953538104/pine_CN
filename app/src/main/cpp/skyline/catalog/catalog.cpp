// SPDX-License-Identifier: MPL-2.0
#include "catalog.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <set>
#include <tuple>

namespace skyline::catalog {
    std::string ProfileName(ContentInfoProfile profile) {
        switch (profile) {
            case ContentInfoProfile::Legacy48: return "legacy-48";
            case ContentInfoProfile::Modern40: return "modern-40";
            default: return "unknown";
        }
    }
    std::string MetaTypeName(uint8_t type) {
        switch (type) {
            case 1: return "SystemProgram"; case 2: return "SystemData"; case 3: return "SystemUpdate";
            case 4: return "BootImagePackage"; case 5: return "BootImagePackageSafe";
            case 0x80: return "Application"; case 0x81: return "Patch"; case 0x82: return "AddOnContent";
            case 0x83: return "Delta"; case 0x84: return "DataPatch"; default: return "Unknown";
        }
    }
    std::string ContentTypeName(uint8_t type) {
        switch (type) {
            case 0: return "Meta"; case 1: return "Program"; case 2: return "Data"; case 3: return "Control";
            case 4: return "HtmlDocument"; case 5: return "LegalInformation"; case 6: return "DeltaFragment";
            default: return "Unknown";
        }
    }
    std::string NcaTypeName(uint8_t type) {
        switch (type) {
            case 0: return "Program"; case 1: return "Meta"; case 2: return "Control";
            case 3: return "Manual"; case 4: return "Data"; case 5: return "PublicData"; default: return "Unknown";
        }
    }
    namespace {
        bool Ends(const std::string &value, const std::string &suffix) {
            return value.size() >= suffix.size() && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
        }
        std::string IdHint(const std::string &name) {
            if (!(name.size() == 36 && Ends(name, ".nca")) && !(name.size() == 41 && Ends(name, ".cnmt.nca"))) return {};
            std::string id = name.substr(0, 32);
            for (auto &c : id) {
                if (!std::isxdigit(static_cast<unsigned char>(c))) return {};
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            return id;
        }
        bool Nonzero(const std::vector<uint8_t> &data, size_t at, size_t size) {
            Range(at, size, data.size());
            return std::any_of(data.begin() + at, data.begin() + at + size, [](uint8_t b) { return b != 0; });
        }
        void Fail(const std::string &code, const std::string &detail) { throw Error(code, detail); }
        void Expect(bool predicate, const std::string &code, const std::string &detail) { if (!predicate) Fail(code, detail); }
        bool TypeMatches(uint8_t reference, uint8_t actual) {
            switch (reference) {
                case 0: return actual == 1;
                case 1: return actual == 0;
                case 2: return actual == 4 || actual == 5;
                case 3: return actual == 2;
                case 4: case 5: return actual == 3;
                default: return false;
            }
        }
    }

    NcaDescriptor PlaintextNcaInspector::Inspect(const PfsEntry &entry, const Options &options) const {
        (void)options;
        NcaDescriptor result;
        result.entry = entry.name; result.offset = entry.offset; result.size = entry.size;
        result.source = entry.source; result.contentIdHint = IdHint(entry.name);
        try {
            const auto header = entry.source->Bytes(0, 0x400);
            if (std::memcmp(header.data() + 0x200, "NCA3", 4)) {
                result.readability = "encrypted_or_unknown";
                result.issues.push_back({"encrypted_header_or_unknown_format", "host inspector has no decryption keys; no CNMT enumeration is claimed"});
                return result;
            }
            result.contentType = header[0x205]; result.programId = U64(header, 0x210);
            result.readability = "plaintext_header";
            Expect(U64(header, 0x208) == entry.size, "nca_size_mismatch", "NCA header size differs from its PFS member size");
            Expect(entry.size >= 0xc00, "truncated_nca_header", "NCA3 filesystem headers require 0xc00 bytes");
            Expect(header[0x204] <= 1, "unsupported_distribution_type", "unknown NCA distribution type");
            bool supported = *result.contentType <= 5;
            std::vector<std::pair<uint64_t, uint64_t>> extents;
            for (uint32_t i{}; i < 4; ++i) {
                auto start = U32(header, 0x240 + i * 0x10), end = U32(header, 0x244 + i * 0x10);
                if (!start && !end) continue;
                Expect(start >= 6 && end > start, "invalid_nca_section_range", "invalid NCA media-unit interval");
                Section section;
                section.index = i; section.offset = Multiply(start, 0x200); section.size = Multiply(end - start, 0x200);
                Range(section.offset, section.size, entry.size);
                extents.emplace_back(section.offset, Add(section.offset, section.size));
                const auto raw = entry.source->Bytes(0x400 + i * 0x200, 0x200);
                const auto actual = Sha256(raw.data(), raw.size());
                section.headerHash = std::equal(actual.begin(), actual.end(), header.begin() + 0x280 + i * 0x20) ? "verified" : "mismatch";
                section.version = U16(raw, 0); section.fsType = raw[2]; section.hashType = raw[3]; section.encryptionType = raw[4]; section.metadataHashType = raw[5];
                section.sparse = Nonzero(raw, 0x148, 0x30); section.compressed = Nonzero(raw, 0x178, 0x28);
                section.patchIndirectOffset = U64(raw, 0x100); section.patchIndirectSize = U64(raw, 0x108);
                section.patchCtrExOffset = U64(raw, 0x120); section.patchCtrExSize = U64(raw, 0x128);
                section.requiresBase = raw[4] == 4 || raw[4] == 6 || section.patchIndirectSize;
                section.support = "unsupported";
                if (section.headerHash == "mismatch") result.issues.push_back({"nca_fs_header_hash_mismatch", "filesystem header does not match the NCA header hash"});
                else if (section.version != 2) result.issues.push_back({"unsupported_nca_fs_version", "only NCA filesystem header version 2 is inspected"});
                else if (section.sparse) result.issues.push_back({"sparse_unsupported", "sparse NCA sections are not supported"});
                else if (section.compressed) result.issues.push_back({"compression_unsupported", "compressed NCA sections are not supported"});
                else if (section.metadataHashType) result.issues.push_back({"metadata_hash_unsupported", "metadata-hashed sections are not supported"});
                else if (Nonzero(raw, 0x100, 0x40)) result.issues.push_back({"patch_info_unsupported", "patch indirection/CTR-Ex information is not reconstructed by the host inspector"});
                else if (section.encryptionType != 1) result.issues.push_back({section.requiresBase ? "bktr_requires_base" : "encrypted_section_unsupported", "host inspector does not decrypt or reconstruct this section"});
                else if (section.fsType == 1 && section.hashType == 2) {
                    auto blockSize = U32(raw, 0x28), layers = U32(raw, 0x2c);
                    Expect(blockSize >= 0x200 && !(blockSize & (blockSize - 1)), "invalid_nca_hash_block_size", "invalid hierarchical SHA256 block size");
                    Expect(layers >= 1 && layers <= 5, "invalid_nca_hash_layer_count", "hierarchical SHA256 layer count is out of range");
                    for (uint32_t layer{}; layer < layers; ++layer)
                        Range(U64(raw, 0x30 + layer * 0x10), U64(raw, 0x38 + layer * 0x10), section.size);
                    if (layers == 2) {
                        auto pfsOffset = U64(raw, 0x40), pfsSize = U64(raw, 0x48);
                        Range(pfsOffset, pfsSize, section.size);
                        auto pfs = Region(entry.source, Add(section.offset, pfsOffset), pfsSize);
                        section.pfs = std::move(pfs); section.support = "supported";
                    } else result.issues.push_back({"nca_hash_layers_unsupported", "host inspector supports exactly two SHA256 layers"});
                } else if (section.fsType == 0 && section.hashType == 3) {
                    section.support = "structure_only";
                    result.issues.push_back({"romfs_not_validated", "RomFS data and executable capability are not validated by the host catalog"});
                } else result.issues.push_back({"nca_section_format_unsupported", "unsupported filesystem or hash type"});
                if (section.support == "unsupported") supported = false;
                result.sections.push_back(std::move(section));
            }
            std::sort(extents.begin(), extents.end());
            for (size_t i{1}; i < extents.size(); ++i)
                Expect(extents[i].first >= extents[i - 1].second, "overlapping_nca_sections", "NCA sections overlap");
            result.support = supported && !result.sections.empty() ? "metadata_readable" : "unsupported";
            if (result.sections.empty()) result.issues.push_back({"nca_no_sections", "NCA has no populated filesystem sections"});
        } catch (const Error &error) {
            result.support = "invalid"; result.readability = result.contentType ? "plaintext_header" : "unreadable";
            result.issues.push_back({error.code, error.what()});
            for (auto &section : result.sections) section.pfs.reset();
        }
        return result;
    }

    Group ParseCnmt(Source source, size_t nca, const std::string &name, const Options &options) {
        Group result; result.nca = nca; result.cnmtEntry = name;
        try {
            Expect(source->Size() <= options.limits.maxCnmtBytes, "scan_budget", "CNMT exceeds configured byte budget");
            const auto bytes = source->Bytes(0, static_cast<size_t>(source->Size()));
            const auto identity = Sha256(bytes.data(), bytes.size());
            result.id = "cnmt-sha256:" + Hex(identity.data(), identity.size());
            Range(0, 0x20, bytes.size());
            result.titleId = U64(bytes, 0); result.version = U32(bytes, 8); result.type = bytes[12];
            result.extendedHeaderSize = U16(bytes, 14);
            auto count = U16(bytes, 16), metaCount = U16(bytes, 18);
            // Platform 0 (Nx) is also the old reserved value; it does not select
            // pre/post-15.0 ContentInfo layout. No title-version heuristic exists.
            Expect(bytes[13] == 0, "unsupported_cnmt_platform", "only Nx CNMT platform is supported");
            Expect(!Nonzero(bytes, 21, 3) && !Nonzero(bytes, 28, 4), "invalid_cnmt_reserved", "nonzero CNMT header reserved bytes");
            result.contentTableOffset = Add(0x20, result.extendedHeaderSize);
            result.metaTableOffset = Add(result.contentTableOffset, Multiply(count, 0x38));
            result.extendedDataOffset = Add(result.metaTableOffset, Multiply(metaCount, 0x10));
            Range(0, result.extendedDataOffset, bytes.size());
            switch (*result.type) {
                case 0x80:
                    Expect(result.extendedHeaderSize == 0x10, "unsupported_extended_header", "Application extended header must be 0x10 bytes");
                    result.patchId = U64(bytes, 0x20);
                    break;
                case 0x81:
                    Expect(result.extendedHeaderSize == 0x18, "unsupported_extended_header", "Patch extended header must be 0x18 bytes");
                    result.applicationId = U64(bytes, 0x20); result.extendedDataSize = U32(bytes, 0x2c);
                    Expect(!Nonzero(bytes, 0x30, 8), "invalid_cnmt_reserved", "nonzero Patch extended header reserved bytes");
                    break;
                case 0x82:
                    Expect(result.extendedHeaderSize == 0x10 || result.extendedHeaderSize == 0x18, "unsupported_extended_header", "AOC extended header must be legacy 0x10 or current 0x18 bytes");
                    result.applicationId = U64(bytes, 0x20);
                    if (result.extendedHeaderSize == 0x18) {
                        result.dataPatchId = U64(bytes, 0x30);
                        Expect(!Nonzero(bytes, 0x2d, 3), "invalid_cnmt_reserved", "nonzero AOC extended header reserved bytes");
                    } else Expect(!Nonzero(bytes, 0x2c, 4), "invalid_cnmt_reserved", "nonzero legacy AOC reserved bytes");
                    break;
                case 0x83:
                    Expect(result.extendedHeaderSize == 0x10, "unsupported_extended_header", "Delta extended header must be 0x10 bytes");
                    result.applicationId = U64(bytes, 0x20); result.extendedDataSize = U32(bytes, 0x28);
                    Expect(!Nonzero(bytes, 0x2c, 4), "invalid_cnmt_reserved", "nonzero Delta reserved bytes");
                    result.contentSupport = "unsupported";
                    result.issues.push_back({"delta_unsupported", "Delta is enumerated but never treated as a complete Program"});
                    break;
                case 0x84:
                    Expect(result.extendedHeaderSize == 0x20, "unsupported_extended_header", "DataPatch extended header must be 0x20 bytes");
                    result.dataId = U64(bytes, 0x20); result.applicationId = U64(bytes, 0x28); result.extendedDataSize = U32(bytes, 0x34);
                    Expect(!Nonzero(bytes, 0x38, 8), "invalid_cnmt_reserved", "nonzero DataPatch reserved bytes");
                    result.contentSupport = "unsupported";
                    result.issues.push_back({"data_patch_unsupported", "DataPatch identity is enumerated but its data patch format is not supported"});
                    break;
                default:
                    result.parsing = "unsupported";
                    result.issues.push_back({"unsupported_cnmt_role", "role-specific extended header is not interpreted"});
                    return result;
            }
            auto digestOffset = Add(result.extendedDataOffset, result.extendedDataSize);
            Expect(Add(digestOffset, 0x20) == bytes.size(), "invalid_cnmt_length", "CNMT tables, declared extended data and mandatory digest do not exactly cover the file");
            const auto digest = Sha256(bytes.data(), static_cast<size_t>(digestOffset));
            result.digest = std::equal(digest.begin(), digest.end(), bytes.begin() + digestOffset) ? "verified" : "mismatch";
            if (result.digest == "mismatch") result.issues.push_back({"cnmt_digest_mismatch", "trailer digest differs from current CNMT bytes; structural parsing is independent of this observation"});
            if (result.extendedDataSize) {
                result.extendedData = "opaque_unsupported";
                result.contentSupport = "unsupported";
                result.issues.push_back({"extended_data_unsupported", "extended data extent is validated but its format is not interpreted"});
            }
            if (options.profile == ContentInfoProfile::Unknown) result.issues.push_back({"content_info_layout_unknown", "size bytes are retained without assuming legacy size48 or modern size40 plus attributes"});
            Expect(count <= options.limits.maxContentReferences && metaCount <= options.limits.maxMetaReferences,
                   "scan_budget", "CNMT reference count exceeds remaining configured output budget");
            std::set<std::string> ids;
            for (uint16_t i{}; i < count; ++i) {
                auto at = static_cast<size_t>(Add(result.contentTableOffset, Multiply(i, 0x38)));
                ContentReference info;
                info.expectedSha256 = Hex(bytes.data() + at, 32); info.contentId = Hex(bytes.data() + at + 32, 16);
                info.rawSize = Hex(bytes.data() + at + 48, 6);
                if (options.profile != ContentInfoProfile::Unknown) {
                    info.size = Little(bytes, at + 48, options.profile == ContentInfoProfile::Legacy48 ? 6 : 5);
                    if (options.profile == ContentInfoProfile::Modern40) info.attributes = bytes[at + 53];
                }
                info.type = bytes[at + 54]; info.idOffset = bytes[at + 55];
                if (!ids.insert(info.contentId).second) result.issues.push_back({"duplicate_content_reference", "same Content ID is referenced more than once in one CNMT"});
                if (info.type > 6 || info.type == 6) {
                    result.contentSupport = "unsupported";
                    result.issues.push_back({info.type == 6 ? "delta_fragment_unsupported" : "unknown_content_type", "content type cannot be used as a complete Program or data member"});
                }
                result.contents.push_back(std::move(info));
            }
            for (uint16_t i{}; i < metaCount; ++i) {
                auto at = static_cast<size_t>(Add(result.metaTableOffset, Multiply(i, 0x10)));
                Expect(U16(bytes, at + 14) == 0, "invalid_cnmt_reserved", "nonzero ContentMetaInfo reserved bytes");
                MetaReference info;
                info.titleId = U64(bytes, at); info.version = U32(bytes, at + 8); info.type = bytes[at + 12]; info.attributes = bytes[at + 13];
                result.metaReferences.push_back(info);
            }
            result.parsing = "parsed";
        } catch (const Error &error) {
            result.parsing = error.code == "scan_budget" ? "not_inspected" : "invalid";
            result.referenceConsistency = error.code == "scan_budget" ? "unknown" : "invalid";
            result.contentSupport = error.code == "scan_budget" ? "unknown" : "invalid";
            result.issues.push_back({error.code, error.what()});
        }
        return result;
    }

    Catalog Scan(Source container, const Options &options, const NcaInspector &inspector) {
        Catalog result; result.options = options; result.containerSize = container->Size();
        result.containerSnapshot = container->SnapshotToken(); result.source = container; result.inspectorName = inspector.Name();
        auto entries = ReadPfs(container, options.limits);
        // Sorting is for reproducible presentation only. No association or
        // selection decision uses physical order or a first/last candidate.
        std::sort(entries.begin(), entries.end(), [](const auto &a, const auto &b) { return a.name < b.name; });
        uint64_t metadataBytes{}, hashBytes{};
        for (const auto &entry : entries) {
            // An encrypted header has no visible magic. An arbitrary filename
            // cannot exclude an NCA: inspect every member large enough for an
            // NCA3 header, and retain failures as unknown candidates.
            bool isNca = Ends(entry.name, ".nca") || entry.size >= 0xc00;
            if (entry.size >= 0x204) {
                const auto magic = entry.source->Bytes(0x200, 4);
                isNca = isNca || std::memcmp(magic.data(), "NCA", 3) == 0;
            }
            if (!isNca) { result.otherEntries.push_back(entry.name); continue; }
            hashBytes = Add(hashBytes, entry.size);
            metadataBytes = Add(metadataBytes, std::min<uint64_t>(entry.size, 0xc00));
            if (metadataBytes > options.limits.maxMetadataBytes) {
                NcaDescriptor skipped; skipped.entry = entry.name; skipped.offset = entry.offset; skipped.size = entry.size;
                skipped.source = entry.source; skipped.contentIdHint = IdHint(entry.name); skipped.readability = "not_inspected";
                skipped.issues.push_back({"scan_budget", "NCA header scan exceeded metadata budget"});
                result.ncas.push_back(std::move(skipped)); result.complete = false;
                continue;
            }
            try { result.ncas.push_back(inspector.Inspect(entry, options)); }
            catch (const std::exception &error) {
                NcaDescriptor failed; failed.entry = entry.name; failed.offset = entry.offset; failed.size = entry.size;
                failed.source = entry.source; failed.contentIdHint = IdHint(entry.name); failed.readability = "unreadable";
                const auto *wireError = dynamic_cast<const Error *>(&error);
                failed.issues.push_back({wireError ? wireError->code : "nca_inspector_failed", "NCA inspector failed; metadata enumeration for this member is incomplete"});
                result.ncas.push_back(std::move(failed)); result.complete = false;
            }
        }
        if (options.verifyHashes && hashBytes <= options.limits.maxHashBytes) {
            for (auto &nca : result.ncas) {
                try {
                    const auto hash = Sha256(*nca.source); nca.actualSha256 = Hex(hash.data(), hash.size());
                    result.hashBytes = Add(result.hashBytes, nca.size);
                } catch (const Error &error) {
                    nca.issues.push_back({error.code, error.what()}); result.complete = false;
                }
            }
        } else if (options.verifyHashes) {
            result.complete = false;
            result.issues.push_back({"hash_budget", "entire NCA set exceeds hash budget; no member hash was attempted"});
        }

        uint32_t cnmtFiles{};
        uint64_t contentReferences{}, metaReferences{};
        for (size_t i{}; i < result.ncas.size(); ++i) {
            auto &nca = result.ncas[i];
            if (!nca.contentType || *nca.contentType != 1) continue;
            bool anyPfs{}, anyCnmt{};
            for (const auto &section : nca.sections) {
                if (!section.pfs) continue;
                anyPfs = true;
                try {
                    metadataBytes = Add(metadataBytes, (*section.pfs)->Size());
                    if (metadataBytes > options.limits.maxMetadataBytes) throw Error("scan_budget", "Meta PFS exceeds cumulative metadata budget");
                    for (const auto &entry : ReadPfs(*section.pfs, options.limits)) {
                        // CNMT has no magic. Names are location hints, so every
                        // sufficiently large Meta-PFS member is a candidate.
                        if (!Ends(entry.name, ".cnmt") && entry.size < 0x20) {
                            nca.issues.push_back({"unclassified_meta_member", "small Meta PFS member is not a recognized CNMT candidate"});
                            continue;
                        }
                        anyCnmt = true; ++cnmtFiles;
                        metadataBytes = Add(metadataBytes, entry.size);
                        if (cnmtFiles > options.limits.maxCnmtFiles || metadataBytes > options.limits.maxMetadataBytes || entry.size > options.limits.maxCnmtBytes) {
                            Group skipped; skipped.nca = i; skipped.cnmtEntry = entry.name; skipped.parsing = "not_inspected";
                            skipped.issues.push_back({"scan_budget", "CNMT count, reference count or byte budget exceeded; remainder of this PFS was not enumerated"});
                            result.groups.push_back(std::move(skipped)); result.complete = false;
                            break;
                        } else {
                            auto remaining = options;
                            remaining.limits.maxContentReferences -= static_cast<uint32_t>(contentReferences);
                            remaining.limits.maxMetaReferences -= static_cast<uint32_t>(metaReferences);
                            auto parsed = ParseCnmt(entry.source, i, entry.name, remaining);
                            contentReferences = Add(contentReferences, parsed.contents.size());
                            metaReferences = Add(metaReferences, parsed.metaReferences.size());
                            if (parsed.parsing == "not_inspected") result.complete = false;
                            result.groups.push_back(std::move(parsed));
                        }
                    }
                } catch (const Error &error) { nca.issues.push_back({error.code, error.what()}); nca.support = "invalid"; result.complete = false; }
            }
            if (!anyPfs) nca.issues.push_back({"meta_cnmt_unreadable", "Meta NCA has no supported readable PFS; its CNMTs have not been enumerated"});
            else if (!anyCnmt) nca.issues.push_back({"meta_has_no_cnmt", "no .cnmt member was found in any readable Meta PFS"});
        }
        std::sort(result.groups.begin(), result.groups.end(), [](const auto &a, const auto &b) {
            return std::tie(a.titleId, a.version, a.type, a.id, a.cnmtEntry) < std::tie(b.titleId, b.version, b.type, b.id, b.cnmtEntry);
        });

        // Bound candidate fan-out as well as the input bytes. A tiny duplicate
        // NCA repeated many times must not expand into billions of reference
        // links. Rejection leaves discovered groups intact and unselected.
        uint64_t contentCount{}, metaCount{}, parentCount{};
        for (const auto &group : result.groups) {
            contentCount = Add(contentCount, group.contents.size()); metaCount = Add(metaCount, group.metaReferences.size());
            if (group.applicationId) ++parentCount;
        }
        const auto associationWork = Add(Multiply(result.ncas.size(), contentCount), Multiply(result.groups.size(), Add(metaCount, parentCount)));
        if (associationWork > options.limits.maxAssociationWork) {
            result.complete = false;
            result.issues.push_back({"association_budget", "candidate fan-out exceeds configured work/output budget; references remain unresolved and no content is selected"});
            container->CheckUnchanged();
            return result;
        }

        std::map<std::string, std::set<size_t>> contentCandidates;
        std::map<std::string, std::set<size_t>> hashCandidates;
        for (size_t i{}; i < result.ncas.size(); ++i) {
            const auto &nca = result.ncas[i];
            if (!nca.contentIdHint.empty()) contentCandidates[nca.contentIdHint].insert(i);
            if (!nca.actualSha256.empty()) hashCandidates[nca.actualSha256].insert(i);
        }
        for (const auto &[id, candidates] : contentCandidates) if (candidates.size() > 1) {
            result.issues.push_back({"duplicate_content_id", "multiple NCA members claim or hash to Content ID " + id});
            for (auto index : candidates) result.ncas[index].issues.push_back({"duplicate_content_id", "Content ID has multiple members; no member is implicitly selected"});
        }
        std::map<std::tuple<uint64_t, uint32_t, uint8_t>, std::vector<size_t>> metaCandidates;
        for (size_t i{}; i < result.groups.size(); ++i) {
            const auto &group = result.groups[i];
            if (group.parsing == "parsed" && group.titleId && group.version && group.type)
                metaCandidates[{*group.titleId, *group.version, *group.type}].push_back(i);
        }
        for (auto &[key, candidates] : metaCandidates) if (candidates.size() > 1) {
            (void)key;
            for (auto index : candidates) result.groups[index].issues.push_back({"metadata_ambiguity", "more than one CNMT has this title, version and role"});
        }
        for (auto &group : result.groups) {
            if (group.parsing != "parsed") continue;
            bool invalid{}, unknown{}, unsupported = group.contentSupport == "unsupported";
            bool allVerified = true, mismatch{};
            for (const auto &issue : group.issues)
                if (issue.code == "duplicate_content_reference" || issue.code == "metadata_ambiguity") invalid = true;
            for (size_t r{}; r < group.contents.size(); ++r) {
                auto &ref = group.contents[r];
                const auto found = contentCandidates.find(ref.contentId);
                std::set<size_t> candidates;
                if (found != contentCandidates.end()) candidates = found->second;
                const auto hashFound = hashCandidates.find(ref.expectedSha256);
                if (hashFound != hashCandidates.end()) candidates.insert(hashFound->second.begin(), hashFound->second.end());
                ref.candidates.assign(candidates.begin(), candidates.end());
                if (ref.candidates.empty()) {
                    ref.resolution = "missing"; ref.consistency = "missing"; invalid = true;
                    group.issues.push_back({"missing_content", "no NCA member resolves Content ID " + ref.contentId});
                } else if (ref.candidates.size() != 1) {
                    ref.resolution = "ambiguous"; ref.consistency = "ambiguous"; invalid = true;
                    group.issues.push_back({"ambiguous_content", "Content ID resolves to multiple NCA members"});
                } else {
                    ref.nca = ref.candidates.front(); const auto &nca = result.ncas[*ref.nca];
                    ref.resolution = nca.actualSha256.empty() ? "filename_hint_only" : nca.actualSha256 == ref.expectedSha256 ? "sha256_match" : "filename_hint_only";
                    bool bad = nca.support == "invalid";
                    bool unproven = !ref.size || !nca.contentType;
                    if (ref.size && *ref.size != nca.size) { bad = true; group.issues.push_back({"content_size_mismatch", "CNMT declared size differs from the referenced NCA size"}); }
                    if (nca.contentType && !TypeMatches(ref.type, *nca.contentType)) { bad = true; group.issues.push_back({"content_type_mismatch", "CNMT role does not match the actual NCA header content type"}); }
                    if (!nca.actualSha256.empty()) ref.hash = nca.actualSha256 == ref.expectedSha256 ? "verified" : "mismatch";
                    ref.consistency = bad ? "invalid" : unproven ? "unknown" : "consistent";
                    invalid = invalid || bad; unknown = unknown || unproven;
                    if (nca.support != "metadata_readable") unsupported = true;
                    if (ref.attributes && *ref.attributes) {
                        unsupported = true; group.issues.push_back({"content_attributes_unsupported", "modern content attributes are retained but not interpreted"});
                    }
                    if (*group.type == 0x82 && ref.type == 2 && nca.contentType && *nca.contentType == 4) {
                        unsupported = true; group.issues.push_back({"aoc_data_runtime_unsupported", "AOC references Data NCA; current runtime storage path expects PublicData"});
                    }
                }
                allVerified = allVerified && ref.hash == "verified"; mismatch = mismatch || ref.hash == "mismatch";
                if (ref.type == 1) {
                    ProgramDescriptor program; program.reference = r; program.idOffset = ref.idOffset;
                    program.programIndex = ref.idOffset; program.indexEvidence = "cnmt_program_id_offset";
                    if (ref.nca) {
                        const auto &nca = result.ncas[*ref.nca]; program.programId = nca.programId;
                        program.requiresBase = std::any_of(nca.sections.begin(), nca.sections.end(), [](const auto &s) { return s.requiresBase; });
                        program.capability = nca.support == "metadata_readable" ? "not_assessed" : "unsupported";
                    }
                    group.programs.push_back(program);
                }
            }
            for (auto &ref : group.metaReferences) {
                const auto found = metaCandidates.find({ref.titleId, ref.version, ref.type});
                if (found != metaCandidates.end()) ref.candidates = found->second;
                ref.state = ref.candidates.empty() ? "missing" : ref.candidates.size() == 1 ? "resolved" : "ambiguous";
                if (ref.state != "resolved") { invalid = true; group.issues.push_back({"meta_reference_" + ref.state, "ContentMetaInfo does not resolve to exactly one parsed CNMT"}); }
            }
            if (group.applicationId) {
                for (size_t i{}; i < result.groups.size(); ++i) {
                    const auto &candidate = result.groups[i];
                    if (candidate.parsing == "parsed" && candidate.type && *candidate.type == 0x80 && candidate.titleId == group.applicationId)
                        group.applicationCandidates.push_back(i);
                }
                group.parentState = !*group.applicationId ? "invalid" : group.applicationCandidates.empty() ? "missing" : group.applicationCandidates.size() == 1 ? "consistent" : "ambiguous";
                if (group.parentState == "consistent" && *group.type == 0x81) {
                    const auto &app = result.groups[group.applicationCandidates.front()];
                    if (app.patchId && *app.patchId && app.patchId != group.titleId) {
                        group.parentState = "mismatch"; group.issues.push_back({"patch_parent_mismatch", "Application patch_id does not point to this Patch"});
                    }
                }
                if (group.parentState != "consistent") group.issues.push_back({"explicit_parent_" + group.parentState, "explicit application_id does not establish a unique matching Application group in this container"});
            }
            group.referenceConsistency = invalid ? "invalid" : unknown ? "unknown" : "consistent";
            group.hashVerification = mismatch ? "mismatch" : allVerified && !group.contents.empty() ? "verified" : "unverified";
            group.contentSupport = unsupported ? "unsupported" : options.profile == ContentInfoProfile::Unknown ? "unknown" : "structure_only";
            if (*group.type == 0x80 || *group.type == 0x81) {
                if (group.programs.empty()) { group.runnable = "missing_program"; group.issues.push_back({"missing_program", "CNMT does not contain a Program reference"}); }
                else if (group.programs.size() > 1) { group.runnable = "ambiguous"; group.issues.push_back({"multiple_programs", "explicit Program selection is required; no index is selected automatically"}); }
                else if (*group.type == 0x81 || group.programs.front().requiresBase) group.runnable = "requires_base_or_runtime_validation";
                else group.runnable = "not_assessed";
            } else if (*group.type == 0x82) {
                group.runnable = "not_a_program";
                if (std::none_of(group.contents.begin(), group.contents.end(), [](const auto &r) { return r.type == 2; })) {
                    group.referenceConsistency = "invalid"; group.issues.push_back({"missing_aoc_data", "AOC has no Data content reference (Data or PublicData NCA)"});
                }
            } else group.runnable = "unsupported";
        }
        // The caller can retain this Source and must recheck before a future
        // launch/selection. A successful scan does not pin mutable file bytes.
        container->CheckUnchanged();
        return result;
    }
    Catalog Scan(Source container, const Options &options) { return Scan(std::move(container), options, PlaintextNcaInspector{}); }

    bool Catalog::AllMetadataEnumerated() const {
        if (!complete || !otherEntries.empty()) return false;
        for (const auto &nca : ncas) {
            if (!nca.contentType || *nca.contentType > 5) return false;
            for (const auto &issue : nca.issues)
                if (issue.code == "meta_cnmt_unreadable" || issue.code == "meta_has_no_cnmt" || issue.code == "unclassified_meta_member") return false;
            if (*nca.contentType == 1) for (const auto &section : nca.sections) if (!section.pfs) return false;
        }
        for (const auto &group : groups) if (group.parsing != "parsed") return false;
        return true;
    }
}
