#include "Core/ConfigStore.h"
#include "Core/SimpleJson.h"

#include <knownfolders.h>
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

namespace CodexQuotaBar {
namespace {

    constexpr std::streamoff kMaxConfigBytes = 64 * 1024;

    struct ConfigData {
        AppSettings settings;
        StoredState state;
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

        std::stringstream stream;
        stream << file.rdbuf();
        const std::string utf8 = stream.str();
        if (utf8.empty() || utf8.size() > static_cast<size_t>((std::numeric_limits<int>::max)())) {
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
        if (object.is_null()) return;
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
            if (!IsSupportedAppearanceColor(name)) {
                AppendValidationError(errors, path + L" 不是支持的颜色字段");
                continue;
            }
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
        if (object.has_key(L"UserScale") && object[L"UserScale"].is_number()) {
            const double value = object[L"UserScale"].as_double(1.0);
            if (value > 0.0 && value <= 10.0) {
                settings.userScale = static_cast<float>(value);
            }
        }
        if (object.has_key(L"CompanionMode") && object[L"CompanionMode"].is_bool()) {
            settings.companionMode = object[L"CompanionMode"].as_bool(false);
        }
        if (object.has_key(L"RefreshIntervalMinutes") &&
            object[L"RefreshIntervalMinutes"].is_number()) {
            const int value = object[L"RefreshIntervalMinutes"].as_int(1);
            if (value >= 1 && value <= 1440) settings.refreshIntervalMinutes = value;
        }
        ReadAppearanceObject(object[L"Appearance"], settings.appearance, errors);
    }

    void ValidateDefaultConfigShape(const JsonValue& root, std::wstring& errors) {
        const JsonValue& settings = root[L"Settings"];
        const JsonValue& appearance = settings[L"Appearance"];
        if (!root.has_key(L"Version") || !root[L"Version"].is_number() ||
            root[L"Version"].as_int() != 2 || !root.has_key(L"Window")) {
            AppendValidationError(
                errors, L"config-default.json 必须使用完整的版本 2 配置结构");
        }
        if (!settings.is_object() || !appearance.is_object()) {
            AppendValidationError(
                errors, L"config-default.json 必须包含 Settings.Appearance 对象");
            return;
        }
        if (!settings.has_key(L"UserScale") ||
            !settings.has_key(L"CompanionMode") ||
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
        if (!root.has_key(L"Version") || !root[L"Version"].is_number() ||
            root[L"Version"].as_int() != 2 ||
            !root.has_key(L"Settings") || !root[L"Settings"].is_object()) {
            AppendValidationError(
                errors, L"config-users.json 必须使用版本 2 配置结构");
        }
    }

    void ReadWindowObject(const JsonValue& object, StoredState& state) {
        if (!object.is_object() || !object.has_key(L"X") || !object[L"X"].is_number() ||
            !object.has_key(L"Y") || !object[L"Y"].is_number()) {
            return;
        }
        state.position.x = object[L"X"].as_int();
        state.position.y = object[L"Y"].as_int();
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

    bool WriteConfig(const std::filesystem::path& path, const ConfigData& data) {
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) return false;

        const std::filesystem::path temporary = path.wstring() + L".tmp";
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file.is_open()) return false;
        std::string fontFamily;
        if (!JsonString(data.settings.appearance.fontFamily, fontFamily)) {
            file.close();
            DeleteFileW(temporary.c_str());
            return false;
        }
        file.imbue(std::locale::classic());
        file << std::setprecision(9);
        file << "{\n";
        file << "  \"Version\": 2,\n";
        file << "  \"Settings\": {\n";
        file << "    \"UserScale\": " << data.settings.userScale << ",\n";
        file << "    \"CompanionMode\": "
             << (data.settings.companionMode ? "true" : "false") << ",\n";
        file << "    \"RefreshIntervalMinutes\": "
             << data.settings.refreshIntervalMinutes << ",\n";
        file << "    \"Appearance\": {\n";
        file << "      \"Mode\": \""
             << (data.settings.appearance.mode == AppearanceMode::Custom
                     ? "Custom" : "Default")
             << "\",\n";
        file << "      \"FontFamily\": " << fontFamily << ",\n";
        file << "      \"BackgroundTransparency\": "
             << data.settings.appearance.backgroundTransparency << ",\n";
        file << "      \"Colors\": {";
        if (!data.settings.appearance.colors.empty()) file << "\n";
        size_t colorIndex = 0;
        for (const auto& [name, value] : data.settings.appearance.colors) {
            std::string key;
            std::string color;
            if (!JsonString(name, key) || !JsonString(value, color)) {
                file.close();
                DeleteFileW(temporary.c_str());
                return false;
            }
            file << "        " << key << ": " << color;
            if (++colorIndex < data.settings.appearance.colors.size()) file << ",";
            file << "\n";
        }
        if (!data.settings.appearance.colors.empty()) file << "      ";
        file << "}\n";
        file << "    }\n";
        file << "  },\n";
        if (data.state.hasPosition) {
            file << "  \"Window\": { \"X\": " << data.state.position.x
                 << ", \"Y\": " << data.state.position.y << " }\n";
        } else {
            file << "  \"Window\": null\n";
        }
        file << "}\n";
        file.flush();
        const bool written = file.good();
        file.close();
        if (!written || !MoveFileExW(temporary.c_str(), path.c_str(),
                                     MOVEFILE_REPLACE_EXISTING |
                                         MOVEFILE_WRITE_THROUGH)) {
            DeleteFileW(temporary.c_str());
            return false;
        }
        return true;
    }

    ConfigData LoadData(
        ReadStatus* readStatus = nullptr,
        std::wstring* validationErrors = nullptr)
    {
        if (validationErrors) validationErrors->clear();
        ConfigData data;
        std::wstring errors;

        JsonValue defaultRoot;
        const ReadStatus defaultStatus = ReadJson(DefaultConfigPath(), defaultRoot);
        if (defaultStatus == ReadStatus::Valid) {
            ValidateDefaultConfigShape(defaultRoot, errors);
            ReadSettingsObject(defaultRoot[L"Settings"], data.settings, errors);
        } else {
            AppendValidationError(
                errors,
                L"config-default.json 不是有效的 UTF-8 JSON，或文件不可读取");
        }
        data.settings.defaultAppearance = data.settings.appearance;

        const std::filesystem::path directory = ConfigDirectory();
        const std::filesystem::path userConfigPath = directory / L"config-users.json";

        JsonValue root;
        const ReadStatus userStatus = ReadJson(userConfigPath, root);

        const ReadStatus combinedStatus =
            defaultStatus == ReadStatus::Valid ? userStatus : ReadStatus::Invalid;
        if (readStatus) *readStatus = combinedStatus;
        if (userStatus == ReadStatus::Valid) {
            ValidateUserConfigShape(root, errors);
            ReadSettingsObject(root[L"Settings"], data.settings, errors);
            ReadWindowObject(root[L"Window"], data.state);
        } else if (userStatus == ReadStatus::Invalid) {
            AppendValidationError(
                errors,
                L"config-users.json 不是有效的 UTF-8 JSON，或文件不可读取");
        }
        if (validationErrors) *validationErrors = errors;
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
        ConfigData data = LoadData(&status, &errors);
        ValidateAppearanceSettings(settings.appearance, errors);
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
        ConfigData data = LoadData(&status, &errors);
        if (status == ReadStatus::Invalid || !errors.empty()) return;
        data.state.position = position;
        data.state.hasPosition = true;
        WriteConfig(ConfigDirectory() / L"config-users.json", data);
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
