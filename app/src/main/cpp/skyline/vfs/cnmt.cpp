// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright © 2023 Strato Team and Contributors (https://github.com/strato-emu/)

#include "cnmt.h"

namespace skyline::vfs {

    CNMT::CNMT(std::shared_ptr<Backing> member) {
        header = member->Read<PackagedContentMetaHeader>();
        if (header.contentMetaType >= ContentMetaType::Application && header.contentMetaType <= ContentMetaType::AddOnContent)
            optionalHeader = member->Read<OptionalHeader>(sizeof(PackagedContentMetaHeader));
        const size_t contents{sizeof(PackagedContentMetaHeader) + header.extendedHeaderSize};
        const size_t metas{contents + static_cast<size_t>(header.contentCount) * sizeof(PackagedContentInfo)};
        for (u16 i{}; i < header.contentCount; ++i)
            contentInfos.emplace_back(member->Read<PackagedContentInfo>(contents + static_cast<size_t>(i) * sizeof(PackagedContentInfo)));
        for (u16 i{}; i < header.contentMetaCount; ++i)
            contentMetaInfos.emplace_back(member->Read<ContentMetaInfo>(metas + static_cast<size_t>(i) * sizeof(ContentMetaInfo)));
    }

    CNMT::CNMT(std::shared_ptr<FileSystem> cnmtSection) {
        auto root{cnmtSection->OpenDirectory("")};
        std::shared_ptr<vfs::Backing> cnmt;
        if (root != nullptr) {
            for (const auto &entry : root->Read()) {
                cnmt = cnmtSection->OpenFile(entry.name);
            }
        }

        header = cnmt->Read<PackagedContentMetaHeader>();
        if (header.contentMetaType >= ContentMetaType::Application && header.contentMetaType <= ContentMetaType::AddOnContent)
            optionalHeader = cnmt->Read<OptionalHeader>(sizeof(PackagedContentMetaHeader));

        for (u16 i = 0; i < header.contentCount; ++i)
            contentInfos.emplace_back(cnmt->Read<PackagedContentInfo>(sizeof(PackagedContentMetaHeader) + i * sizeof(PackagedContentInfo) +
                header.extendedHeaderSize));

        for (u16 i = 0; i < header.contentMetaCount; ++i)
            contentMetaInfos.emplace_back(cnmt->Read<ContentMetaInfo>(sizeof(PackagedContentMetaHeader) + i * sizeof(ContentMetaInfo) +
                header.extendedHeaderSize));
    }

    std::string CNMT::GetTitleId() {
        auto tilteId{header.id};
        return fmt::format("{:016X}", tilteId);
    }

    std::string CNMT::GetParentTitleId() {
        auto parentTilteId{optionalHeader.titleId};
        return fmt::format("{:016X}", parentTilteId);
    }

    ContentMetaType CNMT::GetContentMetaType() {
        return header.contentMetaType;
    }

}

