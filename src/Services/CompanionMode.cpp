#include "Services/CompanionMode.h"
#include "Core/WinHandle.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cwctype>
#include <mutex>
#include <optional>

namespace CodexQuotaBar {
namespace {

    constexpr wchar_t kRunKeyPath[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    constexpr wchar_t kRunValueName[] = L"Codex-Quota-Bar-Companion";

    std::wstring Lowercase(std::wstring value) {
        std::transform(value.begin(), value.end(), value.begin(),
            [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
        return value;
    }

} // namespace

    bool CompanionMode::IsDesktopExecutablePath(const std::wstring& path) {
        const std::wstring lower = Lowercase(path);
        const bool recognizedInstall =
            lower.find(L"\\windowsapps\\openai.codex_") != std::wstring::npos ||
            lower.find(L"\\openai\\codex\\") != std::wstring::npos;
        if (!recognizedInstall) return false;

        const size_t slash = lower.find_last_of(L"\\/");
        const std::wstring filename = slash == std::wstring::npos
            ? lower : lower.substr(slash + 1);
        if (filename == L"chatgpt.exe") return true;
        if (filename != L"codex.exe") return false;

        // 桌面包内 app\Codex.exe 可作为启动器；resources\codex.exe 和
        // LocalAppData\...\bin\codex.exe 都是 CLI/App Server，必须排除。
        return lower.find(L"\\resources\\codex.exe") == std::wstring::npos &&
               lower.find(L"\\bin\\") == std::wstring::npos;
    }

    DesktopProcessState CompanionMode::ProbeDesktopState() {
        // Retaining the verified process handle also prevents PID reuse from
        // identifying an unrelated process as the desktop application.
        static std::mutex cacheMutex;
        static std::optional<WinHandle> cachedProcess;
        const std::lock_guard lock(cacheMutex);
        if (cachedProcess) {
            const DWORD wait = WaitForSingleObject(cachedProcess->Get(), 0);
            if (wait == WAIT_TIMEOUT) return DesktopProcessState::Running;
            cachedProcess.reset();
        }

        DWORD currentSession = 0;
        if (!ProcessIdToSessionId(GetCurrentProcessId(), &currentSession)) {
            return DesktopProcessState::QueryFailed;
        }
        WinHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
        if (!snapshot) return DesktopProcessState::QueryFailed;

        bool queryFailed = false;
        PROCESSENTRY32W entry = {};
        entry.dwSize = sizeof(entry);
        if (Process32FirstW(snapshot.Get(), &entry)) {
            do {
                if (_wcsicmp(entry.szExeFile, L"ChatGPT.exe") != 0 &&
                    _wcsicmp(entry.szExeFile, L"Codex.exe") != 0) continue;
                DWORD processSession = 0;
                if (!ProcessIdToSessionId(entry.th32ProcessID, &processSession) ||
                    processSession != currentSession) continue;

                HANDLE opened = OpenProcess(
                    PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, entry.th32ProcessID);
                const bool canWait = opened != nullptr;
                if (!opened) {
                    opened = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
                }
                WinHandle process(opened);
                if (!process) {
                    queryFailed = true;
                    continue;
                }
                std::wstring path(32768, L'\0');
                DWORD length = static_cast<DWORD>(path.size());
                if (!QueryFullProcessImageNameW(process.Get(), 0, path.data(), &length) || length == 0) {
                    queryFailed = true;
                    continue;
                }
                path.resize(length);
                if (IsDesktopExecutablePath(path)) {
                    if (canWait) {
                        const DWORD wait = WaitForSingleObject(process.Get(), 0);
                        if (wait == WAIT_OBJECT_0) continue;
                        if (wait == WAIT_TIMEOUT) cachedProcess.emplace(std::move(process));
                    }
                    return DesktopProcessState::Running;
                }
            } while (Process32NextW(snapshot.Get(), &entry));
        } else if (GetLastError() != ERROR_NO_MORE_FILES) {
            queryFailed = true;
        }
        return queryFailed ? DesktopProcessState::QueryFailed : DesktopProcessState::NotRunning;
    }

    bool CompanionMode::IsDesktopRunning() {
        return ProbeDesktopState() == DesktopProcessState::Running;
    }

    bool CompanionMode::ConfigureAutoStart(bool enabled) {
        if (!enabled) {
            HKEY key = nullptr;
            const LSTATUS opened = RegOpenKeyExW(
                HKEY_CURRENT_USER, kRunKeyPath, 0, KEY_SET_VALUE, &key);
            if (opened == ERROR_FILE_NOT_FOUND) return true;
            if (opened != ERROR_SUCCESS) return false;
            const LSTATUS deleted = RegDeleteValueW(key, kRunValueName);
            RegCloseKey(key);
            return deleted == ERROR_SUCCESS || deleted == ERROR_FILE_NOT_FOUND;
        }

        std::wstring executable(32768, L'\0');
        const DWORD capacity = static_cast<DWORD>(executable.size());
        DWORD length = GetModuleFileNameW(nullptr, executable.data(), capacity);
        if (length == 0 || length >= capacity) return false;
        executable.resize(length);
        const std::wstring command = L"\"" + executable + L"\"";

        HKEY key = nullptr;
        if (RegCreateKeyExW(
                HKEY_CURRENT_USER, kRunKeyPath, 0, nullptr, 0,
                KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
            return false;
        }
        const LSTATUS written = RegSetValueExW(
            key, kRunValueName, 0, REG_SZ,
            reinterpret_cast<const BYTE*>(command.c_str()),
            static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(key);
        return written == ERROR_SUCCESS;
    }

} // namespace CodexQuotaBar
