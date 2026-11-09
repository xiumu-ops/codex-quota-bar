#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <string>

#include "../Core/Constants.h"

namespace CodexQuotaBar {

    inline constexpr DWORD kMaxPipeCommandBytes = 64 * 1024;

    inline std::wstring CurrentPipeName() {
        DWORD sessionId = 0;
        ProcessIdToSessionId(GetCurrentProcessId(), &sessionId);
        return std::wstring(PIPE_NAME_PREFIX) + std::to_wstring(sessionId);
    }

    inline std::wstring SendPipeCommand(
        const std::wstring& pipeName, const std::wstring& command,
        DWORD timeoutMs, ULONG expectedServerPid = 0) {
        if (command.size() >= kMaxPipeCommandBytes / sizeof(wchar_t)) return L"ERROR";
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
        if (expectedServerPid != 0) {
            ULONG serverPid = 0;
            if (!GetNamedPipeServerProcessId(pipe, &serverPid) || serverPid != expectedServerPid) {
                CloseHandle(pipe);
                return L"ERROR";
            }
        }

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
                    CancelIoEx(pipe, &ovl);
                    GetOverlappedResult(pipe, &ovl, &bytesRead, TRUE);
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
