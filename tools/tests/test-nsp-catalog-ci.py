#!/usr/bin/env python3
"""Check that changes to real native decoder inputs trigger the AES CI lane."""
from fnmatch import fnmatchcase
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
WORKFLOW = ROOT / '.github/workflows/nsp-catalog.yml'


def paths_for_event(text, event):
    """Read this workflow's explicitly indented scalar path lists, not YAML generally."""
    lines = text.splitlines()
    start = lines.index(f'  {event}:') + 1
    event_lines = []
    for line in lines[start:]:
        if line and not line.startswith('    '):
            break
        event_lines.append(line)
    start = event_lines.index('    paths:') + 1
    patterns = []
    for line in event_lines[start:]:
        if not line.startswith("      - '") or not line.endswith("'"):
            break
        patterns.append(line[len("      - '"):-1])
    if not patterns:
        raise AssertionError(f'{event} has no explicit dependency paths')
    return patterns


class CatalogDependencyTriggers(unittest.TestCase):
    def assert_triggers(self, paths):
        paths = tuple(paths)
        text = WORKFLOW.read_text()
        for event in ('push', 'pull_request'):
            patterns = paths_for_event(text, event)
            for path in paths:
                with self.subTest(event=event, path=path):
                    self.assertTrue(any(fnmatchcase(path, pattern) for pattern in patterns),
                                    f'{path} does not trigger the dedicated real-AES lane')

    def test_actual_backend_sources_and_transitive_headers(self):
        # Independently enumerated from the compiled production decoder path;
        # fast-tools' controlled cipher cannot replace this workflow's AES run.
        self.assert_triggers('app/src/main/cpp/skyline/' + name for name in (
            'vfs/nca.cpp', 'vfs/nca.h', 'vfs/partition_filesystem.cpp',
            'vfs/partition_filesystem.h', 'vfs/ctr_encrypted_backing.cpp',
            'vfs/ctr_encrypted_backing.h', 'vfs/bktr.cpp', 'vfs/bktr.h',
            'vfs/os_backing.cpp', 'vfs/os_backing.h', 'vfs/backing.h',
            'vfs/region_backing.h', 'vfs/filesystem.h', 'vfs/directory.h',
            'crypto/aes_cipher.cpp', 'crypto/aes_cipher.h'))

    def test_core_harness_and_fixed_fixtures(self):
        self.assert_triggers((
            'app/src/main/cpp/skyline/catalog/catalog.cpp',
            'app/src/main/cpp/skyline/catalog/native_adapter.cpp',
            'app/CMakeLists.txt', 'tests/catalog/native_adapter_test.cpp',
            'tests/catalog/fixtures/patch-sentinel.cnmt',
            'tests/catalog/native_real_stubs/crypto/aes_cipher.h',
            'tools/tests/test-nsp-catalog-native.py',
            'tools/tests/test-nsp-catalog-ci.py',
            'tools/tests/nsp_catalog_fixture.py',
            'tools/generate-nsp-catalog-example.py',
            'docs/nsp-catalog-v1.schema.json', '.github/workflows/nsp-catalog.yml'))

    def test_pinned_crypto_dependency_updates(self):
        self.assert_triggers(('app/libraries/mbedtls', '.gitmodules'))

    def test_dedicated_lane_requires_real_cipher_and_schema(self):
        command = next(line for line in WORKFLOW.read_text().splitlines()
                       if 'python3 tools/tests/test-nsp-catalog-native.py --real-aes' in line)
        self.assertIn('--schema-check', command)
        self.assertIn('--mbedtls-root app/libraries/mbedtls', command)
        self.assertIn('--sanitize', command)


if __name__ == '__main__':
    unittest.main()
