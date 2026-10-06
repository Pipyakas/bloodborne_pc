# Ghidra headless pipeline (`tools/decomp/ghidra`)

Reproducible headless Ghidra pipeline for the Bloodborne
decompilation project: import the linked PS4 image with import
names applied, seed every EH-frame function, and export
per-function pseudo-C and assembly as starting points for
decompilation workers.

This folder contains **tooling only**. Nothing derived from the
game binary is committed: the normalized ELF, the Ghidra project,
the seed manifest, logs, assembly and pseudo-C all stay in the
private output directory (`C:\code\bbport-decomp\ghidra` by
default).

## Portable tool requirements

Extract the latest **release** ZIP from
<https://github.com/NationalSecurityAgency/ghidra/releases/latest>
and a portable JDK into `C:\code\_tools`:

* Ghidra 12.1.4 → `C:\code\_tools\ghidra_12.1.4_PUBLIC`
* Microsoft OpenJDK 21.0.12.1+1 (Windows x64) →
  `C:\code\_tools\jdk-21.0.12.1+1`

Java 17 is insufficient for Ghidra 12.1.4. No installer, admin
access, registry changes, or global PATH/JAVA_HOME changes are
needed; the runner sets `JAVA_HOME` for its child only. Override
with `--ghidra <dir> --java <dir>`.

## Modes

```powershell
python tools/decomp/ghidra/pipeline.py analyze
python tools/decomp/ghidra/pipeline.py audit
python tools/decomp/ghidra/pipeline.py export 0xa0 0xf0 0x170
python tools/decomp/ghidra/pipeline.py export-batch --batch 10000 --pool game
python tools/decomp/ghidra/pipeline.py export-batch --batch-file list.txt
```

- `analyze` — one-time import + seed + auto-analysis. Runs
  `prepare.py`, imports the normalized ELF
  (`x86:LE:64:default` / `gcc` SysV), creates a function at
  every FDE entry (`SeedFunctions.java`), then runs full
  auto-analysis. Refuses to overwrite an existing project.
- `audit` — read-only quality report (`AuditQuality.java`) →
  `<out>/audit.json`: FDE coverage, function/string/switch
  counts, import-slot and guessed-name label coverage. Fails if
  any import slot or medium-confidence name is missing.
- `export ADDR...` — export the given hex image-offset function
  entries (`.c` + `.asm`), resuming across invocations.
- `export-batch` — export the next N pending entries (or the
  entries in a file), one address per line. `--pool game` runs
  game-code starts first and defers middleware starts
  (`research\middleware\middleware_functions.csv`). Batches
  larger than 400 addresses pass through a private list file:
  Ghidra truncates concatenated script arguments at 255
  characters.

All modes serialize on an exclusive `pipeline.lock` in the
private directory; one runner at a time, including exports. Do
not open the shared project in the GUI while the pipeline runs.
A stale lock is only removed after confirming its recorded PID is
not running.

## What the pipeline does

1. **`prepare.py`** (read-only inputs): validates the EH-frame
   header encodings and FDE bounds, reads all 162,959 function
   starts from the FDE table, recovers import slots from the
   Orbis ELF relocation/symbol tables, and maps NIDs to names
   using the public `src/import_names.inc`. It writes a private
   normalized ELF copy (ET_EXEC, System V OSABI, load/TLS/EH
   program headers only, image-relative pointer relocations
   applied) and a seed manifest with:
   - every FDE start (image offsets; PS4 VA = offset + 0x400000),
   - import slots as `imp_<name>` labels with NID plate comments,
   - **medium-confidence** proposed names from
     `research\strings\names.csv` as `guess_`-prefixed labels
     (low-confidence rows are not applied).
2. **`SeedFunctions.java`** disassembles and creates a function
   at every FDE entry before auto-analysis, applies the guessed
   labels, labels import slots, and marks them as pointer data.
   A nonzero failed-entry count fails the run.
3. **Auto-analysis** runs with no timeout truncation.
4. **`ExportFunctions.java`** (`-readOnly -noanalysis`) writes
   `<out>/<addr>.c` (decompiled, 60 s per-function timeout) and
   `<out>/<addr>.asm` (instruction bytes + text) for every
   requested entry, appending to `<out>/index.csv`
   (address,size,name,decompile,decompiled) so exports resume
   across interruptions. `export-metrics.json` holds the most
   recent batch's per-function decompile times.

## Heap / CPU bounds

Heap capped at 12 GiB (`GHIDRA_HEADLESS_MAXMEM`), analysis CPUs
and JVM active processors at two, one native decompiler worker.
The Windows launcher briefly uses `cmd.exe` (the Python
supervisor stays alive). Do not start other computational jobs
through this pipeline.

## Tests

`python tools/decomp/ghidra/test_prepare.py` — synthetic ELF
fixture tests for `prepare.py` (no proprietary bytes, no game
files).

## Interpretation and limitations

Pseudo-C is an aid, **not** verified replacement code. SysV
register arguments are inferred, but signatures, object layouts,
signedness, vector types, indirect calls and TLS require review
against assembly. FDEs establish entry points, not all possible
functions or perfect recovered bodies; auto-analysis may create
extra functions or absorb an FDE start into a larger function.
Strings and switch tables depend on reachable disassembly and
pointer recovery; unresolved switches and overlapping/tail-called
functions still need manual correction. Runtime host-import
bindings are not injected. Never publish these exports or use
them as proof of functional equivalence.
