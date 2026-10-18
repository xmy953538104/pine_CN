#!/usr/bin/env python3
"""Classify exact changed paths; an unavailable comparison fails open to all tests."""
import argparse
import os
import subprocess
from pathlib import Path


def classify(paths):
    result = dict.fromkeys(('native', 'tools', 'windows', 'jvm'), False)
    for name in paths:
        if not name or name.startswith('docs/') or name.endswith('.md'):
            continue
        if name.startswith(('.github/workflows/', 'tools/ci/', 'gradle/')) or name.endswith('.gradle') or name == '.gitmodules':
            return dict.fromkeys(result, True)
        if name.startswith('app/'):
            return dict.fromkeys(result, True)
        if name.startswith(('app/src/main/cpp/', 'tests/diagnostics/')) or name in (
                'tools/tests/run-native-diagnostics.sh', 'tools/tests/run-content-diagnostics.sh',
                'tools/tests/run-mapped-vma.sh'):
            result['native'] = True
        if name.startswith(('app/src/main/java/', 'app/src/main/res/', 'app/src/test/')) or name in (
                'app/src/main/AndroidManifest.xml', 'app/proguard-rules.pro'):
            result['jvm'] = True
        if name.startswith('tools/'):
            result['tools'] = True
        if name.endswith(('.ps1', '.cs')) or '/fake-adb' in name or name.endswith('test-ctrlc.py'):
            result['windows'] = True
        if not name.startswith(('tools/', 'tests/diagnostics/')):
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
