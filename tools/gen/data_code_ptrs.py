"""Find code reached only through data: entries the catalog misses.

Callbacks, function tables, vtables and C++ static constructor tables are
code pointers stored in data; nothing calls them directly, so no
disassembler pass names them, and an indirect call to one at runtime finds
no lifted function (`ICALL: unresolved`). Two kinds are taken:

- a run of three or more code pointers in a row (a vtable, `__xc_a`..
  `__xc_z`, a callback table), each one not inside a catalogued function;
- a single pointer in a data section that looks like a function start:
  16-byte aligned behind ret/int3/nop padding, or an export.

Data is not only in the data sections. `d3dx9_27.dll` keeps its read-only
data (its import table, constructor table, vtables) inside `.text`, so the
stretches of a code section no catalogued function covers are read as data
too. Without that, msvcr71's `_initterm` called 55 of D3DX's static
constructors that were never lifted. A target that opens with int3 (a
dword of padding read as an address) is not taken.

And a function Microsoft built for hot-patching (/hotpatch: D3DX, most of
Windows) announces itself: five bytes of int3 or nop padding, then
`mov edi, edi`. Every such start in a code section is an entry, wherever
the catalog thought a function ended; 31 of D3DX's were missing, one called
through a table inside what the catalog took for a function's body.

    .venv/bin/python -I tools/gen/data_code_ptrs.py MODULE functions.json run_seeds.json

From tpw-recomp's, without its hand-written renderer's tables.
"""
import bisect
import json
import re
import struct
import sys

import pefile

RUN = 3


def main(exe, catalog, out):
    funcs = json.load(open(catalog))['functions']
    known = {f['address'] for f in funcs}
    iv = sorted((f['address'], max(f.get('end', f['address']), f['address'] + 1)) for f in funcs)
    los, his, top = [a for a, _ in iv], [], 0
    for _, b in iv:
        top = max(top, b)
        his.append(top)

    def inside(v):
        k = bisect.bisect_right(los, v) - 1
        return k >= 0 and v < his[k]

    pe = pefile.PE(exe)
    img = pe.get_memory_mapped_image()
    base = pe.OPTIONAL_HEADER.ImageBase
    code = [(base + s.VirtualAddress, base + s.VirtualAddress + s.Misc_VirtualSize)
            for s in pe.sections if s.Characteristics & 0x20000000]

    def is_code(v):
        return any(lo <= v < hi for lo, hi in code) and img[v - base] != 0xCC

    exports = set()
    if hasattr(pe, 'DIRECTORY_ENTRY_EXPORT'):
        exports = {base + e.address for e in pe.DIRECTORY_ENTRY_EXPORT.symbols if e.address}
    seeds = {a: None for a in exports if a not in known and is_code(a)}

    def take(v, at):
        if v not in known and v not in seeds and not inside(v):
            seeds[v] = at

    for s in pe.sections:
        start, is_text = s.VirtualAddress, s.Characteristics & 0x20000000
        n = min(s.Misc_VirtualSize, len(img) - start)
        run, last = [], None
        for off in range(0, n - 3, 4):
            va = base + start + off
            if is_text and inside(va):
                continue                     # a function's own bytes (its jump tables are the lifter's)
            v = struct.unpack_from('<I', img, start + off)[0]
            if is_code(v) and last == off - 4:
                run.append((v, va))
            else:
                if len(run) >= RUN:
                    for t, at in run:
                        take(t, at)
                run = [(v, va)] if is_code(v) else []
            last = off
            if not is_text and is_code(v) and \
                    (v in exports or (v % 16 == 0 and img[v - base - 1] in (0xC3, 0xCC, 0x90))):
                take(v, va)
        if len(run) >= RUN:
            for t, at in run:
                take(t, at)
    for s in pe.sections:
        if not s.Characteristics & 0x20000000:
            continue
        data = img[s.VirtualAddress:s.VirtualAddress + s.Misc_VirtualSize]
        for m in re.finditer(rb'(?<=[\xcc\x90]{5})\x8b\xff', data):
            v = base + s.VirtualAddress + m.start()
            if v not in known:
                seeds.setdefault(v, 'hotpatch')
    json.dump([{'address': a, 'from': f} for a, f in sorted(seeds.items())], open(out, 'w'), indent=0)
    print(f'{len(seeds)} code pointers in data and exports -> {out}')


if __name__ == '__main__':
    main(*sys.argv[1:4])
