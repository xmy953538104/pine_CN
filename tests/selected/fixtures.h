// SPDX-License-Identifier: MPL-2.0
// Generated synthetic bytes only. Default mode uses the explicitly controlled
// cipher from native_stubs; real mode independently encrypts with OpenSSL.
#pragma once
#include <catalog/native_adapter.h>
#include <crypto/key_store.h>
#include <cstring>
#include <functional>
#include <limits>
#include <set>
#ifdef PINE_CATALOG_TEST_REAL_AES
#include <openssl/evp.h>
#include <lz4.h>
#endif

namespace fixture {
using namespace skyline;
using namespace skyline::catalog;
using Bytes = std::vector<uint8_t>;
using Files = std::vector<std::pair<std::string, Bytes>>;
constexpr uint64_t App = 0x0100000000001000, Patch = App + 0x800, Aoc1 = App + 0x1001, Aoc2 = App + 0x1002;
inline void Check(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
inline void Put(Bytes &bytes, size_t at, uint64_t value, size_t width) {
    for (size_t i{}; i < width; ++i, value >>= 8) bytes.at(at + i) = static_cast<uint8_t>(value);
}
inline size_t Align(size_t size, size_t alignment) { return (size + alignment - 1) / alignment * alignment; }
inline Bytes Text(const std::string &text) { return {text.begin(), text.end()}; }
inline std::string Hash(const Bytes &bytes) { const auto value = Sha256(bytes.data(), bytes.size()); return Hex(value.data(), value.size()); }
inline Bytes Pfs(const Files &files) {
    size_t stringSize{}, payload{};
    for (const auto &[name, bytes] : files) { stringSize += name.size() + 1; payload += bytes.size(); }
    const auto strings = 16 + files.size() * 24, data = strings + stringSize;
    Bytes result(data + payload); std::memcpy(result.data(), "PFS0", 4);
    Put(result, 4, files.size(), 4); Put(result, 8, stringSize, 4);
    size_t nameOffset{}, fileOffset{};
    for (size_t i{}; i < files.size(); ++i) {
        const auto &[name, bytes] = files[i]; const auto at = 16 + i * 24;
        Put(result, at, fileOffset, 8); Put(result, at + 8, bytes.size(), 8); Put(result, at + 16, nameOffset, 4);
        std::copy(name.begin(), name.end(), result.begin() + strings + nameOffset);
        std::copy(bytes.begin(), bytes.end(), result.begin() + data + fileOffset);
        nameOffset += name.size() + 1; fileOffset += bytes.size();
    }
    return result;
}
inline Bytes Romfs(const Files &files) {
    Bytes dirs(0x18); Put(dirs, 4, 0xffffffff, 4); Put(dirs, 8, 0xffffffff, 4);
    Put(dirs, 0x0c, files.empty() ? 0xffffffff : 0, 4); Put(dirs, 0x10, 0xffffffff, 4);
    size_t tableSize{}, dataSize{};
    for (const auto &[name, bytes] : files) { tableSize += Align(0x20 + name.size(), 4); dataSize += bytes.size(); }
    const size_t dirHash = 0x50, dirTable = dirHash + 4, fileHash = dirTable + dirs.size(), fileTable = fileHash + 4;
    const auto data = Align(fileTable + tableSize, 16);
    Bytes result(data + dataSize); Put(result, 0, 0x50, 8);
    Put(result, 8, dirHash, 8); Put(result, 0x10, 4, 8); Put(result, 0x18, dirTable, 8); Put(result, 0x20, dirs.size(), 8);
    Put(result, 0x28, fileHash, 8); Put(result, 0x30, 4, 8); Put(result, 0x38, fileTable, 8); Put(result, 0x40, tableSize, 8); Put(result, 0x48, data, 8);
    Put(result, dirHash, 0, 4); Put(result, fileHash, files.empty() ? 0xffffffff : 0, 4);
    std::copy(dirs.begin(), dirs.end(), result.begin() + dirTable);
    size_t at{}, dataOffset{};
    for (size_t i{}; i < files.size(); ++i) {
        const auto &[name, bytes] = files[i]; const auto next = at + Align(0x20 + name.size(), 4);
        Put(result, fileTable + at + 4, i + 1 == files.size() ? 0xffffffff : next, 4);
        Put(result, fileTable + at + 8, dataOffset, 8); Put(result, fileTable + at + 0x10, bytes.size(), 8);
        Put(result, fileTable + at + 0x18, i ? at - Align(0x20 + files[i - 1].first.size(), 4) : 0xffffffff, 4); Put(result, fileTable + at + 0x1c, name.size(), 4);
        std::copy(name.begin(), name.end(), result.begin() + fileTable + at + 0x20);
        std::copy(bytes.begin(), bytes.end(), result.begin() + data + dataOffset); at = next; dataOffset += bytes.size();
    }
    if (!files.empty()) Put(result, fileHash, at - Align(0x20 + files.back().first.size(), 4), 4);
    return result;
}
inline Bytes Npdm(uint64_t id = App) {
    Bytes result(0x304); std::memcpy(result.data(), "META", 4);
    result[0x0c] = 7; result[0x0e] = 44; Put(result, 0x14, 0x1000, 4); Put(result, 0x1c, 0x200000, 4);
    std::memcpy(result.data() + 0x20, "Synthetic", 9);
    Put(result, 0x70, 0x2c0, 4); Put(result, 0x74, 0x44, 4); Put(result, 0x78, 0x80, 4); Put(result, 0x7c, 0x240, 4);
    std::memcpy(result.data() + 0x280, "ACID", 4); Put(result, 0x284, 0x140, 4);
    Put(result, 0x290, id, 8); Put(result, 0x298, id, 8);
    std::memcpy(result.data() + 0x2c0, "ACI0", 4); Put(result, 0x2d0, id, 8);
    Put(result, 0x2f0, 0x40, 4); Put(result, 0x2f4, 4, 4); Put(result, 0x300, 7 | (63 << 4) | (2 << 24), 4);
    return result;
}
inline Bytes Nso(uint8_t marker = 1, bool compressed = false) {
    const std::array<size_t, 3> sizes{0x40, 0x40, 0x20};
    std::array<Bytes, 3> parts{Bytes(sizes[0], marker), Bytes(sizes[1], static_cast<uint8_t>(marker + 1)), Bytes(sizes[2], 3)};
    Bytes result(0x100); std::memcpy(result.data(), "NSO0", 4); Put(result, 0x0c, 0x38 | (compressed ? 7 : 0), 4); Put(result, 0x3c, 0xfe0, 4);
    for (size_t i{}; i < 3; ++i) {
        const auto hash = Sha256(parts[i].data(), parts[i].size()); std::copy(hash.begin(), hash.end(), result.begin() + 0xa0 + i * 32);
        Bytes stored = parts[i];
        if (compressed) {
#ifdef PINE_CATALOG_TEST_REAL_AES
            Bytes encoded(static_cast<size_t>(LZ4_compressBound(static_cast<int>(stored.size()))));
            const auto count = LZ4_compress_default(reinterpret_cast<const char *>(stored.data()), reinterpret_cast<char *>(encoded.data()),
                                                   static_cast<int>(stored.size()), static_cast<int>(encoded.size()));
            Check(count > 0, "fixture LZ4 compression failed"); encoded.resize(static_cast<size_t>(count)); stored = std::move(encoded);
#else
            stored = {0xff}; // Default stand-in can only test rejection, never compression support.
#endif
        }
        Put(result, 0x10 + i * 0x10, result.size(), 4); Put(result, 0x14 + i * 0x10, i * 0x1000, 4); Put(result, 0x18 + i * 0x10, sizes[i], 4);
        Put(result, 0x60 + i * 4, stored.size(), 4); result.insert(result.end(), stored.begin(), stored.end());
    }
    return result;
}
inline Bytes Nacp() {
    Bytes result(0x4000); std::memcpy(result.data(), "Synthetic selected fixture", 26); std::memcpy(result.data() + 0x200, "Synthetic", 9);
    Put(result, 0x302c, 1, 4); std::memcpy(result.data() + 0x3060, "test1", 5);
    Put(result, 0x3070, App + 0x1000, 8); Put(result, 0x3078, App, 8); return result;
}
inline crypto::KeyStore::Key256 HeaderKey() { crypto::KeyStore::Key256 key{}; for (size_t i{}; i < key.size(); ++i) key[i] = i + 1; return key; }
inline crypto::KeyStore::Key128 AreaKey() { crypto::KeyStore::Key128 key{}; for (size_t i{}; i < key.size(); ++i) key[i] = 0x20 + i; return key; }
inline crypto::KeyStore::Key128 ContentKey() { crypto::KeyStore::Key128 key{}; for (size_t i{}; i < key.size(); ++i) key[i] = 0x40 + i; return key; }
inline auto Keys() { auto keys = std::make_shared<crypto::KeyStore>(); keys->headerKey = HeaderKey(); keys->areaKeyApplication[0] = AreaKey(); return keys; }
#ifdef PINE_CATALOG_TEST_REAL_AES
inline Bytes Encrypt(const EVP_CIPHER *cipher, const uint8_t *key, const uint8_t *iv, const uint8_t *data, size_t size) {
    using Context = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;
    Context context(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free); Check(static_cast<bool>(context), "fixture crypto allocation");
    Check(EVP_EncryptInit_ex(context.get(), cipher, nullptr, key, iv) == 1 && EVP_CIPHER_CTX_set_padding(context.get(), 0) == 1, "fixture crypto init");
    Bytes result(size + 16); int count{}, tail{};
    Check(EVP_EncryptUpdate(context.get(), result.data(), &count, data, static_cast<int>(size)) == 1 &&
          EVP_EncryptFinal_ex(context.get(), result.data() + count, &tail) == 1, "fixture encryption failed");
    result.resize(static_cast<size_t>(count + tail)); Check(result.size() == size, "fixture ciphertext size"); return result;
}
#endif
struct Section { bool pfs{}; Bytes bytes; bool bktr{}; };
inline Bytes Nca(uint8_t type, uint64_t id, const std::vector<Section> &sections) {
    Bytes result(0xc00); std::memcpy(result.data() + 0x200, "NCA3", 4); result[0x205] = type; Put(result, 0x210, id, 8);
#ifdef PINE_CATALOG_TEST_REAL_AES
    const auto area = AreaKey(), content = ContentKey(); const auto wrapped = Encrypt(EVP_aes_128_ecb(), area.data(), nullptr, content.data(), content.size());
    std::copy(wrapped.begin(), wrapped.end(), result.begin() + 0x320);
#endif
    for (size_t slot{}; slot < sections.size(); ++slot) {
        const auto &section = sections[slot]; Bytes fs(0x200), payload; Put(fs, 0, 2, 2); fs[2] = section.pfs ? 1 : 0; fs[3] = section.pfs ? 2 : 3; fs[4] = section.bktr ? 4 : 3;
        if (section.pfs) {
            const auto hashesSize = (section.bytes.size() + 0x1ff) / 0x200 * 32, dataOffset = Align(hashesSize, 0x200);
            payload.resize(Align(dataOffset + section.bytes.size(), 0x200));
            for (size_t at{}; at < section.bytes.size(); at += 0x200) {
                const auto hash = Sha256(section.bytes.data() + at, std::min<size_t>(0x200, section.bytes.size() - at));
                std::copy(hash.begin(), hash.end(), payload.begin() + at / 0x200 * 32);
            }
            const auto master = Sha256(payload.data(), hashesSize); std::copy(master.begin(), master.end(), fs.begin() + 8);
            Put(fs, 0x28, 0x200, 4); Put(fs, 0x2c, 2, 4); Put(fs, 0x38, hashesSize, 8); Put(fs, 0x40, dataOffset, 8); Put(fs, 0x48, section.bytes.size(), 8);
            std::copy(section.bytes.begin(), section.bytes.end(), payload.begin() + dataOffset);
        } else {
            // Six bounded IVFC descriptors; tree verification remains explicitly
            // not performed by the legacy backend or this structural profile.
            std::memcpy(fs.data() + 8, "IVFC", 4); Put(fs, 0x0c, 0x20000, 4); Put(fs, 0x10, 32, 4); Put(fs, 0x14, 7, 4);
            payload.resize(Align(0xa00 + section.bytes.size(), 0x200));
            for (size_t i{}; i < 6; ++i) { Put(fs, 0x18 + i * 0x18, i * 0x200, 8); Put(fs, 0x20 + i * 0x18, i == 5 ? section.bytes.size() : 32, 8); Put(fs, 0x28 + i * 0x18, 9, 4); }
            std::copy(section.bytes.begin(), section.bytes.end(), payload.begin() + 0xa00);
        }
        const auto offset = result.size(); Put(result, 0x240 + slot * 16, offset / 0x200, 4); Put(result, 0x244 + slot * 16, (offset + payload.size()) / 0x200, 4);
        const auto hash = Sha256(fs.data(), fs.size()); std::copy(hash.begin(), hash.end(), result.begin() + 0x280 + slot * 32);
        std::copy(fs.begin(), fs.end(), result.begin() + 0x400 + slot * 0x200);
#ifdef PINE_CATALOG_TEST_REAL_AES
        std::array<uint8_t, 16> iv{}; uint64_t counter = offset >> 4; for (size_t i{}; i < 8; ++i, counter >>= 8) iv[15 - i] = static_cast<uint8_t>(counter);
        payload = Encrypt(EVP_aes_128_ctr(), content.data(), iv.data(), payload.data(), payload.size());
#endif
        result.insert(result.end(), payload.begin(), payload.end());
    }
    Put(result, 0x208, result.size(), 8);
    for (size_t sector{}; sector < 2 + sections.size(); ++sector) {
#ifdef PINE_CATALOG_TEST_REAL_AES
        std::array<uint8_t, 16> tweak{}; tweak[15] = static_cast<uint8_t>(sector); const auto key = HeaderKey();
        const auto encoded = Encrypt(EVP_aes_128_xts(), key.data(), tweak.data(), result.data() + sector * 0x200, 0x200);
        std::copy(encoded.begin(), encoded.end(), result.begin() + sector * 0x200);
#else
        for (size_t i{}; i < 0x200; ++i) result[sector * 0x200 + i] ^= 0xa5;
#endif
    }
    return result;
}
struct Reference { uint8_t type{}, index{}; Bytes bytes; std::optional<std::string> contentId; };
inline Bytes Cnmt(uint8_t role, uint64_t id, uint64_t parent, const std::vector<Reference> &refs) {
    const size_t extended = role == 0x81 ? 0x18 : 0x10;
    Bytes result(0x20 + extended + refs.size() * 0x38); Put(result, 0, id, 8); Put(result, 8, 1, 4); result[12] = role;
    Put(result, 14, extended, 2); Put(result, 16, refs.size(), 2); result[0x15] = 5; result[0x17] = 1;
    Put(result, 0x20, role == 0x80 ? id + 0x800 : parent, 8);
    for (size_t i{}; i < refs.size(); ++i) {
        const auto &ref = refs[i]; const auto at = 0x20 + extended + i * 0x38; const auto hash = Sha256(ref.bytes.data(), ref.bytes.size());
        std::copy(hash.begin(), hash.end(), result.begin() + at); std::copy_n(hash.begin(), 16, result.begin() + at + 32);
        if (ref.contentId) for (size_t j{}; j < 16; ++j) result[at + 32 + j] = static_cast<uint8_t>(std::stoul(ref.contentId->substr(j * 2, 2), nullptr, 16));
        Put(result, at + 48, ref.bytes.size(), 5); result[at + 54] = ref.type; result[at + 55] = ref.index;
    }
    const auto digest = Sha256(result.data(), result.size()); result.insert(result.end(), digest.begin(), digest.end()); return result;
}
class MutableSource final : public Reader {
  public:
    fixture::Bytes bytes; bool changed{}, fail{}; size_t readLimit = std::numeric_limits<size_t>::max(); mutable size_t reads{};
    explicit MutableSource(fixture::Bytes value) : bytes(std::move(value)) {}
    uint64_t Size() const override { return bytes.size(); }
    size_t Read(uint64_t offset, uint8_t *out, size_t count) const override {
        ++reads; if (fail) throw std::runtime_error("PRIVATE_INPUT_PATH PRIVATE_BYTES"); Range(offset, count, bytes.size());
        const auto actual = std::min(count, readLimit); std::copy_n(bytes.data() + offset, actual, out); return actual;
    }
    void CheckUnchanged() const override { if (changed) throw Error("input_changed", "synthetic source changed"); }
    std::string SnapshotToken() const override { return "same-synthetic-token-never-authorizes-bytes"; }
};
struct Package {
    Files files;
    std::vector<Reference> appRefs, patchRefs;
    Bytes appNpdm = Npdm(), appMain = Nso(), patchMain = Nso(7), appRomfs = Romfs({{"marker", Text("base")}}), patchRomfs = Romfs({{"marker", Text("patch")}});
    Bytes nacp = Nacp(); bool omitRtld{}, bktr{}, aocAsData{}, omitPublicData{};
    uint64_t patchParent = App, aocParent = App; bool standalone{}, withPatch{}, withAocs{true};
    size_t aocCount{2};
    uint64_t publicDataIdBias{};
    Files extraExeFs; bool multiplePrograms{};
    Files Build() {
        files.clear();
        auto program = [&](bool patch) {
            Files exe{{"main", patch ? patchMain : appMain}, {"main.npdm", appNpdm}}; if (!omitRtld) exe.push_back({"rtld", Nso(2)});
            exe.insert(exe.end(), extraExeFs.begin(), extraExeFs.end());
            return Nca(0, App, {{true, Pfs(exe)}, {false, patch ? patchRomfs : appRomfs, patch && bktr}});
        };
        auto addGroup = [&](uint8_t role, uint64_t id, uint64_t parent, std::vector<Reference> refs) {
            for (const auto &ref : refs) files.push_back({Hash(ref.bytes).substr(0, 32) + ".nca", ref.bytes});
            const auto meta = Nca(1, id, {{true, Pfs({{"content.cnmt", Cnmt(role, id, parent, refs)}})}});
            files.push_back({Hash(meta).substr(0, 32) + ".cnmt.nca", meta});
        };
        const auto control = Nca(2, App, {{false, Romfs({{"control.nacp", nacp}})}});
        if (!standalone) {
            appRefs = {{1, 0, program(false), {}}, {3, 0, control, {}}};
            if (multiplePrograms) appRefs.push_back({1, 1, Nca(0, App + 1, {{true, Pfs({{"main", Nso(3)}, {"rtld", Nso(4)}, {"main.npdm", Npdm(App + 1)}})}, {false, appRomfs}}), {}});
            addGroup(0x80, App, 0, appRefs);
        }
        if (withPatch || standalone) { patchRefs = {{1, 0, program(true), {}}, {3, 0, control, {}}}; addGroup(0x81, Patch, patchParent, patchRefs); }
        if (withAocs) for (size_t i{}; i < aocCount; ++i) {
            const auto id = Aoc1 + i;
            std::vector<Reference> refs;
            if (!omitPublicData) refs.push_back({2, 0, Nca(aocAsData ? 4 : 5, id + publicDataIdBias, {{false, Romfs({{"marker", Text("aoc" + std::to_string(i + 1))}})}}), {}});
            addGroup(0x82, id, aocParent, refs);
        }
        // Identical shared Control is one container member, not an ambiguous
        // duplicate content ID. Referring groups still each verify it.
        std::set<std::string> names; Files unique;
        for (auto &entry : files) if (names.insert(entry.first).second) unique.push_back(std::move(entry));
        files = unique; return files;
    }
};
inline Options OptionsForTest() { Options options; options.profile = ContentInfoProfile::Modern40; options.verifyHashes = true; return options; }
}
