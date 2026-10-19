#!/usr/bin/env python3
"""Compare trusted ELF64 compiler output for the unchanged ordinary key parser."""
import hashlib
import struct


def key_store_functions(path):
    data = path.read_bytes()
    if data[:6] != b"\x7fELF\x02\x01":
        raise ValueError("ELF64 little-endian compiler object required")
    shoff = struct.unpack_from("<Q", data, 40)[0]
    size, count, names_index = struct.unpack_from("<HHH", data, 58)
    if size != 64:
        raise ValueError("unexpected compiler section header size")
    sections = [struct.unpack_from("<IIQQQQIIQQ", data, shoff + i * size) for i in range(count)]
    def body(section):
        return data[section[4]:section[4] + section[5]]
    def string(table, offset):
        return table[offset:table.index(0, offset)].decode("ascii")
    names = body(sections[names_index])
    section_names = [string(names, s[0]) for s in sections]
    results = {}
    for index, section in enumerate(sections):
        name = section_names[index]
        if not name.startswith(".text._ZN7skyline6crypto8KeyStore"):
            continue
        relocations = []
        for relocation in sections:
            if relocation[1] != 4 or relocation[7] != index:
                continue
            symbols = sections[relocation[6]]
            symbol_names = body(sections[symbols[6]])
            raw_symbols = body(symbols)
            raw_relocations = body(relocation)
            for at in range(0, len(raw_relocations), 24):
                offset, info, addend = struct.unpack_from("<QQq", raw_relocations, at)
                symbol = struct.unpack_from("<IBBHQQ", raw_symbols, (info >> 32) * 24)
                symbol_name = string(symbol_names, symbol[0]) if symbol[0] else section_names[symbol[3]]
                relocations.append([offset, info & 0xffffffff, symbol_name, addend])
        code = body(section)
        results[name] = {"size": len(code), "sha256": hashlib.sha256(code).hexdigest(), "relocations": relocations}
    if len(results) < 5:
        raise ValueError("constructor and all four ordinary parser/populate functions must be present")
    for token in ["C2", "ReadPairs", "PopulateKeys", "PopulateTitleKeys", "PopulateTitleKey"]:
        if not any(token in name for name in results):
            raise ValueError("missing ordinary method " + token)
    return results
