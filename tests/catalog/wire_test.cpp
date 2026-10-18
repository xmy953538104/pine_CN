// SPDX-License-Identifier: MPL-2.0
#include <catalog/catalog.h>
#include <cassert>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <unistd.h>

using namespace skyline::catalog;
class Memory final : public Reader {
  public:
    std::vector<uint8_t> bytes;
    bool shortRead{};
    explicit Memory(std::vector<uint8_t> data) : bytes(std::move(data)) {}
    uint64_t Size() const override { return bytes.size(); }
    size_t Read(uint64_t at, uint8_t *out, size_t count) const override {
        Range(at, count, bytes.size());
        if (count && shortRead) --count;
        std::memcpy(out, bytes.data() + at, count);
        return count;
    }
};
static void Reject(const std::function<void()> &test, const std::string &code) {
    try { test(); assert(false); } catch (const Error &error) { assert(error.code == code); }
}
int main(int argc, char **argv) {
    assert(argc == 2);
    Reject([] { Add(UINT64_MAX, 1); }, "integer_overflow");
    Reject([] { Multiply(UINT64_MAX, 2); }, "integer_overflow");
    Reject([] { Range(UINT64_MAX, 2, UINT64_MAX); }, "out_of_range");
    auto memory = std::make_shared<Memory>(std::vector<uint8_t>{1,2,3,4});
    Reject([&] { memory->Bytes(1, std::numeric_limits<size_t>::max()); }, "out_of_range");
    Reject([&] { Region(memory, UINT64_MAX, 1); }, "out_of_range");
    auto region = Region(memory, 1, 2);
    assert(region->Bytes(0, 2) == std::vector<uint8_t>({2,3}));
    Reject([&] { region->Bytes(2, 1); }, "out_of_range");
    memory->shortRead = true;
    Reject([&] { memory->Bytes(0, 4); }, "short_read");
    auto empty = Sha256(nullptr, 0);
    assert(Hex(empty.data(), 32) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    const uint8_t abc[]{'a','b','c'}; auto digest = Sha256(abc, 3);
    assert(Hex(digest.data(), 32) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    std::vector<uint8_t> million(1000000, 'a'); digest = Sha256(million.data(), million.size());
    assert(Hex(digest.data(), 32) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    auto write = [](const std::string &path, const std::string &bytes) { std::ofstream file(path, std::ios::binary); file << bytes; };
    std::string path = argv[1]; write(path, "four");
    auto file = OpenFile(path); assert(!file->SnapshotToken().empty());
    write(path, "x");
    Reject([&] { file->Bytes(0, 4); }, "short_read");
    Reject([&] { file->CheckUnchanged(); }, "input_changed");
    write(path, "four"); file = OpenFile(path);
    write(path + ".new", "four"); assert(rename((path + ".new").c_str(), path.c_str()) == 0);
    Reject([&] { file->CheckUnchanged(); }, "input_changed");
    unlink(path.c_str());
}
