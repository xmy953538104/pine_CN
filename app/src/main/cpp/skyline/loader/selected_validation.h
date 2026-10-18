// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <catalog/wire.h>
#include <map>

namespace skyline::loader::selected {
    struct Limits {
        uint64_t maxMetadataBytes{128ULL * 1024 * 1024};
        uint64_t maxExecutableBytes{512ULL * 1024 * 1024};
        uint64_t maxSegmentBytes{256ULL * 1024 * 1024};
        uint32_t maxEntries{65536}, maxDepth{64}, maxNameBytes{1024};
        uint64_t maxNameBytesTotal{8ULL * 1024 * 1024};
    };
    struct Romfs {
        std::map<std::string, catalog::Source> files;
    };
    // Structural consistency only: neither signatures nor hardware execution
    // are authenticated by these checks.
    void ValidateNpdm(catalog::Source source, uint64_t programId, const Limits &limits);
    void ValidateNso(catalog::Source source, const Limits &limits, uint64_t &executableBytes);
    void ValidateExeFs(catalog::Source source, uint64_t programId, const Limits &limits);
    Romfs ValidateRomfs(catalog::Source source, const Limits &limits);
    void ValidateNacp(catalog::Source source);
}
