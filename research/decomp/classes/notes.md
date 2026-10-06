# Private class/vtable inventory — Bloodborne CUSA03173 v1.09

## Scope and measured result

Input: `C:\code\bloodborne_pc\out\eboot.elf`; SHA-256 `cec1b276e7f9e4db978e57f524f41fbaac594530a3437b002e23f3fab14b4f86`.
All addresses are image-relative virtual addresses, **not file offsets or host pointers**.
Outputs and scripts stayed in this directory. No game launch, repository edit, commit, or publication.

- 162,959 FDE function starts; 162,959 pc_ranges independently decoded, 0 failures.
- 12,649 candidate address points (12,649 ABI-header candidates; 0 headerless store-confirmed dispatch/vtable candidates).
- 5,298 tables have disassembly-validated vptr-like stores; 19,400 stores from 11,494 distinct functions.
- 138,227 virtual slots, 78,595 distinct FDE-backed method targets.
- 12,648 **provisional class rows**, not a proven distinct class count: one per table address point except the two settings primary/secondary tables, grouped by proven lifecycle/object offsets. Other multiple-inheritance tables may belong to the same class.
- 182 RTTI-backed tables; 173 distinct table-associated RTTI names. Other surviving RTTI strings need not correspond to concrete tables.
- Independent imported-ABI-kind validation decoded 214 RTTI objects and 167 directional base records, including `N4FMOD6ThreadE`. See `rtti_bases.json`; these are stronger inheritance evidence than no-RTTI constructor guesses.
- The independent ABI-kind inventory has 214 distinct names: 207 nested `N4FMOD...` names, three plain FMOD state types, and four `std::error_category` family names. This differs from the earlier 209-string estimate; these counts are from recognized relocated typeinfo objects, not assumed from that estimate.
- 2,613 undirected base/derived/embedded candidates; 11,542 relatedness components.
- 61 automated immediate-argument allocation-site candidates, **not verified object sizes**. Exact `object_size` is blank except settings (`0x2c00`), independently supported by five heap virtual-slot +0x58 allocations of size `0x2c00` aligned `0x10`, and adjacent embedded instances spaced `0x2c00` in caller `0x229598f`/`0x229599e`.
- 105 source-filename name hints proposed from directly referenced strings, explicitly suffixed `_source_hint` and labeled GUESS. Generic container/allocator helper filenames are excluded; these hints may identify a subsystem rather than the object's true C++ class.

## Reproduction and method

Run `C:\Python314\python.exe recover.py`, `rtti_graph.py` (also runs `check_binary.py`), `refine.py`, then `audit.py` in this directory (Capstone 5.0.7).
`run.log` contains the actual extraction run. The older `probe1.py`/`probe_reloc.py` were not trusted: the former imports the broken renderer helper; the latter read dynamic tags from PT_SCE_DYNLIBDATA instead of PT_DYNAMIC and failed with KeyError. Neither was modified.

1. Parse ELF program headers without renderer helpers. Parse dynamic tags from **PT_DYNAMIC (2)**, and interpret Sony table offsets relative to **PT_SCE_DYNLIBDATA (0x61000000)**, as `scripts/prepare.py` does.
2. Resolve R_X86_64_RELATIVE (8) using addends, and locally defined kind 1/6/7 symbols using symbol value + addend. Unresolved imports never silently become zero. Relocation counts: `{'7': 661, '8': 231478, '1': 2838, '6': 23}`.
3. Read EH header `01 1b 03 3b`; data-relative signed starts/FDE pointers are relative to EH-header vaddr. FDE initial_location is PC-relative sdata4; pc_range is the following four-byte length, without PC-relative adjustment. Verify all decoded initial_locations match header starts, including `0xa0,0xf0,0x170`.
4. Scan all 8-byte-aligned file-backed PT_LOAD slots after relocation resolution for exact FDE starts. Split contiguous code-pointer runs at non-function entries. Accept ABI metadata `offset_to_top,typeinfo` before the address point (nonpositive aligned offset, zero typeinfo or validated mangled RTTI string); separately retain headerless runs only when a decoded function stores their address through non-RIP memory.
5. Find common REX LEA/MOV RIP-relative address materializations, map sites into exact FDE pc_ranges, and decode whole owning functions. A limited register-provenance pass tracks vtable values, `this` from entry RDI, register copies and constant-offset LEAs; kill caller-saved values across calls. Record exact instruction site, operand and object offset in `stores` and `store_evidence.json`. Unknown destination provenance stays unknown.
6. Correlate same-object-offset successive vptr stores and calls to other vptr writers with proven `this` arguments. These are **undirected relatedness candidates**, not asserted C++ inheritance. Shared slot targets are recorded separately and never used alone as inheritance proof.
7. Inspect first two virtual targets for paired complete/deleting-destructor patterns and recurring final call targets. These are labeled destructor **candidates**, because a recurring cleanup helper can be mistaken for delete. All other vptr writers remain constructor/destructor/reset candidates. Exact known settings lifecycle labels come from `runtime_effects.c` and were independently disassembled.
8. Search direct callers of vptr writers; retain preceding-call immediate arguments only where RAX is transferred to RDI before the constructor candidate call. Argument values are observations, not assumed sizeof(T). Recover source/assert strings directly referenced by analyzed vptr-writer functions; do not invent names from nearby unrelated strings.

## Known checks

| Address | Status | Slots | Stores | Decoded references |
|---|---|---:|---:|---:|
| `0x533bf20` | FOUND | 8 | 4 | 3 |
| `0x533bc00` | FOUND | 8 | 3 | 3 |
| `0x533bc50` | FOUND | 8 | 3 | 3 |
| `0x533bca0` | FOUND | 8 | 3 | 3 |
| `0x533bcf0` | FOUND | 8 | 3 | 3 |
| `0x533bd40` | FOUND | 8 | 3 | 3 |
| `0x533be30` | FOUND | 8 | 3 | 3 |

`0x22c1050` constructor and `0x22c2e10` destructor both match FDE starts: True / True. Their assembly dumps are included. Primary/secondary vtables: `['0x53b8450', '0x53b8480']`; embedded-member table candidates `['0x5360b60', '0x53b2fa0']` are not merged into that class. Size `0x2c00` has strong static allocation evidence in `settings_call_sites.asm`; no runtime layout test was performed.
Runtime menu names are **existing hook/table labels**, not recovered original C++ class names.

## Confirmed surviving RTTI hierarchies

Typeinfo class kind is established by PS4 NID-matching imports for `__class_type_info`, `__si_class_type_info`, and `__vmi_class_type_info`, not guessed by adjacent data. Decode SI base pointers and VMI base pointer/offset_flags records. Every named base resolves to another recognized RTTI object. `classes.csv` includes directional RTTI-backed base records when the derived class has a detected vtable; a base without a detected vtable retains its typeinfo/name rather than a fabricated table.

| RTTI types in connected hierarchy | Representative names |
|---:|---|
| 101 | N4FMOD14LinkedListNodeE, N4FMOD14StreamInstanceE, N4FMOD10EventSoundE, N4FMOD12EventSystemIE, N4FMOD11EventSystemE, N4FMOD4RIFF11ChunkReaderE, N4FMOD4RIFF17AtomicChunkReaderE … |
| 15 | N4FMOD13CueRepositoryE, N4FMOD17CoreCueRepositoryE, N4FMOD11ChunkReaderE, N4FMOD18CoreLinkRepositoryE, N4FMOD14LinkRepositoryE, N4FMOD19CoreSceneRepositoryE, N4FMOD15SceneRepositoryE … |
| 9 | N4FMOD24PlayModeSequentialGlobal5StateE, N4FMOD13PlayModeStateE, N4FMOD23PlayModeSequentialStateE, N4FMOD21PlayModeShuffledStateE, N4FMOD28PlayModeRandomNoRepeatGlobal5StateE, N4FMOD19PlayModeRandomStateE, N4FMOD27PlayModeRandomNoRepeatStateE … |
| 8 | N4FMOD18PlayModeSequentialE, N4FMOD8PlayModeE, N4FMOD28PlayModeRandomNoRepeatGlobalE, N4FMOD14PlayModeRandomE, N4FMOD21PlayModeShuffleGlobalE, N4FMOD16PlayModeShuffledE, N4FMOD22PlayModeRandomNoRepeatE … |
| 5 | N4FMOD12EventMemPoolE, N4FMOD14SimpleMemPoolTILi1EEE, N4FMOD14SimpleMemPoolTILi4EEE, N4FMOD13SimpleMemPoolE, N4FMOD12TypedMemPoolE |

## Largest primary-vptr hierarchy candidates

`refine.py` restricts same-this calls to offset zero and the caller/callee's own offset-zero vptrs; restricts transitions to offset zero. This removes embedded-object conflation from the broader graph. 504 edges across 168 nonsingleton components; these remain **undirected candidate hierarchies**, not proven inheritance trees. `classes.csv` base candidates use this conservative graph.

| Table address points | Representative addresses |
|---:|---|
| 33 | 0x538df90, 0x538e1a0, 0x538e200, 0x53a0df0, 0x53a0e50, 0x53a0eb0, 0x53a0f10, 0x53a0f70 … |
| 28 | 0x53b6490, 0x53b64e0, 0x53b6c80, 0x53b81c0, 0x53b8210, 0x53b87c0, 0x53b8810, 0x53b93b0 … |
| 25 | 0x5318c70, 0x5318cb0, 0x5318cf0, 0x5318d30, 0x5318d70, 0x5318db0, 0x5318df0, 0x5318e30 … |
| 16 | 0x5372d40, 0x5372f10, 0x5373490, 0x5376b90, 0x5379640, 0x537b720, 0x537c9f0, 0x537da70 … |
| 14 | 0x5321af0, 0x5321ec0, 0x5322160, 0x536d2f0, 0x536d400, 0x536da00, 0x536dab0, 0x536dfb0 … |
| 13 | 0x5321de0, 0x5322080, 0x5322270, 0x5322330, 0x53223e0, 0x53225f0, 0x5322910, 0x53229f0 … |
| 10 | 0x5322510, 0x53302c0, 0x5330340, 0x5330400, 0x5330500, 0x5330540, 0x53305b0, 0x5330b30 … |
| 10 | 0x53b6720, 0x53b72e0, 0x53b7af0, 0x53b7b80, 0x53b8130, 0x53b8540, 0x53b85d0, 0x53b8660 … |
| 9 | 0x536b490, 0x536b5d0, 0x536b700, 0x536b7c0, 0x536ba10, 0x536bb50, 0x539fe30, 0x539fef0 … |
| 7 | 0x5374cc0, 0x5380050, 0x5382610, 0x5382a90, 0x53830c0, 0x53833a0, 0x5383680 |

See `primary_hierarchy_candidates.csv` and `primary_hierarchies.json` for all evidence and top-15 complete memberships.

## Largest broad related families (includes embedded objects; NOT inheritance trees)

| Table address points | Representative addresses | RTTI labels where available |
|---:|---|---|
| 716 | 0x5318790, 0x531b4d0, 0x531b9f0, 0x531c880, 0x531c8b0, 0x531cac0, 0x53203d0, 0x5320460 … | unnamed; inspect source_strings and relation evidence |
| 40 | 0x5331050, 0x53310a0, 0x533d120, 0x5344500, 0x5344550, 0x53453a0, 0x53453f0, 0x5345440 … | unnamed; inspect source_strings and relation evidence |
| 25 | 0x5318c70, 0x5318cb0, 0x5318cf0, 0x5318d30, 0x5318d70, 0x5318db0, 0x5318df0, 0x5318e30 … | unnamed; inspect source_strings and relation evidence |
| 24 | 0x5327df0, 0x5372d40, 0x5372f10, 0x5373490, 0x5374cc0, 0x5376b90, 0x5379640, 0x537b720 … | unnamed; inspect source_strings and relation evidence |
| 17 | 0x533aed0, 0x5349160, 0x53491b0, 0x5349200, 0x5349250, 0x53492a0, 0x53492f0, 0x5349340 … | unnamed; inspect source_strings and relation evidence |
| 11 | 0x533ba40, 0x533edc0, 0x533f900, 0x533f950, 0x5343d30, 0x53445a0, 0x53445f0, 0x5344640 … | unnamed; inspect source_strings and relation evidence |
| 11 | 0x533e6e0, 0x533e730, 0x533e7d0, 0x533e870, 0x533e8c0, 0x533e960, 0x533eb40, 0x533eb90 … | unnamed; inspect source_strings and relation evidence |
| 10 | 0x53b6720, 0x53b72e0, 0x53b7af0, 0x53b7b80, 0x53b8130, 0x53b8540, 0x53b85d0, 0x53b8660 … | unnamed; inspect source_strings and relation evidence |
| 9 | 0x5336150, 0x5336180, 0x5336240, 0x5336270, 0x53362a0, 0x53362d0, 0x5336330, 0x53997d0 … | unnamed; inspect source_strings and relation evidence |
| 8 | 0x5320ee0, 0x5331810, 0x5331890, 0x53318d0, 0x5331910, 0x5331950, 0x5331990, 0x53319d0 | unnamed; inspect source_strings and relation evidence |

See `relations.csv` for the exact function, same-this call, transition and offset behind every edge; `summary.json` lists full membership of the largest 15 components. Large families can be inflated by embedded-object construction, lifecycle wrappers, or branch-insensitive provenance. Hierarchy direction is deliberately unknown.

## Reliability / open problems

- **High confidence:** relocation targets, exact FDE function boundaries, extracted slot values and shared-target equality, disassembly instruction-boundary validation.
- **Strong but static:** validated vptr-like stores and RTTI names. A stored dispatch-table address is not automatically a compiler vptr; headerless entries are marked accordingly.
- **Provisional:** no-RTTI class identity, constructor/destructor roles, allocation sizes, inheritance and names inferred from source strings. No inferred class layout was runtime-verified.
- This is an exhaustive scan under the stated detection rules, **not a proof that every C++ vtable in the binary is recovered**. Null/pure-virtual/import entries can split runs; FDE-less thunks and nonaligned/custom tables are missed. Pointer arrays with zero ABI-looking headers can be false positives, especially with no writer. Secondary address points and construction vtables are not merged automatically. Complex address materialization, stack-spilled vtable values, 32-bit LEAs and CFG-sensitive flows are not fully tracked.
- The provenance pass is linear and branch-insensitive; `this_proven` means the implemented static tracker found a provenance chain, not a symbolic execution proof. Tail-call destructor wrappers and inlined allocators need worker review. Source-string coverage is limited to directly referencing analyzed functions.
- RTTI-family validation establishes that the method works for FMOD, but does not confer names/RTTI on game classes. Nonpolymorphic class enumeration and exact no-RTTI hierarchy recovery remain open.
- `DLFastFixedAllocator` in the prior allocator research is a nonpolymorphic 0x60-byte state (offset zero is slab bookkeeping, not a vptr). Do not invent a vtable/class assignment from its source-path string; allocator-family wrappers may have their own tables. This inventory does not claim to enumerate that nonpolymorphic state or all game structs.
- Exact byte extents stop at the first non-FDE code pointer; slot counts are detected run lengths, potentially truncated at null/import entries. Do not use them to assert total original vtable extent without manual checks.

## Worker use / header generation

Start from `class_id` / table address, inspect `storing_functions`, then disassemble constructor and slot bodies. Treat the CSV as an index of **evidence**, not ready-to-compile recovered headers. Generate private opaque struct names `BB_vt_<address>` and explicit virtual slot prototypes with unknown signatures; preserve zero-based slot index and image-relative target. Use store `object_offset` to draft embedded/secondary vptr offsets only after confirming the destination is the allocation's object, not stack/global memory. Do not infer member order or padding from a class name.

CSV JSON cells can be large because every shared-slot table membership is included. Python consumers should call `csv.field_size_limit(64 * 1024 * 1024)` before `DictReader`; otherwise the default 128 KiB field limit will reject some rows. CSV addresses are hex strings, slot indexes in `shared_slots` are decimal JSON keys, and constructor/destructor/evidence fields contain JSON rather than delimiter-split lists. The final audit checks slot counts, all shared memberships, FMOD Thread, and known table targets against the independent relocation reader.

Prioritize ABI tables with stores, known menu tables, settings lifecycle, and named Dantelion source-string references. Validate allocator signatures to promote candidate sizes, establish constructor/destructor call direction to promote base edges, and merge secondary tables only with shared allocation/RTTI evidence. Recovered C++ headers need differential tests before integration. All binary-derived CSVs, names, assembly and notes stay private.
