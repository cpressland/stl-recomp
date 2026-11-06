"""Copy the files out of an ISO 9660 image, by their Joliet names.

The disc's long, mixed-case names (`Autorun/english.tre`,
`CAT_SPEECH.MAP`) are in its Joliet tree; the plain ISO 9660 tree has
8.3 upper-case ones (isowalk.py). With no Joliet tree, the plain names are
used. Each file keeps the date the disc gives it.

    python3 -I tools/disc/iso.py TPWORLD.ISO work/cd
"""
import calendar
import os
import struct
import sys

SECTOR = 2048


def _root(f):
    """The root directory record: Joliet's when the disc has one."""
    plain = None
    for n in range(16, 64):
        f.seek(n * SECTOR)
        vd = f.read(SECTOR)
        kind = vd[0]
        if vd[1:6] != b'CD001' or kind == 255:
            break
        if kind == 1 and plain is None:
            plain = vd[156:190]
        # A supplementary descriptor whose escapes name UCS-2 levels 1-3.
        if kind == 2 and vd[88:91] in (b'%/@', b'%/C', b'%/E'):
            return vd[156:190], True
    if plain is None:
        raise SystemExit('not an ISO 9660 image')
    return plain, False


def _mtime(rec):
    y, mo, d, h, mi, s, gmt = struct.unpack_from('<6Bb', rec, 18)
    try:
        return calendar.timegm((1900 + y, mo, d, h, mi, s)) - gmt * 15 * 60
    except (OverflowError, ValueError):
        return None


def walk(f, lba, size, joliet, path=''):
    """Yield (path, lba, length, mtime) for everything under a directory; a
    folder's path ends in '/' (the disc has empty ones the game needs)."""
    f.seek(lba * SECTOR)
    data = f.read(size)
    i = 0
    while i < len(data):
        n = data[i]
        if n == 0:                       # records do not cross a sector
            i = (i // SECTOR + 1) * SECTOR
            continue
        rec = data[i:i + n]
        ext, ln = struct.unpack_from('<I', rec, 2)[0], struct.unpack_from('<I', rec, 10)[0]
        flags, nl = rec[25], rec[32]
        raw = rec[33:33 + nl]
        i += n
        if raw in (b'\0', b'\1'):
            continue
        name = (raw.decode('utf-16-be') if joliet else raw.decode('latin-1')).split(';')[0]
        if not joliet and '.' in name and name.endswith('.'):
            name = name[:-1]
        if flags & 2:
            yield path + name + '/', ext, 0, _mtime(rec)
            yield from walk(f, ext, ln, joliet, path + name + '/')
        else:
            yield path + name, ext, ln, _mtime(rec)


def files(iso):
    with open(iso, 'rb') as f:
        root, joliet = _root(f)
        lba, size = struct.unpack_from('<I', root, 2)[0], struct.unpack_from('<I', root, 10)[0]
        return sorted(walk(f, lba, size, joliet))


def extract(iso, out, progress=None):
    """Write every file under `out`; progress(done_bytes, total_bytes)."""
    entries = files(iso)
    total = sum(e[2] for e in entries)
    done = 0
    with open(iso, 'rb') as f:
        for path, lba, ln, mtime in entries:
            dest = os.path.join(out, *path.split('/'))
            if path.endswith('/'):
                os.makedirs(dest, exist_ok=True)
                continue
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            f.seek(lba * SECTOR)
            with open(dest, 'wb') as o:
                left = ln
                while left:
                    chunk = f.read(min(left, 1 << 20))
                    if not chunk:
                        raise SystemExit(f'{iso}: {path} runs past the end of the image')
                    o.write(chunk)
                    left -= len(chunk)
            if mtime is not None:
                os.utime(dest, (mtime, mtime))
            done += ln
            if progress:
                progress(done, total)
    return sum(1 for e in entries if not e[0].endswith('/'))


if __name__ == '__main__':
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    print(f'{extract(sys.argv[1], sys.argv[2])} files')
