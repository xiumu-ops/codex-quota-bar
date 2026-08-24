#pragma once

#include <cmath>

namespace CodexQuotaBar {

    inline int Scale(float logicalPx, float dpiScale) {
        return static_cast<int>(std::round(logicalPx * dpiScale));
    }

    inline float ScaleF(float logicalPx, float dpiScale) {
        return logicalPx * dpiScale;
    }

} // namespace CodexQuotaBar
