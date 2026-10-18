#!/usr/bin/env python3
"""Build and exercise the real native NSP catalog; no APK, keys or game data."""
import copy
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import nsp_catalog_fixture as fixture

ROOT = Path(__file__).resolve().parents[2]
GOLDEN = ROOT / 'tests/catalog/fixtures'


class CatalogTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix='pine-nsp-catalog-')
        cls.binary = Path(cls.build.name) / 'nsp-catalog'
        subprocess.run(['bash', 'tools/build-nsp-catalog.sh', str(cls.binary)], cwd=ROOT, check=True)

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def scan_bytes(self, data, *args, profile='modern-40', verify=True, fatal=None):
        with tempfile.TemporaryDirectory(prefix='pine-nsp-input-') as temporary:
            path = Path(temporary) / 'synthetic.nsp'
            path.write_bytes(data)
            command = [str(self.binary), '--profile', profile]
            if verify:
                command += ['--verify-hashes']
            result = subprocess.run(command + list(args) + [str(path)], capture_output=True, text=True, timeout=15)
            parsed = json.loads(result.stdout)
            self.assertEqual(result.returncode, 2 if fatal else 0, result.stderr or result.stdout)
            if fatal:
                self.assertEqual(parsed['fatal']['code'], fatal)
            return parsed

    def scan(self, entries, *args, **kwargs):
        return self.scan_bytes(fixture.pfs(entries), *args, **kwargs)

    def one_group(self, role=0x80, *, data=None, refs=None, **kwargs):
        title = 0x0100000000001000
        data = data or fixture.nca(0, title, [('main', b'dummy'), ('main.npdm', b'dummy')])
        refs = [fixture.content(data, 1)] if refs is None else refs
        meta = fixture.group_meta(title, role, refs, **kwargs)
        return [fixture.named(data), fixture.named(meta, True)]

    @staticmethod
    def issues(result):
        return {issue['code'] for item in [result] + result.get('ncas', []) + result.get('groups', []) for issue in item.get('issues', [])}

    def test_all_meta_and_all_cnmt_are_enumerated_without_same_type_overwrite(self):
        result = self.scan(fixture.multi_title())
        self.assertEqual(len(result['groups']), 7)
        self.assertEqual(len(result['ncas']), 9)
        self.assertEqual([g['type_name'] for g in result['groups']].count('Application'), 2)
        self.assertEqual([g['type_name'] for g in result['groups']].count('Patch'), 2)
        self.assertEqual([g['type_name'] for g in result['groups']].count('AddOnContent'), 3)
        self.assertTrue(result['all_metadata_enumerated'])
        for group in result['groups']:
            self.assertEqual(group['reference_consistency'], 'consistent')
            self.assertEqual(group['content_hash_verification'], 'verified')
            self.assertEqual(group['aoc_registration'], 'not_performed')
            if group['type_name'] == 'Application':
                self.assertIsNone(group['explicit_application_id'])
                self.assertEqual(group['parent_state'], 'not_applicable')
            else:
                self.assertEqual(group['parent_state'], 'consistent')
        referenced = [g['contents'][0]['member'] for g in result['groups']]
        self.assertEqual(len(set(referenced)), 7)

    def test_rename_reorder_and_unrelated_meta_preserve_logical_groups(self):
        entries = fixture.multi_title()
        baseline = self.scan(entries)
        renamed = [(f'arbitrary-{i}.bin', data) for i, (_, data) in enumerate(reversed(entries))]
        unrelated = fixture.group_meta(0x0100000000ff0000, 0x80, [])
        changed = self.scan(renamed + [('unrelated-meta.bytes', unrelated)])
        by_id = {group['id']: group for group in changed['groups']}
        for group in baseline['groups']:
            self.assertEqual(group, by_id[group['id']])
        self.assertEqual(len(changed['groups']), len(baseline['groups']) + 1)

    def test_cnmt_inside_meta_is_discovered_independently_of_filename(self):
        raw = fixture.cnmt(0x1234, 0x80, [])
        result = self.scan([fixture.named(fixture.nca(1, 0x1234, [('arbitrary-metadata.bin', raw)]), True)])
        self.assertEqual(result['groups'][0]['parsing'], 'parsed')
        self.assertEqual(result['groups'][0]['title_id'], '0000000000001234')
        empty_meta = fixture.nca(1, 0x1234, [('small-unclassified.bin', b'unknown')])
        result = self.scan([fixture.named(empty_meta, True)])
        self.assertFalse(result['all_metadata_enumerated'])

    def test_filename_json_preserves_original_bytes_without_claiming_utf8_decoding(self):
        entries = self.one_group()
        name = '合成-program.nca'
        entries[0] = name, entries[0][1]
        result = self.scan(entries)
        member = next(n for n in result['ncas'] if n['content_type'] == 0)
        self.assertEqual(result['filename_encoding'], 'byte_values_as_unicode_u00xx')
        self.assertEqual(member['entry'].encode('latin1'), name.encode('utf-8'))
        self.assertEqual(result['groups'][0]['contents'][0]['hash_verification'], 'verified')

    def test_default_unknown_profile_and_unverified_filename_hint(self):
        result = self.scan(self.one_group(), verify=False, profile='unknown')
        group = result['groups'][0]
        self.assertEqual(group['content_hash_verification'], 'unverified')
        self.assertEqual(group['reference_consistency'], 'unknown')
        self.assertIsNone(group['contents'][0]['decoded_size'])
        self.assertEqual(group['contents'][0]['resolution'], 'filename_hint_only')
        self.assertTrue(all(nca['actual_sha256'] is None for nca in result['ncas']))
        self.assertEqual(result['verification']['signature_verification'], 'not_verified')

    def test_hash_mismatch_is_separate_from_reference_consistency(self):
        entries = self.one_group()
        damaged = bytearray(entries[0][1]); damaged[-1] ^= 1
        entries[0] = entries[0][0], bytes(damaged)
        group = self.scan(entries)['groups'][0]
        self.assertEqual(group['contents'][0]['hash_verification'], 'mismatch')
        self.assertEqual(group['content_hash_verification'], 'mismatch')
        self.assertEqual(group['reference_consistency'], 'consistent')
        self.assertEqual(group['runnable'], 'not_assessed')

    def test_content_id_is_not_forced_to_be_a_hash_prefix(self):
        data = fixture.nca(0, 0x1000)
        entries = self.one_group(data=data, refs=[fixture.content(data, 1, content_id='a5' * 16)])
        entries[0] = 'renamed-program.bin', data
        group = self.scan(entries)['groups'][0]
        self.assertEqual(group['reference_consistency'], 'consistent')
        self.assertEqual(group['contents'][0]['content_id'], 'a5' * 16)
        self.assertEqual(group['contents'][0]['hash_verification'], 'verified')

    def test_duplicate_content_id_conflicts_never_select_a_member(self):
        entries = self.one_group()
        damaged = bytearray(entries[0][1]); damaged[-1] ^= 1
        entries += [(entries[0][0].replace('.nca', '.cnmt.nca'), bytes(damaged))]
        result = self.scan(entries)
        ref = result['groups'][0]['contents'][0]
        self.assertEqual(ref['resolution'], 'ambiguous')
        self.assertEqual(len(ref['candidate_members']), 2)
        self.assertIsNone(ref['member'])
        self.assertIn('duplicate_content_id', self.issues(result))

    def test_duplicate_identical_nca_under_arbitrary_names_is_ambiguous(self):
        entries = self.one_group()
        entries += [('another-copy.bin', entries[0][1])]
        ref = self.scan(entries)['groups'][0]['contents'][0]
        self.assertEqual(ref['resolution'], 'ambiguous')
        self.assertIsNone(ref['member'])

    def test_metadata_ambiguity_is_not_resolved_by_order_or_version(self):
        entries = self.one_group()
        data = entries[0][1]
        entries += [fixture.named(fixture.group_meta(0x0100000000001000, 0x80, [fixture.content(data, 1, 3)]), True)]
        result = self.scan(entries)
        self.assertEqual(len(result['groups']), 2)
        self.assertTrue(all('metadata_ambiguity' in {i['code'] for i in group['issues']} for group in result['groups']))
        self.assertTrue(all(group['reference_consistency'] == 'invalid' for group in result['groups']))

    def test_multiple_programs_and_indices_require_explicit_selection(self):
        first = fixture.nca(0, 0x1000, payload=b'one')
        second = fixture.nca(0, 0x1003, payload=b'two')
        entries = self.one_group(data=first, refs=[fixture.content(first, 1, 0), fixture.content(second, 1, 3)])
        entries += [fixture.named(second)]
        group = self.scan(entries)['groups'][0]
        self.assertEqual(group['runnable'], 'ambiguous')
        self.assertEqual([p['program_index'] for p in group['programs']], [0, 3])
        self.assertEqual([p['program_id'] for p in group['programs']], ['0000000000001000', '0000000000001003'])
        self.assertTrue(all(p['index_evidence'] == 'cnmt_program_id_offset' for p in group['programs']))

    def test_missing_aoc_data_and_missing_member_are_explicit(self):
        result = self.scan(self.one_group(role=0x82, refs=[], parent=0x9999))
        self.assertIn('missing_aoc_data', self.issues(result))
        self.assertEqual(result['groups'][0]['parent_state'], 'missing')
        result = self.scan(self.one_group()[1:])
        self.assertEqual(result['groups'][0]['contents'][0]['resolution'], 'missing')

    def test_patch_explicit_parent_mismatch_is_reported(self):
        data = fixture.nca(0, 0x1000)
        app = fixture.group_meta(0x1000, 0x80, [fixture.content(data, 1)], patch=0x1800)
        patch = fixture.group_meta(0x2800, 0x81, [fixture.content(data, 1)], parent=0x1000)
        result = self.scan([fixture.named(data), fixture.named(app, True), fixture.named(patch, True)])
        group = next(g for g in result['groups'] if g['type_name'] == 'Patch')
        self.assertEqual(group['parent_state'], 'mismatch')
        self.assertIn('patch_parent_mismatch', self.issues(result))

    def test_data_and_public_data_are_distinct_actual_nca_types(self):
        for actual in (4, 5):
            data = fixture.nca(actual, 0x2001)
            entries = self.one_group(role=0x82, data=data, refs=[fixture.content(data, 2)], parent=0x1000)
            result = self.scan(entries)
            group = result['groups'][0]
            self.assertEqual(group['contents'][0]['actual_nca_type'], actual)
            self.assertEqual(group['contents'][0]['reference_consistency'], 'consistent')
            self.assertEqual('aoc_data_runtime_unsupported' in self.issues(result), actual == 4)

    def test_delta_fragment_sparse_compression_and_plaintext_indirection_are_unsupported(self):
        for options, expected in ((dict(sparse=True), 'sparse_unsupported'), (dict(compressed=True), 'compression_unsupported'),
                                  (dict(encryption=4), 'bktr_requires_base'), (dict(patch_info=True), 'patch_info_unsupported'),
                                  (dict(metadata_hash=1), 'metadata_hash_unsupported')):
            with self.subTest(options=options):
                data = fixture.nca(0, 0x1000, **options)
                result = self.scan(self.one_group(data=data))
                self.assertIn(expected, self.issues(result))
                self.assertEqual(result['groups'][0]['content_support'], 'unsupported')
                if options.get('patch_info'):
                    self.assertTrue(result['groups'][0]['programs'][0]['requires_base'])
        data = fixture.nca(0, 0x1000)
        result = self.scan(self.one_group(role=0x83, refs=[fixture.content(data, 6)], parent=0x1000))
        self.assertIn('delta_unsupported', self.issues(result))
        self.assertIn('delta_fragment_unsupported', self.issues(result))

    def test_data_patch_identity_is_enumerated_with_support_separate(self):
        result = self.scan(self.one_group(role=0x84, refs=[], parent=0x1000, data_id=0x2000))
        group = result['groups'][0]
        self.assertEqual(group['explicit_application_id'], '0000000000001000')
        self.assertEqual(group['data_id'], '0000000000002000')
        self.assertEqual(group['content_support'], 'unsupported')
        self.assertEqual(group['parsing'], 'parsed')

    def test_bad_cnmt_and_valid_group_are_both_retained(self):
        entries = self.one_group()
        good = fixture.cnmt(0x2222, 0x80, [])
        variants = [good[:20], good[:-1], good + b'x']
        wrong = bytearray(good); struct.pack_into('<H', wrong, 14, 0xffff); variants.append(bytes(wrong))
        wrong = bytearray(good); struct.pack_into('<H', wrong, 16, 0xffff); variants.append(bytes(wrong))
        for bad in variants:
            with self.subTest(length=len(bad)):
                meta = fixture.nca(1, 0x2222, [('bad.cnmt', bad)])
                result = self.scan(entries + [fixture.named(meta, True)])
                self.assertEqual(len(result['groups']), 2)
                self.assertEqual(sorted(g['parsing'] for g in result['groups']), ['invalid', 'parsed'])
                self.assertFalse(result['all_metadata_enumerated'])
        wrong = bytearray(good); struct.pack_into('<HH', wrong, 16, 0xffff, 0xffff)
        bad_meta = fixture.nca(1, 0x2222, [('bad-1.cnmt', bytes(wrong)), ('bad-2.cnmt', bytes(wrong))])
        result = self.scan([('first-bad.nca', bad_meta)] + entries)
        self.assertEqual(len([g for g in result['groups'] if g['parsing'] == 'parsed']), 1)

    def test_embedded_digest_observation_does_not_gate_structure(self):
        raw = bytearray(fixture.cnmt(0x1234, 0x80, [])); raw[-1] ^= 1
        result = self.scan([fixture.named(fixture.nca(1, 0x1234, [('a.cnmt', bytes(raw))]), True)])
        group = result['groups'][0]
        self.assertEqual(group['parsing'], 'parsed')
        self.assertEqual(group['cnmt_trailer_digest_observation'], 'mismatch')

    def test_unknown_role_is_not_guessed_as_aoc_or_parent(self):
        raw = fixture.cnmt(0x1234, 0x90, [])
        group = self.scan([fixture.named(fixture.nca(1, 0x1234, [('a.cnmt', raw)]), True)])['groups'][0]
        self.assertEqual(group['title_id'], '0000000000001234')
        self.assertEqual(group['type_name'], 'Unknown')
        self.assertEqual(group['parsing'], 'unsupported')
        self.assertIsNone(group['explicit_application_id'])

    def test_every_encrypted_or_unreadable_member_is_discovered_and_metadata_partial(self):
        entries = self.one_group() + [('encrypted-meta.nca', bytes(0xc00)), ('truncated.nca', b'x'), ('renamed-encrypted.blob', bytes([0x5a]) * 0xc00)]
        result = self.scan(entries)
        self.assertEqual(len(result['ncas']), 5)
        self.assertEqual(len(result['groups']), 1)
        self.assertTrue(result['physical_entry_scan_complete'])
        self.assertFalse(result['all_metadata_enumerated'])
        self.assertIn('encrypted_header_or_unknown_format', self.issues(result))

    def test_hash_and_metadata_budgets_are_bounded_and_explicit(self):
        entries = self.one_group()
        result = self.scan(entries, '--max-hash-bytes', '1')
        self.assertEqual(result['verification']['whole_nca_hash_bytes'], 0)
        self.assertIn('hash_budget', self.issues(result))
        result = self.scan(entries, '--max-metadata-bytes', '1', '--max-hash-bytes', '1')
        self.assertFalse(result['bounded_scan_complete'])
        self.assertFalse(result['all_metadata_enumerated'])
        self.assertEqual(result['verification']['whole_nca_hash_bytes'], 0)
        result = self.scan(entries, '--max-cnmt-files', '0')
        self.assertEqual(result['groups'][0]['parsing'], 'not_inspected')
        self.assertIn('scan_budget', self.issues(result))
        self.scan(entries, '--max-pfs-entries', '0', fatal='scan_budget')
        result = self.scan(entries, '--max-association-work', '0')
        self.assertIn('association_budget', self.issues(result))
        self.assertFalse(result['bounded_scan_complete'])
        self.assertEqual(result['groups'][0]['contents'][0]['resolution'], 'unresolved')

    def test_bad_outer_pfs_length_offsets_names_counts_and_overlaps(self):
        entries = [('one', b'1234'), ('two', b'5678')]
        raw = bytearray(fixture.pfs(entries))
        self.scan_bytes(raw[:15], fatal='out_of_range')
        bad = bytearray(raw); struct.pack_into('<I', bad, 4, 0xffffffff)
        self.scan_bytes(bad, fatal='scan_budget')
        bad = bytearray(raw); struct.pack_into('<Q', bad, 16, 0xffffffffffffffff)
        self.scan_bytes(bad, fatal='integer_overflow')
        bad = bytearray(raw); struct.pack_into('<I', bad, 32, 0xffffffff)
        self.scan_bytes(bad, fatal='invalid_pfs_name')
        bad = bytearray(raw); struct.pack_into('<Q', bad, 16 + 24, 0)
        self.scan_bytes(bad, fatal='overlapping_pfs_entries')
        self.scan([('same', b'a'), ('same', b'b')], fatal='duplicate_pfs_name')
        self.scan([('a' * 1025, b'')], fatal='invalid_pfs_name')
        self.scan([('../escape', b'')], fatal='invalid_pfs_name')

    def test_nca_ranges_headers_hash_layers_and_noncontiguous_sections(self):
        for field, value, code in ((0x240, 0xffffffff, 'invalid_nca_section_range'), (0x244, 0xffffffff, 'out_of_range')):
            data = bytearray(fixture.nca(0, 0x1000)); struct.pack_into('<I', data, field, value)
            result = self.scan(self.one_group(data=bytes(data)))
            self.assertIn(code, self.issues(result))
        for at, value, code in ((0x42c, 6, 'invalid_nca_hash_layer_count'), (0x428, 3, 'invalid_nca_hash_block_size')):
            data = bytearray(fixture.nca(0, 0x1000)); struct.pack_into('<I', data, at, value)
            data[0x280:0x2a0] = hashlib.sha256(data[0x400:0x600]).digest()
            self.assertIn(code, self.issues(self.scan(self.one_group(data=bytes(data)))))
        data = fixture.nca(1, 0x1234, [('a.cnmt', fixture.cnmt(0x1234, 0x80, []))], sections_gap=True)
        result = self.scan([fixture.named(data, True)])
        self.assertEqual(result['ncas'][0]['sections'][0]['index'], 1)
        self.assertEqual(result['groups'][0]['parsing'], 'parsed')

    def test_content_meta_references_resolve_to_the_declared_identity(self):
        first = fixture.cnmt(0x1000, 0x80, [], meta_refs=[(0x2000, 3, 0x82, 0)])
        second = fixture.cnmt(0x2000, 0x82, [], version=3, parent=0x1000)
        meta = fixture.nca(1, 0x1000, [('first.cnmt', first), ('second.cnmt', second)])
        groups = self.scan([fixture.named(meta, True)])['groups']
        ref = groups[0]['content_meta_references'][0]
        self.assertEqual(ref['state'], 'resolved')
        self.assertEqual(ref['candidate_groups'], [groups[1]['id']])

    def test_duplicate_names_inside_meta_rejected_before_any_silent_map_overwrite(self):
        raw = fixture.cnmt(0x1234, 0x80, [])
        bad = fixture.nca(1, 0x1234, [('same.cnmt', raw), ('same.cnmt', raw)])
        result = self.scan(self.one_group() + [fixture.named(bad, True)])
        self.assertEqual(len(result['groups']), 1)
        self.assertIn('duplicate_pfs_name', self.issues(result))
        self.assertFalse(result['all_metadata_enumerated'])

    def test_fixed_independent_golden_and_size_profiles(self):
        expected = json.loads((GOLDEN / 'expected.json').read_text())
        for name in ('patch-sentinel.cnmt', 'layout-sentinel.cnmt', 'patch-sentinel.nsp'):
            self.assertEqual(fixture.sha((GOLDEN / name).read_bytes()), expected['sha256'][name])
        result = self.scan_bytes((GOLDEN / 'patch-sentinel.nsp').read_bytes())
        group = result['groups'][0]
        self.assertEqual(group['layout']['content_table_offset'], 0x38)
        self.assertEqual(group['layout']['content_meta_table_offset'], 0x70)
        self.assertEqual(group['contents'][0]['content_id'], 'a0a1a2a3a4a5a6a7a8a9aaabacadaeaf')
        self.assertEqual(group['content_meta_references'][0]['title_id'], '8877665544332211')
        self.assertEqual(group['content_meta_references'][0]['version'], 0xa1b2c3d4)
        self.assertEqual(group['programs'][0]['program_index'], 7)
        raw = (GOLDEN / 'layout-sentinel.cnmt').read_bytes()
        entries = [fixture.named(fixture.nca(1, 0x1234, [('layout.cnmt', raw)]), True)]
        modern = self.scan(entries)['groups'][0]['contents'][0]
        legacy = self.scan(entries, profile='legacy-48')['groups'][0]['contents'][0]
        unknown = self.scan(entries, profile='unknown')['groups'][0]['contents'][0]
        self.assertEqual(modern['raw_size_bytes'], '7856341234a5')
        self.assertEqual(modern['decoded_size'], 223643719288)
        self.assertEqual(modern['attributes'], 165)
        self.assertEqual(legacy['decoded_size'], 181643062302328)
        self.assertIsNone(unknown['decoded_size'])

    def test_native_reader_short_reads_overflows_hash_vectors_and_input_replacement(self):
        binary = Path(self.build.name) / 'wire-test'
        subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', '-Wall', '-Wextra', '-Werror',
                        '-Iapp/src/main/cpp/skyline', 'app/src/main/cpp/skyline/catalog/wire.cpp',
                        'tests/catalog/wire_test.cpp', '-o', str(binary)], cwd=ROOT, check=True)
        subprocess.run([str(binary), str(Path(self.build.name) / 'changing-input')], check=True)

    def test_schema_covers_real_synthetic_catalog(self):
        try:
            import jsonschema
        except ImportError:
            self.skipTest('jsonschema is optional for standalone host execution')
        schema = json.loads((ROOT / 'docs/nsp-catalog-v1.schema.json').read_text())
        jsonschema.Draft202012Validator.check_schema(schema)
        jsonschema.validate(self.scan(fixture.multi_title()), schema)
        broken = self.scan(self.one_group())
        broken['verification']['signature_verification'] = 'verified'
        with self.assertRaises(jsonschema.ValidationError):
            jsonschema.validate(broken, schema)


if __name__ == '__main__':
    unittest.main()
