#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>
#include <functional>
#include <thread>
#include <atomic>

namespace CodexQuotaBar {

    class PipeServer {
    public:
        using CommandHandler = std::function<std::wstring(const std::wstring&)>;

        PipeServer(const std::wstring& pipeName, CommandHandler handler);
        ~PipeServer();

        bool Start();
        void Stop();

        static std::wstring SendCommand(const std::wstring& pipeName, const std::wstring& command, DWORD timeoutMs = 2000);

    private:
        HANDLE CreateServerPipe() const;
        void ServerThreadLoop(HANDLE initialPipe);

        std::wstring m_pipeName;
        CommandHandler m_handler;
        std::atomic<bool> m_running{ false };
        std::thread m_serverThread;
        HANDLE m_stopEvent = NULL;
        HANDLE m_ioEvent = NULL; // 每次重叠 I/O 复用的完成事件
    };

} // namespace CodexQuotaBar
