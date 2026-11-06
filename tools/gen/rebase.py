"""Write a DLL moved to another base, its relocations applied.

A lifted module's code names absolute addresses, so each module is lifted,
and mapped, at one fixed base, and no two may overlap. `d3dx9_27.dll`
prefers 0x00400000, where `Legacy.exe` is; it is moved to MODULES' base
for it before it is analysed and lifted, and the host maps this copy.

    python3 -I tools/gen/rebase.py game/d3dx9_27.dll 0x01000000 build/analysis/d3dx9_27/d3dx9_27.dll
"""
import os
import sys

import pefile


def rebase(src, base, dst):
    pe = pefile.PE(src)
    if pe.OPTIONAL_HEADER.ImageBase == base:
        data = open(src, 'rb').read()
    else:
        if not hasattr(pe, 'DIRECTORY_ENTRY_BASERELOC'):
            raise SystemExit(f'{src} has no relocations: it cannot move')
        pe.relocate_image(base)
        pe.OPTIONAL_HEADER.ImageBase = base
        data = pe.write()
    os.makedirs(os.path.dirname(os.path.abspath(dst)), exist_ok=True)
    with open(dst, 'wb') as f:
        f.write(data)


if __name__ == '__main__':
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    rebase(sys.argv[1], int(sys.argv[2], 0), sys.argv[3])
