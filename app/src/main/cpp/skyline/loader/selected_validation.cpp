// SPDX-License-Identifier: MPL-2.0
#include "selected_validation.h"
#include <algorithm>
#include <cstring>
#include <functional>
#include <set>
#include <lz4.h>

namespace skyline::loader::selected {
    using namespace catalog;
    namespace {
        void Need(bool valid, const char *code) { if (!valid) throw Error(code, "selected content failed its bounded structural profile"); }
        void Disjoint(std::vector<std::pair<uint64_t, uint64_t>> ranges) {
            std::sort(ranges.begin(), ranges.end());
            for (size_t i{1}; i < ranges.size(); ++i) Need(ranges[i].first >= ranges[i - 1].second, "selected_overlapping_regions");
        }
        std::pair<uint64_t, uint64_t> Pair(uint64_t offset, uint64_t size, uint64_t limit) {
            Range(offset, size, limit); return {offset, Add(offset, size)};
        }
        std::string Name(const std::vector<uint8_t> &bytes, uint32_t maximum, bool root = false) {
            Need(bytes.size() <= maximum && (root || !bytes.empty()), "selected_invalid_name");
            Need(std::none_of(bytes.begin(), bytes.end(), [](uint8_t b) { return !b || b == '/' || b == '\\' || b < 32; }), "selected_invalid_name");
            std::string name(bytes.begin(), bytes.end());
            Need(name != "." && name != "..", "selected_invalid_name"); return name;
        }
    }
    void ValidateNpdm(Source source, uint64_t programId, const Limits &limits) {
        Need(source->Size() <= limits.maxMetadataBytes, "selected_metadata_budget");
        const auto bytes = source->Bytes(0, static_cast<size_t>(source->Size()));
        Range(0, 0x80, bytes.size()); Need(!std::memcmp(bytes.data(), "META", 4), "selected_npdm_magic");
        // Existing Pine models two address-space bits. Limit this explicit entry
        // to the ordinary AArch64/39-bit profile; do not reinterpret new flags.
        Need(bytes[0x0c] == 7 && !bytes[0x0d], "selected_npdm_flags_unsupported");
        Need(U32(bytes, 0x1c) && U32(bytes, 0x1c) % 0x1000 == 0 && U32(bytes, 0x1c) <= 64 * 1024 * 1024,
             "selected_npdm_stack");
        Need(U32(bytes, 0x14) <= 0x1fe00000 && U32(bytes, 0x14) % 0x1000 == 0, "selected_npdm_resources");
        const auto aci = U32(bytes, 0x70), aciSize = U32(bytes, 0x74), acid = U32(bytes, 0x78), acidSize = U32(bytes, 0x7c);
        Need(aci >= 0x80 && aciSize >= 0x40 && acid >= 0x80 && acidSize >= 0x240, "selected_npdm_sections");
        Disjoint({Pair(aci, aciSize, bytes.size()), Pair(acid, acidSize, bytes.size())});
        Need(!std::memcmp(bytes.data() + aci, "ACI0", 4) && !std::memcmp(bytes.data() + acid + 0x200, "ACID", 4), "selected_npdm_identity_magic");
        Need(U64(bytes, aci + 0x10) == programId, "selected_npdm_program_id_mismatch");
        Need(U64(bytes, acid + 0x210) <= programId && programId <= U64(bytes, acid + 0x218), "selected_npdm_acid_program_range");
        const auto signedSize = U32(bytes, acid + 0x204);
        Need(signedSize >= 0x140 && Add(0x100, signedSize) <= acidSize, "selected_npdm_acid_size");
        Need(!(U32(bytes, acid + 0x20c) & 0xffffffc0U), "selected_npdm_external_module_unsupported");
        for (const auto [base, size, first, minimum] : {std::array<uint64_t, 4>{aci, aciSize, 0x20, 0x40},
                                                       std::array<uint64_t, 4>{acid, acidSize, 0x220, 0x240}}) {
            std::vector<std::pair<uint64_t, uint64_t>> regions;
            for (uint64_t at = first; at < first + 0x18; at += 8) {
                const auto offset = U32(bytes, static_cast<size_t>(base + at)), length = U32(bytes, static_cast<size_t>(base + at + 4));
                if (length) { Need(offset >= minimum, "selected_npdm_access_region"); regions.push_back(Pair(offset, length, size)); }
            }
            Disjoint(std::move(regions));
        }
        const auto caps = U32(bytes, aci + 0x30), capBytes = U32(bytes, aci + 0x34);
        Need(capBytes && capBytes % 4 == 0 && capBytes <= 4096, "selected_npdm_capability_count");
        uint32_t threads{};
        for (uint32_t i{}; i < capBytes; i += 4) {
            const auto cap = U32(bytes, static_cast<size_t>(Add(Add(aci, caps), i)));
            if ((cap & 15) != 7) continue;
            ++threads;
            const auto low = (cap >> 4) & 63, high = (cap >> 10) & 63, minCore = (cap >> 16) & 255, maxCore = cap >> 24;
            Need(high <= low && bytes[0x0e] >= high && bytes[0x0e] <= low && minCore <= maxCore && maxCore < 4 &&
                 bytes[0x0f] >= minCore && bytes[0x0f] <= maxCore, "selected_npdm_thread_capability");
        }
        Need(threads == 1, "selected_npdm_thread_capability_missing_or_ambiguous");
    }

    void ValidateNso(Source source, const Limits &limits, uint64_t &executableBytes) {
        const auto header = source->Bytes(0, 0x100);
        Need(!std::memcmp(header.data(), "NSO0", 4) && U32(header, 4) == 0 && !U32(header, 8), "selected_nso_header");
        const auto flags = U32(header, 0x0c); Need(!(flags & ~uint32_t{0x3f}), "selected_nso_flags_unsupported");
        std::vector<std::pair<uint64_t, uint64_t>> files, memory;
        uint64_t memoryEnd{};
        for (uint32_t segment{}; segment < 3; ++segment) {
            const auto at = 0x10 + segment * 0x10;
            const auto offset = U32(header, at), address = U32(header, at + 4), size = U32(header, at + 8);
            const bool compressed = flags & (1U << segment);
            const auto stored = compressed ? U32(header, 0x60 + segment * 4) : size;
            Need(size && size <= limits.maxSegmentBytes && stored && stored <= limits.maxSegmentBytes && offset >= 0x100,
                 "selected_nso_segment_size");
            Need(address % 0x1000 == 0 && (segment || !address), "selected_nso_memory_alignment");
            const auto mapped = Add(size, segment == 2 ? U32(header, 0x3c) : 0);
            const auto aligned = Add(mapped, 0xfff) & ~uint64_t{0xfff};
            Need(address == memoryEnd, "selected_nso_memory_gap_unsupported"); memoryEnd = Add(address, aligned);
            memory.push_back(Pair(address, aligned, limits.maxExecutableBytes));
            files.push_back(Pair(offset, stored, source->Size()));
            executableBytes = Add(executableBytes, aligned); Need(executableBytes <= limits.maxExecutableBytes, "selected_executable_budget");
            auto data = source->Bytes(offset, stored);
            if (compressed) {
                Need(stored <= 0x7fffffff && size <= 0x7fffffff, "selected_nso_lz4_size");
                std::vector<uint8_t> decoded(size);
                Need(LZ4_decompress_safe(reinterpret_cast<const char *>(data.data()), reinterpret_cast<char *>(decoded.data()),
                                         static_cast<int>(stored), static_cast<int>(size)) == static_cast<int>(size), "selected_nso_lz4_invalid");
                data = std::move(decoded);
            }
            if (flags & (8U << segment)) {
                const auto hash = Sha256(data.data(), data.size());
                Need(std::equal(hash.begin(), hash.end(), header.begin() + 0xa0 + segment * 0x20), "selected_nso_segment_hash_mismatch");
            }
            if (segment == 1) {
                Need(size >= 8, "selected_nso_ro_too_small");
                if (!U32(data, 0)) {
                    const auto nameSize = U32(data, 4);
                    if (nameSize) {
                        Range(8, nameSize, data.size());
                        Need(std::find(data.begin() + 8, data.begin() + 8 + nameSize, 0) != data.begin() + 8 + nameSize,
                             "selected_nso_unterminated_module_name");
                    }
                }
                for (size_t relative = 0x88; relative < 0xa0; relative += 8)
                    Range(U32(header, relative), U32(header, relative + 4), size);
            }
        }
        Disjoint(std::move(files)); Disjoint(std::move(memory));
    }

    void ValidateExeFs(Source source, uint64_t programId, const Limits &limits) {
        auto entries = ReadPfs(source, {});
        std::map<std::string, Source> files;
        for (const auto &entry : entries) files.emplace(entry.name, entry.source);
        Need(files.count("rtld") && files.count("main") && files.count("main.npdm"), "selected_exefs_required_module_missing");
        ValidateNpdm(files.at("main.npdm"), programId, limits);
        uint64_t executableBytes{};
        for (const auto &[name, file] : files) {
            if (name == "main.npdm") continue;
            const bool sdk = name.size() == 7 && name.substr(0, 6) == "subsdk" && name[6] >= '0' && name[6] <= '7';
            Need(name == "rtld" || name == "main" || name == "sdk" || sdk, "selected_exefs_member_unsupported");
            ValidateNso(file, limits, executableBytes);
        }
    }

    Romfs ValidateRomfs(Source source, const Limits &limits) {
        const auto header = source->Bytes(0, 0x50);
        Need(U64(header, 0) == 0x50, "selected_romfs_header");
        std::vector<std::pair<uint64_t, uint64_t>> regions{{0, 0x50}};
        uint64_t metadataBytes{};
        for (size_t at = 8; at < 0x48; at += 16) {
            const auto offset = U64(header, at), size = U64(header, at + 8);
            Need(offset >= 0x50 && offset % 4 == 0 && size % 4 == 0, "selected_romfs_table_alignment");
            metadataBytes = Add(metadataBytes, size);
            Need(metadataBytes <= limits.maxMetadataBytes, "selected_romfs_metadata_budget");
            if (size) regions.push_back(Pair(offset, size, source->Size()));
        }
        Disjoint(regions);
        const auto dirBase = U64(header, 0x18), dirSize = U64(header, 0x20), fileBase = U64(header, 0x38), fileSize = U64(header, 0x40), dataBase = U64(header, 0x48);
        Range(dataBase, 0, source->Size()); Need(dataBase >= 0x50 && dataBase % 16 == 0, "selected_romfs_data_alignment");
        Need(dirSize >= 0x18 && dirSize < 0xffffffffU && fileSize < 0xffffffffU, "selected_romfs_table_size");
        struct Node { uint32_t parent{}, sibling{}, child{}, file{}, nextHash{}; uint64_t data{}, size{}; std::string name; };
        std::map<uint32_t, Node> directoryNodes, fileNodes;
        uint64_t names{};
        auto enumerate = [&](Source table, bool directory) {
            const size_t fixed = directory ? 0x18 : 0x20;
            auto &nodes = directory ? directoryNodes : fileNodes;
            for (uint64_t offset{}; offset < table->Size();) {
                const auto wire = table->Bytes(offset, fixed); const auto length = U32(wire, fixed - 4);
                Need(length <= limits.maxNameBytes, "selected_romfs_name_budget");
                names = Add(names, length); Need(names <= limits.maxNameBytesTotal, "selected_romfs_name_budget");
                const auto extent = Add(Add(fixed, length), 3) & ~uint64_t{3}; Range(offset, extent, table->Size());
                Node node; node.parent = U32(wire, 0); node.sibling = U32(wire, 4); node.nextHash = U32(wire, fixed - 8);
                node.name = Name(table->Bytes(Add(offset, fixed), length), limits.maxNameBytes, directory && offset == 0);
                if (directory) { node.child = U32(wire, 8); node.file = U32(wire, 0x0c); }
                else { node.data = Add(dataBase, U64(wire, 8)); node.size = U64(wire, 0x10); Range(node.data, node.size, source->Size()); }
                nodes.emplace(static_cast<uint32_t>(offset), std::move(node));
                Need(directoryNodes.size() + fileNodes.size() <= limits.maxEntries, "selected_romfs_entry_budget");
                offset = Add(offset, extent);
            }
        };
        enumerate(Region(source, dirBase, dirSize), true); enumerate(Region(source, fileBase, fileSize), false);
        auto checkHash = [&](Source hashes, const auto &nodes) {
            Need(hashes->Size() && hashes->Size() / 4 <= limits.maxEntries, "selected_romfs_hash_bucket_budget");
            const auto bytes = hashes->Bytes(0, static_cast<size_t>(hashes->Size())); const auto buckets = bytes.size() / 4;
            std::set<uint32_t> indexed;
            for (size_t bucket{}; bucket < buckets; ++bucket) {
                auto offset = U32(bytes, bucket * 4);
                while (offset != 0xffffffffU) {
                    const auto found = nodes.find(offset);
                    Need(found != nodes.end(), "selected_romfs_hash_node_boundary");
                    Need(indexed.insert(offset).second, "selected_romfs_hash_cycle_or_duplicate");
                    const auto &node = found->second; uint32_t hash = node.parent ^ 123456789U;
                    for (unsigned char byte : node.name) hash = ((hash >> 5) | (hash << 27)) ^ byte;
                    Need(hash % buckets == bucket, "selected_romfs_wrong_hash_bucket");
                    offset = node.nextHash; // Zero is a real record offset.
                }
            }
            Need(indexed.size() == nodes.size(), "selected_romfs_unindexed_node");
        };
        checkHash(Region(source, U64(header, 8), U64(header, 0x10)), directoryNodes);
        checkHash(Region(source, U64(header, 0x28), U64(header, 0x30)), fileNodes);
        Romfs result;
        std::set<uint32_t> directories, files;
        std::set<std::string> paths;
        uint64_t pathBytes{};
        auto addPath = [&](const std::string &path) {
            pathBytes = Add(pathBytes, path.size()); Need(pathBytes <= limits.maxNameBytesTotal, "selected_romfs_path_budget");
            Need(paths.insert(path).second, "selected_romfs_duplicate_path");
        };
        std::function<void(uint32_t, uint32_t, const std::string &, uint32_t)> walk;
        walk = [&](uint32_t offset, uint32_t parent, const std::string &path, uint32_t depth) {
            Need(depth <= limits.maxDepth, "selected_romfs_depth_budget");
            while (offset != 0xffffffffU) {
                Need(directories.insert(offset).second && directories.size() + files.size() <= limits.maxEntries, "selected_romfs_cycle_or_entry_budget");
                const auto found = directoryNodes.find(offset); Need(found != directoryNodes.end(), "selected_romfs_node_boundary"); const auto &dir = found->second;
                Need(dir.parent == parent && (offset || (path.empty() && depth == 0)), "selected_romfs_parent_mismatch");
                Need(offset || dir.sibling == 0xffffffffU, "selected_romfs_root_sibling"); Need(offset || dir.name.empty(), "selected_romfs_root_name");
                const auto full = path + (path.empty() ? "" : "/") + dir.name; addPath(full);
                auto fileOffset = dir.file;
                while (fileOffset != 0xffffffffU) {
                    Need(files.insert(fileOffset).second && directories.size() + files.size() <= limits.maxEntries, "selected_romfs_cycle_or_entry_budget");
                    const auto member = fileNodes.find(fileOffset); Need(member != fileNodes.end(), "selected_romfs_node_boundary"); const auto &file = member->second;
                    Need(file.parent == offset, "selected_romfs_parent_mismatch");
                    const auto fullFile = full + (full.empty() ? "" : "/") + file.name; addPath(fullFile);
                    if (file.size) regions.push_back(Pair(file.data, file.size, source->Size()));
                    result.files.emplace(fullFile, Region(source, file.data, file.size)); fileOffset = file.sibling;
                }
                if (dir.child != 0xffffffffU) walk(dir.child, offset, full, depth + 1);
                offset = dir.sibling;
            }
        };
        walk(0, 0, "", 0);
        Need(directories.size() == directoryNodes.size() && files.size() == fileNodes.size(), "selected_romfs_orphan_node");
        Disjoint(std::move(regions));
        return result;
    }
    void ValidateNacp(Source source) {
        Need(source->Size() == 0x4000, "selected_control_nacp_size");
        const auto bytes = source->Bytes(0, 0x4000);
        bool title{};
        for (size_t language{}; language < 16; ++language) {
            const auto start = bytes.begin() + language * 0x300;
            Need(std::find(start, start + 0x200, 0) != start + 0x200 && std::find(start + 0x200, start + 0x300, 0) != start + 0x300,
                 "selected_nacp_unterminated_text");
            title = title || *start;
        }
        Need(title && (U32(bytes, 0x302c) & 0xffff) && !(U32(bytes, 0x302c) & 0xffff0000U), "selected_nacp_language");
        Need(std::find(bytes.begin() + 0x3060, bytes.begin() + 0x3070, 0) != bytes.begin() + 0x3070, "selected_nacp_unterminated_text");
    }
}
