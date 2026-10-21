#!/usr/bin/env python3
"""Build an isolated API29+ ARM64 kit. Compilation is NOT Android execution."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[2]
CPP = ROOT / 'app/src/main/cpp/skyline'
NODE_SHA = '3602f2bb1a10f2cbab4c36886218a33c1ab3db87290e73b033c46c77147d0237'
NODE_ORIGIN = 'https://nodejs.org/dist/v24.19.0/win-x64/node.exe'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--ndk-cxx', type=Path, required=True)
    parser.add_argument('--windows-node', type=Path, required=True)
    parser.add_argument('--node-license', type=Path, required=True)
    parser.add_argument('--allow-dirty', action='store_true', help='Development only; manifest marks dirty and is not an acceptance artifact')
    args = parser.parse_args()
    commit = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
    dirty = bool(subprocess.check_output(['git', 'status', '--porcelain'], cwd=ROOT, text=True).strip())
    if dirty and not args.allow_dirty:
        parser.error('Commit source changes first; delivery requires an exact clean source tree')
    if sha(args.windows_node) != NODE_SHA:
        parser.error('Windows Node does not match the exact official v24.19.0 artifact')
    out = args.output.resolve();out.mkdir(parents=True, exist_ok=True)
    kit = out / ('Pine-Bionic-Once-' + commit[:8]);kit.mkdir()
    for name in ('device', 'host', 'licenses', 'build-evidence'):
        (kit / name).mkdir()
    for program, script in [('lifecycle', 'test-runtime-lifecycle.py'), ('registers', 'test-runtime-pipeline.py')]:
        target = kit / 'build-evidence' / program
        command = ['python3', str(ROOT / 'tools/tests' / script), '--aarch64', '--compile-only', '--cxx', str(args.ndk_cxx.absolute()), '--output', str(target)]
        with (kit / 'build-evidence' / (program + '.log')).open('w') as log:
            subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True)
        shutil.copy2(target / ('pipeline' if program == 'registers' else 'lifecycle'), kit / 'device' / ('pine-' + program))
    command = [str(args.ndk_cxx.absolute()), '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror',
               '-Wno-missing-field-initializers', '-UNDEBUG', '-Wl,-z,max-page-size=16384',
               '-DPINE_BOUNDARY_SOURCE_COMMIT="' + commit + '"', '-I' + str(ROOT / 'tests/runtime_observer'),
               str(ROOT / 'tests/runtime_observer/android_supervisor.cpp'), '-o', str(kit / 'device/pine-boundary')]
    subprocess.run(command, cwd=ROOT, check=True)
    toolchain = args.ndk_cxx.absolute().parent.parent
    runtime = toolchain / 'sysroot/usr/lib/aarch64-linux-android/libc++_shared.so'
    shutil.copy2(runtime, kit / 'device/libc++_shared.so')
    shutil.copy2(args.windows_node, kit / 'host/node.exe')
    shutil.copy2(args.node_license, kit / 'licenses/Node-LICENSE')
    shutil.copy2(ROOT / 'LICENSE.md', kit / 'licenses/Pine-LICENSE.md')
    ndk = toolchain.parent.parent.parent.parent
    for notice in (ndk / 'NOTICE', ndk / 'NOTICE.toolchain'):
        if notice.exists():shutil.copy2(notice, kit / 'licenses' / notice.name)
    for name in ('Start-PineBionic.cmd', 'Run-PineBionic.ps1', 'README.zh-CN.md'):
        shutil.copy2(ROOT / 'tools/android-bionic' / name, kit / name)
    shutil.copy2(ROOT / 'tools/android-bionic/run-once.js', kit / 'host/run-once.js')
    (kit / 'build-evidence/source.patch').write_bytes(subprocess.check_output(['git', 'diff', '55b3fab1..HEAD'], cwd=ROOT))
    source_paths = ['LICENSE.md', 'tools/android-bionic', 'tools/tests/test-runtime-lifecycle.py', 'tools/tests/test-runtime-pipeline.py',
                    'tests/runtime_observer', 'app/src/main/cpp/skyline/diagnostics', 'app/src/main/cpp/skyline/nce',
                    'app/src/main/cpp/skyline/common', 'app/src/main/cpp/skyline/nce.cpp', 'app/src/main/cpp/skyline/loader',
                    'app/src/main/cpp/skyline/kernel/types/KThread.cpp', 'app/src/main/cpp/skyline/kernel/types/KProcess.cpp']
    (kit / 'corresponding-source.tar').write_bytes(subprocess.check_output(['git', 'archive', 'HEAD', *source_paths], cwd=ROOT))
    metadata = {'schema': 1, 'source_commit': commit, 'source_dirty': dirty, 'baseline': '55b3fab139e0891d3278715406ed1f843686b687',
                'status': 'ANDROID_EXECUTION_PENDING', 'android_executed': False, 'minimum_api': 29, 'abi': 'arm64-v8a',
                'production_gate': 'CLOSED', 'runtime_connected': False, 'real_game_records': 0,
                'scope': 'isolated_fixture_not_pine_app_or_horizon', 'supervisor_command': command,
                'compiler_version': subprocess.check_output([str(args.ndk_cxx), '--version'], text=True).splitlines()[0],
                'host_runtime': {'version': 'Node24.19.0 official Windows x64', 'origin': NODE_ORIGIN, 'sha256': NODE_SHA},
                'device_runtime_sha256': sha(runtime), 'files': []}
    for file in sorted(kit.rglob('*')):
        if file.is_file():metadata['files'].append({'name': file.relative_to(kit).as_posix(), 'sha256': sha(file), 'bytes': file.stat().st_size})
    (kit / 'manifest.json').write_text(json.dumps(metadata, indent=2) + '\n', encoding='utf-8')
    archive = out / (kit.name + '.zip')
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as bundle:
        for file in sorted(kit.rglob('*')):
            if file.is_file():bundle.write(file, file.relative_to(out).as_posix())
    print(json.dumps({'zip': str(archive), 'sha256': sha(archive), 'bytes': archive.stat().st_size, 'commit': commit, 'status': 'ANDROID_EXECUTION_PENDING'}))


if __name__ == '__main__':
    main()
