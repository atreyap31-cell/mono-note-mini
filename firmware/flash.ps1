# Build, flash and watch the Mono Note Mini.
#
#   .\flash.ps1              build, upload, then open the serial monitor
#   .\flash.ps1 -Build       build only
#   .\flash.ps1 -Monitor     just watch the serial output
#
# The monitor is where the first bring-up happens: every touch prints
# "touch x,y state=n" at 115200. Press the four corners - top-left should read
# near 0,0 and bottom-right near 199,199. If they are swapped or mirrored,
# every hit zone in the UI is wrong and the fix belongs in ft6336_bsp.
param(
    [switch]$Build,
    [switch]$Monitor,
    [string]$Port = ""
)

$ErrorActionPreference = "Stop"
$pio = "T:\pio-venv\Scripts\pio.exe"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

if (-not (Test-Path $pio)) { Write-Error "PlatformIO missing at $pio"; exit 1 }

# Toolchain and temp both live on T: - C: is the small drive on this machine.
$env:PLATFORMIO_CORE_DIR = "T:\.platformio"
$env:TMP = "T:\pip-tmp"; $env:TEMP = "T:\pip-tmp"

$portArg = @()
if ($Port) { $portArg = @("--upload-port", $Port) }

if ($Monitor) {
    & $pio device monitor -d $here -b 115200 @portArg
    exit $LASTEXITCODE
}

Write-Host "building..." -ForegroundColor Cyan
& $pio run -d $here -j 1
if ($LASTEXITCODE -ne 0) { Write-Error "build failed"; exit 1 }
if ($Build) { exit 0 }

# Show what is actually attached before trying to talk to it.
Write-Host "`nserial ports:" -ForegroundColor Cyan
& $pio device list -d $here

Write-Host "`nuploading..." -ForegroundColor Cyan
& $pio run -d $here -t upload @portArg
if ($LASTEXITCODE -ne 0) {
    Write-Warning "upload failed. Most common causes, in order:"
    Write-Warning "  1. a charge-only USB cable - it must carry data"
    Write-Warning "  2. the board needs BOOT held while it connects"
    Write-Warning "  3. another program is holding the port (close any serial monitor)"
    exit 1
}

Write-Host "`nflashed. opening the monitor - Ctrl+C to stop.`n" -ForegroundColor Green
& $pio device monitor -d $here -b 115200 @portArg
