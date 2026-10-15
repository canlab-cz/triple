# SPDX-License-Identifier: MIT
# Copyright (c) 2026 CANLAB s.r.o.
# Builds every distribution package on a Windows machine with WSL2 (Ubuntu
# 22.04 with the Linux build requirements of dist/README.md):
#   powershell -ExecutionPolicy Bypass -File dist\build-all.ps1 [-Clean] [-Test] [-SavvyCanGit <repo>]
#
#   -Clean        remove the build directories first (fresh clone and full build)
#   -Test         run the container tests afterwards (Docker in WSL)
#   -SavvyCanGit  SavvyCAN repository for both builds (default GitHub)
# Writes dist\out\SHA256SUMS at the end.
param(
    [switch]$Clean,
    [switch]$Test,
    [string]$SavvyCanGit = "https://github.com/collin80/SavvyCAN.git"
)
$ErrorActionPreference = "Stop"
$Repo = (Resolve-Path "$PSScriptRoot\..").Path
$Out = "$Repo\dist\out"
# C:\a\b -> /mnt/c/a/b (wslpath can return a Docker Desktop bind-mount path instead)
function To-WslPath($p) { '/mnt/' + $p.Substring(0, 1).ToLower() + ($p.Substring(2) -replace '\\', '/') }
$WslRepo = To-WslPath $Repo
$WslGit = if ($SavvyCanGit -match '^[A-Za-z]:\\') { To-WslPath $SavvyCanGit } else { $SavvyCanGit }
$started = Get-Date
# The Linux packages are built in WSL's own file system and copied over from
# Windows: writing them through /mnt/c failed for paths Docker Desktop had
# bind-mounted before, and it is slower.
$WslHome = (wsl.exe -e bash -c 'echo $HOME').Trim()
$WslDistro = (wsl.exe -e bash -c 'echo $WSL_DISTRO_NAME').Trim()
$WslOut = "$WslHome/.cache/usb2can-triple-dist/out"
$WslOutWin = "\\wsl.localhost\$WslDistro" + ($WslOut -replace '/', '\')

function Step($m) { Write-Host "`n######## $m" }
function Invoke-Wsl($cmd) {
    wsl.exe -e bash -c $cmd
    if ($LASTEXITCODE -ne 0) { throw "WSL step failed: $cmd" }
}

if ($Clean) {
    Step "clean"
    Invoke-Wsl 'rm -rf "$HOME/.cache/usb2can-triple-dist"'
    if (Test-Path "$Repo\dist\work") { Remove-Item -Recurse -Force "$Repo\dist\work" }
    # empty dist\out but keep the folder
    if (Test-Path $Out) { Get-ChildItem $Out -Force | Remove-Item -Recurse -Force }
}

New-Item -ItemType Directory -Force $Out | Out-Null

Step "Debian packages (WSL)"
Invoke-Wsl "cd '$WslRepo' && OUT='$WslOut' SAVVYCAN_GIT='$WslGit' bash dist/debian/build.sh"
Step "Linux tarball (WSL)"
Invoke-Wsl "cd '$WslRepo' && OUT='$WslOut' SAVVYCAN_GIT='$WslGit' bash dist/linux/build.sh"
Get-ChildItem $WslOutWin -File | Copy-Item -Destination $Out -Force
Step "Windows zip and setup"
& powershell -NoProfile -ExecutionPolicy Bypass -File "$Repo\dist\windows\build.ps1" -SavvyCanGit $SavvyCanGit
if ($LASTEXITCODE -ne 0) { throw "Windows build failed" }

if ($Test) {
    Step "tests"
    foreach ($img in "ubuntu:24.04", "debian:12") { Invoke-Wsl "cd '$WslRepo' && OUT='$WslOut' bash dist/debian/test-debs.sh $img" }
    foreach ($img in "ubuntu:24.04", "fedora:41") { Invoke-Wsl "cd '$WslRepo' && OUT='$WslOut' bash dist/linux/test-tarball.sh $img" }
}

Step "checksums"
$lines = Get-ChildItem $Out -File | Where-Object { $_.Name -ne "SHA256SUMS" } | Sort-Object Name | ForEach-Object {
    "{0}  {1}" -f (Get-FileHash -Algorithm SHA256 $_.FullName).Hash.ToLower(), $_.Name
}
[IO.File]::WriteAllLines("$Out\SHA256SUMS", [string[]]$lines)
Get-ChildItem $Out -File | Select-Object Name, Length | Format-Table -AutoSize | Out-String | Write-Host
Write-Host ("done in {0:N0} min" -f ((Get-Date) - $started).TotalMinutes)
