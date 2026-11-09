#pragma once

#include <array>
#include <cmath>
#include <limits>

namespace CodexQuotaBar {

    inline constexpr std::array<float, 9> USER_SCALE_LEVELS = {
        0.2f, 0.4f, 0.6f, 0.8f, 1.0f, 1.2f, 1.4f, 1.6f, 1.8f
    };
    inline constexpr int DEFAULT_USER_SCALE_LEVEL = 4;
    inline constexpr int MIN_USER_SCALE_PERCENT = 20;
    inline constexpr int MAX_USER_SCALE_PERCENT = 180;
    inline constexpr int USER_SCALE_PERCENT_STEP = 20;

    inline constexpr std::array<int, 5> REFRESH_INTERVAL_MINUTES = {
        1, 5, 10, 30, 60
    };

    inline bool IsSupportedUserScale(double value) {
        const double scaled = value * 100.0;
        if (!std::isfinite(scaled)) return false;
        const double roundedInt = std::round(scaled);
        if (roundedInt < MIN_USER_SCALE_PERCENT || roundedInt > MAX_USER_SCALE_PERCENT ||
            std::fabs(scaled - roundedInt) > 1e-4) return false;
        const int percent = static_cast<int>(roundedInt);
        return (percent % USER_SCALE_PERCENT_STEP) == 0;
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
