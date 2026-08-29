$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Split-Path -Parent $ScriptDir
$ExePath = Join-Path $ProjectRoot ".build\output\Release\Codex-Quota-Bar.exe"

function Get-TargetProcesses {
    $targetPath = [IO.Path]::GetFullPath($ExePath)
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

$script:Failures = 0
$ConfigFile = Join-Path $env:LOCALAPPDATA "Codex-Quota-Bar\data\config-users.json"
$DefaultConfigFile = Join-Path (Split-Path -Parent $ExePath) "config-default.json"
$backupId = [Guid]::NewGuid().ToString("N")
$ConfigBackup = "$ConfigFile.$backupId.cqbbak"
$ConfigExisted = Test-Path -LiteralPath $ConfigFile
$ConfigBackupPrepared = $false
$PipeName = "Codex-Quota-Bar_Pipe_$((Get-Process -Id $PID).SessionId)"
$targetFullPath = [IO.Path]::GetFullPath($ExePath)
$ExternalInstances = @(Get-Process -ErrorAction SilentlyContinue | ForEach-Object {
    try {
        $externalName = if ($_.Path) { [IO.Path]::GetFileName($_.Path) } else { "" }
        if ($externalName -eq "Codex-Quota-Bar.exe" -and
            -not [string]::Equals(
                [IO.Path]::GetFullPath($_.Path), $targetFullPath,
                [StringComparison]::OrdinalIgnoreCase)) {
            [pscustomobject]@{ Id = $_.Id; Path = [IO.Path]::GetFullPath($_.Path) }
        }
    } catch {}
})
$PreviousDisableLog = $env:CODEX_QUOTA_DISABLE_LOG
$env:CODEX_QUOTA_DISABLE_LOG = "1"

try {
foreach ($externalPath in @($ExternalInstances.Path | Sort-Object -Unique)) {
    Write-Host "Temporarily stopping installed instance: $externalPath" -ForegroundColor Yellow
    $stopper = Start-Process -FilePath $externalPath -ArgumentList '"--exit"' `
        -Wait -PassThru -ErrorAction SilentlyContinue
}
Start-Sleep -Milliseconds 500
foreach ($external in $ExternalInstances) {
    $stillRunning = Get-Process -Id $external.Id -ErrorAction SilentlyContinue
    if ($stillRunning) { $stillRunning | Stop-Process -Force -ErrorAction SilentlyContinue }
}

# 每次都从当前源码重建，避免已有 dist/Release 产物造成假阳性。
Write-Host "Building executable from current sources..." -ForegroundColor Yellow
& (Join-Path $ScriptDir "build.ps1")
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $ExePath)) {
    throw "Build failed; aborting tests."
}
if (-not (Test-Path -LiteralPath $DefaultConfigFile)) {
    throw "Default configuration was not copied beside the application."
}
$defaultConfig = Get-Content -LiteralPath $DefaultConfigFile -Raw | ConvertFrom-Json
if ($defaultConfig.Settings.Appearance.Colors.Surface -ne "#FFFFFF" -or
    $defaultConfig.Settings.Appearance.BackgroundTransparency -ne 0) {
    throw "Default configuration content is incomplete."
}

# 清理上次运行可能残留的实例（避免单实例互斥体干扰本套件）
$leftover = @(Get-TargetProcesses)
if ($leftover.Count -gt 0) {
    Write-Host "Stopping leftover instance from previous run..." -ForegroundColor Yellow
    if (Test-Path $ExePath) { & $ExePath --exit | Out-Null }
    Start-Sleep -Milliseconds 500
    $leftover = @($leftover | Where-Object { -not $_.HasExited })
    if ($leftover.Count -gt 0) { $leftover | Stop-Process -Force -ErrorAction SilentlyContinue }
    Start-Sleep -Milliseconds 300
}

# 以完整参数串启动并断言退出码（GUI 子系统程序经 & 调用不等待，须用 Start-Process -Wait）
function Expect-ExitCode {
    param(
        [string]$Name,
        [string]$ArgumentString,
        [int]$Expected
    )
    if ([string]::IsNullOrEmpty($ArgumentString)) {
        $p = Start-Process -FilePath $ExePath -Wait -PassThru
    } else {
        $p = Start-Process -FilePath $ExePath -ArgumentList $ArgumentString -Wait -PassThru
    }
    $code = $p.ExitCode
    if ($code -eq $Expected) {
        Write-Host "  [PASS] $Name -> exit $code" -ForegroundColor Green
    } else {
        Write-Host "  [FAIL] $Name -> expected $Expected, got $code" -ForegroundColor Red
        $script:Failures++
    }
}

Write-Host "==========================================" -ForegroundColor Cyan
Write-Host "  Codex-Quota-Bar C++ Direct2D Test Suite " -ForegroundColor Cyan
Write-Host "==========================================" -ForegroundColor Cyan

# 1. 使用真正的子进程/stdin/stdout 跑完整 App Server 协议回归测试。
# FakeCodexAppServer 会严格校验 initialize -> initialized -> rateLimits -> usage
# 的顺序，并提供完整、全 null、usage 失败三种响应。
Write-Host "[1/10] 官方 App Server 协议集成测试..." -ForegroundColor Yellow
$CMakeBuildDir = Join-Path $ProjectRoot ".build\tests"
$protocolTestsBuilt = $true
& cmake -S $ProjectRoot -B $CMakeBuildDir -DBUILD_TESTING=ON
if ($LASTEXITCODE -ne 0) {
    Write-Host "  [FAIL] CMake 测试工程配置失败" -ForegroundColor Red
    $script:Failures++
    $protocolTestsBuilt = $false
}
if ($protocolTestsBuilt) {
    & cmake --build $CMakeBuildDir --config Release --target FakeCodexAppServer CodexAppServerIntegrationTests HookConfigTests SimpleJsonTests AppearanceTests PipeServerTests
    if ($LASTEXITCODE -ne 0) {
        Write-Host "  [FAIL] App Server 测试程序构建失败" -ForegroundColor Red
        $script:Failures++
        $protocolTestsBuilt = $false
    }
}
if ($protocolTestsBuilt) {
    & ctest --test-dir $CMakeBuildDir -C Release --output-on-failure `
        -R "^(CodexAppServerIntegration|HookConfig|SimpleJson|Appearance|PipeServer)$"
    if ($LASTEXITCODE -eq 0) {
        Write-Host "  [PASS] App Server、IPC 安全、JSON 边界、Hook 与异常兼容路径" -ForegroundColor Green
    } else {
        Write-Host "  [FAIL] App Server 协议集成测试失败" -ForegroundColor Red
        $script:Failures++
    }
}

# 测试实例（尤其拖动与无实例 Hook 段）会写入窗口位置；
# 先备份用户配置，结束后原样恢复，避免测试数据污染真实状态。
if ($ConfigExisted) {
    Copy-Item $ConfigFile $ConfigBackup -Force
    Remove-Item -LiteralPath $ConfigFile -Force
}
$ConfigBackupPrepared = $true
# 写入用户配置，验证其与默认基线组合读取。
$ConfigDirectory = Split-Path -Parent $ConfigFile
New-Item -ItemType Directory -Path $ConfigDirectory -Force | Out-Null
Set-Content -LiteralPath $ConfigFile -Encoding utf8NoBOM -Value @'
{
  "Version": 2,
  "Settings": {
    "UserScale": 1.0,
    "CompanionMode": false,
    "RefreshIntervalMinutes": 5
  },
  "Window": { "X": 120, "Y": 80 }
}
'@

# 2. 启动
Write-Host "[2/10] 进程启动..." -ForegroundColor Yellow
$proc = Start-Process -FilePath $ExePath -PassThru
Start-Sleep -Milliseconds 800
if (-not $proc -or $proc.HasExited) {
    throw "Process failed to start or exited early."
}
Write-Host "  Process running (PID: $($proc.Id))" -ForegroundColor Green

try {
    $config = Get-Content -LiteralPath $ConfigFile -Raw | ConvertFrom-Json
    $configOk = $config.Version -eq 2 -and
                $config.Settings.UserScale -eq 1.0 -and
                $config.Settings.CompanionMode -eq $false -and
                $config.Settings.RefreshIntervalMinutes -eq 5 -and
                $config.Window.X -eq 120 -and $config.Window.Y -eq 80
    if ($configOk) {
        Write-Host "  [PASS] 用户配置与默认配置组合读取正常" -ForegroundColor Green
    } else {
        Write-Host "  [FAIL] 用户配置与默认配置组合读取不完整" -ForegroundColor Red
        $script:Failures++
    }
} catch {
    Write-Host "  [FAIL] 统一配置读取异常：$($_.Exception.Message)" -ForegroundColor Red
    $script:Failures++
}

# 3. 展开态主动拖动后，收起位置必须保持在拖动后的左上角。
Write-Host "[3/10] 展开拖动后收起位置..." -ForegroundColor Yellow
Add-Type @"
using System;
using System.Runtime.InteropServices;
public struct CqbPoint { public int X; public int Y; }
public struct CqbRect { public int Left; public int Top; public int Right; public int Bottom; }
public static class CqbNative {
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out CqbRect rect);
    [DllImport("user32.dll")] public static extern bool GetCursorPos(out CqbPoint point);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
}
"@

function New-CqbLParam([int]$X, [int]$Y) {
    [IntPtr]::new([int](($X -band 0xffff) -bor (($Y -band 0xffff) -shl 16)))
}

function Get-CqbWindowRect([IntPtr]$Hwnd) {
    $rect = [CqbRect]::new()
    if (-not [CqbNative]::GetWindowRect($Hwnd, [ref]$rect)) { throw "GetWindowRect failed" }
    $rect
}

$proc.Refresh()
$hwnd = [IntPtr]$proc.MainWindowHandle
$savedCursor = [CqbPoint]::new()
[void][CqbNative]::GetCursorPos([ref]$savedCursor)
try {
    if ($hwnd -eq [IntPtr]::Zero) { throw "Main window handle is unavailable" }

    $collapsedBefore = Get-CqbWindowRect $hwnd
    $uiScale = ($collapsedBefore.Right - $collapsedBefore.Left) / 380.0
    $expandX = [int][Math]::Round(366.0 * $uiScale)
    $expandY = [int][Math]::Round(76.0 * $uiScale)
    [void][CqbNative]::SendMessage($hwnd, 0x0201, [IntPtr]::new(1), (New-CqbLParam $expandX $expandY))
    [void][CqbNative]::SendMessage($hwnd, 0x0202, [IntPtr]::Zero, (New-CqbLParam $expandX $expandY))
    Start-Sleep -Milliseconds 150

    $expandedBeforeDrag = Get-CqbWindowRect $hwnd
    if (($expandedBeforeDrag.Bottom - $expandedBeforeDrag.Top) -le
        ($collapsedBefore.Bottom - $collapsedBefore.Top)) {
        throw "Window did not expand"
    }

    $startX = [int][Math]::Round(20.0 * $uiScale)
    $startY = [int][Math]::Round(15.0 * $uiScale)
    $screenStartX = $expandedBeforeDrag.Left + $startX
    $screenStartY = $expandedBeforeDrag.Top + $startY
    $deltaX = if ($expandedBeforeDrag.Left -gt 100) { -40 } else { 40 }
    $deltaY = if ($expandedBeforeDrag.Top -gt 100) { -30 } else { 30 }

    [void][CqbNative]::SetCursorPos($screenStartX, $screenStartY)
    Start-Sleep -Milliseconds 25
    [void][CqbNative]::SendMessage($hwnd, 0x0201, [IntPtr]::new(1), (New-CqbLParam $startX $startY))
    Start-Sleep -Milliseconds 25
    [void][CqbNative]::SetCursorPos($screenStartX + $deltaX, $screenStartY + $deltaY)
    Start-Sleep -Milliseconds 25
    [void][CqbNative]::SendMessage($hwnd, 0x0200, [IntPtr]::new(1), (New-CqbLParam ($startX + $deltaX) ($startY + $deltaY)))
    Start-Sleep -Milliseconds 25
    [void][CqbNative]::SendMessage($hwnd, 0x0202, [IntPtr]::Zero, (New-CqbLParam ($startX + $deltaX) ($startY + $deltaY)))
    Start-Sleep -Milliseconds 150
    $expandedAfterDrag = Get-CqbWindowRect $hwnd

    [void][CqbNative]::SendMessage($hwnd, 0x0201, [IntPtr]::new(1), (New-CqbLParam $expandX $expandY))
    [void][CqbNative]::SendMessage($hwnd, 0x0202, [IntPtr]::Zero, (New-CqbLParam $expandX $expandY))
    Start-Sleep -Milliseconds 150
    $collapsedAfter = Get-CqbWindowRect $hwnd

    $dragMoved = $expandedAfterDrag.Left -ne $expandedBeforeDrag.Left -or
                 $expandedAfterDrag.Top -ne $expandedBeforeDrag.Top
    $anchorKept = $collapsedAfter.Left -eq $expandedAfterDrag.Left -and
                  $collapsedAfter.Top -eq $expandedAfterDrag.Top
    if ($dragMoved -and $anchorKept) {
        Write-Host "  [PASS] 收起后保持展开态拖动位置" -ForegroundColor Green
    } else {
        Write-Host "  [FAIL] 展开拖动位置未保持（moved=$dragMoved, anchor=$anchorKept）" -ForegroundColor Red
        $script:Failures++
    }
} catch {
    Write-Host "  [FAIL] 展开拖动回归测试异常：$($_.Exception.Message)" -ForegroundColor Red
    $script:Failures++
} finally {
    [void][CqbNative]::SetCursorPos($savedCursor.X, $savedCursor.Y)
}

# 4. 单实例互斥：第二实例裸启动应返回 0
Write-Host "[4/10] 单实例互斥..." -ForegroundColor Yellow
Expect-ExitCode "第二实例裸启动" "" 0

# 5. 合法命令
Write-Host "[5/10] 合法 IPC 命令..." -ForegroundColor Yellow
Expect-ExitCode "--worker BUSY" '"--worker" "BUSY"' 0
Expect-ExitCode "--worker STATUS 自然分参" '"--worker" "STATUS" "Compiling" "project..."' 0
Expect-ExitCode "--worker DONE" '"--worker" "DONE"' 0
Expect-ExitCode "--worker IDLE" '"--worker" "IDLE"' 0
Expect-ExitCode "--worker TOGGLE" '"--worker" "TOGGLE"' 0
Expect-ExitCode "--worker STATS 自然分参" '"--worker" "STATS" "12.5B" "2.1B" "10h45m" "25d"' 0
Expect-ExitCode "--worker REFRESH" '"--worker" "REFRESH"' 0
Expect-ExitCode "--refresh" '"--refresh"' 0
Expect-ExitCode "--toggle" '"--toggle"' 0
Expect-ExitCode "--show" '"--show"' 0
Expect-ExitCode "--hide" '"--hide"' 0
Expect-ExitCode "--hook SessionStart（已有实例）" '"--hook" "SessionStart"' 0
Expect-ExitCode "--hook Stop（完成一轮对话）" '"--hook" "Stop"' 0
Expect-ExitCode "--hook SessionEnd（结束会话）" '"--hook" "SessionEnd"' 0

# 6. 长消息（>4094 字节触发 ERROR_MORE_DATA 累积读取路径）
Write-Host "[6/10] 长 STATUS 消息..." -ForegroundColor Yellow
$longText = "汉" * 2040  # (7 + 2040 + 1) * 2 = 4096 字节，恰好超过单次读取上限
Expect-ExitCode "长 STATUS 消息 (4096 字节)" ('"--worker" "STATUS" "' + $longText + '"') 0
$longerText = "汉" * 3000  # (7 + 3000 + 1) * 2 = 6016 字节
Expect-ExitCode "长 STATUS 消息 (6016 字节)" ('"--worker" "STATUS" "' + $longerText + '"') 0
# 说明：服务端 64KB 命令上限（TOO_LONG 回复）无法经 CLI 覆盖测试——
# CreateProcess 命令行上限为 32767 字符（UTF-16 约 65534 字节），刚好低于 64KB。

# 7. 客户端发送命令后不读取回复，不得占住唯一服务线程。
Write-Host "[7/10] 管道客户端不读取回复..." -ForegroundColor Yellow
$stalledClient = New-Object System.IO.Pipes.NamedPipeClientStream(
    ".", $PipeName, [System.IO.Pipes.PipeDirection]::InOut)
try {
    $stalledClient.Connect(1000)
    $commandBytes = [Text.Encoding]::Unicode.GetBytes("REFRESH`0")
    $stalledClient.Write($commandBytes, 0, $commandBytes.Length)
    $stalledClient.Flush()
    Start-Sleep -Milliseconds 200
    Expect-ExitCode "未读取前一回复时第二客户端仍可用" '"--refresh"' 0
} finally {
    $stalledClient.Dispose()
}

# 完全不写入数据的连接也必须在服务端 I/O 超时后释放，避免阻塞后续 Hook。
$silentClient = New-Object System.IO.Pipes.NamedPipeClientStream(
    ".", $PipeName, [System.IO.Pipes.PipeDirection]::InOut)
try {
    $silentClient.Connect(1000)
    $timer = [Diagnostics.Stopwatch]::StartNew()
    $refreshAfterSilent = Start-Process -FilePath $ExePath -ArgumentList '"--refresh"' `
        -Wait -PassThru
    $timer.Stop()
    if ($refreshAfterSilent.ExitCode -eq 0 -and $timer.ElapsedMilliseconds -lt 3500) {
        Write-Host "  [PASS] 空连接超时后 IPC 自动恢复（$($timer.ElapsedMilliseconds)ms）" -ForegroundColor Green
    } else {
        Write-Host "  [FAIL] 空连接阻塞 IPC（exit=$($refreshAfterSilent.ExitCode), $($timer.ElapsedMilliseconds)ms）" -ForegroundColor Red
        $script:Failures++
    }
} finally {
    $silentClient.Dispose()
}

# 8. 错误路径
Write-Host "[8/10] 错误路径退出码..." -ForegroundColor Yellow
Expect-ExitCode "--worker BOGUS (未知命令)" '"--worker" "BOGUS"' 1
Expect-ExitCode "--hook BOGUS (未知事件)" '"--hook" "BOGUS"' 1
Expect-ExitCode "--garbage (未知参数)" '"--garbage"' 1

# 9. EXIT 优雅退出
Write-Host "[9/10] EXIT 优雅退出..." -ForegroundColor Yellow
Expect-ExitCode "--exit" '"--exit"' 0
Start-Sleep -Milliseconds 800
if (@(Get-TargetProcesses).Count -gt 0) {
    Write-Host "  [FAIL] 退出后进程仍在运行" -ForegroundColor Red
    $script:Failures++
} else {
    Write-Host "  [PASS] 退出后进程已结束" -ForegroundColor Green
}
Expect-ExitCode "退出后 --refresh 应失败" '"--refresh"' 1

# 10. 无实例时，SessionStart Hook 应快速返回并派生长驻实例。
# 使用 stdio FakeCodexAppServer，覆盖真实 App Server 进程而不触碰用户配置。
Write-Host "[10/10] SessionStart Hook 无实例启动..." -ForegroundColor Yellow
$fakeCodexPath = Join-Path $CMakeBuildDir "Release\FakeCodexAppServer.exe"
$oldCodexPath = $env:CODEX_QUOTA_CODEX_PATH
$oldScenario = $env:CODEX_QUOTA_FAKE_SCENARIO
$appB = $null
try {
    if (-not (Test-Path -LiteralPath $fakeCodexPath)) {
        Write-Host "  [FAIL] FakeCodexAppServer 测试程序不存在" -ForegroundColor Red
        $script:Failures++
    } else {
        $env:CODEX_QUOTA_CODEX_PATH = $fakeCodexPath
        $env:CODEX_QUOTA_FAKE_SCENARIO = "happy"
        # 由 SessionStart Hook 在无实例时派生长驻 GUI。外层 Hook 必须在
        # SessionEnd 的官方 3 秒上限内快速退出，同时子进程继承测试环境。
        $hookTimer = [Diagnostics.Stopwatch]::StartNew()
        $hookProcess = Start-Process -FilePath $ExePath -ArgumentList '"--hook" "SessionStart"' -PassThru
        # Start-Process -Wait 在 Windows 上会等待整个派生进程树；这里必须像
        # Codex Hook 运行器一样，只等待直接命令进程。
        $hookExited = $hookProcess.WaitForExit(3000)
        $hookTimer.Stop()
        $hookExitCode = if ($hookExited) { $hookProcess.ExitCode } else { -1 }
        if (-not $hookExited -or $hookExitCode -ne 0 -or $hookTimer.ElapsedMilliseconds -ge 3000) {
            Write-Host "  [FAIL] 无实例 Hook 未快速返回（exit=$hookExitCode, $($hookTimer.ElapsedMilliseconds)ms）" -ForegroundColor Red
            $script:Failures++
        } else {
            Write-Host "  [PASS] 无实例 Hook 快速返回并派生同步进程（$($hookTimer.ElapsedMilliseconds)ms）" -ForegroundColor Green
        }

        $hostDeadline = (Get-Date).AddSeconds(5)
        while ((Get-Date) -lt $hostDeadline) {
            $hosts = @(Get-TargetProcesses)
            if ($hosts.Count -gt 0) {
                $appB = $hosts[0]
                break
            }
            Start-Sleep -Milliseconds 100
        }
        if (-not $appB -or $appB.HasExited) {
            Write-Host "  [FAIL] SessionStart Hook 未启动长驻实例" -ForegroundColor Red
            $script:Failures++
        } else {
            $pipeReady = $false
            $pipeDeadline = (Get-Date).AddSeconds(5)
            while ((Get-Date) -lt $pipeDeadline) {
                $refresh = Start-Process -FilePath $ExePath -ArgumentList '"--refresh"' -Wait -PassThru
                if ($refresh.ExitCode -eq 0) {
                    $pipeReady = $true
                    break
                }
                Start-Sleep -Milliseconds 100
            }
            if ($pipeReady) {
                Write-Host "  [PASS] App Server 实例管道已就绪" -ForegroundColor Green
            } else {
                Write-Host "  [FAIL] App Server 实例管道未在 5 秒内就绪" -ForegroundColor Red
                $script:Failures++
            }
        }
    }
} finally {
    if ($appB -and -not $appB.HasExited) {
        & $ExePath --exit | Out-Null
        Start-Sleep -Milliseconds 500
        if (-not $appB.HasExited) { $appB | Stop-Process -Force -ErrorAction SilentlyContinue }
    }
    if ($null -eq $oldCodexPath) { Remove-Item Env:CODEX_QUOTA_CODEX_PATH -ErrorAction SilentlyContinue } else { $env:CODEX_QUOTA_CODEX_PATH = $oldCodexPath }
    if ($null -eq $oldScenario) { Remove-Item Env:CODEX_QUOTA_FAKE_SCENARIO -ErrorAction SilentlyContinue } else { $env:CODEX_QUOTA_FAKE_SCENARIO = $oldScenario }
}

} finally {
    if (Test-Path -LiteralPath $ExePath) {
        $stopper = Start-Process -FilePath $ExePath -ArgumentList '"--exit"' `
            -Wait -PassThru -ErrorAction SilentlyContinue
    }
    Start-Sleep -Milliseconds 300
    @(Get-TargetProcesses) | Stop-Process -Force -ErrorAction SilentlyContinue

    if ($ConfigBackupPrepared) {
        Remove-Item -LiteralPath $ConfigFile -Force -ErrorAction SilentlyContinue
        if ($ConfigExisted -and (Test-Path -LiteralPath $ConfigBackup)) {
            Move-Item -LiteralPath $ConfigBackup -Destination $ConfigFile -Force
        }
        if (-not $ConfigExisted) {
            $testDataDirectory = Split-Path -Parent $ConfigFile
            $testInstallRoot = Split-Path -Parent $testDataDirectory
            if ((Test-Path -LiteralPath $testDataDirectory) -and
                @(Get-ChildItem -LiteralPath $testDataDirectory -Force).Count -eq 0) {
                Remove-Item -LiteralPath $testDataDirectory -Force
            }
            if ((Test-Path -LiteralPath $testInstallRoot) -and
                @(Get-ChildItem -LiteralPath $testInstallRoot -Force).Count -eq 0) {
                Remove-Item -LiteralPath $testInstallRoot -Force
            }
        }
    }
    if ($null -eq $PreviousDisableLog) {
        Remove-Item Env:CODEX_QUOTA_DISABLE_LOG -ErrorAction SilentlyContinue
    } else {
        $env:CODEX_QUOTA_DISABLE_LOG = $PreviousDisableLog
    }
    foreach ($externalPath in @($ExternalInstances.Path | Sort-Object -Unique)) {
        if (Test-Path -LiteralPath $externalPath) {
            Start-Process -FilePath $externalPath -ErrorAction SilentlyContinue | Out-Null
        }
    }
}

Write-Host "==========================================" -ForegroundColor Cyan
if ($script:Failures -eq 0) {
    Write-Host "All test cases passed with 100% success!" -ForegroundColor Green
    exit 0
} else {
    Write-Host "$($script:Failures) test(s) FAILED." -ForegroundColor Red
    exit 1
}
