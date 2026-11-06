"""Copy the files out of an InstallShield 11 setup.exe.

The 1.2 patch is a setup.exe with its files after the program (the PE's
overlay): `InstallShield\\0`, a count, then each file as a 312-byte header
(its name in 260 bytes, flags, size) and its bytes. A file whose flags have
bit 2 or 4 is encoded: each byte's nibbles swapped, XORed with a key made
from the file's own name (each character XOR 13 35 86 07 in turn), and
inverted, the key starting again every 1024 bytes.

    python3 -I tools/disc/isstream.py star_trek_legacy_1_2_patch.exe work/patch
"""
import os
import struct
import sys

MAGIC = b'InstallShield\0'
SEED = (0x13, 0x35, 0x86, 0x07)
HEADER, ENTRY, NAME = 46, 312, 260


def _overlay(blob):
    """Where the PE's sections end."""
    if blob[:2] != b'MZ':
        raise ValueError('not a Windows program')
    pe = struct.unpack_from('<I', blob, 0x3C)[0]
    if blob[pe:pe + 4] != b'PE\0\0':
        raise ValueError('not a PE program')
    n, opt = struct.unpack_from('<HxxxxxxxxxxxxH', blob, pe + 6)
    end, sec = 0, pe + 24 + opt
    for i in range(n):
        size, at = struct.unpack_from('<II', blob, sec + 40 * i + 16)
        end = max(end, at + size)
    return end


def _decode(data, name):
    key = bytes(c ^ SEED[i % 4] for i, c in enumerate(name.encode('latin-1')))
    # Every byte is ~(key ^ swap(b)); a table per key position does it fast.
    swap = bytes(((b >> 4) | (b << 4)) & 0xFF for b in range(256))
    tables = [bytes(~(k ^ swap[b]) & 0xFF for b in range(256)) for k in key]
    out = bytearray(len(data))
    for i in range(len(key)):
        # Positions with i == (p % 1024) % len(key): the same table.
        for start in range(0, len(data), 1024):
            block = data[start + i:start + 1024:len(key)]
            out[start + i:start + i + len(block) * len(key):len(key)] = block.translate(tables[i])
    return bytes(out)


def files(blob):
    """Yield (name, data) for each file in the stream."""
    at = blob.find(MAGIC, _overlay(blob))
    if at < 0:
        raise ValueError('no InstallShield stream after the program')
    count = struct.unpack_from('<H', blob, at + len(MAGIC))[0]
    at += HEADER
    for _ in range(count):
        name = blob[at:at + NAME].split(b'\0')[0].decode('latin-1')
        flags, _unk, size = struct.unpack_from('<III', blob, at + NAME)
        at += ENTRY
        data = blob[at:at + size]
        if len(data) != size:
            raise ValueError(f'{name}: the stream ends early')
        at += size
        if not name or '/' in name or '\\' in name or name in ('.', '..'):
            raise ValueError(f'{name!r} is not a plain file name')
        yield name, _decode(data, name) if flags & 6 else data


def extract(path, out):
    with open(path, 'rb') as f:
        blob = f.read()
    os.makedirs(out, exist_ok=True)
    n = 0
    for name, data in files(blob):
        with open(os.path.join(out, name), 'wb') as o:
            o.write(data)
        n += 1
    return n


if __name__ == '__main__':
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    print(f'{extract(sys.argv[1], sys.argv[2])} files')
