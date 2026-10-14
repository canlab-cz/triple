# SPDX-License-Identifier: MIT
# Copyright (c) 2026 CANLAB s.r.o.
# Windows distribution in dist\out\:
#   usb2can-triple-<version>-windows-x64.zip        portable: unzip and run SavvyCAN\SavvyCAN.exe
#   usb2can-triple-<version>-windows-x64-setup.exe  installer (Inno Setup), if ISCC is found
#
# Needs Qt 6.8 for MinGW with the SerialBus and SerialPort modules, CMake and
# Ninja from the Qt online installer, git and Python 3.
#   powershell -ExecutionPolicy Bypass -File dist\windows\build.ps1
param(
    [string]$SavvyCanGit = "https://github.com/collin80/SavvyCAN.git",
    [string]$SavvyCanRev = "3288f1f",                # QT6WIP commit the changes are tested on
    [string]$QtDir = "C:\Qt\6.8.1\mingw_64",
    [string]$QtTools = "C:\Qt\Tools",
    [string]$Iscc = ""
)
$ErrorActionPreference = "Stop"
$Repo = (Resolve-Path "$PSScriptRoot\..\..").Path
$Version = (Get-Content "$Repo\dist\VERSION").Trim()
$Out = "$Repo\dist\out"
$Work = "$Repo\dist\work\windows"
$Name = "usb2can-triple-$Version-windows-x64"
$Stage = "$Work\stage\$Name"
$env:PATH = "$QtTools\mingw1310_64\bin;$QtTools\Ninja;$QtTools\CMake_64\bin;$QtDir\bin;" + $env:PATH
New-Item -ItemType Directory -Force $Out, $Work | Out-Null

function Log($m) { Write-Host "`n=== $m" }
function Run {
    # no named parameters: arguments such as -e must not bind to one
    $exe = $args[0]
    $rest = @($args | Select-Object -Skip 1)
    & $exe @rest
    if ($LASTEXITCODE -ne 0) { throw "$exe failed with exit code $LASTEXITCODE" }
}

# ---- SavvyCAN (Qt 6, CMake) with the Triple connection and tools ----------
$Src = "$Work\SavvyCAN"
if (-not (Test-Path "$Src\.git")) {
    Log "cloning SavvyCAN from $SavvyCanGit"
    Run git clone -q $SavvyCanGit $Src
}
Log "SavvyCAN at $SavvyCanRev with gui/savvycan applied"
Run git -C $Src checkout -q -f $SavvyCanRev
Run git -C $Src clean -q -fdx -e build
Run python "$Repo\gui\savvycan\apply.py" $Src | Out-Null
Log "building SavvyCAN"
Run cmake -S $Src -B "$Src\build" -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_PREFIX_PATH=$QtDir" | Out-Null
Run cmake --build "$Src\build" -j $env:NUMBER_OF_PROCESSORS | Out-Null

Log "canre_cli"
New-Item -ItemType Directory -Force "$Work\bin" | Out-Null
Run g++ -std=c++17 -O2 -static -I "$Repo\gui\savvycan\re" -o "$Work\bin\canre_cli.exe" "$Repo\tools\canre\canre_cli.cpp"
Run strip "$Work\bin\canre_cli.exe"

# ---- stage ------------------------------------------------------------------
Log "staging $Name"
if (Test-Path "$Work\stage") { Remove-Item -Recurse -Force "$Work\stage" }
New-Item -ItemType Directory -Force "$Stage\SavvyCAN", "$Stage\tools", "$Stage\doc\img", "$Stage\licenses" | Out-Null
Copy-Item "$Src\build\SavvyCAN.exe" "$Stage\SavvyCAN\"
Run strip "$Stage\SavvyCAN\SavvyCAN.exe"
Run windeployqt --release --compiler-runtime --no-translations --no-system-d3d-compiler --no-opengl-sw "$Stage\SavvyCAN\SavvyCAN.exe" | Out-Null

Copy-Item "$Work\bin\canre_cli.exe" "$Stage\tools\"
foreach ($f in "triple_proto.py", "triple_dump.py", "canopen_sim.py", "canopen_sim.eds", "re_demo.py") {
    Copy-Item "$Repo\tools\$f" "$Stage\tools\"
}
Set-Content -Encoding ascii "$Stage\tools\requirements.txt" "pyserial"
Copy-Item "$Repo\README.md" "$Stage\"
foreach ($f in "savvycan-guide.md", "reverse-engineering.md", "linux-driver.md", "protocol.md") {
    Copy-Item "$Repo\doc\$f" "$Stage\doc\"
}
Copy-Item "$Repo\doc\img\*.png" "$Stage\doc\img\"
Copy-Item "$Repo\LICENSE" "$Stage\licenses\LICENSE.usb2can-triple.txt"
Copy-Item "$Repo\LICENSES\*" "$Stage\licenses\"
Copy-Item "$Src\LICENSE" "$Stage\licenses\LICENSE.SavvyCAN.txt"
Copy-Item "$QtDir\..\Src\LICENSES\LGPL-3.0-only.txt" "$Stage\licenses\LICENSE.Qt-LGPL-3.0.txt"

$notice = @"
USB2CAN Triple software $Version for Windows
============================================

SavvyCAN\       SavvyCAN (https://github.com/collin80/SavvyCAN, MIT license)
                at QT6WIP commit $SavvyCanRev with the USB2CAN Triple connection
                and tools of https://github.com/canlab-cz/triple (MIT).
tools\          canre_cli.exe and Python tools (MIT).
doc\            documentation.

Third-party components
----------------------
Qt $((Split-Path $QtDir -Parent | Split-Path -Leaf)) (Qt6*.dll and the plugin folders in SavvyCAN\): The Qt
Company and other contributors, used under the GNU Lesser General Public
License version 3 (licenses\LICENSE.Qt-LGPL-3.0.txt). The source code is
available from https://download.qt.io/official_releases/qt/. You may replace
the Qt DLLs with other builds of the same Qt version.

MinGW-w64 runtime (libgcc_s_seh-1.dll, libstdc++-6.dll, libwinpthread-1.dll):
GCC runtime libraries under the GNU GPL version 3 with the GCC Runtime
Library Exception, and the MinGW-w64 winpthreads library under its MIT-style
license. https://www.mingw-w64.org

License texts of this project and SavvyCAN are in licenses\.
"@
Set-Content -Encoding utf8 "$Stage\LICENSES-WINDOWS.txt" $notice

$readme = @"
USB2CAN Triple software $Version for Windows
============================================

SavvyCAN
  Start SavvyCAN\SavvyCAN.exe. Connection > Open Connection Window >
  Add New Device Connection > "CANLAB USB2CAN Triple (serial)", pick the
  adapter's COM port, Create New Connection. The adapter needs no driver
  (Windows' built-in usbser.sys). User guide: doc\savvycan-guide.md.

Reverse engineering
  RE Tools > Signal Finder and AI Assistant in SavvyCAN; guide:
  doc\reverse-engineering.md. Command line: tools\canre_cli.exe, e.g.
    python tools\re_demo.py demo.log
    tools\canre_cli.exe demo.log analyze_id id=0C9

Python tools (Python 3 from python.org; pip install -r tools\requirements.txt)
  tools\triple_dump.py COM7          print frames without SavvyCAN
  tools\re_demo.py demo.log          simulated vehicle traffic (load it in SavvyCAN)
  tools\canopen_sim.py               CANopen node simulator (Linux SocketCAN only)

The Linux driver is not part of this package; see doc\linux-driver.md and
https://github.com/canlab-cz/triple.
"@
Set-Content -Encoding utf8 "$Stage\README-WINDOWS.txt" $readme

# ---- zip ---------------------------------------------------------------------
Log "zip"
$Zip = "$Out\$Name.zip"
if (Test-Path $Zip) { Remove-Item -Force $Zip }
Compress-Archive -Path $Stage -DestinationPath $Zip
Get-Item $Zip | Select-Object Name, Length | Format-Table -AutoSize | Out-String | Write-Host

# ---- installer ---------------------------------------------------------------
if (-not $Iscc) {
    foreach ($c in "$env:ISCC", "C:\Program Files (x86)\Inno Setup 6\ISCC.exe", "C:\Program Files\Inno Setup 6\ISCC.exe",
                   "C:\Program Files (x86)\Inno Setup 7\ISCC.exe", "C:\Program Files\Inno Setup 7\ISCC.exe",
                   "C:\MentorGraphics\Inno Setup 7\ISCC.exe") {
        if ($c -and (Test-Path $c)) { $Iscc = $c; break }
    }
}
if ($Iscc) {
    Log "installer with $Iscc"
    # the script contains Czech texts: Inno Setup wants UTF-8 with a BOM
    $iss = "$Work\usb2can-triple.iss"
    Get-Content -Encoding utf8 "$PSScriptRoot\usb2can-triple.iss" | Out-File -Encoding utf8 $iss
    Run $Iscc /Q "/DAppVersion=$Version" "/DStageDir=$Stage" "/DOutDir=$Out" "/DIconFile=$Src\icons\SavvyIcon.ico" $iss
    Get-Item "$Out\$Name-setup.exe" | Select-Object Name, Length | Format-Table -AutoSize | Out-String | Write-Host
} else {
    Write-Host "Inno Setup (ISCC.exe) not found: no installer. Pass -Iscc <path> or set ISCC."
}
