$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Split-Path -Parent $ScriptDir
$ExePath = Join-Path $ProjectRoot ".build\output\Release\Codex-Quota-Bar.exe"

if (-not (Test-Path $ExePath)) {
    Write-Host "Executable not found. Running build.ps1 first..." -ForegroundColor Yellow
    & (Join-Path $ScriptDir "build.ps1")
}

Write-Host "Starting Codex-Quota-Bar 2.5.2 (Direct2D Native)..." -ForegroundColor Green
Start-Process -FilePath $ExePath
