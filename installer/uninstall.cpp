#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <knownfolders.h>
#include <shellapi.h>
#include <shlobj.h>

#include <cerrno>
#include <cstdint>
#include <string_view>
#include <filesystem>
#include <string>
#include "InstallSafety.h"

namespace {

constexpr wchar_t kProductName[] = L"Codex-Quota-Bar";
constexpr wchar_t kAppDirectoryName[] = L"app";
constexpr wchar_t kUninstallFilename[] = L"Uninstall.exe";
constexpr wchar_t kDataDirectoryName[] = L"data";
constexpr wchar_t kUninstallKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Codex-Quota-Bar";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"Codex-Quota-Bar-Companion";

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR value = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, 0, nullptr, &value))) return {};
    std::wstring result(value);
    CoTaskMemFree(value);
    return result;
}

std::wstring ModulePath() {
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(),
                                            static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) return {};
    path.resize(length);
    return path;
}

std::wstring Quote(const std::wstring& value) {
    return L"\"" + value + L"\"";
}

std::wstring FullPath(const std::filesystem::path& path) {
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetFullPathNameW(path.c_str(), static_cast<DWORD>(buffer.size()),
                                          buffer.data(), nullptr);
    if (length == 0 || length >= buffer.size()) return {};
    buffer.resize(length);
    while (!buffer.empty() && (buffer.back() == L'\\' || buffer.back() == L'/')) {
        buffer.pop_back();
    }
    return buffer;
}

bool EqualPath(const std::filesystem::path& left, const std::filesystem::path& right) {
    const std::wstring leftFull = FullPath(left);
    const std::wstring rightFull = FullPath(right);
    return !leftFull.empty() && !rightFull.empty() &&
           _wcsicmp(leftFull.c_str(), rightFull.c_str()) == 0;
}

bool DeleteStartupValue() {
    HKEY key = nullptr;
    const LSTATUS opened =
        RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key);
    if (opened == ERROR_FILE_NOT_FOUND) return true;
    if (opened != ERROR_SUCCESS) return false;
    const LSTATUS deleted = RegDeleteValueW(key, kRunValue);
    RegCloseKey(key);
    return deleted == ERROR_SUCCESS || deleted == ERROR_FILE_NOT_FOUND;
}

std::filesystem::path RegisteredPath(const wchar_t* valueName) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kUninstallKey, 0,
                      KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return {};
    }
    DWORD type = 0;
    DWORD bytes = 0;
    if (RegQueryValueExW(key, valueName, nullptr, &type, nullptr, &bytes) !=
            ERROR_SUCCESS ||
        type != REG_SZ || bytes < sizeof(wchar_t) || bytes > 64 * 1024) {
        RegCloseKey(key);
        return {};
    }
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    if (RegQueryValueExW(key, valueName, nullptr, nullptr,
                         reinterpret_cast<BYTE*>(value.data()), &bytes) != ERROR_SUCCESS) {
        RegCloseKey(key);
        return {};
    }
    RegCloseKey(key);
    while (!value.empty() && value.back() == L'\0') value.pop_back();
    return value;
}

bool IsValidInstallDirectory(const std::filesystem::path& installDir) {
    if (installDir.empty()) return false;
    if (_wcsicmp(installDir.filename().c_str(), kAppDirectoryName) != 0) return false;
    const auto installRoot = installDir.parent_path();
    if (_wcsicmp(installRoot.filename().c_str(), kProductName) != 0) return false;
    if (installRoot.parent_path().empty()) return false;

    // 1. 优先校验当前用户的卸载注册表登记位置
    const std::filesystem::path registered = RegisteredPath(L"InstallLocation");
    if (!registered.empty()) {
        return EqualPath(installDir, registered);
    }

    // 2. 校验是否位于当前用户的 LocalAppData 默认安装路径
    const std::wstring localAppData = KnownFolder(FOLDERID_LocalAppData);
    if (!localAppData.empty()) {
        const std::filesystem::path expected =
            std::filesystem::path(localAppData) / kProductName / kAppDirectoryName;
        if (EqualPath(installDir, expected)) {
            return true;
        }
    }

    return false;
}

std::wstring WindowsErrorText(DWORD errorCode) {
    wchar_t* message = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, errorCode, 0, reinterpret_cast<LPWSTR>(&message), 0, nullptr);
    std::wstring result = length > 0 && message ? std::wstring(message, length) : L"未知错误";
    if (message) LocalFree(message);
    while (!result.empty() &&
           (result.back() == L'\r' || result.back() == L'\n' || result.back() == L' ')) {
        result.pop_back();
    }
    return result;
}


constexpr wchar_t kWorkerPrefix[] = L"Codex-Quota-Bar-Uninstall-";

class ManagedFiles {
public:
    ~ManagedFiles() { CancelDeletion(); }

    DWORD OpenForRemoval(const std::filesystem::path& path) {
        DWORD error = ERROR_SUCCESS;
        for (int attempt = 0; attempt < 10; ++attempt) {
            CqbInstaller::Handle handle(CreateFileW(path.c_str(), DELETE | FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            if (handle) {
                FILE_ATTRIBUTE_TAG_INFO info = {};
                if (!GetFileInformationByHandleEx(handle.Get(), FileAttributeTagInfo, &info, sizeof(info))) {
                    return GetLastError();
                }
                if ((info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
                    return ERROR_REPARSE_POINT_ENCOUNTERED;
                }
                if ((info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) return ERROR_DIRECTORY;
                m_files.push_back(std::move(handle));
                return ERROR_SUCCESS;
            }
            error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return ERROR_SUCCESS;
            if (error != ERROR_SHARING_VIOLATION && error != ERROR_ACCESS_DENIED) break;
            Sleep(100);
        }
        return error;
    }

    DWORD MarkForDeletion() {
        FILE_DISPOSITION_INFO disposition = { TRUE };
        for (const auto& file : m_files) {
            if (!SetFileInformationByHandle(file.Get(), FileDispositionInfo, &disposition, sizeof(disposition))) {
                return GetLastError();
            }
            ++m_marked;
        }
        return ERROR_SUCCESS;
    }

    void Commit() {
        m_marked = 0;
        m_files.clear(); // The marked files are deleted when their handles close.
    }

private:
    void CancelDeletion() {
        FILE_DISPOSITION_INFO disposition = { FALSE };
        while (m_marked != 0) {
            --m_marked;
            SetFileInformationByHandle(m_files[m_marked].Get(), FileDispositionInfo,
                                       &disposition, sizeof(disposition));
        }
    }

    std::vector<CqbInstaller::Handle> m_files;
    std::size_t m_marked = 0;
};

DWORD DeleteRegistrationAndShortcut() {
    const std::wstring programs = KnownFolder(FOLDERID_Programs);
    if (!programs.empty()) {
        const auto shortcut = std::filesystem::path(programs) / L"Codex-Quota-Bar.lnk";
        if (!DeleteFileW(shortcut.c_str())) {
            const DWORD error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) return error;
        }
    }
    const auto status = RegDeleteTreeW(HKEY_CURRENT_USER, kUninstallKey);
    return status == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : static_cast<DWORD>(status);
}

int ReportFailure(const std::wstring& message, DWORD error, bool quiet) {
    if (!quiet) {
        const std::wstring detail = message + L"\n\n" + WindowsErrorText(error);
        MessageBoxW(nullptr, detail.c_str(), L"Codex-Quota-Bar 卸载程序",
                    MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
    }
    return 3;
}

int Cleanup(const std::filesystem::path& installDir, bool removeData, bool quiet) {
    if (!IsValidInstallDirectory(installDir)) {
        return ReportFailure(L"安装目录与当前用户注册信息不一致。", ERROR_INVALID_NAME, quiet);
    }

    CqbInstaller::DirectoryGuard appGuard;
    DWORD error = appGuard.Pin(installDir);
    if (error != ERROR_SUCCESS) {
        return ReportFailure(L"无法安全打开安装目录，卸载已中止。", error, quiet);
    }

    const auto installRoot = installDir.parent_path();
    const auto dataDir = installRoot / kDataDirectoryName;
    CqbInstaller::DirectoryGuard dataGuard;
    bool hasData = false;
    if (removeData) {
        error = dataGuard.Pin(dataDir);
        hasData = error == ERROR_SUCCESS;
        if (!hasData && error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
            return ReportFailure(L"无法安全打开配置目录，卸载已中止。", error, quiet);
        }
    }

    const auto appPath = installDir / L"Codex-Quota-Bar.exe";
    if (!CqbInstaller::StopInstalledApp(appPath)) {
        return ReportFailure(L"请先退出 Codex-Quota-Bar，再重试卸载。", ERROR_BUSY, quiet);
    }

    // Remove only files that this product writes. Keep the uninstall entry
    // available if any managed file is still locked or cannot be removed.
    ManagedFiles files;
    constexpr const wchar_t* appFiles[] = {
        L"Codex-Quota-Bar.exe", L"config-default.json", L"Codex-Quota-Bar.exe.uninstalling",
        L"Codex-Quota-Bar.exe.new", L"Uninstall.exe.new", L"config-default.json.new",
        L"Codex-Quota-Bar.exe.codex-quota-bar.rollback",
        L"Uninstall.exe.codex-quota-bar.rollback", L"config-default.json.codex-quota-bar.rollback",
        kUninstallFilename
    };
    for (const auto* name : appFiles) {
        error = files.OpenForRemoval(installDir / name);
        if (error != ERROR_SUCCESS) {
            return ReportFailure(L"无法删除程序文件，请关闭占用文件的程序后重试。\n" +
                                 std::wstring(name), error, quiet);
        }
    }

    if (hasData) {
        constexpr const wchar_t* dataFiles[] = {
            L"config-users.json", L"config-users.json.tmp", L"config.json",
            L"diagnostic.log", L"diagnostic.previous.log"
        };
        for (const auto* name : dataFiles) {
            error = files.OpenForRemoval(dataDir / name);
            if (error != ERROR_SUCCESS) {
                return ReportFailure(L"无法删除本地设置，请关闭占用文件的程序后重试。\n" +
                                     std::wstring(name), error, quiet);
            }
        }
    }

    error = files.MarkForDeletion();
    if (error != ERROR_SUCCESS) {
        return ReportFailure(L"程序文件仍被占用，请关闭相关程序后重试。", error, quiet);
    }
    if (!DeleteStartupValue()) {
        return ReportFailure(L"无法关闭伴随模式启动项，卸载入口已保留。", ERROR_ACCESS_DENIED, quiet);
    }
    error = DeleteRegistrationAndShortcut();
    if (error != ERROR_SUCCESS) {
        return ReportFailure(L"无法清理卸载注册信息。", error, quiet);
    }
    files.Commit();
    if (hasData) {
        dataGuard.ReleaseLeaf();
        RemoveDirectoryW(dataDir.c_str()); // Empty directories only.
    }
    dataGuard.Release();

    appGuard.ReleaseLeaf();
    RemoveDirectoryW(installDir.c_str());
    // The app guard pins the installation root and its ancestors.
    appGuard.ReleaseLeaf();
    RemoveDirectoryW(installRoot.c_str());
    if (!quiet) {
        MessageBoxW(nullptr, L"Codex-Quota-Bar 已卸载完成。",
                    L"Codex-Quota-Bar 卸载程序", MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
    }
    return 0;
}

bool IsWorkerPath(const std::filesystem::path& path) {
    const std::wstring name = path.filename().native();
    const std::wstring_view filename(name);
    const std::wstring_view prefix(kWorkerPrefix);
    constexpr std::wstring_view extension = L".exe";
    if (!filename.starts_with(prefix) || !filename.ends_with(extension) ||
        filename.size() <= prefix.size() + extension.size()) return false;
    const auto suffix = filename.substr(prefix.size(), filename.size() - prefix.size() - extension.size());
    std::error_code ec;
    const auto temp = std::filesystem::temp_directory_path(ec);
    return !ec && EqualPath(path.parent_path(), temp) &&
        suffix.find_first_not_of(L"0123456789-") == std::wstring_view::npos;
}

void DeleteWorkerAfterExit(const std::filesystem::path& self) {
    if (!IsWorkerPath(self)) return;
    wchar_t systemDir[MAX_PATH] = {};
    const UINT len = GetSystemDirectoryW(systemDir, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return;
    const auto system = std::filesystem::path(systemDir);
    const auto cmd = system / L"cmd.exe";
    const auto ping = system / L"ping.exe";
    // Only a generated basename enters the shell command. Pass the temp
    // directory through CreateProcess so shell syntax in a path is inert.
    const auto folder = self.parent_path();
    if (folder.root_name().native().size() != 2) return;
    std::wstring command = Quote(cmd.wstring()) + L" /D /V:OFF /C (" +
        Quote(ping.wstring()) + L" 127.0.0.1 -n 2 >nul & del /f /q " +
        Quote(self.filename().wstring()) + L")";
    STARTUPINFOW startup = { sizeof(startup) };
    PROCESS_INFORMATION process = {};
    if (CreateProcessW(cmd.c_str(), command.data(), nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, folder.c_str(), &startup, &process)) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
}

HANDLE ParseInheritedHandle(const wchar_t* text) {
    wchar_t* end = nullptr;
    errno = 0;
    const auto number = _wcstoui64(text, &end, 10);
    if (errno != 0 || end == text || *end != L'\0' || number == 0) return nullptr;
    return reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(number));
}

int RunStage2(const std::filesystem::path& installDir, HANDLE gate, HANDLE parent,
              bool removeData, bool quiet) {
    const auto self = std::filesystem::path(ModulePath());
    DWORD flags = 0;
    if (!IsWorkerPath(self) || !GetHandleInformation(gate, &flags) ||
        (flags & HANDLE_FLAG_INHERIT) == 0 || !GetHandleInformation(parent, &flags) ||
        (flags & HANDLE_FLAG_INHERIT) == 0 || GetProcessId(parent) == 0) {
        return 3;
    }
    const CqbInstaller::Handle inheritedGate(gate);
    const CqbInstaller::Handle inheritedParent(parent);
    SetHandleInformation(inheritedGate.Get(), HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(inheritedParent.Get(), HANDLE_FLAG_INHERIT, 0);
    const int result = WaitForSingleObject(inheritedParent.Get(), 10000) == WAIT_OBJECT_0
        ? Cleanup(installDir, removeData, quiet)
        : ReportFailure(L"卸载启动进程尚未退出，请稍后重试。", ERROR_BUSY, quiet);
    DeleteWorkerAfterExit(self);
    return result;
}

bool StartCleanupWorker(const std::filesystem::path& self, const std::filesystem::path& installDir,
                        HANDLE gate, bool removeData, bool quiet) {
    std::error_code ec;
    const auto tempPath = std::filesystem::temp_directory_path(ec);
    if (ec || tempPath.empty()) return false;
    const std::filesystem::path temp = FullPath(tempPath);
    CqbInstaller::DirectoryGuard tempGuard;
    if (tempGuard.Pin(temp) != ERROR_SUCCESS) return false;

    HANDLE rawParent = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &rawParent,
                         SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, TRUE, 0)) return false;
    const CqbInstaller::Handle parent(rawParent);
    HANDLE inherited[] = { gate, parent.Get() };

    SIZE_T attributeBytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
    if (attributeBytes == 0) return false;
    std::vector<BYTE> attributes(attributeBytes);
    STARTUPINFOEXW startup = {};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attributeBytes)) return false;

    const auto worker = temp / (kWorkerPrefix + std::to_wstring(GetCurrentProcessId()) + L"-" +
                                std::to_wstring(GetTickCount64()) + L".exe");
    bool started = false;
    if (UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                  inherited, sizeof(inherited), nullptr, nullptr) &&
        CopyFileW(self.c_str(), worker.c_str(), TRUE)) {
        std::wstring command = Quote(worker.wstring()) + L" --stage2 " + Quote(installDir.wstring()) +
            L" --gate " + std::to_wstring(reinterpret_cast<std::uintptr_t>(gate)) +
            L" --parent " + std::to_wstring(reinterpret_cast<std::uintptr_t>(parent.Get()));
        if (quiet) command += L" /quiet";
        if (removeData) command += L" --remove-data";
        PROCESS_INFORMATION process = {};
        {
            // Keep the copy immutable while starting it.
            CqbInstaller::Handle executable(CreateFileW(worker.c_str(), GENERIC_READ, FILE_SHARE_READ,
                nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            FILE_ATTRIBUTE_TAG_INFO info = {};
            started = executable && GetFileInformationByHandleEx(
                executable.Get(), FileAttributeTagInfo, &info, sizeof(info)) &&
                (info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0 &&
                CreateProcessW(worker.c_str(), command.data(), nullptr, nullptr, TRUE,
                               EXTENDED_STARTUPINFO_PRESENT, nullptr, temp.c_str(),
                               &startup.StartupInfo, &process);
        }
        if (started) {
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
        } else {
            DeleteFileW(worker.c_str());
        }
    }
    DeleteProcThreadAttributeList(startup.lpAttributeList);
    return started;
}

} // namespace

int APIENTRY wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 1;

    bool quiet = false;
    bool isStage2 = false;
    bool removeData = false;
    HANDLE gate = nullptr;
    HANDLE parent = nullptr;
    std::filesystem::path stage2InstallDir;
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], L"/quiet") == 0 || _wcsicmp(argv[i], L"--quiet") == 0) {
            quiet = true;
        } else if (_wcsicmp(argv[i], L"--stage2") == 0 && i + 1 < argc) {
            isStage2 = true;
            stage2InstallDir = FullPath(argv[++i]);
        } else if (_wcsicmp(argv[i], L"--gate") == 0 && i + 1 < argc) {
            gate = ParseInheritedHandle(argv[++i]);
        } else if (_wcsicmp(argv[i], L"--parent") == 0 && i + 1 < argc) {
            parent = ParseInheritedHandle(argv[++i]);
        } else if (_wcsicmp(argv[i], L"--remove-data") == 0) {
            removeData = true;
        }
    }
    LocalFree(argv);
    if (isStage2) return RunStage2(stage2InstallDir, gate, parent, removeData, quiet);

    const auto self = std::filesystem::path(ModulePath());
    if (self.empty()) return 1;
    const std::filesystem::path installDir = FullPath(self.parent_path());
    if (!IsValidInstallDirectory(installDir)) {
        return ReportFailure(L"安装目录与当前用户注册信息不一致。", ERROR_INVALID_NAME, quiet);
    }
    if (!quiet && MessageBoxW(nullptr, L"确定要卸载 Codex-Quota-Bar 吗？",
            L"Codex-Quota-Bar 卸载程序", MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND) != IDYES) {
        return 0;
    }
    if (!quiet) {
        removeData = MessageBoxW(nullptr, L"是否同时删除窗口位置、缩放、刷新间隔等本地设置？\n\n"
            L"选择“否”会保留设置，方便以后重新安装。", L"删除本地设置",
            MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND) == IDYES;
    }

    const auto operation = CqbInstaller::AcquireInstallationGate(true);
    if (!operation) {
        return ReportFailure(L"另一个安装或卸载操作正在运行。", ERROR_BUSY, quiet);
    }
    CqbInstaller::DirectoryGuard appGuard;
    const DWORD error = appGuard.Pin(installDir);
    if (error != ERROR_SUCCESS) {
        return ReportFailure(L"无法安全打开安装目录，卸载已中止。", error, quiet);
    }
    if (!StartCleanupWorker(self, installDir, operation.Get(), removeData, quiet)) {
        return ReportFailure(L"无法启动卸载清理进程，卸载入口已保留。", ERROR_ACCESS_DENIED, quiet);
    }
    return 0;
}
