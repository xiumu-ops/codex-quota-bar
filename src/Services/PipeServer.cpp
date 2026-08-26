#include "Services/PipeServer.h"
#include "Core/Logger.h"

#include <sddl.h>
#include <algorithm>
#include <exception>
#include <system_error>

namespace CodexQuotaBar {

    PipeServer::PipeServer(const std::wstring& pipeName, CommandHandler handler)
        : m_pipeName(pipeName), m_handler(handler) {
        m_stopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        m_ioEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    }

    PipeServer::~PipeServer() {
        Stop();
        if (m_stopEvent) {
            CloseHandle(m_stopEvent);
            m_stopEvent = NULL;
        }
        if (m_ioEvent) {
            CloseHandle(m_ioEvent);
            m_ioEvent = NULL;
        }
    }

    bool PipeServer::Start() {
        if (m_running.exchange(true)) return true;
        if (!m_stopEvent || !m_ioEvent) {
            m_running = false;
            return false;
        }
        ResetEvent(m_stopEvent);
        HANDLE initialPipe = CreateServerPipe();
        if (initialPipe == INVALID_HANDLE_VALUE) {
            WriteLog(LogLevel::Error, L"IPC 管道创建失败；拒绝降级为默认权限。");
            m_running = false;
            return false;
        }
        try {
            m_serverThread = std::thread(&PipeServer::ServerThreadLoop, this, initialPipe);
        } catch (const std::system_error&) {
            CloseHandle(initialPipe);
            WriteLog(LogLevel::Error, L"IPC 服务线程创建失败。");
            m_running = false;
            return false;
        }
        return true;
    }

    void PipeServer::Stop() {
        const bool wasRunning = m_running.exchange(false);
        if (wasRunning && m_stopEvent) SetEvent(m_stopEvent);
        if (m_serverThread.joinable()) {
            m_serverThread.join();
        }
    }

    namespace {
        constexpr DWORD kClientIoTimeoutMs = 1000;

        DWORD WaitIoOrStop(HANDLE ioEvent, HANDLE stopEvent, DWORD timeoutMs = INFINITE) {
            HANDLE events[] = { ioEvent, stopEvent };
            return WaitForMultipleObjects(2, events, FALSE, timeoutMs);
        }

        void CancelAndDrainIo(HANDLE pipe, OVERLAPPED& ovl) {
            if (!CancelIoEx(pipe, &ovl) && GetLastError() != ERROR_NOT_FOUND) return;
            DWORD transferred = 0;
            GetOverlappedResult(pipe, &ovl, &transferred, TRUE);
        }
    }

    HANDLE PipeServer::CreateServerPipe() const {
        // 显式限定为对象所有者、SYSTEM 与管理员，并阻止低完整性进程写入。
        // OW 会在对象访问检查时匹配创建该管道的当前用户。
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        constexpr wchar_t kPipeSddl[] =
            L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;OW)S:(ML;;NW;;;ME)";
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                kPipeSddl, SDDL_REVISION_1, &descriptor, nullptr)) {
            return INVALID_HANDLE_VALUE;
        }

        SECURITY_ATTRIBUTES attributes = {};
        attributes.nLength = sizeof(attributes);
        attributes.lpSecurityDescriptor = descriptor;
        attributes.bInheritHandle = FALSE;
        HANDLE pipe = CreateNamedPipeW(
            m_pipeName.c_str(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            PIPE_UNLIMITED_INSTANCES,
            4096, 4096, 0, &attributes);
        LocalFree(descriptor);
        return pipe;
    }

    void PipeServer::ServerThreadLoop(HANDLE initialPipe) {
        HANDLE hPipe = initialPipe;
        while (m_running) {
            if (hPipe == INVALID_HANDLE_VALUE) {
                WriteLog(LogLevel::Error, L"IPC 管道被占用或无法安全重建，服务已停止。");
                break;
            }

            OVERLAPPED ovl = {};
            ovl.hEvent = m_ioEvent;
            ResetEvent(m_ioEvent);

            BOOL connected = ConnectNamedPipe(hPipe, &ovl);
            if (!connected) {
                DWORD err = GetLastError();
                if (err == ERROR_IO_PENDING) {
                    DWORD wait = WaitIoOrStop(m_ioEvent, m_stopEvent);
                    if (wait == WAIT_OBJECT_0 + 1) {
                        CancelAndDrainIo(hPipe, ovl);
                        CloseHandle(hPipe);
                        hPipe = INVALID_HANDLE_VALUE;
                        break;
                    }
                    if (wait != WAIT_OBJECT_0) {
                        CancelAndDrainIo(hPipe, ovl);
                        CloseHandle(hPipe);
                        hPipe = INVALID_HANDLE_VALUE;
                        break;
                    }
                    DWORD transferred = 0;
                    connected = GetOverlappedResult(hPipe, &ovl, &transferred, FALSE);
                    if (!connected) {
                        CloseHandle(hPipe);
                        hPipe = m_running ? CreateServerPipe() : INVALID_HANDLE_VALUE;
                        continue;
                    }
                } else if (err != ERROR_PIPE_CONNECTED) {
                    CloseHandle(hPipe);
                    hPipe = m_running ? CreateServerPipe() : INVALID_HANDLE_VALUE;
                    continue;
                } else {
                    connected = TRUE;
                }
            }

            if (connected && m_running) {
                constexpr DWORD kChunkBytes = 1024 * sizeof(wchar_t);
                constexpr DWORD kMaxCommandBytes = 64 * 1024;
                std::wstring cmd;
                cmd.reserve(kMaxCommandBytes / sizeof(wchar_t));
                BOOL readOk = TRUE;
                bool overLimit = false;

                while (readOk && !overLimit) {
                    wchar_t part[1024] = { 0 };
                    DWORD bytesRead = 0;
                    ResetEvent(m_ioEvent);
                    readOk = ReadFile(hPipe, part, kChunkBytes, &bytesRead, &ovl);
                    if (!readOk) {
                        DWORD err = GetLastError();
                        if (err == ERROR_IO_PENDING) {
                            DWORD wait = WaitIoOrStop(
                                m_ioEvent, m_stopEvent, kClientIoTimeoutMs);
                            if (wait != WAIT_OBJECT_0) {
                                CancelAndDrainIo(hPipe, ovl);
                                readOk = FALSE;
                                break;
                            }
                            readOk = GetOverlappedResult(hPipe, &ovl, &bytesRead, FALSE);
                            err = readOk ? ERROR_SUCCESS : GetLastError();
                        }
                        if (!readOk && err == ERROR_MORE_DATA) {
                            readOk = TRUE;
                            bytesRead = kChunkBytes;
                        }
                    }
                    if (!readOk) break;

                    cmd.append(part, bytesRead / sizeof(wchar_t));
                    if (cmd.size() * sizeof(wchar_t) >= kMaxCommandBytes) {
                        overLimit = true;
                        break;
                    }

                    if (bytesRead < kChunkBytes) {
                        break;
                    }
                    DWORD avail = 0, remaining = 0;
                    if (!PeekNamedPipe(hPipe, NULL, 0, NULL, &avail, &remaining) || avail == 0) {
                        break;
                    }
                }

                auto writeReply = [&](const std::wstring& reply) {
                    DWORD bytesWritten = 0;
                    ResetEvent(m_ioEvent);
                    BOOL writeOk = WriteFile(hPipe, reply.c_str(), static_cast<DWORD>((reply.size() + 1) * sizeof(wchar_t)), &bytesWritten, &ovl);
                    if (!writeOk && GetLastError() == ERROR_IO_PENDING) {
                        DWORD wait = WaitIoOrStop(
                            m_ioEvent, m_stopEvent, kClientIoTimeoutMs);
                        if (wait == WAIT_OBJECT_0) {
                            writeOk = GetOverlappedResult(hPipe, &ovl, &bytesWritten, FALSE);
                        } else {
                            CancelAndDrainIo(hPipe, ovl);
                            writeOk = FALSE;
                        }
                    }
                };

                if (overLimit) {
                    writeReply(L"TOO_LONG");
                } else if (readOk && !cmd.empty()) {
                    while (!cmd.empty() && cmd.back() == L'\0') cmd.pop_back();
                    std::wstring reply = L"OK";
                    if (m_handler) {
                        try {
                            reply = m_handler(cmd);
                        } catch (const std::exception&) {
                            WriteLog(LogLevel::Error, L"IPC 命令处理发生标准异常。");
                            reply = L"ERROR";
                        } catch (...) {
                            WriteLog(LogLevel::Error, L"IPC 命令处理发生未知异常。");
                            reply = L"ERROR";
                        }
                    }
                    writeReply(reply);
                }
            }

            DisconnectNamedPipe(hPipe);
            CloseHandle(hPipe);
            hPipe = m_running ? CreateServerPipe() : INVALID_HANDLE_VALUE;
        }
        if (hPipe != INVALID_HANDLE_VALUE) CloseHandle(hPipe);
        m_running = false;
    }

    std::wstring PipeServer::SendCommand(const std::wstring& pipeName, const std::wstring& command, DWORD timeoutMs) {
        wchar_t replyBuf[2048] = { 0 };
        const ULONGLONG deadline = GetTickCount64() + (std::max)(timeoutMs, 1UL);

        for (;;) {
            const ULONGLONG now = GetTickCount64();
            if (now >= deadline) break;
            const DWORD remaining = static_cast<DWORD>(deadline - now);
            DWORD bytesRead = 0;
            BOOL ok = CallNamedPipeW(
                pipeName.c_str(),
                const_cast<wchar_t*>(command.c_str()),
                static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)),
                replyBuf,
                sizeof(replyBuf),
                &bytesRead,
                remaining);

            if (ok && bytesRead >= sizeof(wchar_t)) {
                size_t charCount = bytesRead / sizeof(wchar_t);
                if (charCount > 0 && replyBuf[charCount - 1] == L'\0') --charCount;
                return std::wstring(replyBuf, charCount);
            }
            const DWORD error = ok ? ERROR_INVALID_DATA : GetLastError();
            const ULONGLONG afterCall = GetTickCount64();
            if (afterCall >= deadline ||
                (error != ERROR_PIPE_BUSY && error != ERROR_FILE_NOT_FOUND)) break;

            const DWORD waitRemaining = static_cast<DWORD>(deadline - afterCall);
            if (error == ERROR_PIPE_BUSY) {
                WaitNamedPipeW(pipeName.c_str(), waitRemaining);
            } else {
                // 服务端处理完上一位客户端后会短暂关闭并重建实例。
                // ERROR_FILE_NOT_FOUND 在这个窗口内是可恢复状态。
                Sleep((std::min)(waitRemaining, 10UL));
            }
        }
        return L"ERROR";
    }

} // namespace CodexQuotaBar
