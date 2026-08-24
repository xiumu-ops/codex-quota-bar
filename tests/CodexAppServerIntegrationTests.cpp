#include "Services/CodexClient.h"
#include "Services/CompanionMode.h"

#include <cmath>
#include <iostream>
#include <string>

using CodexQuotaBar::CodexClient;
using CodexQuotaBar::CompanionMode;
using CodexQuotaBar::QuotaSnapshot;

namespace {

int g_failures = 0;

void Expect(bool condition, const char* message) {
    if (condition) {
        std::cout << "  [PASS] " << message << '\n';
    } else {
        std::cerr << "  [FAIL] " << message << '\n';
        ++g_failures;
    }
}

QuotaSnapshot Fetch(const wchar_t* scenario) {
    _wputenv_s(L"CODEX_QUOTA_FAKE_SCENARIO", scenario);
    return CodexClient::FetchSnapshot();
}

bool Near(double left, double right) {
    return std::abs(left - right) < 0.001;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) {
        std::cerr << "usage: CodexAppServerIntegrationTests <fake-codex-path>\n";
        return 64;
    }

    // 显式覆盖是受信任的自定义安装/测试入口。
    _wputenv_s(L"CODEX_QUOTA_CODEX_PATH", argv[1]);

    std::cout << "Companion desktop-process classification\n";
    Expect(CompanionMode::IsDesktopExecutablePath(
               L"C:\\Program Files\\WindowsApps\\OpenAI.Codex_1.0_x64__test\\app\\ChatGPT.exe"),
           "packaged Codex ChatGPT host is recognized");
    Expect(CompanionMode::IsDesktopExecutablePath(
               L"C:\\Program Files\\WindowsApps\\OpenAI.Codex_1.0_x64__test\\app\\Codex.exe"),
           "packaged Codex launcher is recognized");
    Expect(!CompanionMode::IsDesktopExecutablePath(
               L"C:\\Program Files\\WindowsApps\\OpenAI.Codex_1.0_x64__test\\app\\resources\\codex.exe"),
           "desktop-owned App Server CLI is excluded");
    Expect(!CompanionMode::IsDesktopExecutablePath(
               L"C:\\Users\\test\\AppData\\Local\\OpenAI\\Codex\\bin\\version\\codex.exe"),
           "local App Server CLI is excluded");
    Expect(!CompanionMode::IsDesktopExecutablePath(
               L"C:\\Program Files\\OpenAI\\ChatGPT\\ChatGPT.exe"),
           "unrelated ChatGPT desktop app is excluded");

    std::cout << "App Server happy-path protocol and parsing\n";
    QuotaSnapshot happy = Fetch(L"happy");
    Expect(happy.success, "complete App Server synchronization succeeds");
    Expect(happy.statsSynchronized, "usage summary is marked synchronized");
    Expect(happy.window.available && Near(happy.window.usedPercent, 37.5) &&
           Near(happy.window.remainingPercent, 62.5),
           "five-hour quota is parsed");
    Expect(happy.weekly.available && Near(happy.weekly.usedPercent, 90.0) &&
           Near(happy.weekly.remainingPercent, 10.0),
           "weekly quota is parsed");
    Expect(happy.resetCredits.countAvailable && happy.resetCredits.availableCount == 2,
           "available reset-credit count is parsed");
    Expect(happy.resetCredits.earliestExpiresAt == 1789946796,
           "earliest reset-credit expiresAt is parsed independently from resetsAt");
    Expect(happy.stats.totalTokens == L"1.23 M", "lifetimeTokens is formatted");
    Expect(happy.stats.peakTokens == L"98.8 K", "peakDailyTokens is formatted");
    Expect(happy.stats.longestTask == L"1.5 小时", "longestRunningTurnSec is formatted");
    Expect(happy.stats.streakDays == L"12天", "currentStreakDays is formatted");

    std::cout << "Nullable official usage fields\n";
    QuotaSnapshot nullable = Fetch(L"nullable");
    Expect(nullable.success, "null summary values do not fail synchronization");
    Expect(nullable.statsSynchronized, "valid all-null summary is recognized");
    Expect(nullable.stats.totalTokens.empty() && nullable.stats.peakTokens.empty() &&
           nullable.stats.longestTask.empty() && nullable.stats.streakDays.empty(),
           "null values remain empty for UI placeholder rendering");

    std::cout << "Invalid usage field type\n";
    QuotaSnapshot invalidType = Fetch(L"invalid_type");
    Expect(!invalidType.success, "non-null invalid field type fails synchronization");
    Expect(!invalidType.statsSynchronized, "invalid summary is not marked synchronized");
    Expect(invalidType.window.available && invalidType.weekly.available,
           "quota data survives malformed usage fields");

    std::cout << "Partial failure handling\n";
    QuotaSnapshot usageError = Fetch(L"usage_error");
    Expect(!usageError.success, "usage endpoint error marks full synchronization failed");
    Expect(!usageError.statsSynchronized, "failed usage request is not marked synchronized");
    Expect(usageError.window.available && usageError.weekly.available,
           "quota data survives a usage endpoint failure");
    Expect(usageError.errorMessage.find(L"usage unavailable") != std::wstring::npos,
           "server usage error is preserved");

    _wputenv_s(L"CODEX_QUOTA_FAKE_SCENARIO", L"");
    _wputenv_s(L"CODEX_QUOTA_CODEX_PATH", L"");

    if (g_failures == 0) {
        std::cout << "All App Server integration tests passed.\n";
        return 0;
    }
    std::cerr << g_failures << " App Server integration test(s) failed.\n";
    return 1;
}
