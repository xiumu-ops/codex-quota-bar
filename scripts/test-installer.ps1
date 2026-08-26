param(
    [switch]$Ci
)

$ErrorActionPreference = "Stop"
if (-not $Ci -or $env:GITHUB_ACTIONS -ne "true") {
    throw "Installer integration tests are restricted to an ephemeral GitHub Actions runner."
}

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Split-Path -Parent $ScriptDir
$ReleaseDir = Join-Path $ProjectRoot "dist\Release"
$installRoot = Join-Path $env:LOCALAPPDATA "Codex-Quota-Bar"
$appDir = Join-Path $installRoot "app"
$dataDir = Join-Path $installRoot "data"
$appPath = Join-Path $appDir "Codex-Quota-Bar.exe"
$uninstallPath = Join-Path $appDir "Uninstall.exe"
$uninstallRegistry =
    "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\Codex-Quota-Bar"
$programs = [Environment]::GetFolderPath([Environment+SpecialFolder]::Programs)
$shortcut = Join-Path $programs "Codex-Quota-Bar.lnk"
$codexHome = Join-Path $env:RUNNER_TEMP "codex-quota-bar-installer-test-codex-home"
$hooksFile = Join-Path $codexHome "hooks.json"

$installers = @(
    Get-ChildItem $ReleaseDir -Filter "Codex-Quota-Bar_version_*.exe" `
        -File -ErrorAction Stop)
if ($installers.Count -ne 1) {
    throw "Expected exactly one release installer, found $($installers.Count)."
}

$preexistingState = @(@($installRoot, $uninstallRegistry, $shortcut) | Where-Object {
    Test-Path -LiteralPath $_
})
if ($preexistingState.Count -gt 0) {
    throw "The GitHub runner contains pre-existing installer state: $($preexistingState -join ', ')"
}

New-Item -ItemType Directory -Path $codexHome -Force | Out-Null
$previousCodexHome = $env:CODEX_HOME
$env:CODEX_HOME = $codexHome

try {
    Write-Host "Installing into the current-user layout..." -ForegroundColor Cyan
    $setup = Start-Process -FilePath $installers[0].FullName `
        -ArgumentList '/quiet' -Wait -PassThru
    if ($setup.ExitCode -ne 0) {
        throw "Installer exited with code $($setup.ExitCode)."
    }

    foreach ($requiredFile in $appPath, $uninstallPath, $shortcut, $hooksFile) {
        if (-not (Test-Path -LiteralPath $requiredFile)) {
            throw "Installer did not create: $requiredFile"
        }
    }
    if (Test-Path -LiteralPath $dataDir) {
        throw "The installer created mutable data before the application needed it."
    }
    if (-not (Test-Path -LiteralPath $uninstallRegistry)) {
        throw "The current-user uninstall registration was not created."
    }

    $registration = Get-ItemProperty -LiteralPath $uninstallRegistry
    if (-not [string]::Equals(
            [IO.Path]::GetFullPath($registration.InstallLocation),
            [IO.Path]::GetFullPath($appDir),
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "InstallLocation does not point to the app directory."
    }
    if (-not [string]::Equals(
            [IO.Path]::GetFullPath($registration.UserDataDir),
            [IO.Path]::GetFullPath($dataDir),
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "UserDataDir does not point to the data directory."
    }

    $hookContent = Get-Content -LiteralPath $hooksFile -Raw
    if ($hookContent -notmatch "Codex-Quota-Bar\.exe" -or
        $hookContent -notmatch "--hook") {
        throw "The installed Hook does not target Codex-Quota-Bar."
    }

    Write-Host "Uninstalling and validating cleanup..." -ForegroundColor Cyan
    $uninstall = Start-Process -FilePath $uninstallPath `
        -ArgumentList '/quiet' -Wait -PassThru
    if ($uninstall.ExitCode -notin 0, 3010) {
        throw "Uninstaller exited with code $($uninstall.ExitCode)."
    }

    if (Test-Path -LiteralPath $uninstallRegistry) {
        throw "The current-user uninstall registration was not removed."
    }
    if (Test-Path -LiteralPath $shortcut) {
        throw "The current-user Start menu shortcut was not removed."
    }
    if (Test-Path -LiteralPath $hooksFile) {
        $remainingHooks = Get-Content -LiteralPath $hooksFile -Raw
        if ($remainingHooks -match "Codex-Quota-Bar\.exe") {
            throw "The Codex lifecycle Hook was not removed."
        }
    }

    Write-Host "Installer integration tests passed." -ForegroundColor Green
} finally {
    if ($null -eq $previousCodexHome) {
        Remove-Item Env:CODEX_HOME -ErrorAction SilentlyContinue
    } else {
        $env:CODEX_HOME = $previousCodexHome
    }
}
