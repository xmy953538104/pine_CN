// SPDX-License-Identifier: MPL-2.0
#include "selected_launch.h"

#include <set>

namespace skyline::loader {
    namespace {
        [[noreturn]] void Invalid() {
            throw catalog::Error("invalid_selected_launch", "selected launch fields are invalid");
        }
        std::string Hex(const std::string &value, size_t length, bool nonzero) {
            if (value.size() != length) Invalid();
            std::string normalized;
            normalized.reserve(length);
            bool any{};
            for (char c : value) {
                if (c >= 'A' && c <= 'F') c += 'a' - 'A';
                if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) Invalid();
                any |= c != '0';
                normalized += c;
            }
            if (nonzero && !any) Invalid();
            return normalized;
        }
        uint64_t Id(const std::string &value) {
            const auto normalized = Hex(value, 16, true);
            uint64_t result{};
            for (char c : normalized) result = (result << 4) | static_cast<uint64_t>(c <= '9' ? c - '0' : c - 'a' + 10);
            return result;
        }
        std::string Group(const std::string &value) {
            constexpr size_t prefixSize = 12;
            if (value.size() != prefixSize + 64 || value.compare(0, prefixSize, "cnmt-sha256:") != 0) Invalid();
            return "cnmt-sha256:" + Hex(value.substr(prefixSize), 64, false);
        }
        SelectedLaunchProgram Program(const std::vector<std::string> &fields, size_t at, int32_t index) {
            if (index < 0 || index > 255) Invalid();
            return {Group(fields[at]), Hex(fields[at + 1], 32, true), Id(fields[at + 2]), static_cast<uint8_t>(index)};
        }
    }

    SelectedLaunchRequest ParseSelectedLaunchFields(const std::vector<std::string> &fields,
                                                   const std::vector<int32_t> &indices) {
        if (fields.size() < 10 || fields.size() > 42 || indices.size() != 2) Invalid();
        size_t bytes{};
        for (const auto &field : fields) {
            if (field.size() > 64 * 1024 - bytes) Invalid();
            bytes += field.size();
            for (unsigned char c : field) if (c <= 0x20 || c >= 0x7f) Invalid();
        }
        if (fields[0] != "1" || (fields[1] != "application" && fields[1] != "standalone_patch")) Invalid();
        SelectedLaunchRequest result;
        result.standalone = fields[1] == "standalone_patch";
        if (fields[2] == "modern-40") result.profile = catalog::ContentInfoProfile::Modern40;
        else if (fields[2] == "legacy-48") result.profile = catalog::ContentInfoProfile::Legacy48;
        else Invalid();
        result.applicationId = Id(fields[3]);
        result.primary = Program(fields, 4, indices[0]);
        std::set<std::string> groups{result.primary.groupId};
        const bool update = !fields[7].empty() || !fields[8].empty() || !fields[9].empty();
        if (update) {
            if (result.standalone || fields[7].empty() || fields[8].empty() || fields[9].empty()) Invalid();
            result.update = Program(fields, 7, indices[1]);
            if (!groups.insert(result.update->groupId).second) Invalid();
        } else if (indices[1] != -1) Invalid();
        result.addOnGroups.reserve(fields.size() - 10);
        for (size_t at = 10; at < fields.size(); ++at) {
            auto group = Group(fields[at]);
            if (!groups.insert(group).second) Invalid();
            result.addOnGroups.push_back(std::move(group));
        }
        return result;
    }
}
