"""The modules lifted, where each is mapped, and which build of each.

Bases are fixed (a lifted module's code names its own addresses) and must
not overlap: Legacy.exe and the two Visual C++ 7.1 runtime DLLs keep their
own, and d3dx9_27.dll, which also wants 0x00400000, moves to 0x01000000
(tools/gen/rebase.py), above the game's image and below where win32hle
puts the guest's heap (0x10000000 up), so the host can map every module
before the first allocation.
"""

# name, file in the game folder, base, PE timestamp of the build supported
MODULES = (
    ('legacy',  'Legacy.exe',   0x00400000, 0x45BE63B4),   # 1.2, 9 Feb 2007
    ('d3dx9',   'd3dx9_27.dll', 0x01000000, 0x42E18E12),   # DirectX 9.0c, August 2005
    ('msvcr71', 'msvcr71.dll',  0x7C340000, 0x3E561EAC),   # 7.10.3052.4
    ('msvcp71', 'msvcp71.dll',  0x7C3A0000, 0x3E77EEBB),   # 7.10.3077.0
)


def by_name(name):
    for m in MODULES:
        if m[0] == name:
            return m
    raise KeyError(name)
