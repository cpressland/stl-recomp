"""Copy the files out of a set of Microsoft cabinets.

The disc's install is one cabinet set spanning two files, `Data1.cab` (400 MB)
and `Data11.cab` (1.97 GB): the last folder of the first carries on as the
first folder of the second, its last data block split between them, and a
file can start in one and end in the other. Both are MSZIP (deflate in blocks
of up to 32 KB, each starting `CK`, the blocks before it its dictionary). The
1.2 patch's cabinet, inside its installer database, is LZX (lzx.py).

A cabinet is read where it lies, a stretch of a bigger file if need be (the
disc image), and a folder is decompressed as it is read, so nothing the size
of a cabinet is held in memory.

    python3 -I tools/disc/cab.py OUT Data1.cab Data11.cab
"""
import calendar
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lzx import LZX  # noqa: E402

STORED, MSZIP, QUANTUM, LZX_KIND = 0, 1, 2, 3
PREV, NEXT, RESERVE = 1, 2, 4
FROM_PREV, TO_NEXT, PREV_AND_NEXT = 0xFFFD, 0xFFFE, 0xFFFF


class Part:
    """A cabinet: a file, or `size` bytes of one from `base` (bytes in memory too)."""

    def __init__(self, source, base=0, size=None, label=None):
        self.source, self.base = source, base
        self.label = label or (source if isinstance(source, str) else 'cabinet')
        if isinstance(source, (bytes, bytearray)):
            self.size = len(source) if size is None else size
        else:
            self.size = os.path.getsize(source) - base if size is None else size
        self.f = None

    def read(self, at, n):
        if at < 0 or at + n > self.size:
            raise ValueError(f'{self.label}: a read past its end')
        if isinstance(self.source, (bytes, bytearray)):
            return bytes(self.source[self.base + at:self.base + at + n])
        if self.f is None:
            self.f = open(self.source, 'rb')
        self.f.seek(self.base + at)
        data = self.f.read(n)
        if len(data) != n:
            raise ValueError(f'{self.label}: ends early')
        return data

    def close(self):
        if self.f:
            self.f.close()
            self.f = None


def _cstr(blob, at):
    end = blob.index(b'\0', at)
    return blob[at:end], end + 1


class Cabinet:
    def __init__(self, part):
        self.part = part
        head = part.read(0, min(part.size, 1 << 16))
        (sig, _r1, size, _r2, coff_files, _r3, minor, major, n_folders, n_files,
         self.flags, self.set_id, self.index) = struct.unpack_from('<4sIIIIIBBHHHHH', head, 0)
        if sig != b'MSCF' or (major, minor) != (1, 3):
            raise ValueError(f'{part.label}: not a cabinet')
        at, fold_res, self.data_res = 36, 0, 0
        if self.flags & RESERVE:
            hdr_res, fold_res, self.data_res = struct.unpack_from('<HBB', head, at)
            at += 4 + hdr_res
        for flag in (PREV, NEXT):
            if self.flags & flag:
                _name, at = _cstr(head, at)
                _disk, at = _cstr(head, at)
        self.folders = []
        for _ in range(n_folders):
            start, blocks, kind = struct.unpack_from('<IHH', head, at)
            self.folders.append((start, blocks, kind))
            at += 8 + fold_res
        names = part.read(coff_files, min(part.size - coff_files, max(1 << 16, n_files * 300)))
        self.files, at = [], 0
        for _ in range(n_files):
            while at + 16 > len(names) or b'\0' not in names[at + 16:]:
                names = part.read(coff_files, min(part.size - coff_files, len(names) * 2))
            usize, offset, folder, date, time, attrs = struct.unpack_from('<IIHHHH', names, at)
            name, at = _cstr(names, at + 16)
            name = name.decode('utf-8' if attrs & 0x80 else 'latin-1').replace('\\', '/')
            self.files.append((name, usize, offset, folder, _dos_time(date, time)))

    def blocks(self, folder):
        """(compressed bytes, uncompressed size) of each of a folder's data blocks."""
        start, count, _kind = self.folders[folder]
        at = start
        for _ in range(count):
            _csum, csize, usize = struct.unpack('<IHH', self.part.read(at, 8))
            at += 8 + self.data_res
            yield self.part.read(at, csize), usize
            at += csize


def _dos_time(date, time):
    try:
        return calendar.timegm((1980 + (date >> 9), (date >> 5) & 15, date & 31,
                                time >> 11, (time >> 5) & 63, (time & 31) * 2))
    except (OverflowError, ValueError):
        return None


def _safe_name(name):
    parts = name.split('/')
    if not name or name.startswith('/') or any(p in ('', '.', '..') for p in parts):
        raise ValueError(f'{name!r} is not a plain relative path')
    return name


class CabinetSet:
    """Cabinets given in order, a folder spanning two counted once."""

    def __init__(self, parts):
        self.cabs = [Cabinet(p) for p in parts]
        for a, b in zip(self.cabs, self.cabs[1:]):
            if b.set_id != a.set_id or b.index != a.index + 1 or not (a.flags & NEXT and b.flags & PREV):
                raise ValueError(f'{b.part.label} does not follow {a.part.label} in one set')
        # A spanned folder: [(cabinet, its folder index)], the first part's kind.
        self.folders, where = [], []
        for c, cab in enumerate(self.cabs):
            ids = []
            for i, folder in enumerate(cab.folders):
                if i == 0 and cab.flags & PREV and self.folders:
                    self.folders[-1].append((c, i))
                else:
                    self.folders.append([(c, i)])
                ids.append(len(self.folders) - 1)
            where.append(ids)
        self.files, seen = [], set()
        for c, cab in enumerate(self.cabs):
            for name, usize, offset, folder, mtime in cab.files:
                if folder in (FROM_PREV, PREV_AND_NEXT):
                    g = where[c][0]
                elif folder == TO_NEXT:
                    g = where[c][-1]
                else:
                    g = where[c][folder]
                if (g, offset, name) not in seen:
                    seen.add((g, offset, name))
                    self.files.append((name, usize, g, offset, mtime))

    def kind(self, g):
        c, i = self.folders[g][0]
        return self.cabs[c].folders[i][2]

    def _blocks(self, g):
        """A folder's blocks across its cabinets; a block split at the end of one
        cabinet (uncompressed size 0 there) joined to its rest in the next."""
        held = b''
        for c, i in self.folders[g]:
            for data, usize in self.cabs[c].blocks(i):
                if usize == 0:
                    held += data
                    continue
                yield held + data, usize
                held = b''
        if held:
            raise ValueError('a folder ends in the middle of a block')

    def unpack(self, g):
        """Yield a folder's uncompressed bytes, a block at a time."""
        kind = self.kind(g)
        if kind & 0xF == STORED:
            for data, usize in self._blocks(g):
                yield data
        elif kind & 0xF == MSZIP:
            history = b''
            for data, usize in self._blocks(g):
                if data[:2] != b'CK':
                    raise ValueError('an MSZIP block without its CK')
                d = zlib.decompressobj(-15, zdict=history) if history else zlib.decompressobj(-15)
                out = d.decompress(data[2:], usize) + d.flush()
                if len(out) != usize:
                    raise ValueError('an MSZIP block of the wrong size')
                history = (history + out)[-32768:]
                yield out
        elif kind & 0xF == LZX_KIND:
            # LZX's bits run on from one block into the next, so the folder's
            # input is gathered first (only the patch's 1.4 MB uses it).
            blocks = list(self._blocks(g))
            yield from LZX((kind >> 8) & 0x1F, b''.join(d for d, _ in blocks)).frames(u for _, u in blocks)
        else:
            raise ValueError(f'cabinet compression {kind & 0xF} (Quantum?) is not supported')

    def extract(self, write, progress=None):
        """write(name, data, mtime) for every file; progress(done, total) in bytes."""
        total = sum(f[1] for f in self.files)
        done = 0
        by_folder = {}
        for f in self.files:
            by_folder.setdefault(f[2], []).append(f)
        for g in sorted(by_folder):
            todo = sorted(by_folder[g], key=lambda f: (f[3], f[1]))
            buf, buf_at, i = bytearray(), 0, 0
            stream = self.unpack(g)
            while i < len(todo):
                name, usize, _g, offset, mtime = todo[i]
                while buf_at + len(buf) < offset + usize:
                    try:
                        buf += next(stream)
                    except StopIteration:
                        raise ValueError(f'{name}: past the end of its folder') from None
                write(_safe_name(name), bytes(buf[offset - buf_at:offset - buf_at + usize]), mtime)
                done += usize
                if progress:
                    progress(done, total)
                i += 1
                # Drop what no file still to come starts in.
                keep = todo[i][3] if i < len(todo) else buf_at + len(buf)
                if keep > buf_at:
                    del buf[:keep - buf_at]
                    buf_at = keep
        for cab in self.cabs:
            cab.part.close()
        return len(self.files)


def write_into(out):
    """A write() that puts each file under `out`, dated as the cabinet says."""
    def write(name, data, mtime):
        dest = os.path.join(out, *name.split('/'))
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        with open(dest, 'wb') as f:
            f.write(data)
        if mtime is not None:
            os.utime(dest, (mtime, mtime))
    return write


if __name__ == '__main__':
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    s = CabinetSet([Part(p) for p in sys.argv[2:]])
    print(f'{s.extract(write_into(sys.argv[1]))} files')
