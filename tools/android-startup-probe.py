#!/usr/bin/env python3
"""Bounded Settings-only probe for a fresh, owned official test AVD.
Not the Diagnostics functional suite, physical ARM64 validation or game testing.
No clearing, uninstalling, rooting, security changes or ANR button interaction.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import time
import xml.etree.ElementTree as ET

PACKAGES = ('io.github.pine.emu', 'io.github.pine.emu.diagnostics', 'io.github.pine.emu.diagnostics.preview')


class Blocked(Exception):
    pass


class Failed(Exception):
    pass


class Probe:
    def __init__(self, args):
        self.a = args
        self.out = Path(args.output)
        if self.out.exists() and any(self.out.iterdir()):
            raise Blocked('Output directory is not empty; refusing to mix or overwrite run evidence')
        self.out.mkdir(parents=True, exist_ok=True)
        observer = Path(__file__).read_bytes()
        (self.out / 'observer_source.py').write_bytes(observer)
        self.index = 0
        self.owned = False
        self.installed = False
        self.deadline = None
        self.report = dict(schema=1, probe='settings-startup-only', source_commit=args.source_commit,
                           observer_source='observer_source.py', observer_sha256=hashlib.sha256(observer).hexdigest(),
                           package=args.package, apk_sha256=hashlib.sha256(Path(args.apk).read_bytes()).hexdigest(),
                           status='blocked', physical_arm64_verified=False, diagnostics_functional_suite=False)

    def command(self, *args, name=None, timeout=None, required=True):
        self.index += 1
        name = name or f'command-{self.index:03d}'
        limit = timeout if timeout is not None else self.a.command_timeout
        if self.deadline is not None:
            remaining = self.deadline - time.monotonic()
            if remaining <= 0:
                raise Blocked('Total UI budget exhausted; late observations cannot pass')
            limit = min(limit, remaining)
        started = time.monotonic()
        process = subprocess.Popen([self.a.adb, '-s', self.a.serial, *args], stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, start_new_session=True)
        expired = False
        try:
            stdout, stderr = process.communicate(timeout=limit)
        except subprocess.TimeoutExpired:
            expired = True
            os.killpg(process.pid, signal.SIGKILL)
            stdout, stderr = process.communicate(timeout=2)
        elapsed = time.monotonic() - started
        (self.out / (name + '.stdout')).write_bytes(stdout)
        (self.out / (name + '.stderr')).write_bytes(stderr)
        with (self.out / 'commands.jsonl').open('a') as stream:
            stream.write(json.dumps(dict(args=list(args), evidence=name, returncode=process.returncode,
                                         elapsed=elapsed, timeout=expired)) + '\n')
        if expired or (self.deadline is not None and time.monotonic() >= self.deadline):
            raise Blocked(f'{name} exceeded deadline; late observations rejected')
        if required and process.returncode:
            raise Blocked(f'{name} failed with exit {process.returncode}')
        return stdout.decode('utf-8', errors='replace').strip()

    def snapshot(self, name):
        remote = f'/data/local/tmp/pine-startup-{os.getpid()}-{self.index}.xml'
        self.command('shell', 'uiautomator', 'dump', remote, name=name + '-dump')
        text = self.command('exec-out', 'cat', remote, name=name + '-xml')
        (self.out / (name + '.xml')).write_text(text)
        try:
            root = ET.fromstring(text)
        except ET.ParseError as error:
            raise Blocked('Invalid UI hierarchy') from error
        nodes = list(root.iter('node'))
        labels = [n.get('text', '') for n in nodes if n.get('package') == 'android' and
                  n.get('resource-id') == 'android:id/alertTitle']
        anr_controls = any(n.get('package') == 'android' and n.get('resource-id') in
                           ('android:id/aerr_wait', 'android:id/aerr_close') for n in nodes)
        label = dict(zip(PACKAGES, ('Pine', 'Pine Diagnostics', 'Pine Diagnostics Preview')))[self.a.package]
        if anr_controls:
            # XML already establishes the outcome. Optional visual evidence must
            # never downgrade a known app ANR when its capture times out.
            try:
                self.visual(name, timeout=min(2, self.a.command_timeout))
            except Exception as error:
                (self.out / (name + '-visual.error')).write_text(str(error))
            if f"{label} isn't responding" in labels:
                raise Failed('Exact target Pine ANR dialog observed')
            raise Blocked('Platform or other application ANR dialog; no buttons clicked')
        self.visual(name)
        own = [n for n in nodes if n.get('package') == self.a.package]
        toolbar = any(n.get('text') in ('Settings', '设置') for n in own)
        content = any(n.get('resource-id', '').endswith(('/recycler_view', '/settings')) or
                      n.get('text') in ('Content', '内容', 'Appearance', '外观') for n in own)
        return toolbar and content

    def visual(self, name, timeout=None):
        self.command('exec-out', 'screencap', '-p', name=name + '-screenshot', timeout=timeout, required=False)
        (self.out / (name + '.png')).write_bytes((self.out / (name + '-screenshot.stdout')).read_bytes())
        self.command('shell', 'dumpsys', 'window', name=name + '-window', timeout=timeout, required=False)

    def visible(self, name):
        attempt = 0
        while True:
            attempt += 1
            if self.snapshot(f'{name}-{attempt}'):
                return
            if time.monotonic() >= self.deadline:
                raise Blocked('Settings not observed within UI budget')
            time.sleep(min(0.2, max(0, self.deadline - time.monotonic())))

    def launch(self, name):
        result = self.command('shell', 'am', 'start', '-W', '-n',
                              self.a.package + '/emu.skyline.settings.SettingsActivity', name=name)
        if 'Error:' in result or 'Exception' in result:
            raise Failed('Android rejected Settings activity launch')

    def execute(self):
        if not self.a.fresh_test_avd or not re.fullmatch(r'emulator-\d+', self.a.serial):
            raise Blocked('Requires explicit fresh owned test AVD and emulator serial')
        if self.command('get-state', name='device-state') != 'device':
            raise Blocked('Device is not authorized/online')
        if self.command('shell', 'getprop', 'ro.kernel.qemu', name='emulator-property') != '1':
            raise Blocked('Target is not the expected emulator fixture')
        if self.command('shell', 'getprop', 'sys.boot_completed', name='boot-property') != '1':
            raise Blocked('Android boot incomplete')
        self.command('shell', 'getprop', name='device-properties')
        framework = self.command('shell', 'pm', 'path', 'android', name='package-service')
        if not framework.startswith('package:/'):
            raise Blocked('Package manager not ready; cannot establish fresh target state')
        existing = self.command('shell', 'pm', 'path', self.a.package, name='preexisting-package', required=False)
        if existing:
            raise Blocked('Target package already exists; refusing to clear or replace app data')
        self.owned = True
        for package in PACKAGES:
            self.command('shell', 'am', 'force-stop', package)
        result = self.command('install', self.a.apk, name='install', timeout=self.a.install_timeout)
        if not result.splitlines() or result.splitlines()[-1] != 'Success':
            raise Blocked('APK installation did not report Success')
        self.installed = True
        paths = self.command('shell', 'pm', 'path', self.a.package, name='installed-path').splitlines()
        if len(paths) != 1 or not paths[0].startswith('package:/data/app/') or not paths[0].endswith('/base.apk'):
            raise Failed('Installed package identity/path is not the expected single APK')
        path = paths[0][len('package:'):]
        digest = self.command('shell', 'sha256sum', path, name='installed-apk-sha256').split()
        self.report['installed_apk_sha256'] = digest[0] if digest else None
        if not digest or digest[0] != self.report['apk_sha256']:
            raise Failed('Installed APK hash differs from requested artifact')
        self.deadline = time.monotonic() + self.a.ui_timeout
        self.launch('settings-launch')
        self.visible('settings-visible')
        self.command('shell', 'input', 'keyevent', 'KEYCODE_BACK', name='settings-back')
        back_deadline = min(self.deadline, time.monotonic() + 3)
        back_attempt = 0
        while True:
            back_attempt += 1
            if not self.snapshot(f'after-back-{back_attempt}'):
                break
            if time.monotonic() >= back_deadline:
                raise Failed('Settings stayed visible after Back; interaction not demonstrated')
            time.sleep(min(0.2, max(0, back_deadline - time.monotonic())))
        self.launch('settings-relaunch')
        self.visible('settings-interactive')
        if time.monotonic() >= self.deadline:
            raise Blocked('Late UI result')
        pids = self.command('shell', 'pidof', self.a.package, name='interactive-pids').split()
        if len(pids) != 1 or not re.fullmatch(r'[1-9][0-9]*', pids[0]):
            raise Blocked('Cannot establish unique target Settings process')
        owner = self.command('exec-out', 'run-as', self.a.package, 'cat', f'/proc/{pids[0]}/cmdline', name='interactive-process-owner')
        if owner.rstrip('\x00') != self.a.package:
            raise Blocked('Observed PID does not belong to target fixture package')
        self.report['status'] = 'pass'
        self.report['reason'] = 'Own Settings hierarchy observed, Back changed UI, and Settings reopened within budget'

    def evidence(self):
        self.deadline = None
        if not self.owned:
            return
        def capture(*args, name):
            try:
                return self.command(*args, name=name, timeout=5, required=False)
            except Exception as error:
                (self.out / (name + '.error')).write_text(str(error))
                return ''
        capture('shell', 'dumpsys', 'activity', 'lastanr', name='lastanr')
        capture('logcat', '-d', '-t', '600', '-b', 'all', '-v', 'threadtime', name='final-logcat')
        capture('shell', 'dumpsys', 'activity', 'activities', name='final-activities')
        if not self.installed or self.report['status'] == 'pass':
            return
        pids = capture('shell', 'pidof', self.a.package, name='target-pids').split()
        for pid in pids[:4]:
            if not re.fullmatch(r'[1-9][0-9]*', pid):
                continue
            owner = capture('exec-out', 'run-as', self.a.package, 'cat', f'/proc/{pid}/cmdline', name=f'pid-{pid}-owner')
            if owner.rstrip('\x00') != self.a.package:
                (self.out / f'pid-{pid}-ownership-blocked.txt').write_text('Cannot establish target process ownership; no maps/debuggerd')
                continue
            capture('exec-out', 'run-as', self.a.package, 'cat', f'/proc/{pid}/maps', name=f'pid-{pid}-maps')
            capture('shell', '/system/bin/debuggerd', '-b', pid, name=f'pid-{pid}-backtrace')

    def run(self):
        try:
            self.execute()
        except Failed as error:
            self.report.update(status='failed', reason=str(error))
        except Exception as error:
            self.report.update(status='blocked', reason=f'{type(error).__name__}: {error}')
        finally:
            try:
                self.evidence()
            except Exception as error:
                self.report['evidence_error'] = str(error)
            if self.owned and self.installed:
                try:
                    self.command('shell', 'am', 'force-stop', self.a.package, name='final-force-stop', timeout=5, required=False)
                except Exception as error:
                    self.report['cleanup_error'] = str(error)
            (self.out / 'startup-report.json').write_text(json.dumps(self.report, indent=2) + '\n')
        return {'pass': 0, 'failed': 1, 'blocked': 2}[self.report['status']]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for field in ('adb', 'serial', 'apk', 'source-commit', 'output'):
        parser.add_argument('--' + field, required=True)
    parser.add_argument('--package', required=True, choices=PACKAGES)
    parser.add_argument('--fresh-test-avd', action='store_true', help='Attest this is a newly created, owned test AVD')
    parser.add_argument('--command-timeout', type=float, default=30)
    parser.add_argument('--install-timeout', type=float, default=900)
    parser.add_argument('--ui-timeout', type=float, default=120)
    args = parser.parse_args()
    if not re.fullmatch('[0-9a-f]{40}', args.source_commit):
        parser.error('source-commit must be a full lowercase Git SHA')
    if not 0 < args.command_timeout <= 60 or not 0 < args.install_timeout <= 900 or not 0 < args.ui_timeout <= 120:
        parser.error('budgets must be positive: command <=60, install <=900, UI <=120 seconds')
    try:
        probe = Probe(args)
    except Blocked as error:
        print(f'blocked: {error}')
        raise SystemExit(2)
    raise SystemExit(probe.run())


if __name__ == '__main__':
    main()
