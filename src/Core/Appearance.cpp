#include "Core/Appearance.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cwctype>

namespace CodexQuotaBar {
namespace {

    constexpr std::array<std::wstring_view, 23> kSupportedColors = {
        L"Surface",
        L"StatsCardBackground",
        L"StatsCardBorder",
        L"Text",
        L"MutedText",
        L"TrackBackground",
        L"Unavailable",
        L"ProgressHigh",
        L"ProgressMedium",
        L"ProgressLow",
        L"ProgressCritical",
        L"ProgressHighText",
        L"ProgressMediumText",
        L"ProgressLowText",
        L"ProgressCriticalText",
        L"OuterBorder",
        L"Divider",
        L"Chevron",
        L"SyncSuccess",
        L"SyncIdle",
        L"SyncBusy",
        L"MenuHover",
        L"MenuDivider",
    };

    int HexValue(wchar_t ch) {
        if (ch >= L'0' && ch <= L'9') return ch - L'0';
        if (ch >= L'a' && ch <= L'f') return ch - L'a' + 10;
        if (ch >= L'A' && ch <= L'F') return ch - L'A' + 10;
        return -1;
    }

} // namespace

    std::map<std::wstring, std::wstring> DefaultAppearanceColors() {
        return {
            { L"Surface", L"#FFFFFF" },
            { L"StatsCardBackground", L"#F5F7F6" },
            { L"StatsCardBorder", L"#F5F7F6" },
            { L"Text", L"#242C29" },
            { L"MutedText", L"#626E68" },
            { L"TrackBackground", L"#E9EEEB" },
            { L"Unavailable", L"#626E68" },
            { L"ProgressHigh", L"#20945E" },
            { L"ProgressMedium", L"#B99637" },
            { L"ProgressLow", L"#C78043" },
            { L"ProgressCritical", L"#CA4D54" },
            { L"ProgressHighText", L"#146B42" },
            { L"ProgressMediumText", L"#725713" },
            { L"ProgressLowText", L"#934A1A" },
            { L"ProgressCriticalText", L"#9C3035" },
            { L"OuterBorder", L"#E6EBE8" },
            { L"Divider", L"#EDF0EE" },
            { L"Chevron", L"#626E68" },
            { L"SyncSuccess", L"#20945E" },
            { L"SyncIdle", L"#A8A8A8" },
            { L"SyncBusy", L"#B99637" },
            { L"MenuHover", L"#EEF3F0" },
            { L"MenuDivider", L"#EDF0EE" },
        };
    }

    bool IsSupportedAppearanceColor(std::wstring_view name) {
        return std::find(kSupportedColors.begin(), kSupportedColors.end(), name) !=
               kSupportedColors.end();
    }

    bool TryParseAppearanceColor(std::wstring_view value, uint32_t& rgb) {
        if (value.size() != 7 || value.front() != L'#') return false;
        uint32_t parsed = 0;
        for (size_t i = 1; i < value.size(); ++i) {
            const int digit = HexValue(value[i]);
            if (digit < 0) return false;
            parsed = (parsed << 4) | static_cast<uint32_t>(digit);
        }
        rgb = parsed;
        return true;
    }

    bool IsValidFontFamilyName(std::wstring_view value) {
        if (value.empty() || value.size() > 128) return false;
        return std::none_of(value.begin(), value.end(), [](wchar_t ch) {
            return ch < 0x20 || ch == 0x7F;
        });
    }

    bool IsValidBackgroundTransparency(int value) {
        return value >= 0 && value <= 90;
    }

    bool TryParseBackgroundTransparency(double value, int& transparency) {
        if (!std::isfinite(value) || std::floor(value) != value ||
            value < 0.0 || value > 90.0) {
            return false;
        }
        transparency = static_cast<int>(value);
        return true;
    }

} // namespace CodexQuotaBar
