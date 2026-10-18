#!/usr/bin/env python3
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]


def load(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'tools/ci' / (name + '.py'))
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


class PlanningTest(unittest.TestCase):
    def test_documentation_needs_no_apk_or_host_rebuild(self):
        self.assertFalse(any(load('diagnostics-changes').classify(['docs/protocol.md']).values()))

    def test_observer_does_not_rebuild_jvm_or_native(self):
        self.assertEqual(load('diagnostics-changes').classify(['tools/android-diagnostics-smoke.py']),
                         dict(native=False, tools=True, windows=False, jvm=False))

    def test_native_jvm_and_windows_scopes(self):
        classify = load('diagnostics-changes').classify
        self.assertTrue(classify(['app/src/main/cpp/skyline/os.cpp'])['native'])
        self.assertTrue(classify(['app/src/main/java/emu/skyline/diagnostics/Diagnostics.kt'])['jvm'])
        self.assertTrue(classify(['tools/pine-diag.ps1'])['windows'])
        self.assertTrue(all(classify(['.github/workflows/diagnostics-fast.yml']).values()))
        self.assertTrue(all(classify(['.gitmodules']).values()))
        self.assertTrue(all(classify(['unknown-build-input']).values()))
        self.assertTrue(all(classify(['app/new-production-hook']).values()))

    def test_jvm_graph_rejects_build_and_signing_before_test(self):
        verify = load('verify-jvm-only').verify
        verify(':app:compileFullDebugKotlin SKIPPED\n:app:testFullDebugUnitTest SKIPPED')
        verify(':app:packageFullDebugResources SKIPPED\n:app:bundleFullDebugClassesToRuntimeJar SKIPPED\n'
               ':app:packageFullDebugUnitTestForUnitTest SKIPPED\n:app:testFullDebugUnitTest SKIPPED')
        for task in ('assembleFullDebug', 'buildCMakeDebug', 'configureCMakeDebug', 'validateSigningFullDebug', 'packageFullDebug'):
            with self.assertRaises(ValueError):
                verify(':app:' + task + ' SKIPPED\n:app:testFullDebugUnitTest SKIPPED')

    def test_candidate_requires_exact_latest_success(self):
        require = load('require-fast-checks').require
        good = dict(id=1, name='Diagnostics fast checks', head_sha='a', status='completed', conclusion='success', html_url='run1')
        self.assertEqual(require([good], 'a'), 'run1')
        for runs, commit in (([good], 'b'), ([good, dict(good, id=2, status='in_progress')], 'a'),
                             ([dict(good, conclusion='failure')], 'a')):
            with self.assertRaises(ValueError):
                require(runs, commit)


if __name__ == '__main__':
    unittest.main()
