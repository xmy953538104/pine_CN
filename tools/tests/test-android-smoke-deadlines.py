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
            code, stderr = 0, ''
            def next_result(name, default):
                path = Path(__file__).with_suffix('.state.json')
                state = json.loads(path.read_text()) if path.is_file() else {}
                values = config.get(name, [default])
                index = state.get(name, 0)
                state[name] = index + 1
                path.write_text(json.dumps(state))
                return values[min(index, len(values) - 1)]

            if args == ['get-state']:
                operation, output = 'state', 'device'
            elif args == ['shell', 'getprop']:
                operation = 'properties'
                output = '[sys.boot_completed]: [1]\\n[ro.product.cpu.abilist]: [x86_64,arm64-v8a]'
            elif args == ['shell', 'pidof', 'system_server']:
                operation, output = 'pid', next_result('pid_sequence', '672')
            elif args == ['shell', 'cmd', 'package', 'path', 'android']:
                operation, output = 'package', next_result('package_sequence', 'package:/system/framework/framework-res.apk')
            elif args[:2] == ['shell', 'cat'] and args[2].startswith('/proc/'):
                operation, output = 'proc-status', 'Name:\\toffline-fixture'
                if config.get('status_denied'):
                    code, stderr, output = 1, 'cat: Permission denied', ''
            elif args[:3] == ['shell', 'uiautomator', 'dump']:
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
            if isinstance(output, dict):
                code, stderr = output.get('code', 0), output.get('stderr', '')
                output = output.get('stdout', '')
            def emit():
                if binary:
                    sys.stdout.buffer.write(output)
                    sys.stdout.buffer.flush()
                else:
                    print(output, flush=True)
                if stderr:
                    print(stderr, file=sys.stderr, flush=True)
            if config.get('emit_before_delay') == operation:
                emit()
                time.sleep(config.get(operation, 0))
            else:
                time.sleep(config.get(operation, 0))
                emit()
            raise SystemExit(code)
            '''))
        self.fake.chmod(0o755)

    def smoke(self, name, delays):
        self.config.write_text(json.dumps({"session": SESSION, **delays}))
        self.fake.with_suffix('.state.json').unlink(missing_ok=True)
        # Short fixture budgets keep these real subprocess tests fast while
        # leaving room for interpreter startup on both Linux and macOS.
        # Only helpers and the readiness gate run; APK checks remain unexecuted.
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

    def assert_gate_only(self, smoke, status):
        self.assertEqual(set(smoke.results), {"device_ready"})
        self.assertEqual(smoke.results["device_ready"]["status"], status)
        self.assertEqual(smoke.summary["overall"], "blocked")
        self.assertFalse(any("install" in row["argv"] for row in self.commands(smoke)))
        rows = [json.loads(line) for line in (smoke.output / "readiness-observations.jsonl").read_text().splitlines()]
        self.assertEqual(rows, smoke.summary["android_readiness"]["observations"])
        return rows

    def test_readiness_retries_missing_multiple_and_failed_package_observations(self):
        smoke = self.smoke("readiness-transient", {
            "pid_sequence": [{"stdout": "", "code": 1, "stderr": "pidof: no process"},
                             "672 1618", "672", "672", "672"],
            "package_sequence": [{"stdout": "package:/system/framework/framework-res.apk",
                                  "code": 1, "stderr": "package service unavailable"},
                                 "package:/system/framework/framework-res.apk"],
            "status_denied": True})
        smoke.args.boot_timeout = 12
        smoke.step("device_ready", smoke.device_ready)
        rows = self.assert_gate_only(smoke, "pass")
        self.assertEqual([row["status"] for row in rows], ["retry", "retry", "retry", "pass"])
        self.assertEqual(rows[0]["system_server_before_raw"], "\n")
        self.assertEqual(rows[1]["system_server_before_raw"], "672 1618\n")
        self.assertEqual(rows[1]["system_server_before_pids"], ["672", "1618"])
        self.assertTrue(any("Permission denied" in error for error in rows[1]["command_errors"].values()))
        self.assertIn("package service unavailable", rows[2]["command_errors"]["package-query"])
        self.assertEqual(rows[-1]["system_server_before_pids"], ["672"])
        self.assertEqual(rows[-1]["system_server_after_pids"], ["672"])
        self.assertGreaterEqual(rows[-1]["continuity_seconds"], 2)
        for row in rows:
            self.assertTrue(all((smoke.output / name).is_file() for name in row["evidence"]))

    def test_readiness_persistent_multiple_pids_blocks_at_total_deadline(self):
        smoke = self.smoke("readiness-persistent", {"pid_sequence": ["672 1618"]})
        smoke.args.boot_timeout = 1.6
        started = time.monotonic()
        smoke.step("device_ready", smoke.device_ready)
        elapsed = time.monotonic() - started
        rows = self.assert_gate_only(smoke, "blocked")
        self.assertGreaterEqual(elapsed, smoke.args.boot_timeout)
        self.assertLess(elapsed, smoke.args.boot_timeout + .8)
        self.assertEqual(rows[0]["system_server_before_pids"], ["672", "1618"])
        self.assertNotIn("accepted_attempt", smoke.summary["android_readiness"])
        self.assertIn("gate expired", smoke.results["device_ready"]["reason"])

    def test_readiness_retries_a_changed_pid_before_accepting_stability(self):
        smoke = self.smoke("readiness-changed", {"pid_sequence": ["672", "1618", "1618", "1618"]})
        smoke.args.boot_timeout = 12
        smoke.step("device_ready", smoke.device_ready)
        rows = self.assert_gate_only(smoke, "pass")
        self.assertEqual([row["status"] for row in rows], ["retry", "pass"])
        self.assertEqual(rows[0]["system_server_before_pids"], ["672"])
        self.assertEqual(rows[0]["system_server_after_pids"], ["1618"])
        self.assertIn("672 -> 1618", rows[0]["reason"])
        self.assertEqual(rows[-1]["system_server_before_pids"], ["1618"])
        self.assertEqual(rows[-1]["system_server_after_pids"], ["1618"])
        self.assertGreaterEqual(rows[-1]["continuity_seconds"], 2)


if __name__ == "__main__":
    unittest.main()
