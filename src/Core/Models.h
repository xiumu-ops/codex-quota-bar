#pragma once

#include <string>
#include <vector>
#include <chrono>
#include <cstdint>

namespace CodexQuotaBar {

    enum class SyncState {
        Waiting,  // 尚未开始同步
        Syncing,  // 正在抓取配额
        Synced,   // 最近一次同步成功
        Failed    // 最近一次同步失败
    };

    struct QuotaWindow {
        double usedPercent = 0.0;
        double remainingPercent = 100.0;
        int64_t windowDurationMins = 0;
        int64_t resetTimestamp = 0;             // 重置时间戳（统一为秒），0 表示未返回
        std::wstring resetTimeString;           // 重置时间（月-日 星期 时:分），空表示服务未返回
        bool available = false;
    };

    struct TokenStats {
        // 统计字段默认空：未收到 STATS 数据时渲染 "--"，绝不展示占位假数值
        std::wstring totalTokens;
        std::wstring peakTokens;
        std::wstring longestTask;
        std::wstring streakDays;
    };

    struct RateLimitResetCredits {
        bool countAvailable = false;
        int64_t availableCount = 0;
        // 可用重置额度中最早的 expiresAt（秒）；详情未返回时为 0。
        int64_t earliestExpiresAt = 0;
    };

    struct QuotaSnapshot {
        bool success = false;
        // true 表示 account/usage/read 已返回结构有效的 summary；summary
        // 中的各统计值允许为 null，因此不能用字段是否为空判断同步是否成功。
        bool statsSynchronized = false;
        std::wstring errorMessage;
        QuotaWindow window;  // 5小时滚动周期 (Primary)
        QuotaWindow weekly;  // 7天用量周期 (Secondary)
        TokenStats stats;    // 统计数据
        RateLimitResetCredits resetCredits; // 官方可用额度重置次数及到期时间
        std::chrono::system_clock::time_point fetchedAt;
    };

} // namespace CodexQuotaBar
