# Agent workflow

## Single playable branch (owner decision, 2026-10-06)

- Use `master` in `C:\code\bloodborne_pc` for ongoing port work and the user's
  testing. Do not ask the user to launch a separate task-worktree build.
- Do not create new task branches/worktrees for this port unless explicitly
  requested. Existing branches are historical; preserve them and their dirty
  work until their owners have reviewed/integrated it.
- Coordinate shared-checkout edits, integration, builds and GPU tests before
  starting. The single-checkout policy does not authorize overwriting peers' work.
- Build and test runtime changes before publishing them to `origin/master`.
  Clearly report unfinished work; do not enable unverified experiments by default.

## Curated decomp research

- Curated experimental research notes are consolidated on `master`.
  See `research/decomp/README.md` for scope, provenance and limitations.
- Do not publish game binaries/assets/saves, memory captures, Ghidra projects,
  bulk game-code exports or proprietary SDKs. Keep those inputs private on d1.
- All decomp workers, harness development and decomp GPU/runtime testing belong
  on d1. Preserved Windows decomp work is a backup, not an active worker checkout.
- Never use GPT-6.1 (any provider/alias/variant) for subagents or detached workers,
  even when free workers stall. The user's main composer selection is unaffected.
- Publishing research notes does not authorize enabling experimental decomp
  replacements or rebuilding the playable executable for notes alone.

## Definition of done: update the playable master build

The user tests the port through the **Bloodborne (bbport)** Start menu shortcut.
It launches `C:\code\bloodborne_pc\Bloodborne.cmd`, which uses the prebuilt
`C:\code\bloodborne_pc\out\bb-probe.exe`. A build in another worktree does **not**
update that executable.

For completed implementation work, the user authorizes agents to commit their own
changes, integrate them into `master`, rebuild the playable checkout, and push
the completed work to the user's GitHub fork without asking again. The publishing
remote is `origin`: `https://github.com/Pipyakas/bloodborne_pc.git`. Verify the
remote URL before pushing; do not push to `upstream` or another owner's fork.
Explicit task instructions (for example, review only, leave uncommitted, do not
merge, or do not push) override this default.

During development, the user authorizes agents to close the running playable master
build before rebuilding/updating it, without asking again. Match the executable path
`C:\code\bloodborne_pc\out\bb-probe.exe` exactly: request a graceful close first,
then terminate that confirmed master instance if it does not exit. Do not stop games
running from other worktrees or unrelated applications under this permission.

Before reporting implementation work as finished:

1. Review and test your changes. Keep unfinished, experimental, failing, or
   insufficiently verified changes out of `master`. Report remaining uncertainty.
2. Commit only the files/changes belonging to your task. Never use broad staging
   to include unrelated work, local settings, game data, saves, build output, or
   another agent's edits. Do not discard or reset existing changes.
3. Integrate the completed commits into local `master`. Inspect your branch's
   differences from `master` first: merging a branch must not pull in unrelated
   WIP commits. Prefer a fast-forward when possible; otherwise merge only a
   fully ready branch, or cherry-pick your isolated completed commits. Test any
   conflict resolution before proceeding.
4. Update the playable checkout at `C:\code\bloodborne_pc` to the integrated
   `master` revision, then build **there**, not just in your task worktree:

   ```powershell
   $env:MSYSTEM='CLANG64'
   $env:CHERE_INVOKING='1'
   & C:\msys64\usr\bin\bash.exe -lc 'bash build.sh'
   if ($LASTEXITCODE -ne 0) { throw 'Master build failed' }
   ```

   Use that directory as the command's working directory. If MSYS2 is installed
   elsewhere, use its configured path. On a non-Windows host, run `bash build.sh`
   in the user's designated playable master checkout and explain that it does
   not refresh this Windows executable.
5. Verify the build succeeded and `out/bb-probe.exe` exists. For runtime changes,
   smoke-test the integrated build through `tools/mcp/bbport_mcp.py` and stop your
   test instance afterward. Minimized, silent launches are the default; do not
   launch a foreground game, enable sound, or manipulate desktop focus without
   the user's permission. Preserve the user's settings and saves.
6. Publish tested, integrated work with a normal, non-force push to
   `origin/master`. Fetch/check the current remote first; if it advanced,
   reconcile safely and rerun relevant tests/builds. Never force-push or rewrite
   published history. Verify the published commit and provide its GitHub URL.
   Do not include secrets, game assets, saves, local settings, generated build
   output, or proprietary DLLs. Publishing source does not mean uploading the
   local executable as a GitHub release.
7. Report the integrated/published `master` commit, build result, relevant test
   results, and anything deliberately left out. If integration/build/test/push
   is blocked, say so explicitly; do not claim either the Start menu build or
   GitHub has been updated when it has not.

## Unfinished work

Do not create a separate playable branch for unfinished work. Validate runtime
changes before publishing them on `master`; if blocked, report the exact pending
work and preserve it. Coherent documentation or tooling may be published with
an honest WIP status when it does not enable unfinished runtime behavior.
Never discard or publish someone else's uncommitted changes without review.

## Shared-checkout safety

- Agents may work in separate worktrees, but all publish to one playable master
  checkout. Coordinate integration and builds so only one agent updates/builds
  that checkout at a time. Recheck `master` and checkout status before integrating;
  another agent may have advanced them while you worked.
- Do not switch branches, merge, or overwrite files in a checkout another agent
  is actively editing/building. Do not disturb unrelated dirty files. If safe
  publication is not possible, report the blocker and coordinate with the user.
- Existing local submodule modifications may be deliberate build patches; inspect
  them and preserve them. Do not automatically stage or reset them.
- Older worktrees may not yet contain this file. Read the latest `AGENTS.md` from
  the playable master checkout before publishing completed work.
