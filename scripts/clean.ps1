param(
    [switch]$IncludeDist
)

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = [IO.Path]::GetFullPath((Split-Path -Parent $ScriptDir)).TrimEnd('\', '/')
$targetNames = @(".build", "bin", "build", "build-tests", "cmake-build", "out")
if ($IncludeDist) {
    $targetNames += "dist"
}

$rootPrefix = $ProjectRoot + [IO.Path]::DirectorySeparatorChar
foreach ($name in $targetNames) {
    $target = [IO.Path]::GetFullPath((Join-Path $ProjectRoot $name))
    if (-not $target.StartsWith($rootPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to remove a path outside the project: $target"
    }
    if (Test-Path -LiteralPath $target) {
        Remove-Item -LiteralPath $target -Recurse -Force
        Write-Host "Removed: $target" -ForegroundColor DarkGray
    }
}

Write-Host $(if ($IncludeDist) { "All generated files were removed." } else { "Build caches were removed; dist was preserved." }) -ForegroundColor Green
