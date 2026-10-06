# bbport patch/hook → function → proposed source change

Private; addresses are image offsets (PS4 VA = +0x400000).
Evidence: `checks.json` (patch→owner histograms, insn checks), `targets.csv`.

## A. Resolution / scene / heap / effects (startup XML + `scripts/patches.py`)

| bbport patch/hook | Function (owner) | Proposed source change |
|---|---|---|
| `--render-res` scene writes (`SCENE_WIDTH 0x1d96a6b`, `SCENE_HEIGHT 0x1d96a7a`; template `Resolution Patch 1280x720`) | `0x1d969f0` (0x2b5); LEAs of `0x51289f8/fc` + `mov` into descriptor `[rbp-0x1b0]/[rbp-0x1ac]` | Decompile `0x1d969f0` (+`0x1d99260` init, `0x2194580`/`0x2199d40` chain) with `width,height` parameters; delete the `B8 imm32` substitution in `resolution_writes`. |
| `--output-res`/UI writes (`UI_WIDTH 0x1f58554`, `UI_HEIGHT 0x1f5855d`, `ui=OUTPUT_SIZE`) | `0x1f58360` (0x2a7); LEAs of same words; callee `0x1f58960` | Decompile `0x1f58360` with separate `ui_w,ui_h` canvas params (stay 1920x1080 until aspect work lands); delete UI substitution branch. |
| Aspect bytes (`0x143a35d`, varies per template) | `0x143a0e0` (0xa2e); site insn `mov [rbx+0x54],0x3fe38e39` | Recover projection/aspect field + FOV in `0x143a0e0`; compute from size ratio instead of patching the immediate. Coordinate: FPS++ also writes this function (26 sites) — shared replacement. |
| Light Grid ×4 (`0x2295cb6/0x2295cc0`) | `0x22958d0` (0x808); `mov [rbx+0x78a8/ac],imm`; consumers `0x22a0a30` (both), `0x22b4660`/`0x22b8280` (`+0x78a8`) | Decompile `0x22958d0` grid init with `grid_w,grid_h` params fed from **output** size (preserve XML baseline, not render size); replace consumers together after confirming object identity. Delete 4 XML variants. |
| `Increased Graphics Heap Sizes` (`0x4736dda/de2` in qwords `0x4736dd8/de0`) | DATA (no code owner); access root `0x1fc0e60` → table base `0x4736d40` | Resolve heap indices via `0x1fc0e60`, derive budgets from target formats/extents, size heap entries + `runtime_memory.c` pool (`BB_DMEM_MB`) from the budget; delete 2-byte patch + heap conditional in `patches.py`. Indices unresolved — no replacement yet. |
| `50% Text scale` (`0x492913c`) | DATA float (1.0); consumer `0x1f5ca90` | Decompile text-scale path with a scale parameter; delete float patch. |
| `Optimal 1080p` + template DATA (`0x51289f8/fc`, `0x48f9a00`-family, `0x4926ea4/8`) | DATA + owners `0x16439b0/0x16448f0/0x1644cf0`, `0x1d2c630`, `0x2017770`, `0x2038ab0/ef0`, `0x1bfb2a0`, `0x15e7ce0` | Plumb size-derived constants through decompiled coordinate functions; per-element (lock-on/HP) attribution still needed. |
| `Disable DoF` (`0x21d7bbc`: `je→jmp`) | `0x21d5f80` (0x3e61); site `je 0x21d7ca0` | Boolean `dof_enabled` in decompiled pass orchestration (early-out/skip), driven by settings; delete byte patch + `patch_code` thread path for DoF. |
| `Disable Chromatic Aberration` (`0x229faa8`) | `0x229e990` (0x209d); param-copy head | Zero/skip the aberration parameter in the decompiled builder from settings; delete 12-byte patch + thread path. |
| `Disable SSAO`/`Motion Blur`/`AA`/`Dynamic Light Shadows` (`0x22c2548/49/4a`, `0x22c2538`) | `0x22c1050` (0x1d70); single dword `mov [rbx+0x2968],0x1010101` + `mov [rbx+0x296e],1` | Settings-struct flag fields (`+0x2968` family) set from options in decompiled ctor; live switches become plain flag writes via the object registry (keep registry, drop code rewrite). |
| `Enable SSR`, `Skip Intro`, `Model LOD`, FPS++ | SSR: `0x22c39f0` (5) + `0x22b8280` + DATA (wider, partially mapped); LOD: `0x1d6f9d0`; FPS++: timing workstream (overlaps `0x143a0e0`) | Out of scope for Phase 3 items 3–4; noted for coordination, not replaced here. |
| Host `vk_scene_resolution.cpp` (`SetSize/ProxySize/Eligible`, 1916x1078 trick, `SizeDivisor`) | Guest roots above (`0x1d969f0→0x2194580`, grid init) | After guest sizes are real: feed host from guest descriptor/identity, retire size heuristics and the 1916x1078 special-case at a safe resource-lifetime boundary. |

## B. Menus / options (`runtime_menu.c`, `runtime_effects.c` hooks)

| bbport hook | Function | Proposed source change |
|---|---|---|
| `list_to_dialog` ← 2× `call TO_DIALOG` (`0x1b392fe`, `0x1b4adb3`) | `0x1bea820` (0x2ae) | Decompile `0x1bea820` with native Quit-row insertion; delete both call redirections. |
| `first_menu` ← `call FIRST_MENU` (`0x1b3871c` in `0x1b384f0`) | `0x1b39030` (0x44a) + ctor `0x1b384f0` | Decompile first/title builders with launch-selection support; delete detour (launch = menu-driven selection, never bare invoke). |
| `record_row` ← 5× `call ADD_ROW` in main menu; `*main_slot = main_menu` | `0x1b4c9a0` (0x207); vtable `0x533bf20` slot2 `0x1b4a3d0` | Decompile row insert + `0x1b4a3d0` (Continue/Load/New/System construction); record natively, drop vtable overwrite. |
| `main_menu_log_in` ← `call ADD_ROW` (`0x1b4b1f4`) | `0x1b4a3d0` row site | Native Quit-instead-of-PSN row in decompiled builder; delete hook. |
| `localize_field` ← LEA (`0x1f58a9d`); `env_content` ← LEA (`0x1bdb987`) | `0x1f58960`/`0x1f58c30` (movie text); `0x1b21840` (Env content) | Decompile walker + localizer (real `+0x138` dispatch, confirmed field offsets) and Env content; host movie/heading logic becomes field reads; delete LEA stubs + `screen_movie` scan. |
| `options_after_brightness` ← `call OPTION_ADD_ROW` (`0x1bb410e`); `options_network_label` ← `call MAKE_TEXT` (`0x1bb41e5`) | `0x1bb3ad0` (0xf63); callees `0x1b4e2a0`, `0x1ae8cc0` | Decompile options builder with native Graphics row; delete both hooks. |
| Graphics sub-screens (`OPEN_SCREEN`, factory, `NEW_SCREEN`, widgets) | `0x1bb4c70`, `0x1bdb270` (tail `jmp rax`), `0x1b20900`, `0x1b29370/0x1b2a100/0x1b2ac00/0x1b2b3b0`, `0x1be8a30/0x1bea630/0x1b4cbb0`, `0x1b6ee10` | Decompile in dependency order (adapters → rows/lists → steps/dialog → screens/widgets → builders); validate guest widget layout constants; then drop stubs, vtable copy, movie-depth file patch only when layout is redesigned. |
| Launch shortcuts (`row_step`/`select_row`: Offline/Continue/Load/New/System) | Row invokes `0x1b49cd0/0x1b481d0/0x1b479f0/0x1b47860/0x1b48b90/0x1b46df0` + `0x1b399d0`, `0x1c1a070/0x1c1a5c0/0x1c2cf10` | Reconstruct 3-arg dispatch (rdx context, refcounts, `+0x48` object); implement shortcuts as selections through the live dispatcher; never call row slot2 with 2 args. |
| `settings_created` ← 7× `call SETTINGS_CTOR`; `settings_destroyed` dtor detour; `patch_code` thread | `0x22c1050` (0x1d70, alloc 0x2c00 ×5 sites) / `0x22c2e10` (0xb3d) | Decompile ctor/dtor with registry + semantic flag application; retire detour/thread when renderer field owners verify. |
| `0x20368c0` ("menu lock") | `0x20368c0` (0xe8), timed packed-pointer cmpxchg lock, **no direct callers found** | No replacement: caller relationship unproven. Find indirect callers first; do not treat as menu lock. |
| `FrpgMenuDlgPcOption` ("unused") | String at image `0x4930ecb`, zero covered xrefs | No replacement: consumer unknown. Resolve references (incl. uncovered encodings/table indexing) before declaring unused. |
