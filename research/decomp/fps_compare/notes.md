# High frame rate, DRS and TAA: Bloodborne against later FromSoftware games

Snapshot 2026-10-07. Bloodborne CUSA03173 v1.09, addresses are eboot vaddr (XML patch
address = vaddr + 0x400000). Reference games: the d1 Steam installs of Dark Souls III
(1.15.2), Sekiro, Elden Ring, Elden Ring Nightreign and Armored Core VI. Their executables,
regulation files and decrypted dumps stay private on d1 (`bbport-decomp/inputs/refgames`,
scripts in `bbport-decomp/research/fps_compare`); only findings are published here.

## 1. The 120 FPS sprint slowdown — root cause found, fixed (confidence: high)

Symptom: at 90–120 FPS the hunter sometimes sprints at roughly half speed until stopping.

Cause: the character movement update `0x1514500` (first argument the movement controller in
`r13`, frame time in `xmm0`, spilled to `[rbp-0x74]`) ends with a "stuck against a wall" check
that no FPS preset touches (no `Uncap/90/60 FPS++` write falls in the function):

```
if (requested_speed [r13+0x1e4] > 5.0 && flag) {
    d = |position - previous_position|            // physics body +0x1e0 / +0x1f0, one frame
    if (1.0 > d * 30.0)                          // moved under 1/30 unit this frame
        m = (m > 0.5) ? m * 0.8 : 0.5;           // m = [r13+0x1e0], scales the velocity
    ... return
}
m = (m < 1.0) ? m * 1.2 : 1.0;
```

`d * 30` is "speed at the authored 30 FPS"; at 120 FPS the same sprint moves a quarter of the
distance per frame, so the threshold is effectively 4 units/s instead of 1. Once the
multiplier is pulled down (the start-of-sprint ramp is enough), half-speed sprinting stays
below the per-frame threshold and the slowdown latches. The constants 5.0, 30.0, 0.5, 0.8 and
1.2 (`0x4926328`–`0x4926338`) have no reader outside this function.

Dark Souls III has the identical code (`sqrtps; mulss [30.0]; comiss`, 0.8 / 1.2 / 0.5 on
`+0x25c`), which is why DS3 unlockers documented "slow running" above ~82 FPS for years;
[DS3DebugFPS 2.0](https://github.com/0dm/DS3DebugFPS) fixed it in September 2026 by rescaling
the constants each frame. The DS1 unlocker's `FixGraze` is the same bug in DS1.

Fix (`tools/patch_asm/sprint_slowdown.s`, patch *High FPS sprint slowdown fix*, enabled with
the 60/90/uncap presets): stuck when `d / dt < 1`, factors `0.8^(30 dt)` and `1.2^(30 dt)`
(Padé (2,2) exp). At dt = 1/30 it is the original. It rewrites only the 259-byte tail
`0x1514b92–0x1514c95` and keeps the outside branch target `0x1514c60`.

Runtime test (d1, RX 6700 XT, Linux native, same save copy, Central Yharnam street, hold
forward + sprint 6 s; camera speed in units/s per 0.25 s, deterministic over two runs):

| run | after ramp-up | mean of the 6 s |
|---|---|---|
| 60 FPS, original | 6.2–6.5 | 4.16 |
| 120 FPS, original | 6.4 for ~1 s, then **3.1–3.3 latched** | 3.57 |
| 120 FPS, fixed | 6.2–6.6 | 4.14 |
| 60 FPS, fixed | 6.2–6.5 | 4.16 |

Camera speed is a proxy for player speed; the result does not cover cloth/Havok (>90 FPS
substep budget, `frame_timing/notes.md` §7.2) or other per-frame logic.

## 2. How the later games reach 120 FPS (confidence: medium, public + static evidence)

- FromSoftware shipped only one PC title with an official >60 FPS mode: **Armored Core VI**
  (30/60/90/120 cap; TechPowerUp ran it at ~200 FPS with the cap patched out). DS3, Sekiro,
  Elden Ring and Nightreign cap at 60; community unlockers patch the CSFlipper frame-time
  target and pass the measured delta through (the same idea as `Uncap FPS++`).
- The engine generation does not make simulation rate-independent by itself: DS3 (same
  generation as Bloodborne) shows the sprint bug above; DS1/DS2/DS3 unlockers each needed
  per-system fixes (slope slide, airborne damping, graze/sprint, ladders, lock-on turn, camera
  smoothing). Players report Sekiro/Elden Ring as largely fine above 60, i.e. later code
  converted more per-frame constants to per-second ones. There is no evidence of a
  fixed-tick simulation with render interpolation in these games.
- So the practical backport is not a transplant: **find per-frame constants and convert them**
  (as here), test each system at 30/60/120. The DS1 unlocker's fix list is a good checklist
  of candidate systems to look for in Bloodborne.
- Alternative for 120 Hz displays: keep simulation at 60 and generate frames (ERSS-FG does
  this for Elden Ring/Nightreign with DLSS-G/FSR FG/XeFG). bbport already has DLSS-G on
  Windows; FSR frame generation (RDNA2-capable) is the missing piece for AMD GPUs.

## 3. FromSoftware's DRS controller (confidence: high for values, medium for semantics)

AC VI, Elden Ring and Nightreign drive DRS from `LoadBalancerParam` in `regulation.bin`
(AES-256-CBC with the keys published in SoulsFormatsNEXT, then DFLT/ZSTD BND4; field names
from the Smithbox paramdex). The community AC VI mod
[Dynamic Resolution (Nexus 203)](https://www.nexusmods.com/armoredcore6firesofrubicon/mods/203)
only edits these rows. Shipping values (rows differ per platform/scene):

| field | AC VI | Elden Ring / Nightreign |
|---|---|---|
| lower / upper FPS threshold (30 target) | 30.6 / 31.6 (some 32.5 / 33.5) | same |
| lower / upper (60 target) | 60.6 / 61.6 | 58.6 / 59.6 (ER row 31) |
| frames below to step down / above to step up | 3–5 / 20 | 5 / 20 |
| sleep after a step down / up (frames) | 15–30 / 10 | 30 / 10 |
| DRS range | 70–100 % (50–75 % on weak rows) | 70–100 % (40–75 % one row) |
| min frames between resolution changes | 2–8 | (not in this param) |
| load-balance level that enables DRS | 1–2 (before effects are cut) | 2 |

Reading: a hysteresis ladder on a headroom estimate just above the target (0.6 FPS
margin), fast to react (3–5 frames), slow to recover (20 frames + sleep), and DRS is the first
lever, ahead of cutting bloom/SSAO/effects. Resolution may move every 2–8 frames: many tiny
steps with temporal history retained, not rare 1 % jumps. For bbport's visible DRS steps the
implication is that step size is not the main lever; history continuity across a resize is
(bbport's TAA still resets its history on resize; FSR/DLSS keep it).

The mod's ten profiles (v1.0, built on an older regulation; checked 2026-10-07) change only
row 2, which identifies row 2 as the PC row. Row 2 (vanilla → mod):

| field | vanilla | Relaxed (target T = 30/40/60/90/120) | Aggressive |
|---|---|---|---|
| lower = upper threshold | 30.6 / 31.6 | T (no hysteresis band) | 1.2 × T (20 % GPU headroom) |
| frames below to step down | 5 | 1–2 | 1–2 |
| frames above to step up | 20 | T (one second) | 1.2 × T (120 at T = 120) |
| sleep after step down / up | 30 / 10 frames | 2–5 / T frames (one second) | 2–5 / ~1 s |
| DRS range, enabling level | 70–100 %, 2 | 50–100 % (70 at 120), 2 | 25–100 %, 1 |
| min change period | 8 | 8 (unchanged) | 8 |

It also disables the effect cuts (levels set to 21 = never), so only resolution moves. The
vanilla PC row barely uses DRS (30 FPS target only); the mod's rule of thumb is "react within
1–2 frames, recover only after about a second of headroom". bbport already uses half-second
measurements, one-point steps and slower recovery; the mod changes no step size or history
handling, so it is no evidence about the visibility of individual steps.

## 4. TAA (confidence: low, open)

- bbport's TAA: computed camera motion + replayed object motion, depth rejection, 3×3
  clipping, history reset on output/AA changes and on DRS resizes (`docs/upscaler.md`).
- Bloodborne itself does have object velocity, but only inside motion blur
  (`MotionBlurWriteCMBVelocity`, `SetExVelocity`, `MotionBlurInternalVelocityTex`); the
  frame analysis in `docs/upscaler.md` found no velocity buffer in the captured frame.
  Whether that velocity covers skinned characters, and at which resolution, is unverified;
  it could replace part of the vertex-replay object motion.
- Later games' TAA shaders were not extracted (encrypted archives); no claim about their
  algorithm is made. ERSS replaces Elden Ring's TAA with DLSS/FSR/XeSS using the game's own
  motion vectors and depth; its source is not public. ERSS-FG v4.14.1 (checked statically
  2026-10-07): a `d3d12.dll` proxy loads `ERSS-FG.dll`, whose code is packed (empty sections, one
  encrypted 14.6 MB blob), so its hooks and resource choices cannot be read without running it.
  It ships the stock DX12 SDKs (FidelityFX upscaler 4.0.3 and frame generation 4.0.0, DLSS-G
  310.6, XeSS 1.3.1); nothing in it is reusable for bbport's Vulkan path.

## 5. Open work

1. Survey other per-frame constants in movement/camera code (DS1 unlocker checklist:
   slope slide, air damping, ladder step-down, lock-on turn, camera smoothing).
2. DRS: keep TAA history through resizes (rescale history) before tuning step sizes further;
   consider a FromSoftware-style ladder (fast down, 20-frame recover, sleeps).
3. FSR 3.1 frame generation on Vulkan for AMD (simulation at 60, display at 120).
4. Check the motion-blur velocity target as an object-motion source.
