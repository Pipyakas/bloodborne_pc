# Installed Windows builds only (dist/windows, made on d1 by tools/cross/build-windows.sh):
# Bloodborne.cmd runs this before starting the game. It asks d1 for its latest build over SSH and,
# when it differs from this one, copies it over this folder. Files the build doesn't contain
# (bbport.ini, user\, mods\, out\game_dir.txt, prepared images, DLSS DLLs) are kept. If d1 can't be
# reached in a few seconds the installed build starts unchanged. BBPORT_REMOTE / BBPORT_REMOTE_DIST
# override the host and folder.
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$remote = if ($env:BBPORT_REMOTE) { $env:BBPORT_REMOTE } else { 'd1' }
$dist = if ($env:BBPORT_REMOTE_DIST) { $env:BBPORT_REMOTE_DIST } else { '/var/mnt/ssd1/bloodborne_pc/dist/windows' }
$ssh = @('-o', 'BatchMode=yes', '-o', 'ConnectTimeout=4', $remote)
$running = Get-Process bb-probe -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$root\*" }
if ($running) { Write-Host 'The game is running from this folder: not updating.'; exit 0 }
$local = if (Test-Path "$root\BUILD") { (Get-Content "$root\BUILD" -TotalCount 1).Trim() } else { '' }
try { $latest = (& ssh @ssh "cat $dist/BUILD" 2>$null | Select-Object -First 1) } catch { $latest = $null }
if ($LASTEXITCODE -ne 0 -or -not $latest) { Write-Host "$remote not reachable: starting the installed build."; exit 0 }
$latest = $latest.Trim()
if ($latest -eq $local) { exit 0 }
Write-Host "Updating bbport to $($latest.Substring(0, 12)) from $remote ..."
$archive = Join-Path $env:TEMP 'bbport-update.tar.gz'
# cmd's redirection keeps the archive's bytes intact (PowerShell 5's would re-encode them).
cmd /c "ssh -o BatchMode=yes -o ConnectTimeout=4 $remote `"tar -C $dist -czf - .`" > `"$archive`""
if ($LASTEXITCODE -ne 0) { Write-Host 'Download failed: starting the installed build.'; exit 0 }
# Windows's own tar (bsdtar): a GNU tar earlier on PATH would read "C:" as a remote host.
& "$env:SystemRoot\System32\tar.exe" -xzf $archive -C $root
if ($LASTEXITCODE -ne 0) { Write-Host 'Unpacking failed: the installed build may be incomplete.'; exit 1 }
Remove-Item $archive -ErrorAction SilentlyContinue
Write-Host "Updated: $latest"
