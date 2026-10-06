# Frame timing source map (Bloodborne CUSA03173 v1.09)

Everything below is static analysis of `C:\code\bloodborne_pc\out\eboot.elf`
(sha256 `cec1b276…`, verified by `finish_map.py`). No game was launched. Every
count is reproduced by a script in this folder; the script is named next to the
claim. Run order:

```
verified_map.py            # FDE function table (162959 entries) + verified_gaps.json via --scan
gap_scan.py                # the same index for code outside every FDE
finish_map.py              # data_sites_readers.json (74 data sites -> their readers)
classify_sites.py          # site_classes.json  : role of all 364 sites
site_mechanisms.py         # site_mechanisms.json : how the preset edits each site
rip_redirects.py           # rip_redirects.json  : where each rip-relative edit lands
hook_word.py               # the 0x535a908 live-timestep word
flipper_math.py            # SprjFlipper sleep/skip arithmetic, evaluated
build_targets.py           # targets.csv, pilot_targets.csv (+ pilot_NN_*.asm)
crosstab.py                # the two classifications side by side
validate_outputs.py        # PASS/FAIL gate, writes validation.txt
```

## 1. Headline

| question | answer |
|---|---|
| frame-time producer | `0x2034770` **SprjFlipper::Update** — 2124 bytes, single caller `0x2018d20`, delta written to object+`0x264` at `0x2034cf0` |
| how the delta reaches consumers | three separate paths: (a) the Flipper object's own fields, (b) a global time struct at `0x54b7e00` whose `+8` float is the per-frame delta seconds, published by the 18-byte leaf `0x103f9f0`, (c) task phase timestamps built in `0x103d3b0` |
| 364 patch sites | 74 timestep consumers, 186 pacing, 90 constants, 2 vtable redirects, 12 other (see §5) |
| code outside the FDE table | 84 sites, all explained (§6) |
| 120 FPS ceiling | **not in this game code** (§7.1) |
| Havok at 90 | fixed 1/90 substep and an early-out substep counter, not a hard limit (§7.2) |

## 2. The frame-time producer: SprjFlipper

`0x2034770` is the only function in the image that (a) calls `gettimeofday`
(`0x2bbe728`, named by `plt_symbols.py`) three times per frame at
`0x2034977`, `0x2034b03`, `0x2034be5`, (b) computes a float delta in seconds and
(c) stores it. Its only direct caller is `0x2018d20`, one iteration of the frame.
Confidence: **high** — single-caller property, exact store site, no other
`gettimeofday` result is divided by 1e6 and stored into an object field.

The load-bearing store:

```
02034ca2 mov  rax, qword ptr [r12 + 0x28]      ; previous timestamp (us)
02034ca7 mov  rsi, qword ptr [r12 + 0x20]      ; frame start (us)
02034cac mov  rcx, rax
02034caf sub  rcx, rsi                         ; elapsed since frame start (us)
02034ce8 vdivss xmm0, xmm0, [rip + 0x28f469c]  ; / 1e6   (0x492938c)
02034cf0 vmovss [r12 + 0x264], xmm0            ; <-- delta seconds, +0x264
```

Field map of the Flipper object (0x2c8 bytes, allocated by
`SprjWindow->vtable+0x58` in `0x2018d20`; constructor `0x2034520`):

| offset | meaning | evidence |
|---|---|---|
| `+0x08`, `+0x0c` | frame mode read from two config keys (`Game.FlipMode`, `Game.EnablePSkip` UTF-16 strings, `0x2034557`/`0x20345ab`) | constructor |
| `+0x10` | steps to simulate this frame (0, 1 or 2) | set in the five mode arms `0x203480f`…`0x2034883`, read by leaf `0x2035200` |
| `+0x14` | pacing flag | read by leaf `0x2035210` |
| `+0x18` | **frame-time target in seconds (1/30 authored)** | constructor writes `0x3d088889` at `0x2034608`; 20 patch sites |
| `+0x20`, `+0x28` | last frame timestamp / this frame's timestamp, microseconds | update writes both |
| `+0x260` | 32-entry ring index for the frame-time histogram | `0x2034c72` |
| `+0x264` | **delta seconds for this frame** | `0x2034cf0` |
| `+0x268`, `+0x26c` | foreground / background frames to skip (`m_foregroundFrameSkipNumMax`, `m_backgroundFrameSkipNumMax`) | console variables registered in `0x2035630` |
| `+0x270` | "this frame was dropped" | leaf `0x2035220`, set `0x2034d8e` |
| `+0x271` | reset request | leaf `0x2035240`, honoured `0x203491a` |
| `+0x272` | set every frame by the main loop (`0x2018dee`) | leaf `0x2035250` |
| `+0x273` | skip in progress; while set the loop never sleeps, only re-reads the clock | leaves `0x2035260`/`0x2035270`, branch at `0x2034ad7` |
| `+0x274` | heavy-game gate (sets `+0x4c`/`+0x4d` on the graphics singleton, `0x2034f6f`) | leaf `0x2035280` |
| `+0x276` | mode-change request | leaf `0x2035290` |
| `+0x278`…`+0x2b4` | 16-float sliding window, shifted every frame (`0x2034dae`…`0x2034f02`) | update |
| `+0x2b8` | measured milliseconds per frame (`min(16, 1/sum)`, 0 when the sum is under 1 ms) | `0x2034f28`, read by the log at `0x203557a` |
| `+0x2bc`, `+0x2c0` | skip budgets, clamped to ≥0 at `0x20348f0`/`0x2034907` | console variables |
| `+0x2c4` | reset flag | leaf `0x20352a0`, honoured `0x2034899` |
| `+0x2c5` | when set the update samples the live clock instead of using `+0x28` | `0x2034770` reads it through the singleton at `0x1d973f1` |

The mode dispatch is a 5-entry int32 jump table at `0x2034fa8` (loaded at
`0x20347ff`, `cmp edx,4 / ja`). Every preset rewrites all five entries to
`0x2034863`-relative offsets that land on the *same* target, which is the single
reason one patch set can drive five frame modes.

## 3. How the delta reaches consumers

Three paths, all read out of the image:

**(a) Flipper object fields.** `+0x264` is read by the same function's
histogram and by nothing else found in the reference index. Consumers outside
the Flipper read it through the ten accessor leaves `0x2035200`…`0x20352a0`
(virtual/leaf accessors) — those are the leaves the game uses.

**(b) Global time struct at `0x54b7e00`.** This is the path that matters:

```
0103f9e0 lea  rax, [rip + 0x4478419]   ; 0x54b7e00
0103f9e7 ret                           ; accessor
0103f9f0 vmovss xmm0, dword ptr [rdi + 8]      ; <-- the delta, arg+8
0103f9f5 lea  rax, [rip + 0x4478404]   ; 0x54b7e00
0103f9fc vmovss dword ptr [rax + 8], xmm0      ; publish
0103fa01 ret
```

`0x103f9f0` has exactly one caller, `0xf88c70` at `0xf88cc5`, which is the task
step reached from the main loop through `0x20512a0` → `0xf88c60` → `0xf88c70`.
The struct is `{ vtable, float delta_seconds, … }`; `0x103fac0` is its static
initialiser (copies three 16-byte vectors from `0x48cba80`/`0x48cba90`/`0x48cbaa0`
into `0x54b7e20`/`0x54b7e30`/`0x54b7e40`, zeroes the delta, points the vtable at
`0x52efd40`). **Confidence: high** for the accessor and the publish.

`0x103fac0` has no direct caller and no rip-relative reference, but it is reached
indirectly: an 8-byte pointer to it sits at file offset `0x573d6d0`, inside a
regular 24-byte-stride `(vtable, guard?, ctor)` triple table whose neighbours are
`0x103f940`, `0x103fa30`, `0x103fa60`, `0x103fa90`, `0x10405b0`, `0x10405e0` —
the classic "static-initializer thunk" array (`__init_term` style). `0x1dbbd00`
and `0x1066e60` sit in the same kind of table (`0x57f6248` and `0x57445e8`).
Reproduce with `find_pointers.py`. So "static initialiser" for `0x103fac0` is
**high** confidence (table membership plus the shape of the function); the
*ordering* of the table's execution is not established.

Two readers of `[0x54b7e08]`, both immediately after calling `0x103f9e0`:
`0x103d3b0` at `0x103d405` (builds a 20-byte task-context struct
`{ vtable, delta_seconds, mode, phase }` and hands it to a virtual `+0x10`
callback, then increments a per-phase counter at `+0x44 + 0x14*phase`), and
`0x1066e60` at `0x1066e9c` (same idea, second call site, no direct caller —
virtual). **Confidence: high** for the loads, **medium** for the field semantics
(phase index and mode come from arguments, names are inferred).

**(c) Function arguments.** `SprjWorldAiManager` (string at `0x4935687`, vtable
written by `0x1dbbd00`'s caller) receives the delta as its second argument:
`0x1dbbd40 vmovss xmm0,[rbx+8]` → `0x1dbbd45 vmovss [r13+0x280],xmm0`, and the
AI tick count is converted with the patched constant `1/30` at `0x4928e98`:
`0x1dbc308 vmulss xmm0, xmm0, [0x4928e98]` then `0x1dbc310 vdivss xmm0, xmm0,
[rbx+8]`. **Confidence: medium** — the string names the class, the arithmetic is
exact, but the function has no direct caller so its role in the frame is inferred
from the signature and the 18 patch sites the presets put in it.

**(d) Task-manager enqueue.** Both `0x2018d20` (`0x2018e36`) and `0x2018f00`
(`0x2018f32`) materialise `{ 0x52efd40+0x10, 1/30 }` and pass it to `0x20512a0`.
`0x20512a0` resolves the `FD4::FD4TaskManager` singleton at `0x54b2e30` and
tail-calls `0xf88c60`. **Confidence: high** — the string is in the same function.

## 4. The frame loop, top to bottom

```
01e4a6c0  main()
  call 0x2017770                       one-time engine init
  loop: call 0x2018d20  until it returns 0     <-- the frame
        call 0x2018f00  up to 101 times, sceKernelUsleep(0x14d) between attempts
```

* `0x2018d20` (480 bytes) constructs SprjFlipper via the SprjWindow allocator,
  sets `+0x272 = 1`, calls `0x2034770` (SprjFlipper::Update), then hands 1/30 to
  the task manager and returns the SprjTask singleton's `->+0x98()` result.
* `0x2018f00` (261 bytes) hands 1/30 to the task manager and returns false until
  the singleton at `0x569fa30` says the queue is empty; the outer loop then sleeps
  0x14d µs and retries, up to 101 times (`ebx = 0x65`). **This polling loop is
  where a fixed per-frame CPU cost turns into a frame-rate floor** (§7.1).

## 5. Classification of the 364 patch sites

Two independent views, both from `presets.json` (which `validate_outputs.py`
checks byte-for-byte against `patches/Bloodborne.xml` for all five v1.09
presets). By role (`site_classes.json`, `classify_sites.py`):

| role | sites | what it means |
|---|---:|---|
| timestep consumer | 74 | the patch rewrites a **1/30 (56 sites)** or **30.0 (18 sites)** literal into a field. 56 write 1/60 for `60FPS (no deltatime)`/`60 FPS++` and 1/90 for `90 FPS++`/`Uncap FPS++`; the 18 write 60.0. Largest cluster: `0x1ac3ef0` (11 sites). Spread over 59 functions. |
| pacing | 186 | frame-skip counters, mode dispatch, frame-time target, jump-table edits, profiling stubs, argument marshalling, the world/gameplay tuning table, the heavy-game gate. By owner: `0x2034770` 30, `0x143a0e0` 28, `0x1f77bd0` 23, `0x1dbbd00` 18, `0x143fb60` 15, `0x17f99d0` 15, `0xfd2db0` 8, `0x2083e80` 5, `0x2800e30` 5, the seven Havok cloth operators 3 each, `0x2018d20` 3, remainder 1–2 each. |
| constant | 90 | 75 are **data** constants in `.rodata`/`.data` (41 of them hold 1/30, 10 hold 30.0); 15 are **code** sites that retarget the `disp32` of a `vmovss xmm,[rip+D]`. |
| vtable redirect | 2 | the five `int32` jump-table entries at `0x2034fa8`…`0x2034fb8` (0x2034fa8 is counted under pacing, the other four under other) and the `_Assert` PLT thunk at `0x2bbf178`, rewritten to `xor eax,eax; ret` by Uncap FPS++. |
| other | 12 | the four jump-table entries `0x2034fac`…`0x2034fb8` that no linear decode reaches, and the eight writes into the blank data region `0x36bc000`–`0x36bd000`. |

By mechanism (`site_mechanisms.py`) the same 364 sites split as A literal rewrite
189, B rip-relative redirect 15, C injected stub ≥5 bytes 152, D blank-region data
fill 8. `crosstab.py` prints the cross-tab and fails if the two files disagree
about the site set.

`0x63fad0` is the one "leaf constructor" in the timestep group the previous run
flagged as likely middleware, and the flag was right to be raised but the class
is wrong: it has **five** direct callers (`0x6976a0`, `0x697970`, `0xff4910`,
`0x10588c0`, `0x112faa0`), its vtable (`0x52576d0+0x10`) is filled by a
relocation whose addend is `0x252c420` — a `jmp 0x252c430` thunk sitting in the
game's own code range, not in the `0x60…0x80` Havok block — and the destructor at
`0x252c430` is an ordinary `lock cmpxchg` refcount release. Only one caller
(`0x112faa0`) mentions a Havok type (`hkpWorldCinfo`), so this is a
**Havok-adjacent game-side object** rather than middleware internals. Reproduce
with `who_builds.py`. Keep it in the pilot candidate pool but decompile it last.

**The one hook that makes the whole scheme work** (`hook_word.py`): 52 of the 58
preset/rip-redirect pairs retarget a read of a 1/30 literal onto
**`0x535a908`** — four zero bytes in writable `.data`, 0x18 bytes below the
SprjFlipper mode-name table at `0x535a920` (whose five entries are filled by
`R_X86_64_RELATIVE` relocations with addends pointing at the UTF-16 strings
`30FPS`, `30FPS_withoutSkip`, `60FPS_withoutSkip`, `30FPS_withoutTearing`,
`30FPS_withoutSkipTearing`). No relocation ever writes `0x535a908`, so it is a
scratch word: the presets point every consumer at it and let the runtime fill it
with the active timestep. The three remaining redirects are ordinary constants
(`0x1515faf` → `0x4929170`, which holds 1/30 then 1.0; `0x76fbd4` →
`0x46d78dc` / `0x4726048`, Havok tables).

## 6. The 84 sites outside the FDE ranges — all accounted for

`verified_map.py` reports "Outside FDE 84". None of them is unknown code:

| count | what | how it is handled |
|---:|---|---|
| 74 | constants in `.rodata`/`.data` read by rip-relative operands | listed with their readers in `data_sites_readers.json`; 59 have at least one reader in the image, 15 have none (e.g. `0x43df584`, `0x446aab4`, `0x493bd00`) and are still patched because the preset table lists them |
| 8 | writes into the blank data region `0x36bc000`–`0x36bd000` (`0x36bc208`, `0x36bc42d`, `0x36bc459`, `0x36bc67e`, `0x36bc6aa`, `0x36bc8cf`, `0x36bc8fb`, `0x36bcb20`) | padding between data objects; the preset just zeroes or fills them |
| 1 | `0x2bbf178` — a 16-byte import thunk (`jmp [rip+GOT]` = `_Assert`, plus `push 0xcc; jmp __stack_chk`) | boundary taken from the 16-byte PLT slot size; listed in `targets.csv` as `no FDE; 16-byte import thunk` |
| 1 | `0x1fdede6` — the immediate of `mov dword ptr [rip+…], 0x41f00000` in a 27-byte initializer at `0x1fdedd0` | boundary inferred from the surrounding `nop` padding and the `ret` at `0x1fdedea`; it is pilot #22. It writes the global `0x569e540` = 30.0, which three functions (`0x1fdd530`, `0x1fde730`, `0x1fde870`) copy into field `+0x188` of a 0x1fc-byte timer object, so this is the authored *period* a timer is constructed with, not the frame delta |

Note the earlier concern about the reader map being incomplete was a **false
alarm**: the 15 reader-less data constants are genuinely unreferenced in the
image, and the two code sites outside the FDEs are handled as above.
`gap_scan.py` also decodes every inter-FDE gap (4,148,321 bytes) and
`verified_gaps.json` records 160,717 functions that decode over their full
`pc_range` and 2,242 that do not.

## 7. What limits movement above ~120 FPS and Havok above 90

### 7.1 The ~120 FPS ceiling is not in this game code — confidence: medium-high

Checked by scanning every 4-byte float in `.rodata` for the frame-rate family and
then looking each hit up in the whole-image reference index
(`literal_survey.py`, `literal_readers.py`):

* **1/120 (`0x3d75c28f`) appears 6 times in `.rodata` and has zero
  rip-relative readers anywhere in the image** — none in the Flipper, the main
  loop, the task manager or the physics front end. The only two `120.0f`
  literals in the image are read by `0x15568c0` (collision range-read helper;
  format strings `開始範囲[m]` / `終了範囲[m]` / `開始遅延時間[sec]`,
  UTF-16 at `0x4968762`) and `0x1566290` (the debug-menu builder; 17
  `IsDisable…` area labels plus `【%0.3f[sec]】<点滅間隔`, "blink interval").
  Neither is frame pacing.
* The main loop's retry loop caps at 101 `sceKernelUsleep(0x14d)` attempts
  (`0x1e4a6c0`: `mov ebx,0x65` … `dec ebx; cmp ebx,1; jg`), so the wait is
  bounded, not a limiter.
* `0x2035480` formats `%c:%0.1f[ms](%02.1fFPS)` and `0x2035c20` formats
  `%02.1fFPS` from a 32-bin microsecond histogram — display only.

So on this code the achievable rate is set by how fast the frame body runs, not
by a 120 FPS cap. A ~120 FPS wall in the harness therefore comes from outside
the game code: present interval (vblank at 60 Hz would cap at 120 with a
2-frame swap chain), the frame pacing in `src/`/the runner, or the fact that
`SprjFlipper::Update` will happily run the simulation step twice
(`+0x10 == 2` in mode 0) and every extra step costs CPU. **This is the one claim
here that is not proven by a single instruction**; the way to prove it is to run
with the profiler stub disabled and compare frame-time histograms, which needs a
game run and is out of scope for this task.

### 7.2 Havok is fixed at a 1/90 substep and an early-out counter — confidence: high

`0x76e370` (`TtSimulate` / `TtSubstep Collidables`, 3276 bytes) computes its
substep as

```
0076e3dd vmovss xmm0, [rbp - 0x78]              ; delta seconds (argument)
0076e3e2 vdivss xmm0, xmm0, [r15 + 0x1d0]       ; / world->fixedTimestep
```

and `0x76fb90` (`TtUpdate Effective Damping`, `TtUpdate Particles Time Step`,
1717 bytes) selects a **different constant when the world is in a special mode**:

```
0076fbc6 cmp  dword ptr [r14 + 0x1cc], 1
0076fbce jne  0x76fbda
0076fbd0 vmovss xmm0, [rip + 0x4043570]          ; 1.0        <- site 0x76fbd4
0076fbd8 jmp  0x76fbe3
0076fbda vmovss xmm0, [r14 + 0x1d0]              ; world->fixedTimestep
```

That is exactly the `0x76fbd4` patch site: for 60/90/Uncap the presets redirect
it to `0x46d78dc` (1.0) / `0x4726048` (0.666667), for 30 FPS++ they leave 1.0.
So the "90 FPS" cap is not the substep size but the **substep budget**: the
integer substep count comes from `0x748c50` (`+0x18`, falling back to
`[r13+0x20]`), and at 90 FPS++ the presets rewrite

```
0076e67a mov  ebx, dword ptr [rax + 0x18]     ; -> mov ebx, 1 ; 8 nop bytes
0076e67d test ebx, ebx
```

i.e. **at most one substep per frame**. Same shape at `0x773f84` (`TtActions` /
`TtIntegrate`), where the 90 FPS++ and Uncap presets replace the
`vmulss xmm0,xmm0,xmm0` plus spill with a 9-byte `jmp 0x773faa` — which lands on
that function's own tracer `lea`, so the multiply is skipped and the tracer path
stays consistent with the other six operators. Conclusion: above 90 FPS the cloth
solver integrates one fixed-size substep per frame and simply runs out of
simulation budget, so cloth motion visibly slows rather than the game refusing
to run faster. The seven cloth-operator profiler stubs (§5) only replace the
`lea+mov+rdtsc` trace push with a leaf marker; they do not change integration.

## 8. Files produced by this task

| file | content |
|---|---|
| `notes.md` | this document |
| `targets.csv` | 219 rows: 100 functions that own a patch site, 119 that read patched data, and 19 on the frame-time path (a few rows are in more than one group). Columns: start, PS4 VA, size, boundary, leaf, indirect calls, direct sites, data sites read, site roles, callees, callers, role, evidence, strings, confidence |
| `pilot_targets.csv` | 22 rows (21 strict call-free leaves + one 251-byte function with two calls), each with its `.asm` dump and an explicit patch relation |
| `pilot_01..22_*.asm` | disassembly of each pilot with RIP targets resolved |
| `site_classes.json` / `site_class_counts.json` | per-site role, operand kind, literal values |
| `site_mechanisms.json` | per-site patch mechanism (A/B/C/D) |
| `rip_redirects.json` | where each rip-relative patch lands |
| `data_sites_readers.json` | the 74 data sites with their original values and every reader |
| `plt_symbols.json` | 661 import thunks resolved to PS4 symbols (`gettimeofday`, `sceKernelUsleep`, `pthread_*`, …) |
| `verified_summary.json`, `validation.txt`, `validation_summary.json` | counts and the PASS gate |
| `literal_survey.py`, `literal_readers.py` | the `.rodata` frame-rate literal survey and its reader map (§7) |
| `find_pointers.py` | finds an 8-byte code pointer anywhere in the file (§3) |
| `who_builds.py` | caller/vtable evidence for `0x63fad0` (§5) |
| `classify_sites.py`, `site_mechanisms.py`, `rip_redirects.py`, `hook_word.py`, `flipper_math.py`, `crosstab.py`, `build_targets.py`, `check_notes.py`, `validate_outputs.py` | the scripts that produce all of the above |

## 9. Earlier work in this folder: kept, fixed, deleted

Kept and re-verified:

* `presets.json` — still byte-identical to all five v1.09 presets (checked by
  `validate_outputs.py`).
* `patch_sites.json` — 364 distinct addresses; 137/247/271/328 lines per preset,
  162 for `60FPS (no deltatime)`.
* `verified_map.py` / `verified_functions.json` / `verified_groups.json` /
  `verified_scan.json` / `verified_gaps.json` / `verified_patched_operands.json` —
  the FDE table and the whole-image reference/call index. `verified_map.py`
  asserts `fde_count == 162959` and `starts[:3] == [0xa0, 0xf0, 0x170]`, and
  that every FDE's `pc_begin` equals its table entry.

Superseded (kept on disk, listed here so nobody re-derives from them):

* `ehframe.py`, `ehframe_full.py`, `eh.py` — **superseded and partly wrong**
  `.eh_frame` parsers. `verified_map.py` reads `PT_GNU_EH_FRAME`'s header table
  (version 1, encodings 0x1b/0x03/0x3b, `fde_count` = 162959, first starts
  `0xa0`/`0xf0`/`0x170`) and takes each function's size from its FDE `pc_range`;
  it also asserts `fde_ptr + 8 + pc_begin == initial_location` for every entry.
  `eh.py` additionally claims `.eh_frame` was stripped, which is wrong for this
  image — the FDE records are all present and parse. Do not use any of the three.
* `classify.py`, `disasm.py`, `decode.py`, `sites.py`, `map_sites.py`,
  `owner.py`, `shift.py`, `dump_context.py`, `elapsed.py`, `srcrefs.py`,
  `strings.py`, `tstdump.py`, `survey.py`, `focus_survey.py`, `resume_survey.py`,
  `patched_operands.py`, `verify_relocations.py`, `reloc_table.py`,
  `reloc_index.py`, `vtable_slots.py`, `datablocks.py`, `build_index.py`,
  `inspect_verified.py`, `missed_literals.py`, `probe_got.py` — **superseded**
  exploration helpers, kept only for traceability. `classify.py` in particular
  produced `sites_classified.json`, whose 232 "unmapped" results were an artifact
  of its own recursive decoder disagreeing with the FDE table; use
  `classify_sites.py` instead. `reloc_index.py` and `reloc_table.py` decode the
  relocation tables (`plt_symbols.py` supersedes them for naming thunks);
  `vtable_slots.py` and `reloc_index.py` both mis-derive the slot vaddr and
  report zero hits, so do not trust them.
* `gap_scan.py`, `source_survey.py`, `timing_leaves.py` — **still needed**:
  `gap_scan.py` produces `verified_gaps.json`, which `classify_sites.py`,
  `build_targets.py` and `validate_outputs.py` all read.
* `finish_map.py` still writes `data_sites_readers.json`, `source_readers.json`,
  `unpatched_constant_candidates.json` and `unpatched_timestep_literals.json`,
  but its `targets.csv` / `pilot_targets.csv` writer was replaced by
  `build_targets.py` (its pilot list included `0x63fad0`, a 499-byte Havok
  constructor that the previous run had already flagged as likely middleware, and
  omitted `0x103fac0` and `0xf89db0`).
* `site_delta.json`, `runs.json`, `sites_by_func.json`, `functions.json`,
  `code_context.txt`, `site_context.txt`, `verified_patch_context.txt`,
  `verified_gaps.json`, `verified_scan.json` (the 49 MB index),
  `float_const_index.json`, `timing_leaves`-style `.asm` dumps,
  `verified_fn_*.asm`, `verified_leaf_*.asm` — intermediate artifacts kept for
  traceability.

Deleted by this run: none of the previous files; the old `pilot_*.asm` files were
removed so they could not be confused with the regenerated ones.

## 10. Open problems

1. **The 120 FPS ceiling is unproven.** §7.1 shows no cap in the game code; the
   real cause is outside this analysis. Needs a profiler run.
2. **`0x103d3b0` / `0x1066e60` field semantics** (phase index, mode) are
   inferred from argument order; confirming them needs the SprjTask layout.
3. **`0x1dbbd00`'s frame-time role** rests on the `SprjWorldAiManager` string and
   its 18 patch sites; it has no direct caller.
4. **15 patched data constants have no reader** in the image. Either the reader is
   computed at run time or the patch is defensive; not resolved here.
5. **Boundary confidence**: `0x1fdedd0` (27 bytes) and `0x2bbf178` (16 bytes) are
   inferred, not FDE-derived. Verify before redirecting.
6. `0x2035310` (358 bytes) is the Flipper mode selector: it reads key bits out of
   a `uint32` at `[rsi+0x3c]`, wraps 0..4 and has overflow-sensitive unsigned
   compares. It is not a patch site and was not analysed further; it is the next
   thing to read when turning the mode dispatch into decompiled code.
