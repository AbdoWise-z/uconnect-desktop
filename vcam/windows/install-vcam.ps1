# Installs (or with -Uninstall removes) the uConnect Camera component: the
# media source Windows' camera service loads for uConnect's virtual camera.
# Needs administrator rights; uConnect runs it elevated from the camera viewer.
#
#   install-vcam.ps1 -Source <path to uconnect-vcam.dll>
#   install-vcam.ps1 -Uninstall
#
# The DLL goes to Program Files (the camera service, LocalService, cannot read
# a user's folders) and is registered there. ProgramData\uConnect, where
# uConnect keeps the frame channel, is made readable by the camera service.
param(
    [string]$Source,
    [switch]$Uninstall
)
$ErrorActionPreference = 'Stop'

$target  = Join-Path $env:ProgramFiles 'uConnect\VirtualCamera'
$dll     = Join-Path $target 'uconnect-vcam.dll'
$regsvr  = Join-Path $env:SystemRoot 'System32\regsvr32.exe'

function Invoke-Regsvr([string[]]$arguments) {
    $process = Start-Process $regsvr -ArgumentList $arguments -Wait -PassThru -WindowStyle Hidden
    return $process.ExitCode
}

# Frame Server keeps the DLL loaded while a camera app runs: stop it to let
# the file be replaced or removed (it starts again on demand).
function Copy-OrStopCameraService([scriptblock]$action) {
    try { & $action }
    catch {
        Stop-Service -Name FrameServer -Force -ErrorAction SilentlyContinue
        Start-Sleep -Milliseconds 500
        & $action
    }
}

if ($Uninstall) {
    if (Test-Path $dll) {
        Invoke-Regsvr @('/s', '/u', "`"$dll`"") | Out-Null
        Copy-OrStopCameraService { Remove-Item -Recurse -Force $target }
    }
    exit 0
}

if (-not $Source -or -not (Test-Path $Source)) {
    Write-Error "No component at '$Source'."
    exit 2
}
New-Item -ItemType Directory -Force $target | Out-Null
Copy-OrStopCameraService { Copy-Item -Force $Source $dll }
# Kept beside it, so uConnect can remove it later without its package.
Copy-Item -Force $PSCommandPath (Join-Path $target 'install-vcam.ps1')
$code = Invoke-Regsvr @('/s', "`"$dll`"")
if ($code -ne 0) { exit $code }

# LocalService (S-1-5-19) reads the frame channel under here; uConnect gives
# each channel file its own, narrower permissions.
$data = Join-Path $env:ProgramData 'uConnect'
New-Item -ItemType Directory -Force $data | Out-Null
& icacls.exe $data /grant '*S-1-5-19:(OI)(CI)RX' | Out-Null
exit 0
