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
$defaultConfigPath = Join-Path $appDir "config-default.json"
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

function Assert-UninstallComplete([string]$targetAppDir, [string]$preservedFile) {
    $targetUninstaller = Join-Path $targetAppDir "Uninstall.exe"
    $uninstall = Start-Process -FilePath $targetUninstaller -ArgumentList '/quiet' -Wait -PassThru
    if ($uninstall.ExitCode -ne 0) {
        throw "Uninstaller exited with code $($uninstall.ExitCode)."
    }
    foreach ($target in $targetUninstaller, (Join-Path $targetAppDir "Codex-Quota-Bar.exe"),
             (Join-Path $targetAppDir "config-default.json"), $uninstallRegistry, $shortcut) {
        if (Test-Path -LiteralPath $target) {
            throw "Uninstall left behind: $target"
        }
    }
    if ($preservedFile) {
        if (-not (Test-Path -LiteralPath $preservedFile -PathType Leaf)) {
            throw "Uninstall removed a file it did not install: $preservedFile"
        }
    } elseif (Test-Path -LiteralPath $targetAppDir) {
        throw "Uninstall left an empty app directory: $targetAppDir"
    }
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

    foreach ($requiredFile in $appPath, $uninstallPath, $defaultConfigPath, $shortcut) {
        if (-not (Test-Path -LiteralPath $requiredFile)) {
            throw "Installer did not create: $requiredFile"
        }
    }
    if (Test-Path -LiteralPath $hooksFile) {
        throw "The installer unexpectedly created or touched Codex hooks.json."
    }
    if (Test-Path -LiteralPath $dataDir) {
        throw "The installer created mutable data before the application needed it."
    }
    $defaultConfig = Get-Content -LiteralPath $defaultConfigPath -Raw | ConvertFrom-Json
    if ($defaultConfig.Settings.Appearance.Mode -ne "Default" -or
        $defaultConfig.Settings.Appearance.Colors.Surface -ne "#FFFFFF") {
        throw "The installed default configuration is invalid."
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
    if ($registration.PSObject.Properties.Name -contains "HookFilePath") {
        throw "Uninstall registration unexpectedly contains HookFilePath."
    }

    Write-Host "Uninstalling and validating cleanup..." -ForegroundColor Cyan
    Assert-UninstallComplete $appDir

    Write-Host "Verifying uninstall from a custom location..." -ForegroundColor Cyan
    $customRoot = Join-Path $env:RUNNER_TEMP "custom uninstall test\Codex-Quota-Bar"
    $customApp = Join-Path $customRoot "app"
    $customData = Join-Path $customRoot "data"
    # Quiet setup follows an existing InstallLocation instead of opening the folder picker.
    New-Item -Path $uninstallRegistry -Force | Out-Null
    New-ItemProperty -LiteralPath $uninstallRegistry -Name InstallLocation `
        -Value $customApp -PropertyType String | Out-Null
    $setup = Start-Process -FilePath $installers[0].FullName -ArgumentList '/quiet' -Wait -PassThru
    if ($setup.ExitCode -ne 0 -or -not (Test-Path -LiteralPath (Join-Path $customApp "Uninstall.exe"))) {
        throw "Custom-location installer did not create the expected payload."
    }
    $customUninstaller = Join-Path $customApp "Uninstall.exe"
    $customExe = Join-Path $customApp "Codex-Quota-Bar.exe"
    $customConfig = Join-Path $customApp "config-default.json"
    $otherData = Join-Path $env:RUNNER_TEMP "data outside the installation"
    New-Item -ItemType Directory -Path $otherData | Out-Null
    $otherMarker = Join-Path $otherData "outside-marker.txt"
    Set-Content -LiteralPath $otherMarker -Value "preserve outside data"
    New-Item -ItemType Junction -Path $customData -Target $otherData | Out-Null
    try {
        $probe = Start-Process -FilePath $customUninstaller -ArgumentList '/quiet', '--remove-data' -Wait -PassThru
        if ($probe.ExitCode -ne 0) { throw "Could not start the junction test cleanup worker" }
        foreach ($protectedPath in $otherMarker, $customExe, $customUninstaller, $uninstallRegistry) {
            if (-not (Test-Path -LiteralPath $protectedPath)) {
                throw "Uninstall modified state after encountering a data junction: $protectedPath"
            }
        }
    } finally {
        $dataLink = Get-Item -LiteralPath $customData -Force -ErrorAction SilentlyContinue
        if ($dataLink -and ($dataLink.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            [IO.Directory]::Delete($customData)
        }
    }

    $fileLock = [IO.File]::Open($customConfig, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try {
        $probe = Start-Process -FilePath $customUninstaller -ArgumentList '/quiet' -Wait -PassThru
        if ($probe.ExitCode -ne 0) { throw "Could not start the file-lock test cleanup worker" }
        foreach ($protectedPath in $customExe, $customConfig, $customUninstaller, $uninstallRegistry) {
            if (-not (Test-Path -LiteralPath $protectedPath)) {
                throw "Uninstall removed managed files or registration after a lock: $protectedPath"
            }
        }
    } finally {
        $fileLock.Dispose()
    }

    New-Item -ItemType Directory -Path $customData | Out-Null
    $settingsMarker = Join-Path $customData "settings-marker.txt"
    Set-Content -LiteralPath $settingsMarker -Value "preserve"
    $unrelatedFile = Join-Path $customApp "personal-file.txt"
    Set-Content -LiteralPath $unrelatedFile -Value "not installed by the product"
    Assert-UninstallComplete $customApp $unrelatedFile
    if (-not (Test-Path -LiteralPath $settingsMarker)) {
        throw "Quiet uninstall removed the retained user settings."
    }
    if (Test-Path -LiteralPath $hooksFile) {
        throw "The uninstaller unexpectedly created Codex hooks.json."
    }

    Write-Host "Installer integration tests passed." -ForegroundColor Green
} finally {
    if ($null -eq $previousCodexHome) {
        Remove-Item Env:CODEX_HOME -ErrorAction SilentlyContinue
    } else {
        $env:CODEX_HOME = $previousCodexHome
    }
}
