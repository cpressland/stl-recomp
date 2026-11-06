"""Find code reached only through data: entries the catalog misses.

Callbacks, function tables and vtables the RTTI pass did not reach are code
pointers stored in .rdata and .data; nothing calls them directly, so no
disassembler pass names them. A dword there is often just a number, so
only one that looks like a function start is taken: 16-byte aligned behind
ret/int3/nop padding, or a module export. Jump tables inside .text are the
lifter's own business.

    .venv/bin/python -I tools/gen/data_code_ptrs.py MODULE functions.json run_seeds.json

From tpw-recomp's, without its hand-written renderer's tables.
"""
import json
import struct
import sys

import pefile


def main(exe, catalog, out):
    known = {f['address'] for f in json.load(open(catalog))['functions']}
    pe = pefile.PE(exe)
    img = pe.get_memory_mapped_image()
    base = pe.OPTIONAL_HEADER.ImageBase
    code = [(base + s.VirtualAddress, base + s.VirtualAddress + s.Misc_VirtualSize)
            for s in pe.sections if s.Characteristics & 0x20000000]
    exports = set()
    if hasattr(pe, 'DIRECTORY_ENTRY_EXPORT'):
        exports = {base + e.address for e in pe.DIRECTORY_ENTRY_EXPORT.symbols if e.address}
    seeds = {a: None for a in exports
             if a not in known and any(lo <= a < hi for lo, hi in code)}
    for s in pe.sections:
        if s.Characteristics & 0x20000000:
            continue
        start = s.VirtualAddress
        for off in range(start, start + min(s.Misc_VirtualSize, len(img) - start) - 3, 4):
            v = struct.unpack_from('<I', img, off)[0]
            if v in known or v in seeds or not any(lo <= v < hi for lo, hi in code):
                continue
            if v not in exports and (v % 16 or img[v - base - 1] not in (0xC3, 0xCC, 0x90)):
                continue
            seeds[v] = base + off
    json.dump([{'address': a, 'from': f} for a, f in sorted(seeds.items())], open(out, 'w'), indent=0)
    print(f'{len(seeds)} code pointers in data and exports -> {out}')


if __name__ == '__main__':
    main(*sys.argv[1:4])
