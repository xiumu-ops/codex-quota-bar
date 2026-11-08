#pragma once

#include <cmath>

namespace CodexQuotaBar {

    inline int SnapEven(int v) {
        return (v % 2 != 0) ? (v + 1) : v;
    }

    inline int Scale(float logicalPx, float dpiScale) {
        return static_cast<int>(std::round(logicalPx * dpiScale));
    }

    inline int ScaleEven(float logicalPx, float dpiScale) {
        return SnapEven(Scale(logicalPx, dpiScale));
    }

    inline float ScaleF(float logicalPx, float dpiScale) {
        return logicalPx * dpiScale;
    }

} // namespace CodexQuotaBar
