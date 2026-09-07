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

    // 主窗口与右键菜单共用同一种系统投影机制。
    inline constexpr UINT POPUP_SHADOW_CLASS_STYLE = CS_DROPSHADOW;

    inline void ApplyPopupWindowEffects(HWND hwnd) {
        const int cornerPreference = DWMWCP_ROUND;
        DwmSetWindowAttribute(
            hwnd,
            DWMWA_WINDOW_CORNER_PREFERENCE,
            &cornerPreference,
            sizeof(cornerPreference));
    }

} // namespace CodexQuotaBar
