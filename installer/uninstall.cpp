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

namespace {

constexpr wchar_t kProductName[] = L"Codex-Quota-Bar";
constexpr wchar_t kAppDirectoryName[] = L"app";
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
        if (CreateProcessW(appPath.c_str(), command.data(), nullptr, nullptr, FALSE,
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
            // 隔离原主程序路径，防止卸载期间再次启动新实例。
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
    if (!registered.empty() && EqualPath(installDir, registered)) {
        return true;
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

bool IsRegisteredInstallDirectory(const std::filesystem::path& installDir) {
    return IsValidInstallDirectory(installDir);
}

void DeleteRegistrationAndShortcut() {
    RegDeleteTreeW(HKEY_CURRENT_USER, kUninstallKey);
    const std::wstring programs = KnownFolder(FOLDERID_Programs);
    if (!programs.empty()) {
        const std::filesystem::path shortcut =
            std::filesystem::path(programs) / L"Codex-Quota-Bar.lnk";
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

    // 最长重试约 20 秒，应对父卸载器以及安全软件的短暂扫描锁。
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

void RemoveSettingsData(const std::filesystem::path& installDir) {
    if (_wcsicmp(installDir.filename().c_str(), kAppDirectoryName) != 0 ||
        _wcsicmp(installDir.parent_path().filename().c_str(), kProductName) != 0) {
        return;
    }
    const std::filesystem::path dataDir =
        installDir.parent_path() / kDataDirectoryName;
    std::error_code error;
    std::filesystem::remove_all(dataDir, error);
}

void RemoveEmptyInstallRoot(const std::filesystem::path& installDir) {
    const std::filesystem::path installRoot = installDir.parent_path();
    if (_wcsicmp(installDir.filename().c_str(), kAppDirectoryName) != 0 ||
        _wcsicmp(installRoot.filename().c_str(), kProductName) != 0) {
        return;
    }
    std::error_code error;
    if (std::filesystem::is_empty(installRoot, error) && !error) {
        std::filesystem::remove(installRoot, error);
    }
}

bool ScheduleInstallRootRemoval(const std::filesystem::path& installDir) {
    const std::filesystem::path installRoot = installDir.parent_path();
    if (_wcsicmp(installDir.filename().c_str(), kAppDirectoryName) != 0 ||
        _wcsicmp(installRoot.filename().c_str(), kProductName) != 0) {
        return false;
    }

    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(installRoot, error)) {
        if (error || !EqualPath(entry.path(), installDir)) return false;
    }
    return MoveFileExW(installRoot.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT) != FALSE;
}

int Cleanup(const std::filesystem::path& installDir, bool removeData, bool quiet) {
    if (!IsRegisteredInstallDirectory(installDir)) {
        if (!quiet) {
            MessageBoxW(nullptr,
                        L"安装目录与当前用户注册信息不一致，卸载已中止。",
                        L"Codex-Quota-Bar 卸载程序",
                        MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
        }
        return 3;
    }

    const std::filesystem::path appPath = installDir / L"Codex-Quota-Bar.exe";

    // 1. 先关闭伴随模式的自动启动来源；失败时不进入文件删除阶段。
    if (!DeleteStartupValue()) {
        if (!quiet) {
            MessageBoxW(nullptr,
                        L"无法关闭伴随模式启动项，卸载入口已保留。",
                        L"Codex-Quota-Bar 卸载程序",
                        MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
        }
        return 5;
    }

    // 2. 自动启动来源已切断后，退出并确认安装目录内不再有进程。
    if (!StopInstalledApp(installDir, appPath)) {
        if (!quiet) {
            MessageBoxW(nullptr,
                        L"Codex-Quota-Bar 进程无法结束，卸载入口已保留，请稍后重试。",
                        L"Codex-Quota-Bar 卸载程序",
                        MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
        }
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
        return 7;
    }

    // 3. 最后删除程序与依赖。持久的 Windows 映像锁使用系统标准的
    // MOVEFILE_DELAY_UNTIL_REBOOT 兜底，在下次启动阶段完成清理。
    DWORD removalError = ERROR_SUCCESS;
    const bool removed = SafeRemoveInstallDirectory(installDir, removalError);
    const bool scheduled = !removed && ScheduleInstallDirectoryRemoval(installDir);
    if (removed || scheduled) {
        if (removeData) RemoveSettingsData(installDir);
        DeleteRegistrationAndShortcut();
        if (removed) {
            RemoveEmptyInstallRoot(installDir);
        } else {
            ScheduleInstallRootRemoval(installDir);
        }
    }

    if (!quiet) {
        const std::wstring message = removed
            ? L"Codex-Quota-Bar 已卸载完成，伴随模式已关闭。"
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
    return removed ? 0 : (scheduled ? ERROR_SUCCESS_REBOOT_REQUIRED : 3);
}

void ScheduleTempSelfDelete(const std::filesystem::path& tempExePath) {
    MoveFileExW(tempExePath.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);

    wchar_t systemDir[MAX_PATH] = {};
    const UINT len = GetSystemDirectoryW(systemDir, MAX_PATH);
    const std::wstring cmdExe = (len > 0 && len < MAX_PATH)
        ? (std::filesystem::path(systemDir) / L"cmd.exe").wstring()
        : L"cmd.exe";

    const std::wstring cmdLine =
        Quote(cmdExe) + L" /c (ping 127.0.0.1 -n 2 > nul & del /f /q " +
        Quote(tempExePath.wstring()) + L")";
    std::vector<wchar_t> cmdBuffer(cmdLine.begin(), cmdLine.end());
    cmdBuffer.push_back(L'\0');

    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    if (CreateProcessW(nullptr, cmdBuffer.data(), nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr,
                       (len > 0 && len < MAX_PATH) ? systemDir : nullptr,
                       &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

int RunStage2(const std::filesystem::path& installDir, DWORD waitPid, bool removeData, bool quiet) {
    if (!IsValidInstallDirectory(installDir)) {
        if (!quiet) {
            MessageBoxW(nullptr,
                        L"安装目录与当前用户注册信息不一致，卸载已中止。",
                        L"Codex-Quota-Bar 卸载程序",
                        MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
        }
        return 3;
    }

    // 1. 等待 Stage 1 进程完全退出并释放 Uninstall.exe 镜像锁
    if (waitPid > 0) {
        HANDLE hProcess = OpenProcess(SYNCHRONIZE, FALSE, waitPid);
        if (hProcess) {
            WaitForSingleObject(hProcess, 10000);
            CloseHandle(hProcess);
        }
        Sleep(100);
    }

    // 2. 幂等清理注册表和快捷方式
    DeleteStartupValue();
    if (removeData) {
        RemoveSettingsData(installDir);
    }
    DeleteRegistrationAndShortcut();

    // 3. 删除安装目录文件
    DWORD removalError = ERROR_SUCCESS;
    const bool removed = SafeRemoveInstallDirectory(installDir, removalError);
    const bool scheduled = !removed && ScheduleInstallDirectoryRemoval(installDir);
    if (removed) {
        RemoveEmptyInstallRoot(installDir);
    } else if (scheduled) {
        ScheduleInstallRootRemoval(installDir);
    }

    // 4. 用户交互反馈
    if (!quiet) {
        const std::wstring message = removed
            ? L"Codex-Quota-Bar 已卸载完成，伴随模式已关闭。"
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

    // 5. 调度清理位于 %TEMP% 的自身副本
    const std::wstring self = ModulePath();
    if (!self.empty()) {
        ScheduleTempSelfDelete(self);
    }

    return removed ? 0 : (scheduled ? ERROR_SUCCESS_REBOOT_REQUIRED : 3);
}

} // namespace

int APIENTRY wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 1;

    bool quiet = false;
    bool isStage2 = false;
    bool removeData = false;
    DWORD waitPid = 0;
    std::filesystem::path stage2InstallDir;

    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], L"/quiet") == 0 || _wcsicmp(argv[i], L"--quiet") == 0) {
            quiet = true;
        } else if (_wcsicmp(argv[i], L"--stage2") == 0) {
            isStage2 = true;
            if (i + 1 < argc && argv[i + 1][0] != L'-') {
                stage2InstallDir = FullPath(argv[++i]);
            }
        } else if (_wcsicmp(argv[i], L"--wait-pid") == 0) {
            if (i + 1 < argc) {
                waitPid = static_cast<DWORD>(_wtoi(argv[++i]));
            }
        } else if (_wcsicmp(argv[i], L"--remove-data") == 0) {
            removeData = true;
        }
    }
    LocalFree(argv);

    if (isStage2) {
        return RunStage2(stage2InstallDir, waitPid, removeData, quiet);
    }

    // Stage 1: 从主安装目录执行
    const std::wstring self = ModulePath();
    if (self.empty()) return 1;
    const std::filesystem::path installDir = FullPath(std::filesystem::path(self).parent_path());

    if (!IsValidInstallDirectory(installDir)) {
        if (!quiet) {
            MessageBoxW(nullptr,
                        L"安装目录与当前用户注册信息不一致，卸载已中止。",
                        L"Codex-Quota-Bar 卸载程序",
                        MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
        }
        return 3;
    }

    if (!quiet && MessageBoxW(
            nullptr, L"确定要卸载 Codex-Quota-Bar 吗？",
            L"Codex-Quota-Bar 卸载程序",
            MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND) != IDYES) {
        return 0;
    }

    if (!quiet) {
        removeData = MessageBoxW(
            nullptr,
            L"是否同时删除窗口位置、缩放、刷新间隔等本地设置？\n\n"
            L"选择“否”会保留设置，方便以后重新安装。",
            L"删除本地设置",
            MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND) == IDYES;
    }

    const std::filesystem::path appPath = installDir / L"Codex-Quota-Bar.exe";

    // 1. 先关闭伴随模式的自动启动来源
    if (!DeleteStartupValue()) {
        if (!quiet) {
            MessageBoxW(nullptr,
                        L"无法关闭伴随模式启动项，卸载入口已保留。",
                        L"Codex-Quota-Bar 卸载程序",
                        MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
        }
        return 5;
    }

    // 2. 终止主程序运行并隔离主程序二进制
    if (!StopInstalledApp(installDir, appPath)) {
        if (!quiet) {
            MessageBoxW(nullptr,
                        L"Codex-Quota-Bar 进程无法结束，卸载入口已保留，请稍后重试。",
                        L"Codex-Quota-Bar 卸载程序",
                        MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
        }
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
        return 7;
    }

    // 3. 删除用户配置（若选择）以及卸载注册表与快捷方式
    if (removeData) {
        RemoveSettingsData(installDir);
    }
    DeleteRegistrationAndShortcut();

    // 4. 将自身克隆到 %TEMP% 并启动 Stage 2 完成安装目录清理
    std::error_code ec;
    const std::filesystem::path tempDir = std::filesystem::temp_directory_path(ec);
    if (!ec && !tempDir.empty()) {
        const std::filesystem::path tempExe =
            tempDir / (L"Codex-Quota-Bar-Uninstall-" +
                       std::to_wstring(GetCurrentProcessId()) + L".exe");

        std::filesystem::copy_file(self, tempExe,
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (!ec) {
            std::wstring command = Quote(tempExe.wstring()) +
                                   L" --stage2 " + Quote(installDir.wstring()) +
                                   L" --wait-pid " + std::to_wstring(GetCurrentProcessId());
            if (quiet) {
                command += L" /quiet";
            }
            if (removeData) {
                command += L" --remove-data";
            }

            // 切换工作目录至临时目录，释放安装目录的目录句柄
            SetCurrentDirectoryW(tempDir.c_str());

            STARTUPINFOW si = { sizeof(si) };
            PROCESS_INFORMATION pi = {};
            std::vector<wchar_t> cmdBuffer(command.begin(), command.end());
            cmdBuffer.push_back(L'\0');

            if (CreateProcessW(tempExe.c_str(), cmdBuffer.data(), nullptr, nullptr,
                               FALSE, 0, nullptr, tempDir.c_str(), &si, &pi)) {
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
                return 0; // Stage 1 顺利完成，立即退出释放 app/Uninstall.exe 镜像锁
            }
        }
    }

    // 若临时克隆或启动失败，回退到原位执行
    return Cleanup(installDir, removeData, quiet);
}
