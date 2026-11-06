"""LZX, as Microsoft cabinets carry it, in plain Python.

Only the 1.2 patch's cabinet (1.4 MB in, 4.4 MB out) is LZX, so this is
written to be followed, not to be fast. It is the format as libmspack's
`lzxd.c` reads it:

- the input is 16-bit little-endian words, read most significant bit first;
- the output comes in frames of 32 KB (a cabinet's data blocks), and after
  each frame the input is realigned to a word;
- the stream opens with one bit saying whether x86 CALL (E8) targets were
  made absolute, and if so the 32-bit size they were made against; the
  window keeps the absolute form, and each frame is turned back on its way
  out (all but its last 10 bytes, the first 32768 frames only);
- blocks are verbatim, aligned (offsets' low three bits from their own
  tree), or uncompressed (realigned, then the three repeat offsets as 12
  bytes, then the bytes, padded to an even count); a block's size need not
  match a frame's;
- trees are sent as code lengths, themselves coded by a 20-symbol pretree,
  each length a change (mod 17) from the same tree's in the block before.
"""

VERBATIM, ALIGNED, UNCOMPRESSED = 1, 2, 3
FRAME = 32768
MIN_MATCH = 2
NUM_CHARS = 256
PRETREE, ALIGNED_SYMS, LENGTH_SYMS = 20, 8, 249
SLOTS = {15: 30, 16: 32, 17: 34, 18: 36, 19: 38, 20: 42, 21: 50}

EXTRA = []
for _i in range(52):
    EXTRA.append(min(max(_i // 2 - 1, 0), 17))
BASE = [0]
for _i in range(51):
    BASE.append(BASE[-1] + (1 << EXTRA[_i]))


class Error(ValueError):
    pass


class Tree:
    """A canonical Huffman code, decoded by one look at its longest code's bits."""

    def __init__(self, lengths):
        self.bits = max(lengths) if lengths else 0
        self.table = None
        if not self.bits:
            return                      # empty: allowed, so long as it is never used
        table = [None] * (1 << self.bits)
        code = 0
        for length in range(1, self.bits + 1):
            for sym, n in enumerate(lengths):
                if n == length:
                    shift = self.bits - length
                    lo, hi = code << shift, (code + 1) << shift
                    if hi > len(table):
                        raise Error('a code tree is over-full')
                    table[lo:hi] = [(sym, length)] * (hi - lo)
                    code += 1
            code <<= 1
        self.table = table


class LZX:
    def __init__(self, window_bits, data):
        if window_bits not in SLOTS:
            raise Error(f'an LZX window of 2^{window_bits}')
        self.data, self.pos = data, 0
        self.buf = self.count = 0                 # bits read, not yet used
        self.slots = SLOTS[window_bits]
        self.main_len = [0] * (NUM_CHARS + self.slots * 8)
        self.length_len = [0] * LENGTH_SYMS
        self.r = [1, 1, 1]
        self.window = bytearray()                 # all output so far, E8s absolute
        self.block_type = None
        self.block_left = self.block_len = 0
        self.header = False
        self.e8_size = 0
        self.frame = 0

    # ---- bits ------------------------------------------------------------

    def _ensure(self, n):
        while self.count < n:
            d = self.data
            word = d[self.pos] | d[self.pos + 1] << 8 if self.pos + 1 < len(d) else 0
            self.pos += 2
            self.buf = (self.buf << 16) | word
            self.count += 16

    def bits(self, n):
        if n == 0:
            return 0
        self._ensure(n)
        self.count -= n
        v = self.buf >> self.count
        self.buf &= (1 << self.count) - 1
        return v

    def sym(self, tree):
        if tree.table is None:
            raise Error('a symbol from an empty tree')
        self._ensure(tree.bits)
        entry = tree.table[self.buf >> (self.count - tree.bits)]
        if entry is None:
            raise Error('a code no tree has')
        s, n = entry
        self.count -= n
        self.buf &= (1 << self.count) - 1
        return s

    def _byte(self):
        if self.pos >= len(self.data):
            raise Error('the input ends early')
        b = self.data[self.pos]
        self.pos += 1
        return b

    # ---- trees -----------------------------------------------------------

    def _lengths(self, lens, first, last):
        pre = Tree([self.bits(4) for _ in range(PRETREE)])
        x = first
        while x < last:
            z = self.sym(pre)
            if z == 17:
                run, z = self.bits(4) + 4, None
            elif z == 18:
                run, z = self.bits(5) + 20, None
            elif z == 19:
                run = self.bits(1) + 4
                z = (lens[x] - self.sym(pre)) % 17
            else:
                run, z = 1, (lens[x] - z) % 17
            if x + run > last:
                raise Error('a run of code lengths past its tree')
            for k in range(x, x + run):
                lens[k] = 0 if z is None else z
            x += run

    def _block_header(self):
        if self.block_type == UNCOMPRESSED and self.block_len & 1:
            self._byte()                          # an odd stored block's pad
        self.block_type = self.bits(3)
        hi = self.bits(16)
        self.block_left = self.block_len = (hi << 8) | self.bits(8)
        if self.block_type == ALIGNED:
            self.aligned = Tree([self.bits(3) for _ in range(ALIGNED_SYMS)])
        if self.block_type in (VERBATIM, ALIGNED):
            self._lengths(self.main_len, 0, NUM_CHARS)
            self._lengths(self.main_len, NUM_CHARS, len(self.main_len))
            self.main = Tree(self.main_len)
            self._lengths(self.length_len, 0, LENGTH_SYMS)
            self.length = Tree(self.length_len)
        elif self.block_type == UNCOMPRESSED:
            if self.count == 0:
                self._ensure(16)                  # 1 to 16 bits of padding, never 0
            self.buf = self.count = 0
            self.r = [int.from_bytes(bytes(self._byte() for _ in range(4)), 'little') for _ in range(3)]
        else:
            raise Error(f'LZX block type {self.block_type}')

    # ---- decoding ---------------------------------------------------------

    def _run(self, n):
        """Decode at least n bytes of the current block (a match may run over)."""
        w = self.window
        if self.block_type == UNCOMPRESSED:
            end = self.pos + n
            if end > len(self.data):
                raise Error('a stored block past the input')
            w += self.data[self.pos:end]
            self.pos = end
            return n
        aligned = self.block_type == ALIGNED
        main, length, r = self.main, self.length, self.r
        done = 0
        while done < n:
            m = self.sym(main)
            if m < NUM_CHARS:
                w.append(m)
                done += 1
                continue
            m -= NUM_CHARS
            ml = m & 7
            if ml == 7:
                ml += self.sym(length)
            ml += MIN_MATCH
            slot = m >> 3
            if slot > 2:
                extra = EXTRA[slot]
                off = BASE[slot] - 2
                if aligned:
                    if extra > 3:
                        off += (self.bits(extra - 3) << 3) + self.sym(self.aligned)
                    elif extra == 3:
                        off += self.sym(self.aligned)
                    elif extra > 0:
                        off += self.bits(extra)
                    else:
                        off = 1
                else:
                    off = off + self.bits(extra) if slot != 3 else 1
                r[2], r[1], r[0] = r[1], r[0], off
            elif slot == 0:
                off = r[0]
            elif slot == 1:
                off = r[1]
                r[1], r[0] = r[0], off
            else:
                off = r[2]
                r[2], r[0] = r[0], off
            if off > len(w):
                raise Error('a match from before the start')
            start = len(w) - off
            if off >= ml:
                w += w[start:start + ml]
            else:
                for k in range(ml):
                    w.append(w[start + k])
            done += ml
        return done

    def frames(self, sizes):
        """Yield each frame's bytes, the frames' sizes given in order."""
        for size in sizes:
            if not self.header:
                self.header = True
                if self.bits(1):
                    hi = self.bits(16)
                    self.e8_size = (hi << 16) | self.bits(16)
            start = len(self.window)
            todo = size
            while todo > 0:
                if self.block_left == 0:
                    self._block_header()
                run = min(self.block_left, todo)
                got = self._run(run)
                todo -= got
                self.block_left -= got
                if self.block_left < 0:
                    raise Error('a match past the end of its block')
            if len(self.window) - start != size:
                raise Error('a frame of the wrong size')
            # Realign to a word for the next frame.
            if self.count > 0:
                self._ensure(16)
            self.bits(self.count & 15)
            out = self.window[start:]
            if self.e8_size and self.frame < 32768 and size > 10:
                out = self._e8(out, start)
            self.frame += 1
            yield bytes(out)

    def _e8(self, data, at):
        data = bytearray(data)
        size, i, end = self.e8_size, 0, len(data) - 10
        while True:
            i = data.find(0xE8, i, end)
            if i < 0:
                return data
            pos = at + i
            a = int.from_bytes(data[i + 1:i + 5], 'little', signed=True)
            if -pos <= a < size:
                rel = a - pos if a >= 0 else a + size
                data[i + 1:i + 5] = (rel & 0xFFFFFFFF).to_bytes(4, 'little')
            i += 5
