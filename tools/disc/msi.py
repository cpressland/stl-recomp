"""Read a Windows Installer database (.msi): its tables, its streams, and
where each of its files is installed.

An .msi is an OLE compound file. Its streams' names are packed two
characters to a UTF-16 code unit; a table is the stream `!Name`, stored a
column at a time, its strings as indexes into one shared pool
(`!_StringPool` lengths, `!_StringData` bytes). `!_Columns` says each
table's columns: their order, and whether each is a string (an index two or
three bytes wide) or an integer of two or four bytes, stored with its top
bit flipped.

    python3 -I tools/disc/msi.py "Star Trek Legacy.msi" File Directory
    python3 -I tools/disc/msi.py "Star Trek Legacy.msi" --tree
"""
import os
import struct
import sys

import olefile

STRING = 0x0800        # a column's type: a string, else an integer
WIDTH = 0x00FF         # ... and an integer's width in bytes


def _char(x):
    return '0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz._'[x]


def stream_name(packed):
    """A stream's real name from the one in the compound file."""
    out = []
    for ch in packed:
        c = ord(ch)
        if 0x3800 <= c < 0x4800:
            c -= 0x3800
            out += _char(c & 0x3F), _char(c >> 6)
        elif 0x4800 <= c < 0x4840:
            out.append(_char(c - 0x4800))
        elif c == 0x4840:
            out.append('!')             # a table, not a plain stream
        else:
            out.append(ch)
    return ''.join(out)


class Database:
    def __init__(self, source):
        """source: a path, or the database's bytes."""
        self.ole = olefile.OleFileIO(source)
        self.streams = {stream_name(e[-1]): e for e in self.ole.listdir()}
        pool, data = self.stream('!_StringPool'), self.stream('!_StringData')
        codepage = struct.unpack_from('<I', pool, 0)[0]
        self.ref = 3 if codepage & 0x80000000 else 2
        self.codepage = 'cp%d' % (codepage & 0xFFFF) if codepage & 0xFFFF else 'cp1252'
        self.strings = [''] + self._pool(pool, data)
        self.schema = {}
        cols = self._table('_Columns', [('Table', STRING), ('Number', 2), ('Name', STRING), ('Type', 2)])
        for row in cols:
            self.schema.setdefault(row['Table'], []).append((row['Number'], row['Name'], row['Type']))

    def stream(self, name):
        if name not in self.streams:
            raise KeyError(f'no stream {name!r} in the database')
        return self.ole.openstream(self.streams[name]).read()

    def _pool(self, pool, data):
        out, at, i = [], 0, 4
        while i + 4 <= len(pool):
            length, refs = struct.unpack_from('<HH', pool, i)
            i += 4
            if length == 0 and refs:            # a long string: its length follows
                length = struct.unpack_from('<I', pool, i)[0]
                i += 4
            out.append(data[at:at + length].decode(self.codepage, 'replace'))
            at += length
        return out

    def _table(self, name, cols):
        raw = self.streams.get('!' + name) and self.stream('!' + name) or b''
        widths = [self.ref if t & STRING else (2 if t & WIDTH <= 2 else 4) for _, t in cols]
        rows = len(raw) // sum(widths) if raw else 0
        columns, at = [], 0
        for (_, t), w in zip(cols, widths):
            vals = []
            for r in range(rows):
                v = int.from_bytes(raw[at + r * w:at + r * w + w], 'little')
                if t & STRING:
                    vals.append(self.strings[v] if v < len(self.strings) else '')
                else:
                    vals.append(None if v == 0 else v - (1 << (8 * w - 1)))
            columns.append(vals)
            at += rows * w
        return [dict(zip((c for c, _ in cols), r)) for r in zip(*columns)]

    def table(self, name):
        """Its rows, as dicts by column name ([] for a table it lacks)."""
        if name not in self.schema:
            return []
        return self._table(name, [(n, t) for _, n, t in sorted(self.schema[name])])


def long_name(field):
    """`SHORT~1|Long name` (and a directory's `target:source`): the long one."""
    return field.split(':')[0].split('|')[-1]


def _safe(part, what):
    if part in ('', '.', '..') or '/' in part or '\\' in part or '\0' in part:
        raise ValueError(f'{what} {part!r} is not a plain name')
    return part


def install_tree(db, root='INSTALLDIR'):
    """[(file key, path under `root` with '/', size, sequence)] for every file
    the database installs under `root`, in cabinet order. A file elsewhere
    (the system folder, say) is left out."""
    dirs = {d['Directory']: (d['Directory_Parent'], long_name(d['DefaultDir'])) for d in db.table('Directory')}
    comp = {c['Component']: c['Directory_'] for c in db.table('Component')}
    memo = {}

    def path(d, depth=0):
        if d == root:
            return []
        if d not in dirs or depth > 64:
            return None
        if d not in memo:
            parent, name = dirs[d]
            up = path(parent, depth + 1)
            memo[d] = None if up is None else up if name == '.' else up + [_safe(name, 'a folder')]
        return memo[d]

    out = []
    for f in db.table('File'):
        where = path(comp.get(f['Component_']))
        if where is None:
            continue
        out.append((f['File'], '/'.join(where + [_safe(long_name(f['FileName']), 'a file')]),
                    f['FileSize'], f['Sequence']))
    return sorted(out, key=lambda e: e[3])


def embedded_cabinet(db, name):
    """A cabinet the Media table names as `#name`: one of the database's streams."""
    return db.stream(name.lstrip('#'))


if __name__ == '__main__':
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    db = Database(sys.argv[1])
    if sys.argv[2] == '--tree':
        for key, p, size, seq in install_tree(db):
            print(f'{seq}\t{size}\t{key}\t{p}')
        sys.exit()
    for name in sys.argv[2:]:
        rows = db.table(name)
        cols = [n for _, n, _ in sorted(db.schema.get(name, []))]
        print('#', name, *cols, sep='\t')
        for r in rows:
            print(*('' if r[c] is None else r[c] for c in cols), sep='\t')
