#pragma once

#include <array>
#include <cmath>
#include <limits>

namespace CodexQuotaBar {

    inline constexpr std::array<float, 5> USER_SCALE_LEVELS = {
        0.75f, 0.875f, 1.0f, 1.125f, 1.25f
    };
    inline constexpr int DEFAULT_USER_SCALE_LEVEL = 2;

    inline constexpr std::array<int, 5> REFRESH_INTERVAL_MINUTES = {
        1, 5, 10, 30, 60
    };

    inline bool IsSupportedUserScale(double value) {
        if (!std::isfinite(value)) return false;
        for (const float scale : USER_SCALE_LEVELS) {
            if (value == static_cast<double>(scale)) return true;
        }
        return false;
    }

    inline bool TryParseSupportedRefreshInterval(double value, int& minutes) {
        if (!std::isfinite(value) || std::floor(value) != value ||
            value < static_cast<double>((std::numeric_limits<int>::min)()) ||
            value > static_cast<double>((std::numeric_limits<int>::max)())) {
            return false;
        }
        const int parsed = static_cast<int>(value);
        for (const int supported : REFRESH_INTERVAL_MINUTES) {
            if (parsed == supported) {
                minutes = parsed;
                return true;
            }
        }
        return false;
    }

    inline bool TryParseConfigInteger(double value, int& parsed) {
        if (!std::isfinite(value) || std::floor(value) != value ||
            value < static_cast<double>((std::numeric_limits<int>::min)()) ||
            value > static_cast<double>((std::numeric_limits<int>::max)())) {
            return false;
        }
        parsed = static_cast<int>(value);
        return true;
    }

} // namespace CodexQuotaBar
