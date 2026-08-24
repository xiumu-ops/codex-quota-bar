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

    // 所有应用设置统一持久化到 %LOCALAPPDATA%\Codex-Quota-Bar\config.json。
    class ConfigStore {
    public:
        static AppSettings LoadSettings();
        static StoredState LoadState();
        static void SaveSettings(const AppSettings& settings);
        static void SavePosition(POINT position);
    };

} // namespace CodexQuotaBar
