# Renderer front-end map (renderer-2, resumed)

Binary: `C:\code\bloodborne_pc\out\eboot.elf`,
sha256 `cec1b276...4b4f86` (checked in every script here).
Addresses are image offsets (PS4 VA = offset + 0x400000).
All scripts named `r2_*.py` live in this folder and reproduce every count.

## What earlier work got right / wrong

- KEPT: `survey.json` call/ref tables (sha256 matches this binary; FDE count
  162,959; decoded 41,457,538 of 41,719,719 FDE bytes), `gnm_callers.csv`
  import mapping (58 Gnm/VideoOut slots, all with host names from
  `src/import_names.inc`), `fn_*.asm` dumps, settings-ctor/dtor finding
  (`0x22c1050`/`0x22c2e10` match FDE starts — confirmed again here).
- FIXED: the brief says `elf_tools.py` has a wrong FDE parser. I compared it
  exhaustively against `research/globals/ereader.py`: **0 mismatches on all
  162,959 (start,size) pairs** (`r2_recon.py`). Either it was fixed since, or
  the breakage is elsewhere. I did NOT delete it — instead every number in
  `targets.csv` comes from `ereader.py` (the blessed reader) and says so in
  the evidence column. The `survey.py` `0x360`-immediate heuristic and the
  `candidates['far_immediate']` KeyError path from the old log are not reused.
- REJECTED (matches the monitor's verdict, verified by re-disassembly):
  - `0x4938b99`-style `scaleform_ui` rows are `.rodata` string bytes, not FDE
    starts. Real Scaleform function anchors are e.g. `0x1ad5b10`,
    `0x1f61ed0`, `0x2fc7c0` (all FDE starts, in `targets.csv`).
  - The 7 `far3000`-referencing functions (`0x1244320`, `0x12504d0`,
    `0x13979e0`, `0x1398000`, `0x167bef0`, `0x226e4c0`, `0x22cd2d0`) are NOT
    camera writers. `r2_far.py` shows every site is a `vmovss xmm{0..3},
    [rip+disp]` load of a 3000.0f cell into a param-passing helper
    (`0x292a590` / `0x29316a0` / `0x29beb10` / `0x29acf80`). The `.rodata`
    cells (`r2_verify.py`) hold parameter tables
    (e.g. `0x4925420: [-1.0,1.5,0.0,3.0 | 3000.0,-1.0,5e-05,180.0 |
    10.0,103.0,1.0,1.0]`), not a camera struct. Two are kept in
    `targets.csv` explicitly as documented negatives.
  - The "247 functions that call Gnm imports" / "substantial pass
    dispatcher" claim is not a dispatcher: `gnm_callers.csv` here has 275
    rows, and the bulk (139) is `sceGnmIsUserPaEnabled` (a 1-bit query called
    from `0x10740a0`-style tiny wrappers), plus thin `jmp`-thunks
    (`0x1089a10`-`0x1089c50`, 5 bytes each) and one-level `via_thin_call`
    parents. There is no single function fanning out to passes; the real
    per-frame submits are the 7 wrappers in `targets.csv`.
  - `0x864`-immediate (714 functions) is not "the camera block". Only 14 of
    them have any render-ish string ref (`r2_864.py`); the rest are
    unrelated. No 864-byte camera constant block was found — see camera
    section.

## Frame render flow (traced, not asserted)

Per-frame entry could not be reduced to one function with the static call
graph (no single caller above the submit wrappers; `0x1094700` etc. have no
direct callers in `survey.json` — likely dispatched through vtables/worker
queues). What IS traced end-to-end (each edge = direct call in
`survey.json`, each node = FDE start; `r2_evidence.py` outputs
`r2_ev*.txt` in the global temp dir during the run):

```
0x22f8c70 (frame update, 1721 B)
 └─> 0x22c39f0 (scene dispatch, 6631 B; also called by 0x22f9330/0x22f9940/0x22979b0/self)
      ├─> 0x2266420 ×N (dominant callee — object processing loop candidate, NOT disassembled; open problem)
      ├─> 0x22c1050 (settings ctor)
      └─> 0x2199d40 ...
0x22979b0 (frame post-hook, 11464 B; called by 0x1d96e70/0x21c2ef0)
 ├─> 0x21d2dd0 (postfx anchor, 5184 B)
 │    ├─> 0xbb4b40 ×2 ─> 0xbb1c20 (Yebis context init ─> postfx entry fanout, 14 callees)
 │    ├─> 0xbb26e0/0xbb26f0/0xbb27x0… (Yebis per-effect ApplyEffects chain)
 │    └─> 0x2165c10/0x2166300… (scene draws)
 ├─> 0x2199d40 / 0x22f2060 / 0x22f03d0 (frame blocks; not disassembled)
```

Pass boundaries (what bbport should hook instead of guessing by RT counts):
`0x1094700` / `0x10961d0` / `0x109bc90` (each reaches
`sceGnmSubmitCommandBuffers` via thin jmp `0x1089a10`, site `0x1094a02` shown
by `dump_targets.py`), `0x1094c50`/`0x1095060`/`0x1096550`/`0x109c010` (reach
`sceGnmSubmitAndFlipCommandBuffers` via `0x1089a20`), `0x11faff0`,
`0x26b0b00`, `0x26bb730` (CoreGraphics2/VideoOut path), and VideoOut
`0x11e4170/0x11e41e0/0x11e4530/0x11e4a60 → sceVideoOutSubmitFlip`.
`0x21d5f80` (15,969 B, 144 distinct FDE callees — the 0xbb2xxx Yebis fanout)
is the biggest single draw orchestrator; it also reaches the submit path
(site `0x21d861b: call 0x1089a10`). Its callers are `0x21d5a80`/`0x21d5d90`.

Yebis boundary: everything at `0xbbxxxx`–`0xc8xxxx` with
`G:/workspace/yebis_git/...` or `../../src/Gpu/...` strings is statically
linked Yebis/PPFX (modules.csv: GPUPostEffect 216 fns, GPUTextureUtil 132,
GPUDepthOfField 37, …). Game→Yebis entries: `0xbb4b40` (called ONLY by
`0x21d2dd0`), `0xbd82f0` (`CPostEffect::BeginPostEffectScene`, called by 6
postfx fns `0xbb3c50/0xbb68f0/0xbefb50/0xbf1b80/0xbf1f20/0xbf2bf0`),
`0xbb5e40 → 0x2610760 → searchGnmRenderTargetRegistry`
(`0x26105f0 → 0x11d6800 + 0x2611f10`). Replacement boundary = hook at
`0x21d2dd0`/`0xbb4b40`/`0xbd82f0`, not inside Yebis.

## Camera (view/projection, near/far) — what was actually found

- `0xbb85b0` (size 727, 0 FDE callees, string `GPUMath_MatrixCameraLookAt`,
  ref site `0xbb85ea`): computes a look-at view matrix and stores it to
  `rbx[0x00..0x38]` — three orthonormal rows + eye at `[0x30,0x34,0x38]`,
  w=0/0/0/1 (`0xbb87f7–0xbb8858` disassembled in session). This is Yebis's
  math helper, not the game's camera object writer; it has NO direct callers
  in `survey.json` (likely called via undecoded paths/CFG gaps).
  Confidence: high for "view-matrix writer", low for "the game's camera".
- `0x19baa40` (size 1607, 13 FDE callees, string `SprjCamera` ref): stores a
  full 4×4 (`[rcx+0x10..0x40]`) plus scalars at `[rcx+0x50..0x5c]`, then calls
  `0x1d145b0`, `0x1da6140` (frustum/projection loop, 204 ins, calls
  `0xf5d8d0`), `0x1da6140`-adjacent `0x1d9ec50`, `0xf5d930`/`0xf5d870`
  (virtual-dispatch matrix sinks). Best game-side camera-matrix store found.
  Confidence: medium-high.
- `0x1ff91a0` (size 376): copies a 4×4 twice (`[rbx+0x10..0x40]` →
  `[rbp-0x60..]` and `[rbp-0xa0..]`) and selects a uint param by tag `0x4022`
  (`0x1ff92bf–0x1ff92f6`), then calls `0x1ff22f0`. Camera-adjacent copy, exact
  role unclear. Confidence: medium.
- `0x215b3a0` (`User.Action.CameraLocation`, 11,506 B): input-side action, not
  a matrix writer. Confidence: medium (for "input, not writer").
- near/far: NO `near`/`far` struct fields located. The old
  "far-plane at +0x148 / width +0x110 / height +0x114" claim has no
  disassembly behind it and is withdrawn. bbport's far-plane signature
  (`docs/upscaler.md`: `[0]=3000 [1]=1/3000 [4]=1920 …`, proj row at floats
  52/57/62/63 with near 0.05 → z 1.00002/-0.0500679) was NOT found as a static
  constant block — no function writes that float pattern (the 41 `3000.0f`
  cells are param tables, none adjacent to 1920/1080/1/3000). The projection
  row must be computed at runtime (see `0x1da6140`); capturing it needs a hook
  at `0x19baa40`/`0x1ff91a0`, not a signature. If the hook shows otherwise,
  update this.

## Object transforms / skeletons (partially traced)

- Per-object constant upload path: `0x108fbb0` (537 B) calls
  `sceGnmUpdateVsShader` thin wrapper `0x10754f0` (+`0x1075480`, `0x1076390`,
  `0x1070960`, `0x108e200`); `0x108fdd0` (582 B) calls `sceGnmUpdatePsShader`
  wrapper `0x1075420`. Both are called by `0x21d5f80` and by the
  `0x10901c0/0x1090590/0x1090940/0x1091f10/0x1098370` draw group (which itself
  fans into `0x1076390` = constant-buffer grow/commit helper,
  `0x10873a0`/`0x1073f50` = constant fill loops, `0x108e200`/`0x1070960` =
  bind helpers). Hook `0x108fbb0`/`0x108fdd0` to capture per-object matrices.
  Confidence: medium (call structure verified; buffer layout NOT decoded —
  world-transform vs skeleton offsets unknown).
- Skeleton: `0x10dca80` (3317 B, strings `cameras.` / `skin bindings.`) is a
  Havok skin-binding loader (calls `0x419350/0x425170…` Havok fns), NOT a GPU
  constant writer — the old "skeleton by constant-buffer size" hook claim is
  withdrawn. No bone-matrix upload site identified. Open problem.

## Particles (FFX) and UI (Scaleform) submission points

- FFX: `0x285a5e0` (Box emitter), `0x2aba310`/`0x2abaee0` (billboard
  appearance), `0x2ab0600` (radial blur) etc. carry literal
  `FXClusterEmitter_*.cpp` / `FXParticleAppearance_*.cpp` source paths and
  call only CPU helpers (`0x2879bc0…`, `0x207c460`) — no GNM imports. They
  are CPU particle-system vtables; GPU submission goes through the shared
  draw path above. Hooking them exports particle *spawn*, not draw calls.
  Confidence: medium.
- Scaleform: `0x1ad5b10` (1092 B, `SprjScaleform` ref) is a virtual-dispatch
  value loop (`call qword [rax+0x10]` at `0x1ad5bb3/0x1ad5bcd/0x1ad5bf9`,
  refcount traffic) — UI logic, not GPU submit. `0x1f61ed0`
  (`ScaleformTexRepository`, 1053 B) calls texture-ish `0x2638fa0`,
  `0x2970b40`, `0x27c0e00`, `0x296f770` — the texture-upload side; called by
  `0x1ad4b20`/`0x1ad5b10`/`0x1f3f250`. `0x2fc7c0` is input-event dispatch
  (`scaleform.gfx.*EventEx` strings only). No 1920×1080 constant or direct
  GNM call found in the Scaleform functions — UI geometry likely flows
  through the same `0x1094700`-class submits. So bbport's "UI by 1920×1080
  size" has no static hook confirmed; hook `0x1f61ed0` (texture upload) +
  `0x1ad5b10` (value dispatch) as proxies. Confidence: medium for proxies,
  low for "UI start".

## bbport heuristics → code locations

| bbport heuristic | code location (hook, not full decomp) | status |
|---|---|---|
| camera far-plane signature (`3000, 1/3000, 1920, 1080…`) | none — pattern not present statically; hook `0x19baa40` (store) + `0xbb85b0` (view math) and read matrices at runtime | NOT FOUND statically |
| passes by RT counts + shader hashes | `0x1094700/0x10961d0/0x109bc90/0x1094c50` (submit wrappers) + `0x21d2dd0`/`0xbd82f0` (postfx boundaries); RT search `0x26105f0` | HOOKABLE |
| UI by 1920×1080 | no static site; proxies `0x1f61ed0` (tex upload) + `0x1ad5b10` (dispatch) | NOT FOUND, proxies only |
| skeletons by CB size | no CB writer identified; `0x10dca80` is Havok-side loading only | NOT FOUND |
| objects by draw order | `0x108fbb0`/`0x108fdd0` (per-object const upload) + `0x21d5f80` (orchestrator) | HOOKABLE |

## Open problems

1. `0x2266420` (dominant callee of scene dispatch `0x22c39f0`) never
   disassembled — likely the object loop. 2. Per-frame entry above the submit
   wrappers (vtable/worker dispatch) unresolved — `survey.json` direct-call
   graph has no callers for `0x1094700` etc. 3. Bone-matrix upload site
   unknown. 4. UI GPU submit path unknown (probably shared submits).
   5. `0xbb85b0` has no callers in the direct-call graph — true caller
   unknown (indirect/CFG gap).
