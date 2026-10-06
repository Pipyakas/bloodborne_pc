#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Reverse-engineering helpers for out/eboot.elf (Bloodborne 1.09, no symbols).

Addresses are the patch-file ones (0x400000 + ELF vaddr). Function bounds come from the
.eh_frame_hdr search table; callers from a scan for rel32 calls/jumps.

    python tools/re/bbre.py func ADDR...      containing function, its callers
    python tools/re/bbre.py dis ADDR [N]      N instructions from ADDR
    python tools/re/bbre.py fn ADDR           whole function containing ADDR
    python tools/re/bbre.py refs ADDR         rel32 call/jmp and rip-relative references to ADDR
"""
import struct, sys, functools
import numpy as np
import capstone

BASE = 0x400000
ELF = 'out/eboot.elf'

@functools.cache
def image():
    data = open(ELF, 'rb').read()
    phoff, = struct.unpack_from('<Q', data, 0x20)
    phentsize, phnum = struct.unpack_from('<HH', data, 0x36)
    segs = []
    for i in range(phnum):
        kind, flags, off, vaddr, _, filesz, memsz, _ = struct.unpack_from('<IIQQQQQQ', data, phoff + i * phentsize)
        segs.append((kind, off, vaddr, filesz, memsz))
    return data, segs

def text():
    data, segs = image()
    kind, off, vaddr, filesz, memsz = segs[0]
    return data[off:off + filesz], vaddr

@functools.cache
def fde_table():
    data, segs = image()
    hdr = next(s for s in segs if s[0] == 0x6474e550)
    off, vaddr = hdr[1], hdr[2]
    version, eh_ptr_enc, count_enc, table_enc = data[off:off + 4]
    assert table_enc == 0x3b, hex(table_enc)  # datarel sdata4
    pos = off + 4
    def read(enc, pos):
        if enc == 0x1b:  # pcrel sdata4
            v, = struct.unpack_from('<i', data, pos); return vaddr + (pos - off) + v, pos + 4
        if enc == 0x03:
            v, = struct.unpack_from('<I', data, pos); return v, pos + 4
        raise ValueError(hex(enc))
    _, pos = read(eh_ptr_enc, pos)
    count, pos = read(count_enc, pos)
    starts = []
    fdes = []
    for i in range(count):
        loc, fde = struct.unpack_from('<ii', data, pos + i * 8)
        starts.append(vaddr + loc); fdes.append(vaddr + fde)
    # FDE: length, CIE pointer, pc_begin (pcrel sdata4 typically), pc_range
    ehseg = next(s for s in segs if s[0] == 1 and s[2] <= fdes[0] < s[2] + s[3])
    ranges = []
    for start, fde in zip(starts, fdes):
        fo = ehseg[1] + (fde - ehseg[2])
        rng, = struct.unpack_from('<I', data, fo + 12)
        ranges.append(rng)
    return np.array(starts, dtype=np.int64), np.array(ranges, dtype=np.int64)

def function_of(va):
    starts, ranges = fde_table()
    i = int(np.searchsorted(starts, va, side='right')) - 1
    if i >= 0 and starts[i] <= va < starts[i] + ranges[i]:
        return int(starts[i]), int(starts[i] + ranges[i])
    return None

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)

def disasm(va, count=None, end=None):
    code, base = text()
    out = []
    for ins in md.disasm(code[va - base:(end or va + 4096) - base], va):
        out.append(ins)
        if count and len(out) >= count: break
        if end and ins.address >= end: break
    return out

@functools.cache
def code_array():
    code, base = text()
    return np.frombuffer(code, dtype=np.uint8), base

def callers(target):
    arr, base = code_array()
    res = []
    for op in (0xE8, 0xE9):
        idx = np.nonzero(arr[:-5] == op)[0]
        rel = arr[idx + 1].astype(np.int64) | (arr[idx + 2].astype(np.int64) << 8) | (arr[idx + 3].astype(np.int64) << 16) | (arr[idx + 4].astype(np.int64) << 24)
        rel = np.where(rel >= 1 << 31, rel - (1 << 32), rel)
        hits = idx[(base + idx + 5 + rel) == target]
        res += [(int(base + h), 'call' if op == 0xE8 else 'jmp') for h in hits]
    return sorted(res)

def riprefs(target):
    """Instructions with a rip-relative disp32 equal to target (approximate: scan disp32)."""
    arr, base = code_array()
    b = arr.astype(np.int64)
    disp = b[:-4] | (b[1:-3] << 8) | (b[2:-2] << 16) | (b[3:-1] << 24)
    disp = np.where(disp >= 1 << 31, disp - (1 << 32), disp)
    pos = np.arange(len(disp))
    res = []
    for tail in (0, 1, 4):  # end of instruction after disp32 (+imm8/imm32)
        hits = pos[(base + pos + 4 + tail + disp) == target]
        res += [int(base + h) for h in hits]
    return sorted(set(res))

def fmt(ins):
    return f'  {ins.address + BASE:#010x}: {ins.mnemonic} {ins.op_str}'

def main():
    cmd, args = sys.argv[1], [int(a, 16) for a in sys.argv[2:3]] + [int(a, 0) for a in sys.argv[3:]]
    if cmd == 'func':
        for a in [int(x, 16) for x in sys.argv[2:]]:
            f = function_of(a - BASE)
            if not f:
                print(f'{a:#x}: no FDE'); continue
            cs = callers(f[0])
            print(f'{a:#x}: in {f[0] + BASE:#x}..{f[1] + BASE:#x} ({f[1] - f[0]} bytes), {len(cs)} callers: ' +
                  ' '.join(f'{c + BASE:#x}({k}) in {(function_of(c) or (0,))[0] + BASE:#x}' for c, k in cs[:8]))
    elif cmd == 'dis':
        n = args[1] if len(args) > 1 else 30
        for ins in disasm(args[0] - BASE, n): print(fmt(ins))
    elif cmd == 'fn':
        f = function_of(args[0] - BASE)
        for ins in disasm(f[0], end=f[1]): print(fmt(ins))
    elif cmd == 'refs':
        t = args[0] - BASE
        for c, k in callers(t): print(f'{k} at {c + BASE:#x} in {(function_of(c) or (0,))[0] + BASE:#x}')
        for r in riprefs(t): print(f'rip-ref near {r + BASE:#x} in {(function_of(r) or (0,))[0] + BASE:#x}')

if __name__ == '__main__':
    main()
