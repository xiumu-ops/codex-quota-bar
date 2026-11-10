#pragma once

namespace CodexQuotaBar {

    inline constexpr wchar_t APP_VERSION[] = L"3.0.3";
    inline constexpr char APP_VERSION_UTF8[] = "3.0.3";
    inline constexpr wchar_t PIPE_NAME_PREFIX[] = L"\\\\.\\pipe\\Codex-Quota-Bar_Pipe_";
    inline constexpr wchar_t MUTEX_NAME[] = L"Local\\Codex-Quota-Bar_Mutex_Session";

    constexpr int BASE_DPI = 96;
    constexpr int BAR_WIDTH = 380;
    // 紧凑悬浮卡片：四周边距留白统一 8px，行与卡片间隔边距统一 6px；展开态追加统计与重置子卡片。
    constexpr int COLLAPSED_HEIGHT = 104;
    constexpr int EXPANDED_HEIGHT = 256;
    constexpr int MINI_DUAL_WIDTH = 112;
    constexpr int MINI_SINGLE_WIDTH = 56;
    constexpr int MINI_HEIGHT = 28;
    constexpr int CORNER_RADIUS = 8;

} // namespace CodexQuotaBar
