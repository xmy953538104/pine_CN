"""Deterministic synthetic PFS0/NCA3/CNMT bytes; no Nintendo content or keys.

This general fixture writer is independent of the C++ reader. Fixed golden
sentinels in tests/catalog/fixtures are an additional independent format oracle.
"""
import hashlib
import struct


def sha(data):
    return hashlib.sha256(data).hexdigest()


def align(data, size=0x200):
    return data + bytes((-len(data)) % size)


def pfs(entries):
    names = bytearray()
    table = bytearray()
    payload = bytearray()
    for name, data in entries:
        encoded = name.encode('utf-8')
        table += struct.pack('<QQII', len(payload), len(data), len(names), 0)
        names += encoded + b'\0'
        payload += data
    names = align(names, 16)
    return struct.pack('<4sIII', b'PFS0', len(entries), len(names), 0) + table + names + payload


def nca(content_type, program_id, files=None, payload=b'synthetic data', *, encryption=1,
        sparse=False, compressed=False, patch_info=False, metadata_hash=0, sections_gap=False):
    # One real hierarchical SHA256 PFS section. Program payloads are deliberately
    # dummy bytes: presence of "main" is never reported as a launch proof.
    if files is None:
        files = [('payload', payload)]
    filesystem = align(pfs(files))
    blocks = [filesystem[i:i + 0x200] for i in range(0, len(filesystem), 0x200)]
    hashes = b''.join(hashlib.sha256(block).digest() for block in blocks)
    hash_area = align(hashes)
    section_data = hash_area + filesystem
    header = bytearray(0xc00)
    header[0x200:0x204] = b'NCA3'
    header[0x205] = content_type
    struct.pack_into('<QQ', header, 0x208, len(header) + len(section_data), program_id)
    index = 1 if sections_gap else 0
    struct.pack_into('<II', header, 0x240 + index * 0x10, 6, 6 + len(section_data) // 0x200)
    fs = bytearray(0x200)
    struct.pack_into('<HBBBB', fs, 0, 2, 1, 2, encryption, metadata_hash)
    fs[8:40] = hashlib.sha256(hashes).digest()
    struct.pack_into('<IIQQQQ', fs, 0x28, 0x200, 2, 0, len(hashes), len(hash_area), len(filesystem))
    if sparse:
        struct.pack_into('<Q', fs, 0x148, 0x200)
    if compressed:
        struct.pack_into('<Q', fs, 0x178, 0x200)
    if patch_info:
        struct.pack_into('<QQ', fs, 0x100, 0x200, 0x20)
    header[0x400 + index * 0x200:0x600 + index * 0x200] = fs
    header[0x280 + index * 0x20:0x2a0 + index * 0x20] = hashlib.sha256(fs).digest()
    return bytes(header) + section_data


def content(data, kind, index=0, content_id=None, size=None, attributes=0, size_bytes=None):
    digest = hashlib.sha256(data).digest()
    if content_id is None:
        content_id = digest[:16].hex()
    if size_bytes is None:
        size_bytes = (len(data) if size is None else size).to_bytes(5, 'little') + bytes([attributes])
    assert len(size_bytes) == 6
    return digest + bytes.fromhex(content_id) + size_bytes + bytes([kind, index])


def cnmt(title, role, refs=(), *, version=1, parent=0, patch=0, data_patch=0,
         data_id=0, meta_refs=(), extended_data=b'', legacy_aoc=False):
    if role == 0x80:
        ext = struct.pack('<QII', patch, 0, 0)
    elif role == 0x81:
        ext = struct.pack('<QIIQ', parent, 0, len(extended_data), 0)
    elif role == 0x82:
        ext = struct.pack('<QII', parent, 0, 0) if legacy_aoc else struct.pack('<QIB3xQ', parent, 0, 0, data_patch)
    elif role == 0x83:
        ext = struct.pack('<QII', parent, len(extended_data), 0)
    elif role == 0x84:
        ext = struct.pack('<QQIIQ', data_id, parent, 0, len(extended_data), 0)
    else:
        ext = b''
    header = struct.pack('<QIBBHHHBBHII', title, version, role, 0, len(ext), len(refs), len(meta_refs), 0, 0, 0, 0, 0)
    assert len(header) == 0x20
    metadata = b''.join(struct.pack('<QIBBH', tid, ver, kind, attrs, 0) for tid, ver, kind, attrs in meta_refs)
    raw = header + ext + b''.join(refs) + metadata + extended_data
    return raw + hashlib.sha256(raw).digest()


def named(data, meta=False):
    return sha(data)[:32] + ('.cnmt.nca' if meta else '.nca'), data


def group_meta(title, role, refs, **kwargs):
    return nca(1, title, [('content.cnmt', cnmt(title, role, refs, **kwargs))])


def multi_title():
    app1, app2 = 0x0100000000001000, 0x0100000000010000
    patch1, patch2 = app1 + 0x800, app2 + 0x800
    program1 = nca(0, app1, [('main', b'dummy NSO one'), ('main.npdm', b'dummy NPDM')])
    program2 = nca(0, app2, [('main', b'dummy NSO two'), ('main.npdm', b'dummy NPDM')])
    update1 = nca(0, patch1, [('main', b'dummy patch one'), ('main.npdm', b'dummy NPDM')])
    update2 = nca(0, patch2, [('main', b'dummy patch two'), ('main.npdm', b'dummy NPDM')])
    public1, public2, public3 = [nca(5, title, payload=f'AOC {i}'.encode()) for i, title in enumerate((app1 + 0x1001, app1 + 0x1002, app2 + 0x1001))]
    records = [
        ('application-one.cnmt', cnmt(app1, 0x80, [content(program1, 1)], patch=patch1)),
        ('application-two.cnmt', cnmt(app2, 0x80, [content(program2, 1)], patch=patch2)),
        ('patch-one.cnmt', cnmt(patch1, 0x81, [content(update1, 1)], parent=app1, version=5)),
        ('patch-two.cnmt', cnmt(patch2, 0x81, [content(update2, 1)], parent=app2, version=9)),
        ('aoc-one.cnmt', cnmt(app1 + 0x1001, 0x82, [content(public1, 2)], parent=app1)),
        ('aoc-two.cnmt', cnmt(app1 + 0x1002, 0x82, [content(public2, 2)], parent=app1)),
        ('aoc-three.cnmt', cnmt(app2 + 0x1001, 0x82, [content(public3, 2)], parent=app2)),
    ]
    # More than one CNMT per Meta NCA and more than one Meta NCA are both real.
    metas = [nca(1, app1, records[:4]), nca(1, app2, records[4:])]
    return [named(data) for data in [program1, program2, update1, update2, public1, public2, public3]] + [named(data, True) for data in metas]
