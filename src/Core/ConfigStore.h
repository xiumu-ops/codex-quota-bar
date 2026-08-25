#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace CodexQuotaBar {

    struct StoredState {
        POINT position{};
        bool hasPosition = false;
    };

    struct AppSettings {
        float userScale = 1.0f;
        bool companionMode = false;
        int refreshIntervalMinutes = 1;
    };

    // 安装版设置位于安装根目录的 data\config.json；默认路径为
    // %LOCALAPPDATA%\Codex-Quota-Bar\data\config.json。
    class ConfigStore {
    public:
        static AppSettings LoadSettings();
        static StoredState LoadState();
        static void SaveSettings(const AppSettings& settings);
        static void SavePosition(POINT position);
    };

} // namespace CodexQuotaBar
