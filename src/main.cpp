#include "Core/Constants.h"
#include "Core/Models.h"
#include "UI/MainWindow.h"
#include "Services/PipeServer.h"

#include <shellapi.h>
#include <sstream>

using namespace CodexQuotaBar;

const std::wstring PIPE_NAME = L"\\\\.\\pipe\\Codex-Quota-Bar_Pipe";
const wchar_t* MUTEX_NAME = L"Codex-Quota-Bar_Mutex_Session";

namespace {

bool ResolveHookEvent(const std::wstring& event, std::wstring& wakeCommand) {
    if (event == L"SessionStart") {
        wakeCommand = L"REFRESH";
        return true;
    }
    if (event == L"Stop" || event == L"SessionEnd") {
        wakeCommand = L"DONE";
        return true;
    }
    return false;
}

bool StartHookHost(const std::wstring& event) {
    std::wstring modulePath(32768, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr, modulePath.data(), static_cast<DWORD>(modulePath.size()));
    if (length == 0 || length >= modulePath.size()) return false;
    modulePath.resize(length);

    // Hook 调用本身必须尽快退出，尤其 SessionEnd 最多只允许 3 秒。
    // 派生一个不继承 Hook stdin/stdout 的 GUI 主进程，由它负责长驻和同步。
    std::wstring commandLine = L"\"" + modulePath + L"\" --hook-host " + event;
    STARTUPINFOW startup = { sizeof(startup) };
    PROCESS_INFORMATION process = {};
    if (!CreateProcessW(modulePath.c_str(), commandLine.data(), nullptr, nullptr, FALSE,
                        0, nullptr, nullptr, &startup, &process)) {
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) {
        fwprintf(stderr, L"Codex-Quota-Bar: 无法解析命令行\n");
        return 1;
    }

    bool launchedFromHook = false;
    std::wstring hookWakeCommand;

    // 1. 处理 --worker 外部调用指令（如 CLI、IDE 插件或 Git 钩子唤醒）
    if (argc > 1) {
        std::wstring arg1 = argv[1];
        if (arg1 == L"--worker" || arg1 == L"-w") {
            std::wstring command;
            for (int i = 2; i < argc; ++i) {
                if (!command.empty()) command += L' ';
                command += argv[i];
            }
            if (command.empty()) command = L"REFRESH";
            std::wstring reply = PipeServer::SendCommand(PIPE_NAME, command, 3000);
            LocalFree(argv);
            return (reply == L"OK") ? 0 : 1;
        } else if (arg1 == L"--refresh" || arg1 == L"-r") {
            std::wstring reply = PipeServer::SendCommand(PIPE_NAME, L"REFRESH", 2000);
            LocalFree(argv);
            return (reply == L"OK") ? 0 : 1;
        } else if (arg1 == L"--toggle" || arg1 == L"-t") {
            std::wstring reply = PipeServer::SendCommand(PIPE_NAME, L"TOGGLE", 2000);
            LocalFree(argv);
            return (reply == L"OK") ? 0 : 1;
        } else if (arg1 == L"--show") {
            std::wstring reply = PipeServer::SendCommand(PIPE_NAME, L"SHOW", 2000);
            LocalFree(argv);
            return (reply == L"OK") ? 0 : 1;
        } else if (arg1 == L"--hide") {
            std::wstring reply = PipeServer::SendCommand(PIPE_NAME, L"HIDE", 2000);
            LocalFree(argv);
            return (reply == L"OK") ? 0 : 1;
        } else if (arg1 == L"--exit" || arg1 == L"-x") {
            std::wstring reply = PipeServer::SendCommand(PIPE_NAME, L"EXIT", 2000);
            LocalFree(argv);
            return (reply == L"OK") ? 0 : 1;
        } else if (arg1 == L"--hook" || arg1 == L"--hook-host") {
            const std::wstring event = argc > 2 ? argv[2] : L"";
            if (!ResolveHookEvent(event, hookWakeCommand)) {
                fwprintf(stderr, L"Codex-Quota-Bar: 未知 Hook 事件 \"%s\"\n", event.c_str());
                LocalFree(argv);
                return 1;
            }

            // 外部 Hook 入口只负责快速通知。实例尚未运行时派生后台主进程，
            // 防止同步 SessionEnd Hook 被长驻消息循环卡到超时。
            if (arg1 == L"--hook") {
                if (PipeServer::SendCommand(PIPE_NAME, hookWakeCommand, 1000) == L"OK") {
                    LocalFree(argv);
                    return 0;
                }
                const bool started = StartHookHost(event);
                LocalFree(argv);
                return started ? 0 : 1;
            }

            // 内部主进程入口：没有实例时启动 GUI 并强制首次同步；若并发
            // Hook 已先启动实例，则由下方互斥分支把事件转发给现有实例。
            launchedFromHook = true;
        } else {
            // 未知参数：打印用法并以非零码退出，绝不静默启动 GUI
            fwprintf(stderr, L"Codex-Quota-Bar: 未知参数 \"%s\"\n\n", arg1.c_str());
            fwprintf(stderr, L"用法:\n"
                L"  Codex-Quota-Bar                        启动悬浮窗\n"
                L"  Codex-Quota-Bar (--worker|-w) <命令>   发送命令 (BUSY/IDLE/DONE/OFFLINE/STATUS 文本)\n"
                L"  Codex-Quota-Bar --worker STATS <v1> <v2> <v3> <v4>\n"
                L"                                       上报统计：累计/峰值 Token、最长聊天、连续天数，\n"
                L"                                       字段以空格分隔，内容不可含空格\n"
                L"  Codex-Quota-Bar --refresh | -r         立即刷新配额\n"
                L"  Codex-Quota-Bar --toggle | -t          显示/隐藏切换\n"
                L"  Codex-Quota-Bar --show                 显示窗口\n"
                L"  Codex-Quota-Bar --hide                 隐藏窗口\n"
                L"  Codex-Quota-Bar --hook <事件>          Codex 生命周期 Hook (SessionStart/Stop/SessionEnd)\n"
                L"  Codex-Quota-Bar --exit | -x            退出正在运行的实例\n");
            LocalFree(argv);
            return 1;
        }
    }
    LocalFree(argv);

    // 2. 单实例互斥量检查
    HANDLE hMutex = CreateMutexW(NULL, FALSE, MUTEX_NAME);
    if (hMutex != NULL && GetLastError() == ERROR_ALREADY_EXISTS) {
        // 已有实例在运行：短时重试唤醒，避免对刚启动（尚未建好管道）的实例发命令失败
        const std::wstring wakeCommand = launchedFromHook ? hookWakeCommand : L"SHOW";
        for (int attempt = 0; attempt < 10; ++attempt) {
            if (PipeServer::SendCommand(PIPE_NAME, wakeCommand, 300) == L"OK") break;
            Sleep(100);
        }
        CloseHandle(hMutex);
        return 0;
    }
    if (hMutex == NULL) {
        // 创建失败仅降级：失去单实例保护，但不阻止运行
        fwprintf(stderr, L"Codex-Quota-Bar: 警告：创建互斥量失败，单实例保护不可用\n");
    }

    // 3. 初始化 COM 库
    bool comInitialized = false;
    HRESULT hrCom = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hrCom)) {
        if (hrCom == RPC_E_CHANGED_MODE) {
            // 线程 COM 并发模型已被其他组件占用：跳过初始化，Direct2D 在
            // 多线程套间下同样可用，仅打印警告
            fwprintf(stderr, L"Codex-Quota-Bar: 警告：COM 并发模型已被占用，跳过 COM 初始化\n");
        } else {
            fwprintf(stderr, L"Codex-Quota-Bar: 初始化 COM 失败 (0x%08X)\n", static_cast<unsigned>(hrCom));
            if (hMutex) CloseHandle(hMutex);
            return 1;
        }
    } else {
        comInitialized = true;
    }

    // 4. 创建主窗体
    auto mainWindow = std::make_unique<MainWindow>();
    if (!mainWindow->Create(launchedFromHook)) {
        // DirectWrite/Direct2D 对象必须在线程 COM 套间关闭前释放。
        mainWindow.reset();
        if (hMutex) CloseHandle(hMutex);
        if (comInitialized) CoUninitialize();
        return 1;
    }

    // 5. 启动命名管道 IPC 守护线程
    PipeServer pipeServer(PIPE_NAME, [&mainWindow](const std::wstring& cmd) -> std::wstring {
        HWND hwnd = mainWindow ? mainWindow->GetHwnd() : NULL;
        if (!hwnd) return L"ERROR";

        if (cmd == L"REFRESH") {
            return PostMessageW(hwnd, WM_CQB_REFRESH, 0, 0) ? L"OK" : L"ERROR";
        } else if (cmd == L"BUSY" || cmd == L"WORKING" || cmd == L"GENERATING" ||
                   cmd == L"IDLE" || cmd == L"READY" || cmd == L"OFFLINE" ||
                   cmd.rfind(L"STATUS ", 0) == 0) {
            return L"OK";
        } else if (cmd == L"DONE") {
            return PostMessageW(hwnd, WM_CQB_REFRESH, 0, 0) ? L"OK" : L"ERROR";
        } else if (cmd.rfind(L"STATS ", 0) == 0) {
            auto* stats = new TokenStats();
            std::wistringstream ss(cmd.substr(6));
            std::vector<std::wstring> tokens;
            std::wstring tok;
            while (ss >> tok) {
                tokens.push_back(tok);
            }
            if (tokens.size() > 0) stats->totalTokens = tokens[0];
            if (tokens.size() > 1) stats->peakTokens = tokens[1];
            if (tokens.size() > 2) stats->longestTask = tokens[2];
            if (tokens.size() > 3) stats->streakDays = tokens[3];

            if (!PostMessageW(hwnd, WM_CQB_STATS, 0, reinterpret_cast<LPARAM>(stats))) {
                delete stats;
                return L"ERROR";
            }
            return L"OK";
        } else if (cmd == L"SHOW") {
            return PostMessageW(hwnd, WM_CQB_SHOW, 0, 0) ? L"OK" : L"ERROR";
        } else if (cmd == L"HIDE") {
            return PostMessageW(hwnd, WM_CQB_HIDE, 0, 0) ? L"OK" : L"ERROR";
        } else if (cmd == L"TOGGLE") {
            return PostMessageW(hwnd, WM_CQB_TOGGLE, 0, 0) ? L"OK" : L"ERROR";
        } else if (cmd == L"EXIT") {
            return PostMessageW(hwnd, WM_CLOSE, 0, 0) ? L"OK" : L"ERROR";
        }
        return L"UNKNOWN";
    });
    if (!pipeServer.Start()) {
        fwprintf(stderr, L"Codex-Quota-Bar: IPC 管道启动失败（事件对象创建失败），退出\n");
        mainWindow->BeginShutdown();
        mainWindow.reset();
        if (comInitialized) CoUninitialize();
        if (hMutex) {
            CloseHandle(hMutex);
        }
        return 1;
    }

    // 6. 显示主窗口
    mainWindow->Show();

    // 7. 标准 Windows 消息泵
    MSG msg = {};
    BOOL getMessageResult = FALSE;
    while ((getMessageResult = GetMessageW(&msg, NULL, 0, 0)) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (getMessageResult == -1) {
        fwprintf(stderr, L"Codex-Quota-Bar: 消息循环读取失败\n");
    }

    // 8. 退出清理
    mainWindow->BeginShutdown();
    pipeServer.Stop();
    mainWindow.reset();
    if (comInitialized) CoUninitialize();

    if (hMutex) {
        CloseHandle(hMutex);
    }

    return getMessageResult == -1 ? 1 : static_cast<int>(msg.wParam);
}
