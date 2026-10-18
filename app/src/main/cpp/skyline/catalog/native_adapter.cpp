// SPDX-License-Identifier: MPL-2.0
#include "native_adapter.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>
#include <mutex>
#include <loader/loader.h>
#include <vfs/backing.h>
#include <vfs/nca.h>

namespace skyline::catalog {
    class NativeInspectionBudget {
        std::mutex mutex;
        uint64_t reserved{};
      public:
        void Reserve(uint64_t bytes, uint64_t limit) {
            std::scoped_lock lock(mutex);
            const auto next = Add(reserved, bytes);
            if (next > limit) throw Error("scan_budget", "native metadata reads exceed the configured scan budget");
            reserved = next;
        }
    };
    namespace {
        void Require(bool valid, const char *code, const char *detail) {
            if (!valid) throw Error(code, detail);
        }
        size_t NativeSize(uint64_t value) {
            Require(value <= std::numeric_limits<size_t>::max(), "native_size_overflow", "range does not fit the native backing API");
            return static_cast<size_t>(value);
        }
        class BackingReader final : public Reader {
            std::shared_ptr<vfs::Backing> backing;
            Source snapshot;
            uint64_t size;
          public:
            BackingReader(std::shared_ptr<vfs::Backing> pBacking, Source pSnapshot)
                : backing(std::move(pBacking)), snapshot(std::move(pSnapshot)), size(backing ? backing->size : 0) {
                Require(static_cast<bool>(backing), "native_null_backing", "native backing is absent");
                Require(backing->mode.read && backing->HasReliableReadByteCount(), "native_unreliable_backing", "native inspection requires a readable backing with reliable operation byte counts");
            }
            uint64_t Size() const override { return size; }
            size_t Read(uint64_t offset, uint8_t *out, size_t count) const override {
                CheckUnchanged();
                Range(offset, count, size);
                // Backing::Read returns its total size, including after a short
                // read. Only ReadUnchecked returns the actual operation count.
                const auto actual = backing->ReadUnchecked(span<u8>(out, count), NativeSize(offset));
                Require(actual <= count, "native_invalid_read_count", "native backing returned an impossible byte count");
                return actual;
            }
            void CheckUnchanged() const override {
                Require(backing->size == size, "input_changed", "native backing size changed during inspection");
                if (snapshot) snapshot->CheckUnchanged();
            }
            // A guard may watch the ciphertext source while this reader emits
            // decrypted bytes; that guard is never this backing's identity.
            std::string SnapshotToken() const override { return {}; }
        };
        // Existing NCA readers use Backing::Read<T>, which does not throw on
        // short reads. Make every read exact before that legacy API sees it.
        class SourceBacking final : public vfs::Backing {
            Source source;
          protected:
            size_t ReadImpl(span<u8> output, size_t offset) override {
                source->CheckUnchanged();
                source->Exact(offset, output.data(), output.size());
                return output.size();
            }
          public:
            explicit SourceBacking(Source pSource)
                : Backing({true, false, false}, NativeSize(pSource->Size())), source(std::move(pSource)) {}
            bool HasReliableReadByteCount() const noexcept override { return true; }
        };
        template<class T>
        std::vector<uint8_t> BytesOf(const T &value) {
            const auto *bytes = reinterpret_cast<const uint8_t *>(&value);
            return {bytes, bytes + sizeof(T)};
        }
        bool Nonzero(const std::vector<uint8_t> &bytes, size_t offset, size_t size) {
            Range(offset, size, bytes.size());
            return std::any_of(bytes.begin() + offset, bytes.begin() + offset + size, [](auto byte) { return byte != 0; });
        }
        std::string IdHint(const std::string &name) {
            if (!((name.size() == 36 && name.substr(32) == ".nca") ||
                  (name.size() == 41 && name.substr(32) == ".cnmt.nca"))) return {};
            auto id = name.substr(0, 32);
            for (auto &c : id) {
                if (!std::isxdigit(static_cast<unsigned char>(c))) return {};
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            return id;
        }
        class InspectionValidator final : public vfs::NcaReadValidator {
            std::shared_ptr<NcaDescriptor> owner;
            NcaDescriptor &result;
            Options options;
            Source raw;
            std::shared_ptr<NativeInspectionBudget> budget;
          public:
            InspectionValidator(NcaDescriptor &pResult, const Options &pOptions, Source pRaw, std::shared_ptr<NativeInspectionBudget> pBudget)
                : result(pResult), options(pOptions), raw(std::move(pRaw)), budget(std::move(pBudget)) {}
            InspectionValidator(std::shared_ptr<NcaDescriptor> pResult, const Options &pOptions, Source pRaw, std::shared_ptr<NativeInspectionBudget> pBudget)
                : owner(std::move(pResult)), result(*owner), options(pOptions), raw(std::move(pRaw)), budget(std::move(pBudget)) {}
            void ValidateHeader(const vfs::NCAHeader &header, size_t backingSize) const override {
                const auto bytes = BytesOf(header);
                Require(std::memcmp(bytes.data() + 0x200, "NCA3", 4) == 0, "native_invalid_header", "existing backend did not decode an NCA3 header");
                result.readability = "decrypted_header";
                result.contentType = bytes[0x205];
                result.programId = U64(bytes, 0x210);
                Require(U64(bytes, 0x208) == backingSize, "nca_size_mismatch", "NCA header size differs from its PFS member size");
                Require(backingSize >= 0xc00, "truncated_nca_header", "NCA3 filesystem headers require 0xc00 bytes");
                Require(bytes[0x204] <= 1, "unsupported_distribution_type", "unknown NCA distribution type");
                Require(*result.contentType <= 5, "native_content_type_unsupported", "existing backend cannot inspect this NCA content type");
                Require(bytes[0x207] <= 2, "native_key_area_unsupported", "unknown key area selector");
                const auto generation = std::max(bytes[0x206], bytes[0x220]);
                Require(!generation || generation - 1 < 20, "native_key_generation_unsupported", "key generation exceeds the existing backend's supported array");
                bool emptySeen{};
                uint32_t populated{};
                std::vector<std::pair<uint64_t, uint64_t>> extents;
                for (uint32_t i{}; i < 4; ++i) {
                    const auto start = U32(bytes, 0x240 + i * 0x10), end = U32(bytes, 0x244 + i * 0x10);
                    if (!start && !end) { emptySeen = true; continue; }
                    Require(!emptySeen, "native_noncontiguous_sections_unsupported", "existing backend requires populated section slots before empty slots");
                    Require(start >= 6 && end > start, "invalid_nca_section_range", "invalid NCA media-unit interval");
                    const auto offset = Multiply(start, 0x200), size = Multiply(end - start, 0x200);
                    Range(offset, size, backingSize);
                    extents.emplace_back(offset, Add(offset, size));
                    ++populated;
                }
                std::sort(extents.begin(), extents.end());
                for (size_t i{1}; i < extents.size(); ++i)
                    Require(extents[i].first >= extents[i - 1].second, "overlapping_nca_sections", "NCA sections overlap");
                Require(populated != 0, "nca_no_sections", "NCA has no populated filesystem sections");
            }
            void ValidateSections(const vfs::NCAHeader &header, const std::vector<vfs::NCASectionHeader> &sections) const override {
                Require(sections.size() <= 4, "native_section_count_unsupported", "existing backend returned an unsupported section count");
                for (uint32_t i{}; i < sections.size(); ++i) {
                    const auto bytes = BytesOf(sections[i]);
                    Section section;
                    section.index = i;
                    section.offset = Multiply(header.sectionTables[i].mediaOffset, 0x200);
                    section.size = Multiply(header.sectionTables[i].mediaEndOffset - header.sectionTables[i].mediaOffset, 0x200);
                    section.version = U16(bytes, 0); section.fsType = bytes[2]; section.hashType = bytes[3];
                    section.encryptionType = bytes[4]; section.metadataHashType = bytes[5];
                    section.sparse = Nonzero(bytes, 0x148, 0x30); section.compressed = Nonzero(bytes, 0x178, 0x28);
                    section.patchIndirectOffset = U64(bytes, 0x100); section.patchIndirectSize = U64(bytes, 0x108);
                    section.patchCtrExOffset = U64(bytes, 0x120); section.patchCtrExSize = U64(bytes, 0x128);
                    section.requiresBase = bytes[4] == 4 || bytes[4] == 6 || Nonzero(bytes, 0x100, 0x40);
                    const auto hash = Sha256(bytes.data(), bytes.size());
                    section.headerHash = std::equal(hash.begin(), hash.end(), header.hashTables[i].begin()) ? "verified" : "mismatch";
                    section.support = "unsupported";
                    result.sections.push_back(std::move(section));
                    auto &current = result.sections.back();
                    Require(current.headerHash == "verified", "nca_fs_header_hash_mismatch", "decrypted filesystem header differs from the NCA header hash");
                    Require(current.version == 2, "unsupported_nca_fs_version", "native catalog supports filesystem header version 2");
                    Require(!current.sparse, "sparse_unsupported", "native catalog does not support sparse sections");
                    Require(!current.compressed, "compression_unsupported", "native catalog does not support compressed sections");
                    Require(!current.metadataHashType, "metadata_hash_unsupported", "native catalog does not support metadata-hashed sections");
                    Require(!current.requiresBase && !Nonzero(bytes, 0x100, 0x40), "bktr_requires_base", "native catalog does not reconstruct patch indirection or CTR-Ex sections");
                    Require(current.encryptionType == 1 || current.encryptionType == 3, "native_section_encryption_unsupported", "existing backend supports only unencrypted or AES-CTR catalog sections");
                    if (current.fsType == 1 && current.hashType == 2) {
                        const auto blockSize = U32(bytes, 0x28), layers = U32(bytes, 0x2c);
                        Require(blockSize >= 0x200 && !(blockSize & (blockSize - 1)), "invalid_nca_hash_block_size", "invalid hierarchical SHA256 block size");
                        Require(layers == 2, "nca_hash_layers_unsupported", "existing backend requires exactly two SHA256 layers for PFS0");
                        for (uint32_t layer{}; layer < layers; ++layer)
                            Range(U64(bytes, 0x30 + layer * 0x10), U64(bytes, 0x38 + layer * 0x10), current.size);
                        Range(U64(bytes, 0x40), U64(bytes, 0x48), current.size);
                        Require(current.encryptionType != 3 || U64(bytes, 0x40) % 16 == 0,
                                "native_ctr_alignment_unsupported", "existing CTR backing requires an aligned PFS starting offset");
                    } else if (current.fsType == 0 && current.hashType == 3) {
                        Require(std::memcmp(bytes.data() + 8, "IVFC", 4) == 0, "native_invalid_ivfc", "RomFS section does not carry an IVFC header");
                        Require(U32(bytes, 0x14) == 7, "native_ivfc_levels_unsupported", "existing backend requires the complete six-level IVFC layout");
                        for (uint32_t level{}; level < 6; ++level) {
                            Range(U64(bytes, 0x18 + level * 0x18), U64(bytes, 0x20 + level * 0x18), current.size);
                            Require(U32(bytes, 0x28 + level * 0x18) < 32, "native_invalid_ivfc_block_order", "IVFC block order exceeds the supported structure profile");
                            Require(U32(bytes, 0x2c + level * 0x18) == 0, "native_invalid_ivfc_reserved", "nonzero IVFC level reserved field");
                        }
                        Require(current.encryptionType != 3 || U64(bytes, 0x90) % 16 == 0,
                                "native_ctr_alignment_unsupported", "existing CTR backing requires an aligned RomFS starting offset");
                        current.support = "structure_only";
                        result.issues.push_back({"romfs_not_validated", "native catalog inspects RomFS bounds but does not verify its integrity tree or executable capability"});
                    } else throw Error("nca_section_format_unsupported", "native catalog cannot safely bridge this filesystem and hash combination");
                }
            }
            size_t ValidatePfs0(const vfs::NCASectionHeader &section, const vfs::NCASectionTableEntry &entry,
                                const std::shared_ptr<vfs::Backing> &decrypted) const override {
                Require(static_cast<bool>(decrypted), "native_pfs_backing_unavailable", "existing backend did not provide a decrypted PFS backing");
                const auto bytes = BytesOf(section);
                const auto pfsOffset = U64(bytes, 0x40), pfsSize = U64(bytes, 0x48);
                const auto sectionSize = Multiply(entry.mediaEndOffset - entry.mediaOffset, 0x200);
                Range(pfsOffset, pfsSize, sectionSize);
                // Reserve before any decrypted PFS table read, across all NCA
                // members. Three full extents conservatively cover this strict
                // preflight, the legacy PFS parser and the core's second PFS
                // pass plus every CNMT payload. Failed members consume budget.
                budget->Reserve(Multiply(pfsSize, 3), options.limits.maxMetadataBytes);
                // ReadPfs0's legacy RegionBacking length includes the offset
                // within its section. Independently cap it to the last layer.
                auto pfs = Region(std::make_shared<BackingReader>(decrypted, raw), 0, pfsSize);
                (void)ReadPfs(pfs, options.limits);
                auto found = std::find_if(result.sections.begin(), result.sections.end(), [&](const auto &candidate) {
                    return candidate.offset == Multiply(entry.mediaOffset, 0x200);
                });
                Require(found != result.sections.end(), "native_section_not_validated", "decrypted PFS has no validated section descriptor");
                found->pfs = std::move(pfs); found->support = "supported";
                result.issues.push_back({"section_data_hash_not_verified", "PFS bounds and FS header hash are checked; the section data hash hierarchy is not verified"});
                return NativeSize(pfsSize);
            }
        };
        const char *LoaderCode(loader::LoaderResult result) {
            switch (result) {
                case loader::LoaderResult::MissingHeaderKey: return "missing_header_key";
                case loader::LoaderResult::MissingTitleKey: return "missing_title_key";
                case loader::LoaderResult::MissingTitleKek: return "missing_title_kek";
                case loader::LoaderResult::MissingKeyArea: return "missing_key_area";
                case loader::LoaderResult::ErrorSparseNCA: return "sparse_unsupported";
                case loader::LoaderResult::ErrorCompressedNCA: return "compression_unsupported";
                default: return "native_nca_parse_failed";
            }
        }
    }

    Source MakeBackingSource(std::shared_ptr<vfs::Backing> backing) {
        return std::make_shared<BackingReader>(std::move(backing), Source{});
    }
    std::shared_ptr<vfs::Backing> MakeSourceBacking(Source source) {
        Require(static_cast<bool>(source), "native_null_source", "native backing source is absent");
        return std::make_shared<SourceBacking>(std::move(source));
    }
    OpenedNativeNca OpenNativeNca(const PfsEntry &entry, std::shared_ptr<crypto::KeyStore> keyStore, const Options &options) {
        Require(static_cast<bool>(entry.source) && static_cast<bool>(keyStore), "native_missing_input", "native open requires an exact source and provided key store");
        Require(entry.source->Size() == entry.size, "nca_size_mismatch", "NCA source size differs from its member extent");
        entry.source->CheckUnchanged();
        auto budget = std::make_shared<NativeInspectionBudget>();
        budget->Reserve(0x1004, options.limits.maxMetadataBytes);
        const auto prefix = entry.source->Bytes(0, 0x400);
        Require(std::memcmp(prefix.data() + 0x200, "NCA3", 4) != 0, "native_plaintext_unsupported", "the existing plaintext NCA constructor is not used");
        auto descriptor = std::make_shared<NcaDescriptor>();
        descriptor->entry = entry.name; descriptor->offset = entry.offset; descriptor->size = entry.size;
        descriptor->source = entry.source; descriptor->contentIdHint = IdHint(entry.name);
        auto validator = std::make_shared<InspectionValidator>(descriptor, options, entry.source, std::move(budget));
        auto nca = std::make_shared<vfs::NCA>(MakeSourceBacking(entry.source), std::move(keyStore), false, std::move(validator));
        descriptor->support = "metadata_readable";
        entry.source->CheckUnchanged();
        return {std::move(nca), std::move(descriptor)};
    }
    NativeNcaInspector::NativeNcaInspector(std::shared_ptr<crypto::KeyStore> pKeyStore)
        : keyStore(std::move(pKeyStore)), budget(std::make_shared<NativeInspectionBudget>()) {}

    NcaDescriptor NativeNcaInspector::Inspect(const PfsEntry &entry, const Options &options) const {
        NcaDescriptor result;
        result.entry = entry.name; result.offset = entry.offset; result.size = entry.size;
        result.source = entry.source; result.contentIdHint = IdHint(entry.name);
        try {
            Require(static_cast<bool>(entry.source), "native_null_source", "NCA member source is absent");
            Require(entry.source->Size() == entry.size, "nca_size_mismatch", "NCA member source size differs from its PFS entry");
            // Prefix + legacy main header + all four FS headers, including the
            // core's four-byte candidate probe. Share this reservation with all
            // decrypted-PFS passes instead of giving them a second full budget.
            budget->Reserve(0x1004, options.limits.maxMetadataBytes);
            const auto prefix = entry.source->Bytes(0, 0x400);
            if (std::memcmp(prefix.data() + 0x200, "NCA3", 4) == 0)
                throw Error("native_plaintext_unsupported", "use the explicit plaintext inspector for synthetic plaintext NCA3; the legacy native plaintext section loop is not used");
            Require(static_cast<bool>(keyStore), "missing_key_store", "native inspection requires a caller-provided key store");
            auto validator = std::make_shared<InspectionValidator>(result, options, entry.source, budget);
            const vfs::NCA decoded(std::make_shared<SourceBacking>(entry.source), keyStore, false, std::move(validator));
            result.support = "metadata_readable";
            result.issues.push_back({"native_backend_inspection_only", "decryption used the existing Pine NCA backend; RSA signatures, executable capability and runtime selection are not verified"});
        } catch (const Error &error) {
            result.support = "unsupported";
            if (result.readability == "unknown") result.readability = "unreadable";
            result.issues.push_back({error.code, "native catalog validation rejected this member; metadata enumeration is incomplete"});
            for (auto &section : result.sections) section.pfs.reset();
        } catch (const loader::loader_exception &error) {
            result.support = "unsupported";
            if (result.readability == "unknown") result.readability = "unreadable";
            result.issues.push_back({LoaderCode(error.error), "existing NCA backend could not inspect this member; metadata enumeration is incomplete"});
            for (auto &section : result.sections) section.pfs.reset();
        } catch (const std::exception &) {
            result.support = "invalid";
            if (result.readability == "unknown") result.readability = "unreadable";
            result.issues.push_back({"native_nca_backend_failed", "existing NCA backend failed; metadata enumeration is incomplete"});
            for (auto &section : result.sections) section.pfs.reset();
        }
        return result;
    }
    Catalog ScanNative(std::shared_ptr<vfs::Backing> container, std::shared_ptr<crypto::KeyStore> keyStore,
                       const Options &options) {
        return ScanNative(MakeBackingSource(std::move(container)), std::move(keyStore), options);
    }
    Catalog ScanNative(Source container, std::shared_ptr<crypto::KeyStore> keyStore, const Options &options) {
        Require(static_cast<bool>(container), "native_null_source", "native catalog container source is absent");
        return Scan(std::move(container), options, NativeNcaInspector(std::move(keyStore)));
    }
}
