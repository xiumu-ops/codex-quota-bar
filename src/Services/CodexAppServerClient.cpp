#include "Services/CodexAppServerClient.h"
#include "Core/Constants.h"
#include "Core/Logger.h"
#include "Core/SimpleJson.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <sddl.h>

#include <algorithm>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace CodexQuotaBar {
namespace {

    constexpr ULONGLONG kRequestTimeoutMs = 12000;
    constexpr size_t kMaxOutputBytes = 1024 * 1024;

    class UniqueHandle {
    public:
        UniqueHandle() = default;
        explicit UniqueHandle(HANDLE handle) : m_handle(handle) {}
        ~UniqueHandle() { Reset(); }

        UniqueHandle(const UniqueHandle&) = delete;
        UniqueHandle& operator=(const UniqueHandle&) = delete;

        UniqueHandle(UniqueHandle&& other) noexcept : m_handle(other.Release()) {}
        UniqueHandle& operator=(UniqueHandle&& other) noexcept {
            if (this != &other) Reset(other.Release());
            return *this;
        }

        HANDLE Get() const { return m_handle; }
        bool IsValid() const { return m_handle && m_handle != INVALID_HANDLE_VALUE; }

        HANDLE Release() {
            HANDLE value = m_handle;
            m_handle = nullptr;
            return value;
        }

        void Reset(HANDLE handle = nullptr) {
            if (IsValid()) CloseHandle(m_handle);
            m_handle = handle;
        }

    private:
        HANDLE m_handle = nullptr;
    };

    class UniqueFindHandle {
    public:
        explicit UniqueFindHandle(HANDLE handle) : m_handle(handle) {}
        ~UniqueFindHandle() {
            if (m_handle != INVALID_HANDLE_VALUE) FindClose(m_handle);
        }
        UniqueFindHandle(const UniqueFindHandle&) = delete;
        UniqueFindHandle& operator=(const UniqueFindHandle&) = delete;
        HANDLE Get() const { return m_handle; }
        bool IsValid() const { return m_handle != INVALID_HANDLE_VALUE; }

    private:
        HANDLE m_handle = INVALID_HANDLE_VALUE;
    };

    class UniqueLocalMemory {
    public:
        explicit UniqueLocalMemory(HLOCAL memory = nullptr) : m_memory(memory) {}
        ~UniqueLocalMemory() { if (m_memory) LocalFree(m_memory); }
        UniqueLocalMemory(const UniqueLocalMemory&) = delete;
        UniqueLocalMemory& operator=(const UniqueLocalMemory&) = delete;
        void* Get() const { return m_memory; }

    private:
        HLOCAL m_memory = nullptr;
    };

    std::wstring ReadEnvironment(const wchar_t* name) {
        DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
        if (required == 0) return L"";

        std::wstring value(required, L'\0');
        DWORD written = GetEnvironmentVariableW(name, value.data(), required);
        if (written == 0 || written >= required) return L"";
        value.resize(written);
        return value;
    }

    bool IsFile(const std::wstring& path) {
        if (path.empty()) return false;
        DWORD attributes = GetFileAttributesW(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES &&
               (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
    }

    std::wstring FullPath(const std::wstring& path) {
        if (path.empty()) return L"";
        DWORD required = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
        if (required == 0) return path;

        std::wstring full(required, L'\0');
        DWORD written = GetFullPathNameW(path.c_str(), required, full.data(), nullptr);
        if (written == 0 || written >= required) return path;
        full.resize(written);
        return full;
    }

    std::wstring FindLatestLocalCodex() {
        std::wstring localAppData = ReadEnvironment(L"LOCALAPPDATA");
        if (localAppData.empty()) return L"";

        const std::wstring root = localAppData + L"\\OpenAI\\Codex\\bin";
        const std::wstring direct = root + L"\\codex.exe";
        if (IsFile(direct)) return FullPath(direct);

        WIN32_FIND_DATAW data = {};
        UniqueFindHandle search(FindFirstFileW((root + L"\\*").c_str(), &data));
        if (!search.IsValid()) return L"";

        std::wstring latest;
        FILETIME latestWrite = {};
        do {
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) continue;
            if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) continue;

            const std::wstring candidate = root + L"\\" + data.cFileName + L"\\codex.exe";
            if (!IsFile(candidate)) continue;

            WIN32_FILE_ATTRIBUTE_DATA fileData = {};
            if (!GetFileAttributesExW(candidate.c_str(), GetFileExInfoStandard, &fileData)) continue;
            if (latest.empty() || CompareFileTime(&fileData.ftLastWriteTime, &latestWrite) > 0) {
                latest = candidate;
                latestWrite = fileData.ftLastWriteTime;
            }
        } while (FindNextFileW(search.Get(), &data));

        return latest.empty() ? L"" : FullPath(latest);
    }

    std::wstring ResolveCodexExecutable() {
        std::wstring configured = ReadEnvironment(L"CODEX_QUOTA_CODEX_PATH");
        if (!configured.empty()) return IsFile(configured) ? FullPath(configured) : L"";

        std::wstring local = FindLatestLocalCodex();
        if (!local.empty()) return local;

        std::wstring programFiles = ReadEnvironment(L"ProgramFiles");
        if (!programFiles.empty()) {
            std::wstring installed = programFiles + L"\\OpenAI\\Codex\\bin\\codex.exe";
            if (IsFile(installed)) return FullPath(installed);
        }

        return L"";
    }

    std::wstring FormatWindowsError(DWORD error) {
        wchar_t* raw = nullptr;
        DWORD length = FormatMessageW(
            FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr, error, 0, reinterpret_cast<wchar_t*>(&raw), 0, nullptr);
        std::wstring message = length && raw ? std::wstring(raw, length) : L"Windows 错误 " + std::to_wstring(error);
        if (raw) LocalFree(raw);
        while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n' || message.back() == L' ')) {
            message.pop_back();
        }
        return message;
    }

    bool Utf8ToWide(const std::string& utf8, std::wstring& wide) {
        if (utf8.empty()) return false;
        int length = MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
        if (length <= 0) return false;

        wide.assign(static_cast<size_t>(length), L'\0');
        return MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()),
            wide.data(), length) == length;
    }

    class AppServerProcess {
    public:
        ~AppServerProcess() { Stop(); }

        bool Start(const std::wstring& executable, std::wstring& error) {
            PSECURITY_DESCRIPTOR descriptorRaw = nullptr;
            constexpr wchar_t kChildPipeSddl[] =
                L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;OW)S:(ML;;NW;;;ME)";
            if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                    kChildPipeSddl, SDDL_REVISION_1, &descriptorRaw, nullptr)) {
                error = L"创建 Codex 管道安全描述符失败：" + FormatWindowsError(GetLastError());
                return false;
            }
            UniqueLocalMemory descriptor(static_cast<HLOCAL>(descriptorRaw));
            SECURITY_ATTRIBUTES attributes = {};
            attributes.nLength = sizeof(attributes);
            attributes.bInheritHandle = TRUE;
            attributes.lpSecurityDescriptor = descriptor.Get();

            HANDLE stdoutReadRaw = nullptr;
            HANDLE stdoutWriteRaw = nullptr;
            if (!CreatePipe(&stdoutReadRaw, &stdoutWriteRaw, &attributes, 0)) {
                error = L"创建 Codex 输出管道失败：" + FormatWindowsError(GetLastError());
                return false;
            }
            UniqueHandle stdoutRead(stdoutReadRaw);
            UniqueHandle stdoutWrite(stdoutWriteRaw);
            if (!SetHandleInformation(stdoutRead.Get(), HANDLE_FLAG_INHERIT, 0)) {
                error = L"配置 Codex 输出管道失败：" + FormatWindowsError(GetLastError());
                return false;
            }

            GUID pipeGuid = {};
            wchar_t pipeGuidText[40] = {};
            const bool hasGuid = SUCCEEDED(CoCreateGuid(&pipeGuid)) &&
                StringFromGUID2(pipeGuid, pipeGuidText, static_cast<int>(std::size(pipeGuidText))) > 0;
            const std::wstring pipeSuffix = hasGuid
                ? std::wstring(pipeGuidText)
                : std::to_wstring(GetTickCount64());
            const std::wstring stdinPipeName = L"\\\\.\\pipe\\codex_quota_bar_stdin_"
                + std::to_wstring(GetCurrentProcessId()) + L"_" + pipeSuffix;
            HANDLE stdinReadRaw = CreateNamedPipeW(
                stdinPipeName.c_str(),
                PIPE_ACCESS_INBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                1, 4096, 4096, 0, &attributes);
            if (stdinReadRaw == INVALID_HANDLE_VALUE) {
                error = L"创建 Codex 输入管道失败：" + FormatWindowsError(GetLastError());
                return false;
            }
            UniqueHandle stdinRead(stdinReadRaw);

            HANDLE stdinWriteRaw = CreateFileW(
                stdinPipeName.c_str(), GENERIC_WRITE, 0, nullptr,
                OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
            if (stdinWriteRaw == INVALID_HANDLE_VALUE) {
                error = L"连接 Codex 输入管道失败：" + FormatWindowsError(GetLastError());
                return false;
            }
            UniqueHandle stdinWrite(stdinWriteRaw);

            HANDLE writeEventRaw = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!writeEventRaw) {
                error = L"创建 Codex 输入事件失败：" + FormatWindowsError(GetLastError());
                return false;
            }
            m_writeEvent.Reset(writeEventRaw);

            STARTUPINFOEXW startupEx = {};
            startupEx.StartupInfo.cb = sizeof(startupEx);
            startupEx.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
            startupEx.StartupInfo.hStdInput = stdinRead.Get();
            startupEx.StartupInfo.hStdOutput = stdoutWrite.Get();
            startupEx.StartupInfo.hStdError = stdoutWrite.Get();

            SIZE_T attributeBytes = 0;
            InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
            if (attributeBytes == 0) {
                error = L"初始化 Codex 句柄白名单失败：" + FormatWindowsError(GetLastError());
                return false;
            }
            std::vector<BYTE> attributeStorage(attributeBytes);
            startupEx.lpAttributeList = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(
                attributeStorage.data());
            if (!InitializeProcThreadAttributeList(
                    startupEx.lpAttributeList, 1, 0, &attributeBytes)) {
                error = L"创建 Codex 句柄白名单失败：" + FormatWindowsError(GetLastError());
                return false;
            }

            HANDLE inheritedHandles[] = { stdinRead.Get(), stdoutWrite.Get() };
            if (!UpdateProcThreadAttribute(
                    startupEx.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                    inheritedHandles, sizeof(inheritedHandles), nullptr, nullptr)) {
                error = L"限制 Codex 继承句柄失败：" + FormatWindowsError(GetLastError());
                DeleteProcThreadAttributeList(startupEx.lpAttributeList);
                return false;
            }

            HANDLE jobRaw = CreateJobObjectW(nullptr, nullptr);
            if (jobRaw) {
                JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
                limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
                if (SetInformationJobObject(
                        jobRaw, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
                    m_job.Reset(jobRaw);
                } else {
                    CloseHandle(jobRaw);
                    WriteLog(LogLevel::Warning, L"无法配置 App Server 作业对象，将使用进程级清理。");
                }
            } else {
                WriteLog(LogLevel::Warning, L"无法创建 App Server 作业对象，将使用进程级清理。");
            }

            PROCESS_INFORMATION processInfo = {};
            std::wstring command = L"\"" + executable + L"\" app-server";
            std::vector<wchar_t> commandBuffer(command.begin(), command.end());
            commandBuffer.push_back(L'\0');

            std::wstring workingDirectory = ReadEnvironment(L"USERPROFILE");
            BOOL created = CreateProcessW(
                executable.c_str(), commandBuffer.data(), nullptr, nullptr, TRUE,
                CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr,
                workingDirectory.empty() ? nullptr : workingDirectory.c_str(),
                &startupEx.StartupInfo, &processInfo);
            DeleteProcThreadAttributeList(startupEx.lpAttributeList);
            if (!created) {
                error = L"无法启动 codex app-server：" + FormatWindowsError(GetLastError());
                m_job.Reset();
                return false;
            }

            UniqueHandle thread(processInfo.hThread);
            m_process.Reset(processInfo.hProcess);
            if (m_job.IsValid() && !AssignProcessToJobObject(m_job.Get(), m_process.Get())) {
                WriteLog(LogLevel::Warning, L"无法将 App Server 加入作业对象，将使用进程级清理。");
                m_job.Reset();
            }
            if (ResumeThread(thread.Get()) == static_cast<DWORD>(-1)) {
                error = L"恢复 codex app-server 失败：" + FormatWindowsError(GetLastError());
                TerminateProcess(m_process.Get(), 1);
                WaitForSingleObject(m_process.Get(), 1000);
                m_process.Reset();
                m_job.Reset();
                return false;
            }
            m_stdoutRead = std::move(stdoutRead);
            m_stdinWrite = std::move(stdinWrite);
            return true;
        }

        bool Send(const char* json, ULONGLONG deadline, std::wstring& error) {
            std::string line(json);
            line.push_back('\n');

            size_t offset = 0;
            while (offset < line.size()) {
                OVERLAPPED ovl = {};
                ovl.hEvent = m_writeEvent.Get();
                ResetEvent(m_writeEvent.Get());
                const DWORD chunk = static_cast<DWORD>(
                    (std::min)(line.size() - offset, static_cast<size_t>(MAXDWORD)));
                DWORD written = 0;
                BOOL ok = WriteFile(m_stdinWrite.Get(), line.data() + offset, chunk, &written, &ovl);
                if (!ok && GetLastError() == ERROR_IO_PENDING) {
                    const ULONGLONG now = GetTickCount64();
                    if (now < deadline && WaitForSingleObject(
                            m_writeEvent.Get(), static_cast<DWORD>(deadline - now)) == WAIT_OBJECT_0) {
                        ok = GetOverlappedResult(m_stdinWrite.Get(), &ovl, &written, FALSE);
                    } else {
                        CancelIoEx(m_stdinWrite.Get(), &ovl);
                        DWORD transferred = 0;
                        GetOverlappedResult(m_stdinWrite.Get(), &ovl, &transferred, TRUE);
                        error = L"等待 Codex App Server 写入超时。";
                        return false;
                    }
                }
                if (!ok || written == 0) {
                    error = L"写入 Codex App Server 失败：" + FormatWindowsError(GetLastError());
                    return false;
                }
                offset += written;
            }
            return true;
        }

        bool ReadResponse(
            int expectedId,
            ULONGLONG deadline,
            std::string& pending,
            std::wstring& responseLine,
            JsonValue& response,
            std::wstring& error)
        {
            for (;;) {
                size_t newline = pending.find('\n');
                while (newline != std::string::npos) {
                    std::string line = pending.substr(0, newline);
                    pending.erase(0, newline + 1);
                    if (!line.empty() && line.back() == '\r') line.pop_back();

                    std::wstring wide;
                    JsonValue parsed;
                    if (Utf8ToWide(line, wide) && JsonParser::TryParse(wide, parsed) && parsed.is_object()) {
                        const JsonValue& id = parsed[L"id"];
                        if (id.is_number() && id.as_int(-1) == expectedId) {
                            responseLine = std::move(wide);
                            response = std::move(parsed);
                            return true;
                        }
                    }
                    newline = pending.find('\n');
                }

                if (GetTickCount64() >= deadline) {
                    error = L"等待 Codex App Server 响应超时。";
                    return false;
                }

                DWORD available = 0;
                if (!PeekNamedPipe(m_stdoutRead.Get(), nullptr, 0, nullptr, &available, nullptr)) {
                    error = L"读取 Codex App Server 输出失败：" + FormatWindowsError(GetLastError());
                    return false;
                }

                if (available > 0) {
                    char buffer[8192];
                    DWORD toRead = (std::min)(available, static_cast<DWORD>(sizeof(buffer)));
                    DWORD read = 0;
                    if (!ReadFile(m_stdoutRead.Get(), buffer, toRead, &read, nullptr) || read == 0) {
                        error = L"读取 Codex App Server 输出失败：" + FormatWindowsError(GetLastError());
                        return false;
                    }
                    if (pending.size() + read > kMaxOutputBytes) {
                        error = L"Codex App Server 输出超过安全上限。";
                        return false;
                    }
                    pending.append(buffer, read);
                    continue;
                }

                if (WaitForSingleObject(m_process.Get(), 0) == WAIT_OBJECT_0) {
                    error = L"Codex App Server 提前退出。";
                    return false;
                }
                Sleep(10);
            }
        }

        void Stop() {
            m_stdinWrite.Reset();
            if (m_process.IsValid() && WaitForSingleObject(m_process.Get(), 1500) == WAIT_TIMEOUT) {
                WriteLog(LogLevel::Warning, L"App Server 未在输入关闭后退出，正在执行有界强制清理。");
                if (m_job.IsValid()) {
                    TerminateJobObject(m_job.Get(), 0);
                } else {
                    TerminateProcess(m_process.Get(), 0);
                }
                WaitForSingleObject(m_process.Get(), 1000);
            }
            m_stdoutRead.Reset();
            m_process.Reset();
            m_writeEvent.Reset();
            m_job.Reset();
        }

    private:
        UniqueHandle m_process;
        UniqueHandle m_job;
        UniqueHandle m_stdinWrite;
        UniqueHandle m_stdoutRead;
        UniqueHandle m_writeEvent;
    };

    bool GetResponseError(const JsonValue& response, std::wstring& error) {
        const JsonValue& errorValue = response[L"error"];
        if (errorValue.is_null()) return false;

        if (errorValue.is_object() && errorValue[L"message"].is_string()) {
            error = errorValue[L"message"].as_string(L"Codex App Server 返回错误。");
        } else {
            error = L"Codex App Server 返回错误。";
        }
        return true;
    }

} // namespace

    bool CodexAppServerClient::ReadAccountData(
        std::wstring& rateLimitsResponseJson,
        std::wstring& usageResponseJson,
        std::wstring& usageErrorMessage,
        std::wstring& errorMessage)
    {
        rateLimitsResponseJson.clear();
        usageResponseJson.clear();
        usageErrorMessage.clear();
        errorMessage.clear();

        const std::wstring executable = ResolveCodexExecutable();
        if (executable.empty()) {
            errorMessage = L"未找到 codex.exe；请启动 Codex 或设置 CODEX_QUOTA_CODEX_PATH。";
            WriteLog(LogLevel::Warning, L"同步失败：未找到可用的 Codex App Server 可执行文件。");
            return false;
        }

        AppServerProcess server;
        if (!server.Start(executable, errorMessage)) {
            WriteLog(LogLevel::Error, L"同步失败：Codex App Server 子进程未能启动。");
            return false;
        }

        const ULONGLONG deadline = GetTickCount64() + kRequestTimeoutMs;
        std::string pending;
        JsonValue response;
        std::wstring line;

        const std::string initializeRequest =
            "{\"method\":\"initialize\",\"id\":1,\"params\":{\"clientInfo\":{\"name\":\"codex_quota_bar\",\"title\":\"Codex-Quota-Bar\",\"version\":\"" +
            std::string(APP_VERSION_UTF8) + "\"}}}";
        if (!server.Send(
                initializeRequest.c_str(),
                deadline, errorMessage) ||
            !server.ReadResponse(1, deadline, pending, line, response, errorMessage)) {
            WriteLog(LogLevel::Warning, L"同步失败：App Server initialize 阶段未完成。");
            return false;
        }
        if (GetResponseError(response, errorMessage)) {
            WriteLog(LogLevel::Warning, L"同步失败：App Server 拒绝 initialize 请求。");
            return false;
        }

        if (!server.Send("{\"method\":\"initialized\",\"params\":{}}", deadline, errorMessage) ||
            !server.Send("{\"method\":\"account/rateLimits/read\",\"id\":2}", deadline, errorMessage) ||
            !server.ReadResponse(2, deadline, pending, line, response, errorMessage)) {
            WriteLog(LogLevel::Warning, L"同步失败：额度读取请求未完成。");
            return false;
        }
        if (GetResponseError(response, errorMessage)) {
            WriteLog(LogLevel::Warning, L"同步失败：App Server 返回额度读取错误。");
            return false;
        }

        if (!response[L"result"].is_object()) {
            errorMessage = L"额度响应中缺少 result。";
            WriteLog(LogLevel::Warning, L"同步失败：额度响应结构无效。");
            return false;
        }

        rateLimitsResponseJson = std::move(line);

        if (!server.Send(
                "{\"method\":\"account/usage/read\",\"id\":3,\"params\":{}}",
                deadline, usageErrorMessage) ||
            !server.ReadResponse(3, deadline, pending, line, response, usageErrorMessage)) {
            WriteLog(LogLevel::Warning, L"部分同步：Token 统计读取未完成。");
            return true;
        }
        if (GetResponseError(response, usageErrorMessage)) {
            WriteLog(LogLevel::Warning, L"部分同步：App Server 返回 Token 统计错误。");
            return true;
        }

        if (!response[L"result"].is_object() || !response[L"result"][L"summary"].is_object()) {
            usageErrorMessage = L"Token 统计响应中缺少 summary。";
            WriteLog(LogLevel::Warning, L"部分同步：Token 统计响应结构无效。");
            return true;
        }

        usageResponseJson = std::move(line);
        return true;
    }

} // namespace CodexQuotaBar
