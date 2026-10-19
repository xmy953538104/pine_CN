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
PINE_ANR_XML = PLATFORM_ANR_XML.replace("System UI isn't responding", "Pine Diagnostics Preview isn't responding")
SYSTEM_ANR_XML = PLATFORM_ANR_XML.replace("System UI isn't responding", "Digital Wellbeing isn't responding").replace(
    '</node></hierarchy>', '<node text="Close app" resource-id="android:id/aerr_close" package="android" '
    'class="android.widget.Button" enabled="true" clickable="true" bounds="[64,380][415,428]"/></node></hierarchy>')


def anr_focus(package="com.google.android.apps.wellbeing", window="e7bb3d6"):
    value = f"Window{{{window} u0 Application Not Responding: {package}}}"
    return f"  mCurrentFocus={value}\n    mFocusedWindow={value}\n"
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


def saf_picker_xml(filename=None, provider="com.google.android.documentsui", enabled=True):
    return ui_xml({"package": provider, "text": "Downloads", "resource-id": provider + ":id/breadcrumb_text", "clickable": "false"},
                  {"package": provider, "content-desc": "Show roots", "class": "android.widget.ImageButton", "clickable": "true"},
                  {"package": provider, "text": filename or "pine-diag-" + SESSION + ".zip",
                   "class": "android.widget.EditText", "resource-id": "android:id/title"},
                  {"package": provider, "text": "SAVE", "class": "android.widget.Button", "resource-id": "android:id/button1",
                   "clickable": "true", "enabled": "true" if enabled else "false", "bounds": "[378,728][466,776]"})


def saf_roots_xml(provider="com.google.android.documentsui", title="Downloads"):
    tree = ET.fromstring(saf_picker_xml(provider=provider))
    roots = ET.SubElement(tree, "node", {"package": provider, "resource-id": provider + ":id/roots_list",
        "class": "android.widget.ListView", "enabled": "true", "bounds": "[0,24][400,800]"})
    row = ET.SubElement(roots, "node", {"package": provider, "class": "android.widget.LinearLayout",
        "enabled": "true", "clickable": "true", "bounds": "[0,120][400,184]"})
    ET.SubElement(row, "node", {"package": provider, "class": "android.widget.TextView", "resource-id": "android:id/title",
        "text": title, "enabled": "true", "clickable": "false", "bounds": "[60,120][340,184]"})
    return ET.tostring(tree, encoding="unicode")


SEARCH_LOCATIONS_XML = '''<hierarchy>
<node resource-id="io.github.pine.emu.diagnostics.preview:id/toolbar_layout" package="io.github.pine.emu.diagnostics.preview"
 class="android.widget.FrameLayout" content-desc="Search Locations" enabled="true" bounds="[0,0][480,136]">
 <node package="io.github.pine.emu.diagnostics.preview" class="android.widget.ImageButton" content-desc="Navigate up"
  clickable="true" enabled="true" bounds="[0,28][56,84]"/>
</node>
<node resource-id="io.github.pine.emu.diagnostics.preview:id/locations_list" package="io.github.pine.emu.diagnostics.preview"
 class="androidx.recyclerview.widget.RecyclerView" enabled="true" bounds="[0,136][480,800]"/>
<node text="Add" resource-id="io.github.pine.emu.diagnostics.preview:id/add_location_button" package="io.github.pine.emu.diagnostics.preview"
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
            remote_store = Path(__file__).with_suffix('.remote-files.json')
            remote_files = json.loads(remote_store.read_text()) if remote_store.is_file() else {}
            def save_remote():
                remote_store.write_text(json.dumps(remote_files))
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
            elif args == ['shell', 'pidof', 'io.github.pine.emu.diagnostics.preview']:
                operation, output = 'app-pids', config.get('app_pids', '1234')
            elif args == ['shell', 'dumpsys', 'activity', 'lastanr']:
                operation, output = 'lastanr', config.get('lastanr_result', 'ANR in io.github.pine.emu.diagnostics.preview')
            elif args == ['shell', 'dumpsys', 'activity', 'lastanr-traces']:
                operation, output = 'lastanr-traces', config.get('lastanr_traces_result', 'Global fixture ANR traces')
            elif args == ['shell', 'dumpsys', 'dropbox', '--print', 'data_app_anr']:
                operation, output = 'dropbox', config.get('dropbox_result', 'Retained data_app_anr fixture entry')
            elif args == ['shell', 'dumpsys', 'activity', 'activities']:
                operation, output = 'activities', next_result('activities_sequence', 'Final activity evidence')
            elif args == ['shell', 'dumpsys', 'window']:
                operation, output = 'window-focus', config.get('window_focus', 'mCurrentFocus=Window{abcdef u0 com.google.android.documentsui/com.android.documentsui.picker.PickActivity}')
            elif args == ['shell', 'dumpsys', 'activity', 'providers']:
                operation, output = 'providers', 'Published provider fixture; not a SAF acceptance pass'
            elif args[:7] == ['shell', 'cmd', 'package', 'list', 'packages', '-s', 'com.google.android.apps.wellbeing']:
                operation, output = 'system-package', config.get('system_package_result', 'package:com.google.android.apps.wellbeing')
            elif args[:7] == ['shell', 'cmd', 'package', 'list', 'packages', '-s', 'com.google.android.apps.nexuslauncher']:
                operation, output = 'system-package', config.get('system_package_result', 'package:com.google.android.apps.nexuslauncher')
            elif args == ['logcat', '-d', '-v', 'raw', 'PineDiag:V', '*:S']:
                operation, output = 'pine-logcat', json.dumps({'schema':1,'session_id':config['session'],'sequence':1,'event':'SESSION_METADATA','fields':{'commit':'a'*40}})
            elif args[:2] == ['logcat', '-b']:
                operation, output = 'final-logcat', 'Final crash evidence'
            elif args[:3] == ['shell', 'rm', '-f']:
                operation, output = 'remove-xml', ''
                for name in args[3:]: remote_files.pop(name, None)
                save_remote()
            elif args[:4] == ['shell', 'test', '!', '-e']:
                operation, output = 'new-xml-path', ''
                code = 1 if args[4] in remote_files else 0
            elif args == ['shell', 'cmd', 'package', 'path', 'android']:
                operation, output = 'package', next_result('package_sequence', 'package:/system/framework/framework-res.apk')
            elif args == ['shell', 'cmd', 'package', 'list', 'packages', '-u', 'io.github.pine.emu.diagnostics.preview']:
                operation, output = 'preinstall-package', config.get('preinstall_package', '')
            elif args[:1] == ['install']:
                operation, output = 'install', config.get('install_result', 'Success')
            elif args == ['shell', 'sm', 'list-volumes', 'all']:
                operation, output = 'storage', next_result('storage_sequence', 'private mounted null\\nemulated;0 mounted null')
            elif args == ['shell', 'ls', '-ld', '/sdcard/Download']:
                operation, output = 'downloads-state', 'drwxrws--- 2 media_rw media_rw 4096 2026-10-17 /sdcard/Download'
            elif args == ['shell', 'ls', '-ld', '/sdcard/.']:
                operation, output = 'emulated-directory', next_result('directory_sequence', 'drwxrws--- 2 media_rw media_rw 4096 2026-10-17 /sdcard/.')
            elif args[:2] == ['shell', 'cat'] and args[2].startswith('/proc/'):
                operation, output = 'proc-status', 'Name:\\toffline-fixture'
                if config.get('status_denied'):
                    code, stderr, output = 1, 'cat: Permission denied', ''
            elif args[:3] == ['shell', 'uiautomator', 'dump']:
                operation, output = 'dump', 'UI hierchary dumped to: ' + args[-1]
                null_root = next_result('dump_null_root_sequence', config.get('dump_null_root', False))
                if null_root:
                    output, stderr = '', 'ERROR: null root node returned by UiTestAutomationBridge.'
                elif not config.get('dump_no_file'):
                    remote_files[args[-1]] = next_result('xml_sequence', '<hierarchy><node text="Ready" enabled="true" scrollable="true" bounds="[0,0][100,100]"/></hierarchy>')
                    save_remote()
                output += next_result('dump_suffix_sequence', '')
            elif args[:3] == ['shell', 'am', 'start']:
                operation, output = 'start-settings', 'Status: ok'
            elif args[:2] == ['exec-out', 'cat'] and args[2].startswith('/sdcard/') and 'zip_base64' in config:
                operation, output, binary = 'safzip', base64.b64decode(config['zip_base64']), True
            elif args[:2] == ['exec-out', 'cat']:
                operation = 'xml'
                output = remote_files.get(args[2], '')
                if args[2] not in remote_files:
                    code, stderr = 1, 'cat: No such file or directory'
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
            elif (args[:4] == ['exec-out', 'run-as', 'io.github.pine.emu.diagnostics.preview', 'cat']
                  and args[4].startswith('/proc/') and args[4].endswith('/maps')):
                operation, output = 'maps', config.get('maps_result', 'Fixture library mappings')
            elif args[:5] == ['exec-out', 'run-as', 'io.github.pine.emu.diagnostics.preview', '/system/bin/debuggerd', '-b']:
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
            elif args[:4] == ['exec-out', 'run-as', 'io.github.pine.emu.diagnostics.preview', 'cat'] and args[4] == 'shared_prefs/diagnostics.xml':
                operation, output = 'prefs', config.get('prefs_xml', '<map><string name="files">2</string><string name="categories">content,storage,exception</string></map>')
            elif args[:4] == ['exec-out', 'run-as', 'io.github.pine.emu.diagnostics.preview', 'cat'] and args[4].startswith('cache/diagnostics-exports/'):
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
              dismiss_platform_crash=False, close_system_anr=False):
        self.config.write_text(json.dumps({"session": SESSION, **delays}))
        self.fake.with_suffix('.state.json').unlink(missing_ok=True)
        self.fake.with_suffix('.remote-files.json').unlink(missing_ok=True)
        # Short fixture budgets keep these real subprocess tests fast while
        # leaving room for interpreter startup on both Linux and macOS.
        # Only helpers and the readiness gate run; APK checks remain unexecuted.
        return Smoke(argparse.Namespace(
            adb=str(self.fake), serial="offline-fixture", apk=self.root / "absent.apk",
            expected_commit="a" * 40, output=self.root / name,
            command_timeout=2.5, ui_timeout=1, boot_timeout=2.5, install_timeout=2.5,
            wait_known_platform_anr=allow_platform_anr, collect_system_last_anr_traces=collect_system_anr,
            require_ready_storage=require_storage, dismiss_known_platform_crash=dismiss_platform_crash,
            close_known_system_anr=close_system_anr))

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
        picker = saf_picker_xml(filename, provider)
        files = {"session-metadata.txt": b"fixture metadata\n", "main/diagnostics-0.jsonl":
                 (json.dumps({"schema": 1, "session_id": SESSION, "sequence": 1, "event": "SESSION_METADATA", "fields": {"commit": "a" * 40}}) + "\n").encode()}
        archive = io.BytesIO()
        with zipfile.ZipFile(archive, "w") as zipped:
            for entry, data in files.items():
                zipped.writestr(entry, data)
        config = {"xml_sequence": [ui_xml({"text": "EXPORT", "class": "android.widget.Button"}), picker,
                                   saf_roots_xml(provider), picker, diagnostics_xml()],
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
        self.assertEqual(sum("tap" in row["argv"] for row in commands), 4)
        root = smoke.summary["saf_root_selections"][0]
        self.assertEqual(root["bounds"], "[0,120][400,184]")
        self.assertEqual(commands[root["command_number"] - 1]["argv"][-2:], ["200", "152"])

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
        for index in (3,):
            root = ET.fromstring(config["xml_sequence"][index])
            for node in root.iter("node"):
                if node.get("text") == "SAVE":
                    node.set("package", "foreign.app")
            config["xml_sequence"][index] = ET.tostring(root, encoding="unicode")
        self.config.write_text(json.dumps(config))
        with self.assertRaises(Blocked):
            smoke.saf_save()
        self.assertEqual(sum("tap" in row["argv"] for row in self.commands(smoke)), 3)

    def test_saf_valid_zip_bytes_after_deadline_do_not_pass(self):
        smoke = self.saf_fixture("saf-late", safzip=0.8, emit_before_delay="safzip")
        smoke.args.ui_timeout = 0.5
        with self.assertRaises(Blocked):
            smoke.saf_save()
        self.assertTrue(any(row["timed_out"] and "/sdcard/Download/" in " ".join(row["argv"]) for row in self.commands(smoke)))

    def test_downloads_root_requires_one_clickable_row_inside_real_roots_list(self):
        def screen(xml):
            return MODULE["Screen"](ET.fromstring(xml), self.root / "fixture.xml", 1)
        self.assertIsNone(self.smoke("breadcrumb-only", {}).downloads_root(screen(saf_picker_xml())))
        smoke = self.smoke("actual-root-row", {})
        self.assertEqual(smoke.downloads_root(screen(saf_roots_xml())).get("bounds"), "[0,120][400,184]")
        cases = [saf_roots_xml(provider="foreign.app"), saf_roots_xml(title="Pine Diagnostics Preview"),
                 saf_roots_xml().replace(':id/roots_list', ':id/unrelated_list'),
                 saf_roots_xml().replace('clickable="true" bounds="[0,120]', 'clickable="false" bounds="[0,120]')]
        duplicate = ET.fromstring(saf_roots_xml())
        roots = next(n for n in duplicate if n.get("resource-id", "").endswith(":id/roots_list"))
        roots.append(ET.fromstring(ET.tostring(roots[0])))
        cases.append(ET.tostring(duplicate, encoding="unicode"))
        for xml in cases:
            with self.subTest(xml=xml[:70]):
                self.assertIsNone(smoke.downloads_root(screen(xml)))

    def test_saf_waits_for_enabled_save_after_explicit_root_selection(self):
        smoke = self.saf_fixture("saf-delayed-save", xml_sequence=[
            ui_xml({"text": "EXPORT", "class": "android.widget.Button"}), saf_picker_xml(enabled=False),
            saf_roots_xml(), saf_picker_xml(enabled=False), saf_picker_xml(), diagnostics_xml()])
        smoke.args.ui_timeout = 3
        self.assertTrue(smoke.saf_save()["zip_bytes"])
        self.assertTrue(smoke.verify_zip()["preview_bytes_match"])
        commands = self.commands(smoke)
        save_taps = [row for row in commands if row["argv"][-4:] == ["input", "tap", "422", "752"]]
        self.assertEqual(len(save_taps), 1)
        before = [row for row in commands if row["number"] < save_taps[0]["number"] and "uiautomator" in row["argv"]]
        self.assertEqual(len(before), 5)
        self.assertFalse(smoke.pending_saf_export)

    def test_disabled_save_never_taps_or_passes_zip_and_keeps_pending_picker(self):
        smoke = self.saf_fixture("saf-disabled", xml_sequence=[
            ui_xml({"text": "EXPORT", "class": "android.widget.Button"}), saf_picker_xml(enabled=False),
            saf_roots_xml(), saf_picker_xml(enabled=False)])
        smoke.args.ui_timeout = .5
        smoke.step("saf_save", smoke.saf_save)
        smoke.step("zip_verify", smoke.verify_zip, ("saf_save",))
        self.assertEqual(smoke.results["saf_save"]["status"], "blocked")
        self.assertEqual(smoke.results["zip_verify"]["status"], "blocked")
        self.assertTrue(smoke.pending_saf_export)
        self.assertFalse(any(row["argv"][-4:] == ["input", "tap", "422", "752"] for row in self.commands(smoke)))
        self.assertIsNone(smoke.saved_name)

    def test_cancel_exact_pending_picker_then_observes_real_stop_without_changing_export_block(self):
        smoke = self.cleanup_fixture("cancel-own-picker", xml_sequence=[saf_picker_xml(enabled=False),
            diagnostics_xml(), diagnostics_xml(), diagnostics_xml(), diagnostics_xml(False)])
        smoke.pending_saf_export = True
        smoke.summary["platform_crash_dismissals"].append({"status": "close_sent", "title": "Bluetooth keeps stopping"})
        smoke.results.update(saf_save={"status": "blocked", "reason": "Provider unavailable"},
                             zip_verify={"status": "blocked", "reason": "saf_save blocked"})
        smoke.step("disable_stop", smoke.disable)
        self.assertEqual(smoke.results["disable_stop"]["status"], "pass")
        self.assertEqual(smoke.results["saf_save"]["status"], "blocked")
        self.assertEqual(smoke.results["zip_verify"]["status"], "blocked")
        self.assertEqual(len(smoke.summary["saf_cancel_actions"]), 1)
        self.assertEqual(len([row for row in self.commands(smoke) if "KEYCODE_BACK" in row["argv"]]), 1)
        self.assertTrue(smoke.results["disable_stop"]["summaries"])
        self.assertFalse(smoke.pending_saf_export)

    def test_saf_cancel_rejects_changed_session_foreign_focus_anr_and_null_root(self):
        cases = [("wrong-session", {"xml_sequence": [saf_picker_xml(filename="pine-diag-" + "2" * 36 + ".zip")]}),
                 ("foreign-focus", {"xml_sequence": [saf_picker_xml()], "window_focus": "mCurrentFocus=Window{abcdef u0 foreign.app/.Activity}"}),
                 ("pine-anr", {"xml_sequence": [PINE_ANR_XML]}),
                 ("system-anr", {"xml_sequence": [SYSTEM_ANR_XML]}),
                 ("foreign-picker", {"xml_sequence": [saf_picker_xml(provider="foreign.app")]}),
                 ("null-root", {"dump_null_root": True})]
        for name, config in cases:
            with self.subTest(name=name):
                smoke = self.cleanup_fixture("cancel-reject-" + name, **config)
                smoke.pending_saf_export = True
                smoke.step("disable_stop", smoke.disable)
                self.assertEqual(smoke.results["disable_stop"]["status"], "fail" if name == "pine-anr" else "blocked")
                self.assertFalse(any("input" in row["argv"] for row in self.commands(smoke)))
                self.assertEqual(smoke.summary["saf_cancel_actions"], [])

    def test_saf_cancel_is_single_attempt_when_picker_persists(self):
        smoke = self.cleanup_fixture("cancel-persists", xml_sequence=[saf_picker_xml(enabled=False)])
        smoke.pending_saf_export = True
        smoke.args.ui_timeout = .4
        for _ in range(2):
            with self.assertRaises(Blocked): smoke.cancel_pending_saf()
        self.assertEqual(len(smoke.summary["saf_cancel_actions"]), 1)
        self.assertEqual(len([row for row in self.commands(smoke) if "KEYCODE_BACK" in row["argv"]]), 1)

    def test_storage_provider_capture_is_read_only_and_never_an_acceptance_pass(self):
        smoke = self.smoke("provider-evidence", {}, require_storage=True)
        smoke.capture_saf_storage("saf-fixture")
        self.assertEqual(smoke.results, {})
        self.assertEqual(smoke.summary["overall"], "blocked")
        self.assertTrue(smoke.summary["saf_storage_observations"][0]["diagnostic_only"])
        self.assertEqual([row["argv"][3:] for row in self.commands(smoke)], [
            ["shell", "sm", "list-volumes", "all"], ["shell", "ls", "-ld", "/sdcard/Download"],
            ["shell", "dumpsys", "activity", "providers"]])

    def test_stdout_dump_error_rejects_success_marker_and_new_file_without_reading_or_tapping(self):
        for suffix in ('ERROR: null root node returned by UiTestAutomationBridge.', 'ERROR: hierarchy unavailable'):
            smoke = self.smoke('stdout-error-' + str(len(suffix)), {'dump_suffix_sequence': ['', suffix]})
            smoke.screen('old-ready')
            with self.assertRaisesRegex(Blocked, 'no XML fallback or UI action'):
                smoke.screen('failed-current')
            last = smoke.remote_xml_paths[-1]
            self.assertFalse(any(row['argv'][3:] == ['exec-out', 'cat', last] for row in self.commands(smoke)))
            self.assertFalse(any('input' in row['argv'] for row in self.commands(smoke)))

    def test_unknown_package_query_never_authorizes_installation(self):
        cases = [('installed', {'preinstall_package': 'package:io.github.pine.emu.diagnostics.preview'}),
                 ('error-text', {'preinstall_package': 'Error: package service unavailable'}),
                 ('nonzero', {'preinstall_package': {'stdout': '', 'code': 1}}),
                 ('stderr', {'preinstall_package': {'stdout': '', 'stderr': 'Package service unavailable'}}),
                 ('timeout', {'preinstall-package': 1})]
        for name, config in cases:
            smoke = self.smoke('preinstall-' + name, config)
            smoke.args.command_timeout = .5
            with self.assertRaises(Blocked): smoke.fresh_install()
            self.assertFalse(smoke.installed)
            self.assertFalse(any(row['argv'][3:4] == ['install'] for row in self.commands(smoke)))

    def test_confirmed_absence_and_actual_install_success_are_required(self):
        smoke = self.smoke('install-confirmed', {})
        self.assertTrue(smoke.fresh_install()['fresh'])
        self.assertTrue(smoke.installed)
        self.assertEqual(self.commands(smoke)[0]['argv'][3:], ['shell', 'cmd', 'package', 'list', 'packages', '-u', MODULE['PACKAGE']])
        smoke = self.smoke('install-unsuccessful', {'install_result': 'Error: install failed'})
        with self.assertRaises(AssertionError): smoke.fresh_install()
        self.assertFalse(smoke.installed)

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

    def critical_export_fixture(self, name, critical_name, critical_size=None, report=None):
        # Actual production-sink row; normalize only the controlled session to Android's UUID shape.
        row = json.loads((Path(__file__).resolve().parent / "fixtures/native-trace-snapshot.jsonl").read_text())
        row["session_id"] = SESSION
        files = {"session-metadata.txt": "exporter_commit=" + "a" * 40 + "\nconfigured_enabled=true\nsink_active=true\n",
                 "main/diagnostics-0.jsonl": json.dumps({"schema": 1, "session_id": SESSION, "sequence": 1,
                     "event": "SESSION_METADATA", "fields": {"commit": "a" * 40}}) + "\n",
                 critical_name: json.dumps(row if report is None else report) + "\n"}
        lines = [entry + " (" + str(critical_size if entry == critical_name and critical_size is not None else len(data.encode())) + " bytes)"
                 for entry, data in files.items()]
        smoke = self.smoke(name, {"snapshot_files": files, "xml_sequence": [
            ui_xml({"text": "PREVIEW AND EXPORT / 预览并导出", "class": "android.widget.Button"}),
            ui_xml({"text": "Export snapshot / 导出快照"}, {"text": "\n".join(lines)})]})
        smoke.session = SESSION
        return smoke, files

    @staticmethod
    def selected_scan_report():
        # Controlled Kotlin-report-shaped input; this is not an actual APK/runtime trace.
        return {"schema": 1, "record_kind": "selected_scan_report", "category": "content",
                "event": "SELECTED_SCAN_REPORT", "level": "INFO", "session_id": SESSION,
                "process_scope": "main", "report_sequence": 1,
                "fields": {"scan_attempt_id": "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee",
                           "profile": "modern-40", "terminal_status": "completed",
                           "ui_outcome": "no_groups", "native_report": None}}

    def test_exact_selected_scan_names_survive_preview_snapshot_and_zip(self):
        for index, name in enumerate(sorted(MODULE["SELECTED_SCAN_NAMES"])):
            with self.subTest(name=name):
                smoke, files = self.critical_export_fixture("selected-" + str(index), name,
                                                           report=self.selected_scan_report())
                smoke.export_preview()
                smoke.export_snapshot()
                with zipfile.ZipFile(smoke.output / "exported-diagnostics.zip", "w") as archive:
                    for entry, data in smoke.snapshot_bytes.items():
                        archive.writestr(entry, data)
                self.assertTrue(smoke.verify_zip()["preview_bytes_match"])
                self.assertEqual(smoke.snapshot_bytes[name], files[name].encode())
                self.assertNotIn("sequence", json.loads(smoke.snapshot_bytes[name]))

    def test_selected_scan_name_allowlist_and_128k_file_bound(self):
        for index, name in enumerate(("selected-scan-0.jsonl", "emulation/selected-scan-0.jsonl",
                                      "main/selected-scan-2.jsonl", "main/sub/selected-scan-0.jsonl",
                                      "../main/selected-scan-0.jsonl", "main/selected-scan-0.jsonl.bak")):
            with self.subTest(name=name):
                smoke, _ = self.critical_export_fixture("selected-reject-" + str(index), name,
                                                        report=self.selected_scan_report())
                with self.assertRaisesRegex(AssertionError, "non-allowlisted"):
                    smoke.export_preview()
        name = "main/selected-scan-0.jsonl"
        smoke, files = self.critical_export_fixture("selected-large", name, 128 * 1024 + 1,
                                                   self.selected_scan_report())
        with self.assertRaisesRegex(AssertionError, "per-entry export bound"):
            smoke.export_preview()
        files[name] = files[name] * (128 * 1024 // len(files[name]) + 1)
        smoke.snapshot_bytes = {entry: data.encode() for entry, data in files.items()}
        smoke.preview_entries = {entry: len(data) for entry, data in smoke.snapshot_bytes.items()}
        with zipfile.ZipFile(smoke.output / "exported-diagnostics.zip", "w") as archive:
            for entry, data in smoke.snapshot_bytes.items():
                archive.writestr(entry, data)
        with self.assertRaisesRegex(AssertionError, "per-entry export bound"):
            smoke.verify_zip()

    def test_selected_scan_records_use_separate_sequence_and_bounded_lines(self):
        name = "ZIP/main/selected-scan-1.jsonl"
        parse = MODULE["parse_records"]
        row = self.selected_scan_report()
        self.assertEqual(len(parse(json.dumps(row).encode(), name)), 1)
        for bad in (0, -1, True, 2**63):
            with self.subTest(report_sequence=bad):
                row["report_sequence"] = bad
                with self.assertRaisesRegex(AssertionError, "report sequence"):
                    parse(json.dumps(row).encode(), name)
        row = self.selected_scan_report()
        row["sequence"] = 1
        with self.assertRaisesRegex(AssertionError, "envelope"):
            parse(json.dumps(row).encode(), name)
        row = self.selected_scan_report()
        row["fields"]["padding"] = "x" * 32768
        with self.assertRaisesRegex(AssertionError, "line bound"):
            parse(json.dumps(row).encode(), name)

    def test_exact_critical_state_names_survive_preview_snapshot_and_zip_verification(self):
        names = [prefix + stem for prefix in ("", "main/", "emulation/")
                 for stem in ("critical-state.jsonl", "critical-state-1.jsonl", "critical-state-7.jsonl")]
        for index, name in enumerate(names):
            with self.subTest(name=name):
                smoke, files = self.critical_export_fixture("critical-" + str(index), name)
                smoke.export_preview()
                smoke.export_snapshot()
                with zipfile.ZipFile(smoke.output / "exported-diagnostics.zip", "w") as archive:
                    for entry, data in smoke.snapshot_bytes.items():
                        archive.writestr(entry, data)
                self.assertTrue(smoke.verify_zip()["preview_bytes_match"])
                row = json.loads(smoke.snapshot_bytes[name])
                self.assertEqual(row["fields"]["frames_seen"], "16")
                self.assertEqual(row["snapshot_metadata"]["fields_omitted"], 0)
                self.assertEqual(smoke.snapshot_bytes[name], files[name].encode())

    def test_critical_name_allowlist_does_not_accept_paths_keys_or_backups(self):
        for index, name in enumerate(("other/critical-state.jsonl", "main/sub/critical-state.jsonl",
                                      "../critical-state.jsonl", "main/critical-state.jsonl.bak", "prod.keys",
                                      "main/critical-state-0.jsonl", "main/critical-state-8.jsonl")):
            with self.subTest(name=name):
                smoke, _ = self.critical_export_fixture("critical-reject-" + str(index), name)
                with self.assertRaisesRegex(AssertionError, "non-allowlisted"):
                    smoke.export_preview()

    def test_critical_state_keeps_its_64k_bound_in_preview_and_zip(self):
        smoke, files = self.critical_export_fixture("critical-large", "main/critical-state.jsonl", 65537)
        with self.assertRaisesRegex(AssertionError, "per-entry export bound"):
            smoke.export_preview()
        row = json.loads(files["main/critical-state.jsonl"])
        row["padding"] = "x" * 65536
        files["main/critical-state.jsonl"] = json.dumps(row) + "\n"
        smoke.snapshot_bytes = {name: data.encode() for name, data in files.items()}
        smoke.preview_entries = {name: len(data) for name, data in smoke.snapshot_bytes.items()}
        with zipfile.ZipFile(smoke.output / "exported-diagnostics.zip", "w") as archive:
            for name, data in smoke.snapshot_bytes.items():
                archive.writestr(name, data)
        with self.assertRaisesRegex(AssertionError, "per-entry export bound"):
            smoke.verify_zip()

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

    def test_saf_confirmation_under_anr_never_taps_or_runs_recovery(self):
        for name, overlay, expected in (
                ('system-ui', PLATFORM_ANR_XML, 'blocked'),
                ('wellbeing', SYSTEM_ANR_XML, 'blocked'),
                ('unknown', SYSTEM_ANR_XML.replace('Digital Wellbeing', 'Unknown App'), 'blocked'),
                ('pine', PINE_ANR_XML, 'fail')):
            with self.subTest(name=name):
                root = ET.fromstring(ui_xml({'class': 'android.widget.Button', 'text': 'EXPORT',
                    'resource-id': 'android:id/button1', 'clickable': 'true'}))
                root.extend(ET.fromstring(overlay))
                smoke = self.smoke('saf-anr-' + name, {'xml_sequence': [ET.tostring(root, encoding='unicode')]},
                    allow_platform_anr=True, dismiss_platform_crash=True, close_system_anr=True)
                smoke.step('saf_save', smoke.saf_save)
                self.assertEqual(smoke.results['saf_save']['status'], expected)
                self.assertFalse(any('input' in row['argv'] for row in self.commands(smoke)))
                for key in ('platform_anr_waits', 'platform_crash_dismissals', 'system_anr_closes'):
                    self.assertEqual(smoke.summary[key], [])
                if name == 'pine':
                    self.assertIn('Pine application ANR observed', smoke.results['saf_save']['reason'])

    def test_all_caps_export_confirmation_reaches_saf_without_tapping_unknown_root(self):
        smoke = self.smoke("export-confirm-caps", {"xml_sequence": [
            ui_xml({"class": "android.widget.Button", "text": "EXPORT", "resource-id": "android:id/button1"}),
            ui_xml({"package": "com.google.android.documentsui", "text": "Unrecognized local root"}),
        ]})
        with self.assertRaisesRegex(Blocked, "UI observation 'saf-picker'"):
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

    def test_each_dump_uses_a_new_absent_remote_path(self):
        smoke = self.smoke('unique-dumps', {'xml_sequence': [READY_XML, READY_XML]})
        old = {smoke.remote_xml: PINE_ANR_XML}
        self.fake.with_suffix('.remote-files.json').write_text(json.dumps(old))
        for label in ('first', 'second'):
            self.assertIsNotNone(smoke.screen(label).find('Ready'))
        paths = [row['argv'][-1] for row in self.commands(smoke) if 'uiautomator' in row['argv']]
        self.assertEqual(len(paths), 2)
        self.assertEqual(len(set(paths)), 2)
        self.assertTrue(all(path not in old for path in paths))
        self.assertEqual(paths, smoke.remote_xml_paths)
        self.assertEqual(sum(row['argv'][-4:-1] == ['test', '!', '-e'] for row in self.commands(smoke)), 2)

    def test_null_root_exit_zero_cannot_reuse_old_ready_or_anr(self):
        for name, stale in (('ready', READY_XML), ('pine-anr', PINE_ANR_XML), ('system-anr', SYSTEM_ANR_XML)):
            with self.subTest(name=name):
                smoke = self.smoke('stale-' + name, {'dump_null_root': True}, close_system_anr=True)
                self.fake.with_suffix('.remote-files.json').write_text(json.dumps({smoke.remote_xml: stale}))
                with self.assertRaisesRegex(Blocked, 'fresh hierarchy'):
                    smoke.screen('null-root', deadline=time.monotonic() + 1)
                smoke.args.ui_timeout = .35
                smoke.step('settings_entry', lambda: smoke.wait_ui('null-root', lambda _: True))
                self.assertEqual(smoke.results['settings_entry']['status'], 'blocked')
                commands = self.commands(smoke)
                self.assertFalse(any('input' in row['argv'] for row in commands))
                self.assertFalse(any(row['argv'][3:5] == ['exec-out', 'cat'] for row in commands))
                dumps = [row for row in commands if 'uiautomator' in row['argv']]
                self.assertTrue(all(row['returncode'] == 0 and 'null root' in row['stderr'] for row in dumps))
                self.assertTrue(all(row['argv'][-1] != smoke.remote_xml for row in dumps))

    def test_confirm_null_root_sends_no_close_on_previous_system_anr(self):
        smoke = self.smoke('stale-confirm', {'xml_sequence': [SYSTEM_ANR_XML],
            'dump_null_root_sequence': [False, True], 'activities_sequence': [anr_focus()]}, close_system_anr=True)
        original = smoke.screen('settings')
        with self.assertRaisesRegex(Blocked, 'fresh hierarchy'):
            smoke.close_known_system_anr('settings', original, time.monotonic() + 3)
        self.assertFalse((smoke.output / '002-system-anr-confirm.xml').exists())
        self.assertFalse(any('input' in row['argv'] for row in self.commands(smoke)))
        self.assertEqual(smoke.summary['system_anr_closes'], [])

    def test_success_marker_without_new_file_never_falls_back_to_old_xml(self):
        smoke = self.smoke('dump-no-file', {'dump_no_file': True})
        self.fake.with_suffix('.remote-files.json').write_text(json.dumps({smoke.remote_xml: READY_XML}))
        with self.assertRaisesRegex(Blocked, 'No such file'):
            smoke.screen('missing-current-xml')
        self.assertFalse(any('input' in row['argv'] for row in self.commands(smoke)))

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

    def test_system_anr_close_requires_package_and_fresh_settings_after_action(self):
        for title, package in MODULE['SYSTEM_ANR_TARGETS'].items():
            with self.subTest(package=package):
                xml = SYSTEM_ANR_XML.replace("Digital Wellbeing isn't responding", title)
                smoke = self.smoke(package, {'xml_sequence': [xml, xml, READY_XML],
                    'activities_sequence': [anr_focus(package)], 'system_package_result': 'package:' + package},
                    close_system_anr=True)
                smoke.args.ui_timeout = 4
                observed = smoke.wait_ui('settings', lambda value: value.find('Ready') is not None)
                self.assertEqual(observed.number, 3)
                row, = smoke.summary['system_anr_closes']
                self.assertEqual((row['package'], row['status']), (package, 'close_sent'))
                self.assertEqual(row['source_xml'], '001-settings.xml')
                self.assertEqual(row['confirm_xml'], '002-system-anr-confirm.xml')
                self.assertTrue((smoke.output / row['trace_evidence']).is_file())
                taps = [r for r in self.commands(smoke) if 'tap' in r['argv']]
                self.assertEqual(len(taps), 1)
                self.assertEqual(taps[0]['number'], row['command_number'])
                self.assertEqual(taps[0]['argv'][-2:], ['239', '404'])
                report = json.loads((smoke.output / 'runtime-report.json').read_text())
                self.assertEqual(report['system_anr_closes'], [row])
                # This fixture is neither APK execution nor a Diagnostics pass.
                self.assertEqual((smoke.results, smoke.summary['overall']), ({}, 'blocked'))

    def test_system_anr_close_refuses_unconfirmed_or_changed_identity(self):
        fixtures = {
            'default': ({}, False, 'settings', SYSTEM_ANR_XML),
            'wrong-phase': ({}, True, 'saf-return', SYSTEM_ANR_XML),
            'unknown-title': ({}, True, 'settings', SYSTEM_ANR_XML.replace('Digital Wellbeing', 'Unknown App')),
            'wrong-owner': ({'activities_sequence': [anr_focus('fixture.other')]}, True, 'settings', SYSTEM_ANR_XML),
            'partial-owner': ({'activities_sequence': [anr_focus().splitlines()[0]]}, True, 'settings', SYSTEM_ANR_XML),
            'disagreeing-focus': ({'activities_sequence': [anr_focus().replace('mFocusedWindow=Window{e7bb3d6', 'mFocusedWindow=Window{abcd')]}, True, 'settings', SYSTEM_ANR_XML),
            'not-system': ({'system_package_result': ''}, True, 'settings', SYSTEM_ANR_XML),
            'extra-system': ({'system_package_result': 'package:com.google.android.apps.wellbeing\npackage:fixture.other'}, True, 'settings', SYSTEM_ANR_XML),
            'focus-changed': ({'activities_sequence': [anr_focus(), anr_focus('fixture.other')]}, True, 'settings', SYSTEM_ANR_XML),
            'ui-changed': ({'xml_sequence': [SYSTEM_ANR_XML, READY_XML]}, True, 'settings', SYSTEM_ANR_XML),
        }
        for resource in ('android:id/alertTitle', 'android:id/aerr_close', 'android:id/aerr_wait'):
            for attr, value in (('package', 'fixture.other'), ('enabled', 'false'), ('bounds', '[0,0][0,0]')):
                root = ET.fromstring(SYSTEM_ANR_XML)
                next(n for n in root.iter('node') if n.get('resource-id') == resource).set(attr, value)
                fixtures[resource + attr] = ({}, True, 'settings', ET.tostring(root, encoding='unicode'))
        fixtures['wrong-control'] = ({}, True, 'settings', SYSTEM_ANR_XML.replace('android:id/aerr_close', 'android:id/aerr_app_info'))
        for name, (config, enabled, label, xml) in fixtures.items():
            with self.subTest(name=name):
                smoke = self.smoke('system-guard-' + name.replace('/', '_'), {
                    'xml_sequence': [xml], 'activities_sequence': [anr_focus()], **config}, close_system_anr=enabled)
                screen = smoke.screen('settings')
                self.assertFalse(smoke.close_known_system_anr(label, screen, time.monotonic() + 3))
                self.assertFalse(any('tap' in row['argv'] for row in self.commands(smoke)))
                self.assertEqual(smoke.summary['system_anr_closes'], [])

    def test_system_anr_close_never_masks_pine_anr_or_retries(self):
        for name, xml, focus in (('title', PINE_ANR_XML, anr_focus()),
                                 ('owner', SYSTEM_ANR_XML, anr_focus(MODULE['PACKAGE'])),
                                 ('emulation-owner', SYSTEM_ANR_XML, anr_focus(MODULE['PACKAGE'] + ':emulationProcess'))):
            smoke = self.smoke('system-pine-' + name, {'xml_sequence': [xml], 'activities_sequence': [focus]}, close_system_anr=True)
            screen = smoke.screen('settings')
            with self.assertRaises(MODULE['AppAnr']):
                smoke.close_known_system_anr('settings', screen, time.monotonic() + 2)
            self.assertFalse(any('tap' in row['argv'] for row in self.commands(smoke)))
        smoke = self.smoke('system-persistent', {'xml_sequence': [SYSTEM_ANR_XML],
            'activities_sequence': [anr_focus()]}, close_system_anr=True)
        smoke.args.ui_timeout = 5
        with self.assertRaisesRegex(MODULE['PlatformCrashBlocked'], 'single ordinary Close app'):
            smoke.wait_ui('settings', lambda _: False)
        self.assertEqual(len(smoke.summary['system_anr_closes']), 1)
        self.assertEqual(len([r for r in self.commands(smoke) if 'tap' in r['argv']]), 1)

    def test_system_anr_close_timeout_is_recorded_and_not_repeated(self):
        smoke = self.smoke('system-slow-tap', {'xml_sequence': [SYSTEM_ANR_XML],
            'activities_sequence': [anr_focus()], 'tap': 2.5}, close_system_anr=True)
        screen = smoke.screen('settings')
        with self.assertRaises(MODULE['PlatformCrashBlocked']):
            smoke.close_known_system_anr('settings', screen, time.monotonic() + 1)
        self.assertEqual(smoke.summary['system_anr_closes'][0]['status'], 'blocked')
        self.assertEqual(len([r for r in self.commands(smoke) if 'tap' in r['argv']]), 1)

    def test_unknown_anr_over_underlying_ready_controls_cannot_pass(self):
        root = ET.fromstring(READY_XML)
        root.extend(ET.fromstring(SYSTEM_ANR_XML.replace('Digital Wellbeing', 'Unknown App')))
        smoke = self.smoke('unknown-cover', {'xml_sequence': [ET.tostring(root, encoding='unicode')]}, close_system_anr=True)
        with self.assertRaises(Blocked):
            smoke.wait_ui('settings', lambda screen: screen.find('Ready') is not None)
        self.assertFalse(any('tap' in row['argv'] for row in self.commands(smoke)))
        smoke = self.smoke('post-entry-close', {'xml_sequence': [SYSTEM_ANR_XML],
            'activities_sequence': [anr_focus()]}, close_system_anr=True)
        smoke.results['settings_entry'] = {'status': 'pass'}
        self.assertFalse(smoke.close_known_system_anr('settings', smoke.screen('cleanup'), time.monotonic() + 2))
        self.assertFalse(any('tap' in row['argv'] for row in self.commands(smoke)))

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
            ("pine", BLUETOOTH_CRASH_XML.replace("Bluetooth", "Pine Diagnostics Preview"), True),
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
                                             "text": "Pine Diagnostics Preview keeps stopping", "bounds": "[0,0][100,100]"})
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
            ("pine", PLATFORM_ANR_XML.replace("System UI", "Pine Diagnostics Preview"), True),
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
                    self.assertIn("Pine Diagnostics Preview isn't responding", (smoke.output / "001-pine-anr.xml").read_text())
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
        self.assertIn("Pine Diagnostics Preview isn't responding", (smoke.output / "001-settings.xml").read_text())

    def test_pine_anr_identity_does_not_match_other_titles_or_application_text(self):
        for name, xml in (
            ("other-title", PINE_ANR_XML.replace("Pine Diagnostics Preview", "Fixture App")),
            ("old-preview-title", PINE_ANR_XML.replace("Pine Diagnostics Preview", "Pine Diagnostics")),
            ("application-text", PINE_ANR_XML.replace('package="android"', 'package="io.github.pine.emu.diagnostics.preview"')),
            ("prefixed-resource", PINE_ANR_XML.replace('resource-id="android:id/alertTitle"',
                                                     'resource-id="fixture:android:id/alertTitle"')),
        ):
            with self.subTest(case=name):
                smoke = self.smoke(name, {"xml_sequence": [xml]})
                observed = MODULE['Screen'](ET.fromstring(xml), smoke.output / 'fixture.xml', 0)
                # These are not identified Pine ANRs. Real Android ANR controls
                # still cover the UI and must block, rather than pass a predicate
                # for application controls underneath. App-rendered text alone
                # is neither an OS ANR nor an application failure.
                smoke.reject_app_anr(observed)
                smoke.step("settings_entry", lambda: {"xml": smoke.wait_ui(name, lambda value: True).xml.name})
                expected = 'pass' if name == 'application-text' else 'blocked'
                self.assertEqual(smoke.results["settings_entry"]["status"], expected)
                self.assertEqual(smoke.summary["overall"], "blocked")
                self.assertFalse(any('tap' in row['argv'] for row in self.commands(smoke)))

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
        self.assertTrue(smoke.remote_xml_paths)
        self.assertTrue(all(any("rm" in row["argv"] and path in row["argv"] for row in commands) for path in smoke.remote_xml_paths))

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
                self.assertTrue(smoke.remote_xml_paths)
                self.assertTrue(all(any("rm" in row["argv"] and path in row["argv"] for row in commands) for path in smoke.remote_xml_paths))

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
        self.assertTrue(rows[1]["storage_emulated_mounted"])
        self.assertTrue(rows[1]["emulated_directory_accessible"])
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

    def test_internal_stub_alone_or_failed_real_fuse_stat_cannot_pass_readiness(self):
        cases = [("stub-only", {"storage_sequence": ["private mounted null"]}),
                 ("wrong-user", {"storage_sequence": ["private mounted null\nemulated;10 mounted null"]}),
                 ("fuse-disconnected", {"directory_sequence": [{"stdout": "", "code": 1, "stderr": "Transport endpoint is not connected"}]}),
                 ("false-success", {"directory_sequence": ["ls: /sdcard/.: Transport endpoint is not connected"]}),
                 ("regular-file", {"directory_sequence": ["-rw-rw---- 1 shell shell 0 2026-10-17 /sdcard/."]}),
                 ("late-directory", {"emulated-directory": 2, "emit_before_delay": "emulated-directory"})]
        for name, config in cases:
            with self.subTest(name=name):
                smoke = self.smoke("fuse-" + name, config, require_storage=True)
                smoke.args.boot_timeout = .8
                smoke.step("device_ready", smoke.device_ready)
                self.assert_gate_only(smoke, "blocked")
                self.assertNotIn("accepted_attempt", smoke.summary["android_readiness"])

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
