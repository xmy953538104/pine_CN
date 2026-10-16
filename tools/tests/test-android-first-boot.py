#!/usr/bin/env python3
"""Controlled subprocess regressions; no Android execution or APK validation."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import textwrap
import time
import unittest


OBSERVER = Path(__file__).with_name("wait-android-first-boot.py")
MARKER = "10-16 21:00:00.123   571   610 I ActivityManager: Finished processing BOOT_COMPLETED for u0"


class FirstBootObservations(unittest.TestCase):
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
            if args == ['shell', 'pidof', 'system_server']:
                operation, default = 'pid', '571'
            elif args == ['logcat', '-b', 'system', '-d', '-v', 'threadtime', 'ActivityManager:I', '*:S']:
                operation, default = 'logcat', ''
            else:
                raise SystemExit('Unexpected non-read-only fixture command: ' + repr(args))
            path = Path(__file__).with_suffix('.state.json')
            state = json.loads(path.read_text()) if path.is_file() else {}
            index = state.get(operation, 0)
            state[operation] = index + 1
            path.write_text(json.dumps(state))
            values = config.get(operation, [default])
            result = values[min(index, len(values) - 1)]
            if isinstance(result, str):
                result = {'stdout': result}
            def emit():
                print(result.get('stdout', ''), flush=True)
                print(result.get('stderr', ''), file=sys.stderr, flush=True)
            if result.get('emit_before_delay'):
                emit()
                time.sleep(result.get('delay', 0))
            else:
                time.sleep(result.get('delay', 0))
                emit()
            raise SystemExit(result.get('code', 0))
            '''))
        self.fake.chmod(0o755)

    def observe(self, name, config, timeout=.8, command_timeout=.4):
        self.config.write_text(json.dumps(config))
        self.fake.with_suffix('.state.json').unlink(missing_ok=True)
        output = self.root / name
        started = time.monotonic()
        process = subprocess.run([sys.executable, str(OBSERVER), '--adb', str(self.fake),
                                  '--serial', 'offline-fixture', '--output', str(output),
                                  '--timeout', str(timeout), '--command-timeout', str(command_timeout)],
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout + 3)
        self.assertLess(time.monotonic() - started, timeout + 1.5)
        result = json.loads((output / 'first-boot-result.json').read_text())
        rows = [json.loads(line) for line in (output / 'observations.jsonl').read_text().splitlines()]
        self.assertTrue(result['finished'])
        self.assertFalse(result['system_idle_claimed'])
        self.assertFalse(result['apk_validation_performed'])
        self.assertEqual(result['observation_count'], len(rows))
        for row in rows:
            for command in row['commands']:
                self.assertLessEqual(command['timeout_seconds'], command_timeout)
                for stream in ('stdout', 'stderr'):
                    data = (output / command[stream]).read_bytes()
                    self.assertEqual(hashlib.sha256(data).hexdigest(), command[stream + '_sha256'])
        return process, result, rows, output

    def test_exact_marker_requires_the_current_unchanged_system_server_pid(self):
        process, result, rows, output = self.observe('matching', {'logcat': [MARKER]})
        self.assertEqual(process.returncode, 0, process.stderr.decode())
        self.assertEqual(result['status'], 'observed')
        self.assertEqual(result['system_server_pid'], '571')
        self.assertEqual(result['marker_line'], MARKER)
        self.assertEqual([command['argv'][3] for command in rows[0]['commands']], ['shell', 'logcat', 'shell'])
        self.assertEqual(hashlib.sha256(OBSERVER.read_bytes()).hexdigest(), result['observer_sha256'])
        self.assertEqual((output / 'observer-source.py').read_bytes(), OBSERVER.read_bytes())

    def test_old_pid_changed_pid_missing_and_nonexact_markers_stay_blocked(self):
        scenarios = (
            ('old-pid', {'logcat': [MARKER.replace('571', '570')]}),
            ('changed-pid', {'pid': ['571', '572'], 'logcat': [MARKER]}),
            ('missing', {'logcat': ['No callback marker in this bounded capture']}),
            ('nonexact', {'logcat': [MARKER + ' later text']}),
            ('multiple-pids', {'pid': ['571 572'], 'logcat': [MARKER]}),
        )
        for name, config in scenarios:
            with self.subTest(case=name):
                process, result, rows, _ = self.observe(name, config, timeout=.6)
                self.assertEqual(process.returncode, 2)
                self.assertEqual(result['status'], 'blocked')
                self.assertNotIn('accepted_attempt', result)
                self.assertTrue(all(row['status'] == 'blocked' for row in rows))

    def test_late_and_timed_out_commands_cannot_accept_partial_success(self):
        scenarios = (
            ('late-logcat', {'logcat': [{'stdout': MARKER, 'delay': 2, 'emit_before_delay': True}]}, .5, .4),
            ('command-timeout', {'logcat': [{'stdout': MARKER, 'delay': 2, 'emit_before_delay': True}]}, .8, .2),
            ('late-pid-after', {'pid': ['571', {'stdout': '571', 'delay': 2, 'emit_before_delay': True}],
                                'logcat': [MARKER]}, .5, .4),
        )
        for name, config, timeout, command_timeout in scenarios:
            with self.subTest(case=name):
                process, result, rows, output = self.observe(name, config, timeout, command_timeout)
                self.assertEqual(process.returncode, 2)
                self.assertEqual(result['status'], 'blocked')
                timed_out = [command for row in rows for command in row['commands'] if command['timed_out']]
                self.assertTrue(timed_out)
                self.assertTrue(any((output / command['stdout']).read_text().strip() for command in timed_out))
                self.assertIn('timed out', result['reason'])

    def test_denied_logcat_cannot_pass_even_when_stdout_contains_the_marker(self):
        process, result, rows, output = self.observe('denied', {
            'logcat': [{'stdout': MARKER, 'stderr': 'logcat: Permission denied', 'code': 1}]})
        self.assertEqual(process.returncode, 2)
        self.assertEqual(result['status'], 'blocked')
        command = rows[0]['commands'][-1]
        self.assertEqual(command['returncode'], 1)
        self.assertIn('Permission denied', (output / command['stderr']).read_text())
        self.assertIn(MARKER, (output / command['stdout']).read_text())
        self.assertIn('returned 1', result['reason'])


if __name__ == '__main__':
    unittest.main()
