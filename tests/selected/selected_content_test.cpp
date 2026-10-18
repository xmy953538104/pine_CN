// SPDX-License-Identifier: MPL-2.0
// Actual selected factory, NCA/CTR, CNMT, NPDM, NACP, RomFS and PatchManager.
// Only the Android/kernel process-launch tail and guest NSO mapping are stubbed.
#include "fixtures.h"
#include <loader/selected_content.h>
#include <loader/nca.h>
#include <os.h>
#include <kernel/types/KProcess.h>
#include <vfs/patch_manager.h>
#include <vfs/rom_filesystem.h>
#include <diagnostics/content.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>

using namespace fixture;
using namespace skyline::loader;
namespace {
    size_t cases{}, tailCalls{}, loadExeFsCalls{};
    std::string loadedMainHash, metadataSource;
    std::filesystem::path evidence;
    void Pass(const char *name) { ++cases; std::cout << "PASS " << name << '\n'; }
    template<class Function> std::string Failure(Function function) {
        try { function(); } catch (const Error &error) { return error.code; }
        throw std::runtime_error("operation unexpectedly succeeded");
    }
    template<class Function> void Reject(const char *name, const char *expected, Function function) {
        const auto actual = Failure(function);
        if (actual != expected) throw std::runtime_error(std::string(name) + ": expected " + expected + ", got " + actual);
        Pass(name);
    }
    const Group &GroupFor(const Catalog &catalog, uint64_t id, uint8_t type) {
        const Group *found{};
        for (const auto &group : catalog.groups) if (group.titleId == id && group.type == type) { Check(!found, "test request group ambiguous"); found = &group; }
        Check(found, "test request group absent"); return *found;
    }
    ProgramChoice Choose(const Catalog &catalog, const Group &group, uint8_t index = 0) {
        ProgramChoice choice; choice.groupId = group.id;
        for (const auto &ref : group.contents) if (ref.type == 1 && ref.idOffset == index) {
            Check(choice.contentId.empty(), "test Program choice ambiguous"); choice.contentId = ref.contentId;
            choice.programIndex = index; if (ref.nca) choice.programId = catalog.ncas.at(*ref.nca).programId;
        }
        Check(!choice.contentId.empty(), "test Program absent"); return choice;
    }
    struct Input {
        std::shared_ptr<MutableSource> source;
        Catalog catalog;
        SelectionRequest request;
        explicit Input(Package package = {}) : Input(package.Build(), package.standalone, package.withPatch || package.standalone) {}
        Input(Files files, bool standalone = false, bool patch = false, std::optional<ProgramChoice> explicitPrimary = {})
            : source(std::make_shared<MutableSource>(Pfs(files))), catalog(ScanNative(source, Keys(), OptionsForTest())) {
            request.profile = catalog.options.profile; request.applicationId = App;
            request.mode = standalone ? SelectedMode::StandaloneRebuiltPatch : SelectedMode::Application;
            request.primary = explicitPrimary ? *explicitPrimary : Choose(catalog, GroupFor(catalog, standalone ? Patch : App, standalone ? 0x81 : 0x80));
            if (patch && !standalone) request.update = Choose(catalog, GroupFor(catalog, Patch, 0x81));
            for (const auto &group : catalog.groups) if (group.type == 0x82) request.addOnGroups.push_back(group.id);
        }
        auto Prepare() const { return PrepareSelectedContent(catalog, request, Keys()); }
    };
    std::string FileText(std::shared_ptr<vfs::Backing> backing, const char *path) {
        vfs::RomFileSystem rom(std::move(backing)); const auto file = rom.OpenFile(path); Check(static_cast<bool>(file), "runtime RomFS file missing");
        const auto bytes = MakeBackingSource(file)->Bytes(0, file->size); return {bytes.begin(), bytes.end()};
    }
    std::string EffectiveMainHash(const DeviceState &state) {
        auto files = state.loader->programNca->exeFs;
        if (state.updateLoader) files = vfs::PatchManager{}.PatchExeFS(state, files);
        const auto main = MakeBackingSource(files->OpenFile("main")); const auto hash = Sha256(*main); return Hex(hash.data(), hash.size());
    }
    void Write(const std::string &name, const std::string &text) {
        if (evidence.empty()) return;
        std::ofstream out(evidence / name); out << text; out.close(); Check(static_cast<bool>(out), "evidence write failed");
    }
    void HappyPaths() {
        Input base;
        auto selected = base.Prepare();
        Check(selected->Primary()->cnmt->header.contentMetaType == vfs::ContentMetaType::Application && !selected->Update(), "base role changed");
        Check(selected->Primary()->programNca->header.titleId == App && selected->Primary()->selectedProgramIndex == 0, "explicit Program identity lost");
        Check(selected->Primary()->nacp->nacpContents.saveDataOwnerId == App, "actual NACP not populated");
        Check(FileText(selected->Primary()->romFs, "marker") == "base", "wrong primary RomFS");
        kernel::OS os; selected->Install(os.state);
        Check(os.state.dlcLoaders.size() == 2, "multiple AOCs missing");
        for (size_t i{}; i < os.state.dlcLoaders.size(); ++i) {
            const auto &aoc = os.state.dlcLoaders[i];
            Check(aoc->cnmt->GetContentMetaType() == vfs::ContentMetaType::AddOnContent && aoc->cnmt->header.id == Aoc1 + i, "AOC CNMT identity changed");
            Check(aoc->cnmt->GetParentTitleId() == diagnostics::ContentTitleId(App) && aoc->publicNca &&
                  aoc->publicNca->contentType == vfs::NCAContentType::PublicData && aoc->publicNca->header.titleId == Aoc1 + i, "AOC actual PublicData identity lost");
            // Same existing PatchManager entry used by OpenDataStorageByDataId;
            // each DataID maps to its own actual loader and source bytes.
            auto data = vfs::PatchManager{}.PatchRomFS(os.state, aoc->publicNca, os.state.loader->programNca->ivfcOffset);
            Check(FileText(data, "marker") == "aoc" + std::to_string(i + 1), "AOC data crossed between loaders");
        }
        Check(os.state.dlcLoaders[0]->publicNca->header.titleId != os.state.dlcLoaders[1]->publicNca->header.titleId, "AOC DataIDs collapsed");
        Pass("Application plus two independently selected AOC PublicData objects");
        Package patched; patched.withPatch = true; Input update(patched); auto bundle = update.Prepare(); kernel::OS updated; bundle->Install(updated.state);
        Check(updated.state.loader->cnmt->GetContentMetaType() == vfs::ContentMetaType::Application &&
              updated.state.updateLoader->cnmt->GetContentMetaType() == vfs::ContentMetaType::Patch, "Patch was rewritten as Application");
        Check(EffectiveMainHash(updated.state) == Hash(patched.patchMain), "existing PatchManager did not return selected update ExeFS");
        Check(FileText(vfs::PatchManager{}.PatchRomFS(updated.state, updated.state.updateLoader->programNca,
                       updated.state.loader->programNca->ivfcOffset), "marker") == "patch", "full CTR update RomFS missing");
        Pass("Application plus independently complete CTR Patch and AOCs use existing PatchManager");
        Package rebuilt; rebuilt.standalone = true; Input standalone(rebuilt); const auto lone = standalone.Prepare(); kernel::OS only; lone->Install(only.state);
        Check(only.state.loader->cnmt->GetContentMetaType() == vfs::ContentMetaType::Patch && !only.state.updateLoader &&
              only.state.loader->cnmt->GetParentTitleId() == diagnostics::ContentTitleId(App), "standalone Patch invented a Base");
        Check(FileText(only.state.loader->romFs, "marker") == "patch", "standalone Patch lacks own RomFS");
        Check(diagnostics::DescribeContentIdentity(*only.state.loader).source == "cnmt_patch_parent", "standalone identity mislabeled Base");
        Pass("explicit rebuilt Patch retains real Patch role with no Base loader");
        auto files = patched.Build(); std::reverse(files.begin(), files.end());
        for (size_t i{}; i < files.size(); ++i) files[i].first = "renamed-" + std::to_string(i);
        const auto unrelated = Nca(1, App + 0x5000, {{true, Pfs({{"unrelated.cnmt", Cnmt(0x80, App + 0x5000, 0, {})}})}});
        files.push_back({"unrelated-meta", unrelated}); Input reordered(files, false, true); const auto stable = reordered.Prepare();
        Check(stable->Primary()->selectedGroupId == bundle->Primary()->selectedGroupId && stable->Update()->selectedGroupId == bundle->Update()->selectedGroupId,
              "renamed/reordered/unrelated Meta changed explicit selection");
        Pass("renamed reordered and unrelated Meta do not change explicit selection");
        Package multiple; multiple.multiplePrograms = true; multiple.withAocs = false; Input programs(multiple);
        programs.request.primary = Choose(programs.catalog, GroupFor(programs.catalog, App, 0x80), 1);
        const auto other = programs.Prepare(); Check(other->Primary()->programNca->header.titleId == App + 1 && other->Primary()->selectedProgramIndex == 1, "Program index ignored");
        Pass("multiple Program indices require and honor explicit index and actual ProgramId");
        Write("selected-catalog.json", ToJson(update.catalog));
        auto previewOptions = OptionsForTest(); previewOptions.verifyHashes = false;
        const auto preview = ScanNative(update.source, Keys(), previewOptions);
        Check(preview.complete && preview.AllMetadataEnumerated() && !preview.options.verifyHashes &&
              preview.groups.size() == 4, "native metadata preview failed");
        Write("selected-preview.json", ToJson(preview));
        Pass("real native metadata-only preview retains explicit Application Patch and two AOC candidates");
        Write("selected-runtime.json", "{\"profile\":\"bounded_structural_v1\",\"application_id\":\"" + diagnostics::ContentTitleId(App) +
             "\",\"primary_cnmt_role\":\"application\",\"update_cnmt_role\":\"patch\",\"aoc_count\":2,\"bktr_supported\":false,"
             "\"signature_verification\":\"not_verified\",\"ivfc_tree_verification\":\"not_performed\",\"guest_execution\":\"not_performed\"}\n");
    }
    void RejectionPaths() {
        Input normal;
        auto unknownMode = normal.request; unknownMode.mode = static_cast<SelectedMode>(99);
        Reject("unknown selected mode cannot fall through to standalone Patch", "selected_mode_unsupported", [&] { PrepareSelectedContent(normal.catalog, unknownMode, Keys()); });
        auto missing = normal.request; missing.primary.programIndex.reset();
        Reject("missing Program index rejected", "selected_program_selector_missing", [&] { PrepareSelectedContent(normal.catalog, missing, Keys()); });
        auto wrong = normal.request; wrong.primary.programId = App + 1;
        Reject("actual ProgramId mismatch rejected", "selected_program_identity_mismatch", [&] { PrepareSelectedContent(normal.catalog, wrong, Keys()); });
        auto reused = normal.request; reused.addOnGroups.push_back(reused.addOnGroups.front());
        Reject("duplicate AOC group rejected", "selected_group_reused", [&] { PrepareSelectedContent(normal.catalog, reused, Keys()); });
        Package parent; parent.withPatch = true; parent.patchParent = App + 0x2000; Input wrongParent(parent);
        Reject("Patch parent mismatch rejected", "selected_update_parent_mismatch", [&] { wrongParent.Prepare(); });
        Package aocParent; aocParent.aocParent = App + 0x2000; Input wrongAoc(aocParent);
        Reject("AOC parent mismatch rejected", "selected_aoc_parent_mismatch", [&] { wrongAoc.Prepare(); });
        Package data; data.aocAsData = true; Input wrongData(data);
        Reject("Data NCA cannot be cast to PublicData", "selected_aoc_requires_public_data", [&] { wrongData.Prepare(); });
        Package noData; noData.omitPublicData = true; Input absentData(noData);
        Reject("AOC missing PublicData rejected", "selected_group_has_no_content", [&] { absentData.Prepare(); });
        auto unknownFiles = Package{}.Build(); unknownFiles.push_back({"unknown-small-member.tik", {1, 2, 3}}); Input unknown(unknownFiles);
        Check(unknown.catalog.complete && !unknown.catalog.AllMetadataEnumerated(), "unknown-member test does not distinguish bounded scan and enumeration");
        Reject("unknown small outer member rejects selected container", "selected_unclassified_or_unreadable_metadata", [&] { unknown.Prepare(); });
        Package incomplete; incomplete.standalone = true; incomplete.omitRtld = true; Input noRtld(incomplete);
        Reject("standalone Patch missing required rtld rejected", "selected_exefs_required_module_missing", [&] { noRtld.Prepare(); });
        Package badNpdm; badNpdm.standalone = true; Put(badNpdm.appNpdm, 0x2d0, App + 1, 8); Input wrongNpdm(badNpdm);
        Reject("standalone NPDM actual identity rejected", "selected_npdm_program_id_mismatch", [&] { wrongNpdm.Prepare(); });
        Package cap; cap.standalone = true; Put(cap.appNpdm, 0x300, 0xffffffff, 4); Input missingCap(cap);
        Reject("uninitialized legacy thread capability cannot be reached", "selected_npdm_thread_capability_missing_or_ambiguous", [&] { missingCap.Prepare(); });
        Package acid; acid.standalone = true; Put(acid.appNpdm, 0x298, App - 1, 8); Input wrongAcid(acid);
        Reject("ACID ProgramId bounds required", "selected_npdm_acid_program_range", [&] { wrongAcid.Prepare(); });
        Package nso; nso.standalone = true; nso.patchMain[0x100] ^= 1; Input badNso(nso);
        Reject("NSO declared segment hash checked", "selected_nso_segment_hash_mismatch", [&] { badNso.Prepare(); });
        Package gap; gap.standalone = true; Put(gap.patchMain, 0x24, 0x3000, 4); Input gapNso(gap);
        Reject("NSO memory gap rejects unsafe legacy next module placement", "selected_nso_memory_gap_unsupported", [&] { gapNso.Prepare(); });
        Package rom; rom.standalone = true; const auto fileAt = U64(rom.patchRomfs, 0x38); Put(rom.patchRomfs, fileAt + 4, 0, 4); Input cycle(rom);
        Reject("RomFS graph cycle rejected before legacy traversal", "selected_romfs_cycle_or_entry_budget", [&] { cycle.Prepare(); });
        Package extents; extents.standalone = true; Put(extents.patchRomfs, fileAt + 0x10, UINT64_MAX, 8); Input escaped(extents);
        Reject("RomFS payload escape rejected", "out_of_range", [&] { escaped.Prepare(); });
        Package bktr; bktr.standalone = true; bktr.bktr = true; Input missingBase(bktr);
        Reject("BKTR standalone rejects missing Base", "selected_bktr_unsupported", [&] { missingBase.Prepare(); });
        bktr.standalone = false; bktr.withPatch = true; Input matchingBase(bktr);
        Reject("BKTR still rejected with matching Base", "selected_bktr_unsupported", [&] { matchingBase.Prepare(); });
        Package compressed; compressed.standalone = true; compressed.patchMain = Nso(9, true); Input lz4(compressed);
#ifdef PINE_CATALOG_TEST_REAL_AES
        Check(static_cast<bool>(lz4.Prepare()), "production LZ4 positive failed"); Pass("actual pinned LZ4 compressed NSO validates");
#else
        Reject("controlled lane never claims LZ4 decompression", "selected_nso_lz4_invalid", [&] { lz4.Prepare(); });
#endif
    }
    void ReferenceAndCompletenessTests() {
        Package package; package.withAocs = false; auto files = package.Build();
        auto alias = files; alias.push_back({Hash(package.appRefs[0].bytes).substr(0, 32) + ".cnmt.nca", package.appRefs[1].bytes}); Input duplicateId(alias);
        Reject("conflicting ContentID names cannot override actual referenced hashes", "selected_duplicate_content_id", [&] { duplicateId.Prepare(); });
        auto absent = files; absent.erase(absent.begin()); Input missingNca(absent);
        Reject("missing referenced Program rejected", "selected_content_reference_unverified", [&] { missingNca.Prepare(); });
        auto replaceMeta = [&](Files values, const std::vector<Reference> &refs) {
            const auto meta = Nca(1, App, {{true, Pfs({{"content.cnmt", Cnmt(0x80, App, 0, refs)}})}});
            const auto found = std::find_if(values.begin(), values.end(), [](const auto &entry) { return entry.first.ends_with(".cnmt.nca"); });
            Check(found != values.end(), "test Meta absent"); *found = {Hash(meta).substr(0, 32) + ".cnmt.nca", meta}; return values;
        };
        auto noControlRefs = package.appRefs; noControlRefs.pop_back(); Input noControl(replaceMeta(files, noControlRefs));
        Reject("unique Control is required", "selected_control_missing_or_ambiguous", [&] { noControl.Prepare(); });
        auto twoControlRefs = package.appRefs; auto secondNacp = Nacp(); secondNacp[0x3060] = '2';
        const auto secondControl = Nca(2, App, {{false, Romfs({{"control.nacp", secondNacp}})}}); twoControlRefs.push_back({3, 1, secondControl, {}});
        auto twoControlFiles = replaceMeta(files, twoControlRefs); twoControlFiles.push_back({Hash(secondControl).substr(0, 32) + ".nca", secondControl}); Input ambiguousControl(twoControlFiles);
        Reject("multiple Controls are not chosen by iteration order", "selected_control_missing_or_ambiguous", [&] { ambiguousControl.Prepare(); });
        Package multiple; multiple.withAocs = false; multiple.multiplePrograms = true; auto multiFiles = multiple.Build(); auto sameIndex = multiple.appRefs;
        sameIndex.back().index = 0;
        const ProgramChoice explicitDuplicate{"cnmt-sha256:" + Hash(Cnmt(0x80, App, 0, sameIndex)), Hash(sameIndex.front().bytes).substr(0, 32), 0, App};
        Input duplicateIndex(replaceMeta(multiFiles, sameIndex), false, false, explicitDuplicate);
        Reject("duplicate Program idOffset rejected", "selected_duplicate_program_index", [&] { duplicateIndex.Prepare(); });
        Package wrongDataId; wrongDataId.publicDataIdBias = 1; Input mismatch(wrongDataId);
        Reject("AOC actual PublicDataId must equal requested CNMT DataId", "selected_aoc_data_id_mismatch", [&] { mismatch.Prepare(); });
        Package wrongBase; Put(wrongBase.nacp, 0x3070, App + 0x2000, 8); Input incompatibleBase(wrongBase);
        Reject("AOC service projection must match exact NACP base", "selected_aoc_service_data_id_mismatch", [&] { incompatibleBase.Prepare(); });
        Package overflow; Put(overflow.nacp, 0x3070, UINT64_MAX, 8); Input overflowBase(overflow);
        Reject("AOC service base plus projected index cannot overflow", "integer_overflow", [&] { overflowBase.Prepare(); });
        Package nacp; nacp.standalone = true; std::fill_n(nacp.nacp.begin() + 0x3060, 0x10, 'x'); Input badNacp(nacp);
        Reject("NACP display string must terminate in exact field", "selected_nacp_unterminated_text", [&] { badNacp.Prepare(); });
        Package ro; ro.standalone = true;
        const auto roOffset = U32(ro.patchMain, 0x20), roSize = U32(ro.patchMain, 0x28);
        std::fill_n(ro.patchMain.begin() + roOffset, roSize, 'x'); Put(ro.patchMain, roOffset, 0, 4); Put(ro.patchMain, roOffset + 4, roSize - 8, 4);
        const auto roHash = Sha256(ro.patchMain.data() + roOffset, roSize); std::copy(roHash.begin(), roHash.end(), ro.patchMain.begin() + 0xc0); Input badName(ro);
        Reject("NSO legacy module string is bounded before runtime parsing", "selected_nso_unterminated_module_name", [&] { badName.Prepare(); });
        Package invalidLz4; invalidLz4.standalone = true; invalidLz4.patchMain = Nso(8, true);
        Put(invalidLz4.patchMain, 0x60, 1, 4); invalidLz4.patchMain[0x100] = 0; Input brokenLz4(invalidLz4);
        Reject("NSO decompression must produce its exact declared extent", "selected_nso_lz4_invalid", [&] { brokenLz4.Prepare(); });
    }
    void RomfsIndexTests() {
        std::ifstream file("tests/selected/fixtures/romfs-collision.romfs", std::ios::binary);
        Bytes literal{std::istreambuf_iterator<char>(file), {}};
        Check(literal.size() == 680 && Hash(literal) == "c7d6619a3d10e89e49b292f1d4c9d10cc4479dc95d094bcad4058ccba8176501", "independent RomFS oracle changed");
        Package package; package.standalone = true; package.patchRomfs = literal; Input input(package);
        const auto valid = input.Prepare();
        Check(FileText(valid->Primary()->romFs, "c/a") == "OK" && FileText(valid->Primary()->romFs, "c/d") == "!", "collision or literal data-before-metadata rejected");
        Pass("independent literal collision chains include offset zero and real nested runtime files");
        auto validate = [&](Bytes bytes, selected::Limits limits = {}) { return selected::ValidateRomfs(std::make_shared<MutableSource>(std::move(bytes)), limits); };
        auto wrongBucket = literal; Put(wrongBucket, 0x214, 0xffffffff, 4); Put(wrongBucket, 0x218, 0x18, 4);
        Reject("RomFS directory wrong bucket rejected", "selected_romfs_wrong_hash_bucket", [&] { validate(wrongBucket); });
        auto fileBucket = literal; Put(fileBucket, 0x254, 0xffffffff, 4); Put(fileBucket, 0x258, 0x24, 4);
        Reject("RomFS file wrong bucket rejected", "selected_romfs_wrong_hash_bucket", [&] { validate(fileBucket); });
        auto hashCycle = literal; Put(hashCycle, 0x220 + 0x10, 0x18, 4);
        Reject("RomFS collision chain cycle rejected", "selected_romfs_hash_cycle_or_duplicate", [&] { validate(hashCycle); });
        auto unindexed = literal; Put(unindexed, 0x260 + 0x24 + 0x18, 0xffffffff, 4);
        Reject("RomFS unindexed metadata rejected", "selected_romfs_unindexed_node", [&] { validate(unindexed); });
        auto orphan = literal; Put(orphan, 0x220 + 8, 0xffffffff, 4);
        Reject("RomFS indexed but unreachable nodes rejected", "selected_romfs_orphan_node", [&] { validate(orphan); });
        auto middle = literal; Put(middle, 0x254, 0x25, 4);
        Reject("RomFS hash cannot point inside a record", "selected_romfs_hash_node_boundary", [&] { validate(middle); });
        auto misaligned = literal; Put(misaligned, 0x10, 11, 8);
        Reject("RomFS table alignment required", "selected_romfs_table_alignment", [&] { validate(misaligned); });
        selected::Limits limits; limits.maxMetadataBytes = 0x34 + 0x48;
        Reject("RomFS all four metadata tables share one budget", "selected_romfs_metadata_budget", [&] { validate(literal, limits); });
        // The same corruption also crosses the actual encrypted NCA factory,
        // before any legacy RomFileSystem or process object is installed.
        package.patchRomfs = wrongBucket; Input nativeBad(package);
        Reject("native encrypted RomFS bad hash index rejected", "selected_romfs_wrong_hash_bucket", [&] { nativeBad.Prepare(); });
    }
    void SymbolTableTests() {
        auto nso = Nso(); const auto ro = U32(nso, 0x20), roSize = U32(nso, 0x28);
        std::fill_n(nso.begin() + ro, roSize, 0);
        Put(nso, 0x90, 8, 4); Put(nso, 0x94, 8, 4); Put(nso, 0x98, 16, 4); Put(nso, 0x9c, 24, 4);
        std::memcpy(nso.data() + ro + 9, "bound", 5); Put(nso, ro + 16, 1, 4); nso[ro + 20] = 0x12;
        Put(nso, ro + 22, 1, 2); Put(nso, ro + 24, 0x1000, 8); Put(nso, ro + 32, 4, 8);
        auto refresh = [&](Bytes &value) { const auto hash = Sha256(value.data() + ro, roSize); std::copy(hash.begin(), hash.end(), value.begin() + 0xc0); };
        auto validate = [&](Bytes value) { uint64_t bytes{}; selected::ValidateNso(std::make_shared<MutableSource>(std::move(value)), {}, bytes); };
        refresh(nso); validate(nso);
        Package package; package.standalone = true; package.patchMain = nso; Input actual(package); Check(static_cast<bool>(actual.Prepare()), "bounded symbol table native positive failed");
        Pass("bounded aligned ELF64 symbol and terminated dynstr accepted in actual encrypted Program");
        auto incomplete = nso; Put(incomplete, 0x9c, 1, 4);
        Reject("ELF64 dynsym byte count must be divisible by 24", "selected_nso_symbol_table_layout", [&] { validate(incomplete); });
        auto alignment = nso; Put(alignment, 0x98, 1, 4);
        Reject("ELF64 dynsym offset must align real symbol references", "selected_nso_symbol_table_layout", [&] { validate(alignment); });
        auto outOfRange = nso; Put(outOfRange, ro + 16, 0xffffffffU, 4); refresh(outOfRange);
        Reject("ELF64 symbol name cannot escape dynstr", "selected_nso_symbol_name_range", [&] { validate(outOfRange); });
        auto noNul = nso; std::fill_n(noNul.begin() + ro + 9, 7, 'x'); refresh(noNul);
        Reject("ELF64 symbol name must terminate within dynstr", "selected_nso_unterminated_symbol_name", [&] { validate(noNul); });
        auto endName = nso; Put(endName, ro + 16, 8, 4); refresh(endName);
        Reject("ELF64 one-past-end name index rejected", "selected_nso_symbol_name_range", [&] { validate(endName); });
        package.patchMain = outOfRange; Input nativeBad(package);
        Reject("actual factory rejects bad symbol after valid recomputed RO hash", "selected_nso_symbol_name_range", [&] { nativeBad.Prepare(); });
        auto empty = nso; Put(empty, 0x9c, 0, 4); Put(empty, 0x98, 1, 4); Put(empty, 0x94, 0, 4); validate(empty);
        Pass("empty dynamic tables do not fabricate a symbol or string requirement");
    }
    void SourceAndAtomicity() {
        Input input; auto prepared = input.Prepare(); kernel::OS os;
        const auto members = ReadPfs(input.source, {}); const auto member = members.front();
        auto limited = OptionsForTest(); limited.limits.maxMetadataBytes = 0x1000; const auto reads = input.source->reads;
        Reject("native runtime open reserves budget before prefix reads", "scan_budget", [&] { OpenNativeNca(member, Keys(), limited); });
        Check(input.source->reads == reads, "runtime native open read before its metadata reservation");
        input.source->bytes.at(static_cast<size_t>(member.offset + member.size - 1)) ^= 1;
        Reject("Install rehashes actual same-size source bytes", "selected_source_content_changed", [&] { prepared->Install(os.state); });
        Check(!os.state.loader && !os.state.updateLoader && os.state.dlcLoaders.empty() && !os.keyStore, "failed install mutated state");
        input.source->bytes.at(static_cast<size_t>(member.offset + member.size - 1)) ^= 1;
        input.source->changed = true;
        Reject("changed owning source rejected", "input_changed", [&] { prepared->Install(os.state); });
        input.source->changed = false; input.source->bytes.pop_back();
        Reject("truncated owning source rejected", "out_of_range", [&] { prepared->Install(os.state); });
        Input shortRead; shortRead.source->readLimit = 1;
        Reject("actual short read rejected", "short_read", [&] { shortRead.Prepare(); });
        Input occupied; auto safe = occupied.Prepare(); kernel::OS running; safe->Install(running.state); const auto originalKeys = running.keyStore;
        Reject("occupied launch state rejected without replacement", "selected_state_not_empty", [&] { safe->Install(running.state); });
        Check(running.state.loader == safe->Primary() && running.keyStore == originalKeys, "occupied state replaced");
        Input fail; fail.source->fail = true;
        bool propagated{};
        try { fail.Prepare(); } catch (const std::runtime_error &error) { propagated = std::string(error.what()).find("PRIVATE_") != std::string::npos; }
        Check(propagated, "original reader exception altered");
        Pass("reader exceptions propagate and are not logged by factory");
    }
    void DiagnosticsAndOsEntry() {
        Package package; package.withPatch = true; Input input(package); auto bad = input.request; bad.primary.programId = App + 5;
        const auto disabledError = Failure([&] { PrepareSelectedContent(input.catalog, bad, Keys()); });
        const auto off = input.Prepare(); const auto beforeTail = tailCalls;
        kernel::OS disabled; disabled.ExecuteSelectedContent(input.catalog, input.request, Keys());
        Check(tailCalls == beforeTail + 1 && loadExeFsCalls && loadedMainHash == Hash(package.patchMain) && metadataSource == "selected_primary_control",
              "actual explicit OS entry failed to reach selected runtime objects");
        auto logDir = evidence.empty() ? std::filesystem::temp_directory_path() / ("pine-selected-diag-" + std::to_string(::getpid())) : evidence / "diagnostics";
        diagnostics::Config config; config.directory = logDir.string(); config.sessionId = "selected-synthetic";
        Check(diagnostics::Start(config), "diagnostics could not start");
        const auto enabledError = Failure([&] { PrepareSelectedContent(input.catalog, bad, Keys()); });
        const auto on = input.Prepare(); kernel::OS enabled; enabled.ExecuteSelectedContent(input.catalog, input.request, Keys());
        Check(enabledError == disabledError && on->Primary()->selectedGroupId == off->Primary()->selectedGroupId &&
              on->Update()->selectedGroupId == off->Update()->selectedGroupId && enabled.state.dlcLoaders.size() == disabled.state.dlcLoaders.size(),
              "diagnostics changed selected content or failure");
        Package standalone; standalone.standalone = true; Input rebuilt(standalone); kernel::OS lone; lone.ExecuteSelectedContent(rebuilt.catalog, rebuilt.request, Keys());
        Check(metadataSource == "selected_standalone_patch_control" && lone.state.loader->cnmt->GetContentMetaType() == vfs::ContentMetaType::Patch,
              "OS standalone Patch metadata source was relabeled Base");
        Check(diagnostics::ApplicationTitleId() == diagnostics::ContentTitleId(App), "explicit OS entry failed actual typed diagnostics identity");
        diagnostics::Stop();
        std::string logs;
        for (const auto &entry : std::filesystem::directory_iterator(logDir)) if (entry.is_regular_file()) {
            std::ifstream file(entry.path()); logs.append(std::istreambuf_iterator<char>(file), {});
        }
        Check(logs.find("SELECTED_CONTENT_INSTALLED") != std::string::npos && logs.find("DLC_REGISTERED") != std::string::npos &&
              logs.find("standalone_patch") != std::string::npos && logs.find("cnmt_patch_parent") != std::string::npos && logs.find("PRIVATE_") == std::string::npos,
              "actual selected identity/stages/registered events missing or private exception leaked");
        if (evidence.empty()) std::filesystem::remove_all(logDir);
        Pass("actual OS selected entry and real diagnostics retain identical choice failure and typed roles");
    }
}
namespace skyline::loader {
    void *NcaLoader::LoadExeFs(Loader *, const std::shared_ptr<vfs::FileSystem> &files,
                             const std::shared_ptr<kernel::type::KProcess> &process, const DeviceState &) {
        ++loadExeFsCalls; const auto source = catalog::MakeBackingSource(files->OpenFile("main")); const auto hash = catalog::Sha256(*source);
        loadedMainHash = catalog::Hex(hash.data(), hash.size()); Check(process->npdm.aci0.programId == App, "runtime actual NPDM was not selected");
        return reinterpret_cast<void *>(uintptr_t{0x1234});
    }
}
namespace skyline::kernel {
    void OS::ExecuteLoadedContent(const char *source) {
        ++tailCalls; metadataSource = source; state.process = std::make_shared<type::KProcess>();
        Check(state.loader->LoadProcessData(state.process, state) == reinterpret_cast<void *>(uintptr_t{0x1234}), "selected runtime call lost");
    }
}
int main(int argc, char **argv) {
    try {
        if (argc > 1) { evidence = argv[1]; std::filesystem::create_directories(evidence); }
        HappyPaths(); RejectionPaths(); ReferenceAndCompletenessTests(); RomfsIndexTests(); SymbolTableTests(); SourceAndAtomicity(); DiagnosticsAndOsEntry();
#ifdef PINE_CATALOG_TEST_REAL_AES
        std::cout << "REAL_AES_AND_LZ4 ";
#else
        std::cout << "CONTROLLED_CIPHER ";
#endif
        std::cout << cases << " selected production API scenarios passed; no guest or Android execution\n"; return 0;
    } catch (const Error &error) { std::cerr << "catalog error " << error.code << ": " << error.what() << '\n'; }
      catch (const std::exception &error) { std::cerr << "test failure: " << error.what() << '\n'; }
    diagnostics::Stop(); return 1;
}
