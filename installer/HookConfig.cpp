#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "HookConfig.h"
#include "Core/SimpleJson.h"

#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>

namespace CodexQuotaBarInstaller::HookConfig {
namespace {

using CodexQuotaBar::JsonParser;
using CodexQuotaBar::JsonType;
using CodexQuotaBar::JsonValue;

constexpr std::uintmax_t kMaxHookFileBytes = 4 * 1024 * 1024;
constexpr wchar_t kSessionStart[] = L"SessionStart";
constexpr wchar_t kStop[] = L"Stop";
constexpr wchar_t kSessionEnd[] = L"SessionEnd";

std::wstring ReadEnvironment(const wchar_t* name) {
    const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
    if (required == 0) return {};
    std::wstring value(required, L'\0');
    const DWORD written = GetEnvironmentVariableW(
        name, value.data(), static_cast<DWORD>(value.size()));
    if (written == 0 || written >= value.size()) return {};
    value.resize(written);
    return value;
}

std::wstring ProfileDirectory() {
    PWSTR value = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &value))) return {};
    std::wstring result(value);
    CoTaskMemFree(value);
    return result;
}

bool Utf8ToWide(std::string content, std::wstring& value) {
    if (content.size() >= 3 &&
        static_cast<unsigned char>(content[0]) == 0xEF &&
        static_cast<unsigned char>(content[1]) == 0xBB &&
        static_cast<unsigned char>(content[2]) == 0xBF) {
        content.erase(0, 3);
    }
    if (content.empty()) {
        value.clear();
        return true;
    }
    const int required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, content.data(), static_cast<int>(content.size()),
        nullptr, 0);
    if (required <= 0) return false;
    value.resize(static_cast<size_t>(required));
    return MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, content.data(), static_cast<int>(content.size()),
        value.data(), required) == required;
}

bool WideToUtf8(const std::wstring& value, std::string& content) {
    if (value.empty()) {
        content.clear();
        return true;
    }
    const int required = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    if (required <= 0) return false;
    content.resize(static_cast<size_t>(required));
    return WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        content.data(), required, nullptr, nullptr) == required;
}

bool ReadFile(const std::filesystem::path& path, std::string& content,
              bool& exists, std::wstring& error) {
    std::error_code fsError;
    exists = std::filesystem::exists(path, fsError);
    if (fsError) {
        error = L"无法检查 Codex hooks.json。";
        return false;
    }
    if (!exists) {
        content.clear();
        return true;
    }
    if (!std::filesystem::is_regular_file(path, fsError) || fsError) {
        error = L"Codex hooks.json 不是普通文件。";
        return false;
    }
    const std::uintmax_t size = std::filesystem::file_size(path, fsError);
    if (fsError || size > kMaxHookFileBytes) {
        error = L"Codex hooks.json 过大或无法读取。";
        return false;
    }

    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        error = L"无法打开 Codex hooks.json。";
        return false;
    }
    std::ostringstream stream;
    stream << input.rdbuf();
    content = stream.str();
    if (!input.good() && !input.eof()) {
        error = L"读取 Codex hooks.json 失败。";
        return false;
    }
    return true;
}

void Indent(std::wostringstream& output, size_t depth) {
    for (size_t i = 0; i < depth * 2; ++i) output << L' ';
}

void WriteJsonString(std::wostringstream& output, const std::wstring& value) {
    output << L'"';
    for (const wchar_t ch : value) {
        switch (ch) {
        case L'"': output << L"\\\""; break;
        case L'\\': output << L"\\\\"; break;
        case L'\b': output << L"\\b"; break;
        case L'\f': output << L"\\f"; break;
        case L'\n': output << L"\\n"; break;
        case L'\r': output << L"\\r"; break;
        case L'\t': output << L"\\t"; break;
        default:
            if (ch < 0x20) {
                output << L"\\u" << std::uppercase << std::hex
                       << std::setw(4) << std::setfill(L'0')
                       << static_cast<unsigned>(ch)
                       << std::nouppercase << std::dec << std::setfill(L' ');
            } else {
                output << ch;
            }
            break;
        }
    }
    output << L'"';
}

void WriteJson(std::wostringstream& output, const JsonValue& value, size_t depth) {
    switch (value.type) {
    case JsonType::Null:
        output << L"null";
        break;
    case JsonType::Boolean:
        output << (value.boolVal ? L"true" : L"false");
        break;
    case JsonType::Number:
        if (!value.numberText.empty()) {
            output << value.numberText;
        } else {
            output << std::setprecision(17) << value.numVal;
        }
        break;
    case JsonType::String:
        WriteJsonString(output, value.strVal);
        break;
    case JsonType::Array:
        if (value.arrVal.empty()) {
            output << L"[]";
            break;
        }
        output << L"[\n";
        for (size_t i = 0; i < value.arrVal.size(); ++i) {
            Indent(output, depth + 1);
            WriteJson(output, value.arrVal[i], depth + 1);
            if (i + 1 < value.arrVal.size()) output << L',';
            output << L'\n';
        }
        Indent(output, depth);
        output << L']';
        break;
    case JsonType::Object:
        if (value.objVal.empty()) {
            output << L"{}";
            break;
        }
        output << L"{\n";
        for (auto it = value.objVal.begin(); it != value.objVal.end(); ++it) {
            Indent(output, depth + 1);
            WriteJsonString(output, it->first);
            output << L": ";
            WriteJson(output, it->second, depth + 1);
            if (std::next(it) != value.objVal.end()) output << L',';
            output << L'\n';
        }
        Indent(output, depth);
        output << L'}';
        break;
    }
}

bool Serialize(const JsonValue& root, std::string& content, std::wstring& error) {
    std::wostringstream output;
    WriteJson(output, root, 0);
    output << L'\n';
    if (!WideToUtf8(output.str(), content)) {
        error = L"Codex hooks.json 包含无法编码的字符。";
        return false;
    }
    return true;
}

bool WriteAtomically(const std::filesystem::path& path, const std::string& content,
                     std::wstring& error) {
    std::error_code fsError;
    std::filesystem::create_directories(path.parent_path(), fsError);
    if (fsError) {
        error = L"无法创建 Codex Hook 目录。";
        return false;
    }

    const std::filesystem::path temporary =
        path.wstring() + L".codex-quota-bar." +
        std::to_wstring(GetCurrentProcessId()) + L".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output.is_open()) {
            error = L"无法创建 Codex hooks.json 临时文件。";
            return false;
        }
        output.write(content.data(), static_cast<std::streamsize>(content.size()));
        output.flush();
        if (!output.good()) {
            output.close();
            DeleteFileW(temporary.c_str());
            error = L"写入 Codex hooks.json 临时文件失败。";
            return false;
        }
    }

    if (!MoveFileExW(temporary.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        error = L"原子替换 Codex hooks.json 失败。";
        return false;
    }
    return true;
}

JsonValue Object() {
    JsonValue value;
    value.type = JsonType::Object;
    return value;
}

JsonValue Array() {
    JsonValue value;
    value.type = JsonType::Array;
    return value;
}

JsonValue String(const std::wstring& text) {
    JsonValue value;
    value.type = JsonType::String;
    value.strVal = text;
    return value;
}

JsonValue Number(double number) {
    JsonValue value;
    value.type = JsonType::Number;
    value.numVal = number;
    return value;
}

JsonValue Boolean(bool boolean) {
    JsonValue value;
    value.type = JsonType::Boolean;
    value.boolVal = boolean;
    return value;
}

std::wstring Lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return value;
}

bool IsManagedHandler(const JsonValue& handler, const std::wstring& eventName) {
    if (!handler.is_object()) return false;
    const std::wstring eventToken = L"--hook " + Lower(eventName);
    for (const wchar_t* key : { L"command", L"commandWindows" }) {
        const auto found = handler.objVal.find(key);
        if (found == handler.objVal.end() || !found->second.is_string()) continue;
        const std::wstring command = Lower(found->second.strVal);
        if (command.find(L"codex-quota-bar.exe") != std::wstring::npos &&
            command.find(eventToken) != std::wstring::npos) {
            return true;
        }
    }
    return false;
}

bool RemoveManagedEvent(JsonValue& hooks, const std::wstring& eventName,
                        bool& changed, std::wstring& error) {
    auto event = hooks.objVal.find(eventName);
    if (event == hooks.objVal.end()) return true;
    if (!event->second.is_array()) {
        error = L"Codex hooks.json 中的 " + eventName + L" 不是数组；已拒绝修改。";
        return false;
    }

    std::vector<JsonValue> retainedGroups;
    retainedGroups.reserve(event->second.arrVal.size());
    for (JsonValue group : event->second.arrVal) {
        if (!group.is_object()) {
            error = L"Codex hooks.json 中的 " + eventName + L" 组格式无效；已拒绝修改。";
            return false;
        }
        auto handlers = group.objVal.find(L"hooks");
        if (handlers == group.objVal.end()) {
            retainedGroups.push_back(std::move(group));
            continue;
        }
        if (!handlers->second.is_array()) {
            error = L"Codex hooks.json 中的 " + eventName + L" handlers 不是数组；已拒绝修改。";
            return false;
        }

        std::vector<JsonValue> retainedHandlers;
        retainedHandlers.reserve(handlers->second.arrVal.size());
        bool removedFromGroup = false;
        for (JsonValue handler : handlers->second.arrVal) {
            if (IsManagedHandler(handler, eventName)) {
                removedFromGroup = true;
                changed = true;
            } else {
                retainedHandlers.push_back(std::move(handler));
            }
        }
        handlers->second.arrVal = std::move(retainedHandlers);
        if (!removedFromGroup || !handlers->second.arrVal.empty()) {
            retainedGroups.push_back(std::move(group));
        }
    }

    event->second.arrVal = std::move(retainedGroups);
    if (event->second.arrVal.empty()) hooks.objVal.erase(event);
    return true;
}

bool RemoveAllManagedHooks(JsonValue& root, bool& changed, std::wstring& error) {
    changed = false;
    auto hooks = root.objVal.find(L"hooks");
    if (hooks == root.objVal.end()) return true;
    if (!hooks->second.is_object()) {
        error = L"Codex hooks.json 的 hooks 字段不是对象；已拒绝修改。";
        return false;
    }
    return RemoveManagedEvent(hooks->second, kSessionStart, changed, error) &&
           RemoveManagedEvent(hooks->second, kStop, changed, error) &&
           RemoveManagedEvent(hooks->second, kSessionEnd, changed, error);
}

JsonValue BuildGroup(const std::filesystem::path& appPath,
                     const std::wstring& eventName, bool sessionStart) {
    const std::wstring command =
        L"\"" + appPath.wstring() + L"\" --hook " + eventName;

    JsonValue handler = Object();
    handler.objVal[L"type"] = String(L"command");
    handler.objVal[L"command"] = String(command);
    handler.objVal[L"commandWindows"] = String(command);
    handler.objVal[L"timeout"] = Number(3);
    if (eventName != kSessionEnd) handler.objVal[L"async"] = Boolean(true);

    JsonValue handlers = Array();
    handlers.arrVal.push_back(std::move(handler));

    JsonValue group = Object();
    if (sessionStart) {
        group.objVal[L"matcher"] = String(L"^(startup|resume|clear|compact)$");
    }
    group.objVal[L"hooks"] = std::move(handlers);
    return group;
}

bool ParseRoot(const std::string& content, bool exists,
               JsonValue& root, std::wstring& error) {
    if (!exists) {
        root = Object();
        root.objVal[L"hooks"] = Object();
        return true;
    }
    std::wstring wide;
    if (!Utf8ToWide(content, wide) || !JsonParser::TryParse(wide, root) ||
        !root.is_object()) {
        error = L"Codex hooks.json 不是有效的 UTF-8 JSON 对象；已拒绝修改。";
        return false;
    }
    const auto hooks = root.objVal.find(L"hooks");
    if (hooks != root.objVal.end() && !hooks->second.is_object()) {
        error = L"Codex hooks.json 的 hooks 字段不是对象；已拒绝修改。";
        return false;
    }
    if (hooks == root.objVal.end()) root.objVal[L"hooks"] = Object();
    return true;
}

bool IsOnlyEmptyHooksObject(const JsonValue& root) {
    if (!root.is_object() || root.objVal.size() != 1) return false;
    const auto hooks = root.objVal.find(L"hooks");
    return hooks != root.objVal.end() && hooks->second.is_object() &&
           hooks->second.objVal.empty();
}

} // namespace

std::filesystem::path ResolveHookFilePath() {
    std::wstring codexHome = ReadEnvironment(L"CODEX_HOME");
    if (codexHome.empty()) {
        const std::wstring profile = ProfileDirectory();
        if (profile.empty()) return {};
        codexHome = (std::filesystem::path(profile) / L".codex").wstring();
    }
    std::error_code error;
    const std::filesystem::path absolute = std::filesystem::absolute(codexHome, error);
    return error ? std::filesystem::path{} : absolute / L"hooks.json";
}

Result Install(const std::filesystem::path& hookFilePath,
               const std::filesystem::path& appPath) {
    Result result;
    if (hookFilePath.empty() || appPath.empty()) {
        result.error = L"无法定位 Codex hooks.json 或应用路径。";
        return result;
    }

    std::string content;
    bool exists = false;
    if (!ReadFile(hookFilePath, content, exists, result.error)) return result;

    JsonValue root;
    if (!ParseRoot(content, exists, root, result.error)) return result;

    bool removedOld = false;
    if (!RemoveAllManagedHooks(root, removedOld, result.error)) return result;

    JsonValue& hooks = root.objVal[L"hooks"];
    hooks.objVal[kSessionStart].type = JsonType::Array;
    hooks.objVal[kSessionStart].arrVal.push_back(
        BuildGroup(appPath, kSessionStart, true));
    hooks.objVal[kStop].type = JsonType::Array;
    hooks.objVal[kStop].arrVal.push_back(BuildGroup(appPath, kStop, false));
    hooks.objVal[kSessionEnd].type = JsonType::Array;
    hooks.objVal[kSessionEnd].arrVal.push_back(
        BuildGroup(appPath, kSessionEnd, false));

    if (!Serialize(root, content, result.error) ||
        !WriteAtomically(hookFilePath, content, result.error)) {
        return result;
    }
    result.success = true;
    result.changed = true;
    return result;
}

Result Remove(const std::filesystem::path& hookFilePath) {
    Result result;
    if (hookFilePath.empty()) {
        result.error = L"无法定位 Codex hooks.json。";
        return result;
    }

    std::string content;
    bool exists = false;
    if (!ReadFile(hookFilePath, content, exists, result.error)) return result;
    if (!exists) {
        result.success = true;
        return result;
    }

    JsonValue root;
    if (!ParseRoot(content, true, root, result.error)) return result;

    bool changed = false;
    if (!RemoveAllManagedHooks(root, changed, result.error)) return result;
    if (changed) {
        if (IsOnlyEmptyHooksObject(root)) {
            if (!DeleteFileW(hookFilePath.c_str())) {
                result.error = L"无法删除已清空的 Codex hooks.json。";
                return result;
            }
        } else if (!Serialize(root, content, result.error) ||
                   !WriteAtomically(hookFilePath, content, result.error)) {
            return result;
        }
    }

    result.success = true;
    result.changed = changed;
    return result;
}

} // namespace CodexQuotaBarInstaller::HookConfig
