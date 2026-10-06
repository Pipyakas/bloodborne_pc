# Resolution/scene setup and menus/options — target map (resumed)

Private binary-derived material; do not publish. Addresses are **image offsets**
(PS4 VA = offset + 0x400000). Binary SHA-256
`cec1b276e7f9e4db978e57f524f41fbaac594530a3437b002e23f3fab14b4f86`.
Read-only on `C:\code\bloodborne_pc`; all writes in this folder.
Reproducer: `C:\Python314\python.exe build_maps.py` → `checks.json`, `targets.csv`.

## Prior work disposition

Kept (re-verified by `build_maps.py`, evidence in `checks.json`):
FDE parse (162,959 starts/sizes, first starts 0xa0/0xf0/0x170, all initial
locations match); core patch/site→owner mapping (scene/UI/light-grid/text-scale/
DoF/chromatic/settings flags); vtable slot recovery through relocations;
menu hook call/lea verification; rdx-context diagnosis of row invokes;
settings alloc-size evidence; heap-table access root `0x1fc0e60`.
Superseded: `resolution_targets.csv` (67 rows) / `menu_targets.csv` (129 rows) /
`summary.txt` / `targets.json` remain on disk but are **not authoritative** —
replaced by `targets.csv` (66 rows, schema address,size,role,callees,evidence,
confidence). The old `notes.md` claim set is replaced by this file.
Dropped: the ~30 low-confidence heap-table-neighborhood candidates from
`finalize.py` (all except the access root `0x1fc0e60`); half-chain ownership
beyond the `0x2194580` root; per-element lock-on/HP-bar attribution.
`fn_*.asm`, `verified_evidence.json`, `heap_table_refs.json`,
`first_targets.json` kept as raw material, not as claims.

## A. Resolution and scene setup

| # | Claim | Evidence | Confidence |
|---|---|---|---|
| A1 | Scene render size is built in `0x1d969f0` (size 0x2b5). Patched sites `0x1d96a6b`/`0x1d96a7a` are LEAs of the shared dimension words, not immediates. | `0x1d96a6b: lea rax,[rip+0x3391f86]` → `0x51289f8`; `0x1d96a7a: lea rax,[rip+0x3391f7b]` → `0x51289fc`, each followed by `mov eax,[rax]` into descriptor slots `[rbp-0x1b0]/[rbp-0x1ac]`; descriptor passed to `0x2194580` at `0x1d96bee` (`call 0x2194580` verified in disassembly). Owner of both sites is FDE `0x1d969f0`. Native dim words read `0x780/0x438` (1920/1080). | High |
| A2 | bbport replaces those loads with `mov eax,imm32` via the 1280x720 XML template, then substitutes `--render-res`. `patches.py:resolution_writes` expects template bytes `B8 <w32>` at `SCENE_WIDTH=0x1d96a6b` etc. and raises otherwise. | `patches.py` `replacements` table; `xml_patch_lines.csv` 1280x720 row `0x02196a6b bytes32 0x000500B8` (=`b8000500`); patch→owner histogram: Resolution-1280x720 has 6 writes in `0x1d969f0`, 6 in `0x1f58360`, 1 in `0x143a0e0`, 2 DATA. | High |
| A3 | UI movie viewport is a separate consumer in `0x1f58360` (size 0x2a7): `0x1f58554 lea rax,[…]→0x51289f8`, `0x1f5855d lea rcx,[…]→0x51289fc`; calls walker `0x1f58960` at `0x1f585e1`. bbport keeps UI at 1920x1080 (`UI_WIDTH/UI_HEIGHT`, `ui=OUTPUT_SIZE`) so text rasterization stays native. | Instruction bytes/sites in `checks.json`; `patches.py` comment + `UiComposition::NativeViewport` (host). | High (ownership); medium that 1920x1080-UI is a requirement vs implementation contract — it is a contract, not inherent. |
| A4 | Aspect constant is a code immediate inside `0x143a0e0` (size 0xa2e), not a standalone global: `0x143a35a: mov dword ptr [rbx+0x54],0x3fe38e39` (≈1.778). XML varies the 4 bytes at `0x143a35d` per aspect. | Owner(`0x143a35d`)=`0x143a0e0`; insn bytes `c74354398ee33f`; per-template values differ (e.g. 16:10 `cdcccc3f`). Semantic label "camera/projection" is candidate only. | High (location); medium (role). |
| A5 | Light-culling grid dims are `mov [rbx+0x78a8],imm` / `mov [rbx+0x78ac],imm` at `0x2295cb6`/`0x2295cc0` inside `0x22958d0` (size 0x808). Native default both `0x80`. All four Light Grid patches write only these two sites. | Bytes `c783a878000080000000` / `c783ac78000080000000`; histogram: each Light Grid patch = 2 writes, both owned by `0x22958d0`. `0x22a0a30` reads both displacements (`0x22a5d22/2a`, `0x22a5ded/f3` per prior validated scan); `0x22b4660`/`0x22b8280` read `+0x78a8`. | High (init sites); medium (consumer object identity — same displacement ≠ same object, not proven). |
| A6 | No single "GX init fixes target sizes once at boot" function was identified. ASCII search over RX finds no `sceGnmInit`/`sceVideoOut`/`acquireDisplay`/`GNM`; only unrelated `Flip`/`Gnm` substrings. | Exact byte search over executable segment (0 hits for the four names). **Limitation:** system-call names appear as hashed NIDs (`link.json`, `import_names.inc`), so absence of ASCII names is expected and proves nothing about the GNM boundary. Guest side of target creation is the `0x1d969f0 → 0x2194580 → 0x2199d40` chain (calls verified); host decodes GNM command buffers. | High (search result); unresolved (GX-init identity). |
| A7 | Heap-size XML patches are DATA, not code: 2 bytes at `0x4736dda`/`0x4736de2` inside qwords `0x4736dd8=0x8ef00000`, `0x4736de0=0x37600000` (native file bytes `f08e`/`6037` at the patch offsets). No owning function; indexed access root is `0x1fc0e60` (LEAs of `0x4736d40`, shift/index/load pattern per prior scan). Which indices select the graphics heaps is **not resolved**. | Direct byte/qword reads; `Increased Graphics Heap Sizes` histogram = 2 DATA writes; `heap_table_refs.json` base addresses. | High (values/locations); low (exact heap-index flow). |
| A8 | Heap-size dependency on the bbport side: `runtime_memory.c` direct pool defaults to 5056 MiB (`BB_DMEM_MB`; above-1080p needs ~4 GiB more, set via run.sh); `patches.py` appends the heap patch when scene pixels exceed 1920x1080. Source fix = budget-driven heap sizes + pool sizing, not lifted constants. | `runtime_memory.c:pool_size_bytes`; `patches.py:main` heap-conditional. | High (mechanism); medium (exact minimum — 9 GB-class requirement is supplied context, not re-measured). |
| A9 | Host `vk_scene_resolution.cpp` (`SceneTargets::SetSize/ProxySize/Eligible`) scales host proxies only (full/half 1920x1080 vs 960x540 + mip chains); it does not update guest light-grid dims or CPU culling. `SizeDivisor` explicitly treats native sizes. | Source read of `vk_scene_resolution.cpp:70-120`. The 8x-lights observation is user-supplied, not reproduced here. | High (code behavior); medium (causal light-count claim). |
| A10 | Remaining coordinate owners patched by every resolution template: `0x16439b0/0x16448f0/0x1644cf0` (globals + SIMD scale blocks), `0x1d2c630` leaf (6 writes), `0x2017770`, `0x2038ab0/0x2038ef0`, `0x1bfb2a0`, `0x15e7ce0`. Per-element attribution (lock-on vs HP bars) is unresolved. | 1280x720 histogram (6 writes each); prior validated RIP-xref + SIMD-block reads. `Optimal 1080p` (76 writes) covers the same owners + 13 DATA sites incl. `0x48f9a00`-family scale floats and `0x4926ea4/8` bounds. | High (ownership); low (element attribution). |
| A11 | Text scale: DATA float `0x492913c = 1.0`; XML writes 0.5; consumer `0x1f5ca90` (`vmovss` site per prior scan). | Float read; `50% Text scale` histogram = 1 DATA write. | High. |

Dependency-ordered plan A: (1) recover descriptor/packed-coordinate/viewport/grid field types, incl. zero/odd/overflow/rounding; (2) replace leaves + `0x1d99260`/`0x1d969f0` behind allocator/import wrappers, verify at native size first; (3) reconstruct `0x1d969f0→0x2194580→0x2199d40` target creation with per-resource usage names (no half-chain claim without traces); (4) resolve heap indices via `0x1fc0e60` and budget memory; (5) replace grid init + confirmed consumers together, preserving output-size (not render-size) semantics as baseline; (6) recover aspect/FOV + world-to-movie transforms, 16:9 first; (7) add explicit host pass/target identity, retire 1916x1078 and size heuristics at a safe resource-lifetime boundary.

## B. Menus and options

| # | Claim | Evidence | Confidence |
|---|---|---|---|
| B1 | All 10 hook vtables have 8 relocation-recovered slots, all slot targets are FDE starts: `0x533bf20` (slot2 `0x1b4a3d0`), `0x533bc00` (slot2 `0x1b46df0`), `0x533bc50` (slot2 `0x1b47860`), `0x533bca0` (slot2 `0x1b479f0`), `0x533bcf0` (slot2 `0x1b481d0`), `0x533bd40` (slot2 `0x1b48b90`), `0x533be30` (slot2 `0x1b49cd0`), `0x533d120` (slot2 `0x1b6ee10`), `0x5343880` (slot2 `0x1bdb270`), `0x5343790` (list-step callback). Raw file qwords at these addresses are zero (unrelocated image); slots come from RELATIVE reloc addends. | `checks.json:tables`, `tables_all_fde` all true. | High |
| B2 | All 18 `runtime_menu.c` hook sites verify: 14 `call rel32` (FIRST_MENU_CALL→FIRST_MENU, 2× to_dialog, 5× main-menu ADD_ROW, LOG_IN, BRIGHTNESS_ADD, NETWORK_LABEL, LIST_INIT/TO_STEP/DESTROY) + 4 LEAs (LOCALIZE_FIELD, ENV_CONTENT, LIST_STEP table, OPTION movie name). | `checks.json:menu_hooks` all true; `main_slot` raw-zero caveat as in B1. | High |
| B3 | Title flow: ctor `0x1b384f0` (0x32b) → `FIRST_MENU 0x1b39030` (0x44a, 2 callers: `0x1b384f0@0x1b3871c`, `0x1b4bc30@0x1b4bce8`) → list init `0x1be8a30` → MAKE_TEXT `0x1ae8cc0` → ADD_ROW `0x1b4c9a0` → TO_DIALOG `0x1bea820`; main `0x1b4a3d0` (0xf5d) builds Continue/Load/New/System rows. bbport hooks both to_dialog calls + FIRST_MENU call + records rows + replaces PSN Log In with Quit. | Callees of both builders in `targets.csv`; caller scan (`0x1b39030`: 2 callers). | High |
| B4 | Options flow: builder `0x1bb3ad0` (0xf63, 5 callers incl. `0x1b46df0@0x1b46e51`) → option rows via `0x1b4e2a0` → `LIST_TO_STEP 0x1bea630` → destroy `0x1b4cbb0`; Graphics row injected after Brightness; sub-screens via `OPEN_SCREEN 0x1bb4c70` + factory adapter `0x1bdb270` (`mov rax,[rdi+8]; mov rdi,rsi; mov rsi,rdx; jmp rax` — 4 insns, tail-only) + `NEW_SCREEN 0x1b20900`; widgets `0x1b2a100/0x1b29370/0x1b2ac00`, choices `0x1b2b3b0`. | Disassembly + callees in `targets.csv`; `0x1bb3ad0` caller list in `checks.json`. | High (structure); medium (widget layout constants `0x1190/0xe50/0x90/0xd00` — host reconstruction, guest layout to be validated by capture). |
| B5 | Direct row invocation is an ABI mismatch, not just "needs menu": guest row callbacks consume **rdx** (dialog context). `0x1b46df0@0x1b46e19: lea rbx,[rdx+0x48]`; `0x1b47860@0x1b4787c: add rdx,0x48` then `call 0x1b399d0`; `0x1b479f0@0x1b47a04: mov r12,rdx`; `0x1bdaa90` retains rdx (`mov r14,rdx`); `0x1bdb270` forwards rsi/rdx. `runtime_menu.c:row_step` supplies only rdi/rsi. Refcount inc/dec (`lock inc/xadd [reg+8]`) surrounds the `0x1b399d0` call in `0x1b47860`, so lifetime is also bypassed. | First-12-insn dumps + full `0x1b47860` listing captured in checks run; `row_step` source. Exact fault address not captured (no runtime per rules). | High (mismatch); medium (fault causality — static diagnosis, no crash capture). |
| B6 | Settings ctor `0x22c1050` (size 0x1d70, 816 insns) / dtor `0x22c2e10` (size 0xb3d, prologue `55 48 89 e5 41 56` verified). Alloc size `0x2c00` confirmed at 5 sites (`mov esi,0x2c00; mov edx,0x10; call [rax+0x58]` before ctor in `0x22c39f0`×2, `0x22f8c70`, `0x22f9330`, `0x22f9940`); all 7 ctor call sites verify (`0x22958d0`×2, `0x22c39f0`×2, `0x22f8c70/30/40`). Effect flags live in one dword: `0x22c2542: mov [rbx+0x2968],0x1010101` (covers SSAO/AA/motion-blur bytes `0x22c2548/49/4a` + `0x296b` region); `0x22c2532: mov byte [rbx+0x296e],1` covers `0x22c2538` (dynamic shadows). | `call_ok` ×7 true with owners; byte-level insn overlap shown for all four Disable-* patch addresses. | High |
| B7 | DoF patch `0x21d7bbc` is `je 0x21d7ca0` (`0f84de000000`) inside `0x21d5f80` (0x3e61); XML rewrites to `jmp+nop`. Chromatic site `0x229faa8` is `mov eax,[rbp-0xa70]` at the head of a parameter-copy sequence inside `0x229e990` (0x209d); XML zeroes the store. SSR `Enable…` touches `0x22c39f0` (5 writes) + `0x22b8280` + 2 DATA — wider than the menu/effects hooks. | Insn bytes + owners; patch histogram. SSR details not fully mapped. | High (DoF/chromatic owners); medium (SSR full semantics). |
| B8 | `0x20368c0` (size 0xe8) is a packed-pointer timed cmpxchg lock (40-bit pointers: masks `0xffffffffff`/`0xffffff0000000000`, `rdtsc` timeout, `lock cmpxchg [rbx]` on `[r14+0x18]`, futex-style fallback calls `0x2bbec78/0x2bbfe78`, log via `0x20b55b0`). **Zero direct `E8/E9 rel32` callers and zero covered RIP-refs found** — it is *not* proven to be the menu lock; callers must come via indirect calls/jumps or uncovered encodings. Name TBD. | Full disassembly captured in verification run. | High (what it is); low (that menus use it — unproven). |
| B9 | `FrpgMenuDlgPcOption` string exists (image `0x4930ecb`, in the `FrpgMenuDlg…` name table with Disp/Env/Key/Sound siblings) but **zero RIP-relative xrefs were found** with the covered LEA/MOV encodings, so "unused" is unproven — could be referenced via uncovered encodings, wide-table indexing, or not at all. Dialog-name-table region refs found 18 nearby targets but none resolving to a PcOption consumer. | `d.find` + xref scan over RX. | Medium (string presence); low (unused verdict). |
| B10 | Movie-text path: walker `0x1f58960` takes the localizer address (LEA at `0x1f58a9d` verified) and `0x1f58c30` performs `call [rax+0x138]` (×3 sites) — the `(*movie)[0x138](movie,path,text,1)` setter. Host `screen_movie` (nested pointer scan) and dropdown value-pointer scan are heuristics to replace with real fields after decomp. | LEA check true; `+0x138` call sites; `runtime_menu.c:localize_field/screen_movie` source. Field offsets `+0x18/+0x28` in the hook are host-observed, guest layout unconfirmed. | High (dispatch); medium (field layout). |

Dependency-ordered plan B: (1) per-vtable callback signatures (never share the 2-arg typedef; capture rdx context); (2) tiny adapters (`0x1bdb270`, widgets, `0x1ae8cc0`) then row/list init/destroy; (3) `0x1bea630/0x1bea820` with dispatch arg + refcount + movie binding; (4) screen construction + widget builders with real row/value/default types and dropdown commit; (5) first/main/options builders + native additions (no-save vs save, Offline/Continue, nested Graphics, Defaults, reopen, cancel, quit); (6) System/Load/New contexts via menu-driven selection through the live dispatcher (never bare `row_step`); (7) settings ctor/dtor + render policy after renderer field owners verify; retire code-rewrite thread when semantic reads exist.

## Checks performed (all in `build_maps.py` → `checks.json`)

FDE recount (162,959; first starts ok; all initial locations match); byte reads
(dims `0x780/0x438`, heap qwords, text-scale 1.0, all patch-site bytes);
LEA→target resolution (scene/UI sites → `0x51289f8/fc`); insn-level site checks
(scene, UI, light-grid `mov [rbx+0x78a8/ac],0x80`, aspect `mov [rbx+0x54],…`,
DoF `je`, chromatic head, settings-flag dword overlap); 7 ctor `call_ok` + owners;
18 menu hook checks; 10 vtable slot recoveries (all-FDE); caller scans (16
targets, capped lists); patch→owner histograms (19 patch names); PcOption
string + xref scan; lock disassembly + caller/xref search; dialog-name region
ref scan. Key counts: `targets.csv` 66 rows (56 functions + 10 DATA rows);
callers found e.g. `0x1c1a070`:352, `0x1c1a5c0`:18, `0x22c39f0`:8,
`0x22c1050`:7, `0x1bb3ad0`:5, `0x1f58960`:5, `0x2199d40`:6, `0x2194580`:2,
`0x22958d0`:2, `0x1d969f0`:1, `0x1b39030`:2, `0x1b399d0`:2.

## Open problems

GX-init identity (NID-hashed imports hide it; needs import-table + boot-flow
work, not strings); exact heap indices selecting both patched qwords; guest
half-chain allocators below `0x2194580`; per-element coordinate attribution;
full menu dispatch context layout (`+0x48` object); `0x20368c0` callers;
PcOption consumers; SSR full semantics; runtime equivalence (no game launch
per rules; differential harness is Phase-0 work elsewhere).
