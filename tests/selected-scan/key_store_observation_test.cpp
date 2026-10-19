// SPDX-License-Identifier: MPL-2.0
// Actual unmodified KeyStore parser + Selected-only observation, synthetic roots.
#include "fixtures.h"
#include <crypto/selected_key_store.h>
#include <catalog/scan_report.h>
#include <vfs/filesystem.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <typeindex>

namespace {
using namespace fixture;
using namespace skyline::catalog::observed;
void Write(const std::filesystem::path &path, const std::string &data) {
    std::filesystem::create_directories(path.parent_path()); std::ofstream file(path, std::ios::binary); file << data;
    Check(bool(file), "synthetic key-state write");
}
struct Attempt {
    std::shared_ptr<crypto::KeyStore> keys;
    std::type_index errorType{typeid(void)};
    std::string errorText;
};
template<class F> Attempt Try(F action) {
    try { return {action(), typeid(void), {}}; }
    catch (const std::exception &error) { return {nullptr, typeid(error), error.what()}; }
}
void Same(const Attempt &ordinary, const Attempt &observed) {
    Check(bool(ordinary.keys) == bool(observed.keys), "constructor success equivalence");
    Check(ordinary.errorType == observed.errorType && ordinary.errorText == observed.errorText, "constructor exception equivalence");
    if (ordinary.keys) {
        Check(ordinary.keys->LoadedTitleKeyCount() == observed.keys->LoadedTitleKeyCount(), "unique map count equivalence");
        Check(ordinary.keys->RecognizedProdSlotCount() == observed.keys->RecognizedProdSlotCount(), "recognized slot count equivalence");
        Check(ordinary.keys->headerKey == observed.keys->headerKey && ordinary.keys->titleKek == observed.keys->titleKek &&
              ordinary.keys->areaKeyApplication == observed.keys->areaKeyApplication && ordinary.keys->areaKeyOcean == observed.keys->areaKeyOcean &&
              ordinary.keys->areaKeySystem == observed.keys->areaKeySystem, "recognized values equivalence");
    }
}
class MemoryBacking final : public vfs::Backing {
    Bytes bytes;
  protected:
    size_t ReadImpl(span<u8> out, size_t offset) override {
        ++reads;
        if (fail) throw std::runtime_error("PRIVATE_READ_PATH_KEY_MATERIAL");
        const auto actual = std::min({out.size(), limit, bytes.size() - offset});
        std::copy_n(bytes.data() + offset, actual, out.data()); return actual;
    }
  public:
    size_t reads{}, limit{std::numeric_limits<size_t>::max()}; bool fail{}, reliable{true};
    explicit MemoryBacking(std::string text) : Backing({true, false, false}, text.size()), bytes(text.begin(), text.end()) {}
    bool HasReliableReadByteCount() const noexcept override { return reliable; }
};
class MemoryFs final : public vfs::FileSystem {
  protected:
    std::shared_ptr<vfs::Backing> OpenFileImpl(const std::string &path, vfs::Backing::Mode) override {
        ++opens;
        if (openFail) throw std::runtime_error("PRIVATE_OPEN_PATH");
        return path == "title.keys" ? title : prod;
    }
    std::optional<vfs::Directory::EntryType> GetEntryTypeImpl(const std::string &path) override {
        if (probeFail) throw std::runtime_error("PRIVATE_PROBE_PATH");
        if (path == "title.keys" ? bool(title) : bool(prod)) return vfs::Directory::EntryType::File;
        return std::nullopt;
    }
  public:
    std::shared_ptr<MemoryBacking> title, prod;
    size_t opens{}; bool openFail{}, probeFail{};
};
void Save(const std::filesystem::path &evidence, const char *name, const KeyState &state) {
    ScanReport report; report.Keys() = state;
    report.Begin(Stage::ProfileValidation); report.Complete(Stage::ProfileValidation); report.Begin(Stage::KeyStoreLoad);
    if (state.status == KeyLoad::Failed) report.Fail("other_native_issue");
    else {
        report.Complete(Stage::KeyStoreLoad);
        for (auto stage : {Stage::FdDuplicate, Stage::CatalogScan, Stage::CatalogSerialize}) { report.Begin(stage); report.Complete(stage); }
        report.Finish();
    }
    const auto safe = report.ReportJson(); Check(safe.find("PRIVATE") == std::string::npos && safe.find(std::string(32, 'a')) == std::string::npos &&
        safe.find(std::string(32, '1')) == std::string::npos && safe.find(".keys") == std::string::npos, "safe key-state material/path canary");
    Write(evidence / (std::string(name) + ".json"), safe);
    std::cout << "PASS key-state " << name << '\n';
}
}

void RunKeyStateTests(const std::filesystem::path &root, const std::filesystem::path &evidence) {
    const std::string id(32, 'a'), first(32, '1'), second(32, '2');
    auto compare = [&](const std::string &name, const std::string &title, const std::string &prod, KeyState &state) {
        const auto folder = root / name;
        Write(folder / "title.keys", title); Write(folder / "prod.keys", prod);
        const auto ordinary = Try([&] { return std::make_shared<crypto::KeyStore>(folder.string()); });
        const auto observed = Try([&] { return crypto::SelectedKeyStoreFactory::Load(folder.string(), state); });
        Same(ordinary, observed); return observed;
    };
    KeyState state;
    auto result = compare("duplicates", id + '=' + first + '\n' + id + '=' + second + '\n',
        "header_key=" + std::string(64, '1') + "\nheader_key=" + std::string(64, '2') + "\nkey_area_key_application_00=" + first + "\nunknown_name=PRIVATE\n", state);
    Check(result.keys && state.loadedUniqueTitleKeys == 1 && state.recognizedProdSlots == 2 && !state.countsPartial, "duplicate counts");
    Check(result.keys->GetTitleKey(util::HexStringToArray<16>(id)) == util::HexStringToArray<16>(first), "first title duplicate wins");
    Check(result.keys->headerKey == util::HexStringToArray<32>(std::string(64, '2')), "last prod duplicate wins");
    Save(evidence, "duplicates-first-title-last-prod", state);

    for (const size_t width : {size_t(0), size_t(1), size_t(31), size_t(32), size_t(33), size_t(64)}) {
        result = compare("width-" + std::to_string(width), id + '=' + std::string(width, '1') + '\n', "", state);
        Check(bool(result.keys) == (width == 32), "strict native title value width");
        Check(state.loadedUniqueTitleKeys == (width == 32 ? 1 : 0), "strict native actual map count");
    }
    Save(evidence, "strict-widths", state);
    result = compare("nonhex", id + '=' + std::string(32, 'G') + '\n', "", state);
    Check(!result.keys && state.status == KeyLoad::Failed && state.loadedUniqueTitleKeys == 0, "strict native nonhex rejection");
    Save(evidence, "strict-nonhex", state);
    result = compare("partial-title", id + '=' + first + "\nPRIVATE_BAD_LINE\n", "header_key=" + std::string(64, '1') + '\n', state);
    Check(!result.keys && state.loadedUniqueTitleKeys == 1 && state.recognizedProdSlots == 0 && state.countsPartial &&
          state.titleKeys.load == FileLoad::Failed && state.prodKeys.load == FileLoad::NotAttempted, "partial title prefix preserves stop order");
    Save(evidence, "failed-title-prefix", state);
    result = compare("partial-prod", id + '=' + first + '\n', "header_key=" + std::string(64, '1') + "\nkey_area_key_application_00=G\n", state);
    Check(!result.keys && state.loadedUniqueTitleKeys == 1 && state.recognizedProdSlots == 1 && state.countsPartial, "partial recognized prod prefix");
    Save(evidence, "failed-prod-prefix", state);
    result = compare("unterminated", id + '=' + first, "header_key=" + std::string(64, '1'), state);
    Check(result.keys && state.loadedUniqueTitleKeys == 0 && state.recognizedProdSlots == 0, "unterminated final line remains ignored");
    Save(evidence, "unterminated-final-lines", state);
    result = compare("suffix", "", "titlekek_0g=" + first + "\ntitlekek_00trailing=" + second + '\n', state);
    Check(result.keys && state.recognizedProdSlots == 1 && result.keys->titleKek[0] == util::HexStringToArray<16>(first), "legacy stoul suffix behavior");
    Save(evidence, "legacy-prod-suffix", state);
    result = compare("empty", "", "", state); Check(result.keys && !state.countsPartial, "empty files parser equivalence");
    Save(evidence, "empty-key-files", state);
    result = compare("comments", "#PRIVATE_COMMENT\n", "", state); Check(!result.keys, "native comments remain strict parser failure");
    Save(evidence, "comments-only-rejected", state);

    const auto missing = root / "missing"; std::filesystem::create_directories(missing);
    auto keys = crypto::SelectedKeyStoreFactory::Load(missing.string(), state);
    Check(keys && state.titleKeys.presence == FilePresence::Missing && state.prodKeys.presence == FilePresence::Missing &&
          state.loadedUniqueTitleKeys == 0 && state.recognizedProdSlots == 0, "two fixed files missing");
    Save(evidence, "missing-key-files", state);
    const auto directory = root / "directory"; std::filesystem::create_directories(directory / "title.keys");
    keys = crypto::SelectedKeyStoreFactory::Load(directory.string(), state);
    Check(keys && state.titleKeys.presence == FilePresence::Present && state.titleKeys.load == FileLoad::Skipped &&
          state.titleKeys.read == FileRead::NotAttempted, "present directory is not readable/loadable evidence");
    Save(evidence, "present-directory-skipped", state);

    MemoryFs fs;
    const auto prefix = id + '=' + first + '\n'; fs.title = std::make_shared<MemoryBacking>(prefix + "UNREAD_PRIVATE_TRAILING_BYTES");
    fs.prod = std::make_shared<MemoryBacking>(""); fs.title->limit = prefix.size();
    keys = crypto::SelectedKeyStoreFactory::LoadFrom(fs, state);
    Check(keys && keys->LoadedTitleKeyCount() == 1 && fs.title->reads == 1 && state.titleKeys.read == FileRead::Short &&
          state.titleKeys.load == FileLoad::Loaded && state.countsPartial, "one short read retains parser success and prefix/zero-fill");
    Save(evidence, "short-read-loaded-prefix", state);
    fs.title = std::make_shared<MemoryBacking>(prefix + "BAD\nUNREAD_PRIVATE_TRAILING_BYTES"); fs.title->limit = prefix.size() + 4;
    auto failed = Try([&] { return crypto::SelectedKeyStoreFactory::LoadFrom(fs, state); });
    Check(!failed.keys && fs.title->reads == 1 && state.titleKeys.read == FileRead::Short && state.loadedUniqueTitleKeys == 1 &&
          state.titleKeys.load == FileLoad::Failed && state.countsPartial, "one short read retains parser failure after prefix");
    Save(evidence, "short-read-failed-prefix", state);
    fs.title = std::make_shared<MemoryBacking>(prefix); fs.title->reliable = false;
    keys = crypto::SelectedKeyStoreFactory::LoadFrom(fs, state);
    Check(keys && state.titleKeys.read == FileRead::Unknown && state.countsPartial && fs.title->reads == 1, "unknown read byte-count capability");
    Save(evidence, "unreliable-read-marked-unknown", state);
    fs.title = std::make_shared<MemoryBacking>(prefix); fs.title->fail = true;
    failed = Try([&] { return crypto::SelectedKeyStoreFactory::LoadFrom(fs, state); });
    Check(!failed.keys && state.titleKeys.read == FileRead::Failed && state.titleKeys.load == FileLoad::Failed && state.countsPartial, "controlled read exception");
    Save(evidence, "controlled-read-failure", state);
    fs.title->fail = false; fs.openFail = true;
    failed = Try([&] { return crypto::SelectedKeyStoreFactory::LoadFrom(fs, state); });
    Check(!failed.keys && state.titleKeys.presence == FilePresence::Present && state.titleKeys.read == FileRead::NotAttempted &&
          state.titleKeys.load == FileLoad::Failed && state.countsPartial, "controlled open exception");
    Save(evidence, "controlled-open-failure", state);
    fs.openFail = false; fs.probeFail = true;
    failed = Try([&] { return crypto::SelectedKeyStoreFactory::LoadFrom(fs, state); });
    Check(!failed.keys && state.titleKeys.presence == FilePresence::CheckFailed && state.titleKeys.read == FileRead::NotAttempted &&
          state.titleKeys.load == FileLoad::Failed && state.countsPartial, "controlled probe exception");
    Save(evidence, "controlled-probe-failure", state);
}
