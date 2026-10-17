#!/usr/bin/env python3
"""Symbol association gates with controlled ELF observations, not executable APK fixtures."""
import importlib.util
import json
import pathlib
import tempfile
import unittest
import zipfile

SCRIPT = pathlib.Path(__file__).resolve().parents[1] / "archive-native-symbols.py"
SPEC = importlib.util.spec_from_file_location("native_symbols", SCRIPT)
symbols = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(symbols)

class NativeSymbols(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.temp.name)
        self.apk = self.root / "fixture.apk"
        self.search = self.root / "obj"
        self.search.mkdir()
        self.output = self.root / "symbols"
        self.commit = "c" * 40
        self.write_apk(b"matching:stripped")
        (self.search / "libskyline.so").write_bytes(b"matching:debug_info")
    def tearDown(self):
        self.temp.cleanup()
    def write_apk(self, data):
        with zipfile.ZipFile(self.apk, "w") as archive:
            archive.writestr(symbols.LIBRARY, data)
    @staticmethod
    def inspector(path, tool):
        data = path.read_bytes()
        return ("a" if data.startswith(b"matching:") else "b") * 40, b"debug_info" in data
    def run_archive(self):
        return symbols.archive_symbols(self.apk, self.commit, [self.search], self.output, pathlib.Path("unused"), self.inspector)
    def test_exact_build_id_and_full_symbol_bytes_are_associated_with_apk_and_source(self):
        manifest = self.run_archive()
        self.assertEqual(manifest["elf_build_id"], "a" * 40)
        self.assertEqual(manifest["source_commit"], self.commit)
        self.assertEqual(manifest["unstripped_elf_sha256"], symbols.digest(self.output / "arm64-v8a/libskyline.so"))
        self.assertEqual(set(path.name for path in self.output.iterdir()), {"native-symbols.json", "arm64-v8a"})
        self.assertFalse((self.output / "packaged-library.tmp").exists())
    def test_mismatched_old_build_id_is_rejected(self):
        (self.search / "libskyline.so").write_bytes(b"old:debug_info")
        with self.assertRaisesRegex(ValueError, "found 0"):
            self.run_archive()
    def test_stripped_candidate_is_not_a_symbol_archive(self):
        (self.search / "libskyline.so").write_bytes(b"matching:stripped")
        with self.assertRaisesRegex(ValueError, "found 0"):
            self.run_archive()
    def test_different_binaries_with_same_claimed_id_are_ambiguous(self):
        other = self.search / "other"
        other.mkdir()
        (other / "libskyline.so").write_bytes(b"matching:debug_info altered")
        with self.assertRaisesRegex(ValueError, "found 2"):
            self.run_archive()
    def test_identical_copies_are_accepted_but_outside_symlink_is_ignored(self):
        other = self.search / "other"
        other.mkdir()
        (other / "libskyline.so").write_bytes((self.search / "libskyline.so").read_bytes())
        escaped = self.search / "outside"
        escaped.mkdir()
        outside = self.root / "unrelated.so"
        outside.write_bytes(b"matching:debug_info other")
        (escaped / "libskyline.so").symlink_to(outside)
        self.run_archive()
    def test_real_inspector_rejects_wrong_architecture_before_running_tool(self):
        bad = self.root / "host.so"
        bad.write_bytes(b"\x7fELF\x02\x01" + bytes(12) + (62).to_bytes(2, "little") + bytes(44))
        with self.assertRaisesRegex(ValueError, "AArch64"):
            symbols.inspect_elf(bad, pathlib.Path("must_not_run"))

if __name__ == "__main__":
    unittest.main()
