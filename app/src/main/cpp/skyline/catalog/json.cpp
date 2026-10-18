// SPDX-License-Identifier: MPL-2.0
#include "catalog.h"

#include <sstream>

namespace skyline::catalog {
    namespace {
        struct Object {
            std::ostringstream out;
            bool first{true};
            Object() { out << '{'; }
            void Raw(const char *name, const std::string &value) {
                if (!first) out << ',';
                first = false; out << Json(name) << ':' << value;
            }
            void String(const char *name, const std::string &value) { Raw(name, Json(value)); }
            void Number(const char *name, uint64_t value) { Raw(name, std::to_string(value)); }
            void Bool(const char *name, bool value) { Raw(name, value ? "true" : "false"); }
            std::string End() { return out.str() + '}'; }
        };
        template<typename T, typename F> std::string Array(const std::vector<T> &items, F convert) {
            std::string result{"["}; bool first = true;
            for (const auto &item : items) { if (!first) result += ','; first = false; result += convert(item); }
            return result + ']';
        }
        template<typename T> std::string Number(const std::optional<T> &value) { return value ? std::to_string(*value) : "null"; }
        std::string Id(const std::optional<uint64_t> &value) { return value ? Json(Hex64(*value)) : "null"; }
        std::string Issues(const std::vector<Issue> &issues) {
            return Array(issues, [](const Issue &issue) { Object out; out.String("code", issue.code); out.String("detail", issue.detail); return out.End(); });
        }
        std::string MemberId(const NcaDescriptor &nca) { return nca.actualSha256.empty() ? "unverified-location:" + nca.entry : "nca-sha256:" + nca.actualSha256; }
        std::string MemberIds(const std::vector<size_t> &indices, const Catalog &catalog) {
            return Array(indices, [&](size_t i) { return Json(MemberId(catalog.ncas.at(i))); });
        }
        std::string GroupIds(const std::vector<size_t> &indices, const Catalog &catalog) {
            return Array(indices, [&](size_t i) { return Json(catalog.groups.at(i).id); });
        }
        std::string SectionJson(const Section &section) {
            Object out;
            out.Number("index", section.index); out.Number("version", section.version);
            out.Number("fs_type", section.fsType); out.Number("hash_type", section.hashType); out.Number("encryption_type", section.encryptionType);
            out.Number("metadata_hash_type", section.metadataHashType); out.Number("offset", section.offset); out.Number("size", section.size);
            out.Bool("sparse", section.sparse); out.Bool("compressed", section.compressed); out.Bool("requires_base", section.requiresBase);
            out.Number("patch_indirect_offset", section.patchIndirectOffset); out.Number("patch_indirect_size", section.patchIndirectSize);
            out.Number("patch_ctr_ex_offset", section.patchCtrExOffset); out.Number("patch_ctr_ex_size", section.patchCtrExSize);
            out.String("support", section.support); out.String("fs_header_hash_verification", section.headerHash);
            out.String("section_data_hash_verification", "not_performed"); out.Bool("readable_pfs", section.pfs.has_value());
            return out.End();
        }
        std::string NcaJson(const NcaDescriptor &nca) {
            Object out;
            out.String("id", MemberId(nca)); out.String("entry", nca.entry); out.Number("offset", nca.offset); out.Number("size", nca.size);
            out.String("discovery", nca.discovery); out.String("readability", nca.readability); out.String("support", nca.support);
            out.Raw("content_type", Number(nca.contentType)); out.String("content_type_name", nca.contentType ? NcaTypeName(*nca.contentType) : "Unknown");
            out.Raw("program_id", Id(nca.programId));
            out.Raw("filename_content_id_hint", nca.contentIdHint.empty() ? "null" : Json(nca.contentIdHint));
            out.String("content_id_hint_evidence", "filename_only_unverified");
            out.Raw("actual_sha256", nca.actualSha256.empty() ? "null" : Json(nca.actualSha256));
            out.String("signature_verification", nca.signature);
            out.Raw("sections", Array(nca.sections, SectionJson)); out.Raw("issues", Issues(nca.issues));
            return out.End();
        }
        std::string ReferenceJson(const ContentReference &ref, const Catalog &catalog) {
            Object out;
            out.String("content_id", ref.contentId); out.String("expected_sha256", ref.expectedSha256);
            out.String("raw_size_bytes", ref.rawSize); out.Raw("decoded_size", Number(ref.size)); out.Raw("attributes", Number(ref.attributes));
            out.Number("type", ref.type); out.String("type_name", ContentTypeName(ref.type)); out.Number("id_offset", ref.idOffset);
            out.String("resolution", ref.resolution); out.String("reference_consistency", ref.consistency); out.String("hash_verification", ref.hash);
            out.Raw("candidate_members", MemberIds(ref.candidates, catalog));
            out.Raw("member", ref.nca ? Json(MemberId(catalog.ncas.at(*ref.nca))) : "null");
            out.Raw("actual_nca_type", ref.nca ? Number(catalog.ncas.at(*ref.nca).contentType) : "null");
            out.Raw("actual_nca_size", ref.nca ? std::to_string(catalog.ncas.at(*ref.nca).size) : "null");
            return out.End();
        }
        std::string GroupJson(const Group &group, const Catalog &catalog) {
            Object out;
            out.String("id", group.id); out.Raw("title_id", Id(group.titleId)); out.Raw("version", Number(group.version)); out.Raw("type", Number(group.type));
            out.String("type_name", group.type ? MetaTypeName(*group.type) : "Unknown");
            Object header;
            header.Raw("raw_bytes", group.rawHeader.empty() ? "null" : Json(group.rawHeader));
            header.Raw("platform", Number(group.platform)); header.Raw("attributes", Number(group.metaAttributes));
            header.Raw("storage_id", Number(group.storageId)); header.Raw("install_type", Number(group.installType));
            header.Raw("committed_raw", Number(group.committedRaw));
            header.Raw("committed", !group.committedRaw || *group.committedRaw > 1 ? "null" : *group.committedRaw ? "true" : "false");
            header.Raw("required_download_system_version", Number(group.requiredDownloadSystemVersion));
            header.String("field_evidence", "atmosphere_6b0c077_packaged_content_meta_header");
            out.Raw("packaged_header", header.End());
            out.String("source_member", MemberId(catalog.ncas.at(group.nca))); out.String("cnmt_entry", group.cnmtEntry);
            out.String("discovery", group.discovery); out.String("parsing", group.parsing);
            out.String("cnmt_trailer_digest_observation", group.digest);
            out.String("digest_evidence", "untrusted_embedded_digest_not_signature_authentication");
            out.Raw("explicit_application_id", Id(group.applicationId)); out.Raw("patch_id", Id(group.patchId)); out.Raw("data_patch_id", Id(group.dataPatchId)); out.Raw("data_id", Id(group.dataId));
            out.String("parent_state", group.parentState); out.Raw("application_candidates", GroupIds(group.applicationCandidates, catalog));
            out.String("reference_consistency", group.referenceConsistency); out.String("content_hash_verification", group.hashVerification);
            out.String("content_support", group.contentSupport); out.String("runnable", group.runnable);
            out.String("aoc_registration", "not_performed");
            Object layout; layout.String("content_info_profile", ProfileName(catalog.options.profile));
            layout.Number("extended_header_size", group.extendedHeaderSize); layout.Number("content_table_offset", group.contentTableOffset);
            layout.Number("content_meta_table_offset", group.metaTableOffset); layout.Number("extended_data_offset", group.extendedDataOffset);
            layout.Number("extended_data_size", group.extendedDataSize); layout.String("extended_data_support", group.extendedData);
            out.Raw("layout", layout.End());
            out.Raw("contents", Array(group.contents, [&](const auto &ref) { return ReferenceJson(ref, catalog); }));
            out.Raw("content_meta_references", Array(group.metaReferences, [&](const auto &ref) {
                Object item; item.String("title_id", Hex64(ref.titleId)); item.Number("version", ref.version); item.Number("type", ref.type);
                item.String("type_name", MetaTypeName(ref.type)); item.Number("attributes", ref.attributes); item.String("state", ref.state);
                item.Number("resolution_install_type", 0);
                item.String("resolution_install_type_evidence", "content_meta_info_to_key_default_full");
                item.Raw("candidate_groups", GroupIds(ref.candidates, catalog)); return item.End();
            }));
            out.Raw("programs", Array(group.programs, [&](const auto &program) {
                Object item; item.String("content_id", group.contents.at(program.reference).contentId); item.Number("id_offset", program.idOffset);
                item.Raw("program_index", Number(program.programIndex)); item.String("index_evidence", program.indexEvidence); item.Raw("program_id", Id(program.programId));
                item.String("capability", program.capability); item.Bool("requires_base", program.requiresBase); return item.End();
            }));
            out.Raw("issues", Issues(group.issues)); return out.End();
        }
    }

    std::string ToJson(const Catalog &catalog) {
        catalog.CheckUnchanged();
        Object out;
        out.Number("schema_version", 1); out.String("catalog_kind", "readonly_multi_title_nsp");
        out.String("inspector", catalog.inspectorName); out.String("content_info_profile", ProfileName(catalog.options.profile));
        out.String("filename_encoding", "byte_values_as_unicode_u00xx");
        out.Bool("physical_entry_scan_complete", true); out.Bool("bounded_scan_complete", catalog.complete);
        out.Bool("all_metadata_enumerated", catalog.AllMetadataEnumerated());
        out.String("runtime_integration", catalog.runtimeIntegration);
        Object container;
        container.Number("size", catalog.containerSize);
        container.String("identity_kind", catalog.containerSnapshot.empty() ? "unknown" : "open_file_stat_snapshot");
        container.Raw("snapshot_token", catalog.containerSnapshot.empty() ? "null" : Json(catalog.containerSnapshot));
        container.String("identity_scope", "opened_source_stat_and_named_inode_checked_at_scan_and_serialization; not_a_content_hash");
        container.String("whole_container_sha256_verification", "not_performed");
        out.Raw("container", container.End());
        Object verification;
        verification.Bool("whole_nca_hash_requested", catalog.options.verifyHashes); verification.Number("whole_nca_hash_bytes", catalog.hashBytes);
        verification.String("hash_scope", "raw_full_nca_bytes_compared_to_untrusted_cnmt_reference; not_authentication");
        verification.String("signature_verification", "not_verified"); out.Raw("verification", verification.End());
        out.Raw("ncas", Array(catalog.ncas, NcaJson));
        out.Raw("groups", Array(catalog.groups, [&](const auto &group) { return GroupJson(group, catalog); }));
        out.Raw("other_entries", Array(catalog.otherEntries, [](const auto &name) { return Json(name); }));
        out.Raw("issues", Issues(catalog.issues));
        return out.End() + '\n';
    }
}
