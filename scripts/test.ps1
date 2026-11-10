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

function Fail-Test([string]$message) {
    Write-Host "  [FAIL] $message" -ForegroundColor Red
    Write-Host "::error::$message"
    $script:Failures++
}
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
    Write-Host "::error::Build failed; aborting tests."
    throw "Build failed; aborting tests."
}
if (-not (Test-Path -LiteralPath $DefaultConfigFile)) {
    Write-Host "::error::Default configuration was not copied beside the application."
    throw "Default configuration was not copied beside the application."
}
$defaultConfig = Get-Content -LiteralPath $DefaultConfigFile -Raw | ConvertFrom-Json
if ($defaultConfig.Settings.Appearance.Colors.Surface -ne "#FFFFFF" -or
    $defaultConfig.Settings.Appearance.BackgroundTransparency -ne 0 -or
    $defaultConfig.Settings.AlwaysOnTop -ne $true -or
    $defaultConfig.Settings.MiniMode -ne $false) {
    Write-Host "::error::Default configuration content is incomplete."
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
        Fail-Test "$Name -> expected $Expected, got $code"
    }
}

Write-Host "==========================================" -ForegroundColor Cyan
Write-Host "  Codex-Quota-Bar C++ Direct2D Test Suite " -ForegroundColor Cyan
Write-Host "==========================================" -ForegroundColor Cyan

# 1. 使用真正的子进程/stdin/stdout 跑完整 App Server 协议回归测试。
# FakeCodexAppServer 会严格校验 initialize -> initialized -> rateLimits -> usage
# 的顺序，并提供完整、全 null、usage 失败三种响应。
Write-Host "[1/9] 官方 App Server 协议集成测试..." -ForegroundColor Yellow
$CMakeBuildDir = Join-Path $ProjectRoot ".build\tests"
$protocolTestsBuilt = $true
& cmake -S $ProjectRoot -B $CMakeBuildDir -DBUILD_TESTING=ON
if ($LASTEXITCODE -ne 0) {
    Fail-Test "CMake 测试工程配置失败"
    $protocolTestsBuilt = $false
}
if ($protocolTestsBuilt) {
    & cmake --build $CMakeBuildDir --config Release --target FakeCodexAppServer CodexAppServerIntegrationTests SimpleJsonTests AppearanceTests PipeServerTests
    if ($LASTEXITCODE -ne 0) {
        Fail-Test "App Server 测试程序构建失败"
        $protocolTestsBuilt = $false
    }
}
if ($protocolTestsBuilt) {
    & ctest --test-dir $CMakeBuildDir -C Release --output-on-failure `
        -R "^(CodexAppServerIntegration|SimpleJson|Appearance|PipeServer)$"
    if ($LASTEXITCODE -eq 0) {
        Write-Host "  [PASS] App Server、IPC 安全、JSON 边界与异常兼容路径" -ForegroundColor Green
    } else {
        Fail-Test "App Server 协议集成测试失败"
    }
}

# 测试实例（如拖动与迷你栏缩放测试段）会写入窗口位置；
# 先备份用户配置，结束后原样恢复，避免测试数据污染真实状态。
if ($ConfigExisted) {
    Copy-Item $ConfigFile $ConfigBackup -Force
    Remove-Item -LiteralPath $ConfigFile -Force
}
$ConfigBackupPrepared = $true
# 写入用户配置，验证其与默认基线组合读取。
$ConfigDirectory = Split-Path -Parent $ConfigFile
New-Item -ItemType Directory -Path $ConfigDirectory -Force | Out-Null
Set-Content -LiteralPath $ConfigFile -Encoding utf8BOM -Value @'
{
  "Version": 2,
  "Settings": {
    "UserScale": 1.0,
    "CompanionMode": false,
    "AlwaysOnTop": true,
    "MiniMode": false,
    "RefreshIntervalMinutes": 5,
    "FutureSetting": { "Flag": true }
  },
  "Window": { "X": 120, "Y": 2000000000, "FutureWindowField": "preserve" },
  "FutureRoot": { "LargeInteger": 9007199254740993 }
}
'@

# 2. 启动
Write-Host "[2/9] 进程启动..." -ForegroundColor Yellow
$proc = Start-Process -FilePath $ExePath -PassThru
Start-Sleep -Milliseconds 800
if (-not $proc -or $proc.HasExited) {
    Write-Host "::error::Process failed to start or exited early."
    throw "Process failed to start or exited early."
}
Write-Host "  Process running (PID: $($proc.Id))" -ForegroundColor Green

try {
    $config = Get-Content -LiteralPath $ConfigFile -Raw | ConvertFrom-Json
    $configOk = $config.Version -eq 2 -and
                $config.Settings.UserScale -eq 1.0 -and
                $config.Settings.CompanionMode -eq $false -and
                $config.Settings.AlwaysOnTop -eq $true -and
                $config.Settings.MiniMode -eq $false -and
                $config.Settings.RefreshIntervalMinutes -eq 5 -and
                $config.Window.X -eq 120 -and $config.Window.Y -eq 2000000000
    if ($configOk) {
        Write-Host "  [PASS] 用户配置与默认配置组合读取正常" -ForegroundColor Green
    } else {
        Fail-Test "用户配置与默认配置组合读取不完整"
    }
} catch {
    Fail-Test "统一配置读取异常：$($_.Exception.Message)"
}

# 3. 展开态主动拖动后，收起位置必须保持在拖动后的左上角。
Write-Host "[3/9] 展开拖动后收起位置..." -ForegroundColor Yellow
Add-Type @"
using System;
using System.Runtime.InteropServices;
public struct CqbPoint { public int X; public int Y; }
public struct CqbRect { public int Left; public int Top; public int Right; public int Bottom; }
public struct CqbGuiThreadInfo {
    public uint Size, Flags;
    public IntPtr Active, Focus, Capture, MenuOwner, MoveSize, Caret;
    public CqbRect CaretRect;
}
public static class CqbNative {
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll", EntryPoint="PostMessageW")] public static extern bool PostMessage(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string className, string windowName);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);
    [DllImport("user32.dll", EntryPoint="GetWindowLongPtrW")] public static extern IntPtr GetWindowLongPtr(IntPtr hWnd, int nIndex);
    [DllImport("user32.dll", EntryPoint="GetClassLongPtrW")] public static extern IntPtr GetClassLongPtr(IntPtr hWnd, int nIndex);
    [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, IntPtr processId);
    [DllImport("user32.dll")] public static extern bool GetGUIThreadInfo(uint threadId, ref CqbGuiThreadInfo info);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out CqbRect rect);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr hWnd, IntPtr insertAfter, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] private static extern IntPtr WindowFromPoint(CqbPoint point);
    [DllImport("user32.dll")] private static extern IntPtr GetAncestor(IntPtr hWnd, uint flags);
    public static IntPtr RootWindowAt(CqbPoint point) { return GetAncestor(WindowFromPoint(point), 2); }
    [DllImport("user32.dll")] public static extern bool GetCursorPos(out CqbPoint point);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern bool SystemParametersInfo(uint action, uint param, out CqbRect rect, uint flags);
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

function Open-CqbContextMenu([IntPtr]$Owner) {
    $rect = Get-CqbWindowRect $Owner
    $point = New-CqbLParam ($rect.Left + 12) ($rect.Top + 12)
    if (-not [CqbNative]::PostMessage($Owner, 0x007B, $Owner, $point)) {
        throw "Could not post WM_CONTEXTMENU"
    }
    $deadline = [DateTime]::UtcNow.AddSeconds(3)
    do {
        $popup = [CqbNative]::FindWindow("Codex-Quota-Bar_Menu", $null)
        if ($popup -ne [IntPtr]::Zero -and [CqbNative]::IsWindowVisible($popup)) {
            Start-Sleep -Milliseconds 200
            if (-not [CqbNative]::IsWindowVisible($popup)) { throw "Context menu closed during opening" }
            return $popup
        }
        Start-Sleep -Milliseconds 40
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "Context menu did not open"
}

function Assert-CqbContextMenuClosed([IntPtr]$Popup) {
    $deadline = [DateTime]::UtcNow.AddSeconds(3)
    while ([CqbNative]::IsWindowVisible($Popup) -and [DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 40
    }
    if ([CqbNative]::IsWindowVisible($Popup)) { throw "Context menu did not close" }
}

function Assert-CqbTaskbarRecovery([IntPtr]$Component) {
    $taskbar = [CqbNative]::FindWindow("Shell_TrayWnd", $null)
    if ($taskbar -eq [IntPtr]::Zero -or -not [CqbNative]::IsWindowVisible($taskbar) -or
        ([CqbNative]::GetWindowLongPtr($taskbar, -20).ToInt64() -band 0x8) -eq 0) {
        Write-Host "  [SKIP] 当前桌面没有可用于遮挡测试的置顶任务栏"
        return
    }
    # Let the initial weather-widget probe finish before checking a fixed position.
    Start-Sleep -Milliseconds 1500
    $before = Get-CqbWindowRect $Component
    $tray = Get-CqbWindowRect $taskbar
    $left = [Math]::Max($before.Left, $tray.Left)
    $top = [Math]::Max($before.Top, $tray.Top)
    $right = [Math]::Min($before.Right, $tray.Right)
    $bottom = [Math]::Min($before.Bottom, $tray.Bottom)
    if ($right -le $left -or $bottom -le $top) {
        Write-Host "  [SKIP] 组件与主任务栏没有重叠"
        return
    }
    $point = [CqbPoint]::new()
    $point.X = $left + [Math]::Floor(($right - $left) / 2)
    $point.Y = $top + [Math]::Floor(($bottom - $top) / 2)
    $windowAtPoint = [CqbNative]::RootWindowAt($point)
    if ($windowAtPoint -eq $taskbar) {
        Fail-Test "组件在模拟遮挡前已不可见"
        return
    }
    if ($windowAtPoint -ne $Component) {
        Write-Host "  [SKIP] 组件测试点被其他窗口覆盖"
        return
    }

    # Only lower the test window in the topmost group. Leave Explorer itself alone.
    $keepGeometryAndFocus = 0x0013 # SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE
    if (-not [CqbNative]::SetWindowPos($Component, $taskbar, 0, 0, 0, 0, $keepGeometryAndFocus)) {
        Fail-Test "无法模拟任务栏盖住组件"
        return
    }
    try {
        $deadline = [DateTime]::UtcNow.AddSeconds(2)
        while ([CqbNative]::RootWindowAt($point) -ne $Component -and [DateTime]::UtcNow -lt $deadline) {
            Start-Sleep -Milliseconds 30
        }
        $after = Get-CqbWindowRect $Component
        if ([CqbNative]::RootWindowAt($point) -ne $Component -or -not $before.Equals($after)) {
            Fail-Test "任务栏遮挡后组件未在原位置恢复"
        } else {
            Write-Host "  [PASS] 组件被同为置顶窗口的任务栏遮挡后在原位置恢复" -ForegroundColor Green
        }
    } finally {
        [void][CqbNative]::SetWindowPos($Component, [IntPtr]::new(-1), 0, 0, 0, 0, $keepGeometryAndFocus)
    }
}

$proc.Refresh()
$hwnd = [IntPtr]$proc.MainWindowHandle
$savedCursor = [CqbPoint]::new()
[void][CqbNative]::GetCursorPos([ref]$savedCursor)
try {
    if ($hwnd -eq [IntPtr]::Zero) { Write-Host "::error::Main window handle is unavailable"
    throw "Main window handle is unavailable" }

    $workArea = [CqbRect]::new()
    if (-not [CqbNative]::SystemParametersInfo(48, 0, [ref]$workArea, 0)) {
        Write-Host "::error::SystemParametersInfo(SPI_GETWORKAREA) failed"
    throw "SystemParametersInfo(SPI_GETWORKAREA) failed"
    }
    $initialRect = Get-CqbWindowRect $hwnd
    if ($initialRect.Bottom -le $workArea.Bottom) {
        Write-Host "  [PASS] 置顶窗口启动位置受工作区约束" -ForegroundColor Green
    } else {
        Fail-Test "置顶窗口仍可被任务栏完全遮挡"
    }

    $extendedStyle = [CqbNative]::GetWindowLongPtr($hwnd, -20).ToInt64()
    if (($extendedStyle -band 0x8) -ne 0) {
        Write-Host "  [PASS] 置顶模式开启且未改变工作区约束" -ForegroundColor Green
    } else {
        Fail-Test "置顶模式开启后窗口不是 TOPMOST"
    }

    $collapsedBefore = Get-CqbWindowRect $hwnd
    $uiScale = ($collapsedBefore.Right - $collapsedBefore.Left) / 380.0
    $expandX = [int][Math]::Round(364.0 * $uiScale)
    $expandY = [int][Math]::Round(88.0 * $uiScale)
    [void][CqbNative]::SendMessage($hwnd, 0x0201, [IntPtr]::new(1), (New-CqbLParam $expandX $expandY))
    [void][CqbNative]::SendMessage($hwnd, 0x0202, [IntPtr]::Zero, (New-CqbLParam $expandX $expandY))
    Start-Sleep -Milliseconds 150

    $expandedBeforeDrag = Get-CqbWindowRect $hwnd
    if (($expandedBeforeDrag.Bottom - $expandedBeforeDrag.Top) -le
        ($collapsedBefore.Bottom - $collapsedBefore.Top)) {
        Write-Host "::error::Window did not expand"
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
    $movedRect = Get-CqbWindowRect $hwnd
    [void][CqbNative]::SetCursorPos($screenStartX, $screenStartY)
    $returnPoint = New-CqbLParam ($screenStartX - $movedRect.Left) ($screenStartY - $movedRect.Top)
    [void][CqbNative]::SendMessage($hwnd, 0x0200, [IntPtr]::new(1), $returnPoint)
    $returnedRect = Get-CqbWindowRect $hwnd
    if ($returnedRect.Left -ne $expandedBeforeDrag.Left -or $returnedRect.Top -ne $expandedBeforeDrag.Top) {
        Fail-Test "鼠标回到拖动起点后，窗口未回到原始像素坐标"
    }
    [void][CqbNative]::SetCursorPos($screenStartX + $deltaX, $screenStartY + $deltaY)
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
        Fail-Test "展开拖动位置未保持（moved=$dragMoved, anchor=$anchorKept）"
    }
} catch {
    Fail-Test "展开拖动回归测试异常：$($_.Exception.Message)"
} finally {
    [void][CqbNative]::SetCursorPos($savedCursor.X, $savedCursor.Y)
}

try {
    $beforeSelection = Get-CqbWindowRect $hwnd
    $popup = Open-CqbContextMenu $hwnd
    $menuScale = [CqbNative]::GetDpiForWindow($popup) / 96.0
    $firstRow = New-CqbLParam ([int](12 * $menuScale)) ([int](12 * $menuScale))
    [void][CqbNative]::SendMessage($popup, 0x0201, [IntPtr]::new(1), $firstRow)
    Start-Sleep -Milliseconds 200
    if (-not [CqbNative]::IsWindowVisible($popup)) {
        throw "Menu item activated before the left button was released"
    }
    [void][CqbNative]::SendMessage($popup, 0x0202, [IntPtr]::Zero, $firstRow)
    Assert-CqbContextMenuClosed $popup
    $deadline = [DateTime]::UtcNow.AddSeconds(3)
    do {
        $afterSelection = Get-CqbWindowRect $hwnd
        $heightChanged = ($afterSelection.Bottom - $afterSelection.Top) -ne
            ($beforeSelection.Bottom - $beforeSelection.Top)
        if (-not $heightChanged) { Start-Sleep -Milliseconds 40 }
    } while (-not $heightChanged -and [DateTime]::UtcNow -lt $deadline)
    if (-not $heightChanged) { throw "Releasing the first menu item did not toggle details" }

    $popup = Open-CqbContextMenu $hwnd
    [void][CqbNative]::PostMessage($popup, 0x0100, [IntPtr]::new(0x1B), [IntPtr]::Zero)
    Assert-CqbContextMenuClosed $popup

    $popup = Open-CqbContextMenu $hwnd
    [void][CqbNative]::SendMessage($hwnd, 0x8005, [IntPtr]::Zero, [IntPtr]::Zero)
    Assert-CqbContextMenuClosed $popup
    if ([CqbNative]::IsWindowVisible($hwnd)) { throw "Hide left the main window visible" }
    [void][CqbNative]::SendMessage($hwnd, 0x8004, [IntPtr]::Zero, [IntPtr]::Zero)
    Write-Host "  [PASS] 右键菜单保持打开，松开左键选择，Esc 和隐藏命令关闭菜单" -ForegroundColor Green
} catch {
    Fail-Test "右键菜单回归测试失败：$($_.Exception.Message)"
}

# 4. 单实例互斥：第二实例裸启动应返回 0
Write-Host "[4/9] 单实例互斥..." -ForegroundColor Yellow
Expect-ExitCode "第二实例裸启动" "" 0

# 5. 合法命令
Write-Host "[5/9] 合法 IPC 命令..." -ForegroundColor Yellow
Expect-ExitCode "--refresh" '"--refresh"' 0
Expect-ExitCode "-r" '"-r"' 0
Expect-ExitCode "--toggle" '"--toggle"' 0
Expect-ExitCode "-t" '"-t"' 0
Expect-ExitCode "--show" '"--show"' 0
Expect-ExitCode "--hide" '"--hide"' 0

# 6. 客户端发送命令后不读取回复，不得占住唯一服务线程。
Write-Host "[6/9] 管道客户端不读取回复..." -ForegroundColor Yellow
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

# 完全不写入数据的连接也必须在服务端 I/O 超时后释放，避免阻塞后续 IPC 请求。
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
        Fail-Test "空连接阻塞 IPC（exit=$($refreshAfterSilent.ExitCode), $($timer.ElapsedMilliseconds)ms）"
    }
} finally {
    $silentClient.Dispose()
}

# 7. 错误路径（包括已移除的 --hook 与 --worker 参数）
Write-Host "[7/9] 错误路径退出码..." -ForegroundColor Yellow
Expect-ExitCode "--worker 已移除应拒绝" '"--worker" "REFRESH"' 1
Expect-ExitCode "--hook 已移除应拒绝" '"--hook" "SessionStart"' 1
Expect-ExitCode "--garbage (未知参数)" '"--garbage"' 1

# 8. EXIT 优雅退出
Write-Host "[8/9] EXIT 优雅退出..." -ForegroundColor Yellow
Expect-ExitCode "--exit" '"--exit"' 0
Start-Sleep -Milliseconds 800
if (@(Get-TargetProcesses).Count -gt 0) {
    Fail-Test "退出后进程仍在运行"
} else {
    Write-Host "  [PASS] 退出后进程已结束" -ForegroundColor Green
}
Expect-ExitCode "退出后 --refresh 应失败" '"--refresh"' 1
$savedConfig = Get-Content -LiteralPath $ConfigFile -Raw
$saved = $savedConfig | ConvertFrom-Json
if ($saved.Settings.FutureSetting.Flag -eq $true -and
    $saved.Window.FutureWindowField -eq "preserve" -and
    $savedConfig -match '"LargeInteger"\s*:\s*9007199254740993\b' -and
    $saved.Window.Y -ne 2000000000) {
    Write-Host "  [PASS] 含 BOM 的配置可读取并保存，未知字段保持原值" -ForegroundColor Green
} else {
    Fail-Test "带 BOM 的配置保存失败或未知字段被改写"
}

# 9. 冷启动组件模式时，旧配置中关闭的迷你与置顶仍应被强制开启。
Write-Host "[9/9] App Server 冷启动同步与组件模式锁定..." -ForegroundColor Yellow
$fakeCodexPath = Join-Path $CMakeBuildDir "Release\FakeCodexAppServer.exe"
$oldCodexPath = $env:CODEX_QUOTA_CODEX_PATH
$oldScenario = $env:CODEX_QUOTA_FAKE_SCENARIO
$appB = $null
try {
    if (-not (Test-Path -LiteralPath $fakeCodexPath)) {
        Fail-Test "FakeCodexAppServer 测试程序不存在"
    } else {
        $env:CODEX_QUOTA_CODEX_PATH = $fakeCodexPath
        $env:CODEX_QUOTA_FAKE_SCENARIO = "happy"
        $miniConfig = Get-Content -LiteralPath $ConfigFile -Raw | ConvertFrom-Json
        $miniConfig.Settings.MiniMode = $false
        $miniConfig.Settings.AlwaysOnTop = $false
        $miniConfig.Settings | Add-Member -NotePropertyName DockToWeather -NotePropertyValue $true -Force
        $miniConfig.Settings.UserScale = 1.2
        $miniConfig | ConvertTo-Json -Depth 10 | Set-Content `
            -LiteralPath $ConfigFile -Encoding utf8NoBOM

        $appB = Start-Process -FilePath $ExePath -PassThru
        Start-Sleep -Milliseconds 300
        if (-not $appB -or $appB.HasExited) {
            Fail-Test "迷你栏实例启动失败"
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
                $miniDeadline = (Get-Date).AddSeconds(5)
                $miniSizeOk = $false
                while ((Get-Date) -lt $miniDeadline) {
                    $appB.Refresh()
                    $miniHwnd = [IntPtr]$appB.MainWindowHandle
                    if ($miniHwnd -ne [IntPtr]::Zero) {
                        $miniRect = Get-CqbWindowRect $miniHwnd
                        $dpiScale = [CqbNative]::GetDpiForWindow($miniHwnd) / 96.0
                        $expectedMiniWidth = 2 * [int][Math]::Round(56 * $dpiScale * 1.2, [MidpointRounding]::AwayFromZero)
                        $roundedMiniHeight = [int][Math]::Round(28 * $dpiScale * 1.2, [MidpointRounding]::AwayFromZero)
                        $expectedMiniHeight = $roundedMiniHeight + ($roundedMiniHeight % 2)
                        $miniSizeOk = ($miniRect.Right - $miniRect.Left) -eq $expectedMiniWidth -and
                                      ($miniRect.Bottom - $miniRect.Top) -eq $expectedMiniHeight
                        if ($miniSizeOk) { break }
                    }
                    Start-Sleep -Milliseconds 100
                }
                if ($miniSizeOk) {
                    Write-Host "  [PASS] 双额度迷你栏尺寸响应 120% 用户缩放" -ForegroundColor Green
                } else {
                    Fail-Test "双额度迷你栏尺寸不正确"
                }
                if ($miniHwnd -ne [IntPtr]::Zero) {
                    $style = [CqbNative]::GetWindowLongPtr($miniHwnd, -20).ToInt64()
                    if (($style -band 0x8) -eq 0) {
                        Fail-Test "旧配置关闭置顶时，组件模式未强制置顶"
                    }

                    $gui = [CqbGuiThreadInfo]::new()
                    $gui.Size = [Runtime.InteropServices.Marshal]::SizeOf($gui)
                    $threadId = [CqbNative]::GetWindowThreadProcessId($miniHwnd, [IntPtr]::Zero)
                    $hit = New-CqbLParam 4 4
                    try {
                        [void][CqbNative]::SendMessage($miniHwnd, 0x0201, [IntPtr]::new(1), $hit)
                        if ($threadId -eq 0 -or -not [CqbNative]::GetGUIThreadInfo($threadId, [ref]$gui)) {
                            Fail-Test "无法读取组件窗口的鼠标捕获状态"
                        } elseif ($gui.Capture -eq $miniHwnd) {
                            Fail-Test "组件模式仍可通过左键开始拖动"
                        } else {
                            Write-Host "  [PASS] 组件模式禁止拖动，旧设置的迷你和置顶自动开启" -ForegroundColor Green
                        }
                    } finally {
                        [void][CqbNative]::SendMessage($miniHwnd, 0x0202, [IntPtr]::Zero, $hit)
                    }

                    Assert-CqbTaskbarRecovery $miniHwnd

                    $classStyle = [CqbNative]::GetClassLongPtr($miniHwnd, -26).ToInt64()
                    if (($classStyle -band 0x00020000) -eq 0) {
                        Write-Host "  [PASS] 迷你栏仅使用 DWM 原生投影，无固定下偏阴影" -ForegroundColor Green
                    } else {
                        Fail-Test "迷你栏仍叠加固定 CS_DROPSHADOW 投影"
                    }
                }
            } else {
                Fail-Test "App Server 实例管道未在 5 秒内就绪"
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
