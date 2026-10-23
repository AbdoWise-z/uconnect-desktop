param(
    [string]$BuildDirectory = 'cmake-build-pkg',
    [string]$Label = 'test',
    # The uconnect-vdd driver package (UConnectVDD.inf, .dll, .cat): a release
    # or CI artifact of github.com/AbdoWise-z/uconnect-vdd, or a local build's
    # x64\Release\UConnectVDD. Without it the package has no extended display.
    [string]$DriverPackage = '',
    # Leave out the uConnect Camera component (needs Visual Studio 2022 to build).
    [switch]$SkipVirtualCamera
)

$ErrorActionPreference = 'Stop'
$projectDirectory = Split-Path -Parent $PSScriptRoot
$buildPath = (Resolve-Path -LiteralPath (Join-Path $projectDirectory $BuildDirectory)).Path
$executable = Join-Path $buildPath 'uconnect_desktop.exe'
if (-not (Test-Path -LiteralPath $executable)) {
    throw 'Build uconnect_desktop in Release mode before packaging.'
}
if ($Label -notmatch '^[A-Za-z0-9._-]+$') {
    throw 'The package label may contain only letters, numbers, dots, dashes and underscores.'
}

$distPath = Join-Path $projectDirectory 'dist'
$packageName = "uConnect-0.2.0-$Label-win64"
$packagePath = Join-Path $distPath $packageName
$archivePath = "$packagePath.zip"
if ((Test-Path -LiteralPath $packagePath) -or (Test-Path -LiteralPath $archivePath)) {
    throw "Package already exists: $packageName. Choose another label."
}
New-Item -ItemType Directory -Path $packagePath -Force | Out-Null
Copy-Item -LiteralPath $executable -Destination (Join-Path $packagePath 'uConnect.exe')
Get-ChildItem -LiteralPath $buildPath -Filter '*.dll' -File |
    Where-Object Name -NotMatch '^(Qt6Test|openh264-|libopenh264-)' |
    Copy-Item -Destination $packagePath
foreach ($directory in @('generic', 'iconengines', 'imageformats', 'networkinformation', 'platforms', 'qml', 'tls', 'multimedia')) {
    $sourcePath = Join-Path $buildPath $directory
    if (Test-Path -LiteralPath $sourcePath) {
        Copy-Item -LiteralPath $sourcePath -Destination $packagePath -Recurse
    }
}
# The rendezvous server, beside the app (it uses the same compiler runtime).
foreach ($file in @('uconnect-rendezvous.exe', 'rendezvous.example.yaml')) {
    $sourcePath = Join-Path $buildPath $file
    if (-not (Test-Path -LiteralPath $sourcePath)) { throw "The build has no $file (configure with UCD_BUILD_SERVER=ON)." }
    Copy-Item -LiteralPath $sourcePath -Destination $packagePath
}
Copy-Item -LiteralPath (Join-Path $projectDirectory 'packaging/SERVER.txt') -Destination $packagePath
foreach ($notice in @('third-party-screen.md', 'FFmpeg-LICENSE.txt', 'GPL-3.0.txt', 'OpenH264-BINARY-LICENSE.txt')) {
    $sourcePath = Join-Path $buildPath $notice
    if (Test-Path -LiteralPath $sourcePath) {
        Copy-Item -LiteralPath $sourcePath -Destination $packagePath
    }
}
if ($DriverPackage) {
    foreach ($file in @('UConnectVDD.inf', 'UConnectVDD.dll', 'uconnectvdd.cat')) {
        if (-not (Test-Path -LiteralPath (Join-Path $DriverPackage $file))) { throw "The driver package has no $file." }
    }
    $driverPath = Join-Path $packagePath 'drivers/uconnect-vdd'
    New-Item -ItemType Directory -Path $driverPath -Force | Out-Null
    Get-ChildItem -LiteralPath $DriverPackage -File |
        Where-Object Extension -In '.inf', '.dll', '.cat', '.cer', '.txt' |
        Copy-Item -Destination $driverPath
} else {
    Write-Warning 'No -DriverPackage: extended displays will ask for a driver this package does not have.'
}
# The uConnect Camera: its media source and installer, which the app runs
# elevated the first time a camera is used as a webcam.
if ($SkipVirtualCamera) {
    Write-Warning 'No virtual camera: watched cameras cannot be used as webcams with this package.'
} else {
    & (Join-Path $PSScriptRoot 'build-vcam.ps1') -Output (Join-Path $packagePath 'vcam') | Out-Null
}
@'
[Paths]
Prefix=.
Plugins=.
QmlImports=qml
'@ | Set-Content -LiteralPath (Join-Path $packagePath 'qt.conf') -Encoding ASCII
Copy-Item -LiteralPath (Join-Path $projectDirectory 'packaging/TESTING.txt') -Destination (Join-Path $packagePath 'README.txt')
$revision = & git -C $projectDirectory rev-parse HEAD
if ($LASTEXITCODE -ne 0) { throw 'Cannot determine the source revision.' }
@(
    "Source revision: $revision"
    'Platform: Windows x64'
    'Configuration: Release'
    'Qt runtime and compiler runtime are included.'
) | Set-Content -LiteralPath (Join-Path $packagePath 'BUILD.txt') -Encoding UTF8

Compress-Archive -LiteralPath $packagePath -DestinationPath $archivePath -CompressionLevel Optimal
Get-FileHash -LiteralPath $archivePath -Algorithm SHA256 |
    ForEach-Object { "$($_.Hash.ToLower())  $([IO.Path]::GetFileName($archivePath))" } |
    Set-Content -LiteralPath "$archivePath.sha256" -Encoding ASCII
Write-Output $archivePath
