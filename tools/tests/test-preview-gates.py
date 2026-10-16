#!/usr/bin/env python3
"""Offline release-gate regression tests. ZIP fixtures are not executable APKs."""
import copy
import hashlib
import json
import os
import pathlib
import subprocess
import tempfile
import unittest
import zipfile

SCRIPT = pathlib.Path(__file__).resolve().parents[1] / "prepare-diagnostics-preview.py"


class PreviewGates(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.temp.name)
        self.bin = self.root / "bin"
        self.bin.mkdir()
        self.fake = self.root / "api.json"
        gh = self.bin / "gh"
        gh.write_text("#!/usr/bin/env python3\nimport json,os,sys\nx=json.load(open(os.environ['PINE_PREVIEW_TEST_API']))\nprint(json.dumps(x['jobs'] if sys.argv[-1].endswith('jobs?per_page=100') else x['run']))\n")
        gh.chmod(0o755)
        self.git("init", "-q")
        self.git("-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid", "commit", "--allow-empty", "-qm", "fixture")
        self.sha = self.git("rev-parse", "HEAD").strip()
        self.folder = self.root / "preview"
        self.folder.mkdir()
        self.apk = self.folder / "app-full-debug.apk"
        self.elf = b"\x7fELF\x02\x01" + bytes(12) + (183).to_bytes(2, "little") + b"synthetic header only, not executable ELF"
        with zipfile.ZipFile(self.apk, "w") as z:
            z.writestr("lib/arm64-v8a/libskyline.so", self.elf)
            z.writestr("classes.dex", self.sha)
        digest = hashlib.sha256(self.apk.read_bytes()).hexdigest()
        self.value = {"schema": 1, "commit": self.sha, "apk_sha256": digest, "ci_run_id": 42,
                      "runtime": {"status": "blocked", "evidence": "synthetic gate test only"},
                      "limitations": "no executable APK or device in this test", "physical_arm64_verified": False,
                      "commercial_game_verified": False}
        self.identity = {"commit": self.sha, "apk": self.apk.name, "sha256": digest,
                         "application_id": "io.github.pine.emu.diagnostics", "abi": ["arm64-v8a"], "build_type": "debug",
                         "skyline_elf_sha256": hashlib.sha256(self.elf).hexdigest(),
                         "jvm_tests": {"tests": 20, "failures": 0, "errors": 0, "skipped": 0},
                         "release_emulation_process_manifest_verified": True}
        self.api = {"run": {"repository": {"full_name": "owner/repo"}, "head_sha": self.sha,
                            "name": "Diagnostics Debug", "path": ".github/workflows/diagnostics.yml",
                            "status": "completed", "conclusion": "success"},
                    "jobs": {"jobs": [{"name": x, "conclusion": "success"} for x in ["native", "adb", "debug-apk"]]}}
        self.env = dict(os.environ, PATH=str(self.bin) + os.pathsep + os.environ["PATH"],
                        GITHUB_REF="refs/tags/v2.1.2-diagnostics-v1", GITHUB_SHA=self.sha,
                        GITHUB_REPOSITORY="owner/repo", GITHUB_OUTPUT=str(self.root / "output"),
                        PINE_PREVIEW_TEST_API=str(self.fake))
        (self.root / "docs").mkdir()
        (self.root / "docs/diagnostics-preview-notes.md").write_text("Synthetic gate test.\n")
        self.annotate()

    def tearDown(self):
        self.temp.cleanup()

    def git(self, *args):
        return subprocess.check_output(["git", *args], cwd=self.root, text=True, stderr=subprocess.DEVNULL)

    def annotate(self):
        self.git("tag", "-d", "v2.1.2-diagnostics-v1") if (self.root / ".git/refs/tags/v2.1.2-diagnostics-v1").exists() else None
        self.git("-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid", "tag", "-a", "v2.1.2-diagnostics-v1", "-m", json.dumps(self.value))

    def run_gate(self, *args):
        self.fake.write_text(json.dumps(self.api))
        (self.folder / "build-identity.json").write_text(json.dumps(self.identity))
        return subprocess.run(["python3", str(SCRIPT), *args], cwd=self.root, env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    def test_ci_requires_exact_success_and_all_jobs(self):
        self.assertEqual(self.run_gate().returncode, 0)
        original = copy.deepcopy(self.api)
        for field, value in [("head_sha", "0" * 40), ("conclusion", "failure"), ("path", ".github/workflows/other.yml")]:
            self.api = copy.deepcopy(original)
            self.api["run"][field] = value
            self.assertNotEqual(self.run_gate().returncode, 0)
        self.api = copy.deepcopy(original)
        self.api["jobs"]["jobs"][1]["conclusion"] = "skipped"
        self.assertNotEqual(self.run_gate().returncode, 0)

    def test_requires_annotated_verified_evidence(self):
        self.git("tag", "-d", "v2.1.2-diagnostics-v1")
        self.git("tag", "v2.1.2-diagnostics-v1")
        self.assertNotEqual(self.run_gate().returncode, 0)
        self.value["runtime"]["status"] = "unverified"
        self.annotate()
        self.assertNotEqual(self.run_gate().returncode, 0)

    def test_artifact_identity_and_content_are_bound(self):
        self.assertEqual(self.run_gate("--verify-apk", str(self.folder)).returncode, 0)
        self.apk.write_bytes(self.apk.read_bytes() + b"tamper")
        self.assertNotEqual(self.run_gate("--verify-apk", str(self.folder)).returncode, 0)

    def test_downloaded_evidence_and_metadata_must_match(self):
        import shutil
        self.assertEqual(self.run_gate("--verify-apk", str(self.folder)).returncode, 0)
        downloaded = self.root / "download"
        downloaded.mkdir()
        for name in ("app-full-debug.apk", "build-identity.json", "verification-evidence.json"):
            shutil.copyfile(self.folder / name, downloaded / name)
        self.assertEqual(self.run_gate("--verify-release", str(downloaded)).returncode, 0)
        (downloaded / "verification-evidence.json").unlink()
        self.assertNotEqual(self.run_gate("--verify-release", str(downloaded)).returncode, 0)
        (downloaded / "verification-evidence.json").write_text("{}")
        self.assertNotEqual(self.run_gate("--verify-release", str(downloaded)).returncode, 0)

    def test_extra_apk_is_rejected(self):
        (self.folder / "unverified.apk").write_bytes(b"not a tested APK")
        self.assertNotEqual(self.run_gate("--verify-apk", str(self.folder)).returncode, 0)

    def test_artifact_metadata_rejects_other_source_abi_and_path(self):
        original = copy.deepcopy(self.identity)
        for key, value in [("commit", "0" * 40), ("abi", ["x86_64"]), ("apk", "../app-full-debug.apk"), ("jvm_tests", {"tests": 0, "failures": 0, "errors": 0, "skipped": 0})]:
            self.identity = copy.deepcopy(original)
            self.identity[key] = value
            self.assertNotEqual(self.run_gate("--verify-apk", str(self.folder)).returncode, 0)


if __name__ == "__main__":
    unittest.main()
