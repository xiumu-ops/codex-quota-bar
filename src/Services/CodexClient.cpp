#include "Services/CodexClient.h"
#include "Services/CodexAppServerClient.h"
#include <cmath>
#include <algorithm>
#include <ctime>

namespace CodexQuotaBar {

    static std::wstring FormatResetTimestamp(int64_t timestamp) {
        if (timestamp <= 0) return L"";
        if (timestamp > 100000000000LL) {
            timestamp /= 1000;
        }
        time_t t = static_cast<time_t>(timestamp);
        tm ltm;
        if (localtime_s(&ltm, &t) != 0) return L"";

        wchar_t buf[64];
        swprintf_s(buf, L"%02d月%02d日 %02d:%02d",
            ltm.tm_mon + 1,
            ltm.tm_mday,
            ltm.tm_hour,
            ltm.tm_min);
        return std::wstring(buf);
    }

    static std::wstring TrimFixedNumber(std::wstring value) {
        const size_t dot = value.find(L'.');
        if (dot == std::wstring::npos) return value;
        while (!value.empty() && value.back() == L'0') value.pop_back();
        if (!value.empty() && value.back() == L'.') value.pop_back();
        return value;
    }

    static std::wstring FormatTokenCount(int64_t tokens) {
        if (tokens < 0) return L"";
        if (tokens < 1000) return std::to_wstring(tokens);

        struct Unit {
            int64_t divisor;
            wchar_t suffix;
        };
        constexpr Unit units[] = {
            { 1000000000000LL, L'T' },
            { 1000000000LL, L'B' },
            { 1000000LL, L'M' },
            { 1000LL, L'K' }
        };

        for (const Unit& unit : units) {
            if (tokens < unit.divisor) continue;
            const double scaled = static_cast<double>(tokens) / static_cast<double>(unit.divisor);
            const int decimals = scaled < 10.0 ? 2 : 1;
            wchar_t number[32] = {};
            swprintf_s(number, decimals == 2 ? L"%.2f" : L"%.1f", scaled);
            return TrimFixedNumber(number) + L' ' + unit.suffix;
        }
        return std::to_wstring(tokens);
    }

    static std::wstring FormatDuration(int64_t seconds) {
        if (seconds < 0) return L"";
        if (seconds < 3600) {
            return std::to_wstring((seconds + 59) / 60) + L" 分钟";
        }
        const double hours = static_cast<double>(seconds) / 3600.0;
        wchar_t buf[32] = {};
        if (std::abs(hours - std::round(hours)) < 1e-9) {
            swprintf_s(buf, L"%lld 小时", static_cast<long long>(std::round(hours)));
        } else {
            swprintf_s(buf, L"%.1f 小时", hours);
        }
        return buf;
    }

    static bool ParseAccountUsage(const std::wstring& jsonStr, TokenStats& stats) {
        stats = {};
        if (jsonStr.empty()) return false;

        JsonValue root;
        if (!JsonParser::TryParse(jsonStr, root) || !root.is_object()) return false;
        const JsonValue* result = root[L"result"].is_object() ? &root[L"result"] : &root;
        const JsonValue& summary = (*result)[L"summary"];
        if (!summary.is_object()) return false;

        bool fieldsValid = true;
        auto readNullableNonNegative = [&](const wchar_t* key, int64_t& value) {
            const JsonValue& field = summary[key];
            if (field.is_null()) return false;
            if (!field.is_number()) {
                fieldsValid = false;
                return false;
            }
            value = field.as_int64(-1);
            if (value < 0) {
                fieldsValid = false;
                return false;
            }
            return true;
        };

        int64_t value = 0;
        if (readNullableNonNegative(L"lifetimeTokens", value)) {
            stats.totalTokens = FormatTokenCount(value);
        }
        if (readNullableNonNegative(L"peakDailyTokens", value)) {
            stats.peakTokens = FormatTokenCount(value);
        }
        if (readNullableNonNegative(L"longestRunningTurnSec", value)) {
            stats.longestTask = FormatDuration(value);
        }
        if (readNullableNonNegative(L"currentStreakDays", value)) {
            stats.streakDays = std::to_wstring(value) + L"天";
        }

        return fieldsValid;
    }

    static bool ParseWindowUsedPercent(const JsonValue& obj, double& usedPercent) {
        if (obj.is_object() && obj.has_key(L"usedPercent") && obj[L"usedPercent"].is_number()) {
            usedPercent = obj[L"usedPercent"].as_double(0.0);
            return true;
        }
        if (obj.is_object() && obj.has_key(L"used_percent") && obj[L"used_percent"].is_number()) {
            usedPercent = obj[L"used_percent"].as_double(0.0);
            return true;
        }
        if (obj.is_object() && obj.has_key(L"used") && obj[L"used"].is_number() &&
            obj.has_key(L"limit") && obj[L"limit"].is_number()) {
            double used = obj[L"used"].as_double(0.0);
            double limit = obj[L"limit"].as_double(0.0);
            if (limit > 0.0) {
                usedPercent = used / limit * 100.0;
                return true;
            }
        }
        return false;
    }

    void CodexClient::ParseWindow(const JsonValue& obj, QuotaWindow& out) {
        double usedPercent = 0.0;
        if (!ParseWindowUsedPercent(obj, usedPercent)) {
            out.available = false;
            out.usedPercent = 0.0;
            out.remainingPercent = 0.0;
            return;
        }

        out.available = true;
        out.usedPercent = std::clamp(usedPercent, 0.0, 100.0);
        out.remainingPercent = 100.0 - out.usedPercent;

        if (obj.has_key(L"windowDurationMins") && obj[L"windowDurationMins"].is_number()) {
            out.windowDurationMins = obj[L"windowDurationMins"].as_int64(0);
        } else if (obj.has_key(L"window_duration_mins") && obj[L"window_duration_mins"].is_number()) {
            out.windowDurationMins = obj[L"window_duration_mins"].as_int64(0);
        }

        int64_t resetTs = 0;
        if (obj.has_key(L"resetAt") && obj[L"resetAt"].is_number()) {
            resetTs = obj[L"resetAt"].as_int64(0);
        } else if (obj.has_key(L"resetsAt") && obj[L"resetsAt"].is_number()) {
            resetTs = obj[L"resetsAt"].as_int64(0);
        } else if (obj.has_key(L"reset_at") && obj[L"reset_at"].is_number()) {
            resetTs = obj[L"reset_at"].as_int64(0);
        }
        if (resetTs > 100000000000LL) {
            resetTs /= 1000;
        }
        out.resetTimestamp = resetTs;
        out.resetTimeString = FormatResetTimestamp(resetTs);
    }

    static const JsonValue* FindWindow(const JsonValue& obj, const wchar_t* const* aliasKeys, size_t count) {
        if (!obj.is_object()) return nullptr;
        for (size_t i = 0; i < count; ++i) {
            const JsonValue* v = &obj[aliasKeys[i]];
            if (v->is_object()) return v;
        }
        return nullptr;
    }

    QuotaSnapshot CodexClient::ParseRateLimits(const std::wstring& jsonStr) {
        QuotaSnapshot snap;
        snap.fetchedAt = std::chrono::system_clock::now();

        if (jsonStr.empty()) {
            snap.success = false;
            snap.errorMessage = L"未收到服务响应";
            return snap;
        }

        JsonValue root;
        if (!JsonParser::TryParse(jsonStr, root) || !root.is_object()) {
            snap.success = false;
            snap.errorMessage = L"响应格式错误";
            return snap;
        }

        const JsonValue* result = &root;
        if (root[L"result"].is_object()) result = &root[L"result"];

        const JsonValue& resetCredits = (*result)[L"rateLimitResetCredits"];
        if (resetCredits.is_object()) {
            const JsonValue& availableCount = resetCredits[L"availableCount"];
            if (availableCount.is_number()) {
                const int64_t count = availableCount.as_int64(-1);
                if (count >= 0) {
                    snap.resetCredits.countAvailable = true;
                    snap.resetCredits.availableCount = count;
                }
            }

            const JsonValue& credits = resetCredits[L"credits"];
            if (credits.is_array()) {
                for (const JsonValue& credit : credits.arrVal) {
                    if (!credit.is_object()) continue;
                    const JsonValue& status = credit[L"status"];
                    if (status.is_string() && status.as_string() != L"available") continue;

                    const JsonValue& expiresAt = credit[L"expiresAt"];
                    if (!expiresAt.is_number()) continue;
                    int64_t timestamp = expiresAt.as_int64(0);
                    if (timestamp > 100000000000LL) timestamp /= 1000;
                    if (timestamp <= 0) continue;
                    if (snap.resetCredits.earliestExpiresAt == 0 ||
                        timestamp < snap.resetCredits.earliestExpiresAt) {
                        snap.resetCredits.earliestExpiresAt = timestamp;
                    }
                }
            }
        }

        const wchar_t* windowAliases[] = { L"primary", L"5h", L"5hour", L"5hours" };
        const wchar_t* weeklyAliases[] = { L"secondary", L"7d", L"7day", L"7days", L"weekly", L"week" };

        auto fillFromBucket = [&](const JsonValue& bucket) {
            if (!bucket.is_object()) return;

            QuotaWindow primary;
            QuotaWindow secondary;
            const JsonValue* primaryObj = FindWindow(bucket, windowAliases, _countof(windowAliases));
            const JsonValue* secondaryObj = FindWindow(bucket, weeklyAliases, _countof(weeklyAliases));
            if (primaryObj) ParseWindow(*primaryObj, primary);
            if (secondaryObj) ParseWindow(*secondaryObj, secondary);

            const QuotaWindow* candidates[] = { &primary, &secondary };
            auto chooseClosest = [&](int64_t targetMinutes, int fallbackIndex) {
                int best = -1;
                int64_t bestDifference = INT64_MAX;
                for (int i = 0; i < 2; ++i) {
                    const QuotaWindow& candidate = *candidates[i];
                    if (!candidate.available || candidate.windowDurationMins <= 0) continue;
                    int64_t difference = candidate.windowDurationMins > targetMinutes
                        ? candidate.windowDurationMins - targetMinutes
                        : targetMinutes - candidate.windowDurationMins;
                    if (difference < bestDifference) {
                        bestDifference = difference;
                        best = i;
                    }
                }

                const int64_t tolerance = (std::max<int64_t>)(60, targetMinutes / 4);
                if (best >= 0 && bestDifference <= tolerance) return best;

                const QuotaWindow& fallback = *candidates[fallbackIndex];
                if (fallback.available && fallback.windowDurationMins <= 0) return fallbackIndex;
                return -1;
            };

            const int windowChoice = chooseClosest(300, 0);
            const int weeklyChoice = chooseClosest(10080, 1);

            if (!snap.window.available && windowChoice >= 0) {
                snap.window = *candidates[windowChoice];
            }
            if (!snap.weekly.available && weeklyChoice >= 0 && weeklyChoice != windowChoice) {
                snap.weekly = *candidates[weeklyChoice];
            }
        };

        if ((*result)[L"rateLimits"].is_object()) {
            fillFromBucket((*result)[L"rateLimits"]);
        }

        const JsonValue& byId = (*result)[L"rateLimitsByLimitId"];
        if (byId.is_object() && !byId.objVal.empty()) {
            const JsonValue* selected = nullptr;
            auto codex = byId.objVal.find(L"codex");
            if (codex != byId.objVal.end() && codex->second.is_object()) {
                selected = &codex->second;
            } else if (byId.objVal.size() == 1 && byId.objVal.begin()->second.is_object()) {
                selected = &byId.objVal.begin()->second;
            }
            if (selected) fillFromBucket(*selected);
        }

        snap.success = snap.window.available || snap.weekly.available;
        if (!snap.success) {
            snap.errorMessage = L"未找到限额字段";
        }

        return snap;
    }

    QuotaSnapshot CodexClient::FetchSnapshot() {
        std::wstring response;
        std::wstring usageResponse;
        std::wstring usageError;
        std::wstring error;
        if (CodexAppServerClient::ReadAccountData(
                response, usageResponse, usageError, error)) {
            QuotaSnapshot snapshot = ParseRateLimits(response);
            const bool quotaSuccess = snapshot.success;
            const bool statsSuccess = ParseAccountUsage(usageResponse, snapshot.stats);
            snapshot.statsSynchronized = statsSuccess;
            if (quotaSuccess && statsSuccess) return snapshot;

            snapshot.success = false;
            if (!quotaSuccess) {
                if (error.empty()) error = snapshot.errorMessage;
            } else if (!statsSuccess) {
                snapshot.errorMessage = usageError.empty()
                    ? L"Token 统计响应格式错误"
                    : L"Token 统计同步失败：" + usageError;
            }

            if (quotaSuccess || !usageResponse.empty()) return snapshot;
            if (error.empty()) error = snapshot.errorMessage;
        }

        QuotaSnapshot unavailable;
        unavailable.success = false;
        unavailable.fetchedAt = std::chrono::system_clock::now();
        unavailable.errorMessage = error.empty() ? L"无法读取 Codex 额度" : error;
        return unavailable;
    }

} // namespace CodexQuotaBar
