#pragma once

#include "Graphics/D2DCommon.h"
#include "Graphics/Theme.h"

#include <map>
#include <string>

namespace CodexQuotaBar {

    bool ShowAppearanceColorsDialog(
        HWND owner,
        float dpiScale,
        const std::map<std::wstring, std::wstring>& currentColors,
        std::map<std::wstring, std::wstring>& selectedColors,
        const ThemePalette& palette,
        const std::wstring& fontFamily);

} // namespace CodexQuotaBar
