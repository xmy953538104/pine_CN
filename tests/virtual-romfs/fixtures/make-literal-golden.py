#!/usr/bin/env python3
"""Literal layout oracle; no production reader, writer or fixture imports.

Audited against switch-tools 22756068 src/romfs.c. Hash chains deliberately
contain offset zero, distinct from the empty sentinel 0xffffffff.
"""
import hashlib
import json
from pathlib import Path

root = Path(__file__).parent
header = bytes.fromhex('''
5000000000000000 1402000000000000 0c00000000000000
2002000000000000 3400000000000000 5402000000000000
0c00000000000000 6002000000000000 4800000000000000
0002000000000000
''')
directory_hash = bytes.fromhex('18000000 ffffffff ffffffff')
directory_meta = bytes.fromhex('''
00000000 ffffffff 18000000 ffffffff ffffffff 00000000
00000000 ffffffff ffffffff 00000000 00000000 01000000 63000000
''')
file_hash = bytes.fromhex('24000000 ffffffff ffffffff')
file_meta = bytes.fromhex('''
18000000 24000000 0000000000000000 0200000000000000 ffffffff 01000000 61000000
18000000 ffffffff 1000000000000000 0100000000000000 00000000 01000000 64000000
''')
assert len(header) == 0x50 and len(directory_meta) == 0x34 and len(file_meta) == 0x48
data = header + bytes(0x200 - len(header)) + b'OK' + bytes(14) + b'!' + bytes(3)
assert len(data) == 0x214
data += directory_hash + directory_meta + file_hash + file_meta
assert len(data) == 0x2a8
(root / 'root-nested.romfs').write_bytes(data)
expected = {
    'source_commit': '22756068dd0ed6ff9734c59cb4f99ebd3f62555b',
    'sha256': hashlib.sha256(data).hexdigest(), 'size': len(data),
    'header': {'directory_hash': 0x214, 'directory_metadata': 0x220, 'file_hash': 0x254, 'file_metadata': 0x260, 'data': 0x200},
    'files': {'c/a': 'OK', 'c/d': '!'},
    'hash_vectors': {'root_parent0_empty': '075bcd15', 'directory_parent0_c': 'a83ade0b', 'file_parent24_a': '683ade09', 'file_parent24_d': '683ade0c'},
    'directory_bucket0': 24, 'directory_at24_next_hash': 0,
    'file_bucket0': 36, 'file_at36_next_hash': 0,
    'note': 'The hash field is the next collision-chain metadata offset, not a computed hash value.',
}
(root / 'root-nested.expected.json').write_text(json.dumps(expected, indent=2) + '\n')
print(json.dumps(expected, indent=2))
