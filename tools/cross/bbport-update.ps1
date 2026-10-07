# Installed Windows builds only (dist/windows, made on d1 by tools/cross/build-windows.sh):
# Bloodborne.cmd runs this before starting the game. It asks d1 for its latest build over SSH and,
# when it differs from this one, copies it over this folder. Files the build doesn't contain
# (bbport.ini, user\, mods\, out\game_dir.txt, prepared images, DLSS DLLs) are kept. If d1 can't be
# reached within one second the installed build starts unchanged. BBPORT_REMOTE / BBPORT_REMOTE_DIST
# override the host and folder (the download is that folder's .tar.gz, packed with each build).
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$remote = if ($env:BBPORT_REMOTE) { $env:BBPORT_REMOTE } else { 'd1' }
$dist = if ($env:BBPORT_REMOTE_DIST) { $env:BBPORT_REMOTE_DIST } else { '/var/mnt/ssd1/bloodborne_pc/dist/windows' }
$running = Get-Process bb-probe -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$root\*" }
if ($running) { Write-Host 'The game is running from this folder: not updating.'; exit 0 }
$local = if (Test-Path "$root\BUILD") { (Get-Content "$root\BUILD" -TotalCount 1).Trim() } else { '' }
# A connection timeout alone does not bound DNS, authentication or a stalled
# remote command. Run ssh directly (no cmd child), with a wall-clock deadline.
# File capture avoids the pipe/console hangs seen with the earlier updater.
Write-Host "Checking $remote for a bbport update (1 second maximum) ..."
$token = [Guid]::NewGuid().ToString('N')
$stamp = Join-Path $env:TEMP "bbport-build-$token.txt"
$errors = Join-Path $env:TEMP "bbport-build-$token.err"
$check = $null
$latest = $null
try {
    $check = Start-Process -FilePath 'ssh.exe' -ArgumentList "-n -o BatchMode=yes -o ConnectTimeout=1 $remote `"cat $dist/BUILD`"" -NoNewWindow -PassThru -RedirectStandardOutput $stamp -RedirectStandardError $errors
    # Cache the handle before exit: Windows PowerShell 5 otherwise sometimes
    # loses ExitCode for a short-lived process returned by Start-Process.
    $null = $check.Handle
    if ($check.WaitForExit(1000)) {
        if ($check.ExitCode -eq 0 -and (Test-Path $stamp)) {
            $latest = Get-Content $stamp -TotalCount 1
        }
    } else {
        $check.Kill()
        $check.WaitForExit()
    }
} catch {
    # An unavailable SSH client or failed check must not block offline play.
} finally {
    if ($check) { $check.Dispose() }
    Remove-Item $stamp, $errors -ErrorAction SilentlyContinue
}
if (-not $latest) { Write-Host 'No update found within the check window: starting the installed build.'; exit 0 }
$latest = $latest.Trim()
if ($latest -eq $local) { Write-Host 'Already up to date: starting the game.'; exit 0 }
Write-Host "Updating bbport to $($latest.Substring(0, 12)) from $remote ..."
$archive = Join-Path $env:TEMP 'bbport-update.tar.gz'
# d1 packs each build once (build-windows.sh: dist/windows.tar.gz). scp writes the file itself:
# streaming `ssh tar` into a redirected stdout stalled without a console.
Remove-Item $archive -ErrorAction SilentlyContinue
Write-Host 'Downloading the update ...'
& scp.exe -B -o ConnectTimeout=4 "${remote}:$dist.tar.gz" $archive
if ($LASTEXITCODE -ne 0) { Write-Host 'Download failed: starting the installed build.'; exit 0 }
# Windows's own tar (bsdtar): a GNU tar earlier on PATH would read "C:" as a remote host.
Write-Host 'Installing the update (settings and saves are kept) ...'
& "$env:SystemRoot\System32\tar.exe" -xzf $archive -C $root
if ($LASTEXITCODE -ne 0) { Write-Host 'Unpacking failed: the installed build may be incomplete.'; exit 1 }
Remove-Item $archive -ErrorAction SilentlyContinue
Write-Host "Updated: $latest"
