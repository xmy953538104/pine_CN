// SPDX-License-Identifier: MPL-2.0
// Only generated, content-free fixtures. The controlled cipher is NOT AES.
#include <catalog/native_adapter.h>
#include <crypto/aes_cipher.h>
#include <vfs/backing.h>
#include <vfs/nca.h>
#include <vfs/os_backing.h>
#include <vfs/region_backing.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <filesystem>
#include <fstream>
#ifdef PINE_CATALOG_TEST_REAL_AES
#include <openssl/evp.h>
#endif

using namespace skyline;
using namespace skyline::catalog;
namespace {
    void Check(bool result, const char *detail) { if (!result) throw std::runtime_error(detail); }
    std::filesystem::path evidenceDirectory;
    void Evidence(const char *name, const Catalog &catalog) {
        if (evidenceDirectory.empty()) return;
        std::ofstream output(evidenceDirectory / name);
        output << ToJson(catalog); output.close();
        Check(static_cast<bool>(output), "native JSON evidence write failed");
    }
    class MemoryBacking final : public vfs::Backing {
      public:
        std::vector<uint8_t> bytes;
        size_t readLimit = std::numeric_limits<size_t>::max();
        bool fail{}, oversizedCount{};
        size_t reads{};
        std::vector<std::pair<size_t, size_t>> readRanges;
        bool HasReliableReadByteCount() const noexcept override { return true; }
        explicit MemoryBacking(std::vector<uint8_t> pBytes) : Backing({true, false, false}, pBytes.size()), bytes(std::move(pBytes)) {}
      protected:
        size_t ReadImpl(span<u8> output, size_t offset) override {
            ++reads;
            readRanges.emplace_back(offset, output.size());
            if (fail) throw std::runtime_error("PRIVATE_PATH PRIVATE_KEY PRIVATE_BYTES");
            if (oversizedCount) return output.size() + 1;
            const auto count = std::min({output.size(), readLimit, offset <= bytes.size() ? bytes.size() - offset : size_t{0}});
            std::copy_n(bytes.data() + std::min(offset, bytes.size()), count, output.data());
            return count;
        }
    };
    void Put(std::vector<uint8_t> &bytes, size_t at, uint64_t value, size_t width) {
        for (size_t i{}; i < width; ++i, value >>= 8) bytes.at(at + i) = static_cast<uint8_t>(value);
    }
    std::vector<uint8_t> Pfs(const std::vector<std::pair<std::string, std::vector<uint8_t>>> &files) {
        size_t stringSize{}, payloadSize{};
        for (const auto &[name, data] : files) { stringSize += name.size() + 1; payloadSize += data.size(); }
        const auto stringAt = 16 + files.size() * 24, dataAt = stringAt + stringSize;
        std::vector<uint8_t> result(dataAt + payloadSize);
        std::memcpy(result.data(), "PFS0", 4); Put(result, 4, files.size(), 4); Put(result, 8, stringSize, 4);
        size_t nameOffset{}, fileOffset{};
        for (size_t i{}; i < files.size(); ++i) {
            const auto &[name, data] = files[i]; const auto at = 16 + i * 24;
            Put(result, at, fileOffset, 8); Put(result, at + 8, data.size(), 8); Put(result, at + 16, nameOffset, 4);
            std::copy(name.begin(), name.end(), result.begin() + stringAt + nameOffset);
            std::copy(data.begin(), data.end(), result.begin() + dataAt + fileOffset);
            nameOffset += name.size() + 1; fileOffset += data.size();
        }
        return result;
    }
    std::vector<uint8_t> Cnmt(uint64_t title = 0x0100000000001000) {
        std::vector<uint8_t> result(0x50);
        Put(result, 0, title, 8); Put(result, 8, 1, 4); result[12] = 0x80; Put(result, 14, 0x10, 2);
        Put(result, 0x20, title + 0x800, 8);
        const auto digest = Sha256(result.data(), result.size() - 32);
        std::copy(digest.begin(), digest.end(), result.end() - 32);
        return result;
    }
#ifdef PINE_CATALOG_TEST_REAL_AES
    crypto::KeyStore::Key256 HeaderKey() {
        crypto::KeyStore::Key256 result{};
        for (size_t i{}; i < result.size(); ++i) result[i] = static_cast<uint8_t>(i + 1);
        return result;
    }
    crypto::KeyStore::Key128 AreaKey() {
        crypto::KeyStore::Key128 result{};
        for (size_t i{}; i < result.size(); ++i) result[i] = static_cast<uint8_t>(0x20 + i);
        return result;
    }
    crypto::KeyStore::Key128 ContentKey() {
        crypto::KeyStore::Key128 result{};
        for (size_t i{}; i < result.size(); ++i) result[i] = static_cast<uint8_t>(0x40 + i);
        return result;
    }
    std::vector<uint8_t> Encrypt(const EVP_CIPHER *cipher, const uint8_t *key, const uint8_t *iv,
                                 const uint8_t *bytes, size_t count) {
        using Context = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;
        Context context(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
        Check(static_cast<bool>(context), "OpenSSL fixture context allocation failed");
        Check(EVP_EncryptInit_ex(context.get(), cipher, nullptr, key, iv) == 1, "OpenSSL fixture encrypt init failed");
        Check(EVP_CIPHER_CTX_set_padding(context.get(), 0) == 1, "OpenSSL fixture no-padding failed");
        std::vector<uint8_t> result(count + 16); int amount{}, tail{};
        Check(EVP_EncryptUpdate(context.get(), result.data(), &amount, bytes, static_cast<int>(count)) == 1, "OpenSSL fixture encryption failed");
        Check(EVP_EncryptFinal_ex(context.get(), result.data() + amount, &tail) == 1, "OpenSSL fixture encrypt final failed");
        result.resize(static_cast<size_t>(amount + tail)); Check(result.size() == count, "OpenSSL fixture cipher changed extent");
        return result;
    }
    void EncryptXtsSector(std::vector<uint8_t> &bytes, size_t at, uint64_t sector) {
        std::array<uint8_t, 16> tweak{};
        for (size_t i{}; i < 8; ++i, sector >>= 8) tweak[15 - i] = static_cast<uint8_t>(sector);
        const auto key = HeaderKey();
        const auto encoded = Encrypt(EVP_aes_128_xts(), key.data(), tweak.data(), bytes.data() + at, 0x200);
        std::copy(encoded.begin(), encoded.end(), bytes.begin() + at);
    }
#endif
    struct Fixture {
        std::vector<uint8_t> header = std::vector<uint8_t>(0x400);
        std::vector<uint8_t> fs = std::vector<uint8_t>(0x200);
        std::vector<uint8_t> pfs;
        explicit Fixture(std::vector<uint8_t> pPfs = Pfs({{"a.cnmt", Cnmt()}})) : pfs(std::move(pPfs)) {
            std::memcpy(header.data() + 0x200, "NCA3", 4); header[0x205] = 1;
            Put(header, 0x208, 0x1800, 8); Put(header, 0x210, 0x0100000000001000, 8);
            Put(header, 0x240, 6, 4); Put(header, 0x244, 12, 4);
            Put(fs, 0, 2, 2); fs[2] = 1; fs[3] = 2; fs[4] = 3;
            Put(fs, 0x28, 0x200, 4); Put(fs, 0x2c, 2, 4); Put(fs, 0x38, 32, 8);
            Put(fs, 0x40, 0x200, 8); Put(fs, 0x48, pfs.size(), 8);
        }
        std::vector<uint8_t> Encode(bool plaintext = false) {
#ifdef PINE_CATALOG_TEST_REAL_AES
            // Public, generated fixture keys only. Encryption uses OpenSSL;
            // decryption uses Pine's production wrapper and pinned mbedTLS.
            const auto area = AreaKey(), content = ContentKey();
            const auto encryptedArea = Encrypt(EVP_aes_128_ecb(), area.data(), nullptr, content.data(), content.size());
            std::copy(encryptedArea.begin(), encryptedArea.end(), header.begin() + 0x320);
#endif
            const auto pfsHash = Sha256(pfs.data(), pfs.size());
            const auto masterHash = Sha256(pfsHash.data(), pfsHash.size());
            std::copy(masterHash.begin(), masterHash.end(), fs.begin() + 8);
            auto hash = Sha256(fs.data(), fs.size()); std::copy(hash.begin(), hash.end(), header.begin() + 0x280);
            std::vector<uint8_t> result(0x1800);
            std::copy(header.begin(), header.end(), result.begin());
            std::copy(fs.begin(), fs.end(), result.begin() + 0x400);
            std::copy(pfsHash.begin(), pfsHash.end(), result.begin() + 0xc00);
            Check(pfs.size() <= result.size() - 0xe00, "fixture PFS too large");
            std::copy(pfs.begin(), pfs.end(), result.begin() + 0xe00);
            if (!plaintext) {
#ifdef PINE_CATALOG_TEST_REAL_AES
                EncryptXtsSector(result, 0, 0); EncryptXtsSector(result, 0x200, 1); EncryptXtsSector(result, 0x400, 2);
                std::array<uint8_t, 16> iv{}; iv[15] = 0xc0; // absolute section offset 0xc00 >> 4
                const auto encoded = Encrypt(EVP_aes_128_ctr(), content.data(), iv.data(), result.data() + 0xc00, 0xc00);
                std::copy(encoded.begin(), encoded.end(), result.begin() + 0xc00);
#else
                for (size_t i{}; i < 0x400; ++i) result[i] ^= 0xa5;
                for (size_t i{0x400}; i < 0x600; ++i) result[i] ^= 0xa5;
#endif
            }
            return result;
        }
    };
    std::shared_ptr<crypto::KeyStore> Keys() {
        auto result = std::make_shared<crypto::KeyStore>();
#ifdef PINE_CATALOG_TEST_REAL_AES
        result->headerKey = HeaderKey(); result->areaKeyApplication[0] = AreaKey();
#else
        result->headerKey = crypto::KeyStore::Key256{}; result->areaKeyApplication[0] = crypto::KeyStore::Key128{};
#endif
        return result;
    }
    PfsEntry Entry(std::shared_ptr<MemoryBacking> backing) {
        return {"11111111111111111111111111111111.cnmt.nca", 17, backing->size, MakeBackingSource(std::move(backing))};
    }
    bool IssueIs(const NcaDescriptor &result, const std::string &code) {
        return std::any_of(result.issues.begin(), result.issues.end(), [&](const auto &issue) { return issue.code == code; });
    }
    NcaDescriptor Inspect(Fixture fixture, std::shared_ptr<crypto::KeyStore> keys = Keys()) {
        auto memory = std::make_shared<MemoryBacking>(fixture.Encode());
        return NativeNcaInspector(std::move(keys)).Inspect(Entry(memory), {});
    }
    void Reject(Fixture fixture, const char *code) {
        const auto result = Inspect(std::move(fixture));
        Check(result.support != "metadata_readable", "invalid fixture became readable");
        Check(IssueIs(result, code), code);
        for (const auto &section : result.sections) Check(!section.pfs, "rejected NCA exposed a PFS");
    }
    void ReaderTests() {
        class Unreliable final : public vfs::Backing {
            size_t ReadImpl(span<u8>, size_t) override { return size; }
          public:
            Unreliable() : Backing({true, false, false}, 8) {}
        };
        try { MakeBackingSource(std::make_shared<Unreliable>()); Check(false, "unknown backing count accepted"); }
        catch (const Error &e) { Check(e.code == "native_unreliable_backing", "wrong unreliable-count error"); }
        auto memory = std::make_shared<MemoryBacking>(std::vector<uint8_t>(32, 9));
        const auto source = MakeBackingSource(memory); uint8_t out[8]{};
        memory->readLimit = 3;
        Check(source->Read(0, out, 8) == 3, "adapter returned backing total size instead of actual count");
        Check(memory->Read(span<u8>(out, 8)) == 32, "fixture no longer demonstrates legacy total-size return");
        try { source->Exact(0, out, 8); Check(false, "short read accepted"); } catch (const Error &e) { Check(e.code == "short_read", "wrong short-read result"); }
        memory->readLimit = 32; memory->oversizedCount = true;
        try { source->Read(0, out, 8); Check(false, "impossible count accepted"); } catch (const Error &e) { Check(e.code == "native_invalid_read_count", "wrong invalid-count error"); }
        memory->oversizedCount = false;
        try { source->Read(31, out, 8); Check(false, "range escape accepted"); } catch (const Error &e) { Check(e.code == "out_of_range", "wrong range error"); }
        memory->size = 31;
        try { source->Read(0, out, 8); Check(false, "size mutation accepted"); } catch (const Error &e) { Check(e.code == "input_changed", "wrong change error"); }
    }
    void NativeTests() {
        Fixture normal;
        auto raw = std::make_shared<MemoryBacking>(normal.Encode());
        const auto entry = Entry(raw);
        const auto result = NativeNcaInspector(Keys()).Inspect(entry, {});
        Check(result.support == "metadata_readable" && result.readability == "decrypted_header", "native bridge did not inspect controlled encrypted header");
        Check(result.contentType == 1 && result.programId == 0x0100000000001000, "native header identity lost");
        Check(result.signature == "not_verified" && result.actualSha256.empty(), "native bridge claimed crypto verification");
        Check(result.source == entry.source, "raw ciphertext source replaced");
        Check(result.sections.size() == 1 && result.sections[0].pfs.has_value(), "decrypted PFS absent");
        const auto pfs = *result.sections[0].pfs;
        Check(pfs->Size() == normal.pfs.size(), "legacy oversized region leaked into PFS source");
        Check(pfs->Bytes(0, normal.pfs.size()) == normal.pfs, "decrypted bytes did not reach strict parser");
        Check(std::memcmp(raw->bytes.data() + 0x200, "NCA3", 4) != 0, "ciphertext fixture accidentally kept a plaintext NCA header");
#ifdef PINE_CATALOG_TEST_REAL_AES
        Check(std::memcmp(raw->bytes.data() + 0xe00, normal.pfs.data(), 16) != 0, "real-AES fixture PFS was not encrypted");
#endif
        Check(ReadPfs(pfs, {}).size() == 1, "strict PFS not readable");
        Check(Sha256(*result.source) != Sha256(*pfs), "whole-NCA hash source replaced by PFS plaintext");
#ifndef PINE_CATALOG_TEST_REAL_AES
        Check(crypto::testXtsCalls >= 2 && crypto::testCtrCalls > 0, "actual NCA/CTR backend flow was not called");
#endif

        Reject(Fixture(Pfs({{"same.cnmt", Cnmt()}, {"same.cnmt", Cnmt()}})), "duplicate_pfs_name");
        Fixture duplicate(Pfs({{"same.cnmt", Cnmt()}, {"same.cnmt", Cnmt()}}));
        auto duplicateBacking = std::make_shared<MemoryBacking>(duplicate.Encode());
        const auto duplicateResult = NativeNcaInspector(Keys()).Inspect(Entry(duplicateBacking), {});
        Check(IssueIs(duplicateResult, "duplicate_pfs_name"), "duplicate was lost in legacy directory map");
        Check(std::count(duplicateBacking->readRanges.begin(), duplicateBacking->readRanges.end(), std::pair<size_t, size_t>{0xe00, 16}) == 1,
              "legacy PFS parser ran before strict duplicate rejection");
        Fixture tooShort; Put(tooShort.fs, 0x48, tooShort.pfs.size() - 1, 8); Reject(tooShort, "out_of_range");
        Fixture escaped; Put(escaped.fs, 0x48, 0xb00, 8); Reject(escaped, "out_of_range");
        Fixture table; Put(table.pfs, 4, 1000000, 4); Reject(table, "scan_budget");
        Fixture strings; Put(strings.pfs, 8, 0xffffffff, 4); Reject(strings, "scan_budget");
        Fixture slot; Put(slot.header, 0x240, 0, 4); Put(slot.header, 0x244, 0, 4); Put(slot.header, 0x250, 6, 4); Put(slot.header, 0x254, 12, 4);
        Reject(slot, "native_noncontiguous_sections_unsupported");
        Fixture generation; generation.header[0x206] = 21; Reject(generation, "native_key_generation_unsupported");
        Fixture xts; xts.fs[4] = 2; Reject(xts, "native_section_encryption_unsupported");
        Fixture bktr; bktr.fs[4] = 4; Reject(bktr, "bktr_requires_base");
        Fixture sparse; sparse.fs[0x148] = 1; Reject(sparse, "sparse_unsupported");
        Fixture compressed; compressed.fs[0x178] = 1; Reject(compressed, "compression_unsupported");
        Fixture wrongType; wrongType.header[0x205] = 6; Reject(wrongType, "native_content_type_unsupported");
        Fixture layers; Put(layers.fs, 0x2c, 5, 4); Reject(layers, "nca_hash_layers_unsupported");
        Fixture unaligned; Put(unaligned.fs, 0x40, 0x201, 8); Reject(unaligned, "native_ctr_alignment_unsupported");
        Fixture badHash; auto changed = badHash.Encode(); changed[0x400 + 0x28] ^= 1;
        auto badHashBacking = std::make_shared<MemoryBacking>(changed);
        Check(IssueIs(NativeNcaInspector(Keys()).Inspect(Entry(badHashBacking), {}), "nca_fs_header_hash_mismatch"), "decrypted FS header hash mismatch accepted");

#ifndef PINE_CATALOG_TEST_REAL_AES
        const auto calls = crypto::testXtsCalls;
#endif
        auto plaintext = std::make_shared<MemoryBacking>(normal.Encode(true));
        Check(IssueIs(NativeNcaInspector(Keys()).Inspect(Entry(plaintext), {}), "native_plaintext_unsupported"), "plaintext entered legacy native loop");
#ifndef PINE_CATALOG_TEST_REAL_AES
        Check(crypto::testXtsCalls == calls, "plaintext unexpectedly decrypted");
#endif
        auto legacyBacking = std::make_shared<MemoryBacking>(normal.Encode());
        const vfs::NCA legacy(legacyBacking, Keys());
        Check(legacy.cnmt && legacy.cnmt->FileExists("a.cnmt"), "default-null NCA inspection policy altered legacy content access");
        Check(std::count(legacyBacking->readRanges.begin(), legacyBacking->readRanges.end(), std::pair<size_t, size_t>{0xe00, 16}) == 1,
              "default-null policy installed catalog PFS preflight");

        auto noHeader = Keys(); noHeader->headerKey.reset();
        Check(IssueIs(Inspect(normal, noHeader), "missing_header_key"), "missing header key lost");
        auto noArea = Keys(); noArea->areaKeyApplication[0].reset();
        Check(IssueIs(Inspect(normal, noArea), "missing_key_area"), "missing key area lost");
        Fixture rights; rights.header[0x230] = 1;
        Check(IssueIs(Inspect(rights), "missing_title_key"), "missing title key lost");
        auto noKek = Keys(); noKek->titleKey = crypto::KeyStore::Key128{};
        Check(IssueIs(Inspect(rights, noKek), "missing_title_kek"), "missing title KEK lost");
        Check(IssueIs(Inspect(normal, {}), "missing_key_store"), "missing caller key store lost");

        raw->readLimit = 0x3ff;
        Check(IssueIs(NativeNcaInspector(Keys()).Inspect(entry, {}), "short_read"), "native short header read accepted");
        raw->readLimit = std::numeric_limits<size_t>::max(); raw->fail = true;
        const auto failed = NativeNcaInspector(Keys()).Inspect(entry, {});
        Check(IssueIs(failed, "native_nca_backend_failed"), "native backend exception not retained");
        for (const auto &issue : failed.issues) Check(issue.detail.find("PRIVATE_") == std::string::npos, "private exception text leaked");
    }
    void CatalogTests() {
        Fixture first;
        Fixture second(Pfs({{"other.cnmt", Cnmt(0x0100000000002000)}}));
        auto containerBytes = Pfs({{"11111111111111111111111111111111.cnmt.nca", first.Encode()},
                                   {"22222222222222222222222222222222.cnmt.nca", second.Encode()}});
        auto container = std::make_shared<MemoryBacking>(containerBytes);
        const auto catalog = ScanNative(container, Keys());
        Evidence("native-complete.json", catalog);
        Check(catalog.inspectorName == "pine_existing_nca", "native catalog mislabeled as plaintext");
        Check(catalog.ncas.size() == 2 && catalog.groups.size() == 2, "native factory failed multiple Meta enumeration");
        for (const auto &group : catalog.groups) Check(group.parsing == "parsed" && group.digest == "verified", "native CNMT did not reach core");
        const auto missingKeys = ScanNative(container, std::make_shared<crypto::KeyStore>());
        Evidence("native-missing-keys.json", missingKeys);
        Check(missingKeys.ncas.size() == 2 && missingKeys.groups.empty(), "missing keys silently dropped individual NCAs");
        Check(!missingKeys.AllMetadataEnumerated(), "missing keys claimed complete metadata enumeration");
        for (const auto &nca : missingKeys.ncas) Check(IssueIs(nca, "missing_header_key"), "per-NCA missing key error absent");
        auto duplicate = Fixture(Pfs({{"duplicate.cnmt", Cnmt()}, {"duplicate.cnmt", Cnmt()}}));
        auto mixed = std::make_shared<MemoryBacking>(Pfs({{"good.nca", first.Encode()}, {"bad.nca", duplicate.Encode()}}));
        const auto partial = ScanNative(mixed, Keys());
        Evidence("native-partial.json", partial);
        Check(partial.ncas.size() == 2 && partial.groups.size() == 1, "bad NCA suppressed valid Meta");
        Check(!partial.AllMetadataEnumerated(), "unreadable Meta claimed complete metadata enumeration");
        Options limited; limited.limits.maxMetadataBytes = 2 * 0x1004 + first.pfs.size() * 3;
        const auto members = ReadPfs(MakeBackingSource(container), {});
        const auto secondPfs = members[1].offset + 0xe00;
        container->readRanges.clear();
        const auto budgeted = ScanNative(container, Keys(), limited);
        Evidence("native-pfs-budget.json", budgeted);
        Check(budgeted.ncas.size() == 2 && budgeted.groups.size() == 1, "native scan budget was reset per NCA");
        Check(IssueIs(budgeted.ncas[1], "scan_budget"), "second NCA was not rejected before its decrypted PFS read");
        Check(!budgeted.complete, "native PFS budget exhaustion claimed a complete bounded scan");
        Check(!budgeted.AllMetadataEnumerated(), "native PFS budget exhaustion claimed complete metadata enumeration");
        Check(ToJson(budgeted).find("\"bounded_scan_complete\":false") != std::string::npos,
              "native PFS budget exhaustion serialized bounded_scan_complete=true");
        Check(std::none_of(container->readRanges.begin(), container->readRanges.end(), [&](const auto &read) {
            return read.first < secondPfs + second.pfs.size() && read.first + read.second > secondPfs;
        }), "over-budget second PFS was read before rejection");

        // The core's nominal 0xc00-byte header allowance admits this member,
        // while the native prefix/header/FS reservation needs 0x1004 bytes.
        // This returned-descriptor failure must also propagate incompleteness.
        auto headerContainer = std::make_shared<MemoryBacking>(Pfs({{"header-budget.nca", first.Encode()}}));
        const auto headerMembers = ReadPfs(MakeBackingSource(headerContainer), {});
        const auto firstPfs = headerMembers[0].offset + 0xe00;
        Options headerLimited; headerLimited.limits.maxMetadataBytes = 0xc00;
        headerContainer->readRanges.clear();
        const auto headerBudgeted = ScanNative(headerContainer, Keys(), headerLimited);
        Evidence("native-header-budget.json", headerBudgeted);
        Check(headerBudgeted.ncas.size() == 1 && headerBudgeted.groups.empty(), "native header reservation failure lost its member");
        Check(IssueIs(headerBudgeted.ncas[0], "scan_budget"), "native header reservation failure did not retain scan_budget");
        Check(headerBudgeted.ncas[0].readability == "unreadable", "core skipped the header before the native reservation was exercised");
        Check(!headerBudgeted.complete && !headerBudgeted.AllMetadataEnumerated(), "native header budget exhaustion claimed a complete scan");
        Check(ToJson(headerBudgeted).find("\"bounded_scan_complete\":false") != std::string::npos,
              "native header budget exhaustion serialized bounded_scan_complete=true");
        Check(std::none_of(headerContainer->readRanges.begin(), headerContainer->readRanges.end(), [&](const auto &read) {
            return read.first < firstPfs + first.pfs.size() && read.first + read.second > firstPfs;
        }), "native header reservation failure read decrypted PFS bytes");
        Check(catalog.containerSnapshot.empty(), "unbound Backing fabricated an opened-file identity");
    }
    void OsTests() {
        char path[] = "/tmp/pine-catalog-native-XXXXXX";
        const int fd = mkstemp(path); Check(fd >= 0, "mkstemp failed"); unlink(path);
        const std::vector<uint8_t> bytes(64, 7);
        Check(write(fd, bytes.data(), bytes.size()) == static_cast<ssize_t>(bytes.size()), "temporary synthetic file write failed");
        auto backing = std::make_shared<vfs::OsBacking>(fd, true);
        const auto reader = MakeBackingSource(backing); uint8_t output[64]{};
        Check(ftruncate(fd, 3) == 0, "temporary file truncate failed");
        Check(reader->Read(0, output, 64) == 3, "real OsBacking short count lost");
        try { reader->Exact(0, output, 64); Check(false, "real OsBacking truncation accepted"); } catch (const Error &e) { Check(e.code == "short_read", "wrong real truncation error"); }
    }
    void SnapshotTests() {
        char path[] = "/tmp/pine-catalog-bound-source-XXXXXX";
        const int fd = mkstemp(path); Check(fd >= 0, "snapshot fixture creation failed");
        Fixture fixture;
        const auto bytes = Pfs({{"bound.nca", fixture.Encode()}});
        Check(write(fd, bytes.data(), bytes.size()) == static_cast<ssize_t>(bytes.size()), "snapshot fixture write failed");
        close(fd);
        const auto source = OpenFile(path);
        const auto catalog = ScanNative(source, Keys());
        Check(!catalog.containerSnapshot.empty() && catalog.containerSnapshot == source->SnapshotToken(), "same-source opened identity did not reach native catalog");
        Check(catalog.source == source && catalog.groups.size() == 1, "bound source bytes were replaced by a different backing");
        unlink(path);
        try { catalog.CheckUnchanged(); Check(false, "removed bound source name was accepted"); }
        catch (const Error &error) { Check(error.code == "input_changed", "wrong bound-source replacement error"); }
    }
    void GoldenTests() {
        std::ifstream input("tests/catalog/fixtures/patch-sentinel.cnmt", std::ios::binary);
        Check(static_cast<bool>(input), "independent CNMT golden is missing");
        std::vector<uint8_t> cnmt((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        const auto goldenHash = Sha256(cnmt.data(), cnmt.size());
        Check(Hex(goldenHash.data(), goldenHash.size()) == "53adc31ca6d927c2d01cf32e5ec3381f79c3447c455936de36868c226467fbc4",
              "independent CNMT golden changed");
        Fixture fixture(Pfs({{"patch-sentinel.cnmt", cnmt}}));
        Put(fixture.header, 0x210, 0x0100000012345800, 8);
        const auto encoded = fixture.Encode();
        auto container = std::make_shared<MemoryBacking>(Pfs({{"golden.cnmt.nca", encoded}}));
        Options options; options.profile = ContentInfoProfile::Modern40; options.verifyHashes = true;
        const auto catalog = ScanNative(container, Keys(), options);
        Evidence("native-independent-golden.json", catalog);
        Check(catalog.groups.size() == 1 && catalog.ncas.size() == 1, "native independent golden group missing");
        const auto &group = catalog.groups[0];
        Check(group.parsing == "parsed" && group.type == 0x81 && group.titleId == 0x0100000012345800 && group.applicationId == 0x0100000012345000,
              "native independent golden Patch identity changed");
        Check(group.contents.size() == 1 && group.contents[0].contentId == "a0a1a2a3a4a5a6a7a8a9aaabacadaeaf" &&
              group.contents[0].size == 0x12345678 && group.contents[0].idOffset == 7, "native independent content table changed");
        Check(group.metaReferences.size() == 1 && group.metaReferences[0].titleId == 0x8877665544332211 &&
              group.metaReferences[0].version == 0xa1b2c3d4 && group.metaReferences[0].type == 0x82 && group.metaReferences[0].attributes == 4,
              "native independent ContentMetaInfo table changed");
        const auto rawHash = Sha256(encoded.data(), encoded.size());
        Check(catalog.ncas[0].actualSha256 == Hex(rawHash.data(), rawHash.size()) && catalog.hashBytes == encoded.size(),
              "native whole-NCA hash was not calculated from raw ciphertext bytes");
        Check(catalog.ncas[0].signature == "not_verified", "synthetic AES became signature authentication");
    }
    void NonzeroHeaderGoldenTests() {
        std::ifstream input("tests/catalog/fixtures/header-fields-sentinel.cnmt", std::ios::binary);
        Check(static_cast<bool>(input), "independent nonzero-header CNMT golden is missing");
        std::vector<uint8_t> cnmt((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        const auto goldenHash = Sha256(cnmt.data(), cnmt.size());
        Check(Hex(goldenHash.data(), goldenHash.size()) == "3852a0f68ef82426946fed951dfc2a05c24de87a369eddfe01950371ea1e5391",
              "independent nonzero-header CNMT golden changed");
        Fixture fixture(Pfs({{"header-fields-sentinel.cnmt", cnmt}}));
        Put(fixture.header, 0x210, 0x0100000012345800, 8);
        auto container = std::make_shared<MemoryBacking>(Pfs({{"header-fields.cnmt.nca", fixture.Encode()}}));
        Options options; options.profile = ContentInfoProfile::Modern40; options.verifyHashes = true;
        const auto catalog = ScanNative(container, Keys(), options);
        Evidence("native-nonzero-header.json", catalog);
        Check(catalog.ncas.size() == 1 && catalog.groups.size() == 1, "nonzero packaged header hid the native CNMT group");
        const auto &group = catalog.groups[0];
        Check(group.parsing == "parsed" && group.digest == "verified", "valid nonzero packaged header was rejected");
        Check(group.rawHeader == "0058341200000001443322118100180001000100040501010102030400000000" &&
              group.platform == 0 && group.metaAttributes == 4 && group.storageId == 5 && group.installType == 1 &&
              group.committedRaw == 1 && group.requiredDownloadSystemVersion == 0x04030201,
              "native packaged header fields were lost or treated as padding");
        Check(group.contents.size() == 1 && group.contents[0].contentId == "a0a1a2a3a4a5a6a7a8a9aaabacadaeaf" &&
              group.metaReferences.size() == 1 && group.metaReferences[0].titleId == 0x8877665544332211 &&
              group.contentTableOffset == 0x38 && group.metaTableOffset == 0x70,
              "nonzero packaged header hid or shifted a native content table");
        Check(group.contentSupport == "unsupported" && group.runnable == "unsupported",
              "non-full install metadata became a complete runtime Program");
    }
}
int main(int argc, char **argv) {
    try {
        if (argc == 2) { evidenceDirectory = argv[1]; std::filesystem::create_directories(evidenceDirectory); }
        ReaderTests(); NativeTests(); CatalogTests(); OsTests(); SnapshotTests(); GoldenTests(); NonzeroHeaderGoldenTests();
#ifdef PINE_CATALOG_TEST_REAL_AES
        std::cout << "PASS: native bridge with production AES/NCA/PFS/CTR and pinned mbedTLS; independent OpenSSL synthetic XTS/ECB/CTR fixtures; no ROM/APK validation\n";
#else
        std::cout << "PASS: native bridge with real Backing/NCA/PFS/CTR control flow; controlled non-crypto cipher only; no AES/ROM/APK validation\n";
#endif
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n'; return 1;
    }
}
