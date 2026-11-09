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
        constexpr DWORD kMaxCommandBytes = 64 * 1024;

        DWORD WaitIoOrStop(HANDLE ioEvent, HANDLE stopEvent, DWORD timeoutMs = INFINITE) {
            HANDLE events[] = { ioEvent, stopEvent };
            return WaitForMultipleObjects(2, events, FALSE, timeoutMs);
        }

        void CancelAndDrainIo(HANDLE pipe, OVERLAPPED& ovl) {
            CancelIoEx(pipe, &ovl);
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
            OVERLAPPED ovl = {};
            ovl.hEvent = m_ioEvent;
            ResetEvent(m_ioEvent);

            BOOL connected = ConnectNamedPipe(hPipe, &ovl);
            if (!connected) {
                DWORD err = GetLastError();
                if (err == ERROR_IO_PENDING) {
                    DWORD wait = WaitIoOrStop(m_ioEvent, m_stopEvent);
                    if (wait != WAIT_OBJECT_0) {
                        CancelAndDrainIo(hPipe, ovl);
                        break;
                    }
                    DWORD transferred = 0;
                    connected = GetOverlappedResult(hPipe, &ovl, &transferred, FALSE);
                    if (!connected) {
                        if (GetLastError() == ERROR_INVALID_HANDLE) {
                            WriteLog(LogLevel::Error, L"IPC 管道句柄无效，服务已停止。");
                            break;
                        }
                        DisconnectNamedPipe(hPipe);
                        continue;
                    }
                } else if (err != ERROR_PIPE_CONNECTED) {
                    if (err == ERROR_INVALID_HANDLE) {
                        WriteLog(LogLevel::Error, L"IPC 管道句柄无效，服务已停止。");
                        break;
                    }
                    DisconnectNamedPipe(hPipe);
                    continue;
                } else {
                    connected = TRUE;
                }
            }

            if (connected && m_running) {
                constexpr DWORD kChunkBytes = 1024 * sizeof(wchar_t);
                std::wstring cmd;
                cmd.reserve(kMaxCommandBytes / sizeof(wchar_t));
                BOOL readOk = TRUE;
                bool overLimit = false;

                while (readOk && !overLimit) {
                    wchar_t part[1024] = { 0 };
                    DWORD bytesRead = 0;
                    bool moreData = false;
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
                            moreData = true;
                        }
                    }
                    if (!readOk || bytesRead % sizeof(wchar_t) != 0) {
                        readOk = FALSE;
                        break;
                    }

                    cmd.append(part, bytesRead / sizeof(wchar_t));
                    if (moreData && cmd.size() * sizeof(wchar_t) >= kMaxCommandBytes) {
                        overLimit = true;
                        break;
                    }
                    if (!moreData) break;
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

            if (!DisconnectNamedPipe(hPipe) && GetLastError() == ERROR_INVALID_HANDLE) {
                WriteLog(LogLevel::Error, L"IPC 管道句柄无效，服务已停止。");
                break;
            }
        }
        if (hPipe != INVALID_HANDLE_VALUE) {
            CloseHandle(hPipe);
        }
        m_running = false;
    }

    std::wstring PipeServer::SendCommand(const std::wstring& pipeName, const std::wstring& command, DWORD timeoutMs) {
        if (command.size() >= kMaxCommandBytes / sizeof(wchar_t)) return L"ERROR";
        const ULONGLONG deadline = GetTickCount64() + (std::clamp)(timeoutMs, 1UL, INFINITE - 1UL);
        const auto remainingTime = [deadline]() -> DWORD {
            const ULONGLONG now = GetTickCount64();
            return now < deadline ? static_cast<DWORD>(deadline - now) : 0;
        };

        HANDLE pipe = INVALID_HANDLE_VALUE;
        while (remainingTime() != 0) {
            pipe = CreateFileW(
                pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
            if (pipe != INVALID_HANDLE_VALUE) break;

            const DWORD error = GetLastError();
            const DWORD waitRemaining = remainingTime();
            if (waitRemaining == 0) break;
            if (error == ERROR_PIPE_BUSY) {
                if (!WaitNamedPipeW(pipeName.c_str(), waitRemaining) &&
                    GetLastError() != ERROR_FILE_NOT_FOUND) break;
            } else if (error == ERROR_FILE_NOT_FOUND) {
                Sleep((std::min)(waitRemaining, 10UL));
            } else {
                break;
            }
        }
        if (pipe == INVALID_HANDLE_VALUE) return L"ERROR";

        wchar_t replyBuf[2048] = {};
        DWORD bytesRead = 0;
        DWORD mode = PIPE_READMODE_MESSAGE;
        OVERLAPPED ovl = {};
        ovl.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        BOOL ok = FALSE;
        if (ovl.hEvent && SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr) &&
            remainingTime() != 0) {
            // Once submitted, never retry a command that may have already run.
            ok = TransactNamedPipe(
                pipe, const_cast<wchar_t*>(command.c_str()),
                static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)),
                replyBuf, sizeof(replyBuf), &bytesRead, &ovl);
            if (!ok && GetLastError() == ERROR_IO_PENDING) {
                if (WaitForSingleObject(ovl.hEvent, remainingTime()) == WAIT_OBJECT_0) {
                    ok = GetOverlappedResult(pipe, &ovl, &bytesRead, FALSE);
                } else {
                    CancelAndDrainIo(pipe, ovl);
                }
            }
        }
        CloseHandle(pipe);
        if (ovl.hEvent) CloseHandle(ovl.hEvent);

        if (!ok || bytesRead < sizeof(wchar_t) || bytesRead % sizeof(wchar_t) != 0) {
            return L"ERROR";
        }
        size_t charCount = bytesRead / sizeof(wchar_t);
        if (replyBuf[charCount - 1] == L'\0') --charCount;
        return std::wstring(replyBuf, charCount);
    }

} // namespace CodexQuotaBar
