#!/usr/bin/env python3
"""Selected APK compile-evidence collector rejects absent/conflicting/off flags."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('selected_flags', ROOT / 'tools/verify-selected-content-apk.py')
flags = importlib.util.module_from_spec(spec)
spec.loader.exec_module(flags)


class SelectedBuildFlags(unittest.TestCase):
    def test_effective_definition_order_and_explicit_undefinition(self):
        self.assertEqual(flags.definitions(['-DA=0', '-D', 'A=1', '-UA', '-DB']), {'B': '1'})
        self.assertEqual(flags.definitions(['-D', 'A', '-U', 'A', '-DA=0']), {'A': '0'})
        with self.assertRaises(ValueError):
            flags.definitions(['-D'])

    def test_every_actual_selected_translation_unit_must_be_enabled(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary) / 'Debug' / 'arm64-v8a'
            folder.mkdir(parents=True)
            path = folder / 'compile_commands.json'
            rows = [{'file': '/checkout' + suffix, 'directory': '/build',
                     'arguments': ['clang++', '-DPINE_ENABLE_SELECTED_CONTENT=1']} for suffix in flags.REQUIRED]
            path.write_text(json.dumps(rows))
            self.assertEqual(set(flags.collect(Path(temporary))), set(flags.REQUIRED))
            for arguments in (['clang++'], ['clang++', '-DPINE_ENABLE_SELECTED_CONTENT=0'],
                              ['clang++', '-DPINE_ENABLE_SELECTED_CONTENT=1', '-UPINE_ENABLE_SELECTED_CONTENT']):
                changed = [*rows[:-1], dict(rows[-1], arguments=arguments)]
                path.write_text(json.dumps(changed))
                with self.assertRaises(ValueError):
                    flags.collect(Path(temporary))
            path.write_text(json.dumps(rows[:-1]))
            with self.assertRaises(ValueError):
                flags.collect(Path(temporary))


if __name__ == '__main__':
    unittest.main()
