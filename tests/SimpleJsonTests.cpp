#include "Core/SimpleJson.h"

#include <iostream>
#include <string>

using CodexQuotaBar::JsonParser;
using CodexQuotaBar::JsonValue;

namespace {
int g_failures = 0;

void Expect(bool condition, const char* message) {
    if (condition) std::cout << "  [PASS] " << message << '\n';
    else {
        std::cerr << "  [FAIL] " << message << '\n';
        ++g_failures;
    }
}

bool Parses(const std::wstring& json) {
    JsonValue value;
    return JsonParser::TryParse(json, value);
}
} // namespace

int main() {
    Expect(Parses(L"{\"a\":[true,false,null,-12.5e2],\"b\":\"\\uD83D\\uDE00\"}"),
           "valid values and surrogate pairs parse");
    Expect(!Parses(L"{\"a\":1,}"), "object trailing comma is rejected");
    Expect(!Parses(L"[1,]"), "array trailing comma is rejected");
    Expect(!Parses(L"01"), "leading zero is rejected");
    Expect(!Parses(L"\"\\uDC00\""), "unpaired low surrogate is rejected");
    Expect(!Parses(L"1e9999"), "non-finite number is rejected");

    std::wstring maxDepth;
    for (size_t i = 0; i < JsonParser::kMaxDepth; ++i) maxDepth += L'[';
    maxDepth += L'0';
    for (size_t i = 0; i < JsonParser::kMaxDepth; ++i) maxDepth += L']';
    Expect(Parses(maxDepth), "maximum nesting depth parses");
    maxDepth.insert(maxDepth.begin(), L'[');
    maxDepth += L']';
    Expect(!Parses(maxDepth), "nesting above the safety limit is rejected");

    return g_failures == 0 ? 0 : 1;
}
