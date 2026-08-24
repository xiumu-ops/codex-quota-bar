#pragma once

namespace CodexQuotaBar {

    constexpr int BASE_DPI = 96;
    constexpr int BAR_WIDTH = 380;
    // 紧凑悬浮卡片：内容四周 8px 边距、垂直节奏统一（各元素间隔 5px）；
    // 展开态追加统计子卡片与重置子卡片。
    constexpr int COLLAPSED_HEIGHT = 91;
    constexpr int EXPANDED_HEIGHT = 244;
    constexpr int CORNER_RADIUS = 16;

} // namespace CodexQuotaBar
