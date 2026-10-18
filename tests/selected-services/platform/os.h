#pragma once
#include <common.h>
#include <crypto/key_store.h>
#include <vfs/filesystem.h>
#include <loader/loader.h>
namespace skyline::kernel {
class OS {
public:
 std::shared_ptr<crypto::KeyStore>keyStore;
 DeviceState state;
 std::string publicAppFilesPath;
 std::string privateAppFilesPath;
 std::shared_ptr<vfs::FileSystem>assetFileSystem;
 explicit OS(const std::filesystem::path &root)
     : publicAppFilesPath((root / "public").string()),
       privateAppFilesPath((root / "private").string() + "/") {state.os=this;}
};
}
