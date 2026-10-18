// SPDX-License-Identifier: MPL-2.0
// Actual POSIX descriptor reads plus the standalone, loader-free wire parser.
#include <catalog/catalog.h>
#include <loader/selected_launch.h>

#include <array>
#include <cassert>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <iostream>
#include <set>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace skyline;
static bool interruptNextRead{};
static size_t actualPreadCalls{};
extern "C" ssize_t __real_pread(int, void *, size_t, off_t);
extern "C" ssize_t __wrap_pread(int fd, void *out, size_t size, off_t offset) {
    if (interruptNextRead) { interruptNextRead = false; errno = EINTR; return -1; }
    ++actualPreadCalls;
    return __real_pread(fd, out, size, offset);
}
static void Reject(const std::function<void()> &action, const std::string &code) {
    bool caught{};
    try { action(); } catch (const catalog::Error &error) {
        caught = true; assert(error.code == code);
        assert(std::string(error.what()).find("PRIVATE") == std::string::npos);
    }
    assert(caught);
}
static std::set<int> Descriptors() {
    std::set<int> result;
    for (int fd{}; fd < 128; ++fd) if (fcntl(fd, F_GETFD) >= 0) result.insert(fd);
    return result;
}
static int Fixture(const std::string &path, const std::vector<uint8_t> &bytes) {
    int fd = open(path.c_str(), O_CREAT | O_TRUNC | O_RDWR | O_CLOEXEC, 0600); assert(fd >= 0);
    assert(write(fd, bytes.data(), bytes.size()) == static_cast<ssize_t>(bytes.size()));
    return fd;
}
static std::vector<uint8_t> EmptyPfs() { return {'P','F','S','0',0,0,0,0,0,0,0,0,0,0,0,0}; }
static std::string Serialize(catalog::Source source) { return catalog::ToJson(catalog::Scan(std::move(source))); }
static void DescriptorChecks(const std::string &folder) {
    const auto path = folder + "/synthetic-descriptor.bin";
    const std::vector<uint8_t> bytes{'a','b','c','d','e','f','g','h'};
    int borrowed = Fixture(path, bytes);
    assert(lseek(borrowed, 3, SEEK_SET) == 3);
    const auto initial = Descriptors();
    auto source = catalog::DuplicateFd(borrowed);
    auto after = Descriptors(); assert(after.size() == initial.size() + 1);
    int duplicate = -1; for (int fd : after) if (!initial.count(fd)) duplicate = fd;
    assert(duplicate >= 0 && duplicate != borrowed && (fcntl(duplicate, F_GETFD) & FD_CLOEXEC));
    interruptNextRead = true;
    assert(source->Bytes(0, bytes.size()) == bytes && !interruptNextRead);
    assert(lseek(borrowed, 0, SEEK_CUR) == 3);
    std::array<uint8_t,1> byte{};
    assert(source->Read(source->Size(), byte.data(), 0) == 0);
    const auto reads = actualPreadCalls;
    Reject([&] { source->Read(UINT64_MAX, byte.data(), 1); }, "out_of_range");
    Reject([&] { source->Read(source->Size(), byte.data(), 1); }, "out_of_range");
    assert(actualPreadCalls == reads);
    auto region = catalog::Region(source, 1, 4);
    assert(region->IdentityKind() == "duplicated_fd_stat_snapshot" && region->IdentityScope() == source->IdentityScope());
    assert(region->SnapshotToken() == source->SnapshotToken());
    assert(close(borrowed) == 0);
    source->CheckUnchanged(); source.reset();
    assert(region->Bytes(0, 4) == std::vector<uint8_t>({'b','c','d','e'}));
    assert(fcntl(duplicate, F_GETFD) >= 0); region.reset();
    assert(fcntl(duplicate, F_GETFD) < 0 && errno == EBADF);
    std::cout << "PASS duplicate CLOEXEC ownership, caller-close lifetime, Region forwarding, EINTR retry, exact counts/EOF/bounds and unchanged shared offset\n";

    borrowed = Fixture(path, bytes); source = catalog::DuplicateFd(borrowed);
    const uint8_t changed = 'X'; assert(pwrite(borrowed, &changed, 1, 0) == 1);
    struct stat snapshot{}; assert(fstat(borrowed, &snapshot) == 0);
    struct timespec times[]{snapshot.st_atim, snapshot.st_mtim}; ++times[1].tv_sec;
    assert(futimens(borrowed, times) == 0);
    Reject([&] { source->CheckUnchanged(); }, "input_changed");
    source = catalog::DuplicateFd(borrowed); assert(ftruncate(borrowed, 3) == 0);
    std::array<uint8_t,8> output{}; assert(source->Read(0, output.data(), output.size()) == 3);
    Reject([&] { source->Exact(0, output.data(), output.size()); }, "short_read");
    Reject([&] { source->CheckUnchanged(); }, "input_changed");
    source.reset(); assert(close(borrowed) == 0);
    std::cout << "PASS same-size mutation and truncation checks; real partial pread count and Exact short-read error\n";

    borrowed = Fixture(path, EmptyPfs()); assert(unlink(path.c_str()) == 0);
    source = catalog::DuplicateFd(borrowed); assert(close(borrowed) == 0);
    source->CheckUnchanged(); const auto json = Serialize(source);
    assert(json.find("\"identity_kind\":\"duplicated_fd_stat_snapshot\"") != std::string::npos);
    assert(json.find("no_named_path_check") != std::string::npos);
    assert(json.find("and_named_inode_checked") == std::string::npos);
    std::ofstream(folder + "/descriptor-catalog.json") << json;
    source.reset();
    borrowed = Fixture(path, EmptyPfs()); assert(close(borrowed) == 0);
    const auto fileJson = Serialize(catalog::OpenFile(path));
    assert(fileJson.find("\"identity_kind\":\"open_file_stat_snapshot\"") != std::string::npos);
    assert(fileJson.find("opened_source_stat_and_named_inode_checked_at_scan_and_serialization; not_a_content_hash") != std::string::npos);
    std::cout << "PASS unlinked descriptor catalog works without pathname reopen; FD/OpenFile JSON identity scopes remain distinct\n";

    auto failedAdmission = [&](int fd, const std::string &code) {
        const auto before = Descriptors(); const auto calls = actualPreadCalls;
        Reject([&] { catalog::DuplicateFd(fd); }, code);
        assert(Descriptors() == before && actualPreadCalls == calls);
    };
    failedAdmission(-1, "input_fd_invalid");
    int stale = open(path.c_str(), O_RDONLY | O_CLOEXEC); assert(stale >= 0); assert(close(stale) == 0);
    failedAdmission(stale, "input_fd_duplicate_failed");
    int writeOnly = open(path.c_str(), O_WRONLY | O_CLOEXEC); assert(writeOnly >= 0);
    failedAdmission(writeOnly, "input_fd_not_readable"); assert(fcntl(writeOnly, F_GETFD) >= 0); close(writeOnly);
#ifdef O_PATH
    int metadataOnly = open(path.c_str(), O_PATH | O_CLOEXEC); assert(metadataOnly >= 0);
    failedAdmission(metadataOnly, "input_fd_not_readable"); close(metadataOnly);
#endif
    int pipes[2]; assert(pipe(pipes) == 0);
    failedAdmission(pipes[0], "input_not_regular"); close(pipes[0]); close(pipes[1]);
    int sockets[2]; assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    failedAdmission(sockets[0], "input_not_regular"); close(sockets[0]); close(sockets[1]);
    int directory = open(folder.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC); assert(directory >= 0);
    failedAdmission(directory, "input_not_regular"); close(directory);
    assert(unlink(path.c_str()) == 0);
    std::cout << "PASS invalid/write-only/nonregular descriptors rejected before reads with exact duplicate cleanup and caller ownership preserved\n";
}

static std::string Group(char digit) { return "cnmt-sha256:" + std::string(64, digit); }
static std::vector<std::string> Fields() {
    return {"1","application","modern-40","010000000000ABCD",Group('A'),std::string(32,'B'),"010000000000ABCD","","",""};
}
static void WireChecks() {
    const auto original = Fields();
    auto parsed = loader::ParseSelectedLaunchFields(original, {255,-1});
    assert(!parsed.standalone && !parsed.update && parsed.applicationId == 0x010000000000abcd && parsed.primary.programId == parsed.applicationId);
    assert(parsed.primary.groupId == Group('a') && parsed.primary.contentId == std::string(32,'b') && parsed.primary.programIndex == 255);
    auto fields = original; fields[1] = "standalone_patch"; fields[2] = "legacy-48";
    parsed = loader::ParseSelectedLaunchFields(fields,{0,-1}); assert(parsed.standalone && parsed.profile == catalog::ContentInfoProfile::Legacy48);
    fields = original; fields[7] = Group('C'); fields[8] = std::string(32,'D'); fields[9] = "FFFFFFFFFFFFFFFF";
    for (size_t i{}; i < 32; ++i) { auto group = Group('0'); group.back() = "0123456789abcdef"[i % 16]; group[group.size()-2] = "0123456789abcdef"[i / 16]; fields.push_back(group); }
    parsed = loader::ParseSelectedLaunchFields(fields,{1,255});
    assert(parsed.update && parsed.update->programId == UINT64_MAX && parsed.update->programIndex == 255 && parsed.addOnGroups.size() == 32);
    std::cout << "PASS exact application/standalone profiles, case-normalized IDs, optional update, full-width uint64 IDs and 32 explicit AOCs\n";

    auto reject = [](const std::vector<std::string> &bad, const std::vector<int32_t> &indices = {0,-1}) {
        Reject([&] { loader::ParseSelectedLaunchFields(bad,indices); }, "invalid_selected_launch");
    };
    reject({}); reject(std::vector<std::string>(9)); reject(std::vector<std::string>(43,"PRIVATE"));
    reject(original,{}); reject(original,{0}); reject(original,{0,-1,0});
    for (auto indices : {std::vector<int32_t>{-1,-1},{256,-1},{0,0},{0,-2},{INT32_MIN,-1},{INT32_MAX,-1}}) reject(original,indices);
    for (auto [at,value] : std::vector<std::pair<size_t,std::string>>{{0,"2"},{0,""},{1,"auto"},{2,"unknown"},{2,"modern-41"},{3,std::string(16,'0')},{3,"1"},{3,"FFFFFFFFFFFFFFFFF"},{4,"nca-sha256:"+std::string(64,'a')},{4,"cnmt-sha256:"+std::string(63,'a')},{5,std::string(32,'0')},{5,std::string(31,'b')},{6,std::string(16,'0')},{6,"010000000000ABCZ"}}) { auto bad = original; bad[at] = value; reject(bad); }
    for (auto invalid : {std::string(" "),std::string("\t"),std::string("\n"),std::string("\0",1),std::string("\xc3\xa9",2),std::string("\x7f",1)}) { auto bad = original; bad[3] += invalid; reject(bad); }
    auto large = original; large[4] = std::string(65537,'A'); reject(large);
    for (size_t present = 1; present < 7; ++present) {
        auto bad = original;
        if (present & 1) bad[7] = Group('c');
        if (present & 2) bad[8] = std::string(32,'d');
        if (present & 4) bad[9] = "0100000000000001";
        reject(bad,{0,0});
    }
    auto update = original; update[7] = Group('c'); update[8] = std::string(32,'d'); update[9] = "0100000000000001";
    reject(update,{0,-1}); reject(update,{0,256}); update[1] = "standalone_patch"; reject(update,{0,0});
    update[1] = "application"; update[7] = Group('a'); reject(update,{0,0});
    auto duplicates = original; duplicates.push_back(Group('a')); reject(duplicates);
    duplicates = original; duplicates.push_back(Group('c')); duplicates.push_back(Group('C')); reject(duplicates);
    update[7] = Group('c'); update.push_back(Group('C')); reject(update,{0,0});
    auto emptyAoc = original; emptyAoc.push_back(""); reject(emptyAoc);
    std::cout << "PASS versions/enums/profiles/counts/budgets, ASCII and exact hex, partial/standalone update, indices and normalized duplicate groups rejected without field echo\n";
}

int main(int argc, char **argv) {
    assert(argc == 2);
    DescriptorChecks(argv[1]); WireChecks();
    std::cout << "PASS standalone boundary primitives; no catalog selection, loader execution, Android or game claim\n";
}
