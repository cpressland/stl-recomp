"""Write a DLL moved to another base, its relocations applied.

A lifted module's code names absolute addresses, so each module is lifted,
and mapped, at one fixed base, and no two may overlap. `d3dx9_27.dll`
prefers 0x00400000, where `Legacy.exe` is; it is moved to MODULES' base
for it before it is analysed and lifted (the host moves the game folder's
own copy the same way as it maps it).

The relocations are applied here, to the file's bytes, and ImageBase is
changed; nothing else is touched. (pefile's relocate_image also rewrites
the import table it has parsed, and left d3dx9_27's unreadable: every tool
after it then saw a DLL with no imports.)

    python3 -I tools/gen/rebase.py game/d3dx9_27.dll 0x01000000 build/analysis/d3dx9/d3dx9_27.dll
"""
import os
import struct
import sys

import pefile


def rebase(src, base, dst):
    data = bytearray(open(src, 'rb').read())
    pe = pefile.PE(data=bytes(data), fast_load=True)
    old = pe.OPTIONAL_HEADER.ImageBase
    if old != base:
        pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_BASERELOC']])
        if not hasattr(pe, 'DIRECTORY_ENTRY_BASERELOC'):
            raise SystemExit(f'{src} has no relocations: it cannot move')
        delta = (base - old) & 0xFFFFFFFF
        n = 0
        for block in pe.DIRECTORY_ENTRY_BASERELOC:
            for e in block.entries:
                if e.type == 0:                          # IMAGE_REL_BASED_ABSOLUTE: padding
                    continue
                if e.type != 3:                          # HIGHLOW: the only kind a PE32 DLL has
                    raise SystemExit(f'{src}: relocation type {e.type} at RVA 0x{e.rva:X}')
                off = pe.get_offset_from_rva(e.rva)
                v = struct.unpack_from('<I', data, off)[0]
                struct.pack_into('<I', data, off, (v + delta) & 0xFFFFFFFF)
                n += 1
        struct.pack_into('<I', data, pe.OPTIONAL_HEADER.get_file_offset() + 28, base)    # ImageBase
    os.makedirs(os.path.dirname(os.path.abspath(dst)), exist_ok=True)
    with open(dst, 'wb') as f:
        f.write(data)


if __name__ == '__main__':
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    rebase(sys.argv[1], int(sys.argv[2], 0), sys.argv[3])
