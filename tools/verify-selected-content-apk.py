#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Bind opt-in Debug feature compile evidence to an exact APK, not runtime success."""
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess
import sys
import zipfile

REQUIRED = ('/emu_jni.cpp', '/selected_jni.cpp', '/selected_scan_jni.cpp', '/skyline/loader/selected_launch.cpp',
            '/skyline/loader/selected_content.cpp', '/skyline/loader/selected_execute.cpp',
            '/skyline/catalog/native_adapter.cpp', '/skyline/catalog/wire.cpp',
            '/skyline/catalog/scan_report.cpp')


def definitions(arguments):
    result = {}
    index = 0
    while index < len(arguments):
        argument = arguments[index]
        if argument in ('-D', '-U'):
            index += 1
            if index == len(arguments):
                raise ValueError('incomplete compiler definition')
            value = arguments[index]
            define = argument == '-D'
        elif argument.startswith(('-D', '-U')):
            value, define = argument[2:], argument.startswith('-D')
        else:
            index += 1
            continue
        name, separator, value = value.partition('=')
        if define:
            result[name] = value if separator else '1'
        else:
            result.pop(name, None)
        index += 1
    return result


def collect(root):
    found = {}
    for path in root.rglob('compile_commands.json'):
        if 'Debug' not in str(path):
            continue
        for row in json.loads(path.read_text()):
            source = row['file'].replace('\\', '/')
            suffix = next((name for name in REQUIRED if source.endswith(name)), None)
            if suffix is None:
                continue
            arguments = row.get('arguments') or shlex.split(row['command'])
            if definitions(arguments).get('PINE_ENABLE_SELECTED_CONTENT') != '1':
                raise ValueError('selected feature missing from actual compiler command: ' + suffix)
            if suffix in found and found[suffix]['arguments'] != arguments:
                raise ValueError('conflicting selected feature compiler commands: ' + suffix)
            found[suffix] = {'source': source, 'arguments': arguments}
    if set(found) != set(REQUIRED):
        raise ValueError('missing selected feature compilation: ' + str(set(REQUIRED) - set(found)))
    return found


def main():
    apk, expected = Path(sys.argv[1]), sys.argv[2]
    actual = subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip()
    if not re.fullmatch('[a-f0-9]{40}', expected) or expected != actual:
        raise ValueError('source commit differs from requested APK')
    rows = collect(Path('app/.cxx'))
    build_config = Path('app/build/generated/source/buildConfig/full/debug/emu/skyline/BuildConfig.java')
    if not re.search(r'boolean SELECTED_CONTENT_ENABLED\s*=\s*true\s*;', build_config.read_text()):
        raise ValueError('actual generated Debug BuildConfig does not enable selected entry')
    with zipfile.ZipFile(apk) as archive:
        native = archive.read('lib/arm64-v8a/libskyline.so')
    # The feature compile commands and generated BuildConfig are the gate;
    # packaged JNI symbol strings additionally catch omission of the bridge TU.
    for name in ('Java_emu_skyline_loader_SelectedContentBridge_isAvailable',
                 'Java_emu_skyline_loader_SelectedContentBridge_inspect',
                 'Java_emu_skyline_loader_SelectedContentBridge_inspectObserved',
                 'Java_emu_skyline_EmulationActivity_executeSelectedApplication'):
        if name.encode() + b'\0' not in native:
            raise ValueError('packaged native JNI entry absent: ' + name)
    report = {'schema': 1, 'source_commit': actual,
              'apk_sha256': hashlib.sha256(apk.read_bytes()).hexdigest(),
              'skyline_elf_sha256': hashlib.sha256(native).hexdigest(),
              'selected_content_compiled': True, 'selected_debug_build_config': True,
              'default_build_enabled': False, 'bktr_supported': False,
              'runtime_validation': 'not_performed_by_this_build_verifier', 'commands': rows}
    apk.with_name('selected-content-build-identity.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({key: value for key, value in report.items() if key != 'commands'}))


if __name__ == '__main__':
    main()
