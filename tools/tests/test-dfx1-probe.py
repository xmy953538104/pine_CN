#!/usr/bin/env python3
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('probe', ROOT / 'tools/verify-dfx1-probe.py')
probe = importlib.util.module_from_spec(spec); spec.loader.exec_module(probe)


class ProbeTest(unittest.TestCase):
    def rows(self):
        return list(probe.read_rows(ROOT / 'tools/tests/fixtures/native-sink.jsonl'))

    def test_real_production_host_fixture_preserves_raw_and_decoded_unicode(self):
        result = probe.verify(self.rows(), 'controlled-host-001', expected_message='中文 | %')
        self.assertEqual(result['source'], 'OutputDebugString')
        self.assertFalse(result['menu_or_world_execution_inferred'])

    def test_zip_input_keeps_crc_and_allowlisted_log_checks(self):
        with tempfile.TemporaryDirectory() as root:
            target = Path(root) / 'controlled.zip'
            with zipfile.ZipFile(target, 'w') as archive:
                archive.write(ROOT / 'tools/tests/fixtures/native-sink.jsonl', 'main/diagnostics-0.jsonl')
                archive.writestr('unrelated-private-file', 'not parsed or extracted')
            self.assertEqual(probe.verify(probe.read_rows(target), 'controlled-host-001')['status'], 'matching_raw_and_parsed_records')

    def test_snapshot_or_derived_record_without_raw_is_insufficient(self):
        rows = [row for row in self.rows() if row['event'] == 'DFX_PROBE']
        with self.assertRaisesRegex(ValueError, 'original guest'): probe.verify(rows, 'controlled-host-001')
        rows[0]['record_kind'] = 'critical_snapshot'
        parsed = probe.parse_rows((json.dumps(rows[0]) + '\n').encode())
        with self.assertRaisesRegex(ValueError, 'No matching'): probe.verify(parsed, 'controlled-host-001')

    def test_wrong_probe_source_payload_and_reused_identity_fail(self):
        for kwargs in (dict(probe='absent'), dict(probe='controlled-host-001', source='LogManager'),
                       dict(probe='controlled-host-001', expected_file='fabricated.lua')):
            with self.assertRaises(ValueError): probe.verify(self.rows(), **kwargs)
        rows = self.rows()
        duplicate = json.loads(json.dumps(rows))
        for row in duplicate: row['session_id'] = 'another-session'
        with self.assertRaisesRegex(ValueError, 'reused'): probe.verify(rows + duplicate, 'controlled-host-001')

    def test_derived_payload_cannot_certify_itself_against_a_forged_expectation(self):
        rows = self.rows()
        derived = next(row for row in rows if row['event'] == 'DFX_PROBE')
        derived['fields']['game_message'] = 'forged'
        with self.assertRaisesRegex(ValueError, 'Raw/derived payload mismatch'):
            probe.verify(rows, 'controlled-host-001', expected_message='forged')

    def test_malformed_raw_fields_and_redacted_payload_are_not_passes(self):
        for suffix in ('|extra=x', '|message=%GG', '|message=%00'):
            rows = self.rows()
            for row in rows:
                fields = row['fields']
                if 'DFX1|' in fields.get('message', ''):
                    fields['message'] = fields['message'].split('|message=')[0] + suffix
            with self.assertRaises(ValueError): probe.verify(rows, 'controlled-host-001')
        rows = self.rows()
        for row in rows:
            fields = row['fields']
            if 'DFX1|' in fields.get('message', ''):
                fields['message'] = fields['message'].split('|message=')[0] + '|message=%5BREDACTED_PATH%5D'
                if row['event'] == 'DFX_PROBE': fields['game_message'] = '[REDACTED_PATH]'
        with self.assertRaisesRegex(ValueError, 'unknown'): probe.verify(rows, 'controlled-host-001')


if __name__ == '__main__': unittest.main()
