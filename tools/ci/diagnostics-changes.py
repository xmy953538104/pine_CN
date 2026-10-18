#!/usr/bin/env python3
"""Classify exact changed paths; an unavailable comparison fails open to all tests."""
import argparse
import os
import subprocess
from pathlib import Path


# Only these existing standalone Android observer inputs have a proven host-only
# consumer. Everything else is conservative: run all fast suites. In particular,
# native shell/Python checkers and JSON fixtures are transitive native inputs.
OBSERVER_INPUTS = frozenset((
    'tools/android-diagnostics-smoke.py', 'tools/android-startup-probe.py',
    'tools/tests/test-android-smoke-deadlines.py', 'tools/tests/test-android-startup-probe.py',
    'tools/tests/test-android-host-qualification.py', 'tools/tests/test-android-first-boot.py',
    'tools/tests/test-owned-ci-avd-profile.py', 'tools/tests/configure-owned-ci-avd.py',
    'tools/tests/wait-android-first-boot.py', 'tools/tests/attempt-android-runtime.sh',
))


def classify(paths):
    result = dict.fromkeys(('native', 'tools', 'windows', 'jvm'), False)
    for name in paths:
        if not name or name.startswith('docs/') or name in ('README.md', 'LICENSE'):
            continue
        if name in OBSERVER_INPUTS:
            result['tools'] = True
        else:
            return dict.fromkeys(result, True)
    return result


def changed(base):
    try:
        if not base or set(base) == {'0'}:
            raise ValueError('No prior source')
        subprocess.run(['git', 'fetch', '--no-tags', '--depth=1', 'origin', base], check=True)
        return subprocess.check_output(['git', 'diff', '--name-only', '-z', base, 'HEAD']).decode().strip('\0').split('\0')
    except (ValueError, subprocess.CalledProcessError):
        return ['.github/workflows/unknown-comparison']


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--base', required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    values = dict.fromkeys(('native', 'tools', 'windows', 'jvm'), True) if os.environ.get('FORCE_ALL') == 'true' else classify(changed(args.base))
    with args.output.open('a') as stream:
        for key, value in values.items():
            print(f'{key}={str(value).lower()}', file=stream)
    print(values)
