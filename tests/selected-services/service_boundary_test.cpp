// SPDX-License-Identifier: MPL-2.0
// Public synthetic content only. The IPC client below writes literal wire bytes;
// request decoding, service dispatch and response encoding are production code.
#include "fixtures.h"
#include <loader/selected_content.h>
#include <loader/nca.h>
#include <os.h>
#include <kernel/types/KProcess.h>
#include <services/aocsrv/IAddOnContentManager.h>
#include <services/fssrv/IFileSystemProxy.h>
#include <services/fssrv/IStorage.h>
#include <diagnostics/diagnostics.h>
#include <iostream>

using namespace skyline;
using namespace fixture;
namespace skyline { std::vector<void *> exception::GetStackFrames() { return {}; } }
namespace skyline::loader {
void *NcaLoader::LoadExeFs(Loader *, const std::shared_ptr<vfs::FileSystem> &,
                         const std::shared_ptr<kernel::type::KProcess> &, const DeviceState &) {
    throw std::runtime_error("guest executable mapping must not run in service tests");
}
}
namespace {
class TestAssets final : public vfs::FileSystem {
  public:
    std::map<std::string, std::shared_ptr<vfs::Backing>> files;
    std::vector<std::string> attempted;
  private:
    std::shared_ptr<vfs::Backing> OpenFileImpl(const std::string &path, vfs::Backing::Mode) override {
        attempted.push_back(path);
        const auto item = files.find(path);
        return item == files.end() ? nullptr : item->second;
    }
    std::optional<vfs::Directory::EntryType> GetEntryTypeImpl(const std::string &) override { return {}; }
};
class TestBacking final : public vfs::Backing {
    Bytes bytes;
    size_t ReadImpl(span<u8> output, size_t offset) override {
        Check(offset <= bytes.size() && output.size() <= bytes.size() - offset, "test backing bounds");
        std::copy_n(bytes.data() + offset, output.size(), output.data());
        return output.size();
    }
  public:
    explicit TestBacking(Bytes data) : Backing({true, false, false}, data.size()), bytes(std::move(data)) {}
    bool HasReliableReadByteCount() const noexcept override { return true; }
};
loader::ProgramChoice Choose(const Catalog &catalog, const Group &group) {
    for (const auto &ref : group.contents)
        if (ref.type == 1) return {group.id, ref.contentId, ref.idOffset, catalog.ncas.at(*ref.nca).programId};
    throw std::runtime_error("synthetic Program absent");
}
alignas(16) std::array<uint8_t, 0x100> tls;
size_t passes{};
void Pass(const std::string &name) { ++passes; std::cout << "PASS " << name << '\n'; }
void WirePut(size_t offset, uint64_t value, size_t width) {
    for (size_t i{}; i < width; ++i, value >>= 8) tls.at(offset + i) = static_cast<uint8_t>(value);
}
uint64_t ReadLe(const uint8_t *bytes, size_t width) {
    uint64_t value{};
    for (size_t i{}; i < width; ++i) value |= static_cast<uint64_t>(bytes[i]) << (8 * i);
    return value;
}
uint64_t Reply(size_t at, size_t width) { return ReadLe(tls.data() + at, width); }
void WireRequest(uint32_t command, const Bytes &args = {}, uint8_t *out = nullptr, uint16_t size = 0) {
    Check(args.size() <= 0x60, "test IPC arguments exceed bounded client");
    tls.fill(0);
    WirePut(0, 4, 2); // HIPC Request, no input descriptors or handles.
    const auto rawWords = (24 + args.size() + 3) / 4;
    WirePut(4, rawWords | (out ? 2U << 10 : 0), 4);
    std::memcpy(tls.data() + 16, "SFCI", 4);
    WirePut(24, command, 4);
    std::copy(args.begin(), args.end(), tls.begin() + 32);
    if (out) {
        const auto pointer = reinterpret_cast<uintptr_t>(out);
        Check(pointer < (uint64_t{1} << 48) && size > 0, "host pointer must fit literal Type-C descriptor");
        const auto at = 8 + rawWords * 4;
        WirePut(at, pointer, 6);
        WirePut(at + 6, size, 2);
    }
}
void Success() {
    Check(std::memcmp(tls.data() + 16, "SFCO", 4) == 0 && Reply(24, 4) == 0,
          "real response header/result mismatch");
}
void Call(service::ServiceManager &manager, KHandle handle, uint32_t command, const Bytes &args = {},
          uint8_t *out = nullptr, uint16_t size = 0) {
    WireRequest(command, args, out, size);
    manager.SyncRequestHandler(handle);
    Success();
}
KHandle MovedHandle() {
    Check((Reply(4, 4) & (1U << 31)) != 0 && Reply(8, 4) == 0x20,
          "real response must carry exactly one move handle");
    const auto handle = static_cast<KHandle>(Reply(12, 4));
    Check(handle != 0, "registered storage handle missing");
    return handle;
}
Bytes ReadStorage(service::ServiceManager &manager, KHandle handle, uint64_t offset, uint16_t count) {
    Bytes args(16), result(count);
    Put(args, 0, offset, 8); Put(args, 8, count, 8);
    Call(manager, handle, 0, args, result.data(), count);
    return result;
}
std::string Marker(service::ServiceManager &manager, KHandle handle, const Bytes &expectedRomfs, const std::string &marker) {
    Call(manager, handle, 4);
    Check(Reply(32, 8) == expectedRomfs.size(), "IStorage GetSize must match selected RomFS size");
    auto header = ReadStorage(manager, handle, 0, 0x50);
    Check(header == Bytes(expectedRomfs.begin(), expectedRomfs.begin() + 0x50), "IStorage actual RomFS header differs");
    const auto dataOffset = ReadLe(header.data() + 0x48, 8);
    auto bytes = ReadStorage(manager, handle, dataOffset, static_cast<uint16_t>(marker.size()));
    Check(bytes == Text(marker), "IStorage must return the selected member's distinct public marker");
    return {bytes.begin(), bytes.end()};
}
void Case(const std::filesystem::path &root, const std::filesystem::path &evidence,
          const std::string &name, bool update, bool standalone, bool diagnosticsOn) {
    const auto caseRoot = root / name;
    std::filesystem::create_directories(caseRoot / "private/keys");
    std::filesystem::create_directories(caseRoot / "public/switch/nand/system/Contents/registered");
    fixture::keyRoot = (caseRoot / "private/keys").string();
    kernel::OS os(caseRoot);
    auto assets = std::make_shared<TestAssets>();
    os.assetFileSystem = assets;
    Package package; package.withPatch = update; package.standalone = standalone;
    auto source = std::make_shared<MutableSource>(Pfs(package.Build()));
    auto keys = Keys(); os.keyStore = keys;
    auto catalog = ScanNative(source, keys, OptionsForTest());
    Check(catalog.complete && catalog.AllMetadataEnumerated(), "synthetic catalog must be fully enumerated");
    loader::SelectionRequest request;
    request.profile = catalog.options.profile; request.applicationId = App;
    request.mode = standalone ? loader::SelectedMode::StandaloneRebuiltPatch : loader::SelectedMode::Application;
    for (const auto &group : catalog.groups) {
        if (group.type == 0x80 || (standalone && group.type == 0x81)) request.primary = Choose(catalog, group);
        else if (group.type == 0x81) request.update = Choose(catalog, group);
        else if (group.type == 0x82) request.addOnGroups.push_back(group.id);
    }
    auto selected = loader::PrepareSelectedContent(catalog, request, keys);
    selected->Install(os.state);
    Check(os.state.loader->cnmt->header.contentMetaType == (standalone ? vfs::ContentMetaType::Patch : vfs::ContentMetaType::Application),
          "Install changed the true primary CNMT role");
    os.state.process = std::make_shared<kernel::type::KProcess>(os.state);
    os.state.thread = std::make_shared<kernel::type::KThread>();
    os.state.thread->tlsRegion = tls.data();
    if (diagnosticsOn) {
        diagnostics::Config config;
        config.directory = (evidence / "diagnostics" / name).string();
        config.sessionId = "service-" + name;
        config.queueCapacity = 4096;
        Check(diagnostics::Start(config), "actual diagnostics Start failed");
    } else Check(!diagnostics::Enabled(), "diagnostics off case unexpectedly enabled");
    service::ServiceManager manager(os.state);
    std::shared_ptr<service::BaseService> aoc = std::make_shared<service::aocsrv::IAddOnContentManager>(os.state, manager);
    const auto aocHandle = os.state.process->NewHandle<kernel::type::KSession>(aoc).handle;
    WireRequest(2);
    // Independent literal HIPC/CMIF golden, including raw-size and command ID.
    const std::array<uint8_t, 32> countRequest{4,0,0,0,6,0,0,0,0,0,0,0,0,0,0,0,'S','F','C','I',0,0,0,0,2};
    Check(std::equal(countRequest.begin(), countRequest.end(), tls.begin()), "Count literal request golden differs");
    manager.SyncRequestHandler(aocHandle); Success();
    const std::array<uint8_t, 36> countResponse{0,0,0,0,9,0,0,0,0,0,0,0,0,0,0,0,'S','F','C','O',1,0,0,0,0,0,0,0,0,0,0,0,2};
    Check(std::equal(countResponse.begin(), countResponse.end(), tls.begin()), "Count literal response golden differs");
    Pass(name + " actual IPC Count/response golden=2");
    std::array<uint8_t, 8> listOut{};
    Bytes list(16); Put(list, 4, 2, 4);
    Call(manager, aocHandle, 3, list, listOut.data(), listOut.size());
    Check(Reply(32, 4) == 2 && ReadLe(listOut.data(), 4) == 1 && ReadLe(listOut.data() + 4, 4) == 2, "AOC List identities differ");
    Pass(name + " actual AOC List indices=1,2");
    Put(list, 0, 1, 4); Put(list, 4, 8, 4); listOut.fill(0);
    Call(manager, aocHandle, 3, list, listOut.data(), listOut.size());
    Check(Reply(32, 4) == 1 && ReadLe(listOut.data(), 4) == 2, "AOC safe pagination differs");
    Pass(name + " actual AOC safe pagination offset=1 count=8 returns=2");
    Call(manager, aocHandle, 5);
    Check(Reply(32, 8) == App + 0x1000, "actual NACP AOC base differs");
    Pass(name + " actual AOC GetBase from primary NACP");
    std::shared_ptr<service::BaseService> fs = std::make_shared<service::fssrv::IFileSystemProxy>(os.state, manager);
    const auto fsHandle = os.state.process->NewHandle<kernel::type::KSession>(fs).handle;
    Bytes processId(8); Put(processId, 0, 0x1234, 8);
    Call(manager, fsHandle, 1, processId);
    Pass(name + " actual FSP SetCurrentProcess response");
    for (size_t i{}; i < 2; ++i) {
        const auto id = Aoc1 + i;
        Check(os.state.dlcLoaders.at(i)->cnmt->header.id == id && os.state.dlcLoaders.at(i)->publicNca.has_value(),
              "selected DLC does not own its exact CNMT/PublicData");
        Bytes args(16); args[0] = 5; Put(args, 8, id, 8);
        const auto before = os.state.process->HandleCount();
        Call(manager, fsHandle, 0xca, args);
        const auto handle = MovedHandle();
        Check(os.state.process->HandleCount() == before + 1, "DataID registration count differs");
        const auto marker = "aoc" + std::to_string(i + 1);
        Marker(manager, handle, Romfs({{"marker", Text(marker)}}), marker);
        Pass(name + " actual DataID/registered IStorage distinct marker=" + marker);
    }
    Call(manager, fsHandle, 0xc8);
    const auto current = MovedHandle();
    const auto currentMarker = update || standalone ? "patch" : "base";
    Marker(manager, current, update || standalone ? package.patchRomfs : package.appRomfs, currentMarker);
    Pass(name + " actual CurrentProcess IStorage marker=" + currentMarker + (standalone ? " true_CNMT=Patch" : ""));
    constexpr uint64_t assetId = 0x01000000000ff001, missingId = assetId + 1;
    const auto assetPath = fmt::format("romfs/{:016X}", assetId);
    assets->files.emplace(assetPath, std::make_shared<TestBacking>(Text("public-asset")));
    Bytes args(16); args[0] = 5; Put(args, 8, assetId, 8);
    Call(manager, fsHandle, 0xca, args);
    auto assetHandle = MovedHandle();
    Check(ReadStorage(manager, assetHandle, 0, 12) == Text("public-asset"), "controlled fixture asset differs");
    Check(assets->attempted.back() == assetPath, "actual asset lookup case/name differs");
    Pass(name + " actual unmatched DataID empty-tmp-archives then controlled asset");
    Put(args, 8, missingId, 8);
    const auto before = os.state.process->HandleCount();
    WireRequest(0xca, args);
    bool threw{};
    try { manager.SyncRequestHandler(fsHandle); } catch (const skyline::exception &) { threw = true; }
    Check(threw && os.state.process->HandleCount() == before, "missing asset must normally throw without registering storage");
    Check(assets->attempted.back() == fmt::format("romfs/{:016X}", missingId), "missing fixture lookup differs");
    Pass(name + " actual FileSystem OpenFile missing asset normal exception/no registered handle");
    if (diagnosticsOn) { Check(diagnostics::Flush(2000), "actual diagnostics flush failed"); diagnostics::Stop(); }
    std::ofstream(evidence / (name + ".json")) << "{\"primary_cnmt_role\":\"" << (standalone ? "patch" : "application")
        << "\",\"current_marker\":\"" << currentMarker << "\",\"aoc_markers\":[\"aoc1\",\"aoc2\"],"
        << "\"aoc_indices\":[1,2],\"missing_asset\":\"exception_no_handle\",\"guest_execution\":\"not_performed\"}\n";
    os.state.thread.reset();
}
}
int main(int argc, char **argv) {
    try {
        std::cout.setf(std::ios::unitbuf);
        Check(argc == 4, "usage: service-test explicit-tmp-root evidence-dir diagnostics-on|off");
        const auto root = std::filesystem::path(argv[1]), evidence = std::filesystem::path(argv[2]);
        const bool diag = std::string(argv[3]) == "on";
        Check(diag || std::string(argv[3]) == "off", "unknown diagnostics mode");
        std::filesystem::create_directories(evidence);
        Case(root, evidence, "application", false, false, diag);
        Case(root, evidence, "update", true, false, diag);
        Case(root, evidence, "standalone", false, true, diag);
        std::cout << "SCENARIOS " << passes << " diagnostics=" << (diag ? "on" : "off") << '\n';
        return 0;
    } catch (const std::exception &error) {
        diagnostics::Stop();
        std::cerr << "synthetic service test failed: " << error.what() << '\n';
        return 1;
    }
}
