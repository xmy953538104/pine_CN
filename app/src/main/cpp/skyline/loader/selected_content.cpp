// SPDX-License-Identifier: MPL-2.0
#include "selected_content.h"
#include "nca.h"
#include <catalog/native_adapter.h>
#include <diagnostics/content.h>
#include <kernel/types/KProcess.h>
#include <os.h>
#include <services/aocsrv/content_id.h>
#include <vfs/npdm.h>
#include <vfs/patch_manager.h>
#include <vfs/rom_filesystem.h>
#include <set>

namespace skyline::loader {
    namespace {
        using namespace catalog;
        void Need(bool valid, const char *code) { if (!valid) throw Error(code, "explicit selected content validation failed"); }
        std::string Hash(Source source) { source->CheckUnchanged(); const auto hash = Sha256(*source); return Hex(hash.data(), hash.size()); }
        bool Issue(const std::vector<catalog::Issue> &issues, const char *code) {
            return std::any_of(issues.begin(), issues.end(), [&](const auto &issue) { return issue.code == code; });
        }
        const Group &GroupById(const Catalog &catalog, const std::string &id) {
            const Group *match{};
            for (const auto &group : catalog.groups) if (group.id == id) { Need(!match, "selected_group_ambiguous"); match = &group; }
            Need(match != nullptr, "selected_group_missing"); return *match;
        }
        void ValidateGroup(const Catalog &catalog, const Group &group, uint8_t role) {
            Need(group.parsing == "parsed" && group.type == role && group.titleId && group.version && group.digest == "verified",
                 "selected_group_role_or_digest");
            Need(group.installType == 0 && group.platform == 0 && group.extendedData == "none" && group.metaAttributes == 0 &&
                 group.storageId && *group.storageId <= 6 && group.committedRaw && *group.committedRaw <= 1,
                 "selected_metadata_profile_unsupported");
            Need(!Issue(group.issues, "metadata_ambiguity") && !Issue(group.issues, "duplicate_content_reference"), "selected_metadata_ambiguous");
            Need(!group.contents.empty(), "selected_group_has_no_content");
            for (const auto &ref : group.contents) {
                Need(role != 0x82 || ref.type == 2, "selected_aoc_reference_type_unsupported");
                Need(ref.nca && ref.candidates.size() == 1 && ref.hash == "verified" && ref.consistency == "consistent" && ref.size,
                     "selected_content_reference_unverified");
                const auto &nca = catalog.ncas.at(*ref.nca);
                Need(nca.source && nca.size == *ref.size && nca.actualSha256 == ref.expectedSha256 && nca.contentType,
                     "selected_content_reference_mismatch");
                Need(!ref.attributes || !*ref.attributes, "selected_content_attributes_unsupported");
                Need(ref.type <= 5, "selected_content_type_unsupported");
                Need(!Issue(nca.issues, "duplicate_content_id"), "selected_duplicate_content_id");
                if (std::any_of(nca.sections.begin(), nca.sections.end(), [](const auto &section) { return section.requiresBase; }))
                    throw Error("selected_bktr_unsupported", "legacy BKTR arbitrary boundary reads do not satisfy this selected structural profile");
                Need(nca.support == "metadata_readable", "selected_nca_profile_unsupported");
            }
            for (const auto &meta : group.metaReferences)
                Need(meta.state == "resolved" && meta.candidates.size() == 1, "selected_meta_reference_unresolved");
        }
        Source CnmtSource(const Catalog &catalog, const Group &group) {
            Source match;
            const auto &meta = catalog.ncas.at(group.nca);
            for (const auto &section : meta.sections) if (section.pfs) {
                for (const auto &entry : ReadPfs(*section.pfs, catalog.options.limits)) {
                    if (entry.name != group.cnmtEntry || "cnmt-sha256:" + Hash(entry.source) != group.id) continue;
                    Need(!match, "selected_cnmt_member_ambiguous"); match = entry.source;
                }
            }
            Need(static_cast<bool>(match), "selected_cnmt_member_changed"); return match;
        }
        class SourceDirectory final : public vfs::Directory {
            std::vector<Entry> entries;
          public:
            SourceDirectory(std::vector<Entry> values, ListMode mode) : Directory(mode), entries(std::move(values)) {}
            std::vector<Entry> Read() override { return listMode.file ? entries : std::vector<Entry>{}; }
        };
        class SourcePfs final : public vfs::FileSystem {
            std::map<std::string, Source> files;
          protected:
            std::shared_ptr<vfs::Backing> OpenFileImpl(const std::string &path, vfs::Backing::Mode mode) override {
                Need(mode.read && !mode.write && !mode.append, "selected_read_only");
                const auto found = files.find(path); return found == files.end() ? nullptr : MakeSourceBacking(found->second);
            }
            std::optional<vfs::Directory::EntryType> GetEntryTypeImpl(const std::string &path) override {
                return files.count(path) ? std::optional(vfs::Directory::EntryType::File) : std::nullopt;
            }
            std::shared_ptr<vfs::Directory> OpenDirectoryImpl(const std::string &path, vfs::Directory::ListMode mode) override {
                if (!path.empty()) return nullptr;
                std::vector<vfs::Directory::Entry> entries;
                for (const auto &[name, file] : files) entries.push_back({name, vfs::Directory::EntryType::File, static_cast<size_t>(file->Size())});
                return std::make_shared<SourceDirectory>(std::move(entries), mode);
            }
          public:
            explicit SourcePfs(Source source) { for (const auto &entry : ReadPfs(source, {})) files.emplace(entry.name, entry.source); }
        };
        struct RuntimeNca { std::shared_ptr<vfs::NCA> nca; Source exefs, romfs; selected::Romfs tree; };
        RuntimeNca Open(const Catalog &catalog, size_t index, const std::shared_ptr<crypto::KeyStore> &keys, const selected::Limits &limits) {
            const auto &member = catalog.ncas.at(index);
            PfsEntry entry{member.entry, member.offset, member.size, member.source};
            auto opened = OpenNativeNca(entry, keys, catalog.options);
            Need(opened.nca->contentType == static_cast<vfs::NCAContentType>(*member.contentType) &&
                 opened.nca->header.titleId == member.programId, "selected_native_header_changed");
            RuntimeNca result; result.nca = std::move(opened.nca);
            for (const auto &section : opened.descriptor->sections) if (section.pfs) {
                const auto members = ReadPfs(*section.pfs, catalog.options.limits);
                const bool hasMain = std::any_of(members.begin(), members.end(), [](const auto &file) { return file.name == "main"; });
                const bool hasNpdm = std::any_of(members.begin(), members.end(), [](const auto &file) { return file.name == "main.npdm"; });
                if (!hasMain && !hasNpdm) continue;
                Need(!result.exefs && hasMain && hasNpdm, "selected_exefs_ambiguous_or_partial");
                result.exefs = *section.pfs;
            }
            const auto romfsCount = std::count_if(opened.descriptor->sections.begin(), opened.descriptor->sections.end(), [](const auto &section) {
                return section.fsType == 0 && section.hashType == 3;
            });
            Need(romfsCount <= 1, "selected_romfs_ambiguous");
            if (result.nca->romFs) {
                result.romfs = MakeBackingSource(result.nca->romFs);
                result.tree = selected::ValidateRomfs(result.romfs, limits);
                result.nca->romFs = MakeSourceBacking(result.romfs);
            }
            if (result.exefs) result.nca->exeFs = std::make_shared<SourcePfs>(result.exefs);
            return result;
        }
        const ContentReference &Program(const Catalog &catalog, const Group &group, const ProgramChoice &choice) {
            Need(!choice.contentId.empty() && choice.programIndex && choice.programId, "selected_program_selector_missing");
            const ContentReference *match{};
            std::set<uint8_t> indices;
            for (const auto &ref : group.contents) if (ref.type == 1) {
                Need(indices.insert(ref.idOffset).second, "selected_duplicate_program_index");
                if (ref.contentId == choice.contentId && ref.idOffset == *choice.programIndex) { Need(!match, "selected_program_ambiguous"); match = &ref; }
            }
            Need(match && match->nca && catalog.ncas.at(*match->nca).programId == choice.programId, "selected_program_identity_mismatch");
            return *match;
        }
        const ContentReference &OnlyReference(const Group &group, uint8_t type, const char *code) {
            const ContentReference *match{};
            for (const auto &ref : group.contents) if (ref.type == type) { Need(!match, code); match = &ref; }
            Need(match != nullptr, code); return *match;
        }
    }

    std::shared_ptr<SelectedContent> PrepareSelectedContent(const catalog::Catalog &previous, const SelectionRequest &request,
                                                          std::shared_ptr<crypto::KeyStore> providedKeys) {
        using namespace catalog;
        Need(previous.source && providedKeys && request.applicationId && request.profile != ContentInfoProfile::Unknown &&
             request.profile == previous.options.profile, "selected_explicit_source_profile_and_keys_required");
        previous.CheckUnchanged();
        auto options = previous.options; options.verifyHashes = true;
        auto fresh = ScanNative(previous.source, providedKeys, options);
        Need(fresh.complete, "selected_scan_incomplete");
        Need(fresh.AllMetadataEnumerated(), "selected_unclassified_or_unreadable_metadata");
        for (const auto &nca : fresh.ncas) {
            Need(nca.contentType && *nca.contentType <= 5, "selected_unknown_nca_may_hide_metadata");
            Need(!Issue(nca.issues, "duplicate_content_id"), "selected_duplicate_content_id");
            if (*nca.contentType == 1) Need(!Issue(nca.issues, "meta_cnmt_unreadable") && !Issue(nca.issues, "meta_has_no_cnmt") &&
                                          !Issue(nca.issues, "unclassified_meta_member"), "selected_unknown_metadata");
        }
        for (const auto &group : fresh.groups) Need(group.parsing == "parsed" && !Issue(group.issues, "metadata_ambiguity"), "selected_unknown_or_ambiguous_metadata");
        auto result = std::make_shared<SelectedContent>(); result->owner = previous.source; result->keys = providedKeys;
        std::set<std::string> groupIds;
        std::set<size_t> bound;
        auto group = [&](const std::string &id, uint8_t role) -> const Group & {
            Need(groupIds.insert(id).second, "selected_group_reused");
            (void)GroupById(previous, id);
            const auto &value = GroupById(fresh, id); ValidateGroup(fresh, value, role);
            for (size_t index : {value.nca}) if (bound.insert(index).second)
                result->bindings.push_back({fresh.ncas.at(index).source, fresh.ncas.at(index).actualSha256});
            for (const auto &ref : value.contents) if (bound.insert(*ref.nca).second)
                result->bindings.push_back({fresh.ncas.at(*ref.nca).source, ref.expectedSha256});
            return value;
        };
        auto make = [&](const Group &metadata, const ProgramChoice *choice, bool primary) {
            auto loader = std::shared_ptr<SelectedContentLoader>(new SelectedContentLoader(result->owner));
            loader->selectedGroupId = metadata.id; loader->primary = primary;
            loader->cnmt.emplace(MakeSourceBacking(CnmtSource(fresh, metadata)));
            Need(loader->cnmt->header.id == metadata.titleId && static_cast<uint8_t>(loader->cnmt->GetContentMetaType()) == metadata.type,
                 "selected_cnmt_identity_changed");
            if (choice) {
                const auto &reference = Program(fresh, metadata, *choice);
                auto program = Open(fresh, *reference.nca, providedKeys, request.limits);
                Need(program.exefs && program.romfs, "selected_program_not_self_contained");
                selected::ValidateExeFs(program.exefs, *choice->programId, request.limits);
                // Exercise the actual NPDM parser only after safe capability and
                // identity preflight; no process or guest thread starts here.
                const vfs::NPDM parsed(program.nca->exeFs->OpenFile("main.npdm"));
                Need(parsed.aci0.programId == choice->programId, "selected_npdm_program_id_mismatch");
                loader->programNca.emplace(*program.nca); loader->romFs = program.nca->romFs;
                loader->selectedProgramIndex = choice->programIndex;
                loader->diagnosticProgramContentId = reference.contentId;
                const auto &controlRef = OnlyReference(metadata, 3, "selected_control_missing_or_ambiguous");
                auto control = Open(fresh, *controlRef.nca, providedKeys, request.limits);
                Need(control.romfs && control.tree.files.count("control.nacp"), "selected_control_nacp_missing");
                selected::ValidateNacp(control.tree.files.at("control.nacp"));
                loader->controlFiles = std::make_shared<vfs::RomFileSystem>(MakeSourceBacking(control.romfs));
                loader->nacp.emplace(loader->controlFiles->OpenFile("control.nacp")); loader->controlNca.emplace(*control.nca);
            } else {
                const auto &data = OnlyReference(metadata, 2, "selected_public_data_missing_or_ambiguous");
                Need(fresh.ncas.at(*data.nca).contentType == 5, "selected_aoc_requires_public_data");
                auto publicData = Open(fresh, *data.nca, providedKeys, request.limits);
                Need(publicData.romfs && publicData.nca->header.titleId == metadata.titleId, "selected_aoc_data_id_mismatch");
                loader->publicNca.emplace(*publicData.nca); loader->romFs = publicData.nca->romFs;
            }
            return loader;
        };
        const auto primaryRole = request.mode == SelectedMode::Application ? uint8_t{0x80} : uint8_t{0x81};
        const auto &primaryGroup = group(request.primary.groupId, primaryRole);
        if (request.mode == SelectedMode::Application) Need(primaryGroup.titleId == request.applicationId, "selected_application_id_mismatch");
        else Need(!request.update && primaryGroup.applicationId == request.applicationId, "selected_standalone_patch_parent_mismatch");
        result->primary = make(primaryGroup, &request.primary, true);
        if (request.update) {
            Need(request.mode == SelectedMode::Application, "selected_standalone_update_forbidden");
            const auto &patch = group(request.update->groupId, 0x81);
            Need(patch.applicationId == request.applicationId && (!primaryGroup.patchId || !*primaryGroup.patchId || primaryGroup.patchId == patch.titleId),
                 "selected_update_parent_mismatch");
            Need(request.primary.programIndex == request.update->programIndex && request.primary.programId == request.update->programId,
                 "selected_update_program_identity_mismatch");
            result->update = make(patch, &*request.update, false);
        }
        std::set<uint64_t> dataIds;
        std::set<uint64_t> projectedIndices;
        for (const auto &id : request.addOnGroups) {
            const auto &aoc = group(id, 0x82);
            Need(aoc.applicationId == request.applicationId, "selected_aoc_parent_mismatch");
            Need(dataIds.insert(*aoc.titleId).second && (!aoc.dataPatchId || !*aoc.dataPatchId), "selected_aoc_identity_or_data_patch_unsupported");
            const auto index = *aoc.titleId & service::aocsrv::constant::AOCTitleIdMask;
            const auto base = result->primary->nacp->nacpContents.addOnContentBaseId;
            Need(base != 0, "selected_nacp_aoc_base_missing");
            Need(Add(base, index) == *aoc.titleId, "selected_aoc_service_data_id_mismatch");
            Need(projectedIndices.insert(index).second, "selected_aoc_service_index_duplicate");
            result->addOns.push_back(make(aoc, nullptr, false));
        }
        result->Recheck(); return result;
    }
    void SelectedContent::Recheck() const {
        owner->CheckUnchanged();
        for (const auto &binding : bindings) Need(!binding.sha256.empty() && Hash(binding.source) == binding.sha256, "selected_source_content_changed");
        owner->CheckUnchanged();
    }
    void SelectedContent::Install(DeviceState &state) const {
        Need(state.os && !state.process && !state.loader && !state.updateLoader && state.dlcLoaders.empty(), "selected_state_not_empty");
        Recheck();
        // Copy allocations before any state mutation; the assignments below are
        // noexcept shared_ptr/vector moves on this single-threaded launch path.
        auto dlcs = addOns;
        state.os->keyStore = keys; state.loader = primary; state.updateLoader = update; state.dlcLoaders = std::move(dlcs);
    }
    void *SelectedContentLoader::LoadProcessData(const std::shared_ptr<kernel::type::KProcess> &process, const DeviceState &state) {
        Need(primary && state.loader.get() == this && programNca && nacp, "selected_loader_not_primary_program");
        owner->CheckUnchanged();
        try { diagnostics::SetEffectiveExeFs(diagnostics::ContentTitleId(programNca->header.titleId), diagnosticProgramContentId,
                                             cnmt->GetContentMetaType() == vfs::ContentMetaType::Patch ? "selected_standalone_patch" : "selected_primary"); } catch (...) {}
        auto exefs = programNca->exeFs;
        if (state.updateLoader) exefs = vfs::PatchManager{}.PatchExeFS(state, exefs);
        process->npdm = vfs::NPDM(exefs->OpenFile("main.npdm"));
        return NcaLoader::LoadExeFs(this, exefs, process, state);
    }
}
