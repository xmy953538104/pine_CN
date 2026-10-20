#!/usr/bin/env python3
"""Controlled offline values and real JSON/ZIP bytes; no Guest/APK execution."""
import copy
import ctypes
import io
import json
from pathlib import Path
import random
import subprocess
import sys
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import nvn_abort_contract as decoder

BASE = 0x7100000000
MASK = (1 << 64) - 1


def hx(value):
    return f"0x{value:016x}"


def sample(marker=0x7A032A):
    return {"module": {"name": "sdk.nso", "build_id": "a" * 64,
                       "image_base": hx(BASE), "text_base": hx(BASE), "text_size": hx(0x400000)},
            "instruction_le": "c04c0494",
            "context": {"session_id": "synthetic-session", "launch_id": "synthetic-launch",
                        "process_run_id": "synthetic-process", "guest_thread_id": "synthetic-thread"},
            "registers": {"pc": hx(BASE + 0x2D64E0), "lr": hx(MASK), "sp": hx(BASE + 0x10000),
                          "x0": hx(BASE + marker), "x8": hx(3), "x9": hx(12), "x10": hx(2),
                          "x19": hx(4), "x20": hx(0xFEDCBA9876543210)}}


class ContractTests(unittest.TestCase):
    def decode(self, value=None):
        result = decoder.parse(json.dumps(sample() if value is None else value).encode())
        self.assertFalse(result["runtime_connected"])
        self.assertEqual(result["runtime_admission"], "STOP")
        self.assertEqual(tuple(result["stop_codes"]), decoder.STOP_CODES)
        return result

    def test_complete_claims_never_admit_runtime(self):
        result = self.decode()
        self.assertEqual(result["contract_candidate"], "INSUFFICIENT_AFTER_RETURN")
        self.assertIn("not_verified", result["classification_scope"])
        self.assertIn("unverified", result["module_verification"])

    def test_supplied_snapshot_is_not_mutated_and_output_is_bounded(self):
        value = sample()
        value["context"] = {key: "a" * 128 for key in decoder.CONTEXT_KEYS}
        before = copy.deepcopy(value)
        result = self.decode(value)
        self.assertEqual(value, before)
        self.assertLess(len(json.dumps(result).encode()), 8192)

    def test_no_callback_does_not_interpret_unassigned_x9_x10(self):
        value = sample(0x7A0371)
        value["registers"].update(x8=hx(0), x9=hx(MASK), x10=hx(MASK))
        result = self.decode(value)
        self.assertEqual(result["contract_candidate"], "NO_CALLBACK")
        self.assertIsNone(result["derived_algebra"])
        self.assertEqual(result["raw_registers"]["x10"], hx(MASK))
        self.assertEqual(result["x9_x10_interpretation"], "prohibited_on_no_callback_path")

    def test_no_callback_unavailable_x9_x10_stays_null(self):
        value = sample(0x7A0371)
        value["registers"].update(x8=hx(0), x9=None, x10=None)
        result = self.decode(value)
        self.assertEqual(result["contract_candidate"], "NO_CALLBACK")
        self.assertIsNone(result["raw_registers"]["x9"])
        self.assertIn("x10", result["unavailable_registers"])

    def test_no_callback_nonzero_or_unknown_x8_is_unclassified(self):
        for register in (hx(1), None):
            value = sample(0x7A0371); value["registers"]["x8"] = register
            self.assertEqual(self.decode(value)["contract_candidate"], "UNCLASSIFIED")

    def test_insufficient_algebra_is_not_object_memory(self):
        result = self.decode()
        algebra = result["derived_algebra"]
        self.assertEqual(algebra["p18_mod64"], hx(9))
        self.assertEqual(algebra["p20_mod64"], hx(4))
        self.assertEqual(algebra["r70_u32_hex"], "0x00000002")
        self.assertIn("not_object_memory_or_callback", algebra["scope"])

    def test_signed_negative_available_is_insufficient(self):
        value = sample(); value["registers"].update(x8=hx(MASK), x19=hx(0))
        self.assertEqual(self.decode(value)["contract_candidate"], "INSUFFICIENT_AFTER_RETURN")

    def test_signed_high_bit_request_can_reverse_unsigned_comparison(self):
        value = sample(); value["registers"].update(x8=hx(0), x19=hx(1 << 63))
        self.assertEqual(self.decode(value)["contract_candidate"], "UNCLASSIFIED")

    def test_signed_minimum_and_maximum(self):
        value = sample(); value["registers"].update(x8=hx(1 << 63), x19=hx((1 << 63) - 1))
        self.assertEqual(self.decode(value)["contract_candidate"], "INSUFFICIENT_AFTER_RETURN")

    def test_equal_or_greater_available_is_unclassified(self):
        for register in (4, 5):
            value = sample(); value["registers"]["x8"] = hx(register)
            self.assertEqual(self.decode(value)["contract_candidate"], "UNCLASSIFIED")

    def test_x10_upper32_must_be_zero(self):
        value = sample(); value["registers"]["x10"] = hx(1 << 32)
        result = self.decode(value)
        self.assertEqual(result["reason"], "x10_upper32_not_zero")
        self.assertIsNone(result["derived_algebra"])

    def test_modular_subtraction_and_multiplication_do_not_saturate(self):
        value = sample(); value["registers"].update(x8=hx(1), x9=hx(0), x10=hx(0xFFFFFFFF), x19=hx(2))
        algebra = self.decode(value)["derived_algebra"]
        self.assertEqual(algebra["p18_mod64"], hx(MASK))
        self.assertEqual(algebra["p20_mod64"], hx((1 << 64) - 4 * 0xFFFFFFFF))

    def test_deterministic_10000_comparisons_and_independent_algebra_invariants(self):
        rng = random.Random(0x2D64E0)
        for _ in range(10000):
            a, wanted, end, count = rng.getrandbits(64), rng.getrandbits(64), rng.getrandbits(64), rng.getrandbits(32)
            value = sample(); value["registers"].update(x8=hx(a), x19=hx(wanted), x9=hx(end), x10=hx(count))
            result = self.decode(value)
            insufficient = ctypes.c_int64(a).value < ctypes.c_int64(wanted).value
            self.assertEqual(result["contract_candidate"] == "INSUFFICIENT_AFTER_RETURN", insufficient)
            if insufficient:
                algebra = result["derived_algebra"]
                self.assertEqual((int(algebra["p18_mod64"], 16) + a) & MASK, end)
                self.assertEqual((int(algebra["p20_mod64"], 16) + 4 * count) & MASK, end)

    def test_unknown_required_registers_are_not_zero_filled(self):
        for name in ("x8", "x9", "x10", "x19"):
            value = sample(); del value["registers"][name]
            result = self.decode(value)
            self.assertEqual(result["contract_candidate"], "UNCLASSIFIED")
            self.assertIsNone(result["raw_registers"][name])

    def test_lr_sp_x20_are_raw_and_may_be_explicitly_unavailable(self):
        value = sample(); value["registers"].update(lr=None, sp=None, x20=None)
        result = self.decode(value)
        self.assertEqual(result["contract_candidate"], "INSUFFICIENT_AFTER_RETURN")
        self.assertTrue({"lr", "sp", "x20"} <= set(result["unavailable_registers"]))

    def test_context_absence_is_explicit_and_does_not_invent_ids(self):
        value = sample(); del value["context"]
        result = self.decode(value)
        self.assertTrue(all(v is None for v in result["context_claims"].values()))
        self.assertEqual(set(result["unavailable_context"]), set(decoder.CONTEXT_KEYS))

    def test_unknown_module_and_branch_marker_are_unclassified(self):
        for change in ("module", "x0"):
            value = sample()
            if change == "module": value["module"]["name"] = "main.nso"
            else: value["registers"]["x0"] = hx(BASE + 0x7A032B)
            self.assertEqual(self.decode(value)["contract_candidate"], "UNCLASSIFIED")

    def test_pc_or_instruction_claim_mismatch(self):
        for field in ("pc", "instruction"):
            value = sample()
            if field == "pc": value["registers"]["pc"] = hx(BASE + 0x2D64E4)
            else: value["instruction_le"] = "1f2003d5"
            self.assertEqual(self.decode(value)["contract_candidate"], "UNCLASSIFIED")

    def test_text_base_includes_no_invented_patch_offset(self):
        value = sample(); value["module"]["text_base"] = hx(BASE + 0x1000)
        self.assertEqual(self.decode(value)["reason"], "supplied_text_va_zero_contract_not_met")

    def test_text_range_requires_all_four_instruction_bytes(self):
        value = sample(); value["module"]["text_size"] = hx(0x2D64E3)
        self.assertEqual(self.decode(value)["contract_candidate"], "UNCLASSIFIED")

    def test_pointer_add_overflow_is_not_wrapped(self):
        value = sample(); value["module"].update(image_base=hx(MASK - 8), text_base=hx(MASK - 8))
        self.assertEqual(self.decode(value)["reason"], "address_overflow")

    def test_json_and_real_zip_preserve_every_raw_64_bit_string(self):
        result = self.decode(); buffer = io.BytesIO()
        with zipfile.ZipFile(buffer, "w") as archive:
            archive.writestr("offline-synthetic.json", json.dumps(result, ensure_ascii=True))
        with zipfile.ZipFile(buffer) as archive:
            restored = json.loads(archive.read("offline-synthetic.json"))
        self.assertEqual(restored, result)
        self.assertEqual(restored["raw_registers"]["lr"], "0xffffffffffffffff")
        self.assertIn("not_android", restored["export_scope"])

    def test_numeric_boolean_float_truncated_or_uppercase_u64_rejected(self):
        for raw in (0, 2**64 - 1, True, 1.0, "0x1", "0xFEDCBA9876543210", "-1"):
            value = sample(); value["registers"]["x8"] = raw
            with self.assertRaises(decoder.InputError): decoder.decode(value)

    def test_unverified_claims_cannot_request_runtime_override(self):
        for key in ("runtime_connected", "force", "window_verified"):
            value = sample(); value[key] = True
            with self.assertRaises(decoder.InputError): decoder.decode(value)

    def test_unknown_nested_fields_and_overlong_context_rejected(self):
        for value in ({**sample(), "registers": {"x31": hx(0)}},
                      {**sample(), "context": {"session_id": "a" * 129}},
                      {**sample(), "context": {"session_id": "/private/path"}}):
            with self.assertRaises(decoder.InputError): decoder.decode(value)

    def test_duplicate_keys_nonfinite_utf8_and_input_limit_rejected(self):
        for data in (b'{"module":{},"module":{}}', b'{"registers":{"x8":NaN}}', b'\xff', b' ' * 16385, b'{}{}'):
            with self.assertRaises(decoder.InputError): decoder.parse(data)

    def test_parser_digit_limit_and_deep_nesting_have_bounded_cli_errors(self):
        for data in (b"9" * 5000, b"[" * 1500 + b"0" + b"]" * 1500):
            with self.assertRaises(decoder.InputError): decoder.parse(data)
            process = subprocess.run([sys.executable, str(ROOT / "tools/nvn_abort_contract.py")],
                                     input=data, capture_output=True, timeout=5)
            self.assertEqual(process.returncode, 2)
            self.assertEqual(json.loads(process.stderr)["error"], "STOP_INPUT_INVALID")
            self.assertNotIn(b"Traceback", process.stderr)
            self.assertLess(len(process.stderr), 1024)

    def test_cli_is_bounded_and_marks_success_as_offline_only(self):
        process = subprocess.run([sys.executable, str(ROOT / "tools/nvn_abort_contract.py")],
                                 input=json.dumps(sample()).encode(), capture_output=True, timeout=5)
        self.assertEqual(process.returncode, 0)
        self.assertEqual(json.loads(process.stdout)["runtime_admission"], "STOP")
        self.assertEqual(process.stderr, b"")

    def test_cli_errors_do_not_echo_untrusted_input(self):
        process = subprocess.run([sys.executable, str(ROOT / "tools/nvn_abort_contract.py")],
                                 input=b'{"SECRET_UNTRUSTED_VALUE":1}', capture_output=True, timeout=5)
        self.assertEqual(process.returncode, 2)
        self.assertNotIn(b"SECRET_UNTRUSTED_VALUE", process.stderr)
        self.assertEqual(process.stdout, b"")

    def test_cli_has_no_runtime_override_argument(self):
        process = subprocess.run([sys.executable, str(ROOT / "tools/nvn_abort_contract.py"), "--force"],
                                 input=json.dumps(sample()).encode(), capture_output=True, timeout=5)
        self.assertEqual(process.returncode, 2)
        self.assertEqual(json.loads(process.stderr)["reason"], "unsupported_cli_arguments")


if __name__ == "__main__":
    unittest.main()
