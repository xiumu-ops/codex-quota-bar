#pragma once

#include "Graphics/D2DCommon.h"
#include "Graphics/Theme.h"
#include <string>
#include <vector>

namespace CodexQuotaBar {

    struct MenuItem {
        int id = 0;
        std::wstring text; // 空字符串表示分隔线
    };

    class CustomMenu {
    public:
        // 弹出自绘菜单并模态跟踪，返回选中的命令 id（取消返回 0）
        static int Show(
            HWND owner,
            float dpiScale,
            int screenX,
            int screenY,
            const std::vector<MenuItem>& items,
            const ThemePalette& palette,
            const std::wstring& fontFamily);
    };

} // namespace CodexQuotaBar
