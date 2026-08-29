param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Split-Path -Parent $ScriptDir
$BuildDir = Join-Path $ProjectRoot ".build\app\$Configuration"
$OutputDir = Join-Path $ProjectRoot ".build\output\$Configuration"
$OutputFile = Join-Path $OutputDir "Codex-Quota-Bar.exe"
$DefaultConfigSource = Join-Path $ProjectRoot "config-default.json"
$DefaultConfigOutput = Join-Path $OutputDir "config-default.json"

New-Item -ItemType Directory -Path $BuildDir, $OutputDir -Force | Out-Null

function Get-TargetProcesses {
    param([string]$ExecutablePath)
    $targetPath = [IO.Path]::GetFullPath($ExecutablePath)
    @(Get-Process -ErrorAction SilentlyContinue | Where-Object {
        try {
            $_.Path -and [string]::Equals(
                [IO.Path]::GetFullPath($_.Path), $targetPath,
                [StringComparison]::OrdinalIgnoreCase)
        } catch {
            $false
        }
    })
}

$running = @(Get-TargetProcesses $OutputFile)
if ($running.Count -gt 0) {
    Write-Host "Stopping running instance gracefully..." -ForegroundColor Yellow
    & $OutputFile --exit
    $deadline = (Get-Date).AddSeconds(3)
    while ((Get-Date) -lt $deadline -and @($running | Where-Object { -not $_.HasExited }).Count -gt 0) {
        Start-Sleep -Milliseconds 200
    }
    @($running | Where-Object { -not $_.HasExited }) |
        Stop-Process -Force -ErrorAction SilentlyContinue
}

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path -LiteralPath $vswhere)) {
    throw "Visual Studio 2022 Build Tools were not found. Install MSVC and the Windows 11 SDK."
}

$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path -LiteralPath $vcvars)) {
    throw "MSVC x64 environment was not found."
}

$SourceFiles = Get-ChildItem (Join-Path $ProjectRoot "src") -Filter "*.cpp" -Recurse |
    ForEach-Object { '"' + $_.FullName + '"' }
$SourceArgs = $SourceFiles -join " "
$ResFile = Join-Path $BuildDir "app.res"
$ObjectPrefix = "$BuildDir\\"
$CompileFlags = if ($Configuration -eq "Debug") { "/Od /Zi /MTd" } else { "/O2 /MT" }

Write-Host "Building Codex-Quota-Bar 2.5.8 ($Configuration, Windows 11 x64/MSVC)..." -ForegroundColor Cyan
$command = @"
call "$vcvars" &&
cd /d "$ProjectRoot" &&
rc.exe /nologo /fo "$ResFile" "src\resources\app.rc" &&
cl.exe /nologo $CompileFlags /EHsc /std:c++20 /utf-8 /W4 /WX /permissive- /Zc:preprocessor /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /I"$ProjectRoot\src" /Fo:"$ObjectPrefix" /Fe:"$OutputFile" $SourceArgs "$ResFile" /link user32.lib gdi32.lib shell32.lib ole32.lib d2d1.lib dwrite.lib dwmapi.lib advapi32.lib /SUBSYSTEM:WINDOWS /MANIFEST:NO
"@
$command = $command -replace "`r?`n", " "
cmd.exe /d /c $command
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $OutputFile)) {
    throw "Application build failed."
}
Copy-Item -LiteralPath $DefaultConfigSource -Destination $DefaultConfigOutput -Force

Write-Host "Build successful: $OutputFile" -ForegroundColor Green
