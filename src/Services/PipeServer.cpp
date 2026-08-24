#include "Services/PipeServer.h"

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
        try {
            m_serverThread = std::thread(&PipeServer::ServerThreadLoop, this);
        } catch (...) {
            m_running = false;
            return false;
        }
        return true;
    }

    void PipeServer::Stop() {
        if (!m_running.exchange(false)) return;
        SetEvent(m_stopEvent);
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
            CancelIoEx(pipe, &ovl);
            DWORD transferred = 0;
            GetOverlappedResult(pipe, &ovl, &transferred, TRUE);
        }
    }

    void PipeServer::ServerThreadLoop() {
        while (m_running) {
            HANDLE hPipe = CreateNamedPipeW(
                m_pipeName.c_str(),
                PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                PIPE_UNLIMITED_INSTANCES,
                4096, 4096, 0, NULL);

            if (hPipe == INVALID_HANDLE_VALUE) {
                if (WaitForSingleObject(m_stopEvent, 100) == WAIT_OBJECT_0) {
                    break;
                }
                continue;
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
                        break;
                    }
                    if (wait != WAIT_OBJECT_0) {
                        CancelAndDrainIo(hPipe, ovl);
                        CloseHandle(hPipe);
                        break;
                    }
                    DWORD transferred = 0;
                    connected = GetOverlappedResult(hPipe, &ovl, &transferred, FALSE);
                    if (!connected) {
                        CloseHandle(hPipe);
                        continue;
                    }
                } else if (err != ERROR_PIPE_CONNECTED) {
                    CloseHandle(hPipe);
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
                    writeReply(m_handler ? m_handler(cmd) : L"OK");
                }
            }

            DisconnectNamedPipe(hPipe);
            CloseHandle(hPipe);
        }
    }

    std::wstring PipeServer::SendCommand(const std::wstring& pipeName, const std::wstring& command, DWORD timeoutMs) {
        wchar_t replyBuf[2048] = { 0 };

        for (int attempt = 0;; ++attempt) {
            DWORD bytesRead = 0;
            BOOL ok = CallNamedPipeW(
                pipeName.c_str(),
                const_cast<wchar_t*>(command.c_str()),
                static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)),
                replyBuf,
                sizeof(replyBuf),
                &bytesRead,
                timeoutMs);

            if (ok && bytesRead >= sizeof(wchar_t)) {
                size_t charCount = bytesRead / sizeof(wchar_t);
                if (charCount > 0 && replyBuf[charCount - 1] == L'\0') --charCount;
                return std::wstring(replyBuf, charCount);
            }
            if (attempt < 2 && !ok && GetLastError() == ERROR_PIPE_BUSY &&
                WaitNamedPipeW(pipeName.c_str(), timeoutMs)) {
                continue;
            }
            break;
        }
        return L"ERROR";
    }

} // namespace CodexQuotaBar
