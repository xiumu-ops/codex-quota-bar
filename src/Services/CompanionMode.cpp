#include "Services/CompanionMode.h"

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

    bool CompanionMode::IsDesktopRunning() {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE) return false;

        bool found = false;
        PROCESSENTRY32W entry = {};
        entry.dwSize = sizeof(entry);
        if (Process32FirstW(snapshot, &entry)) {
            do {
                if (_wcsicmp(entry.szExeFile, L"ChatGPT.exe") != 0 &&
                    _wcsicmp(entry.szExeFile, L"Codex.exe") != 0) {
                    continue;
                }

                HANDLE process = OpenProcess(
                    PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
                if (!process) continue;

                std::wstring path(32768, L'\0');
                DWORD length = static_cast<DWORD>(path.size());
                const BOOL queried = QueryFullProcessImageNameW(
                    process, 0, path.data(), &length);
                CloseHandle(process);
                if (!queried || length == 0) continue;

                path.resize(length);
                if (IsDesktopExecutablePath(path)) {
                    found = true;
                    break;
                }
            } while (Process32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
        return found;
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
