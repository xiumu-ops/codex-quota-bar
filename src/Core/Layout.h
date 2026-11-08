#pragma once

#include "Core/Constants.h"
#include "Core/DpiHelper.h"
#include "Core/Models.h"

#include <algorithm>
#include <cmath>

namespace CodexQuotaBar {

    struct MiniBarMetrics {
        float outerCornerRadius = 0.0f;
        float contentInset = 0.0f;
        float pillCornerRadius = 0.0f;
        float dividerThickness = 0.0f;
        float outerBorderThickness = 0.0f;
    };

    inline bool ShouldUseSmallWindowCorner(bool miniVisible, float userScale) {
        return miniVisible && userScale < 1.0f;
    }

    // Windows 11 DWM 窗口圆角仅随显示器物理 DPI 缩放（DWMWCP_ROUND = 8 DIP，
    // DWMWCP_ROUNDSMALL = 4 DIP），不随应用内用户缩放档位改变。外框背景与描边必须
    // 与 DWM 硬件圆角共用同一物理像素半径，避免缩放时四角被裁断或出现双层圆角错位。
    inline float WindowOuterCornerRadius(float monitorDpiScale, bool useSmallCorner = false) {
        const float safeDpiScale = monitorDpiScale > 0.0f ? monitorDpiScale : 1.0f;
        const float logicalRadius = useSmallCorner
            ? 4.0f
            : static_cast<float>(CORNER_RADIUS);
        return (std::max)(2.0f, std::round(logicalRadius * safeDpiScale));
    }

    inline MiniBarMetrics ScaledMiniBarMetrics(float uiScale, float outerCornerRadius) {
        const float safeUiScale = uiScale > 0.0f ? uiScale : 1.0f;
        const float clampedOuter = (std::max)(2.0f, std::round(outerCornerRadius));
        const float contentInset = (std::max)(1.0f, std::round(2.0f * safeUiScale));
        const int physicalHeight = Scale(static_cast<float>(MINI_HEIGHT), safeUiScale);
        const float maxHalfPill = std::floor(
            (std::max)(0.0f, static_cast<float>(physicalHeight) - contentInset * 2.0f) / 2.0f);
        // 内嵌胶囊圆角与外框圆角保持同心（R_pill = R_outer - inset），使四角与直边留白均匀一致。
        const float pillCornerRadius = (std::max)(
            2.0f, (std::min)(maxHalfPill, clampedOuter - contentInset));
        const float hairline = (std::max)(1.0f, std::round(safeUiScale));
        return {
            clampedOuter,
            contentInset,
            pillCornerRadius,
            hairline,
            hairline,
        };
    }

    inline MiniBarMetrics ScaledMiniBarMetricsForWindow(
        float monitorDpiScale,
        float userScale,
        bool miniVisible = true)
    {
        const float safeDpiScale = monitorDpiScale > 0.0f ? monitorDpiScale : 1.0f;
        const float safeUserScale = userScale > 0.0f ? userScale : 1.0f;
        const float uiScale = safeDpiScale * safeUserScale;
        const bool smallCorner = ShouldUseSmallWindowCorner(miniVisible, safeUserScale);
        return ScaledMiniBarMetrics(
            uiScale,
            WindowOuterCornerRadius(safeDpiScale, smallCorner));
    }

    inline MiniBarMetrics ScaledMiniBarMetrics(float userScale) {
        return ScaledMiniBarMetricsForWindow(1.0f, userScale, true);
    }

    inline int MiniQuotaSlotCount(const QuotaSnapshot& snapshot) {
        return snapshot.window.available && snapshot.weekly.available ? 2 : 1;
    }

    inline int MiniBarLogicalWidth(const QuotaSnapshot& snapshot) {
        return MiniQuotaSlotCount(snapshot) == 2
            ? MINI_DUAL_WIDTH
            : MINI_SINGLE_WIDTH;
    }

    // 先将单个额度槽缩放到物理像素，再组合总宽度，保证双额度始终严格等宽。
    inline int MiniBarPhysicalWidth(const QuotaSnapshot& snapshot, float uiScale) {
        return MiniQuotaSlotCount(snapshot) * Scale(
            static_cast<float>(MINI_SINGLE_WIDTH), uiScale);
    }

    inline float MiniDividerHeight(int physicalHeight, const MiniBarMetrics& metrics) {
        const float pillHeight = (std::max)(
            0.0f,
            static_cast<float>(physicalHeight) - metrics.contentInset * 2.0f);
        return (std::max)(
            metrics.dividerThickness,
            pillHeight - metrics.pillCornerRadius * 2.0f);
    }

    // 标准栏与展开态布局（逻辑像素）。所有位置与尺寸都通过 SnapPx 统一取整到
    // 物理像素后再组合：同一逻辑坐标在任意 DPI × 用户缩放下落在同一物理像素，
    // 两行额度的内部结构严格一致，底部状态行的文字、同步点与展开箭头共用同一中线。
    namespace StandardLayout {
        inline constexpr float kPadX = 10.0f;
        inline constexpr float kRowTops[2] = { 4.0f, 38.0f };
        inline constexpr float kRowTextHeight = 20.0f;
        inline constexpr float kTrackOffset = 24.0f;
        inline constexpr float kTrackHeight = 6.0f;
        inline constexpr float kFooterTop = 72.0f;
        inline constexpr float kFooterHeight = 16.0f;
        inline constexpr float kDetailLeft = 114.0f;
        inline constexpr float kDetailRight = 306.0f;
        inline constexpr float kSyncCenterX = 346.0f;
        inline constexpr float kChevronCenterX = 364.0f;
        inline constexpr float kCardGapTop = 8.0f;
        inline constexpr float kCardHeight = 66.0f;
        inline constexpr float kCardGap = 6.0f;
        inline constexpr float kCardGapBottom = 6.0f;
    }

    static_assert(
        static_cast<float>(COLLAPSED_HEIGHT) + StandardLayout::kCardGapTop +
            StandardLayout::kCardHeight * 2.0f + StandardLayout::kCardGap +
            StandardLayout::kCardGapBottom == static_cast<float>(EXPANDED_HEIGHT),
        "展开态逻辑布局必须与 EXPANDED_HEIGHT 一致");

    inline float SnapPx(float logicalPx, float uiScale) {
        return static_cast<float>(Scale(logicalPx, uiScale));
    }

    // 展开态高度由与渲染完全相同的取整片段累加得出，保证底部留白始终等于取整后的 6px。
    inline int ExpandedPhysicalHeight(float uiScale) {
        using namespace StandardLayout;
        return Scale(static_cast<float>(COLLAPSED_HEIGHT), uiScale) +
               Scale(kCardGapTop, uiScale) +
               Scale(kCardHeight, uiScale) * 2 +
               Scale(kCardGap, uiScale) +
               Scale(kCardGapBottom, uiScale);
    }

} // namespace CodexQuotaBar
