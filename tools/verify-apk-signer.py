#!/usr/bin/env python3
"""Verify an APK with official apksigner and pin its existing CI public signer.

Only signed APK bytes, public certificates and source identity are read. This
tool never loads a keystore or credential, generates a key, or signs an APK.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


def verify_certificate(apk, apksigner, expected):
    if not re.fullmatch(r"[0-9a-f]{64}", expected):
        raise ValueError("Invalid pinned public certificate SHA256")
    result = subprocess.run([str(apksigner), "verify", "--print-certs", str(apk)],
                            check=True, capture_output=True, text=True, timeout=60)
    certificates = re.findall(r"^Signer #\d+ certificate SHA-256 digest: ([0-9a-f]{64})$",
                              result.stdout, re.MULTILINE)
    if certificates != [expected]:
        raise ValueError("APK signer differs from the single pinned existing CI certificate: " + repr(certificates))
    return certificates[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--apk", required=True, type=Path)
    parser.add_argument("--apksigner", required=True, type=Path)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--pin", type=Path, default=Path(__file__).with_name("diagnostics-ci-signer.json"))
    args = parser.parse_args()
    commit = subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip()
    if not re.fullmatch(r"[0-9a-f]{40}", args.commit) or args.commit != commit:
        raise ValueError("Source commit does not match the requested APK build")
    pin = json.loads(args.pin.read_text(encoding="utf-8"))
    if pin.get("schema") != 1:
        raise ValueError("Unknown public signer pin schema")
    certificate = verify_certificate(args.apk, args.apksigner, pin["certificate_sha256"])
    with args.apk.open("rb") as stream:
        apk_sha = hashlib.file_digest(stream, "sha256").hexdigest()
    manifest = {"schema": 1, "source_commit": commit, "apk_sha256": apk_sha,
                "certificate_sha256": certificate, "signature_verified": True,
                "signer_reference": pin}
    args.apk.with_name("signer-identity.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(manifest, sort_keys=True))


if __name__ == "__main__":
    main()
