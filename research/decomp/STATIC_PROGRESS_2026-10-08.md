# Static research checkpoint — 2026-10-08

Research and delegated work stopped at the owner's request. This is a curated
checkpoint, **not a reconstructed implementation or a runtime acceptance report**.
No replacements or redirects are enabled by this documentation.

## Independently reviewed findings

Addresses refer to the private CUSA03173 v1.09 image used for the existing
research. `IN` means a function's own incoming RDI; `O` is the accepted incoming
base model for `0x22c39f0`, not a recovered class name.

| Slice | Bounded finding | Manager verification |
| --- | --- | --- |
| `0x2266420` | One conditional 8-byte direct store to `IN+0x28`; no direct read of that field. Guard structure includes a zero descriptor member and a non-null loaded pointer; runtime outcomes are unknown. | 52/52 checks; three fault variants rejected; independent decode |
| `0x22c39f0` | Helper arguments at `0x22c3e34` and `0x22c3e43` are `O+0xce0` and `O+0xd38`, mapping the known store conditionally to distinct `O+0xd08` and `O+0xd60`. Recursive boundaries remain unresolved. | 41/41 checks; independent setup review |
| `0x22c5a60` | Conditional 8-byte direct read at `O+0xd08`; loaded value feeds `0xbb5f10` with a `+0x10` bias. No direct field write. | 25/25 checks; independent consumer review |
| `0xe5b140` | Helper argument bases are `IN+8` and `IN+0x60`; known conditional stores map to `IN+0x30` and `IN+0x88`. Only direct IN-based memory access is a byte write at `IN+0xb8`; no direct own-field `IN+0x28` access. | 65/65 checks; independent decode and three argument mutations rejected |
| `0x22c3d71` incoming edge | Passes `O+0x24f8` to `0xe5b140`, mapping those conditional stores to `O+0x2528` and `O+0x2580`, and its direct byte write to `O+0x25b0`. | 74/74 checks; independent dominance/definition proof and argument mutations |
| First loop of `0x22440b0`, read `0x2244253` | Under declared SysV preserved-register and uncorrupted-frame assumptions, indices are `0..3`, stride index is three times the loop index, and candidate 8-byte read starts are `IN+{0x10,0x28,0x40,0x58}`. The loop guard is **64-bit** `cmp r15,4`; the backedge is an equality-based JNE. | Corrected independent audit 103/103; separate reaching-definition proof, false-width and changed-bound controls |

Verification counts describe bounded static checkers, not gameplay tests.
Passing checks do not certify arbitrary aliases, all call effects, a full
function implementation, or the reliability of a general-purpose analyzer.

## Tooling and remaining uncertainty

The `0x22440b0` verifier's byte-range-overlap predicate and fail-closed
decode/CFG gates were repaired. The focused repair passed 61/61 checks and
independent mid-instruction, valid-start, gap and successor controls. The
candidate overlap helper passed 26 supplied tests, 33,264 independent byte-set
cross-checks and 18 extra validation cases. These tools remain private; they
are not runtime replacements.

Both helper setups in `0x22440b0` use loaded pointers plus strides, not a proven
constant IN-relative address. Its **whole-function census and both complete
six-register helper-call tables remain provisional** beyond the independently
reviewed first-loop read. The second helper-call edge audit at `0x224476e` was
stopped before manager acceptance; partial output is not published as verified.

Remaining unknowns include resource/velocity-map identity and format, loaded
aliases, indirect and recursive effects, guard execution, stack-argument ABI
roles, producer uniqueness, frame corruption through escaped buffers, and
replacement readiness. Negative entry-stack offsets do not exclude outgoing
stack arguments. Exact-address observations do not exclude indirect forwarding.

## Source-conversion relevance and next acceptance gate

These findings support the [renderer](renderer/notes.md) and
[class/lifecycle](classes/notes.md) boundaries in the
[conversion target map](CONVERSION_TARGETS.md), toward the
[source-conversion roadmap](../../docs/DECOMP_ROADMAP.md). They narrow conditional
member accesses and distinguish embedded bases from loaded aliases; they do
not establish renderer/resource semantics or retire a binary dependency.

Any future implementation must first resolve the necessary inputs, ownership,
guards and transitive effects, then demonstrate original-versus-replacement
behavior with independent expectations and injected faults. Graphics/lifetime
checks and gameplay comparisons remain necessary where applicable. No full
source-only game build or portable replacement was demonstrated in this phase.

## Provenance and publication boundary

Manager snapshots, reports, verifiers, mutation controls and task history remain
under the private D1 `bbport-decomp/data/tty-research-20261007/` workspace.
Relevant reviews are `manager-function-2266420/`, `manager-function-22c39f0/`,
`manager-consumer-22c5a60/`, `manager-function-e5b140/`,
`manager-edge-22c3d71/`, `manager-function-22440b0-r4/` and
`manager-22440b0-loop-audit-r2/`. These are provenance references, not included
artifacts. Original executable bytes, raw listings, bulk exports, assets,
captures, saves and settings remain private.

No game/GPU launches, runtime source changes or playable updates were performed
for this checkpoint. The separate upstream-history maintenance attempt was
deferred before rewriting because integration conflicts were unresolved;
existing history and private verified backups were preserved.
