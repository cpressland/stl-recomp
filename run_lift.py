#!/usr/bin/env python3
"""Star Trek: Legacy lift driver: every module's catalog -> one folder of C.

Drives pcrecomp's shared `tools/lift/generate.py` + `lift32`, in the shape of
tpw-recomp's run_lift.py, over the four modules of tools/gen/modules.py:
Legacy.exe, d3dx9_27.dll (rebased) and the Visual C++ 7.1 runtime DLLs. Each
module is lifted at its own base, so its functions' names (`sub_<va>`) are
unique across all of them, and they share one dispatch table: a call through
an import slot is `RECOMP_ICALL(MEM32(slot))`, so once the host binds a slot
to another module's export, the call finds that module's lifted function.

* Every catalogued function is lifted (`--all` is the only mode so far), and
  branch and call targets the catalog missed are added round by round.
* Functions that install an MSVC C++ EH frame get a landing pad
  (tools/lift/eh32.py) at each place one of their catch blocks resumes.
* msvcr71's `_CxxThrowException` is the host's (HOOKS): guest throws are
  dispatched to the guest's own catch blocks (win32hle eh32.c).
* `cpuid` asks the host (stl_cpuid), which decides what d3dx9 may use.

    python3 run_lift.py --work work/analysis --out work/gen
    python3 run_lift.py --work work/analysis --out work/gen --modules msvcr71,msvcp71
"""
import argparse
import filecmp
import hashlib
import json
import os
import shutil
import sys
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
_TOOLS = os.path.join(os.environ.get('PCRECOMP', os.path.join(_HERE, '..', 'pcrecomp')), 'tools')
sys.path.insert(0, os.path.join(_TOOLS, 'lift'))
sys.path.insert(0, os.path.join(_TOOLS, 'pe'))
sys.path.insert(0, os.path.join(_HERE, 'tools', 'gen'))

import pefile                                              # noqa: E402
from capstone import Cs, CS_ARCH_X86, CS_MODE_32          # noqa: E402
import eh32                                                # noqa: E402
from generate import (EXTENT_REACH, find_splits, true_extent,  # noqa: E402
                      linear_disassemble_function, lift_function_linear, write_chunk)
from lift32 import Lifter                                  # noqa: E402
from modules import MODULES                                # noqa: E402
from pe_analyze import analyze_pe, build_iat_map           # noqa: E402

TITLE = 'Star Trek: Legacy'

# Exports whose body is the host's: module -> {export name: C function}.
HOOK_EXPORTS = {
    'msvcr71': {'_CxxThrowException': 'hle_eh_throw'},
}

# Functions per .c file: D3DX's are long (its HLSL compiler), and at 400 a
# few of its files kept one core busy for minutes after the rest were done.
CHUNK = {'d3dx9': 100}

# Remaster patches: C emitted after one instruction, by module (va -> C).
PATCHES = {}

CPUID_LIFTED = 'CPUID(eax, ebx, ecx, edx);'
CPUID_HOST = '{ extern void stl_cpuid(void); RECOMP_REGS_OUT(); stl_cpuid(); RECOMP_REGS_IN(); }'


def apply_patches(body, patches):
    """Add each patch's C after the line that lifts its instruction."""
    for va, c in patches.items():
        tag = '/* 0x%08X:' % va
        k = body.find(tag)
        if k != -1:
            e = body.index('\n', k)
            body = body[:e + 1] + '    ' + c + '\n' + body[e + 1:]
    return body


def lifter_stamp():
    """What a lift depends on besides the game: the lifter and this file."""
    h = hashlib.sha1()
    for p in (os.path.join(_TOOLS, 'lift', n) for n in ('lift32.py', 'lift32_sse.py', 'generate.py', 'eh32.py')):
        h.update(open(p, 'rb').read().replace(b'\r\n', b'\n'))
    for p in (os.path.abspath(__file__), os.path.join(_HERE, 'tools', 'gen', 'modules.py')):
        h.update(open(p, 'rb').read().replace(b'\r\n', b'\n'))
    return h.hexdigest()[:16]


class Module:
    """One module's image, catalog and lift."""

    def __init__(self, work, name, file, base, stamp):
        self.name, self.base, self.stamp = name, base, stamp
        self.path = os.path.join(work, name, file)
        self.work = os.path.join(work, name)
        pe = pefile.PE(self.path)
        if pe.FILE_HEADER.TimeDateStamp != stamp or pe.OPTIONAL_HEADER.ImageBase != base:
            sys.exit(f'{self.path}: not the build at the base this project lifts (tools/gen/analyse.py)')
        self.image = pe.get_memory_mapped_image()
        self.exports = {}
        if hasattr(pe, 'DIRECTORY_ENTRY_EXPORT'):
            for e in pe.DIRECTORY_ENTRY_EXPORT.symbols:
                if e.name and e.address:
                    self.exports[e.name.decode()] = base + e.address
        self.entry = base + pe.OPTIONAL_HEADER.AddressOfEntryPoint
        info = analyze_pe(self.path)
        self.iat = build_iat_map(info)
        self.cs, self.ce = info.code_start, info.code_end
        self.code = self.image[self.cs - base:self.ce - base]

    def read(self, va, n):
        off = va - self.base
        return self.image[off:off + n] if 0 <= off and off + n <= len(self.image) else None

    def catalog(self):
        cat = json.load(open(os.path.join(self.work, 'functions.json')))
        byaddr = {f['address']: f for f in cat['functions'] if self.cs <= f['address'] < self.ce}
        want = set()
        for p in ('rtti_seeds.json', 'run_seeds.json'):
            path = os.path.join(self.work, p)
            if os.path.exists(path):
                want |= {e['address'] for e in json.load(open(path))
                         if self.cs <= e['address'] < self.ce and e['address'] not in byaddr}
        want |= {va for va in self.exports.values() if self.cs <= va < self.ce and va not in byaddr}
        known = sorted(set(byaddr) | want)
        nxt = {a: (known[i + 1] if i + 1 < len(known) else self.ce) for i, a in enumerate(known)}
        for a in want:
            byaddr[a] = {'address': a, 'end': min(nxt[a], a + 0x100), 'calls_to': [], 'entry_kind': 'start'}
        return byaddr, len(want)


def catches(m, head):
    """True if a handler stub pushed (or put in eax) in these instructions
    names a FuncInfo with try blocks. A frame without them (the
    frame-pointer-less `push -1; push handler` form MSVC uses when there is
    only unwinding to do) needs no landing pad: the runtime runs its unwind
    funclets from the guest's own data."""
    for i in head:
        if (i.mnemonic == 'push' and i.op_str.startswith('0x')) or \
                (i.mnemonic == 'mov' and i.op_str.startswith('eax, 0x')):
            fi = eh32.funcinfo(m.read, int(i.op_str.split()[-1], 16))
            if fi is not None and int.from_bytes(m.read(fi + 12, 4), 'little'):
                return True
    return False


def lift_module(m, out, idx, entries, log):
    """Lift one module into out/, chunk files from idx; returns (next idx, stats)."""
    byaddr, injected = m.catalog()
    hooks = {m.exports[n]: fn for n, fn in HOOK_EXPORTS.get(m.name, {}).items()}
    missing = set(HOOK_EXPORTS.get(m.name, {})) - set(m.exports)
    if missing:
        sys.exit(f'{m.name} has no export {", ".join(sorted(missing))} to hook')
    patches = PATCHES.get(m.name, {})
    log(f'[{m.name}] base=0x{m.base:08X} code=0x{m.cs:08X}-0x{m.ce:08X} IAT={len(m.iat)} '
        f'catalog={len(byaddr) - injected} seeds injected={injected}')

    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    lifter = Lifter(iat_map=m.iat, lifted=set(byaddr), precise_carry=True, precise_sbb=True)
    starts = {a for a in byaddr if byaddr[a].get('entry_kind') != 'alias'}
    splits = find_splits(md, m.code, m.cs, m.ce, starts)
    starts -= splits
    first = idx
    st = {'lifted': 0, 'errors': 0, 'no_terminator': 0, 'eh_frames': 0, 'eh_pads': 0, 'eh_catch_no_pad': 0,
          'added': 0, 'cpuid': 0, 'hooks': 0}
    chunk = []

    def flush(force=False):
        nonlocal chunk, idx
        if chunk and (force or len(chunk) >= CHUNK.get(m.name, 400)):
            # Numbered within the module, so a change in one module leaves
            # every other module's files as they were.
            write_chunk(out, idx, chunk)
            os.replace(os.path.join(out, 'recomp_%04d.c' % idx),
                       os.path.join(out, 'recomp_%s_%04d.c' % (m.name, idx - first)))
            idx += 1
            chunk = []

    def lift_one(addr, end, reached):
        name = 'sub_%08X' % addr
        if addr in hooks:
            body = 'extern void %s(void);\nvoid %s(void) { %s(); }\n' % (hooks[addr], name, hooks[addr])
            st['hooks'] += 1
        else:
            try:
                lo = min(reached) if reached else addr
                insns, leaders = (linear_disassemble_function(md, m.code, m.cs, lo, end, reached=reached)
                                  if end > addr else ([], None))
                if leaders is not None:
                    leaders.add(addr)
                head = [i for i in insns if i.address >= addr]
                eh = eh32.resume_points(md, m.read, head)
                if eh is not None:
                    st['eh_frames'] += 1
                    st['eh_pads'] += bool(eh)
                elif catches(m, head[:eh32.PROLOGUE_WINDOW]):
                    # Its FuncInfo has catch blocks, but eh32.py found no
                    # frame it can land in: a throw it catches stops the run.
                    st['eh_catch_no_pad'] += 1
                    log(f'[{m.name}] {name} catches, but has no landing pad')
                body = (lift_function_linear(lifter, name, insns, leaders, addr, eh_resume=eh) if insns
                        else 'void %s(void) { }\n' % name)
                body = apply_patches(body, {va: c for va, c in patches.items() if reached and va in reached})
                if CPUID_LIFTED in body:
                    st['cpuid'] += body.count(CPUID_LIFTED)
                    body = body.replace(CPUID_LIFTED, CPUID_HOST)
            except Exception as e:                  # noqa: BLE001 -- counted, not hidden
                body = '/* ERROR %s: %s */\nvoid %s(void) { RECOMP_NOT_LIFTED(0x%08Xu); }\n' % (name, e, name, addr)
                st['errors'] += 1
                log(f'[{m.name}] error in {name}: {e}')
        chunk.append((body, addr, name))
        entries.append((addr, name))
        st['lifted'] += 1
        flush()

    todo = sorted(byaddr)
    while todo:
        outside = set()
        for addr in todo:
            reached, behind, called = set(), set(), set()
            end, clean = true_extent(md, m.code, m.cs, addr, min(addr + EXTENT_REACH, m.ce), starts,
                                     reached=reached, behind=behind, called=called)
            outside |= {t for t in behind if t not in reached} | called
            st['no_terminator'] += not clean
            lift_one(addr, end, reached)
        todo = sorted(t for t in outside if m.cs <= t < m.ce and t not in byaddr)
        for t in todo:
            byaddr[t] = {'address': t, 'end': m.ce, 'calls_to': [], 'entry_kind': 'start'}
        st['added'] += len(todo)
    flush(force=True)
    return idx, st


def publish(new, out):
    """Make out hold what new does, replacing a file only if it differs."""
    os.makedirs(out, exist_ok=True)
    changed = 0
    have = set(os.listdir(new))
    for fn in os.listdir(out):
        if fn not in have:
            os.remove(os.path.join(out, fn))
            changed += 1
    for fn in sorted(have):
        src, dst = os.path.join(new, fn), os.path.join(out, fn)
        if os.path.exists(dst) and filecmp.cmp(src, dst, shallow=False):
            continue
        os.replace(src, dst)
        changed += 1
    shutil.rmtree(new)
    return changed


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--work', default=os.path.join(_HERE, 'work', 'analysis'),
                    help='the analysis (tools/gen/analyse.py): WORK/<module>/...')
    ap.add_argument('--out', default=os.path.join(_HERE, 'work', 'gen'))
    ap.add_argument('--modules', default=','.join(m[0] for m in MODULES),
                    help='comma-separated, of: %(default)s')
    ap.add_argument('--check', action='store_true',
                    help='exit 0 if the lift is current (this lifter, this driver), 1 if not')
    args = ap.parse_args()
    if args.check:
        try:
            sys.exit(0 if open(os.path.join(args.out, 'lift_stamp.txt')).read().strip() == lifter_stamp() else 1)
        except OSError:
            sys.exit(1)
    want = args.modules.split(',')
    mods = [Module(args.work, *m) for m in MODULES if m[0] in want]
    # Written to a fresh folder, then only the files whose bytes changed are
    # put in --out, so a build recompiles only them (a fix in one module
    # leaves the others' objects alone).
    final = args.out
    args.out = final.rstrip('/') + '.new'
    shutil.rmtree(args.out, ignore_errors=True)
    os.makedirs(args.out)
    t0 = time.time()
    entries, idx, stats = [], 0, {}

    def log(s):
        print(s, flush=True)
    for m in mods:
        t = time.time()
        idx, st = lift_module(m, args.out, idx, entries, log)
        st['seconds'] = round(time.time() - t)
        stats[m.name] = st
        log(f'[{m.name}] ' + ' '.join(f'{k}={v}' for k, v in st.items()))

    if len({a for a, _ in entries}) != len(entries):
        sys.exit('two modules lifted the same address: their bases overlap (tools/gen/modules.py)')
    with open(os.path.join(args.out, 'recomp_funcs.h'), 'w', newline='\n') as f:
        f.write('/* Star Trek: Legacy - AUTO-GENERATED by run_lift.py */\n#pragma once\n'
                '#include <stdint.h>\n\n'
                'void recomp_not_lifted(uint32_t va);\n'
                '#define RECOMP_NOT_LIFTED(va) recomp_not_lifted(va)\n\n')
        for a, n in entries:
            f.write('void %s(void);\n' % n)
    with open(os.path.join(args.out, 'recomp_dispatch.c'), 'w', newline='\n') as f:
        f.write('/* Star Trek: Legacy - AUTO-GENERATED by run_lift.py */\n'
                '#include "recomp_types.h"\n#include "recomp_funcs.h"\n\n'
                'const recomp_dispatch_entry_t recomp_dispatch_table[] = {\n')
        for a, n in sorted(entries):
            f.write('    { 0x%08Xu, %s },\n' % (a, n))
        f.write('};\nconst uint32_t recomp_dispatch_count = %d;\n' % len(entries))
    with open(os.path.join(args.out, 'recomp_target.h'), 'w', newline='\n') as f:
        f.write('/* Generated by run_lift.py: the modules this build runs. */\n#pragma once\n'
                '#define STL_TITLE "%s"\n' % TITLE)
        f.write('#define STL_MODULE_COUNT %d\n' % len(mods))
        f.write('/* file, base, entry, PE timestamp: Legacy.exe first */\n#define STL_MODULES { \\\n')
        for m in mods:
            f.write('    { "%s", 0x%08Xu, 0x%08Xu, 0x%08Xu }, \\\n' % (os.path.basename(m.path), m.base, m.entry, m.stamp))
        f.write('}\n')
    lines = sum(sum(1 for _ in open(os.path.join(args.out, fn), encoding='utf-8', errors='replace'))
                for fn in os.listdir(args.out) if fn.endswith(('.c', '.h')))
    stats['total'] = {'functions': len(entries), 'files': idx, 'lines': lines,
                      'seconds': round(time.time() - t0)}
    json.dump(stats, open(os.path.join(args.work, 'lift_stats.json'), 'w'), indent=1)
    open(os.path.join(args.out, 'lift_stamp.txt'), 'w').write(lifter_stamp() + '\n')
    changed = publish(args.out, final)
    print(f'[*] {changed} of {len(os.listdir(final))} files changed in {final}')
    print('=' * 60)
    for name, st in stats.items():
        if name != 'total':
            print(f'  {name:8} lifted {st["lifted"]:6}  errors {st["errors"]}  no terminator {st["no_terminator"]}'
                  f'  EH frames with catches {st["eh_pads"]} (no landing pad: {st["eh_catch_no_pad"]})')
    print('  %s lines of C in %d files, %ds' % (format(lines, ','), idx, time.time() - t0))
    print('=' * 60)


if __name__ == '__main__':
    main()
