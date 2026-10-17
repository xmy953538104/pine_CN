#!/usr/bin/env python3
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('flags', ROOT/'tools/verify-native-debug-flags.py')
flags = importlib.util.module_from_spec(spec)
spec.loader.exec_module(flags)


class DebugFlags(unittest.TestCase):
    def setUp(self):
        self.args = ['clang++', '-O2', '-g3', '-fno-omit-frame-pointer', '-fstack-protector-strong']

    def test_optimization_does_not_disable_assertions_or_protections(self):
        flags.verify(self.args, '2')
        for extra in ['-O0', '-DNDEBUG', '-DNDEBUG=1', '-DBOOST_DISABLE_ASSERTS', '-fno-stack-protector', '-ffast-math', '-Ofast']:
            with self.subTest(extra=extra), self.assertRaises(ValueError): flags.verify(self.args+[extra], '2')
        for removed in ['-g3', '-fno-omit-frame-pointer', '-fstack-protector-strong']:
            with self.subTest(removed=removed), self.assertRaises(ValueError): flags.verify([a for a in self.args if a!=removed], '2')
        for extra in [['-D', 'NDEBUG'], ['-fomit-frame-pointer']]:
            with self.subTest(extra=extra), self.assertRaises(ValueError): flags.verify(self.args+extra, '2')

    def test_missing_and_mixed_compiler_commands_are_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)/'Debug'/'arm64-v8a'
            folder.mkdir(parents=True)
            path = folder/'compile_commands.json'
            rows = [{'file': '/checkout/'+name, 'directory': '/build', 'arguments': self.args}
                    for name in flags.REQUIRED]
            path.write_text(json.dumps(rows))
            self.assertEqual(set(flags.collect(Path(temporary), '2')), set(flags.REQUIRED))
            path.write_text(json.dumps(rows[:-1]))
            with self.assertRaises(ValueError): flags.collect(Path(temporary), '2')
            rows[-1]['arguments'] = self.args+['-O0']
            path.write_text(json.dumps(rows))
            with self.assertRaises(ValueError): flags.collect(Path(temporary), '2')

    def test_production_cmake_default_candidate_and_invalid_input(self):
        with tempfile.TemporaryDirectory() as temporary:
            script = Path(temporary)/'flags.cmake'
            script.write_text(f'include("{ROOT}/app/cmake/DiagnosticsOptimization.cmake")\nfile(WRITE "{temporary}/actual.txt" "${{CMAKE_CXX_FLAGS_DEBUG}}")\n')
            for option in [None, '0', '2', '3', '2 -DNDEBUG']:
                argv = ['cmake']+(['-DPINE_NATIVE_DEBUG_OPTIMIZATION='+option] if option is not None else [])+['-P', str(script)]
                result = subprocess.run(argv, capture_output=True, text=True)
                if option in (None, '0', '2'):
                    self.assertEqual(result.returncode, 0, result.stderr)
                    expected = option or '0'
                    actual = (Path(temporary)/'actual.txt').read_text()
                    self.assertIn('-O'+expected+' ', actual)
                    self.assertIn('-g3', actual)
                    self.assertIn('-fno-omit-frame-pointer', actual)
                    self.assertNotIn('NDEBUG', actual)
                else:
                    self.assertNotEqual(result.returncode, 0)


if __name__ == '__main__':
    unittest.main()
