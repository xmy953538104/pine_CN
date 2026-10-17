#!/usr/bin/env python3
"""Offline configuration guards using fake ADB; no real device or APK claim."""
import argparse
import json
import os
from pathlib import Path
import runpy
import subprocess
import tempfile
import unittest
from unittest.mock import patch

MODULE = runpy.run_path(str(Path(__file__).with_name("configure-owned-ci-avd.py")))


class OwnedAvdProfile(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.avd = self.root / 'pine-diagnostics-avd'
        (self.avd / 'PineDiagnosticsCI.avd').mkdir(parents=True)
        (self.avd / 'PineDiagnosticsCI.avd/config.ini').write_text('owned test fixture\n')
        self.fake = self.root / 'fake-adb'
        self.fake.write_text('''#!/usr/bin/env python3
import json,sys
from pathlib import Path
config=json.loads(Path(__file__).with_suffix('.json').read_text())
args=sys.argv[3:]
target='com.google.android.apps.wellbeing'
assert sys.argv[1:3]==['-s','emulator-5554']
code=0;stderr=''
if args==['shell','getprop','ro.kernel.qemu']: out=config.get('qemu','1')
elif args==['shell','getprop','ro.build.version.sdk']: out='35'
elif args==['shell','getprop','sys.boot_completed']: out='1'
elif args==['shell','cmd','package','list','packages','-u','io.github.pine.emu.diagnostics.preview']:
    out=config.get('pine','');code=config.get('pine_code',0);stderr=config.get('pine_stderr','')
elif args==['shell','cmd','package','list','packages','-s',target]: out=config.get('system','package:'+target)
elif args==['shell','cmd','package','list','packages','-d',target]: out=config.get('confirmed','package:'+target)
elif args==['shell','dumpsys','package',target]: out='Fixture package state; permissions unchanged'
elif args==['shell','settings','get','global','hide_error_dialogs']: out=config.get('hidden','null')
elif args==['shell','pm','disable-user','--user','0',target]: out='disabled-user';code=config.get('disable_code',0)
else: raise SystemExit('Unexpected profile command: '+repr(args))
print(out)
if stderr: print(stderr,file=sys.stderr)
raise SystemExit(code)
''')
        self.fake.chmod(0o755)
        self.env = {'GITHUB_ACTIONS': 'true', 'RUNNER_TEMP': str(self.root), 'ANDROID_AVD_HOME': str(self.avd)}

    def run_profile(self, name, config=None, **overrides):
        self.fake.with_suffix('.json').write_text(json.dumps(config or {}))
        args = argparse.Namespace(adb=self.fake, serial='emulator-5554', output=self.root / name, owned_ci_avd=True)
        for key, value in overrides.items(): setattr(args, key, value)
        with patch.dict(os.environ, self.env):
            result = MODULE['configure'](args)
        report = json.loads((args.output / 'profile-report.json').read_text())
        log = args.output / 'commands.jsonl'
        rows = [json.loads(line) for line in log.read_text().splitlines()] if log.exists() else []
        return result, report, rows

    def test_success_mutates_only_exact_wellbeing_enabled_state(self):
        status, report, rows = self.run_profile('success')
        self.assertEqual((status, report['status']), (0, 'verified'))
        mutations = [row['argv'][3:] for row in rows if row['argv'][3:5] == ['shell', 'pm']]
        self.assertEqual(mutations, [['shell', 'pm', 'disable-user', '--user', '0', MODULE['TARGET']]])
        self.assertFalse(any(token in row['argv'] for row in rows for token in ('clear','uninstall','grant','revoke','appops','root','setprop','put')))
        self.assertFalse(report['permission_changes_requested'])
        self.assertFalse(report['global_anr_changes_requested'])

    def test_wrong_scope_or_identity_stops_before_mutation(self):
        cases = [('no-opt-in', {}, {'owned_ci_avd': False}), ('physical-serial', {}, {'serial':'usb-device'}),
                 ('not-qemu', {'qemu':'0'}, {}), ('existing-pine', {'pine':'package:/data/app/base.apk'}, {}),
                 ('query-failed', {'pine_code':1}, {}), ('query-unknown', {'pine_stderr':'Package service not ready'}, {}),
                 ('query-error-text', {'pine':'Error: unknown package service'}, {}),
                 ('not-system-target', {'system':''}, {}), ('hidden-errors', {'hidden':'1'}, {})]
        for name, config, overrides in cases:
            with self.subTest(name=name):
                status, report, rows = self.run_profile(name, config, **overrides)
                self.assertEqual((status, report['status']), (1, 'blocked'))
                self.assertFalse(any('disable-user' in row['argv'] for row in rows))
        self.env['GITHUB_ACTIONS']='false'
        self.assertEqual(self.run_profile('not-ci')[0], 1)
        self.env['GITHUB_ACTIONS']='true'
        self.env['ANDROID_AVD_HOME']=str(self.root.parent / 'owner-avd')
        self.assertEqual(self.run_profile('outside-temp')[0], 1)

    def test_pine_absence_query_timeout_is_unknown_and_never_mutates(self):
        original = subprocess.run
        def run(command, **kwargs):
            if command[3:] == ['shell','cmd','package','list','packages','-u',MODULE['PINE']]:
                raise subprocess.TimeoutExpired(command, 20, output=b'', stderr=b'Package query timed out')
            return original(command, **kwargs)
        with patch.object(MODULE['subprocess'], 'run', side_effect=run):
            status, report, rows = self.run_profile('query-timeout')
        self.assertEqual((status, report['status']), (1, 'blocked'))
        self.assertTrue(any(row['label']=='pine-absent' and row['timed_out'] for row in rows))
        self.assertFalse(any('disable-user' in row['argv'] for row in rows))

    def test_rejected_or_unconfirmed_change_has_no_retry_or_fallback(self):
        for name, config in (('rejected', {'disable_code':1}), ('unconfirmed', {'confirmed':''})):
            with self.subTest(name=name):
                status, report, rows = self.run_profile(name, config)
                self.assertEqual((status, report['status']), (1, 'blocked'))
                self.assertEqual(sum('disable-user' in row['argv'] for row in rows), 1)
                self.assertFalse(any(token in row['argv'] for row in rows for token in ('root','setprop','put','clear')))


if __name__=='__main__': unittest.main()
