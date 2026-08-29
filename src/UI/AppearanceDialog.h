#pragma once

#include "Graphics/D2DCommon.h"
#include "Graphics/Theme.h"

#include <functional>
#include <map>
#include <string>

namespace CodexQuotaBar {

    using AppearanceApplyCallback = std::function<bool(
        const std::map<std::wstring, std::wstring>& colors,
        int backgroundTransparency)>;

    bool ShowAppearanceDialog(
        HWND owner,
        float dpiScale,
        const std::map<std::wstring, std::wstring>& currentColors,
        int currentBackgroundTransparency,
        const ThemePalette& palette,
        const std::wstring& fontFamily,
        const AppearanceApplyCallback& applyCallback);

} // namespace CodexQuotaBar
