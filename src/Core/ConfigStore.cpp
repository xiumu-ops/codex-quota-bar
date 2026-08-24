#include "Core/ConfigStore.h"
#include "Core/SimpleJson.h"

#include <knownfolders.h>
#include <shlobj.h>

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
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

    std::mutex g_configMutex;

    std::filesystem::path ConfigDirectory() {
        PWSTR localAppData = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData))) {
            return std::filesystem::current_path() / L"Codex-Quota-Bar";
        }
        const std::filesystem::path directory =
            std::filesystem::path(localAppData) / L"Codex-Quota-Bar";
        CoTaskMemFree(localAppData);
        return directory;
    }

    bool ReadJson(const std::filesystem::path& path, JsonValue& root, bool& exists) {
        std::error_code error;
        exists = std::filesystem::exists(path, error);
        if (error || !exists) return false;

        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) return false;
        file.seekg(0, std::ios::end);
        const std::streamoff size = file.tellg();
        if (size <= 0 || size > kMaxConfigBytes) return false;
        file.seekg(0, std::ios::beg);

        std::stringstream stream;
        stream << file.rdbuf();
        const std::string utf8 = stream.str();
        if (utf8.empty() || utf8.size() > static_cast<size_t>((std::numeric_limits<int>::max)())) {
            return false;
        }

        const int length = MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()),
            nullptr, 0);
        if (length <= 0) return false;
        std::wstring wide(static_cast<size_t>(length), L'\0');
        if (MultiByteToWideChar(
                CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()),
                wide.data(), length) != length) {
            return false;
        }
        return JsonParser::TryParse(wide, root) && root.is_object();
    }

    void ReadSettingsObject(const JsonValue& object, AppSettings& settings) {
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

    bool WriteConfig(const std::filesystem::path& path, const ConfigData& data) {
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) return false;

        const std::filesystem::path temporary = path.wstring() + L".tmp";
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file.is_open()) return false;
        file.imbue(std::locale::classic());
        file << std::setprecision(9);
        file << "{\n";
        file << "  \"Version\": 1,\n";
        file << "  \"Settings\": {\n";
        file << "    \"UserScale\": " << data.settings.userScale << ",\n";
        file << "    \"CompanionMode\": "
             << (data.settings.companionMode ? "true" : "false") << ",\n";
        file << "    \"RefreshIntervalMinutes\": "
             << data.settings.refreshIntervalMinutes << "\n";
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

    ConfigData LoadData() {
        ConfigData data;
        const std::filesystem::path directory = ConfigDirectory();
        const std::filesystem::path configPath = directory / L"config.json";

        JsonValue root;
        bool configExists = false;
        if (ReadJson(configPath, root, configExists)) {
            ReadSettingsObject(root[L"Settings"], data.settings);
            ReadWindowObject(root[L"Window"], data.state);
            return data;
        }
        // 已存在但无效的统一配置绝不被默认值覆盖。
        (void)configExists;
        return data;
    }

} // namespace

    AppSettings ConfigStore::LoadSettings() {
        const std::lock_guard<std::mutex> lock(g_configMutex);
        return LoadData().settings;
    }

    StoredState ConfigStore::LoadState() {
        const std::lock_guard<std::mutex> lock(g_configMutex);
        return LoadData().state;
    }

    void ConfigStore::SaveSettings(const AppSettings& settings) {
        const std::lock_guard<std::mutex> lock(g_configMutex);
        ConfigData data = LoadData();
        data.settings = settings;
        WriteConfig(ConfigDirectory() / L"config.json", data);
    }

    void ConfigStore::SavePosition(POINT position) {
        const std::lock_guard<std::mutex> lock(g_configMutex);
        ConfigData data = LoadData();
        data.state.position = position;
        data.state.hasPosition = true;
        WriteConfig(ConfigDirectory() / L"config.json", data);
    }

} // namespace CodexQuotaBar
