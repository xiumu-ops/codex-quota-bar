#pragma once

namespace CodexQuotaBar {

    inline constexpr wchar_t APP_VERSION[] = L"2.5.8";
    inline constexpr char APP_VERSION_UTF8[] = "2.5.8";
    inline constexpr wchar_t PIPE_NAME_PREFIX[] = L"\\\\.\\pipe\\Codex-Quota-Bar_Pipe_";
    inline constexpr wchar_t MUTEX_NAME[] = L"Local\\Codex-Quota-Bar_Mutex_Session";

    constexpr int BASE_DPI = 96;
    constexpr int BAR_WIDTH = 380;
    // 紧凑悬浮卡片：内容四周 8px 边距、垂直节奏统一（各元素间隔 5px）；
    // 展开态追加统计子卡片与重置子卡片。
    constexpr int COLLAPSED_HEIGHT = 91;
    constexpr int EXPANDED_HEIGHT = 244;
    constexpr int CORNER_RADIUS = 16;

} // namespace CodexQuotaBar
