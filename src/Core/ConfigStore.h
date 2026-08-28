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
        // config-default.json 中的只读外观基线，不写入用户配置。
        AppearanceSettings defaultAppearance;
    };

    // 安装版默认配置位于 app\config-default.json；用户设置位于
    // data\config-users.json。旧 data\config.json 会在首次有效读取后迁移。
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
        static std::wstring DefaultConfigFilePath();
    };

} // namespace CodexQuotaBar
