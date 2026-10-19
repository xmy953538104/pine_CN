// SPDX-License-Identifier: MPL-2.0
package emu.skyline.loader;

import java.nio.file.Files;
import java.nio.file.Path;
import java.security.MessageDigest;
import java.util.HexFormat;

class SelectedContentBridge {
    static native String inspectObserved(int borrowedFd, String privatePath, String profile);
}

public final class SelectedScanObservedFixture {
    static native void prepare(String root);
    static native int open(String path);
    static native void close(int fd);
    static native boolean isOpen(int fd);
    static native void pending();
    private static int passed;
    private static Path root, evidence;
    private static void require(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }
    private static void scan(String name, String file, String keys, String profile, String... tokens) throws Exception {
        int fd = file == null ? -1 : open(root.resolve(file).toString());
        require(file == null || fd >= 0, "synthetic fixture open");
        String envelope;
        try {
            envelope = SelectedContentBridge.inspectObserved(fd, keys == null ? null : root.resolve(keys).toString(), profile);
            require(fd < 0 || isOpen(fd), "borrowed FD remains owned by Java caller");
        }
        finally { if (fd >= 0) close(fd); }
        require(envelope != null && envelope.startsWith("{\"selected_scan_schema\":1,"), "production envelope schema");
        // Deliberately persist only the safe report. Raw Catalog stays transient.
        int at = envelope.lastIndexOf(",\"scan_report\":");
        require(at > 0 && envelope.endsWith("}"), "production envelope report");
        String report = envelope.substring(at + ",\"scan_report\":".length(), envelope.length() - 1);
        require(report.length() <= 16384 && !report.contains("PRIVATE"), "report bounds and privacy");
        byte[] rights = new byte[16]; for (int i = 0; i < rights.length; ++i) rights[i] = (byte)(0x90 + i);
        require(!report.contains(HexFormat.of().formatHex(rights)) &&
                !report.contains(HexFormat.of().formatHex(MessageDigest.getInstance("SHA-256").digest(rights))), "Rights ID and its hash stay private");
        if (report.contains("\"status\":\"failed\""))
            require(envelope.startsWith("{\"selected_scan_schema\":1,\"status\":\"failed\",\"catalog\":null,"), "failed envelope has no raw Catalog");
        else
            require(envelope.startsWith("{\"selected_scan_schema\":1,\"status\":\"completed\",\"catalog\":{"), "completed envelope retains transient Catalog");
        for (String token : tokens) require(report.contains(token), "expected safe token " + token);
        Files.writeString(evidence.resolve(name + ".json"), report);
        ++passed; System.out.println("PASS host-JVM " + name);
    }
    public static void main(String[] args) throws Exception {
        System.load(args[0]); root = Path.of(args[1]); evidence = Path.of(args[2]);
        Files.createDirectories(evidence); prepare(root.toString());
        scan("valid-modern", "valid.nsp", "full", "modern-40", "\"status\":\"completed\"", "\"parsing\":\"parsed\"");
        scan("empty-modern", "empty.nsp", "full", "modern-40", "\"group_count\":0");
        scan("aoc-only", "aoc.nsp", "full", "modern-40", "\"role\":\"aoc\"");
        scan("patch-only", "patch.nsp", "full", "modern-40", "\"role\":\"patch\"");
        scan("missing-meta-area-key", "valid.nsp", "header", "modern-40", "missing_key_area", "\"all_metadata_enumerated\":false");
        scan("missing-header-key", "valid.nsp", "none", "modern-40", "missing_header_key");
        scan("bad-cnmt", "bad-cnmt.nsp", "full", "modern-40", "\"parsing\":\"invalid\"");
        scan("digest-observation", "digest.nsp", "full", "modern-40", "cnmt_digest_mismatch", "\"digest_observation\":\"mismatch\"");
        scan("empty-legacy", "empty.nsp", "full", "legacy-48", "\"status\":\"completed\"");
        scan("bad-profile", null, "full", "PRIVATE_BAD_PROFILE", "\"failed_stage\":\"profile_validation\"", "selected_profile_required");
        scan("null-private-path", null, null, "modern-40", "\"failed_stage\":\"keystore_load\"", "selected_private_path_required");
        scan("invalid-fd", null, "full", "modern-40", "\"failed_stage\":\"fd_duplicate\"", "input_fd_invalid");
        scan("bad-pfs", "bad-pfs.nsp", "full", "modern-40", "\"failed_stage\":\"catalog_scan\"", "unsupported_pfs_format");
        scan("entry-budget", "entry-budget.nsp", "full", "modern-40", "\"failed_stage\":\"catalog_scan\"", "\"error_code\":\"scan_budget\"");
        scan("unknown-native-error", null, "bad", "modern-40", "\"failed_stage\":\"keystore_load\"", "other_native_issue");
        scan("title-map-matched", "title-rights.nsp", "title-full", "modern-40", "\"loaded_unique_title_key_count\":1", "\"recognized_prod_slot_count\":3", "\"title_key_lookup\":\"matched\"", "\"parsing\":\"parsed\"");
        scan("title-map-present-not-matched", "title-rights.nsp", "title-unmatched", "modern-40", "\"loaded_unique_title_key_count\":1", "\"title_key_lookup\":\"not_matched\"", "missing_title_key");
        scan("title-map-match-not-decryption", "title-rights.nsp", "title-no-kek", "modern-40", "\"title_key_lookup\":\"matched\"", "missing_title_kek");
        scan("failed-title-prefix", null, "partial-title", "modern-40", "\"loaded_unique_title_key_count\":1", "\"recognized_prod_slot_count\":0", "\"counts_partial\":true", "\"failed_stage\":\"keystore_load\"");
        scan("failed-prod-prefix", null, "partial-prod", "modern-40", "\"loaded_unique_title_key_count\":1", "\"recognized_prod_slot_count\":1", "\"counts_partial\":true");
        scan("public-cids-version-sizes", "public-content.nsp", "full", "modern-40", "\"version\":4294967295", "\"content_id_source\":\"entry_name_hint\"", "\"full_content_hash\":\"not_performed\"");
        try { pending(); throw new AssertionError("pending exception cleared"); }
        catch (IllegalStateException expected) { require(expected.getMessage().equals("synthetic_pending_exception"), "pending exception preserved"); }
        ++passed; System.out.println("PASS host-JVM pending VM exception preserved");
        System.out.println("PASS " + passed + " actual host-JVM observed assertions; Android/device/guest not run");
    }
}
