param(
    [string]$BuildDirectory = 'cmake-build-pkg',
    [string]$Label = 'test'
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
$packageName = "uConnect-0.1.0-$Label-win64"
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
foreach ($notice in @('third-party-screen.md', 'FFmpeg-LICENSE.txt', 'GPL-3.0.txt', 'OpenH264-BINARY-LICENSE.txt')) {
    $sourcePath = Join-Path $buildPath $notice
    if (Test-Path -LiteralPath $sourcePath) {
        Copy-Item -LiteralPath $sourcePath -Destination $packagePath
    }
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
