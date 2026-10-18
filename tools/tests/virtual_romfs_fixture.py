"""Content-free RomFS bytes, independently packed from the source-audited layout.

This writer deliberately puts file data BEFORE the four metadata tables, as
switch-tools does. The production virtual builder puts metadata before data.
"""
import hashlib
import struct

EMPTY = 0xffffffff


def align(value, boundary):
    return (value + boundary - 1) & ~(boundary - 1)


def path_hash(parent, name):
    value = parent ^ 123456789
    for byte in name.encode('ascii'):
        value = (((value >> 5) | (value << 27)) ^ byte) & 0xffffffff
    return value


def romfs(files, directories=()):
    dirs = {''} | set(directories)
    for name in files:
        parts = name.split('/')
        dirs.update('/'.join(parts[:i]) for i in range(1, len(parts)))
    dirs = sorted(dirs)
    ordered = sorted(files)
    parent = lambda path: path.rpartition('/')[0]
    name = lambda path: path.rpartition('/')[2]
    doffs, foffs = {}, {}
    dsize = fsize = 0
    for path in dirs:
        doffs[path] = dsize
        dsize += 24 + align(len(name(path)), 4)
    for path in ordered:
        foffs[path] = fsize
        fsize += 32 + align(len(name(path)), 4)
    data = bytearray()
    payload_offsets = {}
    for path in ordered:
        data += bytes((-len(data)) % 16)
        payload_offsets[path] = len(data)
        data += files[path]
    dh = [EMPTY] * 3
    fh = [EMPTY] * 3
    dm, fm = bytearray(dsize), bytearray(fsize)
    for path in dirs:
        siblings = [p for p in dirs if p and parent(p) == parent(path)]
        sibling = doffs[siblings[siblings.index(path) + 1]] if path and siblings.index(path) + 1 < len(siblings) else EMPTY
        children = [p for p in dirs if p and parent(p) == path]
        owned = [p for p in ordered if parent(p) == path]
        bucket = path_hash(doffs[parent(path)] if path else 0, name(path)) % 3
        struct.pack_into('<IIIIII', dm, doffs[path], doffs[parent(path)] if path else 0, sibling,
                         doffs[children[0]] if children else EMPTY, foffs[owned[0]] if owned else EMPTY,
                         dh[bucket], len(name(path)))
        dm[doffs[path] + 24:doffs[path] + 24 + len(name(path))] = name(path).encode()
        dh[bucket] = doffs[path]
    for path in ordered:
        siblings = [p for p in ordered if parent(p) == parent(path)]
        sibling = foffs[siblings[siblings.index(path) + 1]] if siblings.index(path) + 1 < len(siblings) else EMPTY
        bucket = path_hash(doffs[parent(path)], name(path)) % 3
        struct.pack_into('<IIQQII', fm, foffs[path], doffs[parent(path)], sibling,
                         payload_offsets[path], len(files[path]), fh[bucket], len(name(path)))
        fm[foffs[path] + 32:foffs[path] + 32 + len(name(path))] = name(path).encode()
        fh[bucket] = foffs[path]
    data_offset = 0x200
    dh_offset = align(data_offset + len(data), 4)
    dm_offset = dh_offset + 12
    fh_offset = dm_offset + dsize
    fm_offset = fh_offset + 12
    header = struct.pack('<10Q', 80, dh_offset, 12, dm_offset, dsize, fh_offset, 12, fm_offset, fsize, data_offset)
    result = bytearray(header + bytes(data_offset - len(header))) + data
    result += bytes(dh_offset - len(result))
    result += struct.pack('<3I', *dh) + dm + struct.pack('<3I', *fh) + fm
    return bytes(result)


FILES = {
    'folder/existing.txt': b'nested original',
    'folder/sub/base.txt': b'subdirectory original',
    'keep.txt': b'original kept bytes',
    'replace.bin': b'old value',
    'zero': b'',
}


def write_fixture(root):
    data = romfs(FILES, ['empty'])
    (root / 'base.romfs').write_bytes(data)
    (root / 'program.bin').write_bytes(b'public synthetic effective Program bytes; no executable content')
    return {'base_sha256': hashlib.sha256(data).hexdigest(), 'base_size': len(data), 'files': {path: value.decode() for path, value in FILES.items()}}
