#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Sampling CPU profiler for a running bb-probe.exe on Windows, without administrator rights.

Per-thread CPU time (GetThreadTimes) over the window, then call stacks of the busiest threads:
each sample suspends a thread, walks its stack with dbghelp's StackWalk64 (x64 unwind data) and
resumes it. Addresses in bb-probe.exe are symbolized with llvm-symbolizer (DWARF), other modules
with dbghelp (exports); guest code shows as "guest+offset".

    python tools/profile/winsample.py [--pid N] [--seconds 10] [--hz 400] [--threads 8]
"""
import argparse
import collections
import ctypes
import ctypes.wintypes as wt
import os
import shutil
import subprocess
import sys
import time

k32 = ctypes.WinDLL('kernel32', use_last_error=True)
psapi = ctypes.WinDLL('psapi', use_last_error=True)
dbghelp = ctypes.WinDLL('dbghelp', use_last_error=True)

TH32CS_SNAPTHREAD = 0x4
THREAD_ALL = 0x0002 | 0x0008 | 0x0010 | 0x0040 | 0x0800  # suspend, get context, query info
PROCESS_ALL = 0x0400 | 0x0010  # query information, vm read
CONTEXT_FULL = 0x10000B


class THREADENTRY32(ctypes.Structure):
    _fields_ = [('dwSize', wt.DWORD), ('cntUsage', wt.DWORD), ('th32ThreadID', wt.DWORD),
                ('th32OwnerProcessID', wt.DWORD), ('tpBasePri', wt.LONG),
                ('tpDeltaPri', wt.LONG), ('dwFlags', wt.DWORD)]


class CONTEXT(ctypes.Structure):  # x64, 16-byte aligned, 1232 bytes
    _pack_ = 16
    _fields_ = [('P1Home', ctypes.c_uint64 * 6), ('ContextFlags', wt.DWORD), ('MxCsr', wt.DWORD),
                ('Seg', wt.WORD * 6), ('EFlags', wt.DWORD), ('Dr', ctypes.c_uint64 * 6),
                ('Rax', ctypes.c_uint64), ('Rcx', ctypes.c_uint64), ('Rdx', ctypes.c_uint64),
                ('Rbx', ctypes.c_uint64), ('Rsp', ctypes.c_uint64), ('Rbp', ctypes.c_uint64),
                ('Rsi', ctypes.c_uint64), ('Rdi', ctypes.c_uint64), ('R', ctypes.c_uint64 * 8),
                ('Rip', ctypes.c_uint64), ('Rest', ctypes.c_byte * (1232 - 256))]


class ADDRESS64(ctypes.Structure):
    _fields_ = [('Offset', ctypes.c_uint64), ('Segment', wt.WORD), ('Mode', wt.DWORD)]


class STACKFRAME64(ctypes.Structure):
    _fields_ = [('AddrPC', ADDRESS64), ('AddrReturn', ADDRESS64), ('AddrFrame', ADDRESS64),
                ('AddrStack', ADDRESS64), ('AddrBStore', ADDRESS64),
                ('FuncTableEntry', ctypes.c_void_p), ('Params', ctypes.c_uint64 * 4),
                ('Far', wt.BOOL), ('Virtual', wt.BOOL), ('Reserved', ctypes.c_uint64 * 3),
                ('KdHelp', ctypes.c_byte * 128)]


class SYMBOL_INFO(ctypes.Structure):
    _fields_ = [('SizeOfStruct', wt.ULONG), ('TypeIndex', wt.ULONG), ('Reserved', ctypes.c_uint64 * 2),
                ('Index', wt.ULONG), ('Size', wt.ULONG), ('ModBase', ctypes.c_uint64),
                ('Flags', wt.ULONG), ('Value', ctypes.c_uint64), ('Address', ctypes.c_uint64),
                ('Register', wt.ULONG), ('Scope', wt.ULONG), ('Tag', wt.ULONG),
                ('NameLen', wt.ULONG), ('MaxNameLen', wt.ULONG), ('Name', ctypes.c_char * 256)]


class MODULEINFO(ctypes.Structure):
    _fields_ = [('lpBaseOfDll', ctypes.c_void_p), ('SizeOfImage', wt.DWORD), ('EntryPoint', ctypes.c_void_p)]


k32.OpenThread.restype = wt.HANDLE
k32.OpenProcess.restype = wt.HANDLE
k32.CreateToolhelp32Snapshot.restype = wt.HANDLE
k32.GetThreadDescription.argtypes = [wt.HANDLE, ctypes.POINTER(ctypes.c_wchar_p)]
dbghelp.SymFunctionTableAccess64.restype = ctypes.c_void_p
dbghelp.SymGetModuleBase64.restype = ctypes.c_uint64
dbghelp.SymFunctionTableAccess64.argtypes = [wt.HANDLE, ctypes.c_uint64]
dbghelp.SymGetModuleBase64.argtypes = [wt.HANDLE, ctypes.c_uint64]
dbghelp.StackWalk64.argtypes = [wt.DWORD, wt.HANDLE, wt.HANDLE, ctypes.POINTER(STACKFRAME64),
                                ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p,
                                ctypes.c_void_p]
psapi.EnumProcessModulesEx.argtypes = [wt.HANDLE, ctypes.c_void_p, wt.DWORD, ctypes.POINTER(wt.DWORD), wt.DWORD]
psapi.GetModuleInformation.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, wt.DWORD]
psapi.GetModuleFileNameExW.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_wchar_p, wt.DWORD]
dbghelp.SymFromAddr.argtypes = [wt.HANDLE, ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint64),
                                ctypes.POINTER(SYMBOL_INFO)]


def find_pid():
    out = subprocess.run(['tasklist', '/FI', 'IMAGENAME eq bb-probe.exe', '/FO', 'CSV', '/NH'],
                         capture_output=True, text=True).stdout
    pids = [int(line.split('","')[1]) for line in out.splitlines() if 'bb-probe' in line]
    if len(pids) != 1:
        sys.exit(f'need exactly one bb-probe.exe (found {pids}); pass --pid')
    return pids[0]


def threads_of(pid):
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
    entry = THREADENTRY32(dwSize=ctypes.sizeof(THREADENTRY32))
    tids = []
    ok = k32.Thread32First(snap, ctypes.byref(entry))
    while ok:
        if entry.th32OwnerProcessID == pid:
            tids.append(entry.th32ThreadID)
        ok = k32.Thread32Next(snap, ctypes.byref(entry))
    k32.CloseHandle(snap)
    return tids


def thread_name(h):
    p = ctypes.c_wchar_p()
    if k32.GetThreadDescription(h, ctypes.byref(p)) >= 0 and p.value:
        name = p.value
        k32.LocalFree(p)
        return name
    return ''


def cpu_time(h):
    c, e, kt, ut = (wt.FILETIME() for _ in range(4))
    k32.GetThreadTimes(h, ctypes.byref(c), ctypes.byref(e), ctypes.byref(kt), ctypes.byref(ut))
    f = lambda t: (t.dwHighDateTime << 32 | t.dwLowDateTime) / 1e7
    return f(kt) + f(ut)


def modules(process):
    arr = (ctypes.c_void_p * 1024)()
    needed = wt.DWORD()
    psapi.EnumProcessModulesEx(process, arr, ctypes.sizeof(arr), ctypes.byref(needed), 3)
    mods = []
    for i in range(needed.value // ctypes.sizeof(ctypes.c_void_p)):
        info = MODULEINFO()
        psapi.GetModuleInformation(process, arr[i], ctypes.byref(info), ctypes.sizeof(info))
        name = ctypes.create_unicode_buffer(260)
        psapi.GetModuleFileNameExW(process, arr[i], name, 260)
        mods.append((info.lpBaseOfDll or 0, info.SizeOfImage, name.value))
    return sorted(mods)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--pid', type=int)
    ap.add_argument('--seconds', type=float, default=10)
    ap.add_argument('--hz', type=float, default=400, help='samples per second per thread')
    ap.add_argument('--threads', type=int, default=8, help='busiest threads to sample')
    ap.add_argument('--depth', type=int, default=48)
    ap.add_argument('--top', type=int, default=25)
    ap.add_argument('--exe', default=os.path.join(os.path.dirname(__file__), '..', '..', 'out', 'bb-probe.exe'))
    args = ap.parse_args()
    pid = args.pid or find_pid()
    process = k32.OpenProcess(PROCESS_ALL, False, pid)
    dbghelp.SymSetOptions(0x2 | 0x4)  # undecorated names, deferred loads
    dbghelp.SymInitializeW(process, None, True)

    # 1. CPU per thread over one second.
    handles = {}
    for tid in threads_of(pid):
        h = k32.OpenThread(THREAD_ALL, False, tid)
        if h:
            handles[tid] = h
    t0 = {tid: cpu_time(h) for tid, h in handles.items()}
    wall0 = time.perf_counter()
    time.sleep(1.0)
    wall = time.perf_counter() - wall0
    usage = {tid: (cpu_time(h) - t0[tid]) / wall for tid, h in handles.items()}
    names = {tid: thread_name(h) for tid, h in handles.items()}
    busiest = sorted(usage, key=usage.get, reverse=True)
    print(f'CPU per thread (pid {pid}, total {sum(usage.values()) * 100:.0f}% of one core):')
    for tid in busiest[:24]:
        if usage[tid] >= 0.02:
            print(f'  {usage[tid] * 100:5.1f}%  {names[tid] or "?"} ({tid})')

    # 2. Stacks of the busiest threads.
    sampled = [tid for tid in busiest[:args.threads] if usage[tid] >= 0.05]
    stacks = {tid: collections.Counter() for tid in sampled}
    ctx = CONTEXT()
    table_access = ctypes.cast(dbghelp.SymFunctionTableAccess64, ctypes.c_void_p)
    module_base = ctypes.cast(dbghelp.SymGetModuleBase64, ctypes.c_void_p)
    period = 1.0 / args.hz
    end = time.perf_counter() + args.seconds
    count = 0
    while time.perf_counter() < end:
        start = time.perf_counter()
        for tid in sampled:
            h = handles[tid]
            if k32.SuspendThread(h) == 0xFFFFFFFF:
                continue
            ctx.ContextFlags = CONTEXT_FULL
            frames = []
            if k32.GetThreadContext(h, ctypes.byref(ctx)):
                frame = STACKFRAME64()
                frame.AddrPC.Offset, frame.AddrPC.Mode = ctx.Rip, 3
                frame.AddrFrame.Offset, frame.AddrFrame.Mode = ctx.Rbp, 3
                frame.AddrStack.Offset, frame.AddrStack.Mode = ctx.Rsp, 3
                for _ in range(args.depth):
                    if not dbghelp.StackWalk64(0x8664, process, h, ctypes.byref(frame), ctypes.byref(ctx),
                                               None, table_access, module_base, None):
                        break
                    if not frame.AddrPC.Offset:
                        break
                    frames.append(frame.AddrPC.Offset)
            k32.ResumeThread(h)
            if frames:
                stacks[tid][tuple(frames)] += 1
        count += 1
        time.sleep(max(0.0, period - (time.perf_counter() - start)))

    # 3. Symbolize.
    mods = modules(process)
    exe_base = next((b for b, s, n in mods if n.lower().endswith('bb-probe.exe')), None)
    addresses = {a for c in stacks.values() for st in c for a in st}
    main_addrs = sorted(a for a in addresses if exe_base and exe_base <= a < exe_base + 0x10000000)
    symbol = {}
    symbolizer = shutil.which('llvm-symbolizer') or r'C:\msys64\clang64\bin\llvm-symbolizer.exe'
    if main_addrs and os.path.exists(symbolizer):
        text = '\n'.join(hex(a - exe_base + 0x140000000) for a in main_addrs)
        out = subprocess.run([symbolizer, '--obj', os.path.abspath(args.exe), '--functions=short',
                              '--demangle', '--no-inlines'],
                             input=text, capture_output=True, text=True).stdout.split('\n\n')
        for a, block in zip(main_addrs, out):
            lines = block.strip().splitlines()
            symbol[a] = lines[0] if lines and lines[0] != '??' else f'bb-probe+{a - exe_base:#x}'
    info = SYMBOL_INFO(SizeOfStruct=ctypes.sizeof(SYMBOL_INFO) - 256 + 1, MaxNameLen=255)
    disp = ctypes.c_uint64()
    for a in addresses:
        if a in symbol:
            continue
        mod = next((n for b, s, n in mods if b <= a < b + s), None)
        if mod and dbghelp.SymFromAddr(process, a, ctypes.byref(disp), ctypes.byref(info)):
            symbol[a] = f'{os.path.basename(mod)}!{info.Name.decode(errors="replace")}'
        elif mod:
            symbol[a] = os.path.basename(mod)
        else:
            symbol[a] = 'guest'

    for tid in sampled:
        c = stacks[tid]
        total = sum(c.values())
        if not total:
            continue
        self_count, incl = collections.Counter(), collections.Counter()
        for st, n in c.items():
            self_count[symbol[st[0]]] += n
            for name in set(symbol[a] for a in st):
                incl[name] += n
        print(f'\n== {names[tid] or "?"} ({tid}): {usage[tid] * 100:.0f}% CPU, {total} samples')
        print('  self:')
        for name, n in self_count.most_common(args.top):
            print(f'    {n * 100 / total:5.1f}%  {name[:150]}')
        print('  inclusive:')
        for name, n in incl.most_common(args.top):
            print(f'    {n * 100 / total:5.1f}%  {name[:150]}')
    print(f'\n{count} sampling rounds over {args.seconds:.0f} s')


if __name__ == '__main__':
    main()
