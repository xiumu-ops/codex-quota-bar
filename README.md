# Codex-Quota-Bar (C++ Win32 Direct2D Native)

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/Platform-Windows%2011%20x64-0078d4.svg)](https://github.com/xiumu-ops/codex-quota-bar)
[![C++20](https://img.shields.io/badge/Language-C%2B%2B20-00599C.svg)](https://en.cppreference.com/w/cpp/20)
[![Direct2D](https://img.shields.io/badge/Graphics-Direct2D%20%2F%20DirectWrite-success.svg)](https://learn.microsoft.com/en-us/windows/win32/direct2d/direct2d-portal)

基于 **C++20 / Win32 / Direct2D / DirectWrite** 构建的原生桌面配额指示条：常驻工作区顶部，通过 Codex 官方 App Server 实时展示窗口额度、每周额度、额度重置时间、Token 活动统计与同步状态。

## 📸 界面预览与功能使用

### 1. 折叠状态（紧凑常驻栏）

![Codex-Quota-Bar 折叠状态](assets/screenshots/quota-bar-collapsed.png)

- **核心功能**：
  - **额度进度直观呈现**：双色指示条分别展示当前 5 小时窗口额度与每周总额度百分比。
  - **重置倒计时提示**：精准显示剩余重置时间（如 `4h 32m` / `3d 12h`），掌握额度周期。
  - **状态指示灯与提示**：实时反馈同步健康状态（🟢 正常同步 / 🟡 正在请求 / 🔴 离线或异常）。
- **交互方式**：
  - **左键单击**：展开或折叠详细统计面板。
  - **左键按住拖动**：自由拖放至屏幕任意位置（跨多显示器、DPI 切换均保持清晰且自动持久化位置）。
  - **鼠标悬停**：查看快速状态与基础统计摘要。

---

### 2. 展开状态（详情面板）

![Codex-Quota-Bar 展开状态](assets/screenshots/quota-bar-expanded.png)

- **核心功能**：
  - **精细额度指标**：展示 5 小时短期限额与每周长期限额的精确百分比与精确时刻（如 `18:45 重置`）。
  - **Token 使用统计卡片**：聚合展示累计 Lifetime Token、单日峰值 Token（Peak Daily）以及最长连续对话时长（Turn Duration），数值智能折算（`K` / `M` / `B` / `T`）。
  - **重置额度卡片 (Reset Credits)**：展示账户可用额度重置次数、最早过期时间及剩余有效天数。
- **交互方式**：
  - **再次左键单击**：收起详情面板回到紧凑状态。
  - **智能避让**：当窗口贴近屏幕底部时，详情面板将自动智能向上拓展展开，避免溢出屏幕。

---

### 3. 右键菜单（快捷设置）

![Codex-Quota-Bar 右键菜单](assets/screenshots/quota-bar-menus.png)

- **核心功能与操作**：
  - **立即刷新**：手动唤起临时 Codex App Server 进行即时额度与 Token 统计同步。
  - **伴随模式（关/开）**：开启后，仅在官方 Codex 桌面端启动时自动显示并同步；Codex 退出后自动静默隐藏并于后台超轻量驻留。
  - **锁定位置**：固定指示条在当前屏幕坐标，避免日常光标误触拖动。
  - **退出**：彻底关闭常驻进程并释放所有系统与图形资源。

---

## ✨ 架构特性

- **Direct2D 1.0 硬件加速渲染**：`ID2D1HwndRenderTarget` 将整帧绘制到窗口表面，交由 DWM 合成；窗口跳过背景擦除（`WM_ERASEBKGND`），过渡期零白闪
- **PerMonitorV2 感知 DPI**：按物理像素对齐绘制，跨屏拖动无缩放模糊
- **DirectWrite 现代排版**：Microsoft YaHei UI 中文字体渲染
- **固定向下展开**：详情面板始终追加在折叠栏下方（统计卡片 + 限额重置卡片）；底部空间不足时整体上移窗口
- **官方账户接口**：临时启动 `codex app-server`，通过 stdio JSON-RPC 调用 `account/rateLimits/read` 与 `account/usage/read`，无需额外常驻服务或开放本地端口
- **官方生命周期 Hook**：安装器注册 `SessionStart`、`Stop`、`SessionEnd`，新会话、每轮对话完成和会话结束时自动同步
- **伴随模式**：可从右键菜单启用；Codex 桌面端启动时自动显示，桌面端退出后防抖隐藏，并通过当前用户启动项在登录后后台等待
- **原生安装与卸载**：单文件安装包以管理员权限写入 `C:\Program Files (x86)\Codex-Quota-Bar`，并注册公共开始菜单和 Windows“已安装的应用”卸载入口
- **Win32 命名管道 IPC**：单实例守护进程，CLI / IDE / Git Hook 毫秒级通信（命令表见下）
- **零第三方运行库**：程序只使用 Windows 系统组件；额度同步需要已安装并登录的官方 Codex

---

## 🛠️ 构建与运行

```powershell
# 1. 构建（Windows 11 x64 / MSVC）
.\scripts\build.ps1

# 2. 运行（后台常驻）
.\scripts\run.ps1

# 3. 完整回归测试（会先强制重建当前源码）
.\scripts\test.ps1

# 4. 清除编译缓存，保留 dist 发布文件
.\scripts\clean.ps1
```

运行前请确认 Codex 桌面端或 CLI 已安装并完成登录。程序会自动发现桌面端附带的 `codex.exe`。

源码构建要求 Visual Studio 2022 Build Tools，并安装“使用 C++ 的桌面开发”工作负载和 Windows 11 SDK。发布程序使用静态 MSVC 运行库，不要求目标机器另装 VC++ Redistributable。

所有 CMake、MSBuild、可执行主程序与安装器中间文件统一写入隐藏目录 `.build\`；`dist\Release` 只保存可分发的单文件安装器及其 SHA256 校验文件。如需连同发布文件一起清理，执行 `.\scripts\clean.ps1 -IncludeDist`。

### 项目目录

```text
assets/       icons/ 应用图标与 screenshots/ README 预览截图
src/          EXE 源码、同模块头文件与 resources/ Win32 资源
installer/    安装器、卸载器及 Hook 配置实现
tests/        App Server、Hook 与异常路径测试
scripts/      构建、运行、测试、安装包与清理脚本
dist/         最终单文件安装器与 SHA256；可删除、可重新生成
.build/       隐藏的编译缓存；不纳入版本控制
```

项目采用单体 Win32 EXE 的共置布局，模块头文件与实现文件放在同一 `src/<Module>` 目录，不再维护内容重复的 `include/` 镜像目录。仓库根目录只保留 `CMakeLists.txt`、`README.md` 和版本控制文件。

### 安装包

直接运行发布目录中的 `Codex-Quota-Bar_version_2.4.2.exe` 即可安装。这是唯一的发布 EXE；主程序只作为安装器内部载荷构建，不再单独放入 `dist`。首次交互式安装会打开目录选择器，所选位置下自动创建独立的 `Codex-Quota-Bar` 子目录；默认位置为：

```text
C:\Program Files (x86)\Codex-Quota-Bar
```

安装器和卸载器会请求管理员权限；应用与卸载入口注册为系统级，公共开始菜单对本机用户可见。自定义路径必须使用名为 `Codex-Quota-Bar` 的独立目录，卸载器会通过注册的 `InstallLocation` 精确校验后才删除，避免误删用户选择位置中的其他文件。静默安装使用默认路径；重新安装沿用已注册的安装位置。应用本身仍是 Windows 11 x64 原生程序，默认放入 `Program Files (x86)` 并不表示生成 32 位程序。

窗口设置、伴随模式启动项以及 Codex Hook 均保持当前用户范围。安装器通过当前用户的 `~/.codex/hooks.json` 注册会话同步 Hook，不读取或修改高敏感的 `config.toml`；`hooks.json` 使用临时文件原子替换，若其中已有其他 Hook 或元数据，会结构化合并并精确保留整数等 JSON 数值。重新安装时会备份现有程序、卸载器和快捷方式，任何关键步骤失败都会恢复原安装。

卸载时只识别并移除调用 `Codex-Quota-Bar.exe --hook` 的三个处理器；文件仅由本软件创建且清空后会一并删除。伴随模式启动项也会始终移除。

首次安装或 Hook 命令路径变化后，Codex 会按照官方安全机制跳过尚未信任的非托管 Hook。请在 Codex 中输入 `/hooks`，审核并信任新增的三个 Hook。官方说明见 [Codex Hooks 文档](https://learn.chatgpt.com/docs/hooks)。

从源码生成安装包需要 Visual Studio Build Tools（MSVC 与 Windows Resource Compiler）：

```powershell
.\scripts\build-installer.ps1
```

正式发布可通过代码签名证书指纹签名应用、卸载器和安装器；脚本会在签名后执行 Authenticode 校验，再计算安装包哈希：

```powershell
$env:CODEX_QUOTA_SIGN_CERT_THUMBPRINT = "证书 SHA-1 指纹"
.\scripts\build-installer.ps1
```

未提供证书时仍可构建，但脚本会明确警告产物未签名。项目不会自动创建或信任自签名证书。

输出文件：

```text
dist\Release\Codex-Quota-Bar_version_2.4.2.exe
dist\Release\Codex-Quota-Bar_version_2.4.2.sha256
```

安装器支持 `/quiet` 或 `/s` 静默安装；已安装的 `Uninstall.exe /quiet` 可执行静默卸载并默认保留本地设置。卸载顺序固定为：精确移除本软件的 Hook、删除伴随启动项、终止全部实例、隔离主程序路径，最后删除程序文件与依赖。若 Windows 仍以错误 32 锁定可执行文件，卸载器会使用 `MOVEFILE_DELAY_UNTIL_REBOOT` 登记下次系统启动时删除，并明确提示需要重启。

---

## ⌨️ CLI 命令

带参数启动时向运行中的实例发送命令；无参数启动进入常驻 GUI。所有命令以退出码报告结果：`0` 表示命令已发送并得到 `OK` 回复，`1` 表示未知命令、未知参数或实例未运行。

| 命令                                    | 说明                                                            |
| ------------------------------------- | ------------------------------------------------------------- |
| `--worker BUSY`                       | Hook 兼容命令：回执成功，不存储、不显示任何工作状态                                  |
| `--worker IDLE` / `READY`             | Hook 兼容命令：回执成功，不改变界面                                          |
| `--worker DONE`                       | 收到命令后立即刷新额度；安装器注册的官方 `Stop` Hook 会在每轮对话完成时自动调用                |
| `--worker OFFLINE`                    | Hook 兼容命令：回执成功，不改变界面                                          |
| `--worker STATUS <文本>`                | Hook 兼容命令：接受文本并回执成功，不改变界面（可超 4KB，长消息自动分块读取）                   |
| `--worker STATS <v1> <v2> <v3> <v4>`  | 手动覆盖统计卡片（兼容旧 Hook）：累计 Token、峰值 Token、最长聊天、连续天数。下一次成功自动同步会替换该值 |
| `--worker REFRESH`                    | 立即刷新配额数据                                                      |
| `--hook SessionStart/Stop/SessionEnd` | 安装器写入的 Codex 生命周期入口；实例未运行时会启动额度条并强制同步                         |
| `--refresh` / `-r`                    | 立即刷新配额数据                                                      |
| `--toggle` / `-t`                     | 显示/隐藏切换                                                       |
| `--show`                              | 显示窗口                                                          |
| `--hide`                              | 隐藏窗口                                                          |
| `--exit` / `-x`                       | 优雅退出运行中的实例                                                    |

未知参数会向 stderr 打印用法并以退出码 `1` 结束，不会静默启动 GUI。

---

## ⚙️ 配置

### 统一应用配置

窗口位置、界面缩放、刷新间隔和伴随模式偏好统一保存在：

```text
%LOCALAPPDATA%\Codex-Quota-Bar\config.json
```

配置仅使用这一份 JSON 文件，不读取其他历史目录或旧格式文件。卸载时选择删除本地设置，会删除整个 `%LOCALAPPDATA%\Codex-Quota-Bar` 文件夹；选择保留则不会删除。

### Codex App Server

默认同步流程：

```text
codex app-server
  → initialize
  → initialized
  → account/rateLimits/read
  → 按 300 / 10080 分钟识别窗口额度与每周额度（各行中部的重置时间取自对应限额的 resetAt）
  → 读取 rateLimitResetCredits.availableCount 与 credits[].expiresAt
  → account/usage/read
  → 读取 lifetimeTokens / peakDailyTokens / longestRunningTurnSec / currentStreakDays
  → 关闭临时 App Server 进程
```

统计卡片展示账户累计 Token、单日 Token 峰值与最长聊天时长。Token 数值自动缩写为 `K` / `M` / `B` / `T`（数字与单位之间带空格，如 `24.5 B`）；聊天时长不足一小时按分钟显示，其余统一折算为总小时（如 `129 小时`）。

展开态第二张「重置」子卡片为三列官方指标：**重置次数**取 `rateLimitResetCredits.availableCount`；**过期时间**取可用重置额度详情中最早的 `credits[].expiresAt`（本地时间 `MM月DD日 HH:MM`）；**剩余天数**为距该到期时间还剩的天数（不足一天按一天计）。服务只返回次数而未返回详情时，到期时间与剩余天数显示 `--`，不会再用额度窗口的 `resetsAt` 冒充。

程序依次从 `CODEX_QUOTA_CODEX_PATH`、`%LOCALAPPDATA%\OpenAI\Codex\bin` 和 Program Files 查找 `codex.exe`。自定义安装可显式指定：

```powershell
$env:CODEX_QUOTA_CODEX_PATH = "D:\path\to\codex.exe"
.\scripts\run.ps1
```

### 应用清单

`src\resources\app.rc` 会把同目录的 `app.manifest`（Windows 11、PerMonitorV2 DPI 感知）编译并嵌入主程序。安装后文件名为 `Codex-Quota-Bar.exe`，发布安装器使用带版本号的文件名。项目只保留 MSVC x64 构建路径；如需修改 DPI 或兼容性声明，直接编辑该清单后重新构建即可。

### 伴随模式

右键悬浮栏选择 **伴随模式：关/开**。启用后程序会：

1. 在 `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` 写入当前 EXE 的启动项；
2. 每 2 秒检查一次官方 Codex 桌面端进程；
3. Codex 启动时自动显示并立即同步；
4. 首次轮询未发现 Codex 时立即隐藏窗口并停止额度抓取，关闭后的检测延迟约为 0～2 秒；
5. 保留轻量后台进程，以便同一登录会话内下一次启动 Codex 时重新显示。

检测仅接受官方 Codex 安装路径中的桌面宿主 `ChatGPT.exe` 或启动器 `Codex.exe`，明确排除 `resources\codex.exe` 与 `%LOCALAPPDATA%\OpenAI\Codex\bin` 下的 CLI/App Server。右键“退出”或 `--exit` 仍会彻底结束后台进程；关闭伴随模式会同步移除当前用户启动项。

---

## ⚠️ 已知限制与安全说明

- **管道访问控制**：命名管道 `\\.\pipe\Codex-Quota-Bar_Pipe` 使用默认安全描述符——同一用户登录会话下的任何进程都可以连接并发送命令（包括 `EXIT`）。这是单用户桌面工具的常见取舍；如需更强隔离，可在 `PipeServer` 中为 `CreateNamedPipeW` 指定专用 DACL。
- **Codex 路径信任**：额度同步自动发现仅检查官方桌面端常用安装目录；显式设置 `CODEX_QUOTA_CODEX_PATH` 表示用户信任该可执行文件。伴随进程检测另行校验官方 Codex 桌面安装路径，并排除 CLI/App Server 路径。
- **工作状态命令零 UI 影响**：`BUSY` / `IDLE` / `DONE` / `OFFLINE` / `STATUS` 仅为兼容 Hook 保留——服务端接受命令并回执 `OK`，但不存储、不显示任何工作状态；底部文字和指示灯只反映额度同步状态。安装器注册的官方 `Stop` Hook 会在每轮对话完成时调用 `DONE` 并触发刷新。
- **`STATS` 字段分隔**：该命令只用于兼容手动 Hook；四个字段以空格分隔且字段内容不可包含空格。正常运行时统计卡片由 `account/usage/read` 自动更新；接口未返回过有效值时才显示 `--`。
- **命令大小上限**：单条命令上限 64KB（消息模式管道）；超限会回复 `TOO_LONG` 并断开连接，客户端以退出码 `1` 报告失败。受 CreateProcess 32767 字符命令行上限约束（UTF-16 约 65,534 字节），正常 CLI 调用无法触及该上限。

---

## 📄 开源许可证 (License)

本项目基于 [MIT License](LICENSE) 开源。

