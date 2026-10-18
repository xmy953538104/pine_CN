#!/usr/bin/env python3
"""Validate one explicit, nonsensitive metadata JSON report; never read game bytes."""

import argparse
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import stat
import sys

try:
    from jsonschema import Draft202012Validator
except ImportError:
    Draft202012Validator = None

MAX_BYTES = 1024 * 1024
MAX_CONTENTS = 2048
SCHEMA = Path(__file__).resolve().parent / "content-handoff/metadata-v1.schema.json"


class InputError(Exception):
    """Fixed reason code only; never contains an input key/value/path."""


def reject(code):
    raise InputError(code)


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            reject("duplicate_json_key")
        result[key] = value
    return result


def read_json(path):
    if not hasattr(os, "O_NOFOLLOW"):
        reject("host_nofollow_required")
    try:
        fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | getattr(os, "O_NONBLOCK", 0))
        with os.fdopen(fd, "rb") as stream:
            before = os.fstat(stream.fileno())
            if not stat.S_ISREG(before.st_mode):
                reject("input_not_regular")
            if before.st_size > MAX_BYTES:
                reject("input_byte_limit")
            data = stream.read(MAX_BYTES + 1)
            after = os.fstat(stream.fileno())
            if len(data) > MAX_BYTES:
                reject("actual_byte_limit")
            if len(data) != before.st_size or (before.st_size, before.st_mtime_ns, before.st_ctime_ns) != (after.st_size, after.st_mtime_ns, after.st_ctime_ns):
                reject("input_changed_or_short")
    except OSError:
        reject("input_open_failed_or_symlink")
    try:
        value = json.loads(data.decode("utf-8", errors="strict"), object_pairs_hook=unique_object,
                           parse_constant=lambda _: reject("non_finite_json_number"))
    except (UnicodeError, ValueError, RecursionError):
        reject("invalid_utf8_json")
    return value, data


def known(fact):
    return fact.get("value") if isinstance(fact, dict) and fact.get("state") in ("observed", "reported") else None


def equal(left, right):
    if isinstance(left, str) and isinstance(right, str):
        return left.lower() == right.lower()
    return left == right


def semantic(report):
    container = report["container"]
    container_id = container["report_id"]
    groups = report["cnmts"]
    contents = [(group, content) for group in groups for content in group.get("contents", [])]
    if len(contents) > MAX_CONTENTS:
        reject("aggregate_content_limit")
    issues, issue_count = [], 0
    unknowns = Counter()

    def issue(code):
        nonlocal issue_count
        issue_count += 1
        if len(issues) < 64:
            issues.append({"code": code})

    records = [container] + report["ncas"] + groups + [content for _, content in contents]
    identifiers = [record["report_id"] for record in records]
    if len(set(identifiers)) != len(identifiers):
        issue("duplicate_report_id")
    records_by_id = {record["report_id"]: record for record in records}
    ncas = {record["report_id"]: record for record in report["ncas"]}
    cnmts = {record["report_id"]: record for record in groups}
    for record in records[1:]:
        if record["container_report_id"] != container_id:
            issue("record_container_mismatch")
    observations = {}
    for observation in report["observations"]:
        identifier = observation["observation_id"]
        if identifier in observations:
            issue("duplicate_observation_id")
        observations[identifier] = observation
        if observation["container_report_id"] != container_id:
            issue("observation_container_mismatch")
        if observation["subject_report_id"] not in records_by_id:
            issue("observation_subject_missing")

    def facts(record, subject_id, scope):
        for field, fact in record.items():
            values = fact if field == "unreadable_metadata" else [fact]
            for value in values:
                if not isinstance(value, dict) or "state" not in value:
                    continue
                if value["state"] == "unknown":
                    unknowns["explicit_unknown_facts"] += 1
                    continue
                observation = observations.get(value["observation_id"])
                if observation is None:
                    issue("fact_observation_missing")
                    continue
                expected_scope = "filename_hint" if field == "filename_content_id_hint" else "runtime" if field == "runtime_report" else scope
                if observation["container_report_id"] != container_id or observation["subject_report_id"] != subject_id:
                    issue("fact_observation_binding_mismatch")
                if observation["scope"] != expected_scope:
                    issue("fact_observation_scope_mismatch")
                if observation["state"] != value["state"]:
                    issue("fact_observation_state_mismatch")
                if value["state"] == "observed" and observation["source_tool"] is None:
                    issue("observed_fact_source_missing")
                if field == "raw_sha256" and value["state"] == "observed" and observation["method"] != "full_raw_sha256":
                    issue("observed_hash_not_full_raw_measurement")

    facts(container, container_id, "raw_container")
    for record in report["ncas"]:
        facts(record, record["report_id"], "raw_nca")
    for group in groups:
        facts(group, group["report_id"], "cnmt")
        for content in group.get("contents", []):
            facts(content, group["report_id"], "cnmt")

    hash_states = Counter()
    comparisons = Counter()
    discovery, readability = Counter(), Counter()
    for nca in report["ncas"]:
        discovery[known(nca.get("discovery")) or "unknown"] += 1
        readability[known(nca.get("readability")) or "unknown"] += 1
        hash_states[nca.get("raw_sha256", {}).get("state", "unknown")] += 1
    ambiguous_groups = 0
    for group in groups:
        meta_id = known(group.get("meta_nca_report_id"))
        if meta_id is None:
            unknowns["meta_nca_link_unknown"] += 1
        elif meta_id not in ncas:
            issue("meta_nca_reference_missing")
        elif known(ncas[meta_id].get("nca_type")) not in (None, 1):
            issue("cnmt_source_is_not_meta_nca")
        parent_id = known(group.get("application_parent_report_id"))
        parent = cnmts.get(parent_id) if parent_id is not None else None
        role = known(group.get("role"))
        explicit_parent = known(group.get("application_parent"))
        if explicit_parent is not None and role in ("Application", "SystemProgram", "SystemData", "SystemUpdate", "BootImagePackage", "BootImagePackageSafe"):
            issue("role_has_no_explicit_application_parent_field")
        if known(group.get("patch_id")) is not None and role not in (None, "Application"):
            issue("patch_id_field_role_mismatch")
        if known(group.get("data_id")) is not None and role not in (None, "DataPatch"):
            issue("data_id_field_role_mismatch")
        if parent_id is not None and parent is None:
            issue("application_parent_reference_missing")
        if parent is not None:
            if known(parent.get("role")) not in (None, "Application"):
                issue("application_parent_role_mismatch")
            parent_title = known(parent.get("title_id"))
            if explicit_parent is not None and parent_title is not None and not equal(explicit_parent, parent_title):
                issue("explicit_application_parent_mismatch")
            if role == "Patch" and known(parent.get("patch_id")) is not None and known(group.get("title_id")) is not None and not equal(known(parent["patch_id"]), known(group["title_id"])):
                issue("explicit_patch_id_mismatch")
        raw_committed, committed = known(group.get("committed_raw")), known(group.get("committed"))
        if raw_committed is not None and committed is not None and (raw_committed not in (0, 1) or bool(raw_committed) != committed):
            issue("committed_decode_mismatch")
        program_count = 0
        for content in group.get("contents", []):
            content_type = known(content.get("content_type"))
            program_count += content_type == 1
            nca_id = known(content.get("nca_report_id"))
            nca = ncas.get(nca_id) if nca_id is not None else None
            if nca_id is not None and nca is None:
                issue("content_nca_reference_missing")
            if nca is None:
                unknowns["content_nca_link_unknown"] += 1
            else:
                actual_type = known(nca.get("nca_type"))
                allowed = {0: (1,), 1: (0,), 2: (4, 5), 3: (2,)}.get(content_type)
                if allowed is not None and actual_type is not None and actual_type not in allowed:
                    issue("content_reference_nca_type_mismatch")
                for expected_field, actual_field, mismatch in (("expected_raw_sha256", "raw_sha256", "raw_hash_mismatch"), ("expected_raw_size", "raw_size", "raw_size_mismatch")):
                    expected, actual = known(content.get(expected_field)), known(nca.get(actual_field))
                    if expected is None or actual is None:
                        comparisons[actual_field + "_unknown"] += 1
                    elif equal(expected, actual):
                        comparisons[actual_field + "_reported_match"] += 1
                    else:
                        comparisons[actual_field + "_mismatch"] += 1
                        issue(mismatch)
            profile, raw = known(content.get("size_profile")), known(content.get("raw_size_bytes"))
            expected_size = known(content.get("expected_raw_size"))
            if profile is not None and raw is not None:
                raw_bytes = bytes.fromhex(raw)
                decoded = int.from_bytes(raw_bytes if profile == "legacy-48" else raw_bytes[:5], "little")
                if expected_size is not None and decoded != expected_size:
                    issue("cnmt_size_profile_mismatch")
                attributes = known(content.get("content_attributes"))
                if attributes is not None and (profile != "modern-40" or attributes != raw_bytes[5]):
                    issue("cnmt_content_attributes_mismatch")
            else:
                unknowns["size_profile_comparison_unknown"] += 1
            index, offset, basis = known(content.get("program_index")), known(content.get("id_offset")), known(content.get("program_index_basis"))
            if index is not None:
                if basis is None or offset is None:
                    unknowns["program_index_basis_unknown"] += 1
                elif basis == "cnmt_id_offset" and index != offset:
                    issue("program_index_id_offset_mismatch")
        ambiguous_groups += program_count > 1

    unreadable = container.get("unreadable_metadata", [])
    for fact in unreadable:
        value = known(fact)
        if value is not None:
            for field, table in (("nca_report_id", ncas), ("cnmt_report_id", cnmts)):
                if value[field] is not None and value[field] not in table:
                    issue("unreadable_metadata_reference_missing")
    enumeration = known(container.get("enumeration"))
    completeness = "unknown" if enumeration is None else "declared_" + enumeration
    referenced_meta = {known(group.get("meta_nca_report_id")) for group in groups}
    incomplete_ncas = any(known(nca.get("readability")) != "readable" or known(nca.get("nca_type")) is None
                          or (known(nca.get("nca_type")) == 1 and nca["report_id"] not in referenced_meta) for nca in report["ncas"])
    incomplete_groups = any(known(group.get("readability")) != "readable" or known(group.get("meta_nca_report_id")) not in ncas
                            or any(known(group.get(field)) is None for field in ("role", "title_id", "version")) or "contents" not in group for group in groups)
    incomplete_contents = any(any(known(content.get(field)) is None for field in ("content_type", "content_id", "expected_raw_sha256", "expected_raw_size", "raw_size_bytes", "size_profile")) for _, content in contents)
    if enumeration == "complete" and (unreadable or incomplete_ncas or incomplete_groups or incomplete_contents):
        issue("complete_enumeration_has_unknown_or_unreadable_metadata")
        completeness = "contradiction"
    return {"link_consistency": "contradiction" if issue_count else "no_known_contradictions",
            "issue_count": issue_count, "issues": issues, "unknowns": dict(unknowns),
            "discovery_reports": dict(discovery), "nca_readability_reports": dict(readability),
            "metadata_completeness": completeness, "raw_nca_hash_report_states": dict(hash_states),
            "comparisons_between_reported_values": dict(comparisons),
            "program_selection": {"ambiguous_groups": ambiguous_groups, "automatic_selection": "not_performed"},
            "runtime_report_state": container.get("runtime_report", {}).get("state", "unknown")}


def validate(path):
    result = {"schema": "pine.content-metadata-validation.v1", "verification_scope": "schema_and_reported_links_only",
              "provenance": "not_authenticated", "container_or_nca_bytes_read": 0,
              "signatures_verified": False, "runtime_verified": False}
    try:
        if Draft202012Validator is None:
            reject("required_jsonschema_dependency_missing")
        schema, _ = read_json(SCHEMA)
        report, data = read_json(path)
        result["input_report_bytes"] = len(data)
        result["input_report_sha256"] = hashlib.sha256(data).hexdigest()
        if not Draft202012Validator(schema).is_valid(report):
            reject("schema_invalid")
        result["schema_validation"] = "valid"
        result["declared_data_kind"] = report["data_kind"]
        result.update(semantic(report))
    except InputError as error:
        result["schema_validation"] = "rejected"
        result["reason"] = str(error)
    return result


def exit_code(result):
    return 2 if result["schema_validation"] == "rejected" else 1 if result["link_consistency"] == "contradiction" else 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="One explicit local metadata JSON file")
    result = validate(parser.parse_args(argv).input)
    print(json.dumps(result, indent=2, sort_keys=True))
    return exit_code(result)


if __name__ == "__main__":
    sys.exit(main())
