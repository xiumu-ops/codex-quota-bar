#pragma once

namespace CodexQuotaBar {

    inline constexpr wchar_t APP_VERSION[] = L"3.1.0";
    inline constexpr char APP_VERSION_UTF8[] = "3.1.0";
    inline constexpr wchar_t PIPE_NAME_PREFIX[] = L"\\\\.\\pipe\\Codex-Quota-Bar_Pipe_";
    inline constexpr wchar_t MUTEX_NAME[] = L"Local\\Codex-Quota-Bar_Mutex_Session";

    constexpr int BASE_DPI = 96;
    constexpr int BAR_WIDTH = 380;
    // 标准面板包含额度行和同步状态，展开态追加统计与重置卡片。
    constexpr int COLLAPSED_HEIGHT = 196;
    constexpr int EXPANDED_HEIGHT = 348;
    constexpr int MINI_DUAL_WIDTH = 112;
    constexpr int MINI_SINGLE_WIDTH = 56;
    constexpr int MINI_HEIGHT = 28;
    constexpr int CORNER_RADIUS = 8;

} // namespace CodexQuotaBar
