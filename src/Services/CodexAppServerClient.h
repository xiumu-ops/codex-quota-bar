#pragma once

#include <string>

namespace CodexQuotaBar {

    // 通过官方 codex app-server 的 stdio JSON-RPC 同时读取账户额度和
    // Token 活动统计。两个输出分别是对应请求的完整响应行；统计接口
    // 不可用时 usageResponseJson 为空，但已取得的额度仍可正常展示。
    class CodexAppServerClient {
    public:
        static bool ReadAccountData(
            std::wstring& rateLimitsResponseJson,
            std::wstring& usageResponseJson,
            std::wstring& usageErrorMessage,
            std::wstring& errorMessage);
    };

} // namespace CodexQuotaBar
