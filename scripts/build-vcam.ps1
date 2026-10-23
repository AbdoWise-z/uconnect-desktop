# Builds the uConnect Camera media source (vcam/windows) with MSVC, runs its
# test, and copies uconnect-vcam.dll and its installer to -Output (by default
# <build directory>\vcam, where a development build of the app looks when
# UCONNECT_VCAM_DIR points there).
#
#   powershell -File scripts/build-vcam.ps1 [-Output <dir>]
param(
    [string]$BuildDirectory = 'build-vcam',
    [string]$Output = ''
)

$ErrorActionPreference = 'Stop'
$projectDirectory = Split-Path -Parent $PSScriptRoot
$buildPath = Join-Path $projectDirectory $BuildDirectory
if (-not $Output) { $Output = Join-Path $buildPath 'vcam' }

$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) { throw 'cmake is not on PATH.' }

& $cmake -S (Join-Path $projectDirectory 'vcam/windows') -B $buildPath -G 'Visual Studio 17 2022' -A x64
if ($LASTEXITCODE -ne 0) { throw 'Configuring the virtual camera failed (needs Visual Studio 2022 with C++).' }
& $cmake --build $buildPath --config Release
if ($LASTEXITCODE -ne 0) { throw 'Building the virtual camera failed.' }
& (Join-Path $buildPath 'Release/vcam-source-test.exe') (Join-Path $buildPath 'Release/uconnect-vcam.dll')
if ($LASTEXITCODE -ne 0) { throw 'The virtual camera test failed.' }

New-Item -ItemType Directory -Path $Output -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $buildPath 'Release/uconnect-vcam.dll') -Destination $Output -Force
Copy-Item -LiteralPath (Join-Path $projectDirectory 'vcam/windows/install-vcam.ps1') -Destination $Output -Force
Write-Output $Output
