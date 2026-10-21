#!/usr/bin/env python3
"""Inspect complete packaged bytes without executing Android test programs."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import struct
import zipfile

NODE_SHA = '3602f2bb1a10f2cbab4c36886218a33c1ab3db87290e73b033c46c77147d0237'
REQUIRED = {
    'Start-PineBionic.cmd', 'Run-PineBionic.ps1', 'host/run-once.js', 'host/node.exe',
    'device/pine-boundary', 'device/pine-lifecycle', 'device/pine-registers',
    'device/libc++_shared.so', 'README.zh-CN.md', 'RWX-RouteB-Decision.zh-CN.md',
    'corresponding-source.tar', 'licenses/Node-LICENSE', 'licenses/Pine-LICENSE.md',
}


def verify(archive, expected_commit):
    prefix = 'Pine-Bionic-Once-' + expected_commit[:8] + '/'
    hashes = {}
    with zipfile.ZipFile(archive) as bundle:
        names = bundle.namelist()
        assert len(names) == len(set(names)), 'duplicate ZIP entry'
        assert names and all(name.startswith(prefix) for name in names), 'unexpected ZIP root'
        for name in names:
            parts = PurePosixPath(name).parts
            assert not name.startswith('/') and '..' not in parts and '\\' not in name, 'unsafe ZIP path'
        assert bundle.testzip() is None, 'ZIP CRC failure'
        manifest = json.loads(bundle.read(prefix + 'manifest.json'))
        assert manifest['source_commit'] == expected_commit and manifest['source_dirty'] is False
        assert manifest['status'] == 'ANDROID_EXECUTION_PENDING' and manifest['android_executed'] is False
        assert manifest['production_gate'] == 'CLOSED' and manifest['runtime_connected'] is False
        assert manifest['real_game_records'] == 0 and manifest['abi'] == 'arm64-v8a' and manifest['minimum_api'] == 29
        listed = {item['name']: item for item in manifest['files']}
        assert len(listed) == len(manifest['files']), 'duplicate manifest entry'
        assert REQUIRED.issubset(listed), 'missing runnable kit component'
        actual = {name[len(prefix):] for name in names if not name.endswith('/')}
        assert actual == set(listed) | {'manifest.json'}, 'manifest/ZIP entry mismatch'
        for name, entry in listed.items():
            with bundle.open(prefix + name) as stream:
                digest = hashlib.sha256(); size = 0
                while block := stream.read(1024 * 1024):
                    size += len(block); digest.update(block)
            assert size == entry['bytes'] and digest.hexdigest() == entry['sha256'], name
            hashes[name] = digest.hexdigest()
        assert hashes['host/node.exe'] == NODE_SHA, 'unverified Windows runtime'
        node = bundle.read(prefix + 'host/node.exe')
        assert node[:2] == b'MZ'
        pe = struct.unpack_from('<I', node, 0x3c)[0]
        assert node[pe:pe + 4] == b'PE\0\0' and struct.unpack_from('<H', node, pe + 4)[0] == 0x8664
        for name in ('pine-boundary', 'pine-lifecycle', 'pine-registers', 'libc++_shared.so'):
            data = bundle.read(prefix + 'device/' + name)
            assert data[:6] == b'\x7fELF\x02\x01' and struct.unpack_from('<H', data, 18)[0] == 183, name
            if name != 'libc++_shared.so':
                assert b'/system/bin/linker64\0' in data and expected_commit.encode() in data, name
        assert hashes['device/libc++_shared.so'] == manifest['device_runtime_sha256']
    return {
        'schema': 1, 'archive': Path(archive).name, 'source_commit': expected_commit,
        'zip_sha256': hashlib.sha256(Path(archive).read_bytes()).hexdigest(),
        'zip_bytes': Path(archive).stat().st_size, 'verified_file_count': len(hashes),
        'inner_root': prefix, 'required_components': sorted(REQUIRED), 'file_sha256': hashes,
        'integrity': 'PASS', 'android_execution': 'ANDROID_EXECUTION_PENDING',
        'observer_installation': 'disabled', 'runtime_connected': False, 'real_game_records': 0,
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('archive', type=Path)
    parser.add_argument('--expected-commit', required=True)
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    report = verify(args.archive, args.expected_commit)
    text = json.dumps(report, indent=2) + '\n'
    if args.report:
        args.report.write_text(text, encoding='utf-8')
    print(text)


if __name__ == '__main__':
    main()
