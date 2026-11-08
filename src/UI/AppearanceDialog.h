#pragma once

#include "Graphics/D2DCommon.h"
#include "Graphics/Theme.h"

#include <functional>
#include <map>
#include <string>

namespace CodexQuotaBar {

    using AppearanceApplyCallback = std::function<bool(
        const std::map<std::wstring, std::wstring>& colors,
        int backgroundTransparency,
        const std::wstring& fontFamily,
        int userScalePercent)>;

    bool ShowAppearanceDialog(
        HWND owner,
        float dpiScale,
        const std::map<std::wstring, std::wstring>& currentColors,
        int currentBackgroundTransparency,
        const std::wstring& currentFontFamily,
        int currentUserScalePercent,
        const ThemePalette& palette,
        const AppearanceApplyCallback& applyCallback);

} // namespace CodexQuotaBar
