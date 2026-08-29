param(
    [string]$CertificateThumbprint = $env:CODEX_QUOTA_SIGN_CERT_THUMBPRINT,
    [string]$TimestampUrl = "http://timestamp.digicert.com"
)

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Split-Path -Parent $ScriptDir
$Configuration = "Release"
$BuildDir = Join-Path $ProjectRoot ".build\installer"
$ReleaseDir = Join-Path $ProjectRoot "dist\Release"
$AppPath = Join-Path $ProjectRoot ".build\output\Release\Codex-Quota-Bar.exe"
$UninstallPath = Join-Path $BuildDir "Uninstall.exe"
$SetupPath = Join-Path $ReleaseDir "Codex-Quota-Bar_version_2.5.8.exe"
$HashPath = Join-Path $ReleaseDir "Codex-Quota-Bar_version_2.5.8.sha256"

Write-Host "Building Codex-Quota-Bar 2.5.8 application..." -ForegroundColor Cyan
& (Join-Path $ScriptDir "build.ps1") -Configuration $Configuration
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $AppPath)) {
    throw "Application build failed."
}

New-Item -ItemType Directory -Path $BuildDir, $ReleaseDir -Force | Out-Null

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    throw "Visual Studio Build Tools were not found. The installer build requires MSVC and rc.exe."
}

$vsPath = & $vswhere -latest -products * -property installationPath
$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) {
    throw "vcvars64.bat was not found."
}

$SignTool = $null
if (-not [string]::IsNullOrWhiteSpace($CertificateThumbprint)) {
    $windowsKits = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10\bin"
    $SignTool = Get-ChildItem -LiteralPath $windowsKits -Filter "signtool.exe" `
        -File -Recurse -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match '\\x64\\signtool\.exe$' } |
        Sort-Object FullName -Descending |
        Select-Object -First 1 -ExpandProperty FullName
    if (-not $SignTool) {
        throw "A signing certificate was requested, but the x64 signtool.exe was not found."
    }
}

function Invoke-CodeSign {
    param([Parameter(Mandatory = $true)][string]$Path)
    if (-not $SignTool) { return }
    & $SignTool sign /fd SHA256 /sha1 $CertificateThumbprint `
        /tr $TimestampUrl /td SHA256 $Path
    if ($LASTEXITCODE -ne 0) { throw "Authenticode signing failed: $Path" }
    & $SignTool verify /pa $Path
    if ($LASTEXITCODE -ne 0) { throw "Authenticode verification failed: $Path" }
}

if ($SignTool) {
    Invoke-CodeSign -Path $AppPath
} else {
    Write-Warning "No signing certificate thumbprint was supplied; release binaries will remain unsigned."
}

$uninstallRes = Join-Path $BuildDir "uninstall.res"
$setupRes = Join-Path $BuildDir "setup.res"
$uninstallObj = Join-Path $BuildDir "uninstall.obj"
$setupObj = Join-Path $BuildDir "setup.obj"
$hookConfigObj = Join-Path $BuildDir "HookConfig.obj"
$hookTestsObj = Join-Path $BuildDir "HookConfigTests.obj"
$hookTestsPath = Join-Path $BuildDir "HookConfigTests.exe"
$uninstallSource = Join-Path $ProjectRoot "installer\uninstall.cpp"
$setupSource = Join-Path $ProjectRoot "installer\setup.cpp"
$hookConfigSource = Join-Path $ProjectRoot "installer\HookConfig.cpp"
$hookTestsSource = Join-Path $ProjectRoot "tests\HookConfigTests.cpp"
$uninstallRc = Join-Path $ProjectRoot "installer\uninstall.rc"
$setupRc = Join-Path $ProjectRoot "installer\setup.rc"

Push-Location $ProjectRoot
try {
    $uninstallCommand = @"
call "$vcvars" &&
rc.exe /nologo /fo "$uninstallRes" "$uninstallRc" &&
cl.exe /nologo /c /O2 /MT /EHsc /std:c++20 /utf-8 /W4 /WX /permissive- /Zc:preprocessor /DUNICODE /D_UNICODE /I"$ProjectRoot\src" /Fo:"$hookConfigObj" "$hookConfigSource" &&
cl.exe /nologo /O2 /MT /EHsc /std:c++20 /utf-8 /W4 /WX /permissive- /Zc:preprocessor /DUNICODE /D_UNICODE /I"$ProjectRoot\src" /I"$ProjectRoot\installer" /Fo:"$hookTestsObj" /Fe:"$hookTestsPath" "$hookTestsSource" "$hookConfigObj" /link shell32.lib ole32.lib /SUBSYSTEM:CONSOLE &&
"$hookTestsPath" &&
cl.exe /nologo /O2 /MT /EHsc /std:c++20 /utf-8 /W4 /WX /permissive- /Zc:preprocessor /DUNICODE /D_UNICODE /Fo:"$uninstallObj" /Fe:"$UninstallPath" "$uninstallSource" "$hookConfigObj" "$uninstallRes" /link user32.lib shell32.lib ole32.lib advapi32.lib /SUBSYSTEM:WINDOWS /MANIFEST:NO
"@
    $uninstallCommand = $uninstallCommand -replace "`r?`n", " "
    cmd.exe /d /c $uninstallCommand
    if ($LASTEXITCODE -ne 0) {
        throw "Uninstaller compilation failed."
    }

    Invoke-CodeSign -Path $UninstallPath

    $setupCommand = @"
call "$vcvars" &&
rc.exe /nologo /fo "$setupRes" "$setupRc" &&
cl.exe /nologo /O2 /MT /EHsc /std:c++20 /utf-8 /W4 /WX /permissive- /Zc:preprocessor /DUNICODE /D_UNICODE /Fo:"$setupObj" /Fe:"$SetupPath" "$setupSource" "$hookConfigObj" "$setupRes" /link user32.lib shell32.lib ole32.lib advapi32.lib /SUBSYSTEM:WINDOWS /MANIFEST:NO
"@
    $setupCommand = $setupCommand -replace "`r?`n", " "
    cmd.exe /d /c $setupCommand
    if ($LASTEXITCODE -ne 0) {
        throw "Installer compilation failed."
    }

    Invoke-CodeSign -Path $SetupPath
} finally {
    Pop-Location
}

if (-not (Test-Path $SetupPath)) {
    throw "Installer output was not created."
}

$hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $SetupPath).Hash
Set-Content -LiteralPath $HashPath `
    -Value "$hash  Codex-Quota-Bar_version_2.5.8.exe" -Encoding ascii

$allowedReleaseFiles = @(
    "Codex-Quota-Bar_version_2.5.8.exe",
    "Codex-Quota-Bar_version_2.5.8.sha256"
)
Get-ChildItem -LiteralPath $ReleaseDir -File -ErrorAction SilentlyContinue |
    Where-Object {
        $_.Name -notin $allowedReleaseFiles -and
        $_.Name -like "Codex-Quota-Bar*"
    } |
    Remove-Item -Force

Write-Host "Installer build successful:" -ForegroundColor Green
Write-Host "  $SetupPath"
Write-Host "  SHA256: $hash"
