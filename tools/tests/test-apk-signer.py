#!/usr/bin/env python3
"""Offline signer-verifier failure boundaries; not an APK build/runtime pass."""
from pathlib import Path
import runpy
import subprocess
import tempfile
import unittest

VERIFY = runpy.run_path(str(Path(__file__).resolve().parents[1] / "verify-apk-signer.py"))["verify_certificate"]
PIN = "a" * 64


class SignerGate(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.tool = self.root / "apksigner"
        self.apk = self.root / "fresh.apk"
        self.apk.write_bytes(b"offline-signature-fixture")

    def fixture(self, stdout, status=0):
        self.tool.write_text("#!/usr/bin/env python3\nimport sys\n"
                             "assert sys.argv[1:3] == ['verify', '--print-certs']\n"
                             "print(" + repr(stdout) + ")\nraise SystemExit(" + str(status) + ")\n")
        self.tool.chmod(0o755)

    def test_single_pinned_certificate_only(self):
        self.fixture("Signer #1 certificate SHA-256 digest: " + PIN)
        self.assertEqual(VERIFY(self.apk, self.tool, PIN), PIN)

    def test_wrong_missing_multiple_and_unverified_certificates_fail(self):
        for stdout, code in (("", 0), ("Signer #1 certificate SHA-256 digest: " + "b" * 64, 0),
                             ("Signer #1 certificate SHA-256 digest: " + PIN + "\nSigner #2 certificate SHA-256 digest: " + PIN, 0),
                             ("Signer #1 certificate SHA-256 digest: " + PIN, 1),
                             ("Source Stamp Signer certificate SHA-256 digest: " + PIN, 0)):
            with self.subTest(stdout=stdout, code=code):
                self.fixture(stdout, code)
                with self.assertRaises((ValueError, subprocess.CalledProcessError)):
                    VERIFY(self.apk, self.tool, PIN)

    def test_invalid_pin_and_missing_official_tool_fail(self):
        self.fixture("Signer #1 certificate SHA-256 digest: " + PIN)
        with self.assertRaises(ValueError):
            VERIFY(self.apk, self.tool, "not a digest")
        with self.assertRaises(OSError):
            VERIFY(self.apk, self.root / "absent", PIN)

if __name__ == "__main__":
    unittest.main()
