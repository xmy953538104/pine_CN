#!/usr/bin/env python3
"""Offline subprocess deadline regressions; no APK, Android device or runtime pass."""
import argparse
import json
from pathlib import Path
import runpy
import tempfile
import textwrap
import time
import unittest


RUNNER = Path(__file__).resolve().parents[1] / "android-diagnostics-smoke.py"
MODULE = runpy.run_path(str(RUNNER))
Smoke, Blocked = MODULE["Smoke"], MODULE["Blocked"]
SESSION = "11111111-2222-3333-4444-555555555555"


class ObservationDeadlines(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.fake = self.root / "fake-adb"
        self.config = self.root / "fake-adb.json"
        self.fake.write_text(textwrap.dedent('''\
            #!/usr/bin/env python3
            import json
            from pathlib import Path
            import sys
            import time

            config = json.loads(Path(__file__).with_suffix('.json').read_text())
            assert sys.argv[1:3] == ['-s', 'offline-fixture']
            args = sys.argv[3:]
            binary = False
            if args[:3] == ['shell', 'uiautomator', 'dump']:
                operation, output = 'dump', 'UI hierarchy dumped'
            elif args[:2] == ['exec-out', 'cat']:
                operation = 'xml'
                output = '<hierarchy><node text="Ready" enabled="true" scrollable="true" bounds="[0,0][100,100]"/></hierarchy>'
            elif args[:3] == ['shell', 'input', 'swipe']:
                operation, output = 'swipe', ''
            elif args[:2] == ['exec-out', 'screencap']:
                operation, output, binary = 'screenshot', b'\\x89PNG\\r\\n\\x1a\\n', True
            elif args[:2] == ['exec-out', 'run-as'] and args[3] == 'ls':
                operation, output = 'list', 'diagnostics-0.jsonl\\ndiagnostics-1.jsonl'
            elif args[:2] == ['exec-out', 'run-as'] and args[4].endswith('session-id.txt'):
                operation, output = 'marker', config['session']
            elif args[:2] == ['exec-out', 'run-as'] and args[3] == 'cat':
                operation = 'record0' if args[4].endswith('diagnostics-0.jsonl') else 'record1'
                output = json.dumps({'schema': 1, 'session_id': config['session'], 'sequence': 1,
                                     'fields': {}, 'event': 'SESSION_START' if operation == 'record0' else 'SESSION_METADATA'})
            else:
                raise SystemExit('Unexpected offline fixture command: ' + repr(args))
            def emit():
                if binary:
                    sys.stdout.buffer.write(output)
                    sys.stdout.buffer.flush()
                else:
                    print(output, flush=True)
            if config.get('emit_before_delay') == operation:
                emit()
                time.sleep(config.get(operation, 0))
            else:
                time.sleep(config.get(operation, 0))
                emit()
            '''))
        self.fake.chmod(0o755)

    def smoke(self, name, delays):
        self.config.write_text(json.dumps({"session": SESSION, **delays}))
        # Short fixture budgets keep these real subprocess tests fast while
        # leaving room for interpreter startup on both Linux and macOS.
        # Only observation helpers run; none of the 18 Android checks is passed.
        return Smoke(argparse.Namespace(
            adb=str(self.fake), serial="offline-fixture", apk=self.root / "absent.apk",
            expected_commit="a" * 40, output=self.root / name,
            command_timeout=2.5, ui_timeout=1, boot_timeout=2.5, install_timeout=2.5))

    def commands(self, smoke):
        return [json.loads(line) for line in (smoke.output / "commands.jsonl").read_text().splitlines()]

    def test_late_ui_marker_and_native_observations_cannot_pass(self):
        scenarios = (
            ("ui", {"dump": .2, "xml": 1.1},
             lambda smoke: smoke.wait_ui("late", lambda screen: screen.find("Ready") is not None)),
            ("marker", {"marker": 1.3, "emit_before_delay": "marker"}, lambda smoke: smoke.marker()),
            ("native", {"list": .1, "record0": .1, "record1": 1.1},
             lambda smoke: smoke.wait_native("late", "SESSION_METADATA", SESSION)),
        )
        for name, delays, observe in scenarios:
            with self.subTest(observation=name):
                smoke = self.smoke(name, delays)
                started = time.monotonic()
                with self.assertRaises(Blocked):
                    observe(smoke)
                self.assertLess(time.monotonic() - started, 1.8)
                self.assertTrue(any(row["timed_out"] for row in self.commands(smoke)))
                self.assertIsNone(smoke.session)

    def test_seek_gesture_uses_the_remaining_observation_budget(self):
        smoke = self.smoke("seek", {"dump": .1, "xml": .1, "swipe": 2.4})
        started = time.monotonic()
        with self.assertRaises(Blocked):
            smoke.seek("Missing control", "seek")
        self.assertLess(time.monotonic() - started, 1.8)
        self.assertTrue(any(row["timed_out"] and "swipe" in row["argv"]
                            for row in self.commands(smoke)))

    def test_slow_diagnostic_capture_preserves_observed_success_and_budget(self):
        smoke = self.smoke("capture", {"screenshot": 4})
        started = time.monotonic()
        screen = smoke.wait_ui("ready", lambda value: value.find("Ready") is not None)
        self.assertLess(time.monotonic() - started, 1.8)
        self.assertIsNotNone(screen.find("Ready"))
        self.assertTrue(smoke.summary["capture_warnings"])
        self.assertTrue(any(row["timed_out"] and "screencap" in row["argv"]
                            for row in self.commands(smoke)))


if __name__ == "__main__":
    unittest.main()
