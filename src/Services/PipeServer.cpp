#include "Services/PipeServer.h"
#include "Services/PipeClient.h"
#include "Core/Logger.h"
#include "Core/WinHandle.h"

#include <sddl.h>
#include <algorithm>
#include <exception>
#include <system_error>
#include <memory>
#include <vector>

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
        const std::lock_guard lock(m_lifecycleMutex);
        if (m_running.exchange(true)) return true;
        if (m_serverThread.joinable()) m_serverThread.join();
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
        const std::lock_guard lock(m_lifecycleMutex);
        const bool wasRunning = m_running.exchange(false);
        if (wasRunning && m_stopEvent) SetEvent(m_stopEvent);
        if (m_serverThread.joinable()) {
            m_serverThread.join();
        }
    }

    namespace {
        constexpr DWORD kClientIoTimeoutMs = 1000;
        constexpr DWORD kReplyGraceMs = 350;

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
        // TokenUser identifies the same account for standard and elevated
        // launches. The object owner may instead be the Administrators group.
        HANDLE rawToken = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken)) return INVALID_HANDLE_VALUE;
        const WinHandle token(rawToken);
        DWORD bytes = 0;
        GetTokenInformation(token.Get(), TokenUser, nullptr, 0, &bytes);
        if (!bytes) return INVALID_HANDLE_VALUE;
        std::vector<BYTE> user(bytes);
        if (!GetTokenInformation(token.Get(), TokenUser, user.data(), bytes, &bytes)) {
            return INVALID_HANDLE_VALUE;
        }
        LPWSTR rawSid = nullptr;
        if (!ConvertSidToStringSidW(reinterpret_cast<const TOKEN_USER*>(user.data())->User.Sid,
                                    &rawSid)) return INVALID_HANDLE_VALUE;
        const std::unique_ptr<wchar_t, decltype(&LocalFree)> sid(rawSid, LocalFree);
        const std::wstring sddl = L"D:P(A;;GA;;;SY)(A;;GA;;;" +
                                  std::wstring(sid.get()) + L")S:(ML;;NW;;;ME)";
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) {
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
            1,
            4096, 4096, 0, &attributes);
        LocalFree(descriptor);
        return pipe;
    }

    void PipeServer::ServerThreadLoop(HANDLE initialPipe) {
        HANDLE hPipe = initialPipe;
        unsigned consecutiveErrors = 0;
        while (m_running) {
            OVERLAPPED ovl = {};
            ovl.hEvent = m_ioEvent;
            ResetEvent(m_ioEvent);

            BOOL connected = ConnectNamedPipe(hPipe, &ovl);
            DWORD connectError = ERROR_SUCCESS;
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
                    if (!connected) connectError = GetLastError();
                } else if (err != ERROR_PIPE_CONNECTED) {
                    connectError = err;
                } else {
                    connected = TRUE;
                }
            }
            if (!connected) {
                const bool transient = connectError == ERROR_NO_DATA || connectError == ERROR_BROKEN_PIPE;
                if (connectError == ERROR_INVALID_HANDLE || (!transient && ++consecutiveErrors >= 5)) {
                    WriteLog(LogLevel::Error, L"IPC 连接失败，服务已停止。错误码：" +
                             std::to_wstring(connectError));
                    break;
                }
                DisconnectNamedPipe(hPipe);
                if (WaitForSingleObject(m_stopEvent, transient ? 10 : 50) == WAIT_OBJECT_0) break;
                continue;
            }
            consecutiveErrors = 0;

            if (connected && m_running) {
                constexpr DWORD kChunkBytes = 1024 * sizeof(wchar_t);
                std::wstring cmd;
                cmd.reserve(kMaxPipeCommandBytes / sizeof(wchar_t));
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
                    if (moreData && cmd.size() * sizeof(wchar_t) >= kMaxPipeCommandBytes) {
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
                    if (!writeOk) return;

                    // A normal client reads the reply then closes its end. Wait
                    // for that close instead of dropping unread reply bytes.
                    // A client that never reads cannot block the server forever.
                    char extra = 0;
                    DWORD read = 0;
                    OVERLAPPED drain = {};
                    drain.hEvent = m_ioEvent;
                    ResetEvent(m_ioEvent);
                    if (!ReadFile(hPipe, &extra, sizeof(extra), &read, &drain) &&
                        GetLastError() == ERROR_IO_PENDING &&
                        WaitForSingleObject(m_ioEvent, kReplyGraceMs) != WAIT_OBJECT_0) {
                        CancelAndDrainIo(hPipe, drain);
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
        return SendPipeCommand(pipeName, command, timeoutMs);
    }

} // namespace CodexQuotaBar
