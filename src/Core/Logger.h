#pragma once

#include <string_view>

namespace CodexQuotaBar {

    enum class LogLevel {
        Info,
        Warning,
        Error
    };

    // 隐私安全的本地诊断日志。调用方不得写入原始 App Server JSON、令牌或账户标识。
    void WriteLog(LogLevel level, std::wstring_view message) noexcept;

} // namespace CodexQuotaBar
