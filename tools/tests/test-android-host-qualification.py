#!/usr/bin/env python3
"""Host-only regression: failed acceleration must never create an AVD or install an APK."""
import hashlib
import json
import os
from pathlib import Path
import runpy
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class HostQualification(unittest.TestCase):
    def test_inaccessible_accelerator_blocks_all_apk_checks_before_avd_or_adb(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            sdk = folder / "official-sdk-fixture"
            (sdk / "emulator").mkdir(parents=True)
            (sdk / "platform-tools").mkdir()
            binaries = folder / "bin"
            binaries.mkdir()
            emulator = sdk / "emulator/emulator"
            emulator.write_text("#!/bin/sh\nprintf 'This user does not have permission to use KVM.\\n'\nexit 11\n")
            marker = folder / "unexpected-android-command"
            for path in (sdk / "platform-tools/adb", binaries / "avdmanager"):
                path.write_text("#!/bin/sh\nprintf attempted > \"$PINE_HOST_TEST_MARKER\"\nexit 99\n")
                path.chmod(0o755)
            emulator.chmod(0o755)
            fixture = folder / "not-an-apk.fixture"
            fixture.write_bytes(b"Host preflight fixture only; not an APK or Android execution")
            output = folder / "evidence"
            environment = dict(os.environ, ANDROID_HOME=str(sdk), RUNNER_TEMP=str(folder),
                PINE_CI_REQUIRE_ACCELERATION="1", PINE_DIAGNOSTICS_OWNED_AVD_PROFILE="1",
                GITHUB_STEP_SUMMARY=str(folder / "fixture-summary.md"),
                PINE_HOST_TEST_MARKER=str(marker), PATH=str(binaries) + os.pathsep + os.environ["PATH"])
            result = subprocess.run(["bash", str(ROOT / "tools/tests/attempt-android-runtime.sh"),
                str(fixture), "f" * 40, str(output)], cwd=ROOT, env=environment,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=15)
            self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))
            report = json.loads((output / "runtime-report.json").read_text())
            self.assertEqual(report["status"], "environment_blocked")
            self.assertTrue(report["finished"])
            self.assertEqual(report["stage"], "host_acceleration_preflight")
            self.assertEqual(report["commit"], "f" * 40)
            self.assertEqual(report["apk_sha256"], hashlib.sha256(fixture.read_bytes()).hexdigest())
            self.assertFalse(report["apk_installed"])
            self.assertFalse(report["physical_arm64_verified"])
            self.assertFalse(report["permission_changes_attempted"])
            required = runpy.run_path(str(ROOT / "tools/android-diagnostics-smoke.py"))["REQUIRED"]
            self.assertEqual(set(report["checks"]), set(required))
            self.assertEqual(len(report["checks"]), 18)
            self.assertTrue(all(row["status"] == "blocked" for row in report["checks"].values()))
            self.assertFalse(marker.exists(), "Neither adb nor avdmanager may run without acceleration")
            self.assertFalse((folder / "pine-diagnostics-avd/PineDiagnosticsCI.avd").exists())
            self.assertFalse((output / "install.txt").exists())
            self.assertIn("11", (output / "accel-check.txt").read_text())


if __name__ == "__main__":
    unittest.main()
