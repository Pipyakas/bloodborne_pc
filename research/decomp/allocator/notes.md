# Priority 5 — allocator / map-load crash research

Date: 2026-10-06. Read-only research; no game launched, no git commands, no bbport changes.
All addresses below are **image-relative virtual addresses**, not file offsets or live VAs.
Private binary-derived artifacts belong only in this directory.

## Outcome and confidence

**Identified with high confidence: Dantelion `DLFastFixedAllocator`, allocate entry
0x263b8b0, free entry 0x263b9b0, initialization 0x263baa0.** Initialization references
the original `Core/Kernel/Source/DLFastFixedAllocator.cpp` path at 0x48690c7.
Crash instruction at 0x263b8e7 is `mov rcx, qword ptr [r14]`.
The containing FDE covers [0x263b8b0, 0x263b9ad), size 0xfd.
This is not libc malloc or Havok's allocator.

Strong static structure recovery, **no identified remaining corruption writer**.
Several independent consumers reach this same allocator. No supplied fresh crash
stack identifies the failing instance, live heap address, or area-load heap ID.
Resource-loading allocator handles and call sites are mapped below, but assigning
them to concrete live heaps requires capture. Do not claim all map-loading heaps
or a global game realloc entry have been completely recovered.

## Evidence and reproducibility

Run with `C:\Python314\python.exe`; scripts use Capstone and the self-contained
ELF program-header reader in `analyze.py`. No renderer-helper dependency remains.

- `analyze.py` — corrected EH search table, targeted disassembly.
- `xref.py` — raw E8/E9 candidates, then instruction-boundary validation by
  Capstone from preceding EH entry; reports direct calls/tail calls, not virtual calls.
- `strings.py` — referenced printable strings in saved disassembly ranges and
  a binary-wide allocator-string inventory.
- `riprefs.py` — bounded common RIP-relative instruction candidate scan, then
  Capstone validation; deliberately not exhaustive.
- `extra.py` — binary SHA256, exact crash FDE/CIE bytes, memory/loading strings.
- `validate.py` — exact FDE ranges and direct-call ownership validation.
- `relocs.py` — exploratory addend search; found no candidate vtable relocations.
  It is not a validated ELF relocation parser and proves nothing about absent vtables.

Actual runs included `analyze.py 263b8e7`, neighborhood dump, default `xref.py`,
`xref.py 26a9b50 26a9db0 26aa070 26aa2e0 26aa440 26aa860 2ba1850 2ba18a0`,
`strings.py`, allocator-path and resource-path `riprefs.py` scans, `extra.py`,
`validate.py`, targeted regular-heap and dispatcher dumps, and relocation searches.
`binary_provenance.json` contains SHA256 and raw EH evidence.
`xref_fde_validation.json`: all 43 saved direct-call sites were inside their
claimed preceding FDE (zero outside). Counts can increase on reruns.

**Important parser correction:** this ELF's EH header is
`01 1b 03 3b 90 83 a9 ff 8f 7c 02 00`; count = **162959**, encoding
at byte 3 = data-relative signed 32-bit, pairs start at byte 12 and are relative
to EH-header VA 0x4f41510. The renderer helper instead treats byte 12 as an
encoding, starts pairs at byte 16, uses per-entry PC bases and counts padding
as entries. Its ~208952 inventory and boundaries are invalid for this image.
No shared helper was modified. Independent inventory agents should verify this.
Crash CIE has `zR` with FDE encoding 0x1b (PC-relative signed 32-bit).

`fn_*.asm` files extend to the next EH start and may include padding / no-FDE
leaf functions. Prefer `fde_*.asm` and `fde_ranges.json` for exact boundaries.
Some late-created dumps need another `validate.py` run to acquire exact files.
Names assigned here, except names in referenced strings, are descriptive guesses.

## Crash semantics — do not confuse a slab with an object

`src/probe.c:124-126` prints RIP minus `image`. Windows `describe()` at
166 likewise prints `guest+offset`. Thus 0x263b8e7 is image offset;
live RIP = image_base + 0x263b8e7. ELF vaddr zero maps to file offset 0x4000,
so fault instruction file offset = 0x263f8e7.

At the fault:

```
rbx = fixed allocator state
rax = slab trailer/header (not the freed user object)
r14 = [rax+0x10] = free-object head
0x263b8e3: r14 = [rax+0x10]
0x263b8e7: rcx = [r14]       // fault here
0x263b8ea: [rax+0x10] = rcx
0x263b8ee: --[rax+0x18]
return r14
```

With historical bad value 0x0000005300000000, the allocator is already attempting
to dereference an invalid **head**. A previously corrupted object's next qword
could have been copied into this head by the preceding successful allocation.
Alternatively a writer directly overwrote slab+0x10. The fault alone cannot
choose between those scenarios or establish a particular GPU fence writer.
If the slab is the active slab, `[rbx] == rax`; it can also have just been
removed from the non-full list in the same call.

The write-log dumper labels RAX as a block and dumps RAX-0x30..RAX+0x60.
Here that is a trailer neighborhood, not an arbitrary freed object's location.
Its register print includes R14, but the normal Windows crash report prints only
RAX..RSP, not R14. Enable BB_WRITE_LOG when reconstructing this failure.

## Recovered layouts and algorithms

See `allocator_map.md` for fields, signatures, call paths, and allocator layers.
No lock/atomic operation exists in the fixed allocator's allocate or free routines.
This establishes that callers must synchronize; it does **not** prove callers fail
to do so (some wrappers visibly hold a mutex).

## Dependency-ordered reconstruction plan

1. **Correct inventory + ABI first.** Validate EH parsing, record actual FDE ranges,
   retain no-FDE leaves separately, preserve image-offset/live-VA distinction.
   Resolve virtual allocator targets from live vtables or a proper relocation parser.
2. **Types and invariants.** Fixed state (0x60), slab trailer (0x20), intrusive
   object-next qword, sentinels. Stub parent allocator vtable calls in replay.
3. **Leaves.** Count-free bytes, initialize size/alignment arithmetic, constructor
   edge cases. Test chunk powers of two, stride alignment, count bounds and overflow.
4. **Fixed allocator.** Allocate, expand, free, finalize; differential synthetic
   sequences for partial/full slab lists, first/last allocation, last-free release,
   null/free and allocation failure. Compare writes and parent-call argument traces.
   Replay double-free and wrong-owner vectors as *invalid-input characterization*,
   not a promise the original allocator tolerates them.
5. **Consumers / ownership.** Size-class router, its sized-free partner and synchronized
   wrappers; 48-byte metadata-node allocator and its split/coalesce list helpers.
   Verify every state-mutating wrapper acquires the same lock and teardown excludes users.
6. **Regular heaps / address ownership.** DLRegularHeap boundary tags, segregated
   lists, allocated/previous-allocated bits, allocated-size query and validity checks.
   Then SprjFastBiHeap / DLSegregatedBiHeap / DLSegregatedRegularHeap routing.
7. **Resource path.** Trace manager heap handle +0x630, event queue heap +0x18,
   thread temporary heap and file-loader queue allocator fields. Capture actual
   heap IDs and sizes during the exact map-load route before naming allocations.
8. **Lifetime verification.** CPU free trace + GPU decode/write-back trace with
   same address/range timeline. Fix only the demonstrated writer/ownership violation.
9. **Equivalence gate.** External author/reviewer, captured vectors, failure and
   multithread schedules, then serialized minimized/silent area-load regression.
   No replacement is currently drafted or verified by this research.

## Twenty best initial targets (dependency order)

| # | Image offset | Target / reason |
|---|---|---|
| 1 | 0x263ba40 | fixed pool free-byte summation; pure structure traversal |
| 2 | 0x263baa0 | fixed pool initialize; authoritative geometry/layout |
| 3 | 0x263b810 | new slab/initial object; isolate parent-allocation ABI |
| 4 | 0x263b8b0 | fixed allocate; exact crash site |
| 5 | 0x263b9b0 | fixed free; ownership, last-free release, non-full/full lists |
| 6 | 0x263bc40 | fixed finalize; destroys partial/full/current slabs |
| 7 | 0x2ba1640 | DLFastSmallObjectAllocator initialize; class table geometry |
| 8 | 0x2ba1850 | small-object allocate router |
| 9 | 0x2ba18a0 | sized/aligned free router; wrong-size risk |
| 10 | 0x20856c0 | DLRegularHeap allocated-size query; boundary-tag leaf |
| 11 | 0x2085d60 | DLRegularHeap block validity predicate |
| 12 | 0x2085730 | DLRegularHeap ordinary allocation, splitting/bins |
| 13 | 0x2085a60 | DLRegularHeap aligned allocation (NOT realloc) |
| 14 | 0x20858a0 | DLRegularHeap free/coalesce |
| 15 | 0x207c4e0 | explicit-allocator allocate dispatcher, vtable+0x58 |
| 16 | 0x26a9b50 | 48-byte metadata pool initialize; alternate crash owner |
| 17 | 0x26a9db0 | low-end metadata split allocation core; lock wrappers first |
| 18 | 0x20c1310 | resource manager factory via manager+0x630 heap |
| 19 | 0x266a9e0 | file-load event queue allocation via queue+0x18 heap |
| 20 | 0x20d38c0 | FileLoader path: data-slot CAS, deferred tasks, allocation/free |

Immediately after these: 0x26aa070 (opposite-end split), 0x26aa2e0/0x26aa440
(metadata free/coalesce), 0x1fc0e60 (SprjMemory bootstrap),
0x20819f0/0x2081d40 (PS4 backing heap), 0x207d250 (thread-temp setup),
0x20ce2e0 (CacheManager thread-temp path), general realloc virtual implementation.

## Unresolved, explicitly

- Current intermittent crash rate and Steam Deck correlation are user reports,
  not measured here. Historical fixes/soaks are documentation evidence only.
- The allocator name, exact crash instruction and layouts are static high-confidence.
- No current live crash stack, corrupted object VA, allocator instance, prior free,
  resource-to-heap ID mapping, or writer was observed.
- Parent virtual targets, global realloc implementation and complete resource
  allocator hierarchy remain open. Do not label an aligned allocation as realloc.
- No runtime experiment or equivalence test was run. See `experiments.md`.
