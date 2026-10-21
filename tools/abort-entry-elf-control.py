#!/usr/bin/env python3
"""Execute our own GNU ELF dynsym/PLT/direct-call controls; never game binding proof."""
import argparse
import json
from pathlib import Path
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def symbols(data):
    assert data[:6] == b'\x7fELF\x02\x01'
    shoff, = struct.unpack_from('<Q', data, 40)
    shentsize, shnum = struct.unpack_from('<HH', data, 58)
    assert shentsize == 64 and shnum < 4096
    sections = [struct.unpack_from('<IIQQQQIIQQ', data, shoff+i*64) for i in range(shnum)]
    result = {}
    for section in sections:
        if section[1] != 11:  # SHT_DYNSYM
            continue
        strings = sections[section[6]]
        names = data[strings[4]:strings[4]+strings[5]]
        assert section[9] == 24
        for pos in range(section[4], section[4]+section[5], 24):
            name, info, other, index, value, size = struct.unpack_from('<IBBHQQ', data, pos)
            end = names.index(0, name)
            result[names[name:end].decode()] = (pos, info, other, index, value, size)
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--cc', default='cc')
    p.add_argument('--qemu')
    p.add_argument('--sysroot', default='/usr/aarch64-linux-gnu')
    a = p.parse_args()
    out = a.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    original, patched, loader = (out/name for name in ('routes.so', 'routes-patched.so', 'loader'))
    subprocess.run([a.cc, '-O0', '-fPIC', '-fplt', '-fsemantic-interposition', '-shared',
                    str(ROOT/'tests/abort_entry/elf_routes.c'), '-o', str(original)], check=True)
    data = original.read_bytes()
    table = symbols(data)
    old, new = table['original_entry'], table['observer_proxy_control']
    assert old[1:4] == new[1:4] and old[1] == 0x12 and old[3] != 0
    patched_data = bytearray(data)
    struct.pack_into('<Q', patched_data, old[0]+8, new[4])
    assert patched_data[:old[0]+8] == data[:old[0]+8] and patched_data[old[0]+16:] == data[old[0]+16:]
    patched.write_bytes(patched_data)
    subprocess.run([a.cc, '-O2', str(ROOT/'tests/abort_entry/elf_loader.c'), '-ldl', '-o', str(loader)], check=True)
    prefix = [a.qemu, '-L', a.sysroot] if a.qemu else []
    lines = []
    for file, expected in ((original, 11), (patched, 22)):
        lines.append(subprocess.check_output([*prefix, str(loader), str(file), str(expected)], text=True).strip())
    result = dict(scope='executed_GNU_ELF_own_fixture_not_Horizon_or_game', external_and_internal_plt_consume_modified_dynsym=True,
                  direct_hidden_call_bypasses=True, changed_field='only original_entry Elf64_Sym.st_value', results=lines)
    (out/'result.json').write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps(result))


if __name__ == '__main__':
    main()
