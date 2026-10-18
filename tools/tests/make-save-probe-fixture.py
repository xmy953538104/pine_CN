#!/usr/bin/env python3
"""Generate only noncommercial synthetic evidence for the offline verifier tests/CI."""

import argparse
import hashlib
import json
from pathlib import Path
import zipfile


KEY = "dfx_diag_probe_v1"
BASENAME = KEY + ".json"
OWNER = "0100000000ABCDEF"


def fixture():
    write_id = "1234567890abcdef1234567890abcdef"
    nonce = "fedcba0987654321fedcba0987654321"
    payload = (json.dumps({"schema": 1, "logical_key": KEY, "write_id": write_id, "nonce": nonce,
                           "text": "pine dedicated save probe v1"}, sort_keys=True, separators=(",", ":")) + "\n").encode("utf-8")
    identity = {"space": "user", "type": "account", "user_id": "00000000000000000000000000000001",
                "save_owner_id": OWNER.lower(), "program_id": OWNER.lower(),
                "path": "switch/nand/user/save/0000000000000000/00000000000000000000000000000001/" + OWNER + "/" + BASENAME}
    # All identity/lifecycle observations are fabricated test assertions, never device evidence.
    def observed(**kwargs):
        observed_identity = dict(identity)
        observed_identity["save_owner_id"] = OWNER
        observed_identity["program_id"] = OWNER
        return {"identity": observed_identity, "write_id": write_id, **kwargs}

    manifest = {"schema": "pine.save-probe.manifest.v1", "evidence_origin": "synthetic",
                "authorization": {"scope": "dedicated_probe_only", "logical_key": KEY, "basename": BASENAME},
                "codec": "pine-probe-json-utf8-v1",
                "zip_member": OWNER + "/" + BASENAME,
                "expected": {"identity": identity, "write_id": write_id,
                             "payload": {"nonce": nonce, "length": len(payload), "sha256": hashlib.sha256(payload).hexdigest()}},
                "observations": {
                    "writer": observed(callback="success", writer="completed", file_closed=True),
                    "initial": observed(capture_id="synthetic-initial-capture", process_id="synthetic-writer-process", source="dedicated_file_snapshot"),
                    "restart": observed(capture_id="synthetic-restart-capture", process_id="synthetic-reader-process", source="validated_get_api",
                                        api_signature_verified=True, previous_process_stopped=True),
                    "export": observed(consistency="stopped", source="save_management_utils_v79a")}}
    return manifest, payload


def write_fixture(output):
    if output.exists() and (not output.is_dir() or any(output.iterdir())):
        raise ValueError("Synthetic fixture destination must be an empty or new directory")
    output.mkdir(parents=True, exist_ok=True)
    manifest, payload = fixture()
    for stage in ("initial", "restart"):
        directory = output / stage
        directory.mkdir()
        (directory / BASENAME).write_bytes(payload)
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    with zipfile.ZipFile(output / "stopped.zip", "w", zipfile.ZIP_DEFLATED) as archive:
        archive.writestr(manifest["zip_member"], payload)
        archive.writestr("unrelated-private-save.bin", b"SYNTHETIC body must never be processed")
    return manifest, payload


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    write_fixture(parser.parse_args().output)
