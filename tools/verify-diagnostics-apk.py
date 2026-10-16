#!/usr/bin/env python3
"""Check packaged Debug ABI/identity and write a SHA256 manifest beside the APK."""
import hashlib
import json
import pathlib
import re
import subprocess
import sys
import zipfile

apk = pathlib.Path(sys.argv[1])
expected = sys.argv[2]
actual = subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip()
assert re.fullmatch(r"[0-9a-f]{40}", expected) and actual == expected, (actual, expected)
badging = apk.with_name("apk-badging.txt").read_text(encoding="utf-8")
assert "name='io.github.pine.emu.diagnostics'" in badging
assert "-diagnostics-v1'" in badging
assert "sdkVersion:'29'" in badging
with zipfile.ZipFile(apk) as archive:
    libs = sorted(p for p in archive.namelist() if p.startswith("lib/") and p.endswith(".so"))
    assert "lib/arm64-v8a/libskyline.so" in libs, libs
    assert all(p.split("/")[1] == "arm64-v8a" for p in libs), libs
    assert any(expected.encode() in archive.read(name) for name in archive.namelist() if re.fullmatch(r"classes\d*\.dex", name)), "BuildConfig commit missing from DEX"
metadata = {
    "schema": 1,
    "commit": actual,
    "apk": apk.name,
    "sha256": hashlib.file_digest(apk.open("rb"), "sha256").hexdigest(),
    "application_id": "io.github.pine.emu.diagnostics",
    "abi": ["arm64-v8a"],
    "min_sdk": 29,
    "build_type": "debug",
    "runtime_validation": "See docs/diagnostics-validation.md; build verification is not device execution.",
}
apk.with_name("build-identity.json").write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
print(json.dumps(metadata, sort_keys=True))
