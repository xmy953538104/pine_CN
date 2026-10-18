#!/usr/bin/env python3
"""Independent literal CNMT/header oracle; imports no catalog reader or writer.

Atmosphere 6b0c077 ncm_content_meta.hpp: storage_id/install_type/committed
are at 0x15/0x16/0x17. Both content and content-meta tables are nonempty.
"""
import hashlib
import json
from pathlib import Path
import struct

out = Path(__file__).parent
body = bytes.fromhex('''
00583412000000014433221181001800
01000100040501010102030400000000
00503412000000010000000000000000
00000000000000000001020304050607
08090a0b0c0d0e0f1011121314151617
18191a1b1c1d1e1fa0a1a2a3a4a5a6a7
a8a9aaabacadaeaf7856341200000107
1122334455667788d4c3b2a182040000
''')
assert len(body) == 0x80
cnmt = body + hashlib.sha256(body).digest()
(out / 'header-fields-sentinel.cnmt').write_bytes(cnmt)
# Build a literal-layout real Meta NCA and outer PFS independently of the
# production parser and the general synthetic fixture helper.
pfs = bytearray(0x200)
struct.pack_into('<4sIIIQQII', pfs, 0, b'PFS0', 1, 0x18, 0, 0, len(cnmt), 0, 0)
pfs[0x28:0x35] = b'fixture.cnmt\0'
pfs[0x40:0x40 + len(cnmt)] = cnmt
nca = bytearray(0x1000)
nca[0x200:0x204] = b'NCA3'
nca[0x205] = 1
struct.pack_into('<QQ', nca, 0x208, len(nca), 0x0100000012345800)
struct.pack_into('<IIII', nca, 0x240, 6, 8, 0, 0)
fs = bytearray(0x200)
struct.pack_into('<HBBBB', fs, 0, 2, 1, 2, 1, 0)
table = hashlib.sha256(pfs).digest()
fs[8:0x28] = hashlib.sha256(table).digest()
struct.pack_into('<IIQQQQ', fs, 0x28, 0x200, 2, 0, 0x20, 0x200, 0x200)
nca[0x400:0x600] = fs
nca[0x280:0x2a0] = hashlib.sha256(fs).digest()
nca[0xc00:0xc20] = table
nca[0xe00:0x1000] = pfs
name = hashlib.sha256(nca).hexdigest()[:32] + '.cnmt.nca'
names = name.encode() + b'\0'
string_size = ((len(names) + 0x28 + 0x1f) // 0x20) * 0x20 - 0x28
nsp = struct.pack('<4sIIIQQII', b'PFS0', 1, string_size, 0, 0, len(nca), 0, 0) + names + bytes(string_size - len(names)) + nca
(out / 'header-fields-sentinel.nsp').write_bytes(nsp)
expected = {
    'provenance': 'Independent literal; Atmosphere fixed 6b0c077 PackagedContentMetaHeader. No production reader/writer imports.',
    'header_hex': body[:0x20].hex(),
    'attributes': 4, 'storage_id': 5, 'install_type': 1, 'committed_raw': 1, 'committed': True,
    'required_download_system_version': 0x04030201,
    'content_count': 1, 'content_meta_count': 1,
    'content_offset': 0x38, 'content_meta_offset': 0x70,
    'content_id': 'a0a1a2a3a4a5a6a7a8a9aaabacadaeaf',
    'content_meta_id': '8877665544332211',
    'sha256': {
        'header-fields-sentinel.cnmt': hashlib.sha256(cnmt).hexdigest(),
        'header-fields-sentinel.nsp': hashlib.sha256(nsp).hexdigest(),
    },
}
(out / 'header-fields-expected.json').write_text(json.dumps(expected, indent=2) + '\n')
print(json.dumps(expected, indent=2))
