# Private Ghidra export pipeline

Tooling only belongs in this repository. Never commit the normalized ELF, seed
manifest, project, logs, assembly, or pseudo-C. Defaults put all game-derived
files in `C:\code\bbport-decomp`, outside this checkout.

## Portable tools

Extract the latest **release** ZIP from
<https://github.com/NationalSecurityAgency/ghidra/releases/latest> into
`C:\code\_tools`. This pipeline was developed with Ghidra 12.1.4 and Microsoft's
portable Windows x64 JDK 21.0.12.1+1, extracted into:

* `C:\code\_tools\ghidra_12.1.4_PUBLIC`
* `C:\code\_tools\jdk-21.0.12.1+1`

Java 17 is insufficient for this Ghidra release. No installer, admin access,
registry changes, or global PATH/JAVA_HOME changes are needed. The runner sets
JAVA_HOME for its child only. Override installation paths with `pipeline.py
--ghidra <directory> --java <directory>`.

## Initial import and analysis (one time)

From the tooling checkout:

```powershell
python tools/decomp/ghidra/pipeline.py analyze
```

The private project is `C:\code\bbport-decomp\ghidra\bbport.gpr`, program
`eboot-ghidra.elf`. The runner refuses to overwrite an existing project.
`prepare.py` reads the original `out/eboot.elf` and `out/link.json` without writing
to either. It checks the FDE encodings and bounds, reads all EH-frame function
starts, makes a **private normalized copy** (ET_EXEC, System V OSABI, load/TLS/EH
headers only), and applies Orbis image-relative pointer relocations. Original
code, virtual addresses, and segment bytes other than those pointers are retained.
The generic ELF loader cannot interpret the original Sony dynamic tags reliably.

The supplied `link.json` has only summary counts and unresolved NIDs, **not**
slots or resolved names. Slots therefore come from Sony ELF relocation/symbol
tables; names come from the public `src/import_names.inc` table. Unmapped names
retain a sanitized NID. Import labels are `imp_<name>` at pointer slots; they
are not host addresses and do not imply known prototypes. Counts include data
relocations and repeated slots, not just unique imported functions.

The loader explicitly selects `x86:LE:64:default` / `gcc` (SysV, not Windows
x64). `SeedFunctions.java` disassembles and creates each FDE entry before normal
auto-analysis. A nonzero failed-entry count is treated as failure.

For a long run that survives a tool-call timeout, launch the Python runner with
PowerShell `Start-Process`, redirect stdout/stderr into the private directory,
and monitor the dated `analyze-*.log` and `analyze-application.log`. The Java
headless launcher itself runs in foreground under that runner. Do not report
completion until the runner has exited and `analyze-status.json` says success.
That JSON records total wall time, including normalization, import, seeding,
analysis, and project save. No analysis timeout truncates the run.

## Worker exports

```powershell
python tools/decomp/ghidra/export.py 0xa0 0xf0 0x170
python tools/decomp/ghidra/pipeline.py export-batch --batch 10000 --pool game
python tools/decomp/ghidra/pipeline.py audit
```

Addresses are hexadecimal **image-offset function entries**, not a runtime
rebased address or interior instruction. Output:

```text
C:\code\bbport-decomp\ghidra\export\a0.c
C:\code\bbport-decomp\ghidra\export\a0.asm
```

Pass several functions in one invocation to amortize JVM/project-open time.
`export-batch` exports the next N pending entries (or a file of addresses)
not yet in `export\index.csv`, so a full export resumes across
interruptions; `--pool game` runs game-code starts first and defers
middleware starts. Batches over 400 addresses pass through a private list
file because Ghidra truncates concatenated script arguments at 255
characters. `audit` is a read-only quality report to `ghidra\audit.json`
(FDE coverage, import-slot and guessed-name label coverage; fails if any
import slot or medium-confidence name is missing).

Exports use `-readOnly -noanalysis`: they never rerun auto-analysis or
persist program changes. Each function has a 60-second decompilation
timeout. The dated log records per-function decompile + file-write time;
`export-status.json` records whole-invocation time.
`ghidra\export\export-metrics.json` records the most recent batch.
Re-exporting replaces those functions' files and batch metrics.
Failure is nonzero; successful earlier files in a failed batch may remain.

## Shared-machine concurrency

**One runner at a time, including exports.** An exclusive
`ghidra/pipeline.lock` serializes imports and readers. Do not open the shared
project in the GUI during pipeline use. Ask the owner to close it or use a
separately owned copy instead. No parallel analysis/export instances.

Heap is capped at **12 GiB**, analysis CPUs and JVM active processors at two;
the decompiler uses one native worker. The Windows launcher also briefly uses
`cmd.exe` (the Python supervisor remains alive). Do not start other computational
jobs through this pipeline. A stale pipeline lock after an abrupt kill must
only be removed after confirming its recorded PID and Java/decompiler children
are no longer running; never delete Ghidra project locks to bypass a live owner.

## Interpretation and limitations

Pseudo-C is an aid, **not verified replacement code**. SysV register arguments
are inferred, but signatures, object layouts, signedness, vector types, indirect
calls and TLS require review against assembly. FDEs establish entry points, not
all possible functions or perfect recovered bodies. Auto-analysis may create
extra functions. Strings and switch tables depend on reachable disassembly and
pointer recovery; unresolved switches and overlapping/tail-called functions
still need manual correction. Runtime host-import bindings are not injected.
Never publish these exports or use them as proof of functional equivalence.
