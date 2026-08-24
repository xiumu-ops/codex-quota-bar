#pragma once

#include "Core/Models.h"
#include "Core/SimpleJson.h"
#include <string>

namespace CodexQuotaBar {

    class CodexClient {
    public:
        // 无状态静态实现：可由任意工作线程调用，不持有任何成员
        static QuotaSnapshot FetchSnapshot();

    private:
        static QuotaSnapshot ParseRateLimits(const std::wstring& jsonStr);
        static void ParseWindow(const JsonValue& obj, QuotaWindow& out);
    };

} // namespace CodexQuotaBar
