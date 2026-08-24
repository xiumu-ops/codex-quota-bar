#pragma once

#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace CodexQuotaBar {

    enum class JsonType {
        Null,
        Boolean,
        Number,
        String,
        Array,
        Object
    };

    struct JsonValue {
        JsonType type = JsonType::Null;
        bool boolVal = false;
        double numVal = 0.0;
        // 保留解析时的数字词法值。Hook 安装器会重写整个 hooks.json；若只
        // 保存 double，超过 2^53 的整数会在无备份场景下被永久舍入。
        std::wstring numberText;
        std::wstring strVal;
        std::vector<JsonValue> arrVal;
        std::map<std::wstring, JsonValue> objVal;

        bool is_null() const { return type == JsonType::Null; }
        bool is_bool() const { return type == JsonType::Boolean; }
        bool is_number() const { return type == JsonType::Number; }
        bool is_string() const { return type == JsonType::String; }
        bool is_array() const { return type == JsonType::Array; }
        bool is_object() const { return type == JsonType::Object; }

        const JsonValue& operator[](const std::wstring& key) const {
            static const JsonValue nullVal;
            if (type != JsonType::Object) return nullVal;
            auto it = objVal.find(key);
            return it != objVal.end() ? it->second : nullVal;
        }

        const JsonValue& operator[](size_t index) const {
            static const JsonValue nullVal;
            if (type != JsonType::Array || index >= arrVal.size()) return nullVal;
            return arrVal[index];
        }

        bool has_key(const std::wstring& key) const {
            return type == JsonType::Object && objVal.find(key) != objVal.end();
        }

        double as_double(double defaultVal = 0.0) const {
            return type == JsonType::Number ? numVal : defaultVal;
        }

        int as_int(int defaultVal = 0) const {
            if (type != JsonType::Number || !std::isfinite(numVal) ||
                numVal < static_cast<double>((std::numeric_limits<int>::min)()) ||
                numVal > static_cast<double>((std::numeric_limits<int>::max)())) {
                return defaultVal;
            }
            return static_cast<int>(numVal);
        }

        int64_t as_int64(int64_t defaultVal = 0) const {
            // double 无法精确表示 INT64_MAX；使用开区间上界避免越界转换。
            constexpr double kMin = -9223372036854775808.0;
            constexpr double kUpperExclusive = 9223372036854775808.0;
            if (type != JsonType::Number || !std::isfinite(numVal) ||
                numVal < kMin || numVal >= kUpperExclusive) {
                return defaultVal;
            }
            return static_cast<int64_t>(numVal);
        }

        bool as_bool(bool defaultVal = false) const {
            return type == JsonType::Boolean ? boolVal : defaultVal;
        }

        std::wstring as_string(const std::wstring& defaultVal = L"") const {
            return type == JsonType::String ? strVal : defaultVal;
        }
    };

    class JsonParser {
    public:
        static constexpr size_t kMaxDepth = 32;

        static bool TryParse(const std::wstring& json, JsonValue& out) {
            size_t idx = 0;
            bool ok = true;
            SkipWhitespace(json, idx);
            if (idx >= json.size()) return false;

            JsonValue parsed = ParseValue(json, idx, 0, ok);
            SkipWhitespace(json, idx);
            if (!ok || idx != json.size()) return false;

            out = std::move(parsed);
            return true;
        }

    private:
        static void SkipWhitespace(const std::wstring& s, size_t& idx) {
            while (idx < s.size() &&
                   (s[idx] == L' ' || s[idx] == L'\t' || s[idx] == L'\r' || s[idx] == L'\n')) {
                ++idx;
            }
        }

        static JsonValue ParseValue(const std::wstring& s, size_t& idx, size_t depth, bool& ok) {
            SkipWhitespace(s, idx);
            if (!ok || idx >= s.size()) {
                ok = false;
                return {};
            }

            const wchar_t c = s[idx];
            if (c == L'{' || c == L'[') {
                if (depth >= kMaxDepth) {
                    ok = false;
                    return {};
                }
                return c == L'{' ? ParseObject(s, idx, depth + 1, ok)
                                  : ParseArray(s, idx, depth + 1, ok);
            }
            if (c == L'"') return ParseString(s, idx, ok);
            if (c == L't' || c == L'f') return ParseBool(s, idx, ok);
            if (c == L'n') return ParseNull(s, idx, ok);
            if (c == L'-' || (c >= L'0' && c <= L'9')) return ParseNumber(s, idx, ok);

            ok = false;
            return {};
        }

        static JsonValue ParseObject(const std::wstring& s, size_t& idx, size_t depth, bool& ok) {
            JsonValue val;
            val.type = JsonType::Object;
            ++idx; // skip '{'
            SkipWhitespace(s, idx);
            if (idx < s.size() && s[idx] == L'}') {
                ++idx;
                return val;
            }

            while (ok) {
                if (idx >= s.size() || s[idx] != L'"') {
                    ok = false;
                    return {};
                }

                JsonValue key = ParseString(s, idx, ok);
                SkipWhitespace(s, idx);
                if (!ok || idx >= s.size() || s[idx] != L':') {
                    ok = false;
                    return {};
                }
                ++idx;

                JsonValue child = ParseValue(s, idx, depth, ok);
                if (!ok) return {};
                val.objVal[key.strVal] = std::move(child);

                SkipWhitespace(s, idx);
                if (idx >= s.size()) {
                    ok = false;
                    return {};
                }
                if (s[idx] == L'}') {
                    ++idx;
                    return val;
                }
                if (s[idx] != L',') {
                    ok = false;
                    return {};
                }
                ++idx;
                SkipWhitespace(s, idx);
                if (idx >= s.size() || s[idx] == L'}') {
                    ok = false; // JSON 不允许尾随逗号
                    return {};
                }
            }
            return {};
        }

        static JsonValue ParseArray(const std::wstring& s, size_t& idx, size_t depth, bool& ok) {
            JsonValue val;
            val.type = JsonType::Array;
            ++idx; // skip '['
            SkipWhitespace(s, idx);
            if (idx < s.size() && s[idx] == L']') {
                ++idx;
                return val;
            }

            while (ok) {
                JsonValue child = ParseValue(s, idx, depth, ok);
                if (!ok) return {};
                val.arrVal.push_back(std::move(child));

                SkipWhitespace(s, idx);
                if (idx >= s.size()) {
                    ok = false;
                    return {};
                }
                if (s[idx] == L']') {
                    ++idx;
                    return val;
                }
                if (s[idx] != L',') {
                    ok = false;
                    return {};
                }
                ++idx;
                SkipWhitespace(s, idx);
                if (idx >= s.size() || s[idx] == L']') {
                    ok = false;
                    return {};
                }
            }
            return {};
        }

        static int HexDigitValue(wchar_t ch) {
            if (ch >= L'0' && ch <= L'9') return ch - L'0';
            if (ch >= L'a' && ch <= L'f') return ch - L'a' + 10;
            if (ch >= L'A' && ch <= L'F') return ch - L'A' + 10;
            return -1;
        }

        static bool ReadHex4(const std::wstring& s, size_t idx, wchar_t& out) {
            if (idx + 4 > s.size()) return false;
            int value = 0;
            for (size_t i = 0; i < 4; ++i) {
                int digit = HexDigitValue(s[idx + i]);
                if (digit < 0) return false;
                value = value * 16 + digit;
            }
            out = static_cast<wchar_t>(value);
            return true;
        }

        static JsonValue ParseString(const std::wstring& s, size_t& idx, bool& ok) {
            JsonValue val;
            val.type = JsonType::String;
            ++idx; // skip opening quote

            while (idx < s.size()) {
                wchar_t c = s[idx++];
                if (c == L'"') {
                    return val;
                }
                if (c < 0x20) {
                    ok = false;
                    return {};
                }
                if (c != L'\\') {
                    val.strVal += c;
                    continue;
                }

                if (idx >= s.size()) {
                    ok = false;
                    return {};
                }
                wchar_t esc = s[idx++];
                switch (esc) {
                case L'"': val.strVal += L'"'; break;
                case L'\\': val.strVal += L'\\'; break;
                case L'/': val.strVal += L'/'; break;
                case L'b': val.strVal += L'\b'; break;
                case L'f': val.strVal += L'\f'; break;
                case L'n': val.strVal += L'\n'; break;
                case L'r': val.strVal += L'\r'; break;
                case L't': val.strVal += L'\t'; break;
                case L'u': {
                    wchar_t high = 0;
                    if (!ReadHex4(s, idx, high)) {
                        ok = false;
                        return {};
                    }
                    idx += 4;
                    if (high >= 0xD800 && high <= 0xDBFF) {
                        if (idx + 6 > s.size() || s[idx] != L'\\' || s[idx + 1] != L'u') {
                            ok = false;
                            return {};
                        }
                        wchar_t low = 0;
                        if (!ReadHex4(s, idx + 2, low) || low < 0xDC00 || low > 0xDFFF) {
                            ok = false;
                            return {};
                        }
                        idx += 6;
                        val.strVal += high;
                        val.strVal += low;
                    } else if (high >= 0xDC00 && high <= 0xDFFF) {
                        ok = false;
                        return {};
                    } else {
                        val.strVal += high;
                    }
                    break;
                }
                default:
                    ok = false;
                    return {};
                }
            }

            ok = false; // unterminated string
            return {};
        }

        static JsonValue ParseBool(const std::wstring& s, size_t& idx, bool& ok) {
            JsonValue val;
            val.type = JsonType::Boolean;
            if (idx + 4 <= s.size() && s.compare(idx, 4, L"true") == 0) {
                val.boolVal = true;
                idx += 4;
                return val;
            }
            if (idx + 5 <= s.size() && s.compare(idx, 5, L"false") == 0) {
                val.boolVal = false;
                idx += 5;
                return val;
            }
            ok = false;
            return {};
        }

        static JsonValue ParseNull(const std::wstring& s, size_t& idx, bool& ok) {
            if (idx + 4 <= s.size() && s.compare(idx, 4, L"null") == 0) {
                idx += 4;
                return {};
            }
            ok = false;
            return {};
        }

        static JsonValue ParseNumber(const std::wstring& s, size_t& idx, bool& ok) {
            const size_t start = idx;
            if (s[idx] == L'-') {
                ++idx;
                if (idx >= s.size()) {
                    ok = false;
                    return {};
                }
            }

            if (s[idx] == L'0') {
                ++idx;
                if (idx < s.size() && s[idx] >= L'0' && s[idx] <= L'9') {
                    ok = false;
                    return {};
                }
            } else if (s[idx] >= L'1' && s[idx] <= L'9') {
                while (idx < s.size() && s[idx] >= L'0' && s[idx] <= L'9') ++idx;
            } else {
                ok = false;
                return {};
            }

            if (idx < s.size() && s[idx] == L'.') {
                ++idx;
                const size_t fractionStart = idx;
                while (idx < s.size() && s[idx] >= L'0' && s[idx] <= L'9') ++idx;
                if (idx == fractionStart) {
                    ok = false;
                    return {};
                }
            }

            if (idx < s.size() && (s[idx] == L'e' || s[idx] == L'E')) {
                ++idx;
                if (idx < s.size() && (s[idx] == L'+' || s[idx] == L'-')) ++idx;
                const size_t exponentStart = idx;
                while (idx < s.size() && s[idx] >= L'0' && s[idx] <= L'9') ++idx;
                if (idx == exponentStart) {
                    ok = false;
                    return {};
                }
            }

            JsonValue val;
            val.type = JsonType::Number;
            try {
                size_t consumed = 0;
                const std::wstring token = s.substr(start, idx - start);
                val.numVal = std::stod(token, &consumed);
                if (consumed != token.size() || !std::isfinite(val.numVal)) {
                    ok = false;
                    return {};
                }
                val.numberText = token;
            } catch (...) {
                ok = false;
                return {};
            }
            return val;
        }
    };

} // namespace CodexQuotaBar
