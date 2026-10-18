#!/usr/bin/env python3
"""Bounded, offline checks of explicitly supplied dedicated SaveData probe evidence."""

import argparse
import binascii
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import struct
import sys
import unicodedata
import zipfile
import zlib


MANIFEST_SCHEMA = "pine.save-probe.manifest.v1"
REPORT_SCHEMA = "pine.save-probe.report.v1"
CODEC = "pine-probe-json-utf8-v1"
KEY = "dfx_diag_probe_v1"
BASENAME = KEY + ".json"
TEXT = "pine dedicated save probe v1"
MAX_MANIFEST = 64 * 1024
MAX_PAYLOAD = 1024
MAX_ZIP = 32 * 1024 * 1024
MAX_CENTRAL_DIRECTORY = 1024 * 1024
MAX_ENTRIES = 2048
MAX_COMPRESSED = 4096
IDENTITY_FIELDS = ("space", "type", "user_id", "save_owner_id", "program_id", "path")
CAPTURE_FIELDS = ("identity", "write_id", "capture_id", "process_id", "source",
                  "api_signature_verified", "previous_process_stopped")
BAD = frozenset(("contradiction", "rejected"))
INCOMPLETE = frozenset(("missing_evidence", "unknown", "unsupported"))


class ProbeError(Exception):
    """Contains a fixed reason code, never private input or an OS error string."""


def check(state, reason):
    return {"state": state, "reason": reason}


def fail(reason):
    raise ProbeError(reason)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def object_without_duplicates(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            fail("duplicate_json_key")
        result[key] = value
    return result


def parse_json(data):
    try:
        return json.loads(data.decode("utf-8", errors="strict"),
                          object_pairs_hook=object_without_duplicates,
                          parse_constant=lambda _: fail("non_finite_json_number"))
    except (UnicodeError, ValueError, RecursionError):
        fail("invalid_utf8_json")


def shape(value, allowed, required=()):
    if not isinstance(value, dict) or set(value) - set(allowed) or set(required) - set(value):
        fail("invalid_object_fields")


def token(value):
    return isinstance(value, str) and re.fullmatch(r"[A-Za-z0-9_.-]{1,64}", value) is not None


def hex_string(value, length):
    return isinstance(value, str) and re.fullmatch("[0-9a-f]{%d}" % length, value) is not None


def identity_hex(value, length):
    return isinstance(value, str) and re.fullmatch("[0-9A-Fa-f]{%d}" % length, value) is not None


def safe_relative_name(name, directory=False):
    if not isinstance(name, str) or not name:
        fail("unsafe_relative_name")
    try:
        if len(name.encode("utf-8", errors="strict")) > 512:
            fail("unsafe_relative_name")
    except UnicodeError:
        fail("unsafe_relative_name")
    if "\\" in name or ":" in name or name.startswith("/") or any(ord(c) < 32 or ord(c) == 127 for c in name):
        fail("unsafe_relative_name")
    stripped = name[:-1] if directory and name.endswith("/") else name
    parts = stripped.split("/")
    if len(parts) > 16 or any(part in ("", ".", "..") for part in parts):
        fail("unsafe_relative_name")
    return stripped


def validate_identity(identity):
    shape(identity, IDENTITY_FIELDS)
    for key, value in identity.items():
        if value is None:
            continue
        if key == "space" and value not in ("system", "user", "temporary", "unknown"):
            fail("invalid_identity_space")
        if key == "type" and value not in ("system", "account", "device", "temporary", "cache", "unknown"):
            fail("invalid_identity_type")
        if key in ("user_id", "save_owner_id", "program_id") and not identity_hex(value, 32 if key == "user_id" else 16):
            fail("invalid_identity_id")
        if key == "path":
            safe_relative_name(value)
            if value.split("/")[-1] != BASENAME:
                fail("path_is_not_dedicated_probe")


def validate_manifest(manifest):
    shape(manifest, ("schema", "authorization", "codec", "expected", "observations", "zip_member", "evidence_origin"),
          ("schema", "authorization"))
    if manifest["schema"] != MANIFEST_SCHEMA:
        fail("unsupported_manifest_schema")
    if manifest["authorization"] != {"scope": "dedicated_probe_only", "logical_key": KEY, "basename": BASENAME}:
        fail("dedicated_probe_authorization_required")
    if "evidence_origin" in manifest and manifest["evidence_origin"] not in ("synthetic", "owner_local"):
        fail("invalid_evidence_origin")
    if "codec" in manifest and (not isinstance(manifest["codec"], str) or not token(manifest["codec"])):
        fail("invalid_codec_identifier")
    expected = manifest.get("expected", {})
    shape(expected, ("identity", "write_id", "payload"))
    validate_identity(expected.get("identity", {}))
    if "write_id" in expected and not hex_string(expected["write_id"], 32):
        fail("invalid_write_id")
    payload = expected.get("payload", {})
    shape(payload, ("nonce", "length", "sha256"))
    if "nonce" in payload and not hex_string(payload["nonce"], 32):
        fail("invalid_expected_nonce")
    if "sha256" in payload and not hex_string(payload["sha256"], 64):
        fail("invalid_expected_sha256")
    if "length" in payload and (type(payload["length"]) is not int or not 1 <= payload["length"] <= MAX_PAYLOAD):
        fail("invalid_expected_length")
    if "zip_member" in manifest:
        member = safe_relative_name(manifest["zip_member"])
        if member.split("/")[-1] != BASENAME:
            fail("zip_member_is_not_dedicated_probe")
    observations = manifest.get("observations", {})
    shape(observations, ("writer", "initial", "restart", "export"))
    for stage, record in observations.items():
        if stage == "writer":
            shape(record, ("identity", "write_id", "callback", "writer", "file_closed"))
            for field, choices in (("callback", ("success", "failure", "unknown")),
                                   ("writer", ("completed", "failed", "unknown"))):
                if field in record and record[field] not in choices:
                    fail("invalid_writer_observation")
            if "file_closed" in record and type(record["file_closed"]) is not bool:
                fail("invalid_close_observation")
        elif stage == "export":
            shape(record, ("identity", "write_id", "consistency", "source"))
            if "consistency" in record and record["consistency"] not in ("stopped", "coordinated", "uncoordinated", "unknown"):
                fail("invalid_export_consistency")
            if "source" in record and record["source"] not in ("save_management_utils_v79a", "explicit_dedicated_probe_archive", "unknown"):
                fail("invalid_export_source")
        else:
            shape(record, CAPTURE_FIELDS)
            for field in ("capture_id", "process_id", "source"):
                if field in record and not token(record[field]):
                    fail("invalid_capture_token")
            for field in ("api_signature_verified", "previous_process_stopped"):
                if field in record and type(record[field]) is not bool:
                    fail("invalid_restart_observation")
        validate_identity(record.get("identity", {}))
        if "write_id" in record and not hex_string(record["write_id"], 32):
            fail("invalid_write_id")
    return manifest


def open_regular(path, limit, dedicated=False):
    """Open only this argument, with no directory enumeration or final symlink follow."""
    if dedicated and Path(path).name != BASENAME:
        fail("input_basename_is_not_dedicated_probe")
    if not hasattr(os, "O_NOFOLLOW"):
        fail("host_requires_nofollow_support")
    flags = os.O_RDONLY | os.O_NOFOLLOW | getattr(os, "O_NONBLOCK", 0)
    try:
        fd = os.open(path, flags)
        try:
            before = os.fstat(fd)
            if not stat.S_ISREG(before.st_mode):
                fail("input_is_not_regular_file")
            if before.st_size > limit:
                fail("input_byte_limit_exceeded")
            return os.fdopen(fd, "rb"), before
        except BaseException:
            os.close(fd)
            raise
    except OSError:
        fail("input_open_failed_or_symlink")


def unchanged(stream, before):
    after = os.fstat(stream.fileno())
    if (before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns, before.st_ctime_ns) != (
            after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns, after.st_ctime_ns):
        fail("input_changed_during_read")


def read_bounded(path, limit, dedicated=False):
    stream, before = open_regular(path, limit, dedicated)
    with stream:
        data = stream.read(limit + 1)
        if len(data) > limit:
            fail("actual_read_byte_limit_exceeded")
        if len(data) != before.st_size:
            fail("short_or_growing_input")
        unchanged(stream, before)
    return data, (before.st_dev, before.st_ino)


def read_exact(stream, size):
    data = stream.read(size)
    if len(data) != size:
        fail("short_zip_read")
    return data


def zip_directory_bounds(stream, size):
    # Reject ZIP64/multidisk and bound central-directory allocation before ZipFile parses it.
    start = max(0, size - 65557)
    stream.seek(start)
    tail = read_exact(stream, size - start)
    offset = tail.rfind(b"PK\x05\x06")
    if offset < 0 or len(tail) - offset < 22:
        fail("zip_end_record_missing")
    _, disk, directory_disk, disk_count, count, directory_size, directory_offset, comment_size = struct.unpack_from("<4s4H2IH", tail, offset)
    if offset + 22 + comment_size != len(tail):
        fail("zip_trailing_or_bad_end_record")
    if disk or directory_disk or disk_count != count:
        fail("zip_multidisk_unsupported")
    if count == 65535 or directory_size == 0xffffffff or directory_offset == 0xffffffff:
        fail("zip64_unsupported")
    if count > MAX_ENTRIES or directory_size > MAX_CENTRAL_DIRECTORY:
        fail("zip_directory_limit_exceeded")
    if directory_offset + directory_size != start + offset:
        fail("zip_directory_range_invalid")
    # zipfile also honors a ZIP64 locator with nonsentinel legacy values. Reject
    # that before its parser can replace our bounded directory size/count.
    end_offset = start + offset
    if end_offset >= 20:
        stream.seek(end_offset - 20)
        if read_exact(stream, 20).startswith(b"PK\x06\x07"):
            fail("zip64_unsupported")
    stream.seek(directory_offset)
    directory = read_exact(stream, directory_size)
    cursor, actual_count = 0, 0
    while cursor < len(directory):
        if len(directory) - cursor < 46 or directory[cursor:cursor + 4] != b"PK\x01\x02":
            fail("zip_directory_record_invalid")
        name_size, extra_size, comment_size = struct.unpack_from("<3H", directory, cursor + 28)
        cursor += 46 + name_size + extra_size + comment_size
        actual_count += 1
        if cursor > len(directory):
            fail("zip_directory_record_invalid")
        if actual_count > MAX_ENTRIES:
            fail("zip_directory_limit_exceeded")
    if actual_count != count:
        fail("zip_directory_count_mismatch")
    return count, directory_offset


def no_zip64_extra(extra):
    while extra:
        if len(extra) < 4:
            fail("malformed_zip_extra")
        tag, size = struct.unpack_from("<HH", extra)
        if size + 4 > len(extra):
            fail("malformed_zip_extra")
        if tag == 1:
            fail("zip64_unsupported")
        extra = extra[size + 4:]


def read_zip_member(path, member):
    """Read metadata and exactly one declared member; never extract or read other bodies."""
    stream, before = open_regular(path, MAX_ZIP)
    with stream:
        count, directory_offset = zip_directory_bounds(stream, before.st_size)
        with zipfile.ZipFile(stream) as archive:
            infos = archive.infolist()
            if len(infos) != count:
                fail("zip_directory_count_mismatch")
            seen, offsets = set(), set()
            selected = None
            for info in infos:
                name = safe_relative_name(info.orig_filename, info.is_dir())
                canonical = unicodedata.normalize("NFC", name).casefold()
                if canonical in seen:
                    fail("zip_duplicate_or_case_alias")
                seen.add(canonical)
                if info.header_offset in offsets or not 0 <= info.header_offset < directory_offset:
                    fail("zip_header_offset_invalid")
                offsets.add(info.header_offset)
                no_zip64_extra(info.extra)
                mode = stat.S_IFMT(info.external_attr >> 16)
                if mode not in (0, stat.S_IFREG, stat.S_IFDIR) or (mode != 0 and (mode == stat.S_IFDIR) != info.is_dir()):
                    fail("zip_symlink_or_special_entry")
                if info.flag_bits & (1 | 64):
                    fail("zip_encrypted_entry")
                if info.orig_filename == member:
                    selected = info
                elif canonical == unicodedata.normalize("NFC", member).casefold():
                    fail("zip_member_case_mismatch")
            if selected is None:
                fail("zip_declared_member_absent")
            info = selected
            if info.is_dir():
                fail("zip_declared_member_is_directory")
            if info.file_size > MAX_PAYLOAD or info.compress_size > MAX_COMPRESSED:
                fail("zip_member_byte_limit_exceeded")
            if info.file_size > max(1, info.compress_size) * 100:
                fail("zip_member_ratio_limit_exceeded")
            if info.compress_type not in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED):
                fail("zip_compression_unsupported")
            if info.flag_bits & ~(8 | 0x800 | 2 | 4):
                fail("zip_flags_unsupported")
            boundary = min((offset for offset in offsets if offset > info.header_offset), default=directory_offset)
            stream.seek(info.header_offset)
            signature, version, flags, method, _, _, crc, compressed_size, file_size, name_size, extra_size = struct.unpack("<4s5H3I2H", read_exact(stream, 30))
            if signature != b"PK\x03\x04" or version > 20 or flags != info.flag_bits or method != info.compress_type:
                fail("zip_local_header_mismatch")
            if name_size > 2048 or extra_size > 4096:
                fail("zip_local_metadata_limit_exceeded")
            if stream.tell() + name_size + extra_size + info.compress_size + (12 if flags & 8 else 0) > boundary:
                fail("zip_member_overlap_or_truncation")
            local_name = read_exact(stream, name_size).decode("utf-8" if flags & 0x800 else "cp437", errors="strict")
            if local_name != member:
                fail("zip_local_member_name_mismatch")
            no_zip64_extra(read_exact(stream, extra_size))
            if flags & 8:
                if (crc not in (0, info.CRC) or compressed_size not in (0, info.compress_size)
                        or file_size not in (0, info.file_size)):
                    fail("zip_local_size_or_crc_mismatch")
            elif (crc, compressed_size, file_size) != (info.CRC, info.compress_size, info.file_size):
                fail("zip_local_size_or_crc_mismatch")
            if stream.tell() + info.compress_size + (12 if flags & 8 else 0) > boundary:
                fail("zip_member_overlap_or_truncation")
            compressed = read_exact(stream, info.compress_size)
            if method == zipfile.ZIP_STORED:
                data = compressed
            else:
                decoder = zlib.decompressobj(-zlib.MAX_WBITS)
                data = decoder.decompress(compressed, MAX_PAYLOAD + 1)
                if len(data) > MAX_PAYLOAD or decoder.unconsumed_tail:
                    fail("zip_actual_output_limit_exceeded")
                if not decoder.eof or decoder.unused_data:
                    fail("zip_deflate_short_or_trailing_data")
            if len(data) > MAX_PAYLOAD:
                fail("zip_actual_output_limit_exceeded")
            if len(data) != info.file_size or binascii.crc32(data) & 0xffffffff != info.CRC:
                fail("zip_actual_length_or_crc_mismatch")
            if flags & 8:
                first = read_exact(stream, 4)
                if first == b"PK\x07\x08":
                    if stream.tell() + 12 > boundary:
                        fail("zip_descriptor_overlap")
                    descriptor = read_exact(stream, 12)
                else:
                    descriptor = first + read_exact(stream, 8)
                if struct.unpack("<3I", descriptor) != (info.CRC, info.compress_size, info.file_size):
                    fail("zip_descriptor_mismatch")
        unchanged(stream, before)
    return data, {"archive_bytes": before.st_size, "metadata_entries": count,
                  "member_bytes": len(data), "member_sha256": sha256(data), "other_member_bodies_read": 0}


def compare(expected, observed, asserted=False):
    if expected is None or observed is None:
        return check("missing_evidence", "expected_or_observed_value_absent")
    if expected == "unknown" or observed == "unknown":
        return check("unknown", "identity_not_established")
    if expected != observed:
        return check("contradiction", "expected_observed_mismatch")
    return check("asserted_match" if asserted else "observed_match",
                 "manifest_assertions_agree" if asserted else "independently_read_bytes_match_expectation")


def identity_checks(expected, record):
    observed = record.get("identity", {})
    result = {}
    for field in IDENTITY_FIELDS:
        left, right = expected.get(field), observed.get(field)
        if field in ("user_id", "save_owner_id", "program_id"):
            left = left.lower() if left is not None else None
            right = right.lower() if right is not None else None
        result[field] = compare(left, right, True)
    return result


def payload_checks(data, expected, write_id):
    result = {"bytes": len(data), "sha256": sha256(data)}
    result["length"] = compare(expected.get("length"), len(data))
    result["hash"] = compare(expected.get("sha256"), result["sha256"])
    try:
        decoded = parse_json(data)
        shape(decoded, ("schema", "logical_key", "write_id", "nonce", "text"),
              ("schema", "logical_key", "write_id", "nonce", "text"))
        if (type(decoded["schema"]) is not int or decoded["schema"] != 1 or decoded["logical_key"] != KEY
                or decoded["text"] != TEXT or not hex_string(decoded["nonce"], 32)
                or not hex_string(decoded["write_id"], 32)):
            fail("payload_does_not_match_dedicated_profile")
        result["format"] = check("observed_match", "dedicated_json_profile_valid")
        result["nonce"] = compare(expected.get("nonce"), decoded["nonce"])
        result["write_id"] = compare(write_id, decoded["write_id"])
        result["nonce_sha256"] = sha256(decoded["nonce"].encode("ascii"))
    except ProbeError as error:
        result["format"] = check("rejected", str(error))
        result["nonce"] = check("unknown", "payload_could_not_be_decoded")
        result["write_id"] = check("unknown", "payload_could_not_be_decoded")
    return result


def claimed_result(value, positive, negative, missing="observation_absent"):
    if value is None:
        return check("missing_evidence", missing)
    if value == positive:
        return check("asserted_match", "manifest_observation_only_no_durability_inference")
    if value == negative:
        return check("contradiction", "manifest_reports_failure")
    return check("unknown", "manifest_reports_unknown")


def capture_binding(record, expected_write):
    return {"write_id": compare(expected_write, record.get("write_id"), True),
            "capture_id": check("asserted_match", "manifest_capture_identifier_present") if record.get("capture_id") else check("missing_evidence", "capture_identifier_absent"),
            "process_id": check("asserted_match", "manifest_process_identifier_present") if record.get("process_id") else check("missing_evidence", "process_identifier_absent")}


def distinct_claim(initial, restart, field):
    if not initial.get(field) or not restart.get(field):
        return check("missing_evidence", "separate_initial_restart_identifier_absent")
    if initial[field] == restart[field]:
        return check("contradiction", "initial_identifier_reused_for_restart")
    return check("asserted_match", "manifest_reports_distinct_initial_restart_identifiers")


def ui_export_scope(record, member):
    """Cross-check declared path against the fixed 79a UI subtree, never host paths."""
    source = record.get("source")
    if source is None:
        return check("missing_evidence", "export_source_not_recorded")
    if source != "save_management_utils_v79a":
        return check("unknown", "existing_ui_export_coverage_not_established")
    identity = record.get("identity", {})
    if any(identity.get(field) is None for field in IDENTITY_FIELDS) or member is None:
        return check("missing_evidence", "ui_export_identity_or_member_absent")
    if identity["space"] == "unknown" or identity["type"] == "unknown":
        return check("unknown", "ui_export_identity_not_established")
    if (identity["space"] != "user" or identity["type"] not in ("account", "device")
            or identity["user_id"].lower() != "00000000000000000000000000000001"
            or identity["program_id"].lower() != identity["save_owner_id"].lower()):
        return check("contradiction", "asserted_location_outside_existing_ui_export_subtree")
    root = "switch/nand/user/save/0000000000000000/00000000000000000000000000000001/"
    if not identity["path"].startswith(root + identity["save_owner_id"].upper() + "/"):
        return check("contradiction", "asserted_path_outside_existing_ui_export_subtree")
    if identity["path"][len(root):] != member:
        return check("contradiction", "declared_member_disagrees_with_asserted_ui_export_path")
    return check("asserted_match", "declared_member_and_asserted_path_fit_existing_ui_export_subtree")


def all_states(value):
    if isinstance(value, dict):
        if "state" in value:
            yield value["state"]
        for child in value.values():
            yield from all_states(child)
    elif isinstance(value, list):
        for child in value:
            yield from all_states(child)


def exit_code(report):
    states = set(all_states(report["evidence"]))
    return 1 if states & BAD else 2 if states & INCOMPLETE else 0


def verify(manifest_path, initial_file=None, restart_file=None, export_zip=None):
    report = {"schema": REPORT_SCHEMA, "evidence_kind": "offline_local_comparison",
              "assertion_authentication": "not_authenticated_by_this_tool",
              "logical_key": KEY,
              "limits": {"manifest_bytes": MAX_MANIFEST, "payload_bytes": MAX_PAYLOAD,
                         "archive_bytes": MAX_ZIP, "central_directory_bytes": MAX_CENTRAL_DIRECTORY,
                         "archive_entries": MAX_ENTRIES, "compressed_member_bytes": MAX_COMPRESSED},
              "durability": check("unknown", "restart_readback_callback_close_and_zip_do_not_prove_power_loss_durability"),
              "evidence": {}}
    evidence = report["evidence"]
    try:
        manifest_data, _ = read_bounded(manifest_path, MAX_MANIFEST)
        manifest = validate_manifest(parse_json(manifest_data))
    except (ProbeError, OSError) as error:
        evidence["manifest"] = check("rejected", str(error) if isinstance(error, ProbeError) else "manifest_io_error")
        return report
    report["manifest"] = {"bytes": len(manifest_data), "sha256": sha256(manifest_data)}
    report["manifest_declared_evidence_origin"] = manifest.get("evidence_origin", "unknown")
    evidence["manifest"] = check("observed_match", "manifest_v1_structure_valid")
    codec = manifest.get("codec")
    evidence["codec"] = check("observed_match", "explicit_supported_dedicated_profile") if codec == CODEC else check(
        "missing_evidence" if codec is None else "unsupported", "codec_absent" if codec is None else "codec_not_supported_no_payload_read")
    expected = manifest.get("expected", {})
    expected_identity = expected.get("identity", {})
    expected_payload = expected.get("payload", {})
    observations = manifest.get("observations", {})
    writer = observations.get("writer", {})
    evidence["writer"] = {"identity": identity_checks(expected_identity, writer),
                          "write_id": compare(expected.get("write_id"), writer.get("write_id"), True),
                          "callback": claimed_result(writer.get("callback"), "success", "failure"),
                          "completion": claimed_result(writer.get("writer"), "completed", "failed"),
                          "file_closed": claimed_result(writer.get("file_closed"), True, False)}
    initial_inode = None
    for stage, path in (("initial", initial_file), ("restart", restart_file), ("export", export_zip)):
        record = observations.get(stage, {})
        layer = {"identity": identity_checks(expected_identity, record),
                 "write_id": compare(expected.get("write_id"), record.get("write_id"), True)}
        evidence[stage] = layer
        if stage in ("initial", "restart"):
            layer["capture"] = capture_binding(record, expected.get("write_id"))
        if stage == "initial":
            layer["source"] = compare("dedicated_file_snapshot", record.get("source"), True)
        elif stage == "restart":
            first = observations.get("initial", {})
            layer["source"] = compare("validated_get_api", record.get("source"), True)
            layer["api_signature_verified"] = claimed_result(record.get("api_signature_verified"), True, False)
            layer["previous_process_stopped"] = claimed_result(record.get("previous_process_stopped"), True, False)
            layer["distinct_process"] = distinct_claim(first, record, "process_id")
            layer["distinct_capture"] = distinct_claim(first, record, "capture_id")
        else:
            consistency = record.get("consistency")
            layer["ui_export_scope"] = ui_export_scope(record, manifest.get("zip_member"))
            layer["consistency"] = check("asserted_match", "manifest_reports_" + consistency + "_export") if consistency in ("stopped", "coordinated") else check(
                "missing_evidence" if consistency is None else "unknown", "export_consistency_not_established")
        if path is None:
            layer["input"] = check("missing_evidence", "explicit_stage_file_not_supplied")
            continue
        if codec != CODEC:
            layer["input"] = check("unsupported" if codec else "missing_evidence", "supported_codec_required_before_payload_read")
            continue
        try:
            if stage == "export":
                if "zip_member" not in manifest:
                    layer["input"] = check("missing_evidence", "exact_zip_member_not_declared")
                    continue
                data, zip_metadata = read_zip_member(path, manifest["zip_member"])
                layer["zip"] = zip_metadata
            else:
                data, inode = read_bounded(path, MAX_PAYLOAD, dedicated=True)
                if stage == "initial":
                    initial_inode = inode
                elif inode == initial_inode:
                    fail("initial_file_reused_for_restart")
            layer["input"] = check("observed_match", "explicit_file_independently_read")
            layer["payload"] = payload_checks(data, expected_payload, expected.get("write_id"))
        except (ProbeError, OSError, ValueError, UnicodeError, struct.error, zipfile.BadZipFile, zlib.error) as error:
            layer["input"] = check("rejected", str(error) if isinstance(error, ProbeError) else "invalid_or_corrupt_stage_file")
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--initial-file", type=Path, help="Explicit local dedicated probe snapshot")
    parser.add_argument("--restart-file", type=Path, help="Separate dedicated probe JSON captured from validated Get API after restart")
    parser.add_argument("--export-zip", type=Path, help="Explicit local ZIP; reads only the exact manifest member")
    args = parser.parse_args(argv)
    report = verify(args.manifest, args.initial_file, args.restart_file, args.export_zip)
    print(json.dumps(report, indent=2, sort_keys=True))
    return exit_code(report)


if __name__ == "__main__":
    sys.exit(main())
