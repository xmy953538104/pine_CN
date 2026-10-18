#!/usr/bin/env python3
"""Frozen literal oracle, independent of the P1 writer and validator.

Layout and hash vectors independently audited against switch-tools commit
22756068dd0ed6ff9734c59cb4f99ebd3f62555b src/romfs.c. Same content-free oracle
used by the independent RomFS review; no runtime dependency on that module.
Both directory and file collision chains deliberately contain offset zero.
"""
import hashlib
from pathlib import Path

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
data = header + bytes(0x200 - len(header)) + b'OK' + bytes(14) + b'!' + bytes(3)
data += directory_hash + directory_meta + file_hash + file_meta
assert len(data) == 680
assert hashlib.sha256(data).hexdigest() == 'c7d6619a3d10e89e49b292f1d4c9d10cc4479dc95d094bcad4058ccba8176501'
(Path(__file__).parent / 'romfs-collision.romfs').write_bytes(data)
