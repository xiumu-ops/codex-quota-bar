#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>

namespace CodexQuotaBar {

    enum class AppearanceMode {
        Default,
        Custom
    };

    std::map<std::wstring, std::wstring> DefaultAppearanceColors();

    struct AppearanceSettings {
        AppearanceMode mode = AppearanceMode::Default;
        std::wstring fontFamily = L"Microsoft YaHei UI";
        int backgroundTransparency = 0;
        std::map<std::wstring, std::wstring> colors = DefaultAppearanceColors();
    };

    bool IsSupportedAppearanceColor(std::wstring_view name);
    bool TryParseAppearanceColor(std::wstring_view value, uint32_t& rgb);
    bool IsValidFontFamilyName(std::wstring_view value);
    bool IsValidBackgroundTransparency(int value);
    bool TryParseBackgroundTransparency(double value, int& transparency);

} // namespace CodexQuotaBar
