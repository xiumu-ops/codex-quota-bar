#include "Services/PipeServer.h"
#include "Services/PipeClient.h"
#include "Core/WinHandle.h"

#include <windows.h>
#include <aclapi.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

using CodexQuotaBar::PipeServer;

namespace {
int g_failures = 0;

void Expect(bool condition, const char* message) {
    if (condition) std::cout << "  [PASS] " << message << '\n';
    else {
        std::cerr << "  [FAIL] " << message << '\n';
        ++g_failures;
    }
}

std::wstring UniquePipeName() {
    return L"\\\\.\\pipe\\Codex-Quota-Bar-Test-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
}

HANDLE OpenClient(const std::wstring& name, DWORD extraAccess = 0) {
    const ULONGLONG deadline = GetTickCount64() + 2000;
    while (GetTickCount64() < deadline) {
        HANDLE pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE | extraAccess,
                                  0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) return pipe;
        const DWORD error = GetLastError();
        if (error == ERROR_PIPE_BUSY) WaitNamedPipeW(name.c_str(), 100);
        else if (error != ERROR_FILE_NOT_FOUND) break;
        Sleep(10);
    }
    return INVALID_HANDLE_VALUE;
}
} // namespace

int main() {
    _wputenv_s(L"CODEX_QUOTA_DISABLE_LOG", L"1");
    const std::wstring name = UniquePipeName();

    HANDLE squatter = CreateNamedPipeW(
        name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
        1, 4096, 4096, 0, nullptr);
    Expect(squatter != INVALID_HANDLE_VALUE, "test pipe squatter is created");
    PipeServer protectedServer(name, [](const std::wstring&) { return L"OK"; });
    Expect(!protectedServer.Start(), "server refuses to join a pre-existing pipe name");
    if (squatter != INVALID_HANDLE_VALUE) CloseHandle(squatter);

    PipeServer server(name, [](const std::wstring& command) {
        return command.rfind(L"PING", 0) == 0 ? L"OK" : L"UNKNOWN";
    });
    Expect(server.Start(), "secured server starts after the squatter is removed");
    CodexQuotaBar::WinHandle extraInstance(CreateNamedPipeW(
        name.c_str(), PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, 4096, 4096, 0, nullptr));
    Expect(!extraInstance, "pipe rejects a second server instance");
    const DWORD serverPid = GetCurrentProcessId();
    const DWORD wrongPid = serverPid == 1 ? 2 : 1;
    Expect(CodexQuotaBar::SendPipeCommand(name, L"PING", 2000, wrongPid) == L"ERROR",
           "installer client rejects a pipe owned by a different process");
    Expect(CodexQuotaBar::SendPipeCommand(name, L"PING", 2000, serverPid) == L"OK",
           "installer client accepts the verified server process");
    Expect(PipeServer::SendCommand(name, L"PING", 2000) == L"OK", "normal command succeeds");
    Expect(PipeServer::SendCommand(name, L"PING " + std::wstring(3000, L'x'), 2000) == L"OK",
           "command larger than the pipe buffer succeeds");

    {
        CodexQuotaBar::WinHandle delayed(OpenClient(name));
        const wchar_t command[] = L"PING";
        DWORD bytes = 0;
        const bool sent = delayed &&
            WriteFile(delayed.Get(), command, 4 * sizeof(wchar_t), &bytes, nullptr);
        bool available = false;
        if (sent) {
            const ULONGLONG deadline = GetTickCount64() + 200;
            while (GetTickCount64() < deadline &&
                   PeekNamedPipe(delayed.Get(), nullptr, 0, nullptr, &bytes, nullptr)) {
                if (bytes != 0) {
                    available = true;
                    break;
                }
                Sleep(5);
            }
        }
        Sleep(75);
        wchar_t response[16] = {};
        Expect(available && ReadFile(delayed.Get(), response, sizeof(response), &bytes, nullptr) &&
                   bytes == 3 * sizeof(wchar_t) && std::wstring_view(response, 2) == L"OK" &&
                   response[2] == L'\0',
               "reply stays readable when the client delays its read");
    }
    HANDLE silent = OpenClient(name, READ_CONTROL);
    Expect(silent != INVALID_HANDLE_VALUE, "silent client connects");
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PACL dacl = nullptr;
    const DWORD securityResult = silent == INVALID_HANDLE_VALUE
        ? ERROR_INVALID_HANDLE
        : GetSecurityInfo(
            silent, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION,
            nullptr, nullptr, &dacl, nullptr, &descriptor);
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    const bool protectedDacl = securityResult == ERROR_SUCCESS && dacl != nullptr &&
        GetSecurityDescriptorControl(descriptor, &control, &revision) &&
        (control & SE_DACL_PROTECTED) != 0;
    Expect(protectedDacl, "pipe uses a protected explicit DACL");
    if (descriptor) LocalFree(descriptor);
    const auto started = std::chrono::steady_clock::now();
    const std::wstring reply = PipeServer::SendCommand(name, L"PING", 3000);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    Expect(reply == L"OK" && elapsed.count() < 2500,
           "silent client is canceled and the server recovers");
    if (silent != INVALID_HANDLE_VALUE) CloseHandle(silent);

    bool repeated = true;
    for (int i = 0; i < 128; ++i) {
        if (PipeServer::SendCommand(name, L"PING " + std::to_wstring(i), 1000) != L"OK") {
            repeated = false;
            break;
        }
    }
    Expect(repeated, "128 consecutive commands receive complete replies");

    server.Stop();

    std::atomic<bool> handlerEntered{ false };
    const auto slowName = name + L"-slow";
    PipeServer slowServer(slowName, [&](const std::wstring&) {
        handlerEntered = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        return L"OK";
    });
    Expect(slowServer.Start(), "slow-response server starts");
    const auto slowStart = std::chrono::steady_clock::now();
    const auto timedOut = PipeServer::SendCommand(slowName, L"PING", 250);
    const auto slowElapsed = std::chrono::steady_clock::now() - slowStart;
    slowServer.Stop();
    Expect(handlerEntered && timedOut == L"ERROR" && slowElapsed < std::chrono::seconds(1),
           "client times out even after the server receives the command");

    _wputenv_s(L"CODEX_QUOTA_DISABLE_LOG", L"");
    return g_failures == 0 ? 0 : 1;
}
