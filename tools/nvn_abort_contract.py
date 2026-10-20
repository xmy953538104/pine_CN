#!/usr/bin/env python3
"""Offline algebra for supplied registers; never a Pine runtime observer.

Basis: owner addresses and the parent's independently read handoff summary.
The original archive/binary/window was not obtained in this environment.
There is deliberately no runtime admission override, memory reader or hook.
"""
import json
import re
import sys

MAX_INPUT_BYTES = 16384
MASK64 = (1 << 64) - 1
TARGET_VA = 0x2D64E0
INSTRUCTION_LE = "c04c0494"
RAW_KEYS = ("pc", "lr", "sp", "x0", "x8", "x9", "x10", "x19", "x20")
CONTEXT_KEYS = ("session_id", "launch_id", "process_run_id", "guest_thread_id")
STOP_CODES = (
    "STOP_RUNTIME_NOT_CONNECTED",
    "STOP_MODULE_IDENTITY_UNVERIFIED",
    "STOP_WINDOW_SPEC_AND_EVIDENCE_MISSING",
    "STOP_NO_SAFE_EXACT_PC_OBSERVER",
)


class InputError(ValueError):
    pass


def _object(value, allowed, reason):
    if not isinstance(value, dict) or not value.keys() <= set(allowed):
        raise InputError(reason)
    return value


def _hex64(value):
    if value is None:
        return None
    if not isinstance(value, str) or re.fullmatch(r"0x[0-9a-f]{16}", value) is None:
        raise InputError("uint64_requires_exact_lowercase_16_digit_hex_string")
    return int(value[2:], 16)


def _hex(value):
    return f"0x{value:016x}"


def _signed(value):
    return value if value < 1 << 63 else value - (1 << 64)


def _add_address(base, offset):
    if base is None or base > MASK64 - offset:
        return None
    return base + offset


def decode(document):
    """Return a candidate from one supplied object, never verified capture."""
    _object(document, ("module", "registers", "context", "instruction_le"), "invalid_top_level")
    module = _object(document.get("module", {}),
                     ("name", "build_id", "image_base", "text_base", "text_size"), "invalid_module")
    supplied = _object(document.get("registers", {}), RAW_KEYS, "invalid_registers")
    context = _object(document.get("context", {}), CONTEXT_KEYS, "invalid_context")
    name = module.get("name")
    if name is not None and (not isinstance(name, str) or re.fullmatch(r"[A-Za-z0-9_.-]{1,32}", name) is None):
        raise InputError("invalid_module_name")
    build_id = module.get("build_id")
    if build_id is not None and (not isinstance(build_id, str) or re.fullmatch(r"[0-9a-f]{64}", build_id) is None):
        raise InputError("invalid_build_id")
    for value in context.values():
        if value is not None and (not isinstance(value, str) or re.fullmatch(r"[A-Za-z0-9_.:-]{1,128}", value) is None):
            raise InputError("invalid_context_identity")
    instruction = document.get("instruction_le")
    if instruction is not None and (not isinstance(instruction, str) or re.fullmatch(r"[0-9a-f]{8}", instruction) is None):
        raise InputError("invalid_instruction_claim")
    values = {key: _hex64(supplied.get(key)) for key in RAW_KEYS}
    base, text, size = (_hex64(module.get(key)) for key in ("image_base", "text_base", "text_size"))
    candidate, reason, algebra = "UNCLASSIFIED", "module_or_address_unavailable", None
    target = _add_address(base, TARGET_VA)
    end = _add_address(text, size) if size is not None else None
    no_callback = _add_address(base, 0x7A0371)
    insufficient = _add_address(base, 0x7A032A)
    if name != "sdk.nso":
        reason = "module_name_mismatch_or_unavailable"
    elif base is None or text is None or size is None:
        pass
    elif text != base:
        reason = "supplied_text_va_zero_contract_not_met"
    elif target is None or end is None or no_callback is None or insufficient is None:
        reason = "address_overflow"
    elif not text <= target < end or end - target < 4:
        reason = "target_not_wholly_in_supplied_text_range"
    elif values["pc"] != target:
        reason = "pc_mismatch_or_unavailable"
    elif instruction != INSTRUCTION_LE:
        reason = "instruction_claim_mismatch_or_unavailable"
    elif values["x0"] == no_callback:
        if values["x8"] == 0:
            candidate, reason = "NO_CALLBACK", "supplied_x0_and_zero_x8_match_contract"
        else:
            reason = "no_callback_x8_nonzero_or_unavailable"
    elif values["x0"] == insufficient:
        if any(values[key] is None for key in ("x8", "x9", "x10", "x19")):
            reason = "comparison_or_derivation_register_unavailable"
        elif values["x10"] >> 32:
            reason = "x10_upper32_not_zero"
        elif _signed(values["x8"]) >= _signed(values["x19"]):
            reason = "signed_comparison_not_insufficient"
        else:
            candidate, reason = "INSUFFICIENT_AFTER_RETURN", "supplied_signed_comparison_matches_contract"
            algebra = {"p18_mod64": _hex((values["x9"] - values["x8"]) & MASK64),
                       "p20_mod64": _hex((values["x9"] - 4 * values["x10"]) & MASK64),
                       "r70_u32_hex": f"0x{values['x10']:08x}",
                       "scope": "algebra_of_one_supplied_snapshot_not_object_memory_or_callback_observation"}
    else:
        reason = "x0_branch_marker_mismatch_or_unavailable"
    return {
        "schema": 1,
        "record_kind": "offline_nvn_abort_contract_decode",
        "runtime_connected": False,
        "runtime_admission": "STOP",
        "stop_codes": list(STOP_CODES),
        "input_origin": "external_supplied_values_not_captured_by_pine",
        "contract_basis": "owner_addresses_and_parent_review_summary_not_original_archive_or_binary_verification",
        "classification_scope": "offline_contract_candidate_only_not_verified_control_flow",
        "contract_candidate": candidate,
        "reason": reason,
        "module_claims": {key: module.get(key) for key in ("name", "build_id", "image_base", "text_base", "text_size")},
        "module_verification": "unverified_no_runtime_mapping_or_window_evidence",
        "instruction_claim_le": instruction,
        "context_claims": {key: context.get(key) for key in CONTEXT_KEYS},
        "unavailable_context": [key for key in CONTEXT_KEYS if context.get(key) is None],
        "raw_registers": {key: _hex(value) if value is not None else None for key, value in values.items()},
        "unavailable_registers": [key for key, value in values.items() if value is None],
        "x9_x10_interpretation": "prohibited_on_no_callback_path" if candidate == "NO_CALLBACK" else "contract_algebra_only",
        "derived_algebra": algebra,
        "export_scope": "offline_json_only_not_android_diagnostics_or_resource_window",
    }


def _pairs(items):
    result = {}
    for key, value in items:
        if key in result:
            raise InputError("duplicate_object_key")
        result[key] = value
    return result


def parse(data):
    if not isinstance(data, bytes) or len(data) > MAX_INPUT_BYTES:
        raise InputError("input_byte_limit")
    try:
        document = json.loads(data.decode("utf-8"), object_pairs_hook=_pairs,
                              parse_constant=lambda _: (_ for _ in ()).throw(InputError("nonfinite_json_number")))
    except InputError:
        raise
    except (UnicodeError, ValueError, RecursionError):
        raise InputError("invalid_utf8_or_json") from None
    return decode(document)


def main():
    try:
        if len(sys.argv) != 1:
            raise InputError("unsupported_cli_arguments")
        result = parse(sys.stdin.buffer.read(MAX_INPUT_BYTES + 1))
    except InputError as error:
        print(json.dumps({"runtime_admission": "STOP", "error": "STOP_INPUT_INVALID", "reason": str(error)}), file=sys.stderr)
        return 2
    print(json.dumps(result, sort_keys=True, ensure_ascii=True))
    return 0  # Successful offline decoding, never successful runtime admission.


if __name__ == "__main__":
    sys.exit(main())
