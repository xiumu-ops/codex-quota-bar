#pragma once

#include "Graphics/D2DCommon.h"
#include "Graphics/Theme.h"

#include <string>

namespace CodexQuotaBar {

    bool ShowTransparencyDialog(
        HWND owner,
        float dpiScale,
        int currentValue,
        int& selectedValue,
        const ThemePalette& palette,
        const std::wstring& fontFamily);

} // namespace CodexQuotaBar
