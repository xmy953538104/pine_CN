#!/usr/bin/env python3
"""Synthetic byte-level, CLI, and hostile archive tests; no saves, ROMs or keys."""

from copy import deepcopy
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import stat
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import warnings
import zipfile


ROOT = Path(__file__).resolve().parents[2]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


probe = load("save_probe", ROOT / "tools/pine-save-probe-verify.py")
fixtures = load("save_probe_fixtures", ROOT / "tools/tests/make-save-probe-fixture.py")


class SaveProbeTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.manifest, self.payload = fixtures.write_fixture(self.root)
        self.manifest_path = self.root / "manifest.json"
        self.initial = self.root / "initial" / probe.BASENAME
        self.restart = self.root / "restart" / probe.BASENAME
        self.archive = self.root / "stopped.zip"

    def save(self):
        self.manifest_path.write_text(json.dumps(self.manifest), encoding="utf-8")

    def verify(self, **overrides):
        self.save()
        args = dict(initial_file=self.initial, restart_file=self.restart, export_zip=self.archive)
        args.update(overrides)
        return probe.verify(self.manifest_path, **args)

    def rejected(self, report, stage, reason=None):
        item = report["evidence"][stage]
        item = item.get("input", item)
        self.assertEqual(item["state"], "rejected", item)
        if reason:
            self.assertEqual(item["reason"], reason)
        self.assertEqual(probe.exit_code(report), 1)

    def zip(self, entries=None, compression=zipfile.ZIP_DEFLATED):
        with warnings.catch_warnings():
            warnings.simplefilter("ignore", UserWarning)
            with zipfile.ZipFile(self.archive, "w", compression) as archive:
                for name, data in entries or [(self.manifest["zip_member"], self.payload)]:
                    archive.writestr(name, data)

    def test_positive_layers_are_independent_and_assertions_are_not_authenticated(self):
        result = self.verify()
        self.assertEqual(probe.exit_code(result), 0)
        self.assertEqual(result["assertion_authentication"], "not_authenticated_by_this_tool")
        self.assertEqual(result["manifest_declared_evidence_origin"], "synthetic")
        self.assertEqual(result["durability"]["state"], "unknown")
        self.assertNotIn("persistence_success", result)
        for stage in ("initial", "restart", "export"):
            with self.subTest(stage=stage):
                layer = result["evidence"][stage]
                self.assertEqual(layer["input"]["state"], "observed_match")
                for field in probe.IDENTITY_FIELDS:
                    self.assertEqual(layer["identity"][field]["state"], "asserted_match")
                for field in ("length", "hash", "nonce", "write_id", "format"):
                    self.assertEqual(layer["payload"][field]["state"], "observed_match")
                self.assertEqual(layer["payload"]["sha256"], hashlib.sha256(self.payload).hexdigest())
        self.assertEqual(result["evidence"]["export"]["zip"]["other_member_bodies_read"], 0)

    def test_missing_fields_and_inputs_are_missing_evidence(self):
        self.manifest.pop("expected")
        self.manifest.pop("observations")
        report = self.verify(restart_file=None, export_zip=None)
        self.assertEqual(probe.exit_code(report), 2)
        self.assertEqual(report["evidence"]["initial"]["payload"]["nonce"]["state"], "missing_evidence")
        self.assertEqual(report["evidence"]["restart"]["input"]["state"], "missing_evidence")
        self.assertEqual(report["evidence"]["writer"]["callback"]["state"], "missing_evidence")
        self.assertEqual(report["evidence"]["initial"]["identity"]["path"]["state"], "missing_evidence")

    def test_unknown_codec_does_not_read_or_guess_payload_format(self):
        for codec in ("game-binary-unknown", None):
            with self.subTest(codec=codec):
                if codec is None:
                    self.manifest.pop("codec", None)
                else:
                    self.manifest["codec"] = codec
                with patch.object(probe, "read_zip_member", side_effect=AssertionError("no ZIP payload read")):
                    # Missing files would produce a rejection if a payload read were attempted.
                    report = self.verify(initial_file=self.root / "absent", restart_file=self.root / "absent")
                self.assertEqual(probe.exit_code(report), 2)
                self.assertNotIn("payload", report["evidence"]["initial"])
                self.assertEqual(report["evidence"]["codec"]["state"], "unsupported" if codec else "missing_evidence")

    def test_wrong_observed_owner_user_program_space_type_or_path_contradicts(self):
        for field, value in (("save_owner_id", "0000000000009999"), ("user_id", "0" * 32),
                             ("program_id", "f" * 16), ("space", "system"), ("type", "cache"),
                             ("path", "another/subtree/" + probe.BASENAME)):
            with self.subTest(field=field):
                self.manifest, _ = fixtures.fixture()
                self.manifest["observations"]["initial"]["identity"][field] = value
                report = self.verify()
                self.assertEqual(report["evidence"]["initial"]["identity"][field]["state"], "contradiction")
                self.assertEqual(report["evidence"]["initial"]["payload"]["hash"]["state"], "observed_match")
                self.assertEqual(probe.exit_code(report), 1)

    def test_unknown_identity_stays_unknown_and_does_not_become_a_match(self):
        self.manifest["observations"]["initial"]["identity"]["space"] = "unknown"
        self.assertEqual(self.verify()["evidence"]["initial"]["identity"]["space"]["state"], "unknown")

    def test_nonce_hash_length_and_write_id_are_compared_independently(self):
        decoded = json.loads(self.payload)
        decoded["nonce"] = "0" * 32
        decoded["write_id"] = "0" * 32
        self.initial.write_text(json.dumps(decoded), encoding="utf-8")
        report = self.verify()
        payload = report["evidence"]["initial"]["payload"]
        self.assertEqual(payload["format"]["state"], "observed_match")
        for field in ("nonce", "hash", "length", "write_id"):
            self.assertEqual(payload[field]["state"], "contradiction", field)
        self.assertEqual(report["evidence"]["restart"]["payload"]["nonce"]["state"], "observed_match")

    def test_callback_writer_failure_or_close_does_not_certify_bytes(self):
        for field, value, output in (("callback", "failure", "callback"), ("writer", "failed", "completion"), ("file_closed", False, "file_closed")):
            with self.subTest(field=field):
                self.manifest["observations"]["writer"][field] = value
                report = self.verify()
                self.assertEqual(report["evidence"]["writer"][output]["state"], "contradiction")
                self.assertEqual(report["evidence"]["initial"]["payload"]["hash"]["state"], "observed_match")
                self.manifest, _ = fixtures.fixture()

    def test_restart_is_missing_without_separate_file_and_record(self):
        report = self.verify(restart_file=None)
        self.assertEqual(probe.exit_code(report), 2)
        self.assertEqual(report["evidence"]["restart"]["input"]["state"], "missing_evidence")
        self.manifest["observations"].pop("restart")
        report = self.verify()
        self.assertEqual(probe.exit_code(report), 2)
        self.assertEqual(report["evidence"]["restart"]["input"]["state"], "observed_match")
        self.assertEqual(report["evidence"]["restart"]["distinct_process"]["state"], "missing_evidence")

    def test_restart_reuse_of_path_hardlink_process_or_capture_is_rejected(self):
        self.rejected(self.verify(restart_file=self.initial), "restart", "initial_file_reused_for_restart")
        self.restart.unlink()
        os.link(self.initial, self.restart)
        self.rejected(self.verify(), "restart", "initial_file_reused_for_restart")
        self.restart.unlink()
        self.restart.write_bytes(self.payload)
        for field, output in (("process_id", "distinct_process"), ("capture_id", "distinct_capture")):
            with self.subTest(field=field):
                self.manifest, _ = fixtures.fixture()
                self.manifest["observations"]["restart"][field] = self.manifest["observations"]["initial"][field]
                report = self.verify()
                self.assertEqual(report["evidence"]["restart"][output]["state"], "contradiction")

    def test_restart_mismatch_is_independent_of_initial_and_zip(self):
        decoded = json.loads(self.payload)
        decoded["nonce"] = "1" * 32
        self.restart.write_text(json.dumps(decoded), encoding="utf-8")
        report = self.verify()
        self.assertEqual(report["evidence"]["restart"]["payload"]["nonce"]["state"], "contradiction")
        for stage in ("initial", "export"):
            self.assertEqual(report["evidence"][stage]["payload"]["nonce"]["state"], "observed_match")

    def test_restart_requires_source_api_signature_stop_and_write_binding(self):
        for field, value, output in (("source", "unverified_get", "source"), ("api_signature_verified", False, "api_signature_verified"),
                                     ("previous_process_stopped", False, "previous_process_stopped"), ("write_id", "0" * 32, "write_id")):
            with self.subTest(field=field):
                self.manifest, _ = fixtures.fixture()
                self.manifest["observations"]["restart"][field] = value
                self.assertEqual(self.verify()["evidence"]["restart"][output]["state"], "contradiction")

    def test_uncoordinated_export_has_matching_bytes_but_unknown_consistency(self):
        for consistency in ("uncoordinated", "unknown"):
            self.manifest["observations"]["export"]["consistency"] = consistency
            report = self.verify()
            self.assertEqual(report["evidence"]["export"]["consistency"]["state"], "unknown")
            self.assertEqual(report["evidence"]["export"]["payload"]["hash"]["state"], "observed_match")
            self.assertEqual(probe.exit_code(report), 2)
        self.manifest["observations"]["export"]["consistency"] = "coordinated"
        self.assertEqual(probe.exit_code(self.verify()), 0)

    def test_ui_export_member_binds_to_default_user_and_save_owner_not_application_id(self):
        for field, value in (("user_id", "f" * 32), ("program_id", "f" * 16), ("space", "temporary"),
                             ("type", "cache"), ("path", "unrelated/" + probe.BASENAME)):
            with self.subTest(field=field):
                self.manifest, _ = fixtures.fixture()
                self.manifest["observations"]["export"]["identity"][field] = value
                report = self.verify()
                self.assertEqual(report["evidence"]["export"]["ui_export_scope"]["state"], "contradiction")
        self.manifest, _ = fixtures.fixture()
        self.manifest["zip_member"] = "different-owner/" + probe.BASENAME
        self.zip()
        report = self.verify()
        self.assertEqual(report["evidence"]["export"]["payload"]["hash"]["state"], "observed_match")
        self.assertEqual(report["evidence"]["export"]["ui_export_scope"]["state"], "contradiction")
        self.manifest["observations"]["export"]["source"] = "explicit_dedicated_probe_archive"
        self.assertEqual(self.verify()["evidence"]["export"]["ui_export_scope"]["state"], "unknown")

    def test_hex_identity_case_is_normalized_but_actual_uppercase_paths_are_exact(self):
        self.assertIn("ABCDEF", self.manifest["zip_member"])
        self.assertIn("abcdef", self.manifest["expected"]["identity"]["save_owner_id"])
        self.assertEqual(probe.exit_code(self.verify()), 0)
        # A broader dedicated archive can describe a nondefault user, with no UI coverage claim.
        user = "0123456789abcdef0123456789abcdef"
        self.manifest["expected"]["identity"]["user_id"] = user
        old_path = self.manifest["expected"]["identity"]["path"]
        new_path = old_path.replace("00000000000000000000000000000001", user.upper())
        self.manifest["expected"]["identity"]["path"] = new_path
        for observation in self.manifest["observations"].values():
            observation["identity"]["user_id"] = user.upper()
            observation["identity"]["path"] = new_path
        self.manifest["observations"]["export"]["source"] = "explicit_dedicated_probe_archive"
        result = self.verify()
        for stage in ("initial", "restart", "export"):
            for field in ("user_id", "save_owner_id", "program_id", "path"):
                self.assertEqual(result["evidence"][stage]["identity"][field]["state"], "asserted_match")
            self.assertEqual(result["evidence"][stage]["payload"]["hash"]["state"], "observed_match")
        self.assertEqual(result["evidence"]["export"]["ui_export_scope"]["state"], "unknown")
        self.assertEqual(probe.exit_code(result), 2)
        # Numeric IDs can agree while case-folding an actual filesystem path is false.
        self.manifest, _ = fixtures.fixture()
        lowercase_path = self.manifest["expected"]["identity"]["path"].lower()
        self.manifest["observations"]["initial"]["identity"]["path"] = lowercase_path
        self.assertEqual(self.verify()["evidence"]["initial"]["identity"]["path"]["state"], "contradiction")
        self.manifest["expected"]["identity"]["path"] = lowercase_path
        for observation in self.manifest["observations"].values():
            observation["identity"]["path"] = lowercase_path
        self.manifest["zip_member"] = self.manifest["zip_member"].lower()
        self.zip()
        self.assertEqual(self.verify()["evidence"]["export"]["ui_export_scope"]["state"], "contradiction")

    def test_zip_member_absent_and_member_not_declared_are_distinct(self):
        self.zip([("different/" + probe.BASENAME, self.payload)])
        self.rejected(self.verify(), "export", "zip_declared_member_absent")
        self.manifest.pop("zip_member")
        self.assertEqual(self.verify()["evidence"]["export"]["input"]["state"], "missing_evidence")

    def test_zip_rejects_duplicates_case_aliases_and_case_only_selection(self):
        member = self.manifest["zip_member"]
        for entries in ([(member, self.payload), (member, self.payload)],
                        [(member, self.payload), (member.upper(), self.payload)],
                        [("DIR/other.bin", b"1"), ("dir/OTHER.bin", b"2"), (member, self.payload)],
                        [("caf\u00e9/other.bin", b"1"), ("cafe\u0301/other.bin", b"2"), (member, self.payload)]):
            self.zip(entries)
            self.rejected(self.verify(), "export", "zip_duplicate_or_case_alias")
        self.zip([(member.upper(), self.payload)])
        self.rejected(self.verify(), "export", "zip_member_case_mismatch")

    def test_zip_rejects_traversal_absolute_drive_backslash_and_control_names(self):
        for name in ("../private", "/private", "C:/private", "a\\private", "a/./private", "a//private", "bad\nname"):
            with self.subTest(name=repr(name)):
                self.zip([(self.manifest["zip_member"], self.payload), (name, b"not read")])
                self.rejected(self.verify(), "export", "unsafe_relative_name")

    def test_zip_rejects_symlink_special_file_encryption_and_unsupported_compression(self):
        for file_type in (stat.S_IFLNK, stat.S_IFIFO, stat.S_IFSOCK):
            info = zipfile.ZipInfo("link-or-special")
            info.create_system = 3
            info.external_attr = (file_type | 0o600) << 16
            self.zip([(self.manifest["zip_member"], self.payload), (info, b"private-target")])
            self.rejected(self.verify(), "export", "zip_symlink_or_special_entry")
        self.zip()
        raw = bytearray(self.archive.read_bytes())
        offset = raw.index(b"PK\x01\x02")
        struct.pack_into("<H", raw, offset + 8, 1)
        self.archive.write_bytes(raw)
        self.rejected(self.verify(), "export", "zip_encrypted_entry")
        self.zip(compression=zipfile.ZIP_BZIP2)
        self.rejected(self.verify(), "export", "zip_compression_unsupported")

    def test_zip_metadata_and_physical_archive_limits_precede_payload_read(self):
        self.zip()
        for field_offset, form, value in ((10, "<H", probe.MAX_ENTRIES + 1), (12, "<I", probe.MAX_CENTRAL_DIRECTORY + 1)):
            raw = bytearray(self.archive.read_bytes())
            end = raw.rindex(b"PK\x05\x06")
            struct.pack_into(form, raw, end + field_offset, value)
            if field_offset == 10:
                struct.pack_into("<H", raw, end + 8, value)
            self.archive.write_bytes(raw)
            self.rejected(self.verify(), "export", "zip_directory_limit_exceeded")
            self.zip()
        with self.archive.open("r+b") as stream:
            stream.truncate(probe.MAX_ZIP + 1)
        self.rejected(self.verify(), "export", "input_byte_limit_exceeded")

    def test_zip64_locator_and_actual_central_record_count_are_checked_before_stdlib(self):
        self.zip()
        raw = bytearray(self.archive.read_bytes())
        end = raw.rindex(b"PK\x05\x06")
        directory_size = struct.unpack_from("<I", raw, end + 12)[0]
        raw[end:end] = b"PK\x06\x07" + b"\0" * 16
        struct.pack_into("<I", raw, end + 20 + 12, directory_size + 20)
        self.archive.write_bytes(raw)
        self.rejected(self.verify(), "export", "zip64_unsupported")
        self.zip()
        raw = bytearray(self.archive.read_bytes())
        end = raw.rindex(b"PK\x05\x06")
        struct.pack_into("<2H", raw, end + 8, 0, 0)
        self.archive.write_bytes(raw)
        self.rejected(self.verify(), "export", "zip_directory_count_mismatch")
        entries = [("entry-%04d" % number, b"") for number in range(probe.MAX_ENTRIES + 1)]
        self.zip(entries)
        raw = bytearray(self.archive.read_bytes())
        end = raw.rindex(b"PK\x05\x06")
        struct.pack_into("<2H", raw, end + 8, 1, 1)
        self.archive.write_bytes(raw)
        self.rejected(self.verify(), "export", "zip_directory_limit_exceeded")

    def test_zip_declared_and_actual_output_are_both_bounded(self):
        self.zip([(self.manifest["zip_member"], b"a" * (probe.MAX_PAYLOAD + 1))])
        self.rejected(self.verify(), "export", "zip_member_byte_limit_exceeded")
        # Lie in both local and central metadata: a truncated ZipExtFile-style read is insufficient.
        self.zip([(self.manifest["zip_member"], b"a" * 20000)])
        raw = bytearray(self.archive.read_bytes())
        central = raw.index(b"PK\x01\x02")
        struct.pack_into("<I", raw, 22, 100)
        struct.pack_into("<I", raw, central + 24, 100)
        self.archive.write_bytes(raw)
        self.rejected(self.verify(), "export", "zip_actual_output_limit_exceeded")

    def test_corrupt_crc_truncated_archive_short_and_trailing_deflate_are_rejected(self):
        self.zip(compression=zipfile.ZIP_STORED)
        raw = bytearray(self.archive.read_bytes())
        payload_at = 30 + struct.unpack_from("<H", raw, 26)[0] + struct.unpack_from("<H", raw, 28)[0]
        raw[payload_at] ^= 1
        self.archive.write_bytes(raw)
        self.rejected(self.verify(), "export", "zip_actual_length_or_crc_mismatch")
        self.archive.write_bytes(b"not a zip")
        self.rejected(self.verify(), "export", "zip_end_record_missing")
        self.zip()
        raw = self.archive.read_bytes()
        self.archive.write_bytes(raw[:-5])
        self.rejected(self.verify(), "export")
        self.zip()
        raw = bytearray(self.archive.read_bytes())
        central = raw.index(b"PK\x01\x02")
        compressed_size = struct.unpack_from("<I", raw, central + 20)[0]
        # Shorten the recorded deflate stream while preserving structural directory ranges.
        struct.pack_into("<I", raw, 18, compressed_size - 1)
        struct.pack_into("<I", raw, central + 20, compressed_size - 1)
        self.archive.write_bytes(raw)
        self.rejected(self.verify(), "export", "zip_deflate_short_or_trailing_data")
        self.zip()
        raw = bytearray(self.archive.read_bytes())
        central = raw.index(b"PK\x01\x02")
        compressed_size = struct.unpack_from("<I", raw, central + 20)[0]
        raw[central:central] = b"X"
        central += 1
        struct.pack_into("<I", raw, 18, compressed_size + 1)
        struct.pack_into("<I", raw, central + 20, compressed_size + 1)
        struct.pack_into("<I", raw, raw.rindex(b"PK\x05\x06") + 16, central)
        self.archive.write_bytes(raw)
        self.rejected(self.verify(), "export", "zip_deflate_short_or_trailing_data")

    def test_streaming_android_style_zip_data_descriptor_supported_and_checked(self):
        class Unseekable(io.BytesIO):
            def seekable(self):
                return False

            def seek(self, *args):
                raise io.UnsupportedOperation("seek")

        buffer = Unseekable()
        with zipfile.ZipFile(buffer, "w", zipfile.ZIP_DEFLATED) as archive:
            archive.writestr(self.manifest["zip_member"], self.payload)
        self.archive.write_bytes(buffer.getvalue())
        self.assertEqual(probe.exit_code(self.verify()), 0)
        raw = bytearray(buffer.getvalue())
        descriptor = raw.index(b"PK\x07\x08")
        raw[descriptor + 4] ^= 1
        self.archive.write_bytes(raw)
        self.rejected(self.verify(), "export", "zip_descriptor_mismatch")

    def test_stored_member_directories_and_unrelated_corrupt_bodies_are_not_read(self):
        member = self.manifest["zip_member"]
        self.zip([(member.split("/")[0] + "/", b""), (member, self.payload), ("unrelated-private.bin", b"DO NOT READ")], zipfile.ZIP_STORED)
        raw = bytearray(self.archive.read_bytes())
        raw[raw.index(b"DO NOT READ")] ^= 1
        self.archive.write_bytes(raw)
        # Success here is intentional: no archive-wide integrity or snapshot claim is made.
        self.assertEqual(probe.exit_code(self.verify()), 0)

    def test_explicit_allowlist_never_scans_directory_or_opens_unlisted_files(self):
        (self.root / "private-save-secret").write_bytes(b"DO NOT OPEN")
        with patch.object(Path, "rglob", side_effect=AssertionError("recursive scan")), patch.object(Path, "iterdir", side_effect=AssertionError("directory scan")), patch.object(zipfile.ZipFile, "open", side_effect=AssertionError("unbounded member reader")):
            original_open = os.open
            opened = []

            def track(path, *args, **kwargs):
                opened.append(Path(path))
                return original_open(path, *args, **kwargs)

            with patch.object(os, "open", side_effect=track):
                self.assertEqual(probe.exit_code(self.verify()), 0)
        self.assertEqual(opened, [self.manifest_path, self.initial, self.restart, self.archive])
        self.rejected(self.verify(initial_file=self.root / "private-save-secret"), "initial", "input_basename_is_not_dedicated_probe")
        self.rejected(self.verify(initial_file=self.initial.parent), "initial", "input_basename_is_not_dedicated_probe")

    def test_symlink_fifo_oversized_files_and_changed_reads_are_rejected(self):
        self.restart.unlink()
        self.restart.symlink_to(self.initial)
        self.rejected(self.verify(), "restart", "input_open_failed_or_symlink")
        self.restart.unlink()
        os.mkfifo(self.restart)
        self.rejected(self.verify(), "restart", "input_is_not_regular_file")
        self.restart.unlink()
        self.restart.write_bytes(b"x" * (probe.MAX_PAYLOAD + 1))
        self.rejected(self.verify(), "restart", "input_byte_limit_exceeded")
        self.restart.write_bytes(self.payload)
        original_unchanged = probe.unchanged

        def changed(stream, before):
            if before.st_ino == self.initial.stat().st_ino:
                probe.fail("input_changed_during_read")
            return original_unchanged(stream, before)

        with patch.object(probe, "unchanged", side_effect=changed):
            self.rejected(self.verify(), "initial", "input_changed_during_read")

    def test_strict_json_payload_rejects_shared_private_objects_duplicates_and_wrong_markers(self):
        for data in (b'{"world":"private"}', self.payload.replace(b'"schema":1', b'"schema":true'),
                     self.payload.replace(b'"schema":1', b'"schema":1,"schema":1'),
                     self.payload.replace(b'"text":"pine dedicated save probe v1"', b'"text":"private"'),
                     b"\xff", b'{"schema": NaN}'):
            with self.subTest(data_length=len(data)):
                self.initial.write_bytes(data)
                self.assertEqual(self.verify()["evidence"]["initial"]["payload"]["format"]["state"], "rejected")

    def test_manifest_strict_schema_authorization_duplicate_and_unicode_errors_are_safe(self):
        cases = []
        base = deepcopy(self.manifest)
        for key, value in (("authorization", {}), ("schema", "unknown"), ("unexpected_secret", "NEVER PRINT THIS"),
                           ("zip_member", "../" + probe.BASENAME)):
            case = deepcopy(base)
            case[key] = value
            cases.append(json.dumps(case).encode())
        cases.extend((b'{"schema":1,"schema":2}', b'{"schema": NaN}', b"\xff"))
        invalid_unicode = deepcopy(base)
        invalid_unicode["expected"]["identity"]["path"] = "\ud800/" + probe.BASENAME
        cases.append(json.dumps(invalid_unicode).encode())
        for data in cases:
            self.manifest_path.write_bytes(data)
            report = probe.verify(self.manifest_path)
            self.rejected(report, "manifest")
            self.assertNotIn("NEVER PRINT THIS", json.dumps(report))

    def test_committed_examples_match_the_synthetic_generator_and_byte_expectations(self):
        example = ROOT / "tools/save-probe/example"
        self.assertEqual(json.loads((example / "manifest-v1.example.json").read_text()), self.manifest)
        self.assertEqual((example / probe.BASENAME).read_bytes(), self.payload)
        self.assertEqual(probe.payload_checks(self.payload, self.manifest["expected"]["payload"], self.manifest["expected"]["write_id"])["hash"]["state"], "observed_match")

    def test_cli_outputs_redacted_report_and_meaningful_exit_codes(self):
        command = [sys.executable, str(ROOT / "tools/pine-save-probe-verify.py"), "--manifest", str(self.manifest_path),
                   "--initial-file", str(self.initial), "--restart-file", str(self.restart), "--export-zip", str(self.archive)]
        result = subprocess.run(command, capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(result.stdout)
        for private in (str(self.root), self.manifest["expected"]["identity"]["path"], self.manifest["expected"]["payload"]["nonce"], "pine dedicated save probe v1"):
            self.assertNotIn(private, result.stdout)
        self.assertEqual(report["evidence"]["initial"]["payload"]["bytes"], len(self.payload))
        self.assertEqual(subprocess.run(command[:4], capture_output=True, check=False).returncode, 2)
        self.initial.write_bytes(b"bad payload")
        self.assertEqual(subprocess.run(command, capture_output=True, check=False).returncode, 1)


if __name__ == "__main__":
    unittest.main(verbosity=2)
