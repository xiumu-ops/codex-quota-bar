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
#include <tlhelp32.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "HookConfig.h"

namespace {

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

bool IsSubPathOf(const std::filesystem::path& child, const std::filesystem::path& parent) {
    const std::wstring childFull = FullPath(child);
    const std::wstring parentFull = FullPath(parent);
    if (childFull.empty() || parentFull.empty()) return false;
    if (childFull.size() <= parentFull.size()) return false;
    if (_wcsnicmp(childFull.c_str(), parentFull.c_str(), parentFull.size()) != 0) return false;
    return childFull[parentFull.size()] == L'\\' || childFull[parentFull.size()] == L'/';
}

std::vector<DWORD> GetProcessIdsRunningFrom(const std::filesystem::path& targetDirOrFile) {
    std::vector<DWORD> pids;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return pids;

    const DWORD currentPid = GetCurrentProcessId();
    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (entry.th32ProcessID == 0 || entry.th32ProcessID == currentPid) continue;

            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                         entry.th32ProcessID);
            if (!process) continue;

            std::wstring path(32768, L'\0');
            DWORD length = static_cast<DWORD>(path.size());
            const BOOL queried = QueryFullProcessImageNameW(process, 0, path.data(), &length);
            CloseHandle(process);
            if (queried && length > 0) {
                path.resize(length);
                if (EqualPath(path, targetDirOrFile) || IsSubPathOf(path, targetDirOrFile)) {
                    pids.push_back(entry.th32ProcessID);
                }
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return pids;
}

bool StopInstalledApp(const std::filesystem::path& installDir,
                      const std::filesystem::path& appPath) {
    std::error_code error;
    if (std::filesystem::exists(appPath, error)) {
        std::wstring command = Quote(appPath.wstring()) + L" --exit";
        STARTUPINFOW startup = { sizeof(startup) };
        PROCESS_INFORMATION process = {};
        if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                           CREATE_NO_WINDOW, nullptr, appPath.parent_path().c_str(),
                           &startup, &process)) {
            WaitForSingleObject(process.hProcess, 3000);
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
        }
    }

    // 1. 先等待 IPC 优雅退出（最多 1.5 秒）
    for (int attempt = 0; attempt < 15; ++attempt) {
        if (GetProcessIdsRunningFrom(installDir).empty()) return true;
        Sleep(100);
    }

    // 2. 针对任何挂起或残留的程序进程，强制终止
    const auto remainingPids = GetProcessIdsRunningFrom(installDir);
    for (DWORD pid : remainingPids) {
        HANDLE hProcess = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
        if (hProcess) {
            TerminateProcess(hProcess, 0);
            WaitForSingleObject(hProcess, 2000);
            CloseHandle(hProcess);
        }
    }
    return GetProcessIdsRunningFrom(installDir).empty();
}

bool QuarantineAppExecutable(const std::filesystem::path& installDir,
                             const std::filesystem::path& appPath,
                             DWORD& errorCode) {
    errorCode = ERROR_SUCCESS;
    const std::filesystem::path quarantined = appPath.wstring() + L".uninstalling";
    for (int attempt = 0; attempt < 100; ++attempt) {
        StopInstalledApp(installDir, appPath);
        std::error_code error;
        if (!std::filesystem::exists(appPath, error)) return true;
        if (MoveFileExW(appPath.c_str(), quarantined.c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            // 活动 Codex 会话即使缓存了旧 Hook，也无法再通过原路径派生实例。
            return true;
        }
        errorCode = GetLastError();
        Sleep(200);
    }
    return false;
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
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kUninstallKey, 0,
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

std::filesystem::path RegisteredHookFilePath() {
    return RegisteredPath(L"HookFilePath");
}

std::filesystem::path RegisteredUserDataPath() {
    return RegisteredPath(L"UserDataDir");
}

bool IsRegisteredInstallDirectory(const std::filesystem::path& installDir) {
    const std::filesystem::path registered = RegisteredPath(L"InstallLocation");
    return !registered.empty() && EqualPath(installDir, registered) &&
           _wcsicmp(installDir.filename().c_str(), L"Codex-Quota-Bar") == 0 &&
           !installDir.parent_path().empty();
}

void DeleteRegistrationAndShortcut() {
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, kUninstallKey);
    const std::wstring commonPrograms = KnownFolder(FOLDERID_CommonPrograms);
    if (!commonPrograms.empty()) {
        const std::filesystem::path shortcut =
            std::filesystem::path(commonPrograms) / L"Codex-Quota-Bar.lnk";
        DeleteFileW(shortcut.c_str());
    }
}

bool SafeRemoveInstallDirectory(const std::filesystem::path& installDir,
                                DWORD& errorCode) {
    errorCode = ERROR_SUCCESS;
    if (!IsRegisteredInstallDirectory(installDir)) {
        errorCode = ERROR_INVALID_NAME;
        return false;
    }

    // 最长重试约 20 秒，应对父卸载器、Hook 子进程以及安全软件的短暂扫描锁。
    for (int attempt = 0; attempt < 100; ++attempt) {
        std::error_code error;
        if (!std::filesystem::exists(installDir, error)) return true;

        SetFileAttributesW(installDir.c_str(), FILE_ATTRIBUTE_NORMAL);
        for (const auto& entry : std::filesystem::recursive_directory_iterator(installDir, error)) {
            if (error) break;
            SetFileAttributesW(entry.path().c_str(), FILE_ATTRIBUTE_NORMAL);
        }

        std::filesystem::remove_all(installDir, error);
        if (error) errorCode = static_cast<DWORD>(error.value());
        error.clear();
        if (!std::filesystem::exists(installDir, error)) return true;
        if (error) errorCode = static_cast<DWORD>(error.value());

        Sleep(200);
    }

    std::error_code error;
    if (std::filesystem::is_empty(installDir, error) && !error) {
        SetFileAttributesW(installDir.c_str(), FILE_ATTRIBUTE_NORMAL);
        if (RemoveDirectoryW(installDir.c_str())) return true;
        errorCode = GetLastError();
    }

    const bool missing = !std::filesystem::exists(installDir, error);
    if (!missing && errorCode == ERROR_SUCCESS) {
        errorCode = error ? static_cast<DWORD>(error.value()) : ERROR_ACCESS_DENIED;
    }
    return missing;
}

bool ScheduleInstallDirectoryRemoval(const std::filesystem::path& installDir) {
    if (!IsRegisteredInstallDirectory(installDir)) return false;

    std::error_code error;
    if (!std::filesystem::exists(installDir, error)) return true;

    std::vector<std::filesystem::path> directories;
    directories.push_back(installDir);
    for (const auto& entry : std::filesystem::recursive_directory_iterator(installDir, error)) {
        if (error) return false;
        if (entry.is_directory(error)) {
            if (error) return false;
            directories.push_back(entry.path());
            continue;
        }
        SetFileAttributesW(entry.path().c_str(), FILE_ATTRIBUTE_NORMAL);
        if (!MoveFileExW(entry.path().c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT)) {
            return false;
        }
    }

    // 启动阶段按登记顺序执行：先删除文件，再从最深层目录向上删除目录。
    std::sort(directories.begin(), directories.end(),
              [](const std::filesystem::path& left,
                 const std::filesystem::path& right) {
                  return left.native().size() > right.native().size();
              });
    for (const auto& directory : directories) {
        SetFileAttributesW(directory.c_str(), FILE_ATTRIBUTE_NORMAL);
        if (!MoveFileExW(directory.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT)) {
            return false;
        }
    }
    return true;
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

void RemoveSettingsData() {
    std::filesystem::path dataDir = RegisteredUserDataPath();
    std::error_code error;
    if (!dataDir.empty()) std::filesystem::remove_all(dataDir, error);
    const std::wstring localAppData = KnownFolder(FOLDERID_LocalAppData);
    if (!localAppData.empty()) {
        const std::filesystem::path currentDataDir =
            std::filesystem::path(localAppData) / L"Codex-Quota-Bar";
        std::filesystem::remove_all(currentDataDir, error);
    }
}

void RemoveStaleTemporaryHelpers(const std::wstring& currentHelper) {
    wchar_t tempDir[MAX_PATH] = {};
    if (GetTempPathW(MAX_PATH, tempDir) == 0) return;
    const std::filesystem::path tempRoot = tempDir;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(tempRoot, error)) {
        if (error || !entry.is_regular_file(error)) continue;
        const std::wstring name = entry.path().filename().wstring();
        if (name.rfind(L"Codex-Quota-Bar-Uninstall-", 0) != 0 ||
            entry.path().extension() != L".exe" ||
            EqualPath(entry.path(), currentHelper)) {
            continue;
        }
        SetFileAttributesW(entry.path().c_str(), FILE_ATTRIBUTE_NORMAL);
        DeleteFileW(entry.path().c_str());
    }
}

void ScheduleSelfDelete(const std::wstring& selfPath) {
    wchar_t systemDirectory[MAX_PATH] = {};
    if (GetSystemDirectoryW(systemDirectory, MAX_PATH) == 0) return;
    const std::filesystem::path cmdPath =
        std::filesystem::path(systemDirectory) / L"cmd.exe";
    std::wstring command = Quote(cmdPath.wstring()) +
        L" /d /c ping.exe 127.0.0.1 -n 2 >nul & del /f /q " + Quote(selfPath);
    STARTUPINFOW startup = { sizeof(startup) };
    PROCESS_INFORMATION process = {};
    if (CreateProcessW(cmdPath.c_str(), command.data(), nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr,
                       &startup, &process)) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
}

int Cleanup(const std::filesystem::path& installDir, bool removeData,
            DWORD parentPid, bool quiet) {
    if (parentPid != 0) {
        HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, parentPid);
        if (parent) {
            // 父进程不等待清理器，不存在循环等待；必须等其映像完全卸载，
            // 否则 Program Files 中的 Uninstall.exe 会触发错误 32。
            WaitForSingleObject(parent, INFINITE);
            CloseHandle(parent);
            Sleep(1000); // 确保父进程句柄与文件映射完全解开
        } else {
            // 父进程可能已抢先退出，仍给映像段和安全扫描器留出释放时间。
            Sleep(1000);
        }
    }

    if (!IsRegisteredInstallDirectory(installDir)) {
        if (!quiet) {
            MessageBoxW(nullptr,
                        L"安装目录与系统注册信息不一致，卸载已中止。",
                        L"Codex-Quota-Bar 卸载程序",
                        MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
        }
        const std::wstring currentHelper = ModulePath();
        RemoveStaleTemporaryHelpers(currentHelper);
        ScheduleSelfDelete(currentHelper);
        return 3;
    }

    const std::filesystem::path appPath = installDir / L"Codex-Quota-Bar.exe";

    // 1. 先精确撤销本软件写入的 Hook，保留用户已有 Hook 与其他元数据。
    std::filesystem::path hookFilePath = RegisteredHookFilePath();
    if (hookFilePath.empty()) {
        hookFilePath = CodexQuotaBarInstaller::HookConfig::ResolveHookFilePath();
    }
    const auto hookResult = CodexQuotaBarInstaller::HookConfig::Remove(hookFilePath);
    if (!hookResult.success) {
        if (!quiet) {
            MessageBoxW(nullptr,
                        (L"无法安全移除 Codex 会话同步 Hook，卸载已中止：\n\n" +
                         hookResult.error).c_str(),
                        L"Codex-Quota-Bar 卸载程序",
                        MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
        }
        const std::wstring currentHelper = ModulePath();
        RemoveStaleTemporaryHelpers(currentHelper);
        ScheduleSelfDelete(currentHelper);
        return 4;
    }

    // 2. 再关闭伴随模式的自动启动来源；失败时不进入文件删除阶段。
    if (!DeleteStartupValue()) {
        if (!quiet) {
            MessageBoxW(nullptr,
                        L"无法关闭伴随模式启动项，卸载入口已保留。",
                        L"Codex-Quota-Bar 卸载程序",
                        MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
        }
        const std::wstring currentHelper = ModulePath();
        RemoveStaleTemporaryHelpers(currentHelper);
        ScheduleSelfDelete(currentHelper);
        return 5;
    }

    // 3. 所有重新启动来源均已切断后，退出并确认安装目录内不再有进程。
    if (!StopInstalledApp(installDir, appPath)) {
        if (!quiet) {
            MessageBoxW(nullptr,
                        L"Codex-Quota-Bar 进程无法结束，卸载入口已保留，请稍后重试。",
                        L"Codex-Quota-Bar 卸载程序",
                        MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
        }
        const std::wstring currentHelper = ModulePath();
        RemoveStaleTemporaryHelpers(currentHelper);
        ScheduleSelfDelete(currentHelper);
        return 6;
    }

    DWORD quarantineError = ERROR_SUCCESS;
    if (!QuarantineAppExecutable(installDir, appPath, quarantineError)) {
        if (!quiet) {
            const std::wstring message =
                L"无法隔离正在使用的主程序，卸载入口已保留。\n\nWindows 错误 " +
                std::to_wstring(quarantineError) + L"：" +
                WindowsErrorText(quarantineError);
            MessageBoxW(nullptr, message.c_str(), L"Codex-Quota-Bar 卸载程序",
                        MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
        }
        const std::wstring currentHelper = ModulePath();
        RemoveStaleTemporaryHelpers(currentHelper);
        ScheduleSelfDelete(currentHelper);
        return 7;
    }

    // 4. 最后删除程序与依赖。持久的 Windows 映像锁使用系统标准的
    // MOVEFILE_DELAY_UNTIL_REBOOT 兜底，在下次启动阶段完成清理。
    DWORD removalError = ERROR_SUCCESS;
    const bool removed = SafeRemoveInstallDirectory(installDir, removalError);
    const bool scheduled = !removed && removalError == ERROR_SHARING_VIOLATION &&
                           ScheduleInstallDirectoryRemoval(installDir);
    if (removed || scheduled) {
        DeleteRegistrationAndShortcut();
        if (removeData) RemoveSettingsData();
    }

    if (!quiet) {
        const std::wstring message = removed
            ? L"Codex-Quota-Bar 已卸载完成，Hook 与伴随模式已关闭。"
            : scheduled
                ? L"Codex-Quota-Bar 已卸载。Windows 正在占用残留文件，已登记在下次系统启动时自动删除；请重启一次电脑。"
                : L"无法删除安装目录，卸载入口已保留。\n\nWindows 错误 " +
                      std::to_wstring(removalError) + L"：" +
                      WindowsErrorText(removalError);
        MessageBoxW(nullptr, message.c_str(),
                    L"Codex-Quota-Bar 卸载程序",
                    MB_OK | ((removed || scheduled) ? MB_ICONINFORMATION : MB_ICONERROR) |
                        MB_SETFOREGROUND);
    }
    const std::wstring currentHelper = ModulePath();
    RemoveStaleTemporaryHelpers(currentHelper);
    ScheduleSelfDelete(currentHelper);
    return removed ? 0 : (scheduled ? ERROR_SUCCESS_REBOOT_REQUIRED : 3);
}

} // namespace

int APIENTRY wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 1;

    if (argc >= 5 && _wcsicmp(argv[1], L"--cleanup") == 0) {
        const std::filesystem::path installDir = argv[2];
        const bool removeData = wcstol(argv[3], nullptr, 10) != 0;
        const DWORD parentPid = static_cast<DWORD>(wcstoul(argv[4], nullptr, 10));
        bool quiet = false;
        for (int i = 5; i < argc; ++i) {
            if (_wcsicmp(argv[i], L"/quiet") == 0) quiet = true;
        }
        LocalFree(argv);
        return Cleanup(installDir, removeData, parentPid, quiet);
    }

    bool quiet = false;
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], L"/quiet") == 0) quiet = true;
    }
    LocalFree(argv);

    if (!quiet && MessageBoxW(
            nullptr, L"确定要卸载 Codex-Quota-Bar 吗？",
            L"Codex-Quota-Bar 卸载程序",
            MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND) != IDYES) {
        return 0;
    }

    bool removeData = false;
    if (!quiet) {
        removeData = MessageBoxW(
            nullptr,
            L"是否同时删除窗口位置、缩放、刷新间隔等本地设置？\n\n"
            L"选择“否”会保留设置，方便以后重新安装。",
            L"删除本地设置",
            MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND) == IDYES;
    }

    const std::wstring self = ModulePath();
    if (self.empty()) return 1;
    const std::filesystem::path installDir = std::filesystem::path(self).parent_path();

    wchar_t tempDir[MAX_PATH] = {};
    if (GetTempPathW(MAX_PATH, tempDir) == 0) return 1;
    const std::filesystem::path helper = std::filesystem::path(tempDir) /
        (L"Codex-Quota-Bar-Uninstall-" + std::to_wstring(GetCurrentProcessId()) + L".exe");
    if (!CopyFileW(self.c_str(), helper.c_str(), FALSE)) {
        if (!quiet) {
            MessageBoxW(nullptr, L"无法启动临时卸载程序。", L"卸载失败",
                        MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
        }
        return 2;
    }

    std::wstring parameters = L"--cleanup " + Quote(installDir.wstring()) + L" " +
        (removeData ? L"1" : L"0") + L" " + std::to_wstring(GetCurrentProcessId());
    if (quiet) parameters += L" /quiet";

    SHELLEXECUTEINFOW execute = { sizeof(execute) };
    execute.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    // 临时清理程序负责删除 Program Files 中的文件，必须明确保持管理员权限。
    execute.lpVerb = L"runas";
    execute.lpFile = helper.c_str();
    execute.lpParameters = parameters.c_str();
    execute.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&execute)) {
        DeleteFileW(helper.c_str());
        if (!quiet) {
            MessageBoxW(nullptr, L"无法启动卸载清理程序。", L"卸载失败",
                        MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
        }
        return 2;
    }
    if (execute.hProcess) CloseHandle(execute.hProcess);
    return 0;
}
