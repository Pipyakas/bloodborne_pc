# Agent workflow

## Definition of done: update the playable master build

The user tests the port through the **Bloodborne (bbport)** Start menu shortcut.
It launches `C:\code\bloodborne_pc\Bloodborne.cmd`, which uses the prebuilt
`C:\code\bloodborne_pc\out\bb-probe.exe`. A build in another worktree does **not**
update that executable.

For completed implementation work, the user authorizes agents to commit their own
changes, integrate them into local `master`, and rebuild the playable checkout
without asking again. Do not push unless the user explicitly requests it.
Explicit task instructions (for example, review only, leave uncommitted, or do not
merge) override this default.

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
6. Report the integrated `master` commit, build result, relevant test results,
   and anything deliberately left out. If integration/build/test is blocked,
   say so explicitly; do not claim the Start menu build has been updated.

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
