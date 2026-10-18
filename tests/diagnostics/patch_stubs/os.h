#pragma once
#include <vfs/nca.h>
namespace skyline {
struct PatchTestLoader { std::optional<vfs::NCA> programNca; std::string diagnosticProgramContentId; };
struct PatchTestOS { std::shared_ptr<crypto::KeyStore> keyStore; };
struct DeviceState {
    std::shared_ptr<PatchTestLoader> loader, updateLoader;
    std::shared_ptr<PatchTestOS> os;
};
}
