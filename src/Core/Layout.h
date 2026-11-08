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

    inline bool ShouldUseSmallWindowCorner(bool /*miniVisible*/ = false, float /*userScale*/ = 1.0f) {
        return false; // 统一全应用基准圆角体系，迷你栏与普通栏保持一致的 8px DWM 硬件圆角
    }

    // Windows 11 DWM 窗口圆角与外框背景均统一基于 CORNER_RADIUS (8 DIP) 随显示器物理 DPI 缩放，
    // 不随应用内用户缩放档位跳变，使全应用圆角保持严谨一致，避免双层圆角与裁剪冲突。
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
        const int physicalHeight = ScaleEven(static_cast<float>(MINI_HEIGHT), safeUiScale);
        const float maxHalfPill = std::floor(
            (std::max)(0.0f, static_cast<float>(physicalHeight) - contentInset * 2.0f) / 2.0f);
        // 内嵌胶囊圆角与外框圆角保持同心统一（R_pill = R_outer - inset），使四角与直边留白均匀一致。
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
        bool /*miniVisible*/ = true)
    {
        const float safeDpiScale = monitorDpiScale > 0.0f ? monitorDpiScale : 1.0f;
        const float safeUserScale = userScale > 0.0f ? userScale : 1.0f;
        const float uiScale = safeDpiScale * safeUserScale;
        return ScaledMiniBarMetrics(
            uiScale,
            WindowOuterCornerRadius(safeDpiScale, false));
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

    // 先将单个额度槽缩放到物理像素，再组合总宽度，保证双额度始终严格等宽且总宽为偶数。
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
    // 物理偶数像素后再组合：四周边距留白统一为 8px，行与卡片之间的间隔边距统一为 6px。
    namespace StandardLayout {
        inline constexpr float kMargin = 8.0f;          // 统一四周边距留白（左、右、上、下及展开栏底部）
        inline constexpr float kGap = 6.0f;             // 统一各行与各子卡片之间的间隔边距
        inline constexpr float kPadX = kMargin;         // 保持兼容别名（统一 8px）
        inline constexpr float kRowTextHeight = 20.0f;
        inline constexpr float kTrackOffset = 24.0f;
        inline constexpr float kTrackHeight = 6.0f;
        inline constexpr float kRowTops[2] = { kMargin, kMargin + 30.0f + kGap }; // { 8.0f, 44.0f }
        inline constexpr float kFooterTop = kRowTops[1] + 30.0f + kGap;           // 80.0f
        inline constexpr float kFooterHeight = 16.0f;
        inline constexpr float kDetailLeft = 114.0f;
        inline constexpr float kDetailRight = 306.0f;
        inline constexpr float kSyncCenterX = 346.0f;
        inline constexpr float kChevronCenterX = 364.0f;
        inline constexpr float kCardGapTop = kGap;      // 6.0f：展开态第一张子卡片上方间隔（与普通栏间隔边距统一）
        inline constexpr float kCardHeight = 66.0f;
        inline constexpr float kCardGap = kGap;         // 6.0f：子卡片之间的间隔（与普通栏间隔边距统一）
        inline constexpr float kCardGapBottom = kMargin;// 8.0f：展开态底部留白边距（与普通栏留白边距统一）
    }

    static_assert(
        static_cast<float>(COLLAPSED_HEIGHT) + StandardLayout::kCardGapTop +
            StandardLayout::kCardHeight * 2.0f + StandardLayout::kCardGap +
            StandardLayout::kCardGapBottom == static_cast<float>(EXPANDED_HEIGHT),
        "展开态逻辑布局必须与 EXPANDED_HEIGHT 一致");

    inline float SnapPx(float logicalPx, float uiScale) {
        return static_cast<float>(ScaleEven(logicalPx, uiScale));
    }

    // 展开态高度由与渲染完全相同的取整片段累加得出，保证底部留白严格对应偶数像素。
    inline int ExpandedPhysicalHeight(float uiScale) {
        using namespace StandardLayout;
        return static_cast<int>(SnapPx(static_cast<float>(COLLAPSED_HEIGHT), uiScale)) +
               static_cast<int>(SnapPx(kCardGapTop, uiScale)) +
               static_cast<int>(SnapPx(kCardHeight, uiScale)) * 2 +
               static_cast<int>(SnapPx(kCardGap, uiScale)) +
               static_cast<int>(SnapPx(kCardGapBottom, uiScale));
    }

} // namespace CodexQuotaBar
