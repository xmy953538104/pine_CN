#!/usr/bin/env python3
"""Audit four supplied evidence texts, not original NSOs or runtime binding.

stdin is a four-string Library-read transcript: README, symbols, windows,
identity. No source file is materialized/replaced by this program. Only the
derived audit report is written. CRLF reconstruction is accepted only when
both the exact supplied byte count and README digest match.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import sys


def audit(texts):
    assert len(texts) == 4 and all(isinstance(t, str) for t in texts)
    readme = texts[0]
    symbols, windows, identity = [json.loads(t) for t in texts[1:]]
    whole = []
    for name, text in zip(('nvn-abort-symbol-audit.json', 'nvn-instruction-windows.json', 'nvn-runtime-identity.json'), texts[1:]):
        match = re.search(re.escape(name) + r'.*?(\d+) bytes; SHA256 `([0-9a-f]{64})`', readme)
        assert match, name
        size, expected = int(match[1]), match[2]
        normalized = text.replace('\r\n', '\n')
        variants = {v.encode() for v in (normalized, normalized+'\n', normalized.replace('\n', '\r\n'), normalized.replace('\n', '\r\n')+'\r\n')}
        matches = [v for v in variants if len(v) == size and hashlib.sha256(v).hexdigest() == expected]
        assert len(matches) == 1, (name, 'read transcript differs from supplied whole-file hash')
        whole.append(dict(name=name, bytes=size, sha256=expected, reconstructed_line_endings='CRLF' if b'\r\n' in matches[0] else 'LF'))
    count, hashes = 0, 0

    def walk(node, path):
        nonlocal count, hashes
        if isinstance(node, dict):
            if 'raw_hex' in node:
                count += 1
                data = bytes.fromhex(node['raw_hex'])
                if 'bytes' in node: assert len(data) == node['bytes'], path
                if 'sha256' in node:
                    hashes += 1
                    assert hashlib.sha256(data).hexdigest() == node['sha256'], path
            for key, value in node.items(): walk(value, path+'/'+key)
        elif isinstance(node, list):
            for i, value in enumerate(node): walk(value, path+'/'+str(i))
    for i, item in enumerate((symbols, windows, identity)): walk(item, str(i))
    assert count == 60, count
    h = lambda value: int(value, 16)
    for module in ('sdk', 'main'):
        sym, table = symbols[module+'_dynsym'], symbols[module+'_dynamic_table']
        tags = {}
        for i, entry in enumerate(table['entries']):
            tag, value = struct.unpack('<qQ', bytes.fromhex(entry['raw_hex']))
            assert tag == entry['tag'] and value == h(entry['value_hex'])
            assert h(entry['record_nso_va']) == h(table['dynamic_nso_va'])+i*16
            tags[tag] = value
        record = struct.unpack('<IBBHQQ', bytes.fromhex(sym['raw_hex']))
        assert record == (sym['st_name'], h(sym['st_info_hex']), h(sym['st_other_hex']), sym['st_shndx'], h(sym['st_value_nso_va']), sym['st_size'])
        assert h(sym['record_nso_va']) == tags[6]+sym['index']*tags[11]
        assert h(sym['name_nso_va']) == tags[5]+sym['st_name']
        assert bytes.fromhex(sym['name_raw_hex_with_nul']) == symbols['exact_mangled_name'].encode()+b'\0'
        for relocation in symbols[module+'_matching_relocations']:
            offset, info, addend = struct.unpack('<QQq', bytes.fromhex(relocation['raw_hex']))
            assert offset == h(relocation['r_offset_nso_va']) and info == h(relocation['r_info_hex']) and addend == relocation['r_addend']
            assert info >> 32 == sym['index'] == relocation['symbol_index']
            assert info & 0xffffffff == 1026 == relocation['type_number']
            assert tags[23] <= h(relocation['record_nso_va']) < tags[23]+tags[2]
            assert (h(relocation['record_nso_va'])-tags[23]) % 24 == 0
    for window in windows['windows']:
        data, start = bytes.fromhex(window['raw_hex']), h(window['start_nso_va'])
        assert h(window['end_nso_va_exclusive'])-start == len(data)
        assert data == b''.join(bytes.fromhex(i['bytes_le']) for i in window['instructions'])
        assert all(h(ins['nso_va']) == start+i*4 for i, ins in enumerate(window['instructions']))
    call = symbols['original_call']
    word = int.from_bytes(bytes.fromhex(call['raw_hex']), 'little')
    immediate = word & 0x3ffffff
    if immediate & 0x2000000: immediate -= 0x4000000
    assert word >> 26 == 0x25 and h(call['callsite_nso_va'])+immediate*4 == 0x3e97e0
    adrp, ldr, add, br = [int.from_bytes(bytes.fromhex(i['bytes_le']), 'little') for i in symbols['PLT']['instructions']]
    immediate = ((adrp >> 5) & 0x7ffff)*4 + ((adrp >> 29) & 3)
    if immediate & 0x100000: immediate -= 0x200000
    page = (0x3e97e0 & ~0xfff)+(immediate << 12)
    assert adrp & 31 == 16 and ldr & 31 == 17 and (ldr >> 5) & 31 == 16
    assert page+(((ldr >> 10) & 0xfff) << 3) == 0x987210
    assert page+((add >> 10) & 0xfff) == 0x987210 and br == 0xd61f0220
    assert int.from_bytes(bytes.fromhex(symbols['GOT']['original_unrelocated_slot']['raw_hex']), 'little') == 0x3e9760
    for item in symbols['assertion_strings']:
        assert bytes.fromhex(item['raw_hex']) == item['ascii'].encode('ascii')+b'\0'
    assert bytes.fromhex(symbols['static_definition_candidate']['raw_hex'])[:4] == bytes.fromhex('ff8300d1')
    return dict(schema=1, source_route='all four actual Library files completely read; line-end reconstruction verified against supplied README, not original-source materialization',
                whole_json_hashes=whole, raw_hex_objects_checked=count, raw_objects_with_hash_checked=hashes,
                elf64_symbols=2, rela_records=2, window_coverage_checked=4,
                chain='sdk+2D64E0 BL -> PLT3E97E0 -> GOT987210 -> unknown live target',
                outer_definition='GLOBAL FUNC DEFAULT index16826 sdk+445D0 size32',
                correction='BL to a PLT does not establish bypass of dynsym binding; guest resolver policy and current GOT remain unknown',
                full_nso_and_six_segment_hashes='producer provenance claims, no complete NSO/segments here to independently recompute',
                runtime_sdk_authenticated=False)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = audit(json.load(sys.stdin))
    args.output.write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps(result, indent=2))
