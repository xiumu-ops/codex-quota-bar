#include "Services/PipeServer.h"

#include <windows.h>
#include <aclapi.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>

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
    Expect(PipeServer::SendCommand(name, L"PING", 2000) == L"OK", "normal command succeeds");
    Expect(PipeServer::SendCommand(name, L"PING " + std::wstring(3000, L'x'), 2000) == L"OK",
           "command larger than the pipe buffer succeeds");

    WaitNamedPipeW(name.c_str(), 2000);
    HANDLE silent = CreateFileW(
        name.c_str(), GENERIC_READ | GENERIC_WRITE | READ_CONTROL, 0, nullptr,
        OPEN_EXISTING, 0, nullptr);
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

    server.Stop();
    _wputenv_s(L"CODEX_QUOTA_DISABLE_LOG", L"");
    return g_failures == 0 ? 0 : 1;
}
