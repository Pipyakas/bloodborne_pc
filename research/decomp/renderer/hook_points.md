# Hook points (renderer-2) — smallest set exporting data to bbport

All addresses are image offsets (VA = +0x400000). Sizes are FDE pc_range via
`research/globals/ereader.py`; callee counts in `targets.csv` are distinct
FDE-start direct call/jmp targets from `survey.json` (sha256-checked).
"Hook" = read args/buffers at entry/exit; no full decompilation needed.
Detail and disassembly excerpts: `notes.md`.

## Camera matrices (2 hooks; far-plane signature does NOT exist statically)

1. `0x19baa40` (size 1607, 13 callees) — game-side camera-matrix store.
   Stores 4×4 to `[rcx+0x10..0x40]` + scalars `[rcx+0x50..0x5c]`
   (`0x19babc9–0x19bac2b`); calls frustum builder `0x1da6140`.
   Export: view/proj source matrices + the `0x4022`-tagged param select is
   downstream at `0x1ff91a0`.
2. `0xbb85b0` (size 727, 0 callees) — `GPUMath_MatrixCameraLookAt`; writes
   view rows + eye to `rbx[0x00..0x38]` (`0xbb87f7–0xbb8858`).
   Export: ground-truth view matrix to validate hook 1 against.

## Pass boundaries (3 hooks cover frame → postfx → GPU submit)

3. `0x21d2dd0` (size 5184, 23 callees) — postfx anchor: calls `0xbb4b40`
   twice (→ `0xbb1c20` Yebis fanout) + `0xbd82f0 BeginPostEffectScene`.
   Export: pass-list begin/end (before = scene passes, after = postfx chain).
4. `0x1094700` (size 962, 6 callees) — reaches `sceGnmSubmitCommandBuffers`
   via thin jmp `0x1089a10` (site `0x1094a02`). Siblings `0x10961d0`,
   `0x109bc90` give per-queue coverage; add `0x1094c50` for
   `SubmitAndFlip`. Export: per-submit pass boundary + command-buffer args.
5. `0x108fbb0` (size 537, 5 callees) + `0x108fdd0` (582 B, 4 callees) —
   per-object VS/PS constant uploads (`UpdateVsShader`/`UpdatePsShader`
   wrappers). Export: per-draw matrices (world/skeleton layout TBD at hook).

## UI start (2 proxy hooks; no GPU submit found in Scaleform code)

6. `0x1f61ed0` (size 1053, 10 callees) — `ScaleformTexRepository` texture
   upload path. Export: UI texture activity ≈ UI start.
7. `0x1ad5b10` (size 1092, 5 callees) — SprjScaleform value dispatch loop.
   Export: UI value/visibility changes.

## Minimal set

`0x19baa40`, `0xbb85b0`, `0x21d2dd0`, `0x1094700` (+`0x1094c50` for flip),
`0x108fbb0`/`0x108fdd0`, `0x1f61ed0`, `0x1ad5b10` = 8–9 hooks exporting
camera matrices, pass boundaries, per-object constants, and UI proxies.
Deliberately NOT hooked: `far3000` param functions (`0x226e4c0`,
`0x22cd2d0` — CPU param tables, verified by `r2_far.py`), `0x22c1050`
(settings ctor), `0x10dca80` (Havok-side skin loading), Yebis internals
(`0xbb1c20` fanout and below — hook the `0x21d2dd0`/`0xbb4b40` boundary).
