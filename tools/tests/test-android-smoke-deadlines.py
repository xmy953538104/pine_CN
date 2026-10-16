#!/usr/bin/env python3
"""Offline subprocess deadline regressions; no APK, Android device or runtime pass."""
import argparse
import base64
import io
import zipfile
import json
from pathlib import Path
import runpy
import tempfile
import textwrap
import time
import unittest
import xml.etree.ElementTree as ET


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
BLUETOOTH_CRASH_XML = '''<hierarchy><node package="android" class="android.widget.FrameLayout" bounds="[48,301][431,498]">
<node text="Bluetooth keeps stopping" resource-id="android:id/alertTitle" class="android.widget.TextView"
 package="android" enabled="true" bounds="[88,335][391,363]"/>
<node text="App info" resource-id="android:id/aerr_app_info" class="android.widget.Button"
 package="android" enabled="true" clickable="true" bounds="[64,378][415,426]"/>
<node text="Close app" resource-id="android:id/aerr_close" class="android.widget.Button"
 package="android" enabled="true" clickable="true" bounds="[64,426][415,474]"/>
</node></hierarchy>'''


def ui_xml(*nodes):
    root = ET.Element("hierarchy")
    for node in nodes:
        ET.SubElement(root, "node", {"package": MODULE["PACKAGE"], "class": "android.widget.TextView",
                                     "enabled": "true", "bounds": "[20,30][220,130]", **node})
    return ET.tostring(root, encoding="unicode")


def diagnostics_xml(enabled=True, package=None):
    package = package or MODULE["PACKAGE"]
    checked = "true" if enabled else "false"
    return ui_xml({"package": package, "class": "android.widget.Switch", "checkable": "true",
                   "checked": checked, "clickable": "true", "text": "Enable diagnostics / 启用诊断 (default off)"},
                  {"package": package, "text": f"Status process: main\nSession: {SESSION}\nSink active: {checked}\nWritten: 2, dropped: 0, disk errors: 0"})


SEARCH_LOCATIONS_XML = '''<hierarchy>
<node resource-id="io.github.pine.emu.diagnostics:id/toolbar_layout" package="io.github.pine.emu.diagnostics"
 class="android.widget.FrameLayout" content-desc="Search Locations" enabled="true" bounds="[0,0][480,136]">
 <node package="io.github.pine.emu.diagnostics" class="android.widget.ImageButton" content-desc="Navigate up"
  clickable="true" enabled="true" bounds="[0,28][56,84]"/>
</node>
<node resource-id="io.github.pine.emu.diagnostics:id/locations_list" package="io.github.pine.emu.diagnostics"
 class="androidx.recyclerview.widget.RecyclerView" enabled="true" bounds="[0,136][480,800]"/>
<node text="Add" resource-id="io.github.pine.emu.diagnostics:id/add_location_button" package="io.github.pine.emu.diagnostics"
 class="android.widget.Button" enabled="true" bounds="[344,704][464,760]"/>
</hierarchy>'''


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
            import base64
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
            elif args == ['shell', 'dumpsys', 'dropbox', '--print', 'data_app_anr']:
                operation, output = 'dropbox', config.get('dropbox_result', 'Retained data_app_anr fixture entry')
            elif args == ['shell', 'dumpsys', 'activity', 'activities']:
                operation, output = 'activities', 'Final activity evidence'
            elif args == ['logcat', '-d', '-v', 'raw', 'PineDiag:V', '*:S']:
                operation, output = 'pine-logcat', json.dumps({'schema':1,'session_id':config['session'],'sequence':1,'event':'SESSION_METADATA','fields':{'commit':'a'*40}})
            elif args[:2] == ['logcat', '-b']:
                operation, output = 'final-logcat', 'Final crash evidence'
            elif args[:3] == ['shell', 'rm', '-f']:
                operation, output = 'remove-xml', ''
            elif args == ['shell', 'cmd', 'package', 'path', 'android']:
                operation, output = 'package', next_result('package_sequence', 'package:/system/framework/framework-res.apk')
            elif args == ['shell', 'sm', 'list-volumes', 'all']:
                operation, output = 'storage', next_result('storage_sequence', 'private mounted null')
            elif args[:2] == ['shell', 'cat'] and args[2].startswith('/proc/'):
                operation, output = 'proc-status', 'Name:\\toffline-fixture'
                if config.get('status_denied'):
                    code, stderr, output = 1, 'cat: Permission denied', ''
            elif args[:3] == ['shell', 'uiautomator', 'dump']:
                operation, output = 'dump', 'UI hierarchy dumped'
            elif args[:3] == ['shell', 'am', 'start']:
                operation, output = 'start-settings', 'Status: ok'
            elif args[:2] == ['exec-out', 'cat'] and args[2].startswith('/sdcard/') and 'zip_base64' in config:
                operation, output, binary = 'safzip', base64.b64decode(config['zip_base64']), True
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
                if config.get('cleanup_simulation'):
                    path = Path(__file__).with_suffix('.state.json')
                    state = json.loads(path.read_text()) if path.is_file() else {}
                    state['cleanup_tapped'] = True
                    path.write_text(json.dumps(state))
            elif args == ['shell', 'input', 'keyevent', 'KEYCODE_BACK']:
                operation, output = 'back', ''
            elif args[:2] == ['exec-out', 'screencap']:
                operation, output, binary = 'screenshot', b'\\x89PNG\\r\\n\\x1a\\n', True
            elif (args[:4] == ['exec-out', 'run-as', 'io.github.pine.emu.diagnostics', 'cat']
                  and args[4].startswith('/proc/') and args[4].endswith('/maps')):
                operation, output = 'maps', config.get('maps_result', 'Fixture library mappings')
            elif args[:5] == ['exec-out', 'run-as', 'io.github.pine.emu.diagnostics', '/system/bin/debuggerd', '-b']:
                operation, output = 'backtrace', config.get('backtrace_result', 'Fixture native backtrace')
            elif args[:2] == ['exec-out', 'run-as'] and args[3] == 'ls':
                operation = 'list'
                if args[-1] == 'files/diagnostics/main':
                    names = ['diagnostics-0.jsonl', 'diagnostics-1.jsonl', 'session-id.txt']
                elif args[-1] == 'cache/diagnostics-exports':
                    names = ['snapshot-' + config['session'], 'not-a-snapshot.txt']
                else:
                    raise SystemExit('Unexpected list directory: ' + repr(args))
                # Reproduce the actual adb default: success with multiple names
                # on one line. Only explicit ls -1 produces machine-safe lines.
                output = ('\\n' if args[4:-1] == ['-1'] else '  ').join(names)
            elif args[:2] == ['exec-out', 'run-as'] and args[4].endswith('session-id.txt'):
                operation, output = 'marker', config['session']
            elif args[:4] == ['exec-out', 'run-as', 'io.github.pine.emu.diagnostics', 'cat'] and args[4] == 'shared_prefs/diagnostics.xml':
                operation, output = 'prefs', config.get('prefs_xml', '<map><string name="files">2</string><string name="categories">content,storage,exception</string></map>')
            elif args[:4] == ['exec-out', 'run-as', 'io.github.pine.emu.diagnostics', 'cat'] and args[4].startswith('cache/diagnostics-exports/'):
                operation, binary = 'snapshot', True
                name = args[4].split('/', 3)[3]
                output = config['snapshot_files'][name].encode()
            elif args[:2] == ['exec-out', 'run-as'] and args[3] == 'cat':
                operation = 'record0' if args[4].endswith('diagnostics-0.jsonl') else 'record1'
                events = ['SESSION_START'] if operation == 'record0' else config.get('record1_events', ['SESSION_METADATA'])
                if config.get('cleanup_simulation'):
                    path = Path(__file__).with_suffix('.state.json')
                    state = json.loads(path.read_text()) if path.is_file() else {}
                    if not state.get('cleanup_tapped'): events = [event for event in events if event != 'SESSION_SUMMARY']
                output = '\\n'.join(json.dumps({'schema': 1, 'session_id': config['session'], 'sequence': index + 1,
                                                'fields': {'commit': 'a' * 40, 'disk_errors': '0'}, 'event': event})
                                   for index, event in enumerate(events))
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

    def smoke(self, name, delays, allow_platform_anr=False, collect_system_anr=False, require_storage=False,
              dismiss_platform_crash=False):
        self.config.write_text(json.dumps({"session": SESSION, **delays}))
        self.fake.with_suffix('.state.json').unlink(missing_ok=True)
        # Short fixture budgets keep these real subprocess tests fast while
        # leaving room for interpreter startup on both Linux and macOS.
        # Only helpers and the readiness gate run; APK checks remain unexecuted.
        return Smoke(argparse.Namespace(
            adb=str(self.fake), serial="offline-fixture", apk=self.root / "absent.apk",
            expected_commit="a" * 40, output=self.root / name,
            command_timeout=2.5, ui_timeout=1, boot_timeout=2.5, install_timeout=2.5,
            wait_known_platform_anr=allow_platform_anr, collect_system_last_anr_traces=collect_system_anr,
            require_ready_storage=require_storage, dismiss_known_platform_crash=dismiss_platform_crash))

    def commands(self, smoke):
        return [json.loads(line) for line in (smoke.output / "commands.jsonl").read_text().splitlines()]

    def cleanup_fixture(self, name, **overrides):
        config = {"cleanup_simulation": True, "xml_sequence": [diagnostics_xml(), diagnostics_xml(), diagnostics_xml(False)],
                  "record1_events": ["SESSION_METADATA", "SESSION_SUMMARY"],
                  "prefs_xml": '<map><boolean name="enabled" value="false"/></map>', **overrides}
        smoke = self.smoke(name, config)
        smoke.ever_enabled = True
        smoke.session = SESSION
        return smoke

    def test_disable_already_open_diagnostics_reads_actual_native_stop(self):
        smoke = self.cleanup_fixture("cleanup-visible")
        result = smoke.disable()
        self.assertEqual(result["stop_event"], "SESSION_SUMMARY")
        self.assertEqual(result["session_id"], SESSION)
        self.assertTrue(result["summaries"])
        commands = self.commands(smoke)
        self.assertEqual(sum("tap" in row["argv"] for row in commands), 1)
        self.assertTrue((smoke.output / "disabled-main-diagnostics-1.jsonl").is_file())

    def test_disable_from_real_settings_keeps_preference_entry_path(self):
        settings = ui_xml({"text": "Developer diagnostics / 开发者诊断"})
        smoke = self.cleanup_fixture("cleanup-settings", xml_sequence=[settings, settings, diagnostics_xml(),
                                                                        diagnostics_xml(), diagnostics_xml(False)])
        result = smoke.disable()
        self.assertEqual(result["stop_event"], "SESSION_SUMMARY")
        self.assertEqual(sum("tap" in row["argv"] for row in self.commands(smoke)), 2)

    def test_initial_settings_entry_cannot_accept_cleanup_shortcut(self):
        smoke = self.cleanup_fixture("initial-strict", xml_sequence=[diagnostics_xml()])
        with self.assertRaises(Blocked):
            smoke.open_settings()
        self.assertFalse(any("tap" in row["argv"] for row in self.commands(smoke)))

    def test_cleanup_rejects_foreign_unknown_saf_and_app_anr(self):
        for name, xml in (("foreign", diagnostics_xml(package="foreign.app")), ("unknown", READY_XML),
                          ("saf", diagnostics_xml(package="com.google.android.documentsui")),
                          ("anr", PINE_ANR_XML)):
            with self.subTest(name=name):
                smoke = self.cleanup_fixture("cleanup-" + name, xml_sequence=[xml])
                smoke.args.ui_timeout = 0.35
                expected = MODULE["AppAnr"] if name == "anr" else Blocked
                with self.assertRaises(expected):
                    smoke.disable()
                self.assertFalse(any("tap" in row["argv"] for row in self.commands(smoke)))

    def test_cleanup_without_native_stop_summary_remains_blocked(self):
        smoke = self.cleanup_fixture("cleanup-no-summary", record1_events=["SESSION_METADATA"])
        with self.assertRaises(Blocked):
            smoke.disable()

    def saf_fixture(self, name, filename=None, provider="com.google.android.documentsui", **overrides):
        filename = filename or "pine-diag-" + SESSION + ".zip"
        picker = ui_xml({"package": provider, "text": "Downloads"},
                        {"package": provider, "text": filename, "class": "android.widget.EditText", "resource-id": "android:id/title"},
                        {"package": provider, "text": "SAVE", "class": "android.widget.Button", "resource-id": "android:id/button1", "clickable": "true"})
        files = {"session-metadata.txt": b"fixture metadata\n", "main/diagnostics-0.jsonl":
                 (json.dumps({"schema": 1, "session_id": SESSION, "sequence": 1, "event": "SESSION_METADATA", "fields": {"commit": "a" * 40}}) + "\n").encode()}
        archive = io.BytesIO()
        with zipfile.ZipFile(archive, "w") as zipped:
            for entry, data in files.items():
                zipped.writestr(entry, data)
        config = {"xml_sequence": [ui_xml({"text": "EXPORT", "class": "android.widget.Button"}), picker, picker, diagnostics_xml()],
                  "zip_base64": base64.b64encode(archive.getvalue()).decode(), **overrides}
        smoke = self.smoke(name, config)
        smoke.session = SESSION
        smoke.snapshot_bytes = files
        smoke.preview_entries = {entry: len(data) for entry, data in files.items()}
        return smoke

    def test_saf_preserves_observed_default_filename_and_verifies_saved_zip(self):
        smoke = self.saf_fixture("saf-default")
        saved = smoke.saf_save()
        self.assertEqual(saved["observed_filename"], "pine-diag-" + SESSION + ".zip")
        self.assertTrue(smoke.verify_zip()["preview_bytes_match"])
        commands = self.commands(smoke)
        self.assertFalse(any("KEYCODE_DEL" in row["argv"] or "KEYCODE_MOVE_END" in row["argv"] or
                             row["argv"][-2:-1] == ["text"] for row in commands))
        self.assertEqual(sum("tap" in row["argv"] for row in commands), 3)

    def test_saf_wrong_session_or_path_and_foreign_provider_are_blocked(self):
        cases = (("session", {"filename": "pine-diag-" + "2" * 36 + ".zip"}),
                 ("path", {"filename": "../pine-diag-" + SESSION + ".zip"}),
                 ("provider", {"provider": "untrusted.documentsui"}))
        for name, options in cases:
            with self.subTest(name=name):
                smoke = self.saf_fixture("saf-reject-" + name, **options)
                smoke.args.ui_timeout = 0.5
                with self.assertRaises(Blocked):
                    smoke.saf_save()
                self.assertFalse(any("/sdcard/Download/" in " ".join(row["argv"]) for row in self.commands(smoke)))
                self.assertIsNone(smoke.saved_name)

    def test_saf_foreign_save_control_is_not_tapped(self):
        smoke = self.saf_fixture("saf-foreign-save")
        config = json.loads(self.config.read_text())
        for index in (1, 2):
            root = ET.fromstring(config["xml_sequence"][index])
            for node in root.iter("node"):
                if node.get("text") == "SAVE":
                    node.set("package", "foreign.app")
            config["xml_sequence"][index] = ET.tostring(root, encoding="unicode")
        self.config.write_text(json.dumps(config))
        with self.assertRaises(Blocked):
            smoke.saf_save()
        self.assertEqual(sum("tap" in row["argv"] for row in self.commands(smoke)), 2)

    def test_saf_valid_zip_bytes_after_deadline_do_not_pass(self):
        smoke = self.saf_fixture("saf-late", safzip=0.8, emit_before_delay="safzip")
        smoke.args.ui_timeout = 0.5
        with self.assertRaises(Blocked):
            smoke.saf_save()
        self.assertTrue(any(row["timed_out"] and "/sdcard/Download/" in " ".join(row["argv"]) for row in self.commands(smoke)))

    def test_explicit_single_column_listing_reads_actual_native_metadata_and_summary(self):
        smoke = self.smoke("native-columns", {"record1_events": ["SESSION_METADATA", "SESSION_SUMMARY"]})
        raw = smoke.adb("exec-out", "run-as", MODULE["PACKAGE"], "ls", "files/diagnostics/main")
        self.assertEqual(raw, b"diagnostics-0.jsonl  diagnostics-1.jsonl  session-id.txt\n")
        smoke.session = SESSION
        smoke.step("native_jsonl", smoke.verify_native)
        self.assertEqual(smoke.results["native_jsonl"]["status"], "pass")
        rows, names, summaries = smoke.wait_native("disabled", "SESSION_SUMMARY", SESSION)
        self.assertEqual(names, ["diagnostics-0.jsonl", "diagnostics-1.jsonl"])
        self.assertEqual([row["event"] for row in rows], ["SESSION_START", "SESSION_METADATA", "SESSION_SUMMARY"])
        self.assertEqual(len(summaries), 1)
        self.assertEqual(summaries[0]["fields"]["disk_errors"], "0")
        listings = [row for row in self.commands(smoke) if "ls" in row["argv"]]
        self.assertTrue(all(row["argv"][-3:] == ["ls", "-1", "files/diagnostics/main"] for row in listings[1:]))
        self.assertTrue((smoke.output / "enabled-main-diagnostics-1.jsonl").is_file())
        self.assertTrue((smoke.output / "disabled-main-diagnostics-1.jsonl").is_file())

    def test_snapshot_listing_uses_single_column_output_and_reads_exact_snapshot_bytes(self):
        files = {
            "session-metadata.txt": "exporter_commit=" + "a" * 40 + "\nconfigured_enabled=true\nsink_active=true\n",
            "main/diagnostics-0.jsonl": json.dumps({"schema": 1, "session_id": SESSION, "sequence": 1,
                                                   "event": "SESSION_METADATA", "fields": {"commit": "a" * 40}}) + "\n",
        }
        smoke = self.smoke("snapshot-columns", {"snapshot_files": files})
        raw = smoke.adb("exec-out", "run-as", MODULE["PACKAGE"], "ls", "cache/diagnostics-exports")
        self.assertEqual(raw.decode().strip(), "snapshot-" + SESSION + "  not-a-snapshot.txt")
        smoke.preview_entries = {name: len(data.encode()) for name, data in files.items()}
        smoke.step("export_snapshot", smoke.export_snapshot)
        self.assertEqual(smoke.results["export_snapshot"]["status"], "pass")
        self.assertEqual(smoke.snapshot_bytes, {name: data.encode() for name, data in files.items()})
        self.assertEqual(smoke.snapshot_id, "snapshot-" + SESSION)
        listings = [row for row in self.commands(smoke) if "ls" in row["argv"]]
        self.assertEqual(listings[-1]["argv"][-3:], ["ls", "-1", "cache/diagnostics-exports"])

    def test_observed_all_caps_buttons_work_in_retention_category_and_preview_flows(self):
        labels = {
            "retention_setting": "RETENTION / 保留: 2, 4 OR 8 × 2 MIB PER PROCESS",
            "category_setting": "DIAGNOSTIC CATEGORIES / 诊断项目",
            "export_preview": "PREVIEW AND EXPORT / 预览并导出",
        }
        for check, label in labels.items():
            with self.subTest(check=check):
                button = ui_xml({"class": "android.widget.Button", "text": label, "resource-id": "", "clickable": "true"})
                if check == "retention_setting":
                    screens = [button, ui_xml({"text": "2 files / 4 MiB"}), ui_xml({"text": "Limit per process: 2 × 2 MiB"})]
                    action = "retention"
                elif check == "category_setting":
                    screens = [button, ui_xml({"text": "guest", "checked": "true"}),
                               ui_xml({"text": "guest", "checked": "false"},
                                      {"class": "android.widget.Button", "text": "OK", "resource-id": "android:id/button1"})]
                    action = "categories"
                else:
                    screens = [button, ui_xml({"text": "Export snapshot / 导出快照"},
                                             {"text": "session-metadata.txt (10 bytes)"},
                                             {"text": "main/diagnostics-0.jsonl (10 bytes)"})]
                    action = "export_preview"
                smoke = self.smoke("button-" + check, {"xml_sequence": screens})
                smoke.session = "00000000-0000-0000-0000-000000000000"
                smoke.step(check, getattr(smoke, action))
                self.assertEqual(smoke.results[check]["status"], "pass")
                self.assertEqual(smoke.summary["overall"], "blocked")
                self.assertTrue(any("tap" in row["argv"] for row in self.commands(smoke)))

    def test_known_button_matching_rejects_other_packages_nonbuttons_and_disabled_nodes(self):
        for name, override in (("wrong-package", {"package": "fixture.other"}),
                               ("textview", {"class": "android.widget.TextView"}),
                               ("disabled", {"enabled": "false"})):
            with self.subTest(case=name):
                xml = ui_xml({"class": "android.widget.Button", "text": "PREVIEW AND EXPORT / 预览并导出", **override})
                smoke = self.smoke("button-" + name, {"xml_sequence": [xml]})
                with self.assertRaises(Blocked):
                    smoke.seek("Preview and export", name, button=True)
                self.assertFalse(any("tap" in row["argv"] for row in self.commands(smoke)))

    def test_all_caps_export_confirmation_reaches_the_unchanged_saf_observation(self):
        smoke = self.smoke("export-confirm-caps", {"xml_sequence": [
            ui_xml({"class": "android.widget.Button", "text": "EXPORT", "resource-id": "android:id/button1"}),
            ui_xml({"package": "com.google.android.documentsui", "text": "Unrecognized local root"}),
        ]})
        with self.assertRaisesRegex(Blocked, "SAF picker does not expose a recognized local Downloads root"):
            smoke.saf_save()
        taps = [row for row in self.commands(smoke) if "tap" in row["argv"]]
        self.assertEqual(len(taps), 1)
        self.assertEqual(smoke.results, {})
        self.assertEqual(smoke.summary["overall"], "blocked")

    def test_known_search_locations_gets_one_back_then_fresh_settings_and_diagnostics(self):
        settings = ui_xml({"text": "Developer diagnostics"})
        diagnostics = ui_xml({"text": "Enable diagnostics"})
        smoke = self.smoke("settings-back", {"xml_sequence": [SEARCH_LOCATIONS_XML, settings, settings, diagnostics]})
        smoke.args.ui_timeout = 3
        smoke.step("settings_entry", smoke.open_settings)
        self.assertEqual(smoke.results["settings_entry"]["status"], "pass")
        attempts = smoke.summary["settings_back_recoveries"]
        self.assertEqual(len(attempts), 1)
        self.assertEqual(attempts[0]["source_xml"], "001-settings.xml")
        self.assertEqual(attempts[0]["status"], "back_sent")
        commands = self.commands(smoke)
        backs = [row for row in commands if "KEYCODE_BACK" in row["argv"]]
        self.assertEqual(len(backs), 1)
        self.assertEqual(backs[0]["number"], attempts[0]["command_number"])
        self.assertFalse(any(row["argv"][-2:] == ["404", "732"] for row in commands))
        self.assertTrue((smoke.output / "004-diagnostics.xml").is_file())
        report = json.loads((smoke.output / "runtime-report.json").read_text())
        self.assertEqual(report["settings_back_recoveries"], attempts)
        self.assertEqual(smoke.summary["overall"], "blocked")

    def test_search_locations_recovery_rejects_other_labels_pages_controls_and_pine_anr(self):
        cases = (
            ("other-label", "diagnostics", SEARCH_LOCATIONS_XML),
            ("other-package", "settings", SEARCH_LOCATIONS_XML.replace('package="' + MODULE["PACKAGE"] + '"',
                                                                        'package="fixture.other"')),
            ("other-title", "settings", SEARCH_LOCATIONS_XML.replace('content-desc="Search Locations"', 'content-desc="Other page"')),
            ("missing-list", "settings", SEARCH_LOCATIONS_XML.replace(':id/locations_list', ':id/unknown_list')),
            ("disabled-add", "settings", SEARCH_LOCATIONS_XML.replace('class="android.widget.Button" enabled="true"',
                                                                       'class="android.widget.Button" enabled="false"')),
            ("wrong-up-control", "settings", SEARCH_LOCATIONS_XML.replace('class="android.widget.ImageButton"',
                                                                           'class="android.widget.TextView"')),
            ("unclickable-up", "settings", SEARCH_LOCATIONS_XML.replace('clickable="true"', 'clickable="false"')),
        )
        for name, label, xml in cases:
            with self.subTest(case=name):
                smoke = self.smoke("search-" + name, {"xml_sequence": [xml]})
                smoke.args.ui_timeout = .5
                with self.assertRaises(Blocked):
                    smoke.wait_ui(label, lambda value: False)
                self.assertFalse(any("input" in row["argv"] for row in self.commands(smoke)))
                self.assertEqual(smoke.summary["settings_back_recoveries"], [])
        root = ET.fromstring(SEARCH_LOCATIONS_XML)
        root.extend(ET.fromstring(PINE_ANR_XML))
        smoke = self.smoke("search-pine-anr", {"xml_sequence": [ET.tostring(root, encoding="unicode")]},
                           allow_platform_anr=True)
        smoke.step("settings_entry", lambda: smoke.wait_ui("settings", lambda value: True))
        self.assertEqual(smoke.results["settings_entry"]["status"], "fail")
        self.assertFalse(any("input" in row["argv"] for row in self.commands(smoke)))
        self.assertEqual(smoke.summary["settings_back_recoveries"], [])

    def test_search_locations_back_respects_deadline_and_cleanup_cannot_repeat_it(self):
        smoke = self.smoke("search-late-back", {"xml_sequence": [SEARCH_LOCATIONS_XML], "back": 2.5})
        started = time.monotonic()
        with self.assertRaises(Blocked):
            smoke.wait_ui("settings", lambda value: False)
        self.assertLess(time.monotonic() - started, 1.8)
        self.assertEqual(smoke.summary["settings_back_recoveries"][0]["status"], "blocked")
        self.assertTrue(any(row["timed_out"] and "KEYCODE_BACK" in row["argv"] for row in self.commands(smoke)))

        smoke = self.smoke("search-persistent", {"xml_sequence": [SEARCH_LOCATIONS_XML]})
        smoke.args.ui_timeout = 3
        smoke.ever_enabled = True
        smoke.step("disable_stop", smoke.disable)
        self.assertEqual(smoke.results["disable_stop"]["status"], "blocked")
        self.assertEqual(len([row for row in self.commands(smoke) if "KEYCODE_BACK" in row["argv"]]), 1)
        self.assertEqual(smoke.summary["settings_back_recoveries"][0]["status"], "back_sent")
        self.assertIn("single Settings Back attempt", smoke.summary["settings_back_recovery_blocker"]["reason"])
        smoke.step("settings_entry", smoke.open_settings)
        self.assertEqual(smoke.results["settings_entry"]["status"], "blocked")
        self.assertEqual(len([row for row in self.commands(smoke) if "KEYCODE_BACK" in row["argv"]]), 1)

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

    def test_opt_in_bluetooth_close_requires_a_fresh_settings_observation(self):
        smoke = self.smoke("bluetooth-settings", {"xml_sequence": [BLUETOOTH_CRASH_XML, READY_XML]},
                           dismiss_platform_crash=True)
        smoke.args.ui_timeout = 3
        screen = smoke.wait_ui("settings", lambda value: value.find("Ready") is not None)
        self.assertEqual(screen.number, 2)
        attempts = smoke.summary["platform_crash_dismissals"]
        self.assertEqual(len(attempts), 1)
        self.assertEqual(attempts[0]["status"], "close_sent")
        self.assertEqual(attempts[0]["source_xml"], "001-settings.xml")
        taps = [row for row in self.commands(smoke) if "tap" in row["argv"]]
        self.assertEqual(len(taps), 1)
        self.assertEqual(taps[0]["argv"][-2:], ["239", "450"])
        self.assertEqual(taps[0]["number"], attempts[0]["command_number"])
        self.assertEqual(smoke.results, {})
        self.assertEqual(smoke.summary["overall"], "blocked")
        report = json.loads((smoke.output / "runtime-report.json").read_text())
        self.assertTrue(report["platform_crash_dismiss_enabled"])
        self.assertEqual(report["platform_crash_dismissals"], attempts)

    def test_bluetooth_close_rejects_default_pine_unknown_anr_and_wrong_controls(self):
        def changed(resource, **attributes):
            root = ET.fromstring(BLUETOOTH_CRASH_XML)
            node = next(node for node in root.iter("node") if node.get("resource-id") == resource)
            for key, value in attributes.items():
                if value is None:
                    node.attrib.pop(key, None)
                else:
                    node.set(key, value)
            return ET.tostring(root, encoding="unicode")

        scenarios = (
            ("default", BLUETOOTH_CRASH_XML, False),
            ("pine", BLUETOOTH_CRASH_XML.replace("Bluetooth", "Pine Diagnostics"), True),
            ("unknown", BLUETOOTH_CRASH_XML.replace("Bluetooth", "Fixture App"), True),
            ("anr", PLATFORM_ANR_XML, True),
            ("package", changed("android:id/aerr_close", package="fixture.other"), True),
            ("title-package", changed("android:id/alertTitle", package="fixture.other"), True),
            ("prefixed-resource", changed("android:id/aerr_close", **{"resource-id": "fixture:android:id/aerr_close"}), True),
            ("app-info", changed("android:id/aerr_close", text="App info"), True),
            ("image-button", changed("android:id/aerr_close", **{"class": "android.widget.ImageButton"}), True),
            ("disabled", changed("android:id/aerr_close", enabled="false"), True),
            ("missing-enabled", changed("android:id/aerr_close", enabled=None), True),
            ("unclickable", changed("android:id/aerr_close", clickable="false"), True),
            ("invalid-bounds", changed("android:id/aerr_close", bounds="[64,426][64,474]"), True),
        )
        for name, xml, enabled in scenarios:
            with self.subTest(case=name):
                smoke = self.smoke("bluetooth-guard-" + name, {"xml_sequence": [xml]}, dismiss_platform_crash=enabled)
                screen = smoke.screen("guard", deadline=time.monotonic() + 1)
                self.assertFalse(smoke.dismiss_known_platform_crash("settings", screen, time.monotonic() + 1))
                self.assertFalse(any("tap" in row["argv"] for row in self.commands(smoke)))
                self.assertEqual(smoke.summary["platform_crash_dismissals"], [])
        for name in ("separate-container", "ambiguous-title"):
            root = ET.fromstring(BLUETOOTH_CRASH_XML)
            close = next(node for node in root.iter("node") if node.get("resource-id") == "android:id/aerr_close")
            if name == "separate-container":
                root[0].remove(close)
                root.append(close)
            else:
                ET.SubElement(root, "node", {"resource-id": "android:id/alertTitle", "package": "android",
                                             "text": "Pine Diagnostics keeps stopping", "bounds": "[0,0][100,100]"})
            smoke = self.smoke("bluetooth-guard-" + name, {"xml_sequence": [ET.tostring(root, encoding="unicode")]},
                               dismiss_platform_crash=True)
            screen = smoke.screen("guard", deadline=time.monotonic() + 1)
            self.assertFalse(smoke.dismiss_known_platform_crash("settings", screen, time.monotonic() + 1))
            self.assertFalse(any("tap" in row["argv"] for row in self.commands(smoke)))
        for label in ("diagnostics", "category-dialog", "export-preview", "saf-picker"):
            smoke = self.smoke("bluetooth-other-" + label, {"xml_sequence": [BLUETOOTH_CRASH_XML]},
                               dismiss_platform_crash=True)
            smoke.args.ui_timeout = .4
            with self.assertRaises(Blocked):
                smoke.wait_ui(label, lambda value: False)
            self.assertFalse(any("input" in row["argv"] for row in self.commands(smoke)))
            self.assertEqual(smoke.summary["platform_crash_dismissals"], [])
        smoke = self.smoke("bluetooth-no-seek-close", {"xml_sequence": [BLUETOOTH_CRASH_XML]},
                           dismiss_platform_crash=True)
        with self.assertRaises(Blocked):
            smoke.seek("Ready", "settings")
        self.assertFalse(any("input" in row["argv"] for row in self.commands(smoke)))

    def test_bluetooth_close_is_bounded_and_cleanup_cannot_repeat_or_replace_it(self):
        smoke = self.smoke("bluetooth-late-close", {"xml_sequence": [BLUETOOTH_CRASH_XML], "tap": 2.5},
                           dismiss_platform_crash=True)
        started = time.monotonic()
        with self.assertRaises(Blocked):
            smoke.wait_ui("settings", lambda value: False)
        self.assertLess(time.monotonic() - started, 1.8)
        self.assertEqual(smoke.summary["platform_crash_dismissals"][0]["status"], "blocked")
        self.assertTrue(any(row["timed_out"] and "tap" in row["argv"] for row in self.commands(smoke)))

        smoke = self.smoke("bluetooth-persistent", {"xml_sequence": [BLUETOOTH_CRASH_XML]}, dismiss_platform_crash=True)
        smoke.args.ui_timeout = 3
        smoke.ever_enabled = True
        smoke.step("disable_stop", smoke.disable)
        self.assertEqual(smoke.results["disable_stop"]["status"], "blocked")
        self.assertEqual(smoke.summary["platform_crash_dismissals"][0]["status"], "close_sent")
        with self.assertRaises(Blocked):
            smoke.seek("Ready", "persistent")
        inputs = [row for row in self.commands(smoke) if "input" in row["argv"]]
        self.assertEqual(len(inputs), 1)
        self.assertIn("tap", inputs[0]["argv"])
        self.assertIn("single ordinary Close app", smoke.summary["platform_crash_dismiss_blocker"]["reason"])
        self.assertEqual(smoke.summary["overall"], "blocked")

    def test_bluetooth_recovery_never_masks_pine_anr_or_a_recorded_pine_crash(self):
        root = ET.fromstring(BLUETOOTH_CRASH_XML)
        root.extend(ET.fromstring(PINE_ANR_XML))
        smoke = self.smoke("bluetooth-pine-anr", {"xml_sequence": [ET.tostring(root, encoding="unicode")]},
                           dismiss_platform_crash=True)
        smoke.step("settings_entry", lambda: smoke.wait_ui("settings", lambda value: True))
        self.assertEqual(smoke.results["settings_entry"]["status"], "fail")
        self.assertFalse(any("input" in row["argv"] for row in self.commands(smoke)))

        smoke = self.smoke("bluetooth-pine-crash", {"xml_sequence": [BLUETOOTH_CRASH_XML, READY_XML]},
                           dismiss_platform_crash=True)
        smoke.args.ui_timeout = 3
        smoke.wait_ui("settings", lambda value: value.find("Ready") is not None)
        log = smoke.output / "runtime-logcat.txt"
        log.write_text("AndroidRuntime: FATAL EXCEPTION: main\nProcess: " + MODULE["PACKAGE"] + ", PID: 1234\n")
        smoke.log_stream = log.open("ab")
        self.addCleanup(smoke.log_stream.close)
        smoke.logger = type("AliveCapture", (), {"poll": lambda self: None})()
        smoke.step("app_crash_free", smoke.app_crash_free)
        self.assertEqual(smoke.results["app_crash_free"]["status"], "fail")
        self.assertEqual(smoke.summary["overall"], "fail")
        self.assertEqual(len(smoke.summary["platform_crash_dismissals"]), 1)

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
                             "dropbox" in row["argv"] or
                             "run-as" in row["argv"] for row in self.commands(smoke)))
        self.assertEqual(smoke.summary["overall"], "pass")

    def test_system_anr_traces_are_opt_in_and_capture_errors_preserve_failure_and_cleanup(self):
        for name, enabled, config in (
            ("default", False, {}),
            ("denied", True, {"lastanr_traces_result": {"code": 1, "stderr": "ANR trace permission denied"}}),
            ("timeout", True, {"lastanr_traces_result": {"stdout": "Partial global trace", "stderr": "Trace still being read"},
                               "lastanr-traces": 2, "emit_before_delay": "lastanr-traces"}),
            ("dropbox-denied", True, {"dropbox_result": {"code": 1, "stderr": "DropBox permission denied"}}),
            ("dropbox-timeout", True, {"dropbox_result": {"stdout": "Partial data_app_anr entry", "stderr": "DropBox still being read"},
                                       "dropbox": 2, "emit_before_delay": "dropbox"}),
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
                self.assertIn("all retained data_app_anr", report["system_last_anr_traces_scope"])
                self.assertIn("old or truncated", report["system_last_anr_traces_scope"])
                commands = self.commands(smoke)
                traces = [row for row in commands if "lastanr-traces" in row["argv"]]
                dropbox = [row for row in commands if "dropbox" in row["argv"]]
                self.assertEqual(len(traces), int(enabled))
                self.assertEqual(len(dropbox), int(enabled))
                self.assertEqual((smoke.output / "last-anr-traces.txt").exists(), enabled)
                self.assertEqual((smoke.output / "dropbox-data-app-anr.txt").exists(), enabled)
                if enabled:
                    self.assertEqual(dropbox[0]["argv"][-5:], ["shell", "dumpsys", "dropbox", "--print", "data_app_anr"])
                if name == "denied":
                    self.assertEqual(traces[0]["returncode"], 1)
                    self.assertIn("permission denied", (smoke.output / "last-anr-traces.stderr.txt").read_text())
                if name == "timeout":
                    self.assertTrue(traces[0]["timed_out"])
                    self.assertIn("Partial global trace", (smoke.output / "last-anr-traces.txt").read_text())
                    self.assertIn("still being read", (smoke.output / "last-anr-traces.stderr.txt").read_text())
                if name == "dropbox-denied":
                    self.assertEqual(dropbox[0]["returncode"], 1)
                    self.assertIn("permission denied", (smoke.output / "dropbox-data-app-anr.stderr.txt").read_text())
                if name == "dropbox-timeout":
                    self.assertTrue(dropbox[0]["timed_out"])
                    self.assertIn("Partial data_app_anr entry", (smoke.output / "dropbox-data-app-anr.txt").read_text())
                    self.assertIn("still being read", (smoke.output / "dropbox-data-app-anr.stderr.txt").read_text())
                self.assertTrue((smoke.output / "final-activities.txt").is_file())
                self.assertTrue(any(row["argv"][-3:] == ["rm", "-f", smoke.remote_xml] for row in commands))

    def assert_gate_only(self, smoke, status):
        self.assertEqual(set(smoke.results), {"device_ready"})
        self.assertEqual(smoke.results["device_ready"]["status"], status)
        self.assertEqual(smoke.summary["overall"], "blocked")
        self.assertFalse(any("install" in row["argv"] for row in self.commands(smoke)))
        if not smoke.summary["storage_readiness_required"]:
            self.assertFalse(any("sm" in row["argv"] for row in self.commands(smoke)))
        rows = [json.loads(line) for line in (smoke.output / "readiness-observations.jsonl").read_text().splitlines()]
        self.assertEqual(rows, smoke.summary["android_readiness"]["observations"])
        return rows

    def test_optional_storage_readiness_retries_unmounted_then_confirms_pid_stability(self):
        smoke = self.smoke("storage-transient", {"storage_sequence": [
            "emulated;0 mounted null\nprivate unmounted null", "private mounted null\nemulated;0 mounted null"]},
                           require_storage=True)
        smoke.args.boot_timeout = 6
        smoke.step("device_ready", smoke.device_ready)
        rows = self.assert_gate_only(smoke, "pass")
        self.assertEqual([row["status"] for row in rows], ["retry", "pass"])
        self.assertFalse(rows[0]["storage_internal_mounted"])
        self.assertEqual(rows[0]["storage_volumes_raw"], "emulated;0 mounted null\nprivate unmounted null\n")
        self.assertTrue(rows[1]["storage_internal_mounted"])
        self.assertEqual(rows[1]["storage_internal_volume_line"], "private mounted null")
        self.assertEqual(rows[1]["system_server_before_pids"], rows[1]["system_server_after_pids"])
        self.assertGreaterEqual(rows[1]["continuity_seconds"], 2)
        self.assertTrue(smoke.summary["storage_readiness_required"])
        self.assertTrue(json.loads((smoke.output / "runtime-report.json").read_text())["storage_readiness_required"])
        for row in rows:
            self.assertTrue(any(name.endswith("storage-volumes.txt") for name in row["evidence"]))

    def test_optional_storage_readiness_missing_denied_and_late_observations_cannot_pass(self):
        cases = (
            ("unmounted", {"storage_sequence": ["private unmounted null"]}),
            ("adopted-only", {"storage_sequence": ["private:179,65 mounted uuid\nemulated;0 mounted null"]}),
            ("read-only", {"storage_sequence": ["private mounted_ro null"]}),
            ("denied", {"storage_sequence": [{"stdout": "private mounted null", "code": 1, "stderr": "Storage permission denied"}]}),
            ("late", {"storage_sequence": ["private mounted null"], "storage": 2, "emit_before_delay": "storage"}),
        )
        for name, config in cases:
            with self.subTest(case=name):
                smoke = self.smoke("storage-" + name, config, require_storage=True)
                smoke.args.boot_timeout = .8
                started = time.monotonic()
                smoke.step("device_ready", smoke.device_ready)
                self.assertLess(time.monotonic() - started, 1.6)
                rows = self.assert_gate_only(smoke, "blocked")
                self.assertNotIn("accepted_attempt", smoke.summary["android_readiness"])
                self.assertTrue(all(row.get("storage_internal_mounted") is not True for row in rows))
                self.assertIsNotNone(rows[0]["storage_volumes_raw"])
                if name == "denied":
                    self.assertIn("permission denied", rows[0]["command_errors"]["storage-volumes"])
                if name == "late":
                    self.assertTrue(any(row["timed_out"] and "sm" in row["argv"] for row in self.commands(smoke)))

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
