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
READY_XML = '<hierarchy><node text="Ready" enabled="true" bounds="[0,0][100,100]"/></hierarchy>'
PLATFORM_ANR_XML = '''<hierarchy><node package="android" bounds="[48,313][431,510]">
<node text="System UI isn't responding" resource-id="android:id/alertTitle" package="android"
 class="android.widget.TextView" enabled="true" bounds="[88,335][391,363]"/>
<node text="Wait" resource-id="android:id/aerr_wait" package="android"
 class="android.widget.Button" enabled="true" clickable="true" bounds="[64,438][415,486]"/>
</node></hierarchy>'''
PINE_ANR_XML = PLATFORM_ANR_XML.replace("System UI isn't responding", "Pine Diagnostics isn't responding")


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
            elif args == ['shell', 'pidof', 'io.github.pine.emu.diagnostics']:
                operation, output = 'app-pids', config.get('app_pids', '1234')
            elif args == ['shell', 'dumpsys', 'activity', 'lastanr']:
                operation, output = 'lastanr', config.get('lastanr_result', 'ANR in io.github.pine.emu.diagnostics')
            elif args == ['shell', 'dumpsys', 'activity', 'lastanr-traces']:
                operation, output = 'lastanr-traces', config.get('lastanr_traces_result', 'Global fixture ANR traces')
            elif args == ['shell', 'dumpsys', 'activity', 'activities']:
                operation, output = 'activities', 'Final activity evidence'
            elif args[:2] == ['logcat', '-b']:
                operation, output = 'final-logcat', 'Final crash evidence'
            elif args[:3] == ['shell', 'rm', '-f']:
                operation, output = 'remove-xml', ''
            elif args == ['shell', 'cmd', 'package', 'path', 'android']:
                operation, output = 'package', next_result('package_sequence', 'package:/system/framework/framework-res.apk')
            elif args[:2] == ['shell', 'cat'] and args[2].startswith('/proc/'):
                operation, output = 'proc-status', 'Name:\\toffline-fixture'
                if config.get('status_denied'):
                    code, stderr, output = 1, 'cat: Permission denied', ''
            elif args[:3] == ['shell', 'uiautomator', 'dump']:
                operation, output = 'dump', 'UI hierarchy dumped'
            elif args[:3] == ['shell', 'am', 'start']:
                operation, output = 'start-settings', 'Status: ok'
            elif args[:2] == ['exec-out', 'cat']:
                operation = 'xml'
                output = next_result('xml_sequence', '<hierarchy><node text="Ready" enabled="true" scrollable="true" bounds="[0,0][100,100]"/></hierarchy>')
                if config.get('shared_storage_unavailable') and args[2].startswith('/sdcard/'):
                    # Android exec-out can report success while cat's error is
                    # returned as stdout. This must never become a UI success.
                    output = 'cat: ' + args[2] + ': Transport endpoint is not connected'
            elif args[:3] == ['shell', 'input', 'swipe']:
                operation, output = 'swipe', ''
            elif args[:3] == ['shell', 'input', 'tap']:
                operation, output = 'tap', ''
            elif args[:2] == ['exec-out', 'screencap']:
                operation, output, binary = 'screenshot', b'\\x89PNG\\r\\n\\x1a\\n', True
            elif (args[:4] == ['exec-out', 'run-as', 'io.github.pine.emu.diagnostics', 'cat']
                  and args[4].startswith('/proc/') and args[4].endswith('/maps')):
                operation, output = 'maps', config.get('maps_result', 'Fixture library mappings')
            elif args[:5] == ['exec-out', 'run-as', 'io.github.pine.emu.diagnostics', '/system/bin/debuggerd', '-b']:
                operation, output = 'backtrace', config.get('backtrace_result', 'Fixture native backtrace')
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

    def smoke(self, name, delays, allow_platform_anr=False, collect_system_anr=False):
        self.config.write_text(json.dumps({"session": SESSION, **delays}))
        self.fake.with_suffix('.state.json').unlink(missing_ok=True)
        # Short fixture budgets keep these real subprocess tests fast while
        # leaving room for interpreter startup on both Linux and macOS.
        # Only helpers and the readiness gate run; APK checks remain unexecuted.
        return Smoke(argparse.Namespace(
            adb=str(self.fake), serial="offline-fixture", apk=self.root / "absent.apk",
            expected_commit="a" * 40, output=self.root / name,
            command_timeout=2.5, ui_timeout=1, boot_timeout=2.5, install_timeout=2.5,
            wait_known_platform_anr=allow_platform_anr, collect_system_last_anr_traces=collect_system_anr))

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

    def test_ui_hierarchy_survives_unavailable_shared_storage_without_faking_xml(self):
        smoke = self.smoke("shell-storage", {"shared_storage_unavailable": True})
        screen = smoke.screen("shell-storage", deadline=time.monotonic() + 1)
        self.assertIsNotNone(screen.find("Ready"))
        self.assertIsNone(smoke.session)
        self.assertEqual(smoke.results, {})
        # Reproduce the observed old-path failure with the same real subprocess
        # fixture; a zero exit code and saved .xml filename are insufficient.
        smoke.remote_xml = "/sdcard/" + Path(smoke.remote_xml).name
        with self.assertRaisesRegex(Blocked, "did not provide parseable XML"):
            smoke.screen("broken-fuse", deadline=time.monotonic() + 1)
        self.assertIn("Transport endpoint is not connected",
                      (smoke.output / "002-broken-fuse.xml").read_text())
        self.assertEqual(smoke.results, {})

    def test_opt_in_platform_wait_requires_a_fresh_observation_after_the_tap(self):
        smoke = self.smoke("platform-recovery", {"xml_sequence": [PLATFORM_ANR_XML, READY_XML]},
                           allow_platform_anr=True)
        smoke.args.ui_timeout = 3
        screen = smoke.wait_ui("recovery", lambda value: value.find("Ready") is not None)
        self.assertEqual(screen.number, 2)
        attempts = smoke.summary["platform_anr_waits"]
        self.assertEqual(len(attempts), 1)
        self.assertEqual(attempts[0]["status"], "tap_sent")
        self.assertEqual(attempts[0]["source_xml"], "001-recovery.xml")
        taps = [row for row in self.commands(smoke) if "tap" in row["argv"]]
        self.assertEqual(len(taps), 1)
        self.assertEqual(taps[0]["argv"][-2:], ["239", "462"])
        # Even successful helper recovery is no APK/native/export check pass.
        self.assertEqual(smoke.results, {})
        self.assertEqual(smoke.summary["overall"], "blocked")

    def test_platform_wait_never_handles_default_pine_unknown_or_wrong_controls(self):
        scenarios = (
            ("default", PLATFORM_ANR_XML, False),
            ("pine", PLATFORM_ANR_XML.replace("System UI", "Pine Diagnostics"), True),
            ("unknown", PLATFORM_ANR_XML.replace("System UI", "Fixture App"), True),
            ("wrong-package", PLATFORM_ANR_XML.replace('package="android"', 'package="fixture.other"'), True),
            ("prefixed-resource", PLATFORM_ANR_XML.replace('resource-id="android:', 'resource-id="fixture:android:'), True),
            ("close", PLATFORM_ANR_XML.replace("android:id/aerr_wait", "android:id/aerr_close"), True),
            ("not-clickable", PLATFORM_ANR_XML.replace('clickable="true"', 'clickable="false"'), True),
            ("disabled", PLATFORM_ANR_XML.replace('enabled="true"', 'enabled="false"'), True),
            ("missing-enabled", PLATFORM_ANR_XML.replace('enabled="true"', ''), True),
        )
        for name, xml, enabled in scenarios:
            with self.subTest(case=name):
                smoke = self.smoke(name, {"xml_sequence": [xml]}, allow_platform_anr=enabled)
                screen = smoke.screen(name, deadline=time.monotonic() + 1)
                self.assertFalse(smoke.wait_known_platform_dialog(screen, time.monotonic() + 1))
                self.assertFalse(any("tap" in row["argv"] for row in self.commands(smoke)))
                self.assertEqual(smoke.summary["platform_anr_waits"], [])

    def test_repeated_platform_anr_is_still_blocked_after_two_waits(self):
        smoke = self.smoke("platform-persistent", {"xml_sequence": [PLATFORM_ANR_XML]},
                           allow_platform_anr=True)
        smoke.args.ui_timeout = 3
        started = time.monotonic()
        with self.assertRaises(Blocked):
            smoke.wait_ui("persistent", lambda value: value.find("Ready") is not None)
        self.assertLess(time.monotonic() - started, 3.8)
        self.assertEqual(len([row for row in self.commands(smoke) if "tap" in row["argv"]]), 2)
        self.assertEqual(len(smoke.summary["platform_anr_waits"]), 2)
        self.assertEqual(smoke.results, {})
        self.assertEqual(smoke.summary["overall"], "blocked")

    def test_platform_wait_command_cannot_extend_the_original_ui_deadline(self):
        smoke = self.smoke("platform-slow-tap", {"xml_sequence": [PLATFORM_ANR_XML], "tap": 2.5},
                           allow_platform_anr=True)
        started = time.monotonic()
        with self.assertRaises(Blocked):
            smoke.wait_ui("slow-tap", lambda value: value.find("Ready") is not None)
        self.assertLess(time.monotonic() - started, 1.8)
        self.assertTrue(any(row["timed_out"] and "tap" in row["argv"] for row in self.commands(smoke)))
        self.assertEqual(smoke.summary["platform_anr_waits"][0]["status"], "blocked")

    def test_pine_anr_fails_ui_steps_before_predicates_and_never_taps_wait(self):
        for enabled in (False, True):
            for observer in ("wait_ui", "seek"):
                with self.subTest(platform_wait=enabled, observer=observer):
                    smoke = self.smoke(f"pine-anr-{enabled}-{observer}", {"xml_sequence": [PINE_ANR_XML]},
                                       allow_platform_anr=enabled)
                    if observer == "wait_ui":
                        observe = lambda: smoke.wait_ui("pine-anr", lambda value: True)
                    else:
                        observe = lambda: smoke.seek("Wait", "pine-anr", exact=True)
                    smoke.step("settings_entry", observe)
                    self.assertEqual(smoke.results["settings_entry"]["status"], "fail")
                    self.assertIn("Pine application ANR observed", smoke.results["settings_entry"]["reason"])
                    self.assertEqual(smoke.summary["overall"], "fail")
                    self.assertIn("Pine Diagnostics isn't responding", (smoke.output / "001-pine-anr.xml").read_text())
                    self.assertFalse(any("tap" in row["argv"] for row in self.commands(smoke)))
                    self.assertEqual(smoke.summary["platform_anr_waits"], [])

    def test_disable_cleanup_cannot_swallow_an_observed_pine_anr(self):
        smoke = self.smoke("disable-anr", {"xml_sequence": [PINE_ANR_XML]}, allow_platform_anr=True)
        smoke.ever_enabled = True
        smoke.step("disable_stop", smoke.disable)
        self.assertEqual(smoke.results["disable_stop"]["status"], "fail")
        self.assertIn("Pine application ANR observed", smoke.results["disable_stop"]["reason"])
        self.assertEqual(smoke.summary["overall"], "fail")
        self.assertFalse(any("input" in row["argv"] for row in self.commands(smoke)))
        self.assertIn("Pine Diagnostics isn't responding", (smoke.output / "001-settings.xml").read_text())

    def test_pine_anr_identity_does_not_match_other_titles_or_application_text(self):
        for name, xml in (
            ("other-title", PINE_ANR_XML.replace("Pine Diagnostics", "Fixture App")),
            ("application-text", PINE_ANR_XML.replace('package="android"', 'package="io.github.pine.emu.diagnostics"')),
            ("prefixed-resource", PINE_ANR_XML.replace('resource-id="android:id/alertTitle"',
                                                     'resource-id="fixture:android:id/alertTitle"')),
        ):
            with self.subTest(case=name):
                smoke = self.smoke(name, {"xml_sequence": [xml]})
                smoke.step("settings_entry", lambda: {"xml": smoke.wait_ui(name, lambda value: True).xml.name})
                self.assertEqual(smoke.results["settings_entry"]["status"], "pass")
                self.assertEqual(smoke.summary["overall"], "blocked")

    def test_failed_run_collects_bounded_process_evidence_without_overwriting_anr(self):
        smoke = self.smoke("failure-evidence", {
            "xml_sequence": [PINE_ANR_XML], "app_pids": "1234 5678 91011",
            "lastanr_result": {"code": 1, "stderr": "lastanr: denied"},
            "maps_result": {"code": 1, "stderr": "maps: Permission denied"},
            "backtrace_result": {"stdout": "Partial native frames", "stderr": "debuggerd: fixture waiting"},
            "backtrace": 2, "emit_before_delay": "backtrace",
        })
        smoke.step("settings_entry", lambda: smoke.wait_ui("pine-anr", lambda value: True))
        before = json.loads(json.dumps(smoke.results))
        smoke.installed = True
        smoke.args.command_timeout = .6
        started = time.monotonic()
        smoke.finish()
        self.assertLess(time.monotonic() - started, 3)
        self.assertEqual(smoke.results, before)
        self.assertEqual(smoke.summary["overall"], "fail")
        self.assertEqual(json.loads((smoke.output / "runtime-report.json").read_text())["status"], "failed")
        commands = self.commands(smoke)
        backtraces = [row for row in commands if "/system/bin/debuggerd" in row["argv"]]
        self.assertEqual([row["argv"][-1] for row in backtraces], ["1234", "5678"])
        self.assertTrue(all(row["timed_out"] for row in backtraces))
        self.assertEqual((smoke.output / "last-anr.stderr.txt").read_text(), "lastanr: denied\n")
        for pid in ("1234", "5678"):
            self.assertEqual((smoke.output / f"pine-maps-{pid}.stderr.txt").read_text(), "maps: Permission denied\n")
            self.assertIn("Partial native frames", (smoke.output / f"pine-native-backtrace-{pid}.txt").read_text())
            self.assertIn("fixture waiting", (smoke.output / f"pine-native-backtrace-{pid}.stderr.txt").read_text())
        self.assertEqual((smoke.output / "pine-process-ids.txt").read_text().strip(), "1234 5678 91011")
        self.assertTrue((smoke.output / "final-activities.txt").is_file())
        self.assertTrue((smoke.output / "crash-buffer.txt").is_file())
        self.assertTrue(any(row["argv"][-3:] == ["rm", "-f", smoke.remote_xml] for row in commands))

    def test_failure_process_probes_reject_untrusted_pids_and_skip_verified_runs(self):
        for name, pids in (("mixed", "1234 not-a-pid"), ("zero", "0"), ("unicode", "１２３４")):
            with self.subTest(case=name):
                smoke = self.smoke(name, {"app_pids": pids})
                smoke.installed = True
                smoke.finish()
                self.assertFalse(any("run-as" in row["argv"] for row in self.commands(smoke)))
                self.assertEqual(smoke.summary["overall"], "blocked")
        smoke = self.smoke("verified", {}, collect_system_anr=True)
        smoke.installed = True
        smoke.results.update({name: {"status": "pass"} for name in MODULE["REQUIRED"]})
        smoke.write_summary()
        smoke.finish()
        self.assertFalse(any("pidof" in row["argv"] or "lastanr" in row["argv"] or "lastanr-traces" in row["argv"] or
                             "run-as" in row["argv"] for row in self.commands(smoke)))
        self.assertEqual(smoke.summary["overall"], "pass")

    def test_system_anr_traces_are_opt_in_and_capture_errors_preserve_failure_and_cleanup(self):
        for name, enabled, config in (
            ("default", False, {}),
            ("denied", True, {"lastanr_traces_result": {"code": 1, "stderr": "ANR trace permission denied"}}),
            ("timeout", True, {"lastanr_traces_result": {"stdout": "Partial global trace", "stderr": "Trace still being read"},
                               "lastanr-traces": 2, "emit_before_delay": "lastanr-traces"}),
        ):
            with self.subTest(case=name):
                smoke = self.smoke("system-traces-" + name, {"xml_sequence": [PINE_ANR_XML], "app_pids": "", **config},
                                   collect_system_anr=enabled)
                smoke.step("settings_entry", lambda: smoke.wait_ui("pine-anr", lambda value: True))
                before = json.loads(json.dumps(smoke.results))
                smoke.installed = True
                smoke.args.command_timeout = .6
                smoke.finish()
                self.assertEqual(smoke.results, before)
                report = json.loads((smoke.output / "runtime-report.json").read_text())
                self.assertEqual(report["status"], "failed")
                self.assertEqual(report["system_last_anr_traces_enabled"], enabled)
                self.assertIn("not filtered to Pine", report["system_last_anr_traces_scope"])
                commands = self.commands(smoke)
                traces = [row for row in commands if "lastanr-traces" in row["argv"]]
                self.assertEqual(len(traces), int(enabled))
                self.assertEqual((smoke.output / "last-anr-traces.txt").exists(), enabled)
                if name == "denied":
                    self.assertEqual(traces[0]["returncode"], 1)
                    self.assertIn("permission denied", (smoke.output / "last-anr-traces.stderr.txt").read_text())
                if name == "timeout":
                    self.assertTrue(traces[0]["timed_out"])
                    self.assertIn("Partial global trace", (smoke.output / "last-anr-traces.txt").read_text())
                    self.assertIn("still being read", (smoke.output / "last-anr-traces.stderr.txt").read_text())
                self.assertTrue((smoke.output / "final-activities.txt").is_file())
                self.assertTrue(any(row["argv"][-3:] == ["rm", "-f", smoke.remote_xml] for row in commands))

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
