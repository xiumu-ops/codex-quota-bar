#include "Core/SimpleJson.h"

#include <clocale>
#include <cmath>
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

    std::setlocale(LC_ALL, "German");
    JsonValue localeVal;
    Expect(JsonParser::TryParse(L"{\"usedPercent\":37.5,\"rate\":0.001}", localeVal) &&
           std::abs(localeVal[L"usedPercent"].as_double() - 37.5) < 1e-9 &&
           std::abs(localeVal[L"rate"].as_double() - 0.001) < 1e-9,
           "floating-point numbers parse independently of C runtime locale");
    std::setlocale(LC_ALL, "C");

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
