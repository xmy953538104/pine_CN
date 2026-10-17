#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Verify actual generated Debug compiler commands; bind evidence to the built APK."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess


REQUIRED = ('skyline/diagnostics/diagnostics.cpp', 'skyline/services/fssrv/IStorage.cpp',
            'skyline/gpu/presentation_engine.cpp', 'skyline/nce.cpp')


def verify(arguments, expected):
    optimizations = [arg for arg in arguments if re.fullmatch(r'-O(?:[0-3sgz]|fast)', arg)]
    if not optimizations or optimizations[-1] != '-O'+expected:
        raise ValueError('Effective optimization differs from the controlled candidate')
    definitions = [arg[2:] if arg != '-D' else arguments[index+1]
                   for index,arg in enumerate(arguments) if arg.startswith('-D') and (arg != '-D' or index+1 < len(arguments))]
    if any(value.split('=', 1)[0] in ('NDEBUG', 'BOOST_DISABLE_ASSERTS') for value in definitions):
        raise ValueError('Assertions disabled')
    frame_flags = [arg for arg in arguments if arg in ('-fomit-frame-pointer', '-fno-omit-frame-pointer')]
    if '-g3' not in arguments or not frame_flags or frame_flags[-1] != '-fno-omit-frame-pointer':
        raise ValueError('Debug symbols or frame pointers missing')
    if '-fno-stack-protector' in arguments or '-ffast-math' in arguments or '-Ofast' in arguments:
        raise ValueError('Release protection/math changes are outside this experiment')
    if '-fstack-protector-strong' not in arguments:
        raise ValueError('Original Android stack protector missing')


def collect(root, expected):
    found = {}
    for path in root.rglob('compile_commands.json'):
        if 'Debug' not in str(path):
            continue
        for row in json.loads(path.read_text()):
            source = row['file'].replace('\\', '/')
            suffix = next((name for name in REQUIRED if source.endswith('/'+name)), None)
            if suffix is None:
                continue
            args = row.get('arguments') or shlex.split(row['command'])
            verify(args, expected)
            found[suffix] = {'source': source, 'directory': row['directory'], 'arguments': args,
                             'effective_optimization': 'O'+expected, 'assertions_disabled': False}
    if set(found) != set(REQUIRED):
        raise ValueError('Missing generated commands: '+str(set(REQUIRED)-set(found)))
    return found


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--optimization', choices=('0', '2'), required=True)
    parser.add_argument('--root', type=Path, default=Path('app/.cxx'))
    parser.add_argument('--apk', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    rows = collect(args.root, args.optimization)
    with args.apk.open('rb') as stream:
        sha = hashlib.file_digest(stream, 'sha256').hexdigest()
    report = {'schema': 1, 'source_commit': subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip(),
              'apk_sha256': sha, 'native_debug_optimization': 'O'+args.optimization,
              'scope': 'Actual compiler commands for required Debug core translation units; not game speed acceptance.',
              'commands': rows}
    args.output.write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps({key:value for key,value in report.items() if key!='commands'}))


if __name__ == '__main__':
    main()
