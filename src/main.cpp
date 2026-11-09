#include "Core/Constants.h"
#include "Core/Logger.h"
#include "Core/WinHandle.h"
#include "UI/MainWindow.h"
#include "Services/PipeServer.h"
#include "Services/PipeClient.h"

#include <shellapi.h>

using namespace CodexQuotaBar;

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const std::wstring pipeName = CurrentPipeName();

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) {
        fwprintf(stderr, L"Codex-Quota-Bar: 无法解析命令行\n");
        return 1;
    }

    // 1. 处理外部 CLI 控制指令
    if (argc > 1) {
        std::wstring arg1 = argv[1];
        if (arg1 == L"--refresh" || arg1 == L"-r") {
            std::wstring reply = PipeServer::SendCommand(pipeName, L"REFRESH", 2000);
            LocalFree(argv);
            return (reply == L"OK") ? 0 : 1;
        } else if (arg1 == L"--toggle" || arg1 == L"-t") {
            std::wstring reply = PipeServer::SendCommand(pipeName, L"TOGGLE", 2000);
            LocalFree(argv);
            return (reply == L"OK") ? 0 : 1;
        } else if (arg1 == L"--show") {
            std::wstring reply = PipeServer::SendCommand(pipeName, L"SHOW", 2000);
            LocalFree(argv);
            return (reply == L"OK") ? 0 : 1;
        } else if (arg1 == L"--hide") {
            std::wstring reply = PipeServer::SendCommand(pipeName, L"HIDE", 2000);
            LocalFree(argv);
            return (reply == L"OK") ? 0 : 1;
        } else if (arg1 == L"--exit" || arg1 == L"-x") {
            std::wstring reply = PipeServer::SendCommand(pipeName, L"EXIT", 2000);
            LocalFree(argv);
            return (reply == L"OK") ? 0 : 1;
        } else {
            // 未知参数：打印用法并以非零码退出，绝不静默启动 GUI
            fwprintf(stderr, L"Codex-Quota-Bar: 未知参数 \"%s\"\n\n", arg1.c_str());
            fwprintf(stderr, L"用法:\n"
                L"  Codex-Quota-Bar                  启动悬浮窗\n"
                L"  Codex-Quota-Bar --refresh | -r   立即刷新配额\n"
                L"  Codex-Quota-Bar --toggle | -t    显示/隐藏切换\n"
                L"  Codex-Quota-Bar --show           显示窗口\n"
                L"  Codex-Quota-Bar --hide           隐藏窗口\n"
                L"  Codex-Quota-Bar --exit | -x      退出正在运行的实例\n");
            LocalFree(argv);
            return 1;
        }
    }
    LocalFree(argv);

    // 2. 单实例互斥量检查
    const WinHandle hMutex(CreateMutexW(nullptr, FALSE, MUTEX_NAME));
    if (hMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        // 已有实例在运行：短时重试唤醒，避免对刚启动（尚未建好管道）的实例发命令失败
        for (int attempt = 0; attempt < 10; ++attempt) {
            if (PipeServer::SendCommand(pipeName, L"SHOW", 300) == L"OK") break;
            Sleep(100);
        }
        return 0;
    }
    if (!hMutex) {
        // 创建失败仅降级：失去单实例保护，但不阻止运行
        fwprintf(stderr, L"Codex-Quota-Bar: 警告：创建互斥量失败，单实例保护不可用\n");
    }

    // 3. 初始化 COM 库
    bool comInitialized = false;
    HRESULT hrCom = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hrCom)) {
        if (hrCom == RPC_E_CHANGED_MODE) {
            // 线程已经属于另一种 COM 套间；保留现有初始化且不调用 CoUninitialize。
            // Direct2D/DirectWrite 工厂不要求 STA，因此可安全继续。
            fwprintf(stderr, L"Codex-Quota-Bar: 警告：COM 并发模型已被占用，跳过 COM 初始化\n");
            WriteLog(LogLevel::Warning, L"主线程已使用其他 COM 套间模型，沿用现有初始化。");
        } else {
            fwprintf(stderr, L"Codex-Quota-Bar: 初始化 COM 失败 (0x%08X)\n", static_cast<unsigned>(hrCom));
            WriteLog(LogLevel::Error, L"COM 初始化失败，应用终止。");
            return 1;
        }
    } else {
        comInitialized = true;
    }

    // 4. 创建主窗体
    auto mainWindow = std::make_unique<MainWindow>();
    if (!mainWindow->Create()) {
        WriteLog(LogLevel::Error, L"主窗口或图形资源初始化失败。");
        // DirectWrite/Direct2D 对象必须在线程 COM 套间关闭前释放。
        mainWindow.reset();
        if (comInitialized) CoUninitialize();
        return 1;
    }

    // 5. 启动命名管道 IPC 守护线程
    PipeServer pipeServer(pipeName, [&mainWindow](const std::wstring& cmd) -> std::wstring {
        HWND hwnd = mainWindow ? mainWindow->GetHwnd() : NULL;
        if (!hwnd) return L"ERROR";

        if (cmd == L"REFRESH") {
            return PostMessageW(hwnd, WM_CQB_REFRESH, 0, 0) ? L"OK" : L"ERROR";
        } else if (cmd == L"SHOW") {
            return PostMessageW(hwnd, WM_CQB_SHOW, 0, 0) ? L"OK" : L"ERROR";
        } else if (cmd == L"HIDE") {
            return PostMessageW(hwnd, WM_CQB_HIDE, 0, 0) ? L"OK" : L"ERROR";
        } else if (cmd == L"TOGGLE") {
            return PostMessageW(hwnd, WM_CQB_TOGGLE, 0, 0) ? L"OK" : L"ERROR";
        } else if (cmd == L"EXIT") {
            WriteLog(LogLevel::Info, L"收到 IPC 退出请求。");
            return PostMessageW(hwnd, WM_CLOSE, 0, 0) ? L"OK" : L"ERROR";
        }
        return L"UNKNOWN";
    });
    if (!pipeServer.Start()) {
        fwprintf(stderr, L"Codex-Quota-Bar: IPC 管道安全初始化或服务线程启动失败，退出\n");
        mainWindow->BeginShutdown();
        mainWindow.reset();
        if (comInitialized) CoUninitialize();
        return 1;
    }
    WriteLog(LogLevel::Info, std::wstring(L"Codex-Quota-Bar ") + APP_VERSION + L" 已启动。");

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
    pipeServer.Stop();
    mainWindow->BeginShutdown();
    mainWindow.reset();
    WriteLog(
        getMessageResult == -1 ? LogLevel::Error : LogLevel::Info,
        getMessageResult == -1
            ? L"Codex-Quota-Bar 因消息循环错误退出。"
            : L"Codex-Quota-Bar 已正常退出。");
    if (comInitialized) CoUninitialize();

    return getMessageResult == -1 ? 1 : static_cast<int>(msg.wParam);
}
