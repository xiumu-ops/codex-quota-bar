#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dwmapi.h>

namespace CodexQuotaBar {

    // 主窗口与右键菜单均由 DWM 提供原生投影。不要叠加 CS_DROPSHADOW：它的固定向下
    // 偏移会在低高度迷你栏两侧形成明显的上下阴影断层。
    inline void ApplyPopupWindowEffects(HWND hwnd, bool useSmallCorner = false) {
        const int cornerPreference = useSmallCorner
            ? DWMWCP_ROUNDSMALL
            : DWMWCP_ROUND;
        DwmSetWindowAttribute(
            hwnd,
            DWMWA_WINDOW_CORNER_PREFERENCE,
            &cornerPreference,
            sizeof(cornerPreference));
    }

} // namespace CodexQuotaBar
