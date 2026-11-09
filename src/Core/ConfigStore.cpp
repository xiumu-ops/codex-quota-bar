#include "Core/ConfigStore.h"
#include "Core/Settings.h"
#include "Core/Logger.h"
#include "Core/SimpleJson.h"

#include <knownfolders.h>
#include <objbase.h>
#include <shlobj.h>

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>

namespace CodexQuotaBar {
namespace {

    constexpr std::streamoff kMaxConfigBytes = 64 * 1024;

    struct ConfigData {
        AppSettings settings;
        StoredState state;
        JsonValue userDocument;
    };

    enum class ReadStatus {
        Missing,
        Valid,
        Invalid
    };

    std::mutex g_configMutex;

    std::filesystem::path ModuleDirectory() {
        std::wstring modulePath(32768, L'\0');
        const DWORD length = GetModuleFileNameW(
            nullptr, modulePath.data(), static_cast<DWORD>(modulePath.size()));
        if (length == 0 || length >= modulePath.size()) return {};
        modulePath.resize(length);
        return std::filesystem::path(modulePath).parent_path();
    }

    std::filesystem::path InstalledDataDirectory() {
        const std::filesystem::path appDirectory = ModuleDirectory();
        if (appDirectory.empty()) return {};
        const std::filesystem::path installRoot = appDirectory.parent_path();
        if (_wcsicmp(appDirectory.filename().c_str(), L"app") != 0 ||
            _wcsicmp(installRoot.filename().c_str(), L"Codex-Quota-Bar") != 0) {
            return {};
        }
        return installRoot / L"data";
    }

    std::filesystem::path DefaultConfigPath() {
        const std::filesystem::path moduleDirectory = ModuleDirectory();
        return moduleDirectory.empty()
            ? std::filesystem::path(L"config-default.json")
            : moduleDirectory / L"config-default.json";
    }

    std::filesystem::path ConfigDirectory() {
        const std::filesystem::path installedData = InstalledDataDirectory();
        if (!installedData.empty()) return installedData;

        PWSTR localAppData = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData))) {
            return std::filesystem::current_path() / L"Codex-Quota-Bar" / L"data";
        }
        const std::filesystem::path directory =
            std::filesystem::path(localAppData) / L"Codex-Quota-Bar" / L"data";
        CoTaskMemFree(localAppData);
        return directory;
    }

    void AppendValidationError(std::wstring& errors, const std::wstring& message) {
        if (!errors.empty()) errors += L"\n";
        errors += L"- " + message;
    }

    ReadStatus ReadJson(const std::filesystem::path& path, JsonValue& root) {
        std::error_code error;
        const bool exists = std::filesystem::exists(path, error);
        if (error) return ReadStatus::Invalid;
        if (!exists) return ReadStatus::Missing;

        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) return ReadStatus::Invalid;
        file.seekg(0, std::ios::end);
        const std::streamoff size = file.tellg();
        if (size <= 0 || size > kMaxConfigBytes) return ReadStatus::Invalid;
        file.seekg(0, std::ios::beg);

        std::string utf8(static_cast<size_t>(size), '\0');
        if (!file.read(utf8.data(), static_cast<std::streamsize>(size)) ||
            file.peek() != std::char_traits<char>::eof() || file.bad()) {
            return ReadStatus::Invalid;
        }

        const int length = MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()),
            nullptr, 0);
        if (length <= 0) return ReadStatus::Invalid;
        std::wstring wide(static_cast<size_t>(length), L'\0');
        if (MultiByteToWideChar(
                CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()),
                wide.data(), length) != length) {
            return ReadStatus::Invalid;
        }
        return JsonParser::TryParse(wide, root) && root.is_object()
            ? ReadStatus::Valid
            : ReadStatus::Invalid;
    }

    void ReadAppearanceObject(
        const JsonValue& object,
        AppearanceSettings& appearance,
        std::wstring& errors)
    {
        if (!object.is_object()) {
            AppendValidationError(errors, L"Settings.Appearance 必须是对象");
            return;
        }

        if (object.has_key(L"Mode")) {
            if (!object[L"Mode"].is_string()) {
                AppendValidationError(errors, L"Settings.Appearance.Mode 必须是字符串");
            } else {
                const std::wstring mode = object[L"Mode"].as_string();
                if (mode == L"Default") appearance.mode = AppearanceMode::Default;
                else if (mode == L"Custom") appearance.mode = AppearanceMode::Custom;
                else AppendValidationError(
                    errors, L"Settings.Appearance.Mode 只能是 Default 或 Custom");
            }
        }

        if (object.has_key(L"FontFamily")) {
            if (!object[L"FontFamily"].is_string() ||
                !IsValidFontFamilyName(object[L"FontFamily"].as_string())) {
                AppendValidationError(
                    errors, L"Settings.Appearance.FontFamily 必须是 1 至 128 个有效字符");
            } else {
                appearance.fontFamily = object[L"FontFamily"].as_string();
            }
        }

        if (object.has_key(L"BackgroundTransparency")) {
            const JsonValue& transparency = object[L"BackgroundTransparency"];
            int parsedValue = 0;
            if (!transparency.is_number() ||
                !TryParseBackgroundTransparency(
                    transparency.as_double(-1.0), parsedValue)) {
                AppendValidationError(
                    errors,
                    L"Settings.Appearance.BackgroundTransparency 必须是 0 至 90 的整数");
            } else {
                appearance.backgroundTransparency = parsedValue;
            }
        }

        if (!object.has_key(L"Colors")) return;
        const JsonValue& colors = object[L"Colors"];
        if (!colors.is_object()) {
            AppendValidationError(errors, L"Settings.Appearance.Colors 必须是对象");
            return;
        }
        for (const auto& [name, value] : colors.objVal) {
            const std::wstring path = L"Settings.Appearance.Colors." + name;
            if (!IsSupportedAppearanceColor(name)) continue;
            uint32_t rgb = 0;
            if (!value.is_string() ||
                !TryParseAppearanceColor(value.as_string(), rgb)) {
                AppendValidationError(errors, path + L" 必须使用 #RRGGBB 格式");
                continue;
            }
            appearance.colors[name] = value.as_string();
        }
    }

    void ReadSettingsObject(
        const JsonValue& object,
        AppSettings& settings,
        std::wstring& errors)
    {
        if (!object.is_object()) return;

        if (object.has_key(L"UserScale")) {
            const JsonValue& userScale = object[L"UserScale"];
            const double value = userScale.as_double();
            if (userScale.is_number() && IsSupportedUserScale(value)) {
                settings.userScale = static_cast<float>(value);
            } else {
                AppendValidationError(
                    errors,
                    L"Settings.UserScale 只能是 0.2 至 1.8 且为 0.2 整数倍的数值（即 20% 至 180%）");
            }
        }
        if (object.has_key(L"CompanionMode")) {
            if (object[L"CompanionMode"].is_bool()) {
                settings.companionMode = object[L"CompanionMode"].as_bool(false);
            } else {
                AppendValidationError(errors, L"Settings.CompanionMode 必须是布尔值");
            }
        }
        if (object.has_key(L"AlwaysOnTop")) {
            if (object[L"AlwaysOnTop"].is_bool()) {
                settings.alwaysOnTop = object[L"AlwaysOnTop"].as_bool(true);
            } else {
                AppendValidationError(errors, L"Settings.AlwaysOnTop 必须是布尔值");
            }
        }
        if (object.has_key(L"MiniMode")) {
            if (object[L"MiniMode"].is_bool()) {
                settings.miniMode = object[L"MiniMode"].as_bool(false);
            } else {
                AppendValidationError(errors, L"Settings.MiniMode 必须是布尔值");
            }
        }
        if (object.has_key(L"DockToWeather")) {
            if (object[L"DockToWeather"].is_bool()) {
                settings.dockToWeather = object[L"DockToWeather"].as_bool(false);
            } else {
                AppendValidationError(errors, L"Settings.DockToWeather 必须是布尔值");
            }
        }
        if (object.has_key(L"RefreshIntervalMinutes")) {
            int value = 0;
            const JsonValue& interval = object[L"RefreshIntervalMinutes"];
            if (interval.is_number() &&
                TryParseSupportedRefreshInterval(interval.as_double(), value)) {
                settings.refreshIntervalMinutes = value;
            } else {
                AppendValidationError(
                    errors,
                    L"Settings.RefreshIntervalMinutes 只能是 1、5、10、30 或 60");
            }
        }
        if (object.has_key(L"Appearance")) {
            ReadAppearanceObject(object[L"Appearance"], settings.appearance, errors);
        }
    }

    bool HasConfigVersion2(const JsonValue& root) {
        int version = 0;
        return root.has_key(L"Version") && root[L"Version"].is_number() &&
               TryParseConfigInteger(root[L"Version"].as_double(), version) &&
               version == 2;
    }

    void ValidateDefaultConfigShape(const JsonValue& root, std::wstring& errors) {
        const JsonValue& settings = root[L"Settings"];
        const JsonValue& appearance = settings[L"Appearance"];
        if (!HasConfigVersion2(root) || !root.has_key(L"Window") ||
            !root[L"Window"].is_null()) {
            AppendValidationError(
                errors,
                L"config-default.json 必须使用完整的版本 2 配置结构，且 Window 必须为 null");
        }
        if (!settings.is_object() || !appearance.is_object()) {
            AppendValidationError(
                errors, L"config-default.json 必须包含 Settings.Appearance 对象");
            return;
        }
        if (!settings.has_key(L"UserScale") ||
            !settings.has_key(L"CompanionMode") ||
            !settings.has_key(L"AlwaysOnTop") ||
            !settings.has_key(L"MiniMode") ||
            !settings.has_key(L"RefreshIntervalMinutes")) {
            AppendValidationError(
                errors, L"config-default.json 缺少完整的通用设置基线");
        }
        if (!appearance.has_key(L"Mode") ||
            !appearance[L"Mode"].is_string() ||
            appearance[L"Mode"].as_string() != L"Default") {
            AppendValidationError(
                errors, L"config-default.json 的 Appearance.Mode 必须是 Default");
        }
        if (!appearance.has_key(L"FontFamily") ||
            !appearance.has_key(L"BackgroundTransparency") ||
            !appearance.has_key(L"Colors") ||
            !appearance[L"Colors"].is_object()) {
            AppendValidationError(
                errors, L"config-default.json 必须包含完整的字体、透明度和颜色配置");
            return;
        }
        const JsonValue& colors = appearance[L"Colors"];
        for (const auto& [name, unused] : DefaultAppearanceColors()) {
            static_cast<void>(unused);
            if (!colors.has_key(name)) {
                AppendValidationError(
                    errors, L"config-default.json 缺少默认颜色 " + name);
            }
        }
    }

    void ValidateUserConfigShape(const JsonValue& root, std::wstring& errors) {
        if (!HasConfigVersion2(root) ||
            !root.has_key(L"Settings") || !root[L"Settings"].is_object()) {
            AppendValidationError(
                errors, L"config-users.json 必须使用版本 2 配置结构");
        }
    }

    void ReadWindowObject(
        const JsonValue& object,
        StoredState& state,
        std::wstring& errors)
    {
        if (object.is_null()) return;
        if (!object.is_object()) {
            AppendValidationError(errors, L"Window 必须是包含 X、Y 的对象或 null");
            return;
        }
        int x = 0;
        int y = 0;
        if (!object.has_key(L"X") || !object[L"X"].is_number() ||
            !TryParseConfigInteger(object[L"X"].as_double(), x) ||
            !object.has_key(L"Y") || !object[L"Y"].is_number() ||
            !TryParseConfigInteger(object[L"Y"].as_double(), y)) {
            AppendValidationError(errors, L"Window.X 和 Window.Y 必须是 32 位整数");
            return;
        }
        state.position.x = x;
        state.position.y = y;
        state.hasPosition = true;
    }

    bool WideToUtf8(const std::wstring& value, std::string& utf8) {
        if (value.empty()) {
            utf8.clear();
            return true;
        }
        if (value.size() > static_cast<size_t>((std::numeric_limits<int>::max)())) {
            return false;
        }
        const int length = WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
            nullptr, 0, nullptr, nullptr);
        if (length <= 0) return false;
        utf8.resize(static_cast<size_t>(length));
        return WideCharToMultiByte(
                   CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                   utf8.data(), length, nullptr, nullptr) == length;
    }

    bool JsonString(const std::wstring& value, std::string& quoted) {
        std::wstring escaped;
        escaped.reserve(value.size() + 2);
        escaped += L'"';
        constexpr wchar_t hex[] = L"0123456789ABCDEF";
        for (const wchar_t ch : value) {
            switch (ch) {
            case L'"': escaped += L"\\\""; break;
            case L'\\': escaped += L"\\\\"; break;
            case L'\b': escaped += L"\\b"; break;
            case L'\f': escaped += L"\\f"; break;
            case L'\n': escaped += L"\\n"; break;
            case L'\r': escaped += L"\\r"; break;
            case L'\t': escaped += L"\\t"; break;
            default:
                if (ch < 0x20) {
                    escaped += L"\\u00";
                    escaped += hex[(ch >> 4) & 0xF];
                    escaped += hex[ch & 0xF];
                } else {
                    escaped += ch;
                }
                break;
            }
        }
        escaped += L'"';
        return WideToUtf8(escaped, quoted);
    }

    bool ValidateAppearanceSettings(
        const AppearanceSettings& appearance,
        std::wstring& errors)
    {
        if (!IsValidFontFamilyName(appearance.fontFamily)) {
            AppendValidationError(
                errors, L"Settings.Appearance.FontFamily 必须是 1 至 128 个有效字符");
        }
        if (!IsValidBackgroundTransparency(appearance.backgroundTransparency)) {
            AppendValidationError(
                errors,
                L"Settings.Appearance.BackgroundTransparency 必须是 0 至 90 的整数");
        }
        for (const auto& [name, value] : appearance.colors) {
            const std::wstring path = L"Settings.Appearance.Colors." + name;
            uint32_t rgb = 0;
            if (!IsSupportedAppearanceColor(name)) {
                AppendValidationError(errors, path + L" 不是支持的颜色字段");
            } else if (!TryParseAppearanceColor(value, rgb)) {
                AppendValidationError(errors, path + L" 必须使用 #RRGGBB 格式");
            }
        }
        return errors.empty();
    }

    bool ValidateAppSettings(const AppSettings& settings, std::wstring& errors) {
        if (!IsSupportedUserScale(settings.userScale)) {
            AppendValidationError(
                errors,
                L"Settings.UserScale 只能是 0.2 至 1.8 且为 0.2 整数倍的数值（即 20% 至 180%）");
        }
        int refreshInterval = 0;
        if (!TryParseSupportedRefreshInterval(
                static_cast<double>(settings.refreshIntervalMinutes),
                refreshInterval)) {
            AppendValidationError(
                errors,
                L"Settings.RefreshIntervalMinutes 只能是 1、5、10、30 或 60");
        }
        ValidateAppearanceSettings(settings.appearance, errors);
        return errors.empty();
    }

    JsonValue& ObjectMember(JsonValue& object, const wchar_t* name) {
        object.type = JsonType::Object;
        return object.objVal[name];
    }

    JsonValue Number(double number) {
        JsonValue value;
        value.type = JsonType::Number;
        value.numVal = number;
        return value;
    }

    JsonValue Boolean(bool flag) {
        JsonValue value;
        value.type = JsonType::Boolean;
        value.boolVal = flag;
        return value;
    }

    JsonValue String(const std::wstring& text) {
        JsonValue value;
        value.type = JsonType::String;
        value.strVal = text;
        return value;
    }

    void UpdateUserDocument(ConfigData& data) {
        auto& root = data.userDocument;
        ObjectMember(root, L"Version") = Number(2);
        auto& settings = ObjectMember(root, L"Settings");
        ObjectMember(settings, L"UserScale") = Number(std::round(data.settings.userScale * 5.0) / 5.0);
        ObjectMember(settings, L"CompanionMode") = Boolean(data.settings.companionMode);
        ObjectMember(settings, L"AlwaysOnTop") = Boolean(data.settings.alwaysOnTop);
        ObjectMember(settings, L"MiniMode") = Boolean(data.settings.miniMode);
        ObjectMember(settings, L"DockToWeather") = Boolean(data.settings.dockToWeather);
        ObjectMember(settings, L"RefreshIntervalMinutes") = Number(data.settings.refreshIntervalMinutes);
        auto& appearance = ObjectMember(settings, L"Appearance");
        const auto& current = data.settings.appearance;
        ObjectMember(appearance, L"Mode") = String(
            current.mode == AppearanceMode::Custom ? L"Custom" : L"Default");
        ObjectMember(appearance, L"FontFamily") = String(current.fontFamily);
        ObjectMember(appearance, L"BackgroundTransparency") = Number(current.backgroundTransparency);
        auto& colors = ObjectMember(appearance, L"Colors");
        colors.type = JsonType::Object;
        for (const auto& [name, color] : current.colors) {
            colors.objVal[name] = String(color);
        }

        auto& window = ObjectMember(root, L"Window");
        if (data.state.hasPosition) {
            ObjectMember(window, L"X") = Number(data.state.position.x);
            ObjectMember(window, L"Y") = Number(data.state.position.y);
        } else {
            window = {};
        }
    }

    bool WriteJsonValue(std::ostream& stream, const JsonValue& value, size_t depth = 0) {
        switch (value.type) {
        case JsonType::Null: stream << "null"; break;
        case JsonType::Boolean: stream << (value.boolVal ? "true" : "false"); break;
        case JsonType::Number:
            if (value.numberToken.empty()) stream << value.numVal;
            else stream << value.numberToken;
            break;
        case JsonType::String: {
            std::string quoted;
            if (!JsonString(value.strVal, quoted)) return false;
            stream << quoted;
            break;
        }
        case JsonType::Array: {
            stream << '[';
            bool first = true;
            for (const auto& item : value.arrVal) {
                if (!first) stream << ", ";
                if (!WriteJsonValue(stream, item, depth + 1)) return false;
                first = false;
            }
            stream << ']';
            break;
        }
        case JsonType::Object: {
            stream << '{';
            bool first = true;
            for (const auto& [key, item] : value.objVal) {
                std::string quoted;
                if (!JsonString(key, quoted)) return false;
                stream << (first ? "\n" : ",\n") << std::string((depth + 1) * 2, ' ')
                       << quoted << ": ";
                if (!WriteJsonValue(stream, item, depth + 1)) return false;
                first = false;
            }
            if (!first) stream << '\n' << std::string(depth * 2, ' ');
            stream << '}';
            break;
        }
        }
        return stream.good();
    }

    bool WriteConfig(const std::filesystem::path& path, ConfigData& data) {
        UpdateUserDocument(data);
        std::ostringstream stream;
        stream.imbue(std::locale::classic());
        stream << std::setprecision(std::numeric_limits<double>::max_digits10);
        if (!WriteJsonValue(stream, data.userDocument)) return false;
        stream << '\n';
        const auto json = stream.str();
        if (json.size() > static_cast<size_t>(kMaxConfigBytes)) return false;

        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) return false;

        GUID id = {};
        wchar_t suffix[40] = {};
        if (FAILED(CoCreateGuid(&id)) || StringFromGUID2(id, suffix, _countof(suffix)) == 0) return false;
        const std::wstring temporary = path.wstring() + L".tmp-" + suffix;
        const std::wstring backup = temporary + L".bak";
        const HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
                                        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        DWORD written = 0;
        const auto length = static_cast<DWORD>(json.size());
        const bool flushed = WriteFile(file, json.data(), length, &written, nullptr) &&
                             written == length && FlushFileBuffers(file);
        CloseHandle(file);

        if (flushed) {
            for (DWORD attempt = 0; attempt < 6; ++attempt) {
                if (ReplaceFileW(path.c_str(), temporary.c_str(), backup.c_str(), 0, nullptr, nullptr)) {
                    DeleteFileW(backup.c_str());
                    return true;
                }
                DWORD status = GetLastError();
                if (status == ERROR_UNABLE_TO_MOVE_REPLACEMENT_2) {
                    // ReplaceFile may have moved the original to backup already.
                    if (!MoveFileExW(backup.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH)) {
                        WriteLog(LogLevel::Error, L"配置替换失败；原文件保留在 " + backup);
                    }
                    break;
                }
                if (status == ERROR_FILE_NOT_FOUND) {
                    if (MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH)) return true;
                    status = GetLastError();
                }
                if (status != ERROR_SHARING_VIOLATION && status != ERROR_LOCK_VIOLATION &&
                    status != ERROR_ACCESS_DENIED && status != ERROR_ALREADY_EXISTS &&
                    status != ERROR_FILE_EXISTS && status != ERROR_UNABLE_TO_REMOVE_REPLACED) break;
                if (attempt < 5) Sleep(15U * (attempt + 1));
            }
        }
        DeleteFileW(temporary.c_str());
        return false;
    }

    ConfigData LoadData(
        ReadStatus* readStatus = nullptr,
        std::wstring* validationErrors = nullptr,
        bool includeDefaultWarnings = true)
    {
        if (validationErrors) validationErrors->clear();
        ConfigData data;
        std::wstring errors;

        JsonValue defaultRoot;
        std::wstring defaultErrors;
        if (ReadJson(DefaultConfigPath(), defaultRoot) == ReadStatus::Valid) {
            AppSettings baseline;
            ValidateDefaultConfigShape(defaultRoot, defaultErrors);
            ReadSettingsObject(defaultRoot[L"Settings"], baseline, defaultErrors);
            if (defaultErrors.empty()) data.settings = std::move(baseline);
        } else {
            AppendValidationError(defaultErrors, L"无法读取 config-default.json，已使用内置默认设置");
        }
        data.settings.defaultAppearance = data.settings.appearance;

        const std::filesystem::path directory = ConfigDirectory();
        const std::filesystem::path userConfigPath = directory / L"config-users.json";

        JsonValue root;
        const ReadStatus userStatus = ReadJson(userConfigPath, root);

        if (userStatus == ReadStatus::Valid) {
            ValidateUserConfigShape(root, errors);
            if (errors.empty()) {
                ReadSettingsObject(root[L"Settings"], data.settings, errors);
                ReadWindowObject(root[L"Window"], data.state, errors);
                data.userDocument = std::move(root);
            }
        } else if (userStatus == ReadStatus::Invalid) {
            AppendValidationError(errors, L"config-users.json 不是有效的 UTF-8 JSON，或文件不可读取");
        }
        if (readStatus) *readStatus = errors.empty() ? userStatus : ReadStatus::Invalid;
        if (validationErrors) {
            *validationErrors = errors;
        }
        if (includeDefaultWarnings && !defaultErrors.empty()) WriteLog(LogLevel::Warning, defaultErrors);
        return data;
    }

} // namespace

    AppSettings ConfigStore::LoadSettings(
        std::wstring* validationError,
        bool* configReadable)
    {
        const std::lock_guard<std::mutex> lock(g_configMutex);
        ReadStatus status = ReadStatus::Missing;
        AppSettings settings = LoadData(&status, validationError).settings;
        if (configReadable) *configReadable = status != ReadStatus::Invalid;
        return settings;
    }

    StoredState ConfigStore::LoadState() {
        const std::lock_guard<std::mutex> lock(g_configMutex);
        return LoadData().state;
    }

    bool ConfigStore::SaveSettings(
        const AppSettings& settings,
        std::wstring* validationError)
    {
        const std::lock_guard<std::mutex> lock(g_configMutex);
        if (validationError) validationError->clear();
        ReadStatus status = ReadStatus::Missing;
        std::wstring errors;
        ConfigData data = LoadData(&status, &errors, false);
        ValidateAppSettings(settings, errors);
        if (status == ReadStatus::Invalid || !errors.empty()) {
            if (validationError) *validationError = errors;
            return false;
        }
        data.settings = settings;
        const bool saved = WriteConfig(ConfigDirectory() / L"config-users.json", data);
        if (!saved && validationError) {
            *validationError = L"- config-users.json 写入失败";
        }
        return saved;
    }

    void ConfigStore::SavePosition(POINT position) {
        const std::lock_guard<std::mutex> lock(g_configMutex);
        ReadStatus status = ReadStatus::Missing;
        std::wstring errors;
        ConfigData data = LoadData(&status, &errors, false);
        if (status == ReadStatus::Invalid || !errors.empty()) return;
        if (data.state.hasPosition && data.state.position.x == position.x &&
            data.state.position.y == position.y) return;
        data.state.position = position;
        data.state.hasPosition = true;
        if (!WriteConfig(ConfigDirectory() / L"config-users.json", data)) {
            WriteLog(LogLevel::Warning, L"窗口位置未保存：config-users.json 写入失败。");
        }
    }

    std::wstring ConfigStore::DataDirectory() {
        return ConfigDirectory().wstring();
    }

    std::wstring ConfigStore::ConfigFilePath() {
        return (ConfigDirectory() / L"config-users.json").wstring();
    }

    std::wstring ConfigStore::DefaultConfigFilePath() {
        return DefaultConfigPath().wstring();
    }

} // namespace CodexQuotaBar
