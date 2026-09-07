#pragma once

#include "Core/Constants.h"
#include "Core/Models.h"

namespace CodexQuotaBar {

    struct MiniBarMetrics {
        float outerCornerRadius = 0.0f;
        float contentInset = 0.0f;
        float pillCornerRadius = 0.0f;
        float dividerThickness = 0.0f;
    };

    inline MiniBarMetrics ScaledMiniBarMetrics(float uiScale) {
        return {
            static_cast<float>(CORNER_RADIUS) * uiScale,
            4.0f * uiScale,
            5.0f * uiScale,
            1.0f * uiScale,
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

} // namespace CodexQuotaBar
