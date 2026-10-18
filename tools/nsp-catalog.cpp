// SPDX-License-Identifier: MPL-2.0
#include <catalog/catalog.h>
#include <iostream>
#include <limits>

namespace {
    uint64_t Integer(const std::string &text) {
        if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos) throw skyline::catalog::Error("invalid_option", "budget must be an unsigned decimal integer");
        try { size_t end{}; auto value = std::stoull(text, &end); if (end == text.size()) return value; }
        catch (const std::exception &) {}
        throw skyline::catalog::Error("invalid_option", "budget is out of range");
    }
    uint32_t Count(const std::string &text) {
        auto value = Integer(text);
        if (value > std::numeric_limits<uint32_t>::max()) throw skyline::catalog::Error("invalid_option", "count budget exceeds uint32");
        return static_cast<uint32_t>(value);
    }
}
int main(int argc, char **argv) {
    using namespace skyline::catalog;
    try {
        Options options;
        std::string input;
        for (int i{1}; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--help") {
                std::cout << "Usage: nsp-catalog [--profile modern-40|legacy-48|unknown] [--verify-hashes] [--max-hash-bytes N] [--max-metadata-bytes N] [--max-cnmt-files N] [--max-pfs-entries N] [--max-association-work N] INPUT.nsp\n"
                    "Read-only host catalog. Encrypted NCA headers/sections are listed as unreadable/unsupported. No keys, signature verification, AOC registration or game launch. Default ContentInfo profile is unknown.\n";
                return 0;
            }
            if (arg == "--verify-hashes") options.verifyHashes = true;
            else if (arg == "--profile" || arg == "--max-hash-bytes" || arg == "--max-metadata-bytes" || arg == "--max-cnmt-files" || arg == "--max-pfs-entries" || arg == "--max-association-work") {
                if (++i == argc) throw Error("invalid_option", "option requires a value");
                std::string value = argv[i];
                if (arg == "--profile") {
                    if (value == "modern-40") options.profile = ContentInfoProfile::Modern40;
                    else if (value == "legacy-48") options.profile = ContentInfoProfile::Legacy48;
                    else if (value == "unknown") options.profile = ContentInfoProfile::Unknown;
                    else throw Error("invalid_option", "unrecognized explicit ContentInfo profile");
                } else if (arg == "--max-hash-bytes") options.limits.maxHashBytes = Integer(value);
                else if (arg == "--max-metadata-bytes") options.limits.maxMetadataBytes = Integer(value);
                else if (arg == "--max-cnmt-files") options.limits.maxCnmtFiles = Count(value);
                else if (arg == "--max-association-work") options.limits.maxAssociationWork = Integer(value);
                else options.limits.maxPfsEntries = Count(value);
            } else if (arg.rfind("--", 0) == 0 || !input.empty()) throw Error("invalid_option", "unknown option or multiple inputs");
            else input = arg;
        }
        if (input.empty()) throw Error("invalid_option", "one NSP input is required; use --help");
        auto catalog = Scan(OpenFile(input), options);
        std::cout << ToJson(catalog);
        return 0; // Member failures are structured catalog results, not hidden.
    } catch (const Error &error) {
        std::cout << "{\"schema_version\":1,\"fatal\":{\"code\":" << Json(error.code) << ",\"detail\":" << Json(error.what()) << "}}\n";
        return 2;
    } catch (const std::exception &) {
        std::cerr << "Catalog failed without a complete result.\n";
        return 3;
    }
}
