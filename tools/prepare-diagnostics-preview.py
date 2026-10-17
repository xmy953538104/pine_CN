#!/usr/bin/env python3
"""Only publish a manually verified tag's exact successful-CI APK as a preview."""
import hashlib
import json
import os
import pathlib
import re
import subprocess
import sys
import zipfile


def gh_json(endpoint):
    return json.loads(subprocess.check_output(["gh", "api", endpoint], text=True))


def evidence():
    ref = os.environ["GITHUB_REF"]
    assert re.fullmatch(r"refs/tags/v[0-9.]+-diagnostics-v1(?:[.-][0-9]+)?", ref), ref
    # Lightweight tags and unverified free-form annotations cannot publish.
    assert subprocess.check_output(["git", "cat-file", "-t", ref], text=True).strip() == "tag"
    body = subprocess.check_output(["git", "for-each-ref", "--format=%(contents)", ref], text=True)
    value = json.loads(body)
    actual = subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip()
    tagged = subprocess.check_output(["git", "rev-parse", ref + "^{commit}"], text=True).strip()
    assert value["schema"] == 1 and value["commit"] == actual == tagged
    assert re.fullmatch(r"[0-9a-f]{64}", value["apk_sha256"])
    assert value["runtime"]["status"] in ("verified", "blocked")
    assert value["runtime"]["evidence"] and value["limitations"]
    assert value["physical_arm64_verified"] is False and value["commercial_game_verified"] is False
    return value


value = evidence()
if len(sys.argv) > 1:
    assert sys.argv[1] in ("--verify-apk", "--verify-release")
    downloaded = sys.argv[1] == "--verify-release"
    folder = pathlib.Path(sys.argv[2])
    if downloaded:
        assert {p.name for p in folder.iterdir()} == {"app-full-debug.apk", "build-identity.json", "verification-evidence.json"}
        assert json.loads((folder / "verification-evidence.json").read_text()) == value
        assert (folder / "build-identity.json").read_bytes() == pathlib.Path("preview/build-identity.json").read_bytes()
    assert {p.name for p in folder.glob("*.apk")} == {"app-full-debug.apk"}
    identity = json.loads((folder / "build-identity.json").read_text())
    assert identity["commit"] == value["commit"]
    assert identity["application_id"] == "io.github.pine.emu.diagnostics.preview"
    assert identity["abi"] == ["arm64-v8a"] and identity["build_type"] == "debug"
    assert identity["release_emulation_process_manifest_verified"] is True
    jvm = identity["jvm_tests"]
    assert jvm["tests"] >= 20 and all(jvm[key] == 0 for key in ("failures", "errors", "skipped"))
    assert identity["apk"] == "app-full-debug.apk"
    apk = folder / identity["apk"]
    with apk.open("rb") as stream:
        assert hashlib.file_digest(stream, "sha256").hexdigest() == identity["sha256"] == value["apk_sha256"]
    with zipfile.ZipFile(apk) as archive:
        elf = archive.read("lib/arm64-v8a/libskyline.so")
        assert elf[:6] == b"\x7fELF\x02\x01" and int.from_bytes(elf[18:20], "little") == 183
        assert hashlib.sha256(elf).hexdigest() == identity["skyline_elf_sha256"]
        assert any(value["commit"].encode() in archive.read(name) for name in archive.namelist() if re.fullmatch(r"classes\d*\.dex", name))
    if downloaded:
        print("Verified draft assets exactly match tested APK, CI identity and annotated evidence")
        sys.exit(0)
    (folder / "verification-evidence.json").write_text(json.dumps(value, indent=2) + "\n")
    notes = pathlib.Path("docs/diagnostics-preview-notes.md").read_text()
    notes += f"\nCommit: `{value['commit']}`\n\nAPK SHA-256: `{value['apk_sha256']}`\n\n"
    notes += f"CI: https://github.com/{os.environ['GITHUB_REPOSITORY']}/actions/runs/{value['ci_run_id']}\n\n"
    notes += "Runtime: " + value["runtime"]["status"] + ". " + value["runtime"]["evidence"] + "\n\n"
    notes += "Limitations: " + value["limitations"] + "\n"
    (folder / "release-notes.md").write_text(notes)
else:
    repo = os.environ["GITHUB_REPOSITORY"]
    run_id = int(value["ci_run_id"])
    run = gh_json(f"repos/{repo}/actions/runs/{run_id}")
    assert run["repository"]["full_name"] == repo and run["head_sha"] == value["commit"]
    assert run["name"] == "Diagnostics Debug" and run["path"] == ".github/workflows/diagnostics.yml"
    assert run["status"] == "completed" and run["conclusion"] == "success"
    jobs = gh_json(f"repos/{repo}/actions/runs/{run_id}/jobs?per_page=100")["jobs"]
    required = {"native", "adb", "debug-apk"}
    assert required <= {j["name"] for j in jobs if j["conclusion"] == "success"}
    with open(os.environ["GITHUB_OUTPUT"], "a") as output:
        output.write(f"run_id={run_id}\ncommit={value['commit']}\n")
    print("Verified successful CI for", value["commit"], "; runtime status:", value["runtime"]["status"])
