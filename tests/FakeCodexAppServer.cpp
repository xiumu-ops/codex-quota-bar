#include <cstdlib>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

namespace {

bool Contains(const std::string& value, const char* expected) {
    return value.find(expected) != std::string::npos;
}

bool ValidateCommon(const std::string& line) {
    // 官方 app-server 在线路上省略 JSON-RPC 版本头。
    return !Contains(line, "\"jsonrpc\"");
}

int ProtocolFailure(int id, const char* message) {
    if (id > 0) {
        std::cout << "{\"id\":" << id
                  << ",\"error\":{\"code\":-32600,\"message\":\""
                  << message << "\"}}" << std::endl;
    }
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2 || std::string(argv[1]) != "app-server") {
        return 64;
    }

    char* rawScenario = nullptr;
    size_t scenarioLength = 0;
    _dupenv_s(&rawScenario, &scenarioLength, "CODEX_QUOTA_FAKE_SCENARIO");
    const std::string scenario = rawScenario ? rawScenario : "happy";
    std::free(rawScenario);

    std::string line;
    int step = 0;
    while (std::getline(std::cin, line)) {
        if (!ValidateCommon(line)) return ProtocolFailure(0, "jsonrpc header must be omitted");

        if (step == 0) {
            if (!Contains(line, "\"method\":\"initialize\"") ||
                !Contains(line, "\"id\":1") ||
                !Contains(line, "\"name\":\"codex_quota_bar\"") ||
                !Contains(line, "\"version\":\"2.5.8\"")) {
                return ProtocolFailure(1, "invalid initialize request");
            }
            // 非 JSON 诊断行用于覆盖客户端的输出降噪路径。
            std::cerr << "fake app-server diagnostic" << std::endl;
            std::cout << "{\"id\":1,\"result\":{\"userAgent\":\"fake-codex\"}}" << std::endl;
        } else if (step == 1) {
            if (!Contains(line, "\"method\":\"initialized\"") || Contains(line, "\"id\"")) {
                return ProtocolFailure(0, "invalid initialized notification");
            }
        } else if (step == 2) {
            if (!Contains(line, "\"method\":\"account/rateLimits/read\"") ||
                !Contains(line, "\"id\":2")) {
                return ProtocolFailure(2, "invalid rate-limits request");
            }
            if (scenario == "malformed_rate_limits") {
                std::cout << "{not-json" << std::endl;
                return 0;
            }
            if (scenario == "oversized_output") {
                std::cout << std::string(1024 * 1024 + 4096, 'x') << std::flush;
                return 0;
            }

            // 无关通知应被客户端忽略，随后再读取 id=2 的真正响应。
            std::cout << "{\"method\":\"account/rateLimits/updated\",\"params\":{}}" << std::endl;
            std::cout
                << "{\"id\":2,\"result\":{\"rateLimits\":null,"
                   "\"rateLimitsByLimitId\":{\"codex\":{"
                   "\"primary\":{\"usedPercent\":37.5,\"windowDurationMins\":300,\"resetsAt\":1893456000},"
                   "\"secondary\":{\"usedPercent\":90,\"windowDurationMins\":10080,\"resetsAt\":1893888000}"
                   "}},"
                   "\"rateLimitResetCredits\":{\"availableCount\":2,\"credits\":["
                   "{\"status\":\"available\",\"expiresAt\":1789946796},"
                   "{\"status\":\"available\",\"expiresAt\":1792538796}"
                   "]}}}"
                << std::endl;
        } else if (step == 3) {
            if (!Contains(line, "\"method\":\"account/usage/read\"") ||
                !Contains(line, "\"id\":3") || !Contains(line, "\"params\":{}")) {
                return ProtocolFailure(3, "invalid usage request");
            }

            if (scenario == "usage_error") {
                std::cout << "{\"id\":3,\"error\":{\"code\":-32000,\"message\":\"usage unavailable\"}}"
                          << std::endl;
            } else if (scenario == "nullable") {
                std::cout
                    << "{\"id\":3,\"result\":{\"summary\":{"
                       "\"lifetimeTokens\":null,\"peakDailyTokens\":null,"
                       "\"longestRunningTurnSec\":null,\"currentStreakDays\":null"
                       "}}}"
                    << std::endl;
            } else if (scenario == "invalid_type") {
                std::cout
                    << "{\"id\":3,\"result\":{\"summary\":{"
                       "\"lifetimeTokens\":\"not-a-number\",\"peakDailyTokens\":null,"
                       "\"longestRunningTurnSec\":null,\"currentStreakDays\":null"
                       "}}}"
                    << std::endl;
            } else if (scenario == "token_units_small") {
                std::cout
                    << "{\"id\":3,\"result\":{\"summary\":{"
                       "\"lifetimeTokens\":2300000,\"peakDailyTokens\":1200,"
                       "\"longestRunningTurnSec\":null,\"currentStreakDays\":null"
                       "}}}"
                    << std::endl;
            } else if (scenario == "token_units_large") {
                std::cout
                    << "{\"id\":3,\"result\":{\"summary\":{"
                       "\"lifetimeTokens\":4500000000000,\"peakDailyTokens\":3400000000,"
                       "\"longestRunningTurnSec\":null,\"currentStreakDays\":null"
                       "}}}"
                    << std::endl;
            } else {
                std::cout
                    << "{\"id\":3,\"result\":{\"summary\":{"
                       "\"lifetimeTokens\":1234567,\"peakDailyTokens\":98765,"
                       "\"longestRunningTurnSec\":5400,\"currentStreakDays\":12"
                       "}}}"
                    << std::endl;
            }
            if (scenario == "hang_after_response") {
                std::this_thread::sleep_for(std::chrono::seconds(10));
            }
            return 0;
        } else {
            return ProtocolFailure(0, "unexpected request");
        }
        ++step;
    }

    return step == 4 ? 0 : 3;
}
