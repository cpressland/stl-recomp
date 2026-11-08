"""The modules lifted, where each is mapped, and which build of each.

Bases are fixed (a lifted module's code names its own addresses) and must
not overlap: Legacy.exe and the two Visual C++ 7.1 runtime DLLs keep their
own, and d3dx9_27.dll, which also wants 0x00400000, moves to 0x01000000
(tools/gen/rebase.py), above the game's image and below where win32hle
puts the guest's heap (0x10000000 up), so the host can map every module
before the first allocation. The missions' script DLLs move likewise,
from 0x02000000.
"""

# name, file in the game folder (/ between folders), base, PE timestamp of the build supported
MODULES = (
    ('legacy',  'Legacy.exe',   0x00400000, 0x45BE63B4),   # 1.2, 9 Feb 2007
    ('d3dx9',   'd3dx9_27.dll', 0x01000000, 0x42E18E12),   # DirectX 9.0c, August 2005
    ('msvcr71', 'msvcr71.dll',  0x7C340000, 0x3E561EAC),   # 7.10.3052.4
    ('msvcp71', 'msvcp71.dll',  0x7C3A0000, 0x3E77EEBB),   # 7.10.3077.0
) + (
    # The missions' scripts: DLLs the game loads (LoadLibrary) as a mission
    # starts and frees as it ends, an S (the script) and an R that imports
    # it, each with its own static C runtime. All want 0x10000000; each moves
    # to its own 256 KB from 0x02000000, above d3dx9_27.dll.
    ('m_coopwaver', 'missions/CoopWaveR.drl', 0x02000000, 0x454FB7DB),
    ('m_coopwaves', 'missions/CoopWaveS.dsl', 0x02040000, 0x454FB7D9),
    ('m_escortr', 'missions/escortR.drl', 0x02080000, 0x4511B427),
    ('m_escorts', 'missions/escortS.dsl', 0x020C0000, 0x4511B425),
    ('m_raiderr', 'missions/RaiderR.drl', 0x02100000, 0x44F47B3C),
    ('m_raiders', 'missions/RaiderS.dsl', 0x02140000, 0x44F47B3A),
    ('m_stl12r', 'missions/STL12R.drl', 0x02180000, 0x456CA31C),
    ('m_stl12s', 'missions/STL12S.dsl', 0x021C0000, 0x456CA314),
    ('m_stl13r', 'missions/STL13R.drl', 0x02200000, 0x454A4E99),
    ('m_stl13s', 'missions/STL13S.dsl', 0x02240000, 0x454A4E98),
    ('m_stl14r', 'missions/STL14R.drl', 0x02280000, 0x454F8F77),
    ('m_stl14s', 'missions/STL14S.dsl', 0x022C0000, 0x454F8F76),
    ('m_stl15r', 'missions/STL15R.drl', 0x02300000, 0x4550A032),
    ('m_stl15s', 'missions/STL15S.dsl', 0x02340000, 0x4550A030),
    ('m_stl16r', 'missions/STL16R.drl', 0x02380000, 0x454FA481),
    ('m_stl16s', 'missions/STL16S.dsl', 0x023C0000, 0x454FA47F),
    ('m_stl21thesqueezer', 'missions/STL21TheSqueezeR.drl', 0x02400000, 0x454F9110),
    ('m_stl21thesqueezes', 'missions/STL21TheSqueezeS.dsl', 0x02440000, 0x454F910E),
    ('m_stl22r', 'missions/STL22R.drl', 0x02480000, 0x4552510A),
    ('m_stl22s', 'missions/STL22S.dsl', 0x024C0000, 0x45525108),
    ('m_stl23r', 'missions/STL23R.drl', 0x02500000, 0x454A542F),
    ('m_stl23s', 'missions/STL23S.dsl', 0x02540000, 0x454A542D),
    ('m_stl24r', 'missions/STL24R.drl', 0x02580000, 0x4558F0FA),
    ('m_stl24s', 'missions/STL24S.dsl', 0x025C0000, 0x4558F0F9),
    ('m_stl25fulgr', 'missions/STL25FulgR.drl', 0x02600000, 0x45520BA4),
    ('m_stl25fulgs', 'missions/STL25FulgS.dsl', 0x02640000, 0x45520BA2),
    ('m_stl31r', 'missions/STL31R.drl', 0x02680000, 0x454FAF79),
    ('m_stl31s', 'missions/STL31S.dsl', 0x026C0000, 0x454FAF77),
    ('m_stl32r', 'missions/STL32R.drl', 0x02700000, 0x4558F105),
    ('m_stl32s', 'missions/STL32S.dsl', 0x02740000, 0x4558F103),
    ('m_stl33r', 'missions/STL33R.drl', 0x02780000, 0x4558E493),
    ('m_stl33s', 'missions/STL33S.dsl', 0x027C0000, 0x4558E491),
    ('m_stl34r', 'missions/STL34R.drl', 0x02800000, 0x454FA95E),
    ('m_stl34s', 'missions/STL34S.dsl', 0x02840000, 0x454FA95C),
    ('m_stl35r', 'missions/STL35R.drl', 0x02880000, 0x454D0633),
    ('m_stl35s', 'missions/STL35S.dsl', 0x028C0000, 0x454D0631),
    ('m_vanguardr', 'missions/VanguardR.drl', 0x02900000, 0x44F47B43),
    ('m_vanguards', 'missions/VanguardS.dsl', 0x02940000, 0x44F47B41),
)

# Modules the game loads itself, by LoadLibrary, rather than at the start.
ON_DEMAND = {m[0] for m in MODULES if m[1].startswith('missions/')}


def by_name(name):
    for m in MODULES:
        if m[0] == name:
            return m
    raise KeyError(name)
