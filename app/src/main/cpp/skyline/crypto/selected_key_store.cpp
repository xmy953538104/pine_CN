// SPDX-License-Identifier: MPL-2.0
#include "selected_key_store.h"
#include <vfs/os_filesystem.h>
#include <sys/stat.h>
#include <cerrno>

namespace skyline::crypto {
namespace {
using namespace catalog::observed;
class ObservedRead final : public vfs::Backing {
    std::shared_ptr<vfs::Backing> backing;
    KeyFileState &state;
  protected:
    size_t ReadImpl(span<u8> output, size_t offset) override {
        try {
            const auto actual = backing->ReadUnchecked(output, offset);
            state.read = !backing->HasReliableReadByteCount() || actual > output.size() ? FileRead::Unknown :
                actual == output.size() ? FileRead::Complete : FileRead::Short;
            // Preserve the old Backing::Read short-read warning, total-size
            // return and zero-filled parser buffer. Never retry or reread.
            return actual;
        } catch (...) { state.read = FileRead::Failed; throw; }
    }
  public:
    ObservedRead(std::shared_ptr<vfs::Backing> value, KeyFileState &pState)
        : Backing({value->mode.read, false, false}, value->size), backing(std::move(value)), state(pState) {}
    bool HasReliableReadByteCount() const noexcept override { return backing->HasReliableReadByteCount(); }
};
FilePresence AbsentPresence(const std::string *rootPath, const char *name) {
    if (!rootPath) return FilePresence::CheckFailed;
    struct stat info{};
    const auto path = *rootPath + (rootPath->ends_with('/') ? "" : "/") + name;
    if (::stat(path.c_str(), &info) == 0) return FilePresence::Present;
    return errno == ENOENT || errno == ENOTDIR ? FilePresence::Missing : FilePresence::CheckFailed;
}
}
std::shared_ptr<KeyStore> SelectedKeyStoreFactory::LoadImpl(vfs::FileSystem &root, catalog::observed::KeyState &state,
                                                          const std::string *rootPath) {
    using namespace catalog::observed;
    auto keys = std::shared_ptr<KeyStore>(new KeyStore(KeyStore::ObservedEmptyTag{}));
    auto refresh = [&] {
        state.loadedUniqueTitleKeys = keys->LoadedTitleKeyCount();
        state.recognizedProdSlots = keys->RecognizedProdSlotCount();
    };
    refresh();
    auto load = [&](const char *name, KeyFileState &file, KeyStore::ReadPairsCallback callback) {
        try {
            // Identical FileExists/OpenFile order to the ordinary constructor.
            // Extra stat is diagnostic-only and only when FileExists is false.
            if (!root.FileExists(name)) {
                file.presence = AbsentPresence(rootPath, name); file.load = FileLoad::Skipped; return;
            }
            file.presence = FilePresence::Present;
            auto backing = root.OpenFile(name);
            keys->ReadPairs(std::make_shared<ObservedRead>(std::move(backing), file), callback);
            file.load = FileLoad::Loaded;
        } catch (...) {
            if (file.presence == FilePresence::NotChecked) file.presence = FilePresence::CheckFailed;
            file.load = FileLoad::Failed; throw;
        }
    };
    try {
        load("title.keys", state.titleKeys, &KeyStore::PopulateTitleKeys); refresh();
        load("prod.keys", state.prodKeys, &KeyStore::PopulateKeys); refresh();
        auto uncertain = [](const KeyFileState &file) {
            return file.presence == FilePresence::CheckFailed || file.read == FileRead::Short || file.read == FileRead::Unknown;
        };
        state.status = KeyLoad::Loaded; state.countsPartial = uncertain(state.titleKeys) || uncertain(state.prodKeys); return keys;
    } catch (...) { refresh(); state.status = KeyLoad::Failed; state.countsPartial = true; throw; }
}
std::shared_ptr<KeyStore> SelectedKeyStoreFactory::Load(const std::string &rootPath, catalog::observed::KeyState &state) {
    using namespace catalog::observed;
    state = {}; state.status = KeyLoad::Loading;
    try { vfs::OsFileSystem root(rootPath); return LoadImpl(root, state, &rootPath); }
    catch (...) { state.status = KeyLoad::Failed; state.countsPartial = true; throw; }
}
std::shared_ptr<KeyStore> SelectedKeyStoreFactory::LoadFrom(vfs::FileSystem &root, catalog::observed::KeyState &state) {
    state = {}; state.status = catalog::observed::KeyLoad::Loading;
    try { return LoadImpl(root, state, nullptr); }
    catch (...) { state.status = catalog::observed::KeyLoad::Failed; state.countsPartial = true; throw; }
}
}
