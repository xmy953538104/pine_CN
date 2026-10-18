#!/usr/bin/env python3
"""Compile and exercise production virtual RomFS, RomFileSystem and IStorage."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import virtual_romfs_fixture as fixture

ROOT = Path(__file__).resolve().parents[2]
PARSER = argparse.ArgumentParser()
PARSER.add_argument('--sanitize', action='store_true')
PARSER.add_argument('--evidence-dir', type=Path)
ARGS, REMAINING = PARSER.parse_known_args()


class VirtualRomfsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix='pine-virtual-romfs-')
        cls.root = Path(cls.build.name)
        cls.evidence = ARGS.evidence_dir.resolve() if ARGS.evidence_dir else cls.root / 'evidence'
        cls.evidence.mkdir(parents=True, exist_ok=True)
        fixture.write_fixture(cls.root)
        (cls.root / 'empty.romfs').write_bytes(fixture.romfs({}))
        shutil.copyfile(ROOT / 'tests/virtual-romfs/fixtures/root-nested.romfs', cls.root / 'root-nested.romfs')
        (cls.root / 'case-collision.romfs').write_bytes(fixture.romfs({'CASE': b'a', 'case': b'b'}))
        (cls.root / 'path-conflict.romfs').write_bytes(fixture.romfs({'same': b'x'}, ['same']))
        cls.binary = cls.root / 'virtual-romfs-test'
        command = [os.environ.get('CXX', 'c++'), '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                   '-Wno-unused-parameter', '-Wno-sign-compare', '-Wno-reorder', '-Wno-missing-field-initializers', '-pthread',
                   '-Itests/virtual-romfs/stubs', '-Itests/diagnostics/storage_stubs', '-Iapp/src/main/cpp/skyline',
                   'app/src/main/cpp/skyline/vfs/prototype/romfs_detail.cpp',
                   'app/src/main/cpp/skyline/vfs/prototype/virtual_romfs.cpp',
                   'app/src/main/cpp/skyline/vfs/rom_filesystem.cpp',
                   'app/src/main/cpp/skyline/services/fssrv/IStorage.cpp',
                   'tests/virtual-romfs/virtual_romfs_test.cpp', '-o', str(cls.binary)]
        if ARGS.sanitize:
            command[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie']
        run = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
        (cls.evidence / 'compile.txt').write_text(run.stdout + run.stderr)
        if run.returncode:
            raise RuntimeError(run.stdout + run.stderr)
        cls.command = command
        run = subprocess.run([str(cls.binary), str(cls.root), str(cls.evidence)], capture_output=True, text=True, timeout=60)
        (cls.evidence / 'native-test.txt').write_text(run.stdout + run.stderr)
        if run.returncode:
            raise RuntimeError(f'native test exit {run.returncode}:\n{run.stdout}{run.stderr}')
        shutil.copyfile(cls.root / 'base.romfs', cls.evidence / 'base.romfs')
        shutil.copyfile(cls.root / 'program.bin', cls.evidence / 'program.bin')
        cls.result = json.loads((cls.evidence / 'result.json').read_text())

    @classmethod
    def tearDownClass(cls):
        commit = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
        tracked = subprocess.check_output(['git', 'diff', '--binary', 'HEAD'], cwd=ROOT)
        untracked = subprocess.check_output(['git', 'ls-files', '--others', '--exclude-standard', '-z'], cwd=ROOT).decode().strip('\0').split('\0')
        untracked = [name for name in untracked if name and not (ROOT / name).resolve().is_relative_to(cls.evidence)]
        files = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(cls.evidence.iterdir()) if p.is_file() and p.name != 'manifest.json'}
        for name in ('virtual-romfs-prototype.md',):
            if (ROOT / 'docs' / name).is_file():
                shutil.copyfile(ROOT / 'docs' / name, cls.evidence / name)
                files[name] = hashlib.sha256((cls.evidence / name).read_bytes()).hexdigest()
        for name in ('root-nested.romfs', 'root-nested.expected.json'):
            shutil.copyfile(ROOT / 'tests/virtual-romfs/fixtures' / name, cls.evidence / name)
            files[name] = hashlib.sha256((cls.evidence / name).read_bytes()).hexdigest()
        (cls.evidence / 'manifest.json').write_text(json.dumps({
            'source_commit': commit, 'source_dirty': bool(tracked or untracked),
            'tracked_source_diff_sha256': hashlib.sha256(tracked).hexdigest() if tracked else None,
            'untracked_source_files': untracked, 'compile_command': cls.command,
            'sanitizers': 'address,undefined' if ARGS.sanitize else 'none',
            'synthetic_only': True, 'sha256': files,
        }, indent=2) + '\n')
        cls.build.cleanup()

    def test_real_cpp_integration_and_truthful_evidence(self):
        self.assertEqual(self.result['status'], 'activated')
        self.assertEqual(self.result['runtime_identity_integration'], 'not_implemented')
        self.assertEqual(self.result['signature_verification'], 'not_performed')
        self.assertEqual(self.result['real_ctr_bktr_validation'], 'not_performed')
        self.assertLess(self.result['metadata_bytes'], self.result['virtual_size'])

    def test_raw_hashes_match_independent_hashlib(self):
        for field, name in (('base_romfs_sha256', 'base.romfs'), ('program_sha256', 'program.bin'), ('virtual_sha256', 'virtual.romfs')):
            self.assertEqual(self.result[field], hashlib.sha256((self.evidence / name).read_bytes()).hexdigest())

    def test_literal_oracle_uses_parent_offsets_rotate5_and_collision_links(self):
        root = ROOT / 'tests/virtual-romfs/fixtures'
        expected = json.loads((root / 'root-nested.expected.json').read_text())
        data = (root / 'root-nested.romfs').read_bytes()
        self.assertEqual(len(data), 680)
        self.assertEqual(hashlib.sha256(data).hexdigest(), expected['sha256'])
        self.assertEqual(struct.unpack_from('<3I', data, 0x214), (24, 0xffffffff, 0xffffffff))
        self.assertEqual(struct.unpack_from('<I', data, 0x220 + 24 + 16)[0], 0)
        self.assertEqual(struct.unpack_from('<3I', data, 0x254), (36, 0xffffffff, 0xffffffff))
        self.assertEqual(struct.unpack_from('<I', data, 0x260 + 36 + 24)[0], 0)
        self.assertEqual(data[512:514], b'OK')
        self.assertEqual(data[528:529], b'!')

    def test_independent_reader_sees_rebuilt_names_sizes_data_and_hash_indexes(self):
        data = (self.evidence / 'virtual.romfs').read_bytes()
        _, dh, dhs, dm, dms, fh, fhs, fm, fms, payload = struct.unpack_from('<10Q', data)
        dirs, files = {}, {}
        for start, size, fixed, output in ((dm, dms, 24, dirs), (fm, fms, 32, files)):
            at = 0
            while at < size:
                length = struct.unpack_from('<I', data, start + at + fixed - 4)[0]
                name = data[start + at + fixed:start + at + fixed + length].decode('ascii')
                output[at] = (struct.unpack_from('<II', data, start + at), name)
                at += fixed + fixture.align(length, 4)
            self.assertEqual(at, size)
        paths = {0: ''}
        while len(paths) < len(dirs):
            before = len(paths)
            for at, ((parent, _), name) in dirs.items():
                if at not in paths and parent in paths:
                    paths[at] = '/'.join(filter(None, (paths[parent], name)))
            self.assertGreater(len(paths), before)
        actual = {}
        for at, ((parent, _), name) in files.items():
            relative, size = struct.unpack_from('<QQ', data, fm + at + 8)
            actual['/'.join(filter(None, (paths[parent], name)))] = data[payload + relative:payload + relative + size]
        expected = dict(fixture.FILES)
        expected.update({'replace.bin': b'L' * 4097, 'new/deep/added.txt': b'new nested bytes', 'folder/extra.txt': b'extra'})
        self.assertEqual(actual, expected)
        for table, size, meta, records, fixed in ((dh, dhs, dm, dirs, 24), (fh, fhs, fm, files, 32)):
            visited = set()
            for bucket in range(size // 4):
                at = struct.unpack_from('<I', data, table + bucket * 4)[0]
                while at != fixture.EMPTY:
                    self.assertNotIn(at, visited)
                    visited.add(at)
                    ((parent, _), name) = records[at]
                    self.assertEqual(fixture.path_hash(parent, name) % (size // 4), bucket)
                    at = struct.unpack_from('<I', data, meta + at + fixed - 8)[0]
            self.assertEqual(visited, set(records))

    def test_build_option_is_off_and_no_default_guest_hook_was_added(self):
        cmake = (ROOT / 'app/CMakeLists.txt').read_text()
        self.assertIn('option(PINE_ENABLE_VIRTUAL_ROMFS_PROTOTYPE "Compile the explicit read-only virtual RomFS prototype API" OFF)', cmake)
        for name in ('vfs/patch_manager.cpp', 'services/fssrv/IFileSystemProxy.cpp', 'loader/nsp.cpp'):
            self.assertNotIn('CreateVirtualRomfs', (ROOT / 'app/src/main/cpp/skyline' / name).read_text())

    def test_dedicated_ci_covers_real_dependencies_and_builds_no_apk(self):
        workflow = (ROOT / '.github/workflows/virtual-romfs.yml').read_text()
        for path in ('app/src/main/cpp/skyline/vfs/**', 'app/CMakeLists.txt', 'tests/virtual-romfs/**',
                     'tests/diagnostics/composite_stubs/common.h', 'tools/tests/virtual_romfs_fixture.py'):
            self.assertEqual(workflow.count("'" + path + "'"), 2)
        self.assertIn('contents: read', workflow)
        self.assertIn('persist-credentials: false', workflow)
        self.assertIn('--sanitize', workflow)
        self.assertIn('github.event.pull_request.head.sha || github.sha', workflow)
        for forbidden in ('gradlew', 'assemble', 'secrets.', 'release create'):
            self.assertNotIn(forbidden, workflow)


if __name__ == '__main__':
    unittest.main(argv=[sys.argv[0]] + REMAINING)
