# SPDX-License-Identifier: MPL-2.0
"""Public synthetic byte fixtures; no signature or credential is genuine."""

import json
from dataclasses import FrozenInstanceError
import inspect
import unittest

try:
    from . import auxiliary as aux
except ImportError:
    import auxiliary as aux


# Independent literal fixture table from the pinned Pine switch; not generated
# through the classifier's constants or a production serialization function.
LAYOUTS = ((0x010000, 0x240, 0x3C0), (0x010001, 0x140, 0x2C0),
           (0x010002, 0x80, 0x200), (0x010003, 0x240, 0x3C0),
           (0x010004, 0x140, 0x2C0), (0x010005, 0x80, 0x200))


def synthetic_ticket(signature=0x010001):
    _, body, extent = next(row for row in LAYOUTS if row[0] == signature)
    result = bytearray(extent)
    result[:4] = signature.to_bytes(4, "little")
    # A fixed public pattern, deliberately not a signature or usable key.
    result[4:body] = b"\xA6" * (body - 4)
    issuer = b"Synthetic-Only-No-Credential"
    result[body:body + len(issuer)] = issuer
    return bytes(result)


class MemoryReader:
    def __init__(self, data):
        self.data = data
        self.calls = []

    def size(self):
        self.calls.append(("size",))
        return len(self.data)

    def read(self, offset, count):
        self.calls.append(("read", offset, count))
        return self.data[offset:offset + count]


class ScriptedReader(MemoryReader):
    def __init__(self, data=b"", *, sizes=None, read_result=None):
        super().__init__(data)
        self.sizes = [len(data), len(data)] if sizes is None else list(sizes)
        self.read_result = data if read_result is None else read_result

    def size(self):
        self.calls.append(("size",))
        value = self.sizes.pop(0)
        if isinstance(value, Exception):
            raise value
        return value

    def read(self, offset, count):
        self.calls.append(("read", offset, count))
        if isinstance(self.read_result, Exception):
            raise self.read_result
        return self.read_result


class AuxiliaryTests(unittest.TestCase):
    def classify(self, data, hint=aux.Hint.NONE, budget=None):
        return aux.classify(MemoryReader(data), aux.Budget() if budget is None else budget, hint)

    def test_all_six_literal_layouts_remain_untrusted(self):
        for signature, _, size in LAYOUTS:
            with self.subTest(signature=signature):
                reader = MemoryReader(synthetic_ticket(signature))
                result = aux.classify(reader, aux.Budget(), aux.Hint.TICKET)
                self.assertEqual(result.classification, aux.Classification.TICKET)
                safe = json.loads(result.to_json())
                self.assertEqual(safe["authentication"], "not_verified")
                self.assertIs(safe["runtime_admissible"], False)
                self.assertEqual(safe["read_bytes"], size)
                self.assertEqual(reader.calls, [("size",), ("read", 0, size), ("size",)])

    def test_extension_hint_alone_cannot_recognize(self):
        result = self.classify(b"Not a Ticket", aux.Hint.TICKET)
        self.assertEqual(result.classification, aux.Classification.UNKNOWN)

    def test_certificate_remains_explicitly_unsupported(self):
        result = self.classify(b"Synthetic unsupported certificate", aux.Hint.CERTIFICATE)
        self.assertEqual(result.code, aux.Code.CERTIFICATE_UNSUPPORTED)
        self.assertEqual(result.classification, aux.Classification.UNSUPPORTED)

    def test_conflicting_hint_rejects_recognized_ticket(self):
        for hint in (aux.Hint.CERTIFICATE, aux.Hint.OTHER):
            with self.subTest(hint=hint):
                self.assertEqual(self.classify(synthetic_ticket(), hint).code, aux.Code.HINT_CONFLICT)

    def test_nca_sentinels_cannot_be_treated_as_auxiliary(self):
        for offset in (0, 0x200):
            with self.subTest(offset=offset):
                data = bytearray(synthetic_ticket())
                data[offset:offset + 4] = b"NCA3"
                result = self.classify(bytes(data), aux.Hint.TICKET)
                self.assertEqual(result.code, aux.Code.NCA_VERIFIER_REQUIRED)
                self.assertEqual(result.routing, aux.Routing.NCA)

    def test_nca_sized_member_rejected_without_read(self):
        reader = MemoryReader(b"\xA5" * 0xC00)
        result = aux.classify(reader, aux.Budget(), aux.Hint.TICKET)
        self.assertEqual(result.code, aux.Code.MEMBER_SIZE_LIMIT)
        self.assertEqual(reader.calls, [("size",)])
        self.assertIsNone(result.declared_size)

    def test_exact_extent_rejects_truncation_and_trailer(self):
        data = synthetic_ticket()
        for changed in (data[:-1], data + b"\0"):
            with self.subTest(size=len(changed)):
                self.assertEqual(self.classify(changed).code, aux.Code.FIXED_EXTENT_MISMATCH)

    def test_personalized_profile_is_not_supported(self):
        data = bytearray(synthetic_ticket())
        data[0x140 + 0x141] = 1
        self.assertEqual(self.classify(bytes(data)).code, aux.Code.PERSONAL_UNSUPPORTED)

    def test_opaque_bit_changes_cannot_be_authenticated(self):
        for offset in (4, 0x140 + 0x40, 0x140 + 0x160):
            with self.subTest(offset=offset):
                data = bytearray(synthetic_ticket())
                data[offset] ^= 0x80
                safe = self.classify(bytes(data)).to_safe_dict()
                self.assertEqual(safe["classification"], "recognized_untrusted_ticket_structure")
                self.assertEqual(safe["authentication"], "not_verified")
                self.assertIs(safe["runtime_admissible"], False)

    def test_shared_budget_charges_unknown_members(self):
        budget = aux.Budget(aux.Limits(max_members=1))
        self.assertEqual(self.classify(b"unknown", budget=budget).code, aux.Code.UNKNOWN_STRUCTURE)
        reader = MemoryReader(synthetic_ticket())
        self.assertEqual(aux.classify(reader, budget).code, aux.Code.MEMBER_COUNT_LIMIT)
        self.assertEqual(reader.calls, [])
        self.assertEqual(budget.snapshot()["member_attempts"], 1)

    def test_short_read_is_not_retried_or_size_rechecked(self):
        data = synthetic_ticket()
        reader = ScriptedReader(data, read_result=data[:-1])
        budget = aux.Budget()
        result = aux.classify(reader, budget)
        self.assertEqual(result.code, aux.Code.SHORT_READ)
        self.assertEqual(result.read_bytes, len(data) - 1)
        self.assertFalse(result.read_complete)
        self.assertEqual(reader.calls, [("size",), ("read", 0, len(data))])
        self.assertEqual(budget.snapshot(), {"member_attempts": 1, "read_attempts": 1,
                                           "reserved_bytes": len(data), "halt_code": None})

    def test_oversized_return_has_no_unbounded_numeric_field(self):
        data = synthetic_ticket()
        reader = ScriptedReader(data, read_result=data + b"X")
        result = aux.classify(reader, aux.Budget())
        self.assertEqual(result.code, aux.Code.INVALID_READ)
        self.assertIsNone(result.read_bytes)
        self.assertEqual(len(reader.calls), 2)

    def test_nonbyte_read_results_are_rejected_without_formatting(self):
        class Unprintable:
            def __repr__(self):
                raise AssertionError("must not format a returned object")

        for value in (bytearray(4), memoryview(b"test"), "test", [0, 1], 12, Unprintable()):
            with self.subTest(kind=type(value).__name__):
                reader = ScriptedReader(b"test", read_result=value)
                self.assertEqual(aux.classify(reader, aux.Budget()).code, aux.Code.INVALID_READ)
                self.assertEqual(reader.calls, [("size",), ("read", 0, 4)])

    def test_invalid_initial_size_never_reads(self):
        for size in (-1, True, False, 1.0, "PRIVATE-SIZE", None):
            with self.subTest(kind=type(size).__name__):
                reader = ScriptedReader(sizes=[size])
                result = aux.classify(reader, aux.Budget())
                self.assertEqual(result.code, aux.Code.INVALID_SIZE)
                self.assertIsNone(result.declared_size)
                self.assertEqual(reader.calls, [("size",)])

    def test_huge_size_is_not_echoed_or_read(self):
        reader = ScriptedReader(sizes=[10 ** 200])
        result = aux.classify(reader, aux.Budget())
        self.assertEqual(result.code, aux.Code.MEMBER_SIZE_LIMIT)
        self.assertIsNone(result.to_safe_dict()["declared_size"])
        self.assertEqual(reader.calls, [("size",)])

    def test_changed_size_rejects_after_one_exact_read(self):
        data = synthetic_ticket()
        for after in (len(data) - 1, len(data) + 1, 10 ** 200):
            with self.subTest(after_is_huge=after > 1000):
                reader = ScriptedReader(data, sizes=[len(data), after])
                result = aux.classify(reader, aux.Budget())
                self.assertEqual(result.code, aux.Code.INPUT_CHANGED)
                self.assertFalse(result.read_complete)
                self.assertEqual(result.declared_size, len(data))
                self.assertEqual(reader.calls, [("size",), ("read", 0, len(data)), ("size",)])

    def test_invalid_second_size_does_not_escape(self):
        data = synthetic_ticket()
        for after in (-1, True, "PRIVATE-SIZE", None):
            with self.subTest(kind=type(after).__name__):
                reader = ScriptedReader(data, sizes=[len(data), after])
                self.assertEqual(aux.classify(reader, aux.Budget()).code, aux.Code.INVALID_SIZE)
                self.assertEqual(len(reader.calls), 3)

    def test_exception_at_each_callback_is_fixed_and_never_retried(self):
        data = synthetic_ticket()
        canary = "PRIVATE-EXCEPTION /private/member.tik secret-body"
        cases = ((ScriptedReader(data, sizes=[RuntimeError(canary)]), 1, 0, 0),
                 (ScriptedReader(data, read_result=RuntimeError(canary)), 2, 1, len(data)),
                 (ScriptedReader(data, sizes=[len(data), RuntimeError(canary)]), 3, 1, len(data)))
        for reader, calls, reads, reserved in cases:
            with self.subTest(calls=calls):
                budget = aux.Budget()
                result = aux.classify(reader, budget)
                self.assertEqual(result.code, aux.Code.READER_FAILURE)
                self.assertNotIn(canary, result.to_json())
                self.assertEqual(len(reader.calls), calls)
                self.assertEqual(budget.snapshot()["member_attempts"], 1)
                self.assertEqual(budget.snapshot()["read_attempts"], reads)
                self.assertEqual(budget.snapshot()["reserved_bytes"], reserved)

    def test_missing_reader_methods_yield_fixed_failure(self):
        result = aux.classify(object(), aux.Budget())
        self.assertEqual(result.code, aux.Code.READER_FAILURE)

    def test_all_hard_limits_cannot_be_raised(self):
        maxima = {"max_member_bytes": 0x3C0, "max_members": 32,
                  "max_total_bytes": 32768, "max_read_calls": 32}
        for name, maximum in maxima.items():
            with self.subTest(name=name):
                with self.assertRaisesRegex(aux.ConfigurationError, "^invalid_limits$"):
                    aux.Limits(**{name: maximum + 1})

    def test_invalid_limits_do_not_format_supplied_values(self):
        for name in ("max_member_bytes", "max_members", "max_total_bytes", "max_read_calls"):
            for value in (-1, True, 1.0, "PRIVATE-LIMIT", None):
                with self.subTest(name=name, kind=type(value).__name__):
                    with self.assertRaisesRegex(aux.ConfigurationError, "^invalid_limits$"):
                        aux.Limits(**{name: value})

    def test_budget_revalidates_supplied_limits(self):
        limits = aux.Limits()
        object.__setattr__(limits, "max_members", 33)
        with self.assertRaisesRegex(aux.ConfigurationError, "^invalid_limits$"):
            aux.Budget(limits)
        with self.assertRaisesRegex(aux.ConfigurationError, "^invalid_limits$"):
            aux.Budget({"max_members": 33})

    def test_lowered_member_limit_rejects_before_read(self):
        reader = MemoryReader(synthetic_ticket())
        budget = aux.Budget(aux.Limits(max_member_bytes=511))
        self.assertEqual(aux.classify(reader, budget).code, aux.Code.MEMBER_SIZE_LIMIT)
        self.assertEqual(reader.calls, [("size",)])
        self.assertEqual(budget.snapshot()["read_attempts"], 0)

    def test_zero_member_budget_has_no_reader_callbacks(self):
        reader = MemoryReader(synthetic_ticket())
        result = aux.classify(reader, aux.Budget(aux.Limits(max_members=0)))
        self.assertEqual(result.code, aux.Code.MEMBER_COUNT_LIMIT)
        self.assertIsNone(result.member_ordinal)
        self.assertEqual(reader.calls, [])

    def test_hard_member_limit_bounds_callbacks_and_all_ordinals(self):
        budget = aux.Budget()
        for ordinal in range(32):
            reader = MemoryReader(b"unknown")
            result = aux.classify(reader, budget)
            self.assertEqual(result.member_ordinal, ordinal)
            self.assertEqual(len(reader.calls), 3)
        for _ in range(20):
            reader = MemoryReader(b"unknown")
            result = aux.classify(reader, budget)
            self.assertEqual(result.code, aux.Code.MEMBER_COUNT_LIMIT)
            self.assertIsNone(result.member_ordinal)
            self.assertEqual(reader.calls, [])
        self.assertEqual(budget.snapshot()["member_attempts"], 32)
        self.assertEqual(budget.snapshot()["read_attempts"], 32)

    def test_aggregate_boundary_is_precharged_and_sticky(self):
        data = synthetic_ticket()
        budget = aux.Budget(aux.Limits(max_total_bytes=len(data)))
        self.assertEqual(self.classify(data, budget=budget).code, aux.Code.TICKET_STRUCTURE)
        reader = MemoryReader(data)
        self.assertEqual(aux.classify(reader, budget).code, aux.Code.AGGREGATE_BYTE_LIMIT)
        self.assertEqual(reader.calls, [("size",)])
        again = MemoryReader(data)
        self.assertEqual(aux.classify(again, budget).code, aux.Code.AGGREGATE_BYTE_LIMIT)
        self.assertEqual(again.calls, [])
        self.assertEqual(budget.snapshot(), {"member_attempts": 2, "read_attempts": 1,
                                           "reserved_bytes": len(data), "halt_code": "aggregate_byte_limit"})

    def test_read_call_boundary_is_precharged_and_sticky(self):
        budget = aux.Budget(aux.Limits(max_read_calls=1))
        self.classify(b"unknown", budget=budget)
        reader = MemoryReader(b"unknown")
        self.assertEqual(aux.classify(reader, budget).code, aux.Code.READ_COUNT_LIMIT)
        self.assertEqual(reader.calls, [("size",)])
        again = MemoryReader(b"unknown")
        self.assertEqual(aux.classify(again, budget).code, aux.Code.READ_COUNT_LIMIT)
        self.assertEqual(again.calls, [])
        self.assertEqual(budget.snapshot()["read_attempts"], 1)

    def test_failed_read_does_not_refund_aggregate_or_read_budget(self):
        data = synthetic_ticket()
        budget = aux.Budget(aux.Limits(max_total_bytes=len(data)))
        failure = ScriptedReader(data, read_result=RuntimeError("PRIVATE"))
        self.assertEqual(aux.classify(failure, budget).code, aux.Code.READER_FAILURE)
        reader = MemoryReader(data)
        self.assertEqual(aux.classify(reader, budget).code, aux.Code.AGGREGATE_BYTE_LIMIT)
        self.assertEqual(reader.calls, [("size",)])
        self.assertEqual(budget.snapshot()["read_attempts"], 1)
        self.assertEqual(budget.snapshot()["reserved_bytes"], len(data))

    def test_zero_read_budget_never_invokes_read(self):
        reader = MemoryReader(b"unknown")
        budget = aux.Budget(aux.Limits(max_read_calls=0))
        self.assertEqual(aux.classify(reader, budget).code, aux.Code.READ_COUNT_LIMIT)
        self.assertEqual(reader.calls, [("size",)])

    def test_invalid_hint_is_charged_without_echo_or_reader_access(self):
        budget = aux.Budget(aux.Limits(max_members=1))
        reader = MemoryReader(synthetic_ticket())
        result = aux.classify(reader, budget, "/PRIVATE/member.tik")
        self.assertEqual(result.code, aux.Code.INVALID_HINT)
        self.assertEqual(result.hint, aux.Hint.NONE)
        self.assertNotIn("PRIVATE", result.to_json())
        self.assertEqual(reader.calls, [])
        self.assertEqual(budget.snapshot()["member_attempts"], 1)
        self.assertEqual(aux.classify(reader, budget).code, aux.Code.MEMBER_COUNT_LIMIT)

    def test_invalid_budget_cannot_invoke_reader(self):
        reader = MemoryReader(synthetic_ticket())
        for budget in (None, {}, aux.Limits()):
            with self.subTest(kind=type(budget).__name__):
                result = aux.classify(reader, budget)
                self.assertEqual(result.code, aux.Code.INVALID_BUDGET)
                self.assertIs(result.to_safe_dict()["runtime_admissible"], False)
        self.assertEqual(reader.calls, [])

    def test_public_budget_views_cannot_reset_charges_or_raise_caps(self):
        budget = aux.Budget(aux.Limits(max_members=1))
        self.classify(b"unknown", budget=budget)
        snapshot = budget.snapshot()
        snapshot["member_attempts"] = 0
        limits = budget.limits
        with self.assertRaises(FrozenInstanceError):
            limits.max_members = 32
        # Even deliberate modification of the returned copy cannot mutate the
        # original. Python arbitrary private-state writes are outside this API.
        object.__setattr__(limits, "max_members", 32)
        with self.assertRaises(AttributeError):
            budget.member_attempts = 0
        reader = MemoryReader(b"unknown")
        self.assertEqual(aux.classify(reader, budget).code, aux.Code.MEMBER_COUNT_LIMIT)
        self.assertEqual(reader.calls, [])

    def test_result_fields_enforce_fixed_enums_and_numeric_bounds(self):
        invalid = ({"code": "PRIVATE"}, {"hint": "PRIVATE"}, {"profile": "PRIVATE"},
                   {"member_ordinal": 32}, {"member_ordinal": -1}, {"declared_size": 961},
                   {"read_bytes": 10 ** 200}, {"read_complete": True})
        for changes in invalid:
            with self.subTest(field=next(iter(changes))):
                fields = {"classification": aux.Classification.REJECTED,
                          "code": aux.Code.UNKNOWN_STRUCTURE, "hint": aux.Hint.NONE}
                fields.update(changes)
                with self.assertRaisesRegex(aux.ConfigurationError, "^invalid_result$"):
                    aux.Result(**fields)

    def test_issuer_policy_rejects_structural_damage(self):
        invalid = (bytes(64), b"X" * 64, b"bad\x1Fissuer\0" + bytes(53),
                   b"bad\x7Fissuer\0" + bytes(53), b"ok\0hidden" + bytes(55))
        for issuer in invalid:
            with self.subTest(prefix=issuer[:3].hex()):
                self.assertEqual(len(issuer), 64)
                data = bytearray(synthetic_ticket())
                data[0x140:0x180] = issuer
                self.assertEqual(self.classify(bytes(data)).code, aux.Code.ISSUER_PROFILE_INVALID)

    def test_issuer_63_byte_boundary_is_supported_only_as_local_policy(self):
        data = bytearray(synthetic_ticket())
        data[0x140:0x180] = b"X" * 63 + b"\0"
        self.assertEqual(self.classify(bytes(data)).code, aux.Code.TICKET_STRUCTURE)

    def test_unknown_title_key_type_rejects(self):
        for value in (2, 255):
            with self.subTest(value=value):
                data = bytearray(synthetic_ticket())
                data[0x140 + 0x141] = value
                self.assertEqual(self.classify(bytes(data)).code, aux.Code.INVALID_TITLE_KEY_TYPE)

    def test_unknown_and_big_endian_signature_declarations_are_unknown(self):
        for prefix in (b"\0" * 4, b"\xFF" * 4, bytes.fromhex("00010000")):
            with self.subTest(prefix=prefix.hex()):
                data = bytearray(synthetic_ticket())
                data[:4] = prefix
                self.assertEqual(self.classify(bytes(data)).classification, aux.Classification.UNKNOWN)

    def test_all_nca_marker_versions_are_conservatively_routed(self):
        for marker in (b"NCA0", b"NCA1", b"NCA2", b"NCA3"):
            for offset in (0, 0x200):
                with self.subTest(marker=marker.decode("ascii"), offset=offset):
                    data = bytearray(synthetic_ticket())
                    data[offset:offset + 4] = marker
                    result = self.classify(bytes(data), aux.Hint.CERTIFICATE)
                    self.assertEqual(result.code, aux.Code.NCA_VERIFIER_REQUIRED)
                    self.assertEqual(result.routing, aux.Routing.NCA)

    def test_nca_hint_routes_without_opening_source(self):
        reader = MemoryReader(synthetic_ticket())
        result = aux.classify(reader, aux.Budget(), aux.Hint.NCA)
        self.assertEqual(result.routing, aux.Routing.NCA)
        self.assertEqual(reader.calls, [])

    def test_empty_member_has_bounded_zero_byte_read_and_stays_unknown(self):
        reader = MemoryReader(b"")
        result = aux.classify(reader, aux.Budget())
        self.assertEqual(result.classification, aux.Classification.UNKNOWN)
        self.assertEqual(result.declared_size, 0)
        self.assertEqual(result.read_bytes, 0)
        self.assertTrue(result.read_complete)
        self.assertEqual(reader.calls, [("size",), ("read", 0, 0), ("size",)])

    def test_safe_report_does_not_contain_opaque_body_issuer_or_exception(self):
        data = bytearray(synthetic_ticket())
        issuer = b"PRIVATE-ISSUER-CANARY"
        signature = b"PRIVATE-SIGNATURE-CANARY"
        keyblock = b"PRIVATE-OPAQUE-KEY-CANARY"
        rights = b"PRIVATE-RIGHTS"
        data[0x140:0x180] = issuer + bytes(64 - len(issuer))
        data[4:4 + len(signature)] = signature
        data[0x180:0x180 + len(keyblock)] = keyblock
        data[0x2A0:0x2A0 + len(rights)] = rights
        result = self.classify(bytes(data), aux.Hint.TICKET)
        self.assertEqual(result.code, aux.Code.TICKET_STRUCTURE)
        text = result.to_json()
        for canary in (issuer, signature, keyblock, rights):
            self.assertNotIn(canary.decode("ascii"), text)
        self.assertNotIn("PRIVATE", text)
        self.assertNotIn("hash", text)
        self.assertLessEqual(len(text.encode("ascii")), 1024)
        self.assertEqual(set(json.loads(text)), {"schema_version", "classification", "code", "hint", "profile",
                                               "routing", "authentication", "runtime_admissible", "member_ordinal",
                                               "declared_size", "read_bytes", "read_complete"})

    def test_safe_report_only_contains_bounded_numbers(self):
        budget = aux.Budget()
        for ordinal in range(32):
            result = self.classify(synthetic_ticket(0x010000), budget=budget)
            safe = result.to_safe_dict()
            self.assertEqual(safe["member_ordinal"], ordinal)
            self.assertLessEqual(safe["declared_size"], 960)
            self.assertLessEqual(safe["read_bytes"], 960)
            self.assertLessEqual(len(result.to_json()), 1024)
        self.assertEqual(budget.snapshot()["reserved_bytes"], 30720)
        self.assertLessEqual(budget.snapshot()["reserved_bytes"], 32768)

    def test_no_key_matching_import_or_runtime_admission_input_exists(self):
        self.assertEqual(tuple(inspect.signature(aux.classify).parameters), ("reader", "budget", "hint"))
        for data, hint in ((synthetic_ticket(), aux.Hint.TICKET), (b"unknown", aux.Hint.TICKET),
                           (b"certificate", aux.Hint.CERTIFICATE), (b"NCA3", aux.Hint.TICKET)):
            with self.subTest(hint=hint, size=len(data)):
                safe = self.classify(data, hint).to_safe_dict()
                self.assertIs(safe["runtime_admissible"], False)
                self.assertEqual(safe["authentication"], "not_verified")

    def test_size_recheck_does_not_claim_snapshot_identity(self):
        data = synthetic_ticket()

        class ChangedAfterRead(MemoryReader):
            def read(self, offset, count):
                observed = super().read(offset, count)
                self.data = b"X" * len(self.data)
                return observed

        reader = ChangedAfterRead(data)
        safe = aux.classify(reader, aux.Budget()).to_safe_dict()
        # A same-size mutation is intentionally not authenticated by this
        # one-read experiment. No result is a runtime or identity permission.
        self.assertEqual(safe["classification"], "recognized_untrusted_ticket_structure")
        self.assertEqual(safe["authentication"], "not_verified")
        self.assertIs(safe["runtime_admissible"], False)


if __name__ == "__main__":
    unittest.main()
