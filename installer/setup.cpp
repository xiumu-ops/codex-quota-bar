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
#include <shobjidl.h>
#include <tlhelp32.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "HookConfig.h"
#include "resource.h"

namespace {

constexpr wchar_t kProductName[] = L"Codex-Quota-Bar";
constexpr wchar_t kAppDirectoryName[] = L"app";
constexpr wchar_t kDataDirectoryName[] = L"data";
constexpr wchar_t kAppFilename[] = L"Codex-Quota-Bar.exe";
constexpr wchar_t kUninstallFilename[] = L"Uninstall.exe";
constexpr wchar_t kDefaultConfigFilename[] = L"config-default.json";
constexpr wchar_t kUninstallKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Codex-Quota-Bar";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"Codex-Quota-Bar-Companion";

struct FileRollback {
    std::filesystem::path target;
    std::filesystem::path backup;
    bool existed = false;
    bool prepared = false;
};

enum class FolderSelectionResult {
    Selected,
    Canceled,
    Failed
};

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR value = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &value))) return {};
    std::wstring result(value);
    CoTaskMemFree(value);
    return result;
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

bool IsQuietInstall() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool quiet = false;
    if (argv) {
        for (int i = 1; i < argc; ++i) {
            if (_wcsicmp(argv[i], L"/quiet") == 0 || _wcsicmp(argv[i], L"/s") == 0) {
                quiet = true;
            }
        }
        LocalFree(argv);
    }
    return quiet;
}

FolderSelectionResult ChooseInstallParent(
    const std::filesystem::path& suggestedParent,
    std::filesystem::path& selectedParent) {
    IFileOpenDialog* dialog = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&dialog));
    if (FAILED(hr)) return FolderSelectionResult::Failed;

    DWORD options = 0;
    hr = dialog->GetOptions(&options);
    if (SUCCEEDED(hr)) {
        hr = dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM |
                                FOS_PATHMUSTEXIST | FOS_DONTADDTORECENT);
    }
    if (SUCCEEDED(hr)) {
        hr = dialog->SetTitle(
            L"选择安装位置（将自动创建 Codex-Quota-Bar\\app 与 data）");
    }

    IShellItem* suggestedItem = nullptr;
    if (SUCCEEDED(hr) &&
        SUCCEEDED(SHCreateItemFromParsingName(suggestedParent.c_str(), nullptr,
                                              IID_PPV_ARGS(&suggestedItem)))) {
        dialog->SetFolder(suggestedItem);
        dialog->SetDefaultFolder(suggestedItem);
        suggestedItem->Release();
    }

    if (SUCCEEDED(hr)) hr = dialog->Show(nullptr);
    if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        dialog->Release();
        return FolderSelectionResult::Canceled;
    }
    if (FAILED(hr)) {
        dialog->Release();
        return FolderSelectionResult::Failed;
    }

    IShellItem* result = nullptr;
    hr = dialog->GetResult(&result);
    dialog->Release();
    if (FAILED(hr) || !result) return FolderSelectionResult::Failed;

    PWSTR path = nullptr;
    hr = result->GetDisplayName(SIGDN_FILESYSPATH, &path);
    result->Release();
    if (FAILED(hr) || !path || !*path) {
        if (path) CoTaskMemFree(path);
        return FolderSelectionResult::Failed;
    }
    selectedParent = path;
    CoTaskMemFree(path);
    return FolderSelectionResult::Selected;
}

bool ExtractResource(int resourceId, const std::filesystem::path& destination) {
    HMODULE module = GetModuleHandleW(nullptr);
    HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(resourceId), RT_RCDATA);
    if (!resource) return false;
    HGLOBAL loaded = LoadResource(module, resource);
    if (!loaded) return false;
    const DWORD size = SizeofResource(module, resource);
    const void* bytes = LockResource(loaded);
    if (!bytes || size == 0) return false;

    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) return false;
    output.write(static_cast<const char*>(bytes), static_cast<std::streamsize>(size));
    output.flush();
    return output.good();
}

bool ReplacePayload(int resourceId, const std::filesystem::path& destination) {
    const std::filesystem::path temporary = destination.wstring() + L".new";
    std::error_code error;
    std::filesystem::remove(temporary, error);
    if (!ExtractResource(resourceId, temporary)) return false;

    for (int attempt = 0; attempt < 25; ++attempt) {
        if (MoveFileExW(temporary.c_str(), destination.c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            return true;
        }
        Sleep(200);
    }
    std::filesystem::remove(temporary, error);
    return false;
}

bool PrepareFileRollback(const std::filesystem::path& target, FileRollback& rollback) {
    rollback.target = target;
    rollback.backup = target.wstring() + L".codex-quota-bar.rollback";
    std::error_code error;
    std::filesystem::remove(rollback.backup, error);
    error.clear();
    rollback.existed = std::filesystem::exists(target, error);
    if (error) return false;
    if (rollback.existed) {
        std::filesystem::copy_file(target, rollback.backup,
                                   std::filesystem::copy_options::overwrite_existing, error);
        if (error) return false;
    }
    rollback.prepared = true;
    return true;
}

void RestoreFileRollback(const FileRollback& rollback) {
    if (!rollback.prepared) return;
    if (rollback.existed) {
        if (!MoveFileExW(rollback.backup.c_str(), rollback.target.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            // 保留回滚副本，避免一次恢复失败又把最后的可恢复文件删除。
            return;
        }
    } else {
        DeleteFileW(rollback.target.c_str());
    }
    DeleteFileW(rollback.backup.c_str());
}

void CommitFileRollback(const FileRollback& rollback) {
    if (rollback.prepared) DeleteFileW(rollback.backup.c_str());
}

void RequestExistingAppExit(const std::filesystem::path& installDir, const std::filesystem::path& appPath) {
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

    // 1. 等待优雅退出
    for (int attempt = 0; attempt < 15; ++attempt) {
        if (GetProcessIdsRunningFrom(installDir).empty()) return;
        Sleep(100);
    }

    // 2. 终止任何仍占有安装目录的残留旧版实例
    const auto remainingPids = GetProcessIdsRunningFrom(installDir);
    for (DWORD pid : remainingPids) {
        HANDLE hProcess = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
        if (hProcess) {
            TerminateProcess(hProcess, 0);
            WaitForSingleObject(hProcess, 2000);
            CloseHandle(hProcess);
        }
    }
}

bool CreateStartMenuShortcut(const std::filesystem::path& shortcut,
                             const std::filesystem::path& appPath) {
    IShellLinkW* link = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&link));
    if (FAILED(hr)) return false;

    link->SetPath(appPath.c_str());
    link->SetWorkingDirectory(appPath.parent_path().c_str());
    link->SetDescription(L"查看 Codex 使用限额");
    link->SetIconLocation(appPath.c_str(), 0);

    IPersistFile* persist = nullptr;
    hr = link->QueryInterface(IID_PPV_ARGS(&persist));
    if (SUCCEEDED(hr)) {
        hr = persist->Save(shortcut.c_str(), TRUE);
        persist->Release();
    }
    link->Release();
    return SUCCEEDED(hr);
}

bool SetRegistryString(HKEY key, const wchar_t* name, const std::wstring& value) {
    return RegSetValueExW(
        key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
        static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

bool RegisterUninstaller(const std::filesystem::path& installDir,
                         const std::filesystem::path& appPath,
                         const std::filesystem::path& uninstallPath,
                         const std::filesystem::path& defaultConfigPath,
                         const std::filesystem::path& hookFilePath,
                         const std::filesystem::path& userDataDir) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kUninstallKey, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }

    const std::wstring uninstallCommand = Quote(uninstallPath.wstring());
    const std::wstring quietCommand = uninstallCommand + L" /quiet";
    bool ok =
        SetRegistryString(key, L"DisplayName", kProductName) &&
        SetRegistryString(key, L"DisplayVersion", L"2.5.3") &&
        SetRegistryString(key, L"Publisher", L"xiumu-ops") &&
        SetRegistryString(key, L"InstallLocation", installDir.wstring()) &&
        SetRegistryString(key, L"HookFilePath", hookFilePath.wstring()) &&
        SetRegistryString(key, L"UserDataDir", userDataDir.wstring()) &&
        SetRegistryString(key, L"DisplayIcon", Quote(appPath.wstring()) + L",0") &&
        SetRegistryString(key, L"UninstallString", uninstallCommand) &&
        SetRegistryString(key, L"QuietUninstallString", quietCommand) &&
        SetRegistryString(key, L"URLInfoAbout", L"https://github.com/xiumu-ops/codex-quota-bar");

    DWORD one = 1;
    ok = ok && RegSetValueExW(key, L"NoModify", 0, REG_DWORD,
                              reinterpret_cast<const BYTE*>(&one), sizeof(one)) == ERROR_SUCCESS;
    ok = ok && RegSetValueExW(key, L"NoRepair", 0, REG_DWORD,
                              reinterpret_cast<const BYTE*>(&one), sizeof(one)) == ERROR_SUCCESS;

    std::uintmax_t totalBytes = 0;
    std::error_code error;
    const std::uintmax_t appBytes = std::filesystem::file_size(appPath, error);
    if (!error) totalBytes += appBytes;
    error.clear();
    const std::uintmax_t uninstallBytes = std::filesystem::file_size(uninstallPath, error);
    if (!error) totalBytes += uninstallBytes;
    error.clear();
    const std::uintmax_t defaultConfigBytes =
        std::filesystem::file_size(defaultConfigPath, error);
    if (!error) totalBytes += defaultConfigBytes;
    const DWORD estimatedKb = static_cast<DWORD>((totalBytes + 1023) / 1024);
    ok = ok && RegSetValueExW(key, L"EstimatedSize", 0, REG_DWORD,
                              reinterpret_cast<const BYTE*>(&estimatedKb),
                              sizeof(estimatedKb)) == ERROR_SUCCESS;

    RegCloseKey(key);
    return ok;
}

bool HasUserUninstallRegistration() {
    HKEY key = nullptr;
    const LSTATUS status = RegOpenKeyExW(
        HKEY_CURRENT_USER, kUninstallKey, 0, KEY_QUERY_VALUE, &key);
    if (status == ERROR_SUCCESS) RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

bool HasLegacySystemInstallation() {
    HKEY key = nullptr;
    const LSTATUS status = RegOpenKeyExW(
        HKEY_LOCAL_MACHINE, kUninstallKey, 0, KEY_QUERY_VALUE, &key);
    if (status == ERROR_SUCCESS) RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

std::filesystem::path RegisteredInstallDirectory() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kUninstallKey, 0,
                      KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return {};
    }
    DWORD type = 0;
    DWORD bytes = 0;
    if (RegQueryValueExW(key, L"InstallLocation", nullptr, &type, nullptr, &bytes) !=
            ERROR_SUCCESS ||
        type != REG_SZ || bytes < sizeof(wchar_t) || bytes > 64 * 1024) {
        RegCloseKey(key);
        return {};
    }
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    if (RegQueryValueExW(key, L"InstallLocation", nullptr, nullptr,
                         reinterpret_cast<BYTE*>(value.data()), &bytes) != ERROR_SUCCESS) {
        RegCloseKey(key);
        return {};
    }
    RegCloseKey(key);
    while (!value.empty() && value.back() == L'\0') value.pop_back();
    return value;
}

void UpdateExistingCompanionStartup(const std::filesystem::path& appPath) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0,
                      KEY_QUERY_VALUE | KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
        return;
    }
    DWORD type = 0;
    if (RegQueryValueExW(key, kRunValue, nullptr, &type, nullptr, nullptr) == ERROR_SUCCESS &&
        (type == REG_SZ || type == REG_EXPAND_SZ)) {
        SetRegistryString(key, kRunValue, Quote(appPath.wstring()));
    }
    RegCloseKey(key);
}

void RollBackInstall(const FileRollback& appRollback,
                     const FileRollback& uninstallRollback,
                     const FileRollback& defaultConfigRollback,
                     const FileRollback& shortcutRollback,
                     bool existingUserInstall,
                     const std::filesystem::path& installDir,
                     const std::filesystem::path& appPath,
                     const std::filesystem::path& uninstallPath,
                     const std::filesystem::path& defaultConfigPath,
                     const std::filesystem::path& hookFilePath,
                     const std::filesystem::path& userDataDir) {
    RegDeleteTreeW(HKEY_CURRENT_USER, kUninstallKey);
    RestoreFileRollback(shortcutRollback);
    RestoreFileRollback(defaultConfigRollback);
    RestoreFileRollback(uninstallRollback);
    RestoreFileRollback(appRollback);
    if (existingUserInstall) {
        RegisterUninstaller(
            installDir, appPath, uninstallPath, defaultConfigPath,
            hookFilePath, userDataDir);
    } else {
        std::error_code error;
        if (std::filesystem::is_empty(installDir, error) && !error) {
            std::filesystem::remove(installDir, error);
        }
        const std::filesystem::path installRoot = installDir.parent_path();
        error.clear();
        if (_wcsicmp(installRoot.filename().c_str(), kProductName) == 0 &&
            std::filesystem::is_empty(installRoot, error) && !error) {
            std::filesystem::remove(installRoot, error);
        }
    }
}

void ReportFailure(const std::wstring& message, bool quiet) {
    if (!quiet) {
        MessageBoxW(nullptr, message.c_str(), L"Codex-Quota-Bar 安装程序",
                    MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
    }
}

} // namespace

int APIENTRY wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    const bool quiet = IsQuietInstall();
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    const std::wstring localAppData = KnownFolder(FOLDERID_LocalAppData);
    const std::wstring programs = KnownFolder(FOLDERID_Programs);
    if (localAppData.empty() || programs.empty()) {
        ReportFailure(L"无法定位当前用户的应用目录。", quiet);
        if (SUCCEEDED(comResult)) CoUninitialize();
        return 1;
    }

    if (HasLegacySystemInstallation()) {
        ReportFailure(
            L"检测到旧的系统级安装。请先从 Windows“已安装的应用”中卸载旧版，"
            L"然后重新运行本安装包。\n\n新版本不会静默保留旧目录、Hook 或启动项。",
            quiet);
        if (SUCCEEDED(comResult)) CoUninitialize();
        return 7;
    }

    std::filesystem::path installDir = RegisteredInstallDirectory();
    if (installDir.empty()) {
        std::filesystem::path installParent = localAppData;
        if (!quiet) {
            const FolderSelectionResult selection =
                ChooseInstallParent(installParent, installParent);
            if (selection == FolderSelectionResult::Canceled) {
                if (SUCCEEDED(comResult)) CoUninitialize();
                return 0;
            }
            if (selection == FolderSelectionResult::Failed) {
                ReportFailure(L"无法打开安装位置选择器。", quiet);
                if (SUCCEEDED(comResult)) CoUninitialize();
                return 2;
            }
        }
        const std::filesystem::path installRoot =
            _wcsicmp(installParent.filename().c_str(), kProductName) == 0
            ? installParent
            : installParent / kProductName;
        installDir = installRoot / kAppDirectoryName;
    }
    const std::filesystem::path installRoot = installDir.parent_path();
    if (_wcsicmp(installDir.filename().c_str(), kAppDirectoryName) != 0 ||
        _wcsicmp(installRoot.filename().c_str(), kProductName) != 0 ||
        installRoot.parent_path().empty()) {
        ReportFailure(L"安装目录必须使用 Codex-Quota-Bar\\app 的独立结构。", quiet);
        if (SUCCEEDED(comResult)) CoUninitialize();
        return 2;
    }
    const std::filesystem::path appPath = installDir / kAppFilename;
    const std::filesystem::path uninstallPath = installDir / kUninstallFilename;
    const std::filesystem::path defaultConfigPath =
        installDir / kDefaultConfigFilename;
    const std::filesystem::path shortcut =
        std::filesystem::path(programs) / L"Codex-Quota-Bar.lnk";
    const std::filesystem::path userDataDir = installRoot / kDataDirectoryName;

    std::error_code error;
    if (std::filesystem::exists(installDir, error) && !error &&
        !std::filesystem::is_empty(installDir, error) && !error &&
        !std::filesystem::exists(appPath, error) &&
        !std::filesystem::exists(uninstallPath, error)) {
        ReportFailure(L"所选安装目录不是空目录。请选择其他位置。", quiet);
        if (SUCCEEDED(comResult)) CoUninitialize();
        return 2;
    }
    error.clear();
    std::filesystem::create_directories(installDir, error);
    if (error) {
        ReportFailure(L"无法创建安装目录：\n" + installDir.wstring(), quiet);
        if (SUCCEEDED(comResult)) CoUninitialize();
        return 2;
    }

    RequestExistingAppExit(installDir, appPath);

    bool existingUserInstall = std::filesystem::exists(appPath, error);
    if (!error && !existingUserInstall) {
        existingUserInstall = std::filesystem::exists(uninstallPath, error);
    }
    if (error) {
        ReportFailure(L"无法检查现有安装状态。", quiet);
        if (SUCCEEDED(comResult)) CoUninitialize();
        return 3;
    }
    existingUserInstall = existingUserInstall || HasUserUninstallRegistration();

    FileRollback appRollback;
    FileRollback uninstallRollback;
    FileRollback defaultConfigRollback;
    FileRollback shortcutRollback;
    if (!PrepareFileRollback(appPath, appRollback) ||
        !PrepareFileRollback(uninstallPath, uninstallRollback) ||
        !PrepareFileRollback(defaultConfigPath, defaultConfigRollback) ||
        !PrepareFileRollback(shortcut, shortcutRollback)) {
        RestoreFileRollback(shortcutRollback);
        RestoreFileRollback(defaultConfigRollback);
        RestoreFileRollback(uninstallRollback);
        RestoreFileRollback(appRollback);
        ReportFailure(L"无法创建升级回滚备份，安装未开始。", quiet);
        if (SUCCEEDED(comResult)) CoUninitialize();
        return 3;
    }

    const std::filesystem::path hookFilePath =
        CodexQuotaBarInstaller::HookConfig::ResolveHookFilePath();
    if (!ReplacePayload(IDR_APP_PAYLOAD, appPath) ||
        !ReplacePayload(IDR_UNINSTALL_PAYLOAD, uninstallPath) ||
        !ReplacePayload(IDR_DEFAULT_CONFIG_PAYLOAD, defaultConfigPath)) {
        RollBackInstall(appRollback, uninstallRollback, defaultConfigRollback, shortcutRollback,
                        existingUserInstall, installDir, appPath,
                        uninstallPath, defaultConfigPath, hookFilePath, userDataDir);
        ReportFailure(L"无法写入程序文件。请退出正在运行的旧版本后重试。", quiet);
        if (SUCCEEDED(comResult)) CoUninitialize();
        return 3;
    }

    if (!CreateStartMenuShortcut(shortcut, appPath)) {
        RollBackInstall(appRollback, uninstallRollback, defaultConfigRollback, shortcutRollback,
                        existingUserInstall, installDir, appPath,
                        uninstallPath, defaultConfigPath, hookFilePath, userDataDir);
        ReportFailure(L"无法创建当前用户的开始菜单快捷方式，安装已回滚。", quiet);
        if (SUCCEEDED(comResult)) CoUninitialize();
        return 4;
    }

    if (!RegisterUninstaller(
            installDir, appPath, uninstallPath, defaultConfigPath,
            hookFilePath, userDataDir)) {
        RollBackInstall(appRollback, uninstallRollback, defaultConfigRollback, shortcutRollback,
                        existingUserInstall, installDir, appPath,
                        uninstallPath, defaultConfigPath, hookFilePath, userDataDir);
        ReportFailure(L"无法注册当前用户的卸载入口，安装已回滚。", quiet);
        if (SUCCEEDED(comResult)) CoUninitialize();
        return 5;
    }

    // Hook 是最后一个可能失败的步骤。HookConfig 使用同目录临时文件原子替换；
    // 失败时原文件不变，因此文件与注册表可以安全回滚。
    const auto hookResult =
        CodexQuotaBarInstaller::HookConfig::Install(hookFilePath, appPath);
    if (!hookResult.success) {
        RollBackInstall(appRollback, uninstallRollback, defaultConfigRollback, shortcutRollback,
                        existingUserInstall, installDir, appPath,
                        uninstallPath, defaultConfigPath, hookFilePath, userDataDir);
        ReportFailure(L"无法注册 Codex 会话同步 Hook，安装已回滚：\n\n" +
                      hookResult.error, quiet);
        if (SUCCEEDED(comResult)) CoUninitialize();
        return 6;
    }

    CommitFileRollback(appRollback);
    CommitFileRollback(uninstallRollback);
    CommitFileRollback(defaultConfigRollback);
    CommitFileRollback(shortcutRollback);
    UpdateExistingCompanionStartup(appPath);

    bool launch = false;
    if (!quiet) {
        launch = MessageBoxW(
            nullptr,
            L"Codex-Quota-Bar 已安装完成，并已添加会话同步 Hook。\n\n"
            L"首次使用请在 Codex 中输入 /hooks，审核并信任新增 Hook。\n\n"
            L"是否立即启动额度条？",
            L"安装完成", MB_YESNO | MB_ICONINFORMATION | MB_SETFOREGROUND) == IDYES;
    }
    if (launch) {
        ShellExecuteW(nullptr, L"open", appPath.c_str(), nullptr,
                      installDir.c_str(), SW_SHOWNORMAL);
    }

    if (SUCCEEDED(comResult)) CoUninitialize();
    return 0;
}
