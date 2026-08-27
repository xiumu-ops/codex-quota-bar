#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "Core/Appearance.h"

#include <string>

namespace CodexQuotaBar {

    struct StoredState {
        POINT position{};
        bool hasPosition = false;
    };

    struct AppSettings {
        float userScale = 1.0f;
        bool companionMode = false;
        int refreshIntervalMinutes = 1;
        AppearanceSettings appearance;
    };

    // 安装版设置位于安装根目录的 data\config.json；默认路径为
    // %LOCALAPPDATA%\Codex-Quota-Bar\data\config.json。
    class ConfigStore {
    public:
        static AppSettings LoadSettings(
            std::wstring* validationError = nullptr,
            bool* configReadable = nullptr);
        static StoredState LoadState();
        static bool SaveSettings(
            const AppSettings& settings,
            std::wstring* validationError = nullptr);
        static void SavePosition(POINT position);
        static std::wstring DataDirectory();
        static std::wstring ConfigFilePath();
    };

} // namespace CodexQuotaBar
