#pragma once

#include <string>

namespace CodexQuotaBar {

    inline constexpr int COMPANION_MISSING_POLL_THRESHOLD = 3;

    enum class DesktopProcessState {
        NotRunning = 0,
        Running = 1,
        QueryFailed = 2,
    };

    class CompanionMode {
    public:
        // 公开路径判定用于回归测试，确保 CLI/App Server 不会被当成桌面端。
        static bool IsDesktopExecutablePath(const std::wstring& path);
        static DesktopProcessState ProbeDesktopState();
        static bool IsDesktopRunning();
        static bool ConfigureAutoStart(bool enabled);
    };

} // namespace CodexQuotaBar
