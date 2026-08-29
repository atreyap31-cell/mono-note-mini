# Start the Mono Note Mini backend.
#   .\run.ps1                 small.en, GPU if available
#   .\run.ps1 -Model medium.en
param(
    [string]$Model = "small.en",
    [int]$Port = 8000,
    [string]$OllamaModel = "llama3.2:3b"
)

$venv = "T:\mnm-server-venv\Scripts\python.exe"
if (-not (Test-Path $venv)) {
    Write-Error "venv missing - see README.md, Setup"
    exit 1
}

# Keep model downloads and temp off C:, which is the small drive here.
$env:HF_HOME = "T:\hf-cache"
$env:TMP = "T:\pip-tmp"; $env:TEMP = "T:\pip-tmp"
$env:WHISPER_MODEL = $Model
$env:OLLAMA_MODEL = $OllamaModel
$env:PORT = "$Port"

$ip = (Get-NetIPAddress -AddressFamily IPv4 |
       Where-Object { $_.IPAddress -notlike "127.*" -and $_.IPAddress -notlike "169.254.*" } |
       Select-Object -First 1).IPAddress
Write-Host ""
Write-Host "  Put this in the device's API field:  http://${ip}:$Port" -ForegroundColor Green
Write-Host "  Health check:                        http://${ip}:$Port/health"
Write-Host ""

& $venv app.py
