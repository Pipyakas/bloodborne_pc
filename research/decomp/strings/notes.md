# Private string-derived module/name map

Input: `C:\code\bloodborne_pc\out\eboot.elf` (requested CUSA03173 v1.09). The requested identity was not independently authenticated against a retail manifest.
SHA-256: `cec1b276e7f9e4db978e57f524f41fbaac594530a3437b002e23f3fab14b4f86`.

## Measured coverage

- Parsed and checked **162,959** FDE entries, including FDE initial PC and `pc_range`; unique starts match the count.
- **83,515** selected string occurrences (not deduplicated by text); **15,778** have direct references.
- **71,064** instruction-validated RIP-relative LEA/MOV references in **20,957** functions (**12.86%** of FDE entries).
- **540** distinct normalized literal source-path modules; **538** are directly linked to **8,206** distinct functions. Plus 16 overlapping inferred subsystem buckets.
- **17,508** preferred proposed labels: **823** medium-confidence scoped-method candidates; all remaining labels are low-confidence navigation associations. No proposed name is treated as a verified original symbol.
- Encodings: {'ASCII': 76701, 'UTF-16LE': 6814}.

## Method and limitations

ELF has no section table. Scanned file-backed, non-writable PT_LOAD bytes and excluded string starts inside exact FDE ranges. This is a conservative read-only-data approximation, not a reconstructed `.rodata` section. Embedded shader packages account for many unreferenced rendering strings (e.g. TexUtil_D3D11), and should not be mistaken for active D3D11 renderer code.
ASCII and ASCII-character UTF-16LE NUL-terminated runs of at least four characters are selected by path, prefix, diagnostic, format-string and subsystem rules. Non-ASCII Japanese strings, shorter names, pointer-table references, absolute-immediate references, computed addresses, VEX forms, non-LEA/MOV instructions, and code outside FDEs are not comprehensively covered. An empty reference list means not found by this method, not unused.
Opcode filtering is followed by Capstone 5.0.7 linear decoding from the containing FDE entry; every accepted site is inside its exact pc_range and contains a RIP-memory operand to the recorded string byte range. References to interior/suffix bytes are accepted and their actual target retained. Linear decoding can stop at embedded data; this is not CFG recovery. MOV may load string bytes rather than form a string pointer.
Paths are slash-normalized and leading printable bytes before drive letters removed (NUL-delimited scans can absorb adjacent scalar bytes). Relative paths remain distinct; path case and `..` are not collapsed. Literal source-path association is high confidence, but inlined headers/helpers can spread it across thousands of callers; it does not prove exclusive translation-unit ownership. Subsystem buckets are keyword guesses and overlap. Scoped diagnostic names can name a callee, a member, or an asserted operation instead of the containing function.
No allocator helpers were executed/imported because their top-level scripts write to their own directory. Their working ELF/FDE and RIP-reference approaches were inspected and independently implemented here. The broken renderer ELF helper was not used.

## Top-level structure supported by strings

- **SPRJ game layer**, under `E:/AB/SPRJ/Source/SPRJ/Source`: AI/Types, FRPG/Game/Chr and Field, Game/Gaitem, Map, Menu/Dialogs, Obj, Gfx/Res/Tex, Havok bridges, Res/Esd, Res/Param/Solo, Sys/Localize, Sys/Mem, Sys/Util.
- **FD4 engine/framework**, `N:/FD4/GameEngine` and `Library/FD4/dist_ps4_vc2012`: Core/Singleton, Memory, RequestUpdate, UserInput, HavokCloth. Singleton headers have very broad inlined coverage; class strings and registration labels provide more specific follow-up anchors.
- **Dantelion2 ClassLibrary/Core**: Compression (Edge/Zlib), IO (binder3/4, streams, PS4 disk), Kernel (heaps/allocators, PS4 synchronization/threading), Resource (cache/load queues/transfer tasks), Script, Logging, Reflection, Runtime, Text and Util.
- **Dantelion2 Extended/ApplicationLibrary**: Lua bridges, Serialize/SerializeLight, UserPreference; CoreGraphics2 GNM synchronization, EzState, FFX particles, FSHKPlus, MagicOrchestra_FMOD, ExtNetwork2 and NexusRevolution2_5. Separate EzMenu library paths.
- **Graphics framework GS/GX**: swapchain control and failover allocator. **YEBIS/PPFX**, `G:/workspace/yebis_git/yebis_draft/src`: GPU interfaces/resources, depth of field, glare, luminance, post effects, render texture pools, TinyEffect and GNM shim.
- **Middleware**: Havok Common/Physics/Physics2012/Animation/Behavior/AI/Cloth and Script/SingleFileVM virtual machine paths; Scaleform/GFx, FMOD codec/event/music sources, PNG/zlib. Path tokens suggest Havok `2014_1_0/r1` and FMOD `fmodexsrc44450`; these are build-tree evidence, not independently verified runtime versions. Lua identifiers include Havok Script VM, not solely upstream Lua.

## Roadmap target correspondence

| Target | String-derived functions (overlapping keyword bucket) | Reliability |
|---|---:|---|
| frame_timing | 24 | Sparse; mixed middleware/game debug controls |
| rendering | 922 | Navigation candidates, not verified implementation ownership |
| resolution_scene | 354 | Navigation candidates, not verified implementation ownership |
| menus | 1,223 | Navigation candidates, not verified implementation ownership |
| memory | 7,549 | Navigation candidates, not verified implementation ownership |
| resources | 1,449 | Navigation candidates, not verified implementation ownership |

### Frame timing

- `0x047d5e84` (ASCII): `hkbGetTimestep` -> `0x009769b0`, `0x00976be0`
- `0x048a7886` (UTF-16LE): `Fix Fps` -> `0x00e9a3c0`
- `0x048ab85a` (UTF-16LE): `Fix Fps` -> `0x00ec42a0`
- `0x048c6bc8` (UTF-16LE): `60Fps` -> `0x01019320`
- `0x048c6bd4` (UTF-16LE): `30Fps` -> `0x01019320`
- `0x048c6be0` (UTF-16LE): `20Fps` -> `0x01019320`
- `0x049bed5c` (UTF-16LE): `%c:%0.1f[ms](%02.1fFPS)` -> `0x02035c20`
- `0x049cb0e4` (UTF-16LE): `Sea Wave Fix Fps` -> `0x0222b170`
- `0x049cb27c` (UTF-16LE): `Fix Fps` -> `0x0222b170`, `0x0222d950`
- `0x049bec72` (UTF-16LE): `30FPS` -> **no direct LEA/MOV reference found**

### Rendering and resolution/scene setup

- `0x0474630d` (ASCII): `pixelAspectRatio` -> `0x00199f30`
- `0x04746368` (ASCII): `screenResolutionX` -> `0x00199f30`, `0x0019ad00`
- `0x0474637a` (ASCII): `screenResolutionY` -> `0x00199f30`, `0x0019ad00`
- `0x047508cd` (ASCII): `The method class_::Capabilities::pixelAspectRatioGet() is not implemented ` -> `0x00255860`, `0x02462220`
- `0x047509ad` (ASCII): `The method class_::Capabilities::screenResolutionXGet() is not implemented ` -> `0x00255950`, `0x024624c0`
- `0x047509f9` (ASCII): `The method class_::Capabilities::screenResolutionYGet() is not implemented ` -> `0x00255980`, `0x02462550`
- `0x047baebc` (ASCII): `resolutionRoundingMode` -> `0x00825e40`
- `0x047bb171` (ASCII): `resolution` -> `0x00825e40`
- `0x047fe8b5` (ASCII): `..\..\src\Gpu\GPUPostEffect.h(3236) : Assertion Failed (iEffectViewIndex == PFXVIEW_CURRENT || iEffectViewIndex >= 0 && iEffectViewIndex < GetNumViews())` -> `0x00bb1c20`, `0x00bb31d0`, `0x00bb3250`, `0x00bb3360`, `0x00bb33e0`
- `0x047fe965` (ASCII): `..\..\src\Gpu\GPUPostEffect.h(3238) : Assertion Failed (iEffectViewIndex >= 0 && iEffectViewIndex < GetNumViews())` -> `0x00bb1c20`, `0x00bb31d0`, `0x00bb3250`, `0x00bb3360`, `0x00bb33e0`
- `0x047fe9d8` (ASCII): `..\..\src\Gpu\GPUPostEffect.h(2486) : Assertion Failed (m_eDepthFactorSource != PFXDFSRC_DEPTHTEXTURE)` -> `0x00bb2a90`, `0x00bb2c20`, `0x00bb2dc0`
- `0x04802ee0` (ASCII): `G:\workspace\yebis_git\yebis_draft\src\GPU\GPUPostEffect.cpp(539) : Assertion Failed (m_pGPUDevice)` -> `0x00bc6590`, `0x00bc98b0`
- `0x04802f5d` (ASCII): `G:\workspace\yebis_git\yebis_draft\src\GPU\GPUPostEffect.cpp(548) : Assertion Failed (m_pGPUDevice)` -> `0x00bc65d0`, `0x00be49e0`, `0x00be4a40`, `0x00bea440`, `0x00bef0a0`, `0x00bf1990`, `0x00bf1b80`, `0x00bf1f20`
- `0x04802fe1` (ASCII): `G:\workspace\yebis_git\yebis_draft\src\GPU\GPUPostEffect.cpp(555) : Assertion Failed (m_pGPUDevice)` -> `0x00bc6610`

### Menus/options

- `0x0487ac64` (ASCII): `N:\SPRJ\Source\Library\EzMenu\Source\Source\detail\Manager\EzMenuEnvironment.cpp` -> `0x00cf9e40`
- `0x049336a7` (ASCII): `E:\AB\SPRJ\Source\SPRJ\Source\Game\Menu\Dialogs\GaitemSelectDialog.cpp` -> `0x01b10dd0`
- `0x04933790` (ASCII): `E:\AB\SPRJ\Source\SPRJ\Source\Game\Menu\Dialogs\GaitemStatusDialog.cpp` -> `0x01b13a10`
- `0x0493fb14` (ASCII): `N:\SPRJ\Source\Library\Dantelion2\dist_ps4\source\ClassLibrary\Extended\UserPreference\Source\DLUserPreferenceService.cpp` -> `0x0215a1c0`
- `0x0493ffc8` (ASCII): `DLUserPreferenceNotifyServiceImpl` -> `0x02162450`
- `0x0493452a` (ASCII): `SprjMenuImp` -> **no direct LEA/MOV reference found**
- `0x04999ae0` (UTF-16LE): `SprjMenuImp` -> **no direct LEA/MOV reference found**

### Memory allocation

- `0x0493a27a` (ASCII): `E:\AB\SPRJ\Source\SPRJ\Source\Sys\Mem\CSGraphicsPrivateAllocator.cpp` -> `0x01fbec80`, `0x01fbecd0`, `0x01fbed10`, `0x01fbed40`, `0x01fbed60`, `0x01fbed90`, `0x01fbedc0`, `0x01fbedf0` (+3 functions)
- `0x0493a2bf` (ASCII): `E:\AB\SPRJ\Source\SPRJ\Source\Sys\Mem\SprjFastBiHeap.cpp` -> `0x01fbf0c0`, `0x01fbf300`, `0x01fbfa10`, `0x01fbfdb0`, `0x01fc0e60`
- `0x0493a300` (ASCII): `SprjFastBiHeap already initialized` -> `0x01fbf0c0`, `0x01fc0e60`
- `0x0493a561` (ASCII): `E:\AB\SPRJ\Source\SPRJ\Source\Sys\Mem\SprjMemoryHeapAllocator.cpp` -> `0x01fca0c0`, `0x01fca140`, `0x01fca1a0`, `0x01fca200`, `0x01fca280`, `0x01fca2d0`, `0x01fca300`, `0x01fca340` (+14 functions)
- `0x0493ba46` (ASCII): `N:\SPRJ\Source\Library\Dantelion2\dist_ps4\source\ClassLibrary\Core\Kernel\Source\DLHeapManager.cpp` -> `0x0207bbf0`, `0x0207c000`
- `0x0493c6d4` (ASCII): `N:\SPRJ\Source\Library\Dantelion2\dist_ps4\source\ClassLibrary\Core\Kernel\Source\DLRegularHeap.cpp` -> `0x020858a0`, `0x02085e20`
- `0x0493cbd6` (ASCII): `N:\SPRJ\Source\Library\Dantelion2\dist_ps4\source\ClassLibrary\Core\Kernel\Source\DLSegregatedRegularHeap.cpp` -> `0x01fc48c0`, `0x01fc4a50`, `0x01fc4cf0`, `0x01fc8440`, `0x0208a400`, `0x0208a6b0`

### Resource loading

- `0x0486bf81` (ASCII): `N:\SPRJ\Source\Library\Dantelion2\dist_ps4\source\ClassLibrary\Core\Resource\Source\CacheSystem.cpp` -> `0x0266ff90`, `0x0266fff0`, `0x02670030`, `0x026700b0`, `0x026700e0`, `0x02670170`, `0x02670370`, `0x026703a0` (+9 functions)
- `0x0486e8d0` (ASCII): `N:\SPRJ\Source\Library\Dantelion2\dist_ps4\source\ClassLibrary\Core\IO\Source\DLBinder4DataReader.cpp` -> `0x0269fde0`, `0x026a0230`, `0x026a02f0`, `0x026a08c0`, `0x026a0980`, `0x026a0ed0`, `0x026a1340`, `0x026a1540` (+1 functions)
- `0x04930040` (ASCII): `E:\AB\SPRJ\Source\SPRJ\Source\FRPG\Game\Field\WorldRes.cpp` -> `0x01547650`, `0x01547b40`, `0x01565fe0`, `0x01566020`
- `0x0493534f` (ASCII): `E:\AB\SPRJ\Source\SPRJ\Source\Gfx\Res\Tex\TpfResCap.cpp` -> `0x01d8e320`
- `0x0493875a` (ASCII): `E:\AB\SPRJ\Source\SPRJ\Source\Res\Esd\EsdResCap.cpp` -> `0x01ee4fc0`, `0x01ee5e00`
- `0x0493d566` (ASCII): `N:\SPRJ\Source\Library\Dantelion2\dist_ps4\source\ClassLibrary\Core\Resource\Source\DLResourceManagerImpl.cpp` -> `0x020c1160`, `0x020c11a0`, `0x020c1250`, `0x020c1310`, `0x020c13c0`, `0x020c1530`, `0x020c1760`, `0x020c17e0` (+2 functions)
- `0x0493d9a9` (ASCII): `N:\SPRJ\Source\Library\Dantelion2\dist_ps4\source\ClassLibrary\Core\Resource\Source\FileTransferTask.cpp` -> `0x020d6950`, `0x020d6a20`, `0x020d6f80`, `0x020d7560`, `0x020d7b10`, `0x020d7b80`, `0x020d7df0`, `0x020d7e50` (+1 functions)
- `0x049b2854` (UTF-16LE): `param:/GameParam/GameParam.parambnd` -> `0x01948fe0`, `0x01f10150`
- `0x049387cb` (ASCII): `EsdResCap` -> **no direct LEA/MOV reference found**
- `0x049ad782` (UTF-16LE): `EsdResCap` -> **no direct LEA/MOV reference found**

### Parameter/EzState names

- `0x04877cc4` (ASCII): `N:\SPRJ\Source\Library\Dantelion2\dist_ps4\source\ApplicationLibrary\EzState\Source\Interface\EzStateEventImpl.cpp` -> `0x02744e80`, `0x027451b0`, `0x027453f0`
- `0x0493abb4` (ASCII): `EzState v2.25.0#0` -> `0x02019010`
- `0x04947b54` (UTF-16LE): `EzStateResult:` -> `0x013084f0`
- `0x04986058` (UTF-16LE): `EzState fix randum integer number %d (-1:randum)` -> `0x01920510`
- `0x049bc256` (UTF-16LE): `ezstate_id` -> `0x01ff2120`, `0x02007de0`
- `0x049bf2ec` (UTF-16LE): `EzState[TalkID:%d]` -> `0x02039960`
- `0x049bf312` (UTF-16LE): `EzState Talk Env` -> `0x02039430`, `0x02039960`, `0x0203f9f0`, `0x0203fa20`
- `0x049bf334` (UTF-16LE): `EzState Talk Event` -> `0x02044480`, `0x02044550`
- `0x048780ee` (UTF-16LE): `EzStateProject` -> **no direct LEA/MOV reference found**
- `0x0487810c` (ASCII): `EzStateProject` -> **no direct LEA/MOV reference found**
- `0x048781a0` (UTF-16LE): `EzStateRegisterSetEvent` -> **no direct LEA/MOV reference found**
- `0x048781d0` (ASCII): `EzStateRegisterSetEvent` -> **no direct LEA/MOV reference found**
- `0x04878256` (UTF-16LE): `EzStateSpawnChildEvent` -> **no direct LEA/MOV reference found**
- `0x04878284` (ASCII): `EzStateSpawnChildEvent` -> **no direct LEA/MOV reference found**

Frame-timing caveat: the FPS option strings and 60/30/20Fps labels are useful controls/registration anchors, not proof of the arithmetic timestep consumers sought in the roadmap. Likewise rendering/scene/menu diagnostic functions may be developer UI and must be traced before replacement.

Parameter names (including EquipParamWeapon/Protector/Accessory/Goods, NpcParam, AtkParam_Npc/Pc, BehaviorParam and SpEffectParam) are predominantly UTF-16LE data-table names. Many lack direct references because tables can supply pointers. Do not assign adjacent functions based solely on string proximity.

## File schemas and reproduction

- `strings.csv`: string VA, encoding, overlapping categories, full text, distinct referencing FDE starts, instruction sites/kinds and actual RIP target.
- `modules.csv`: source path or inferred subsystem, confidence and basis, function count/list, string addresses and examples. `unlinked` source rows have no demonstrated direct reference.
- `names.csv`: one preferred proposal per function, exact size, confidence/kind, literal evidence and caveat. Address-suffixed labels are invented, not recovered symbols. Alternate evidence remains in strings.csv.
- `stats.json`: machine-readable input provenance and coverage. `build_map.py` and `write_notes.py` reproduce the results using `C:\Python314\python.exe`; outputs are restricted to this directory. CSV readers should raise csv.field_size_limit because shader-table strings and module function lists are large.

## Validation performed

CSV counts, unique preferred-name function keys, reference-function union, and every name evidence address checked after generation. All 162,959 FDE initial addresses and ranges checked against the EH table. No runtime testing, game launch, repository edit, commit, or publication performed. At most two research processes were concurrent (usually one).
