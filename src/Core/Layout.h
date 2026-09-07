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

    inline MiniBarMetrics ScaledMiniBarMetrics(float uiScale) {
        return {
            (std::max)(2.0f, std::floor(static_cast<float>(CORNER_RADIUS) * uiScale)),
            (std::max)(1.0f, std::round(4.0f * uiScale)),
            (std::max)(2.0f, std::floor(5.0f * uiScale)),
            (std::max)(1.0f, std::round(uiScale)),
            (std::max)(1.0f, std::round(uiScale)),
        };
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

} // namespace CodexQuotaBar
