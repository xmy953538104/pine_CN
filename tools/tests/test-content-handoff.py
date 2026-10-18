#!/usr/bin/env python3
"""Only synthetic metadata. No container, NCA, ROM, ticket, key or save is read."""
from copy import deepcopy
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

from jsonschema import Draft202012Validator  # Required dependency; never skip green.

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("handoff", ROOT / "tools/validate-content-handoff.py")
handoff = importlib.util.module_from_spec(spec)
spec.loader.exec_module(handoff)
BASE = ROOT / "tools/content-handoff"


class HandoffTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / "metadata.json"
        self.report = json.loads((BASE / "example.json").read_text())

    def validate(self):
        self.path.write_text(json.dumps(self.report), encoding="utf-8")
        return handoff.validate(self.path)

    def contradiction(self, code):
        result = self.validate()
        self.assertEqual(handoff.exit_code(result), 1, result)
        self.assertIn(code, [item["code"] for item in result["issues"]])

    def test_required_schema_and_both_examples_validate_without_authentication_claims(self):
        schema = json.loads((BASE / "metadata-v1.schema.json").read_text())
        Draft202012Validator.check_schema(schema)
        for name in ("example.json", "unknown-template.json"):
            document = json.loads((BASE / name).read_text())
            Draft202012Validator(schema).validate(document)
            result = handoff.validate(BASE / name)
            self.assertEqual(handoff.exit_code(result), 0, result)
            self.assertEqual(result["verification_scope"], "schema_and_reported_links_only")
            self.assertEqual(result["provenance"], "not_authenticated")
            self.assertFalse(result["signatures_verified"])
            self.assertFalse(result["runtime_verified"])
            self.assertEqual(result["container_or_nca_bytes_read"], 0)
        self.assertEqual(handoff.validate(BASE / "unknown-template.json")["metadata_completeness"], "unknown")

    def test_multiple_programs_are_ambiguous_and_content_id_need_not_be_sha_prefix(self):
        result = self.validate()
        self.assertEqual(handoff.exit_code(result), 0)
        self.assertEqual(result["program_selection"], {"ambiguous_groups": 1, "automatic_selection": "not_performed"})
        reference = self.report["cnmts"][0]["contents"][0]
        self.assertNotEqual(reference["content_id"]["value"].lower(), reference["expected_raw_sha256"]["value"][:32])

    def test_container_record_and_observation_bindings_are_explicit(self):
        self.report["ncas"][0]["container_report_id"] = "other-container"
        self.contradiction("record_container_mismatch")
        self.report["observations"][0]["container_report_id"] = "other-container"
        self.contradiction("observation_container_mismatch")

    def test_duplicate_report_and_observation_ids_are_contradictions(self):
        self.report["ncas"][1]["report_id"] = self.report["ncas"][0]["report_id"]
        self.contradiction("duplicate_report_id")
        self.report["observations"].append(deepcopy(self.report["observations"][0]))
        self.contradiction("duplicate_observation_id")

    def test_dangling_nca_meta_parent_and_observation_references_are_not_resolved_by_hint(self):
        for section, key, code in (("content", "nca_report_id", "content_nca_reference_missing"), ("group", "meta_nca_report_id", "meta_nca_reference_missing"), ("group", "application_parent_report_id", "application_parent_reference_missing")):
            with self.subTest(key=key):
                self.report = json.loads((BASE / "example.json").read_text())
                group = self.report["cnmts"][1]
                target = group["contents"][0] if section == "content" else group
                target[key]["value"] = "missing-id"
                self.contradiction(code)
        self.report["ncas"][0]["nca_type"]["observation_id"] = "missing-observation"
        self.contradiction("fact_observation_missing")

    def test_known_content_and_meta_role_type_mismatches(self):
        self.report["ncas"][0]["nca_type"]["value"] = 0
        self.contradiction("cnmt_source_is_not_meta_nca")
        self.report["ncas"][1]["nca_type"]["value"] = 4
        self.contradiction("content_reference_nca_type_mismatch")

    def test_parent_ids_are_compared_only_from_explicit_known_fields(self):
        self.report["cnmts"][1]["application_parent"]["value"] = "FFFFFFFFFFFFFFFF"
        self.contradiction("explicit_application_parent_mismatch")
        self.report["cnmts"][1]["application_parent"] = {"state": "unknown", "value": None}
        self.assertEqual(handoff.exit_code(self.validate()), 0)
        self.report["cnmts"][0]["role"]["value"] = "Patch"
        self.contradiction("application_parent_role_mismatch")

    def test_application_self_parent_link_without_parent_title_is_rejected(self):
        group = self.report["cnmts"][0]
        self.assertNotIn("application_parent", group)
        group["application_parent_report_id"] = {"state": "observed", "value": group["report_id"],
                                                  "observation_id": "application-cnmt-decode"}
        self.contradiction("role_has_no_explicit_application_parent_field")
        self.contradiction("application_parent_self_reference")

    def test_application_and_system_roles_reject_known_parent_links_without_titles(self):
        for role in ("Application", "SystemProgram", "SystemData", "SystemUpdate", "BootImagePackage", "BootImagePackageSafe"):
            with self.subTest(role=role):
                self.report = json.loads((BASE / "example.json").read_text())
                group = self.report["cnmts"][1]
                group["role"]["value"] = role
                group.pop("application_parent")
                self.contradiction("role_has_no_explicit_application_parent_field")

    def test_unknown_parent_link_and_missing_parent_title_are_not_inferred(self):
        for unknown in ({"state": "unknown", "value": None}, {"state": "unknown", "value": None, "observation_id": None}):
            self.report["cnmts"][0]["application_parent_report_id"] = unknown
            self.assertEqual(handoff.exit_code(self.validate()), 0)
        self.report["cnmts"][0].pop("application_parent_report_id")
        self.report["cnmts"][1].pop("application_parent")
        self.assertEqual(handoff.exit_code(self.validate()), 0)

    def test_known_self_parent_link_is_rejected_even_when_role_is_unknown(self):
        self.report["container"]["enumeration"]["value"] = "partial"
        group = self.report["cnmts"][1]
        group["role"] = {"state": "unknown", "value": None}
        group.pop("application_parent")
        group["application_parent_report_id"]["value"] = group["report_id"]
        self.contradiction("application_parent_self_reference")

    def test_only_known_report_links_form_parent_cycles(self):
        self.report["container"]["enumeration"]["value"] = "partial"
        first, second = self.report["cnmts"]
        for group in (first, second):
            group["role"] = {"state": "unknown", "value": None}
            group.pop("application_parent", None)
        first["application_parent_report_id"] = {"state": "observed", "value": second["report_id"],
                                                 "observation_id": "application-cnmt-decode"}
        self.contradiction("application_parent_cycle")
        first["application_parent_report_id"] = {"state": "unknown", "value": None}
        self.assertEqual(handoff.exit_code(self.validate()), 0)
        first.pop("application_parent_report_id")
        self.assertEqual(handoff.exit_code(self.validate()), 0)

    def test_hash_size_and_raw_six_byte_profile_mismatches_are_reported_not_verified(self):
        self.report["ncas"][1]["raw_sha256"]["value"] = "f" * 64
        self.contradiction("raw_hash_mismatch")
        self.report["ncas"][1]["raw_size"]["value"] += 1
        self.contradiction("raw_size_mismatch")
        self.report["cnmts"][0]["contents"][0]["raw_size_bytes"]["value"] = "010000000000"
        self.contradiction("cnmt_size_profile_mismatch")

    def test_unknown_profile_keeps_raw_bytes_without_guessing_size(self):
        content = self.report["cnmts"][0]["contents"][0]
        content["size_profile"] = {"state": "unknown", "value": None}
        content["raw_size_bytes"]["value"] = "FFFFFFFFFFFF"
        self.report["container"]["enumeration"]["value"] = "partial"
        result = self.validate()
        self.assertEqual(handoff.exit_code(result), 0)
        self.assertEqual(result["unknowns"]["size_profile_comparison_unknown"], 1)

    def test_observed_full_raw_hash_is_distinct_from_reported_or_partial_header_values(self):
        fact = self.report["ncas"][1]["raw_sha256"]
        source = next(item for item in self.report["observations"] if item["observation_id"] == fact["observation_id"])
        source["method"] = "header_fields"
        self.contradiction("observed_hash_not_full_raw_measurement")
        source["state"] = "reported"
        fact["state"] = "reported"
        result = self.validate()
        self.assertEqual(handoff.exit_code(result), 0)
        self.assertEqual(result["raw_nca_hash_report_states"]["reported"], 1)
        source["scope"] = "cnmt"
        self.contradiction("fact_observation_scope_mismatch")

    def test_complete_enumeration_cannot_hide_unknown_unreadable_or_unlisted_metadata(self):
        self.report["ncas"][1]["readability"] = {"state": "unknown", "value": None}
        self.contradiction("complete_enumeration_has_unknown_or_unreadable_metadata")
        self.report["container"]["enumeration"] = {"state": "unknown", "value": None}
        self.assertEqual(handoff.exit_code(self.validate()), 0)
        self.report = json.loads((BASE / "example.json").read_text())
        self.report["cnmts"].pop()
        self.contradiction("complete_enumeration_has_unknown_or_unreadable_metadata")

    def test_unreadable_metadata_is_explicit_and_nullable_links_stay_unknown(self):
        self.report["container"]["enumeration"]["value"] = "partial"
        self.report["container"]["unreadable_metadata"] = [{"state": "observed", "observation_id": "container-enumeration", "value": {"nca_report_id": None, "cnmt_report_id": None, "reason": "encrypted"}}]
        result = self.validate()
        self.assertEqual(handoff.exit_code(result), 0)
        self.assertEqual(result["metadata_completeness"], "declared_partial")
        self.report["container"]["enumeration"]["value"] = "complete"
        self.contradiction("complete_enumeration_has_unknown_or_unreadable_metadata")

    def test_unknown_fact_cannot_carry_value_or_known_source(self):
        self.report["ncas"][0]["requires_base"] = {"state": "unknown", "value": False}
        self.assertEqual(self.validate()["reason"], "schema_invalid")
        self.report["ncas"][0]["requires_base"] = {"state": "unknown", "value": None, "observation_id": "hidden-source"}
        self.assertEqual(self.validate()["reason"], "schema_invalid")

    def test_program_index_mapping_is_explicit_and_never_derived_from_program_id(self):
        self.report["cnmts"][0]["contents"][0]["program_index"]["value"] = 42
        self.contradiction("program_index_id_offset_mismatch")
        self.report["cnmts"][0]["contents"][0].pop("program_index_basis")
        self.assertEqual(handoff.exit_code(self.validate()), 0)

    def test_no_private_keys_values_paths_or_validation_error_text_are_printed(self):
        for field in ("keys", "ticket", "raw_data", "host_path", "uri", "notes", "logs", "credentials"):
            self.report = json.loads((BASE / "unknown-template.json").read_text())
            self.report[field] = "PRIVATE_SECRET_123/owner/save/never-print"
            self.validate()
            result = subprocess.run([sys.executable, str(ROOT / "tools/validate-content-handoff.py"), str(self.path)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 2)
            self.assertNotIn("PRIVATE_SECRET", result.stdout + result.stderr)
            self.assertNotIn(str(self.path), result.stdout + result.stderr)
            self.assertEqual(json.loads(result.stdout)["reason"], "schema_invalid")

    def test_duplicate_json_utf8_nonfinite_and_byte_limits_have_safe_codes(self):
        for raw, reason in ((b'{"PRIVATE_KEY":1,"PRIVATE_KEY":2}', "duplicate_json_key"), (b'\xff', "invalid_utf8_json"), (b'{"x":NaN}', "non_finite_json_number"), (b' ' * (handoff.MAX_BYTES + 1), "input_byte_limit")):
            self.path.write_bytes(raw)
            result = handoff.validate(self.path)
            self.assertEqual(result["reason"], reason)
            self.assertNotIn("PRIVATE", json.dumps(result))

    def test_nonregular_symlink_and_fifo_do_not_block_or_follow(self):
        self.path.symlink_to(BASE / "example.json")
        self.assertEqual(handoff.validate(self.path)["reason"], "input_open_failed_or_symlink")
        self.path.unlink()
        os.mkfifo(self.path)
        result = subprocess.run([sys.executable, str(ROOT / "tools/validate-content-handoff.py"), str(self.path)], capture_output=True, text=True, timeout=3)
        self.assertEqual(result.returncode, 2)
        self.assertEqual(json.loads(result.stdout)["reason"], "input_not_regular")

    def test_explicit_input_only_and_required_dependency_never_skips_green(self):
        with patch.object(Path, "rglob", side_effect=AssertionError("no recursive scanning")), patch.object(Path, "iterdir", side_effect=AssertionError("no directory scanning")):
            self.assertEqual(handoff.exit_code(self.validate()), 0)
        with patch.object(handoff, "Draft202012Validator", None):
            result = handoff.validate(self.path)
        self.assertEqual(handoff.exit_code(result), 2)
        self.assertEqual(result["reason"], "required_jsonschema_dependency_missing")


if __name__ == "__main__":
    unittest.main(verbosity=2)
