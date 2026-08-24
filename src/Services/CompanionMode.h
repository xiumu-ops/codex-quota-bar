#pragma once

#include <string>

namespace CodexQuotaBar {

    class CompanionMode {
    public:
        // 公开路径判定用于回归测试，确保 CLI/App Server 不会被当成桌面端。
        static bool IsDesktopExecutablePath(const std::wstring& path);
        static bool IsDesktopRunning();
        static bool ConfigureAutoStart(bool enabled);
    };

} // namespace CodexQuotaBar
