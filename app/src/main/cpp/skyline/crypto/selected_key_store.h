// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "key_store.h"
#include <catalog/scan_observations.h>

namespace skyline::vfs { class FileSystem; }
namespace skyline::crypto {
// Opt-in Selected inspection only. Reuses the original parser and callbacks;
// exceptions and first-title/last-prod duplicate precedence are preserved.
class SelectedKeyStoreFactory {
  public:
    static std::shared_ptr<KeyStore> Load(const std::string &rootPath, catalog::observed::KeyState &state);
    // The supplied filesystem is useful for bounded synthetic short-read/error
    // fixtures. A failed FileExists probe has unknown presence without OS stat.
    static std::shared_ptr<KeyStore> LoadFrom(vfs::FileSystem &root, catalog::observed::KeyState &state);
  private:
    static std::shared_ptr<KeyStore> LoadImpl(vfs::FileSystem &root, catalog::observed::KeyState &state,
                                            const std::string *rootPath);
};
}
