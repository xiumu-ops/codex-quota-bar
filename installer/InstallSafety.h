#pragma once

#include "../src/Services/PipeClient.h"

#include <tlhelp32.h>
#include <filesystem>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace CqbInstaller {

class Handle {
public:
    explicit Handle(HANDLE handle = nullptr) : m_handle(handle) {}
    ~Handle() { if (*this) CloseHandle(m_handle); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : m_handle(std::exchange(other.m_handle, nullptr)) {}
    explicit operator bool() const { return m_handle && m_handle != INVALID_HANDLE_VALUE; }
    HANDLE Get() const { return m_handle; }

private:
    HANDLE m_handle;
};

// The inherited handle keeps setup locked out while the first uninstaller
// process exits and the cleanup worker takes over.
inline Handle AcquireInstallationGate(bool inherit = false) {
    SECURITY_ATTRIBUTES attributes = { sizeof(attributes), nullptr, inherit ? TRUE : FALSE };
    const HANDLE gate = CreateEventW(&attributes, TRUE, FALSE, L"Local\\Codex-Quota-Bar_Installation");
    if (gate && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(gate);
        return Handle{};
    }
    return Handle(gate);
}

class DirectoryGuard {
public:
    DWORD Pin(const std::filesystem::path& absolute) {
        const auto drive = absolute.root_name().native();
        if (!absolute.is_absolute() || drive.size() != 2 || drive[1] != L':') {
            return ERROR_INVALID_NAME;
        }
        auto path = absolute.root_path();
        for (const auto& component : absolute.relative_path()) {
            if (component.empty() || component == L"." || component == L"..") {
                return ERROR_INVALID_NAME;
            }
            path /= component;
            // Inspect the directory entry itself and prevent replacement or
            // reparse-point changes while its children are being accessed.
            Handle handle(CreateFileW(path.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
                                      FILE_SHARE_READ,
                                      nullptr, OPEN_EXISTING,
                                      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            if (!handle) return GetLastError();
            FILE_ATTRIBUTE_TAG_INFO info = {};
            if (!GetFileInformationByHandleEx(handle.Get(), FileAttributeTagInfo, &info, sizeof(info))) {
                return GetLastError();
            }
            if ((info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
                return ERROR_REPARSE_POINT_ENCOUNTERED;
            }
            if ((info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) return ERROR_DIRECTORY;
            m_handles.push_back(std::move(handle));
        }
        return ERROR_SUCCESS;
    }

    void ReleaseLeaf() { m_handles.pop_back(); }
    void Release() { m_handles.clear(); }

private:
    std::vector<Handle> m_handles;
};

inline bool StopInstalledApp(const std::filesystem::path& appPath) {
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot) return false;

    DWORD currentSession = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &currentSession)) return false;
    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    if (!Process32FirstW(snapshot.Get(), &entry)) return false;
    const auto appName = appPath.filename().native();
    do {
        if (_wcsicmp(entry.szExeFile, appName.c_str()) != 0) continue;
        Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
                                   FALSE, entry.th32ProcessID));
        if (!process) continue;
        wchar_t image[32768] = {};
        DWORD length = static_cast<DWORD>(std::size(image));
        if (!QueryFullProcessImageNameW(process.Get(), 0, image, &length) ||
            _wcsicmp(image, appPath.c_str()) != 0) continue;
        DWORD session = 0;
        if (ProcessIdToSessionId(entry.th32ProcessID, &session) && session == currentSession) {
            // Never launch an executable from the install directory. Only the
            // pipe owned by the matched process may receive the exit request.
            CodexQuotaBar::SendPipeCommand(CodexQuotaBar::CurrentPipeName(), L"EXIT", 1500,
                                           entry.th32ProcessID);
        }
        if (WaitForSingleObject(process.Get(), 2000) != WAIT_OBJECT_0) return false;
    } while (Process32NextW(snapshot.Get(), &entry));
    return GetLastError() == ERROR_NO_MORE_FILES;
}

} // namespace CqbInstaller
