#include "Core/Appearance.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cwctype>

namespace CodexQuotaBar {
namespace {

    constexpr std::array<std::wstring_view, 19> kSupportedColors = {
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
            { L"StatsCardBackground", L"#FAFAFA" },
            { L"StatsCardBorder", L"#E0E0E0" },
            { L"Text", L"#404040" },
            { L"MutedText", L"#757575" },
            { L"TrackBackground", L"#F7F7F7" },
            { L"Unavailable", L"#757575" },
            { L"ProgressHigh", L"#159957" },
            { L"ProgressMedium", L"#D9A900" },
            { L"ProgressLow", L"#D76B26" },
            { L"ProgressCritical", L"#C00000" },
            { L"OuterBorder", L"#FFFFFF" },
            { L"Divider", L"#E0E0E0" },
            { L"Chevron", L"#5F5F5F" },
            { L"SyncSuccess", L"#159957" },
            { L"SyncIdle", L"#A8A8A8" },
            { L"SyncBusy", L"#D9A900" },
            { L"MenuHover", L"#F0F0F0" },
            { L"MenuDivider", L"#EAEAEA" },
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
