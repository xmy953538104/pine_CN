#!/usr/bin/env python3
"""Controlled fake-ADB subprocess tests. These never run an Android APK."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import textwrap
import unittest

RUNNER = Path(__file__).resolve().parents[1] / 'android-startup-probe.py'
PACKAGE = 'io.github.pine.emu'


class StartupProbeTests(unittest.TestCase):
    def run_probe(self, mode='ok', package=PACKAGE, extra=()):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        root = Path(temp.name)
        apk = root / 'sample.apk'
        apk.write_bytes(b'offline-test-not-an-apk')
        config = dict(mode=mode, package=package, digest=hashlib.sha256(apk.read_bytes()).hexdigest())
        (root / 'config.json').write_text(json.dumps(config))
        fake = root / 'adb'
        fake.write_text(textwrap.dedent('''\
            #!/usr/bin/env python3
            import json, pathlib, sys, time
            root = pathlib.Path(__file__).parent
            c = json.loads((root/'config.json').read_text())
            statefile = root/'state.json'
            s = json.loads(statefile.read_text()) if statefile.exists() else {'installed':False,'back':False}
            assert sys.argv[1:3] == ['-s','emulator-5554']
            a=sys.argv[3:]; out=''; code=0
            with (root/'calls.jsonl').open('a') as f: f.write(json.dumps(a)+'\\n')
            if a == ['get-state']: out='device'
            elif a[:2] == ['shell','getprop']:
                out='0' if c['mode']=='realdevice' else '1'
            elif a[:3] == ['shell','pm','path']:
                out=('package:/system/framework/framework-res.apk' if a[-1]=='android' else ('package:/data/app/test/base.apk' if s['installed'] or c['mode']=='existing' else ''))
            elif a[0]=='install': s['installed']=True; out='Performing Streamed Install\\nSuccess'
            elif a[:2]==['shell','sha256sum']:
                out=('0'*64 if c['mode']=='hash' else c['digest'])+'  /data/app/test/base.apk'
            elif a[:3]==['shell','am','start']:
                s['back']=False; out='Status: ok\\nComplete launch stdout'
                if c['mode']=='late': time.sleep(0.5)
            elif a[:3]==['shell','input','keyevent']: s['back']=c['mode']!='unresponsive'
            elif a[:2]==['exec-out','cat']:
                if c['mode']=='late-ui': time.sleep(0.6)
                label={'io.github.pine.emu': 'Pine', 'io.github.pine.emu.diagnostics': 'Pine Diagnostics', 'io.github.pine.emu.diagnostics.preview': 'Pine Diagnostics Preview'}[c['package']]
                if c['mode'] in ('anr','anr-screenshot-timeout','platform'):
                    title=label if c['mode'].startswith('anr') else 'System UI'
                    out='<hierarchy><node package="android" resource-id="android:id/alertTitle" text="'+title+' isn&apos;t responding"/><node package="android" resource-id="android:id/aerr_wait"/></hierarchy>'
                elif s['back']: out='<hierarchy><node package="com.android.launcher3" text="Home"/></hierarchy>'
                else: out='<hierarchy><node package="'+c['package']+'" text="Settings"/><node package="'+c['package']+'" resource-id="'+c['package']+':id/recycler_view"/></hierarchy>'
            elif a[:2]==['shell','pidof']: out='١٢٣' if c['mode']=='unicode-pid' else '123'
            elif a[:2]==['exec-out','screencap'] and c['mode']=='anr-screenshot-timeout': time.sleep(3)
            elif a[:3]==['exec-out','run-as',c['package']]:
                out=(('other.package' if c['mode']=='wrong-pid' else c['package'])+'\\0') if a[-1].endswith('cmdline') else 'maps'
            elif a[:4]==['shell','dumpsys','activity','lastanr']:
                if c['mode']=='evidence-error': code=1; print('permission denied',file=sys.stderr)
            elif a[:3]==['shell','am','force-stop']:
                if c['mode']=='cleanup-error' and s['installed']: code=1
            statefile.write_text(json.dumps(s))
            print(out)
            sys.exit(code)
        '''))
        fake.chmod(0o755)
        result = subprocess.run([sys.executable, str(RUNNER), '--adb', str(fake), '--serial', 'emulator-5554',
                                 '--apk', str(apk), '--package', package, '--source-commit', 'f'*40,
                                 '--output', str(root/'out'), '--fresh-test-avd', '--ui-timeout',
                                 '0.15' if mode=='late' else ('0.4' if mode=='late-ui' else '8'), *extra], capture_output=True, timeout=20)
        self.assertTrue((root/'out/startup-report.json').exists(), result.stderr.decode())
        report=json.loads((root/'out/startup-report.json').read_text())
        calls=[json.loads(line) for line in (root/'calls.jsonl').read_text().splitlines()]
        for call in calls:
            self.assertNotIn('uninstall',call); self.assertNotIn('clear',call); self.assertNotIn('root',call)
            self.assertNotIn('tap',call); self.assertNotIn('-c',call)
        self.assertFalse(report['diagnostics_functional_suite'])
        return result, report, calls, root

    def test_visible_and_back_reopen_both_packages(self):
        for package in (PACKAGE, PACKAGE+'.diagnostics', PACKAGE+'.diagnostics.preview'):
            with self.subTest(package=package):
                result,report,calls,root=self.run_probe(package=package)
                self.assertEqual(result.returncode,0,report)
                self.assertEqual(report['status'],'pass')
                self.assertEqual(report['apk_sha256'],report['installed_apk_sha256'])
                self.assertIn('Complete launch stdout',(root/'out/settings-launch.stdout').read_text())
                self.assertEqual(sum(c[:3]==['shell','am','start'] for c in calls),2)
                self.assertEqual(calls[-1],['shell','am','force-stop',package])
                self.assertFalse(any('/system/bin/debuggerd' in c or any('/maps' in a for a in c) for c in calls))
                self.assertEqual(report['observer_sha256'],hashlib.sha256(RUNNER.read_bytes()).hexdigest())
                self.assertEqual((root/'out/observer_source.py').read_bytes(),RUNNER.read_bytes())

    def test_target_anr_fails_platform_anr_blocks(self):
        for mode,status in [('anr','failed'),('platform','blocked')]:
            self.assertEqual(self.run_probe(mode)[1]['status'],status)

    def test_anr_observation_survives_screenshot_timeout(self):
        _,report,_,root=self.run_probe('anr-screenshot-timeout')
        self.assertEqual(report['status'],'failed')
        self.assertTrue(list((root/'out').glob('*-visual.error')))
        commands=[json.loads(line) for line in (root/'out/commands.jsonl').read_text().splitlines()]
        self.assertTrue(any(c['timeout'] and 'screencap' in c['args'] for c in commands))

    def test_existing_evidence_output_refused_without_overwrite(self):
        result,_,_,root=self.run_probe()
        for marker in ('commands.jsonl','startup-report.json','observer_source.py'):
            with self.subTest(marker=marker):
                reused=root/marker.replace('.', '-')
                reused.mkdir()
                (reused/marker).write_bytes(b'prior-evidence')
                args=list(result.args)
                args[args.index('--output')+1]=str(reused)
                before=(root/'calls.jsonl').read_bytes()
                attempt=subprocess.run(args,capture_output=True,timeout=5)
                self.assertEqual(attempt.returncode,2)
                self.assertEqual((reused/marker).read_bytes(),b'prior-evidence')
                self.assertEqual(list(reused.iterdir()),[reused/marker])
                self.assertEqual((root/'calls.jsonl').read_bytes(),before)

    def test_unicode_pid_is_not_a_process_id(self):
        _,report,calls,_=self.run_probe('unicode-pid')
        self.assertEqual(report['status'],'blocked')
        self.assertFalse(any('/system/bin/debuggerd' in c or any('/proc/' in a for a in c) for c in calls))

    def test_late_launch_cannot_pass(self):
        for mode in ('late', 'late-ui'):
            self.assertEqual(self.run_probe(mode)[1]['status'],'blocked')

    def test_hash_mismatch_fails_before_launch(self):
        _,report,calls,_=self.run_probe('hash')
        self.assertEqual(report['status'],'failed')
        self.assertFalse(any(c[:3]==['shell','am','start'] for c in calls))

    def test_existing_app_and_nonfixture_block_before_mutation(self):
        for mode in ('existing','realdevice'):
            _,report,calls,_=self.run_probe(mode)
            self.assertEqual(report['status'],'blocked')
            self.assertFalse(any(c[0]=='install' or c[:3]==['shell','am','force-stop'] for c in calls))

    def test_wrong_process_blocks_and_never_backtraces(self):
        _,report,calls,_=self.run_probe('wrong-pid')
        self.assertEqual(report['status'],'blocked')
        self.assertFalse(any('/system/bin/debuggerd' in c for c in calls))

    def test_back_must_change_ui(self):
        self.assertEqual(self.run_probe('unresponsive')[1]['status'],'failed')

    def test_evidence_and_cleanup_errors_do_not_mask_observation(self):
        for mode in ('evidence-error','cleanup-error'):
            self.assertEqual(self.run_probe(mode)[1]['status'],'pass')


if __name__=='__main__':
    unittest.main()
