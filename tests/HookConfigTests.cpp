#include "HookConfig.h"
#include "Core/SimpleJson.h"

#include <windows.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

std::string Read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream stream;
    stream << input.rdbuf();
    return stream.str();
}

void Write(const std::filesystem::path& path, const std::string& value) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(value.data(), static_cast<std::streamsize>(value.size()));
}

size_t Count(const std::string& value, const std::string& needle) {
    size_t count = 0;
    size_t offset = 0;
    while ((offset = value.find(needle, offset)) != std::string::npos) {
        ++count;
        offset += needle.size();
    }
    return count;
}

bool Expect(bool condition, const wchar_t* message) {
    if (!condition) std::wcerr << L"[FAIL] " << message << L'\n';
    return condition;
}

bool IsValidJson(const std::string& content) {
    if (content.empty()) return false;
    const int required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, content.data(), static_cast<int>(content.size()),
        nullptr, 0);
    if (required <= 0) return false;
    std::wstring wide(static_cast<size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, content.data(),
                            static_cast<int>(content.size()), wide.data(), required) != required) {
        return false;
    }
    CodexQuotaBar::JsonValue root;
    return CodexQuotaBar::JsonParser::TryParse(wide, root) && root.is_object();
}

} // namespace

int wmain() {
    wchar_t tempDirectory[MAX_PATH] = {};
    if (GetTempPathW(MAX_PATH, tempDirectory) == 0) return 1;
    const std::filesystem::path root = std::filesystem::path(tempDirectory) /
        (L"Codex-Quota-Bar-HookConfigTests-" + std::to_wstring(GetCurrentProcessId()));
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root, error);
    if (error) return 1;

    bool ok = true;
    const std::filesystem::path hooksFile = root / L"hooks.json";
    const std::filesystem::path app =
        L"C:\\Users\\Test\\AppData\\Local\\Codex-Quota-Bar\\app\\Codex-Quota-Bar.exe";
    const std::string original =
        "{\n"
        "  \"description\": \"user hooks\",\n"
        "  \"customMetadata\": { \"keep\": true, \"largeId\": 9007199254740993 },\n"
        "  \"hooks\": {\n"
        "    \"Stop\": [{\n"
        "      \"hooks\": [{\n"
        "        \"type\": \"command\",\n"
        "        \"command\": \"existing-hook\",\n"
        "        \"timeout\": 30\n"
        "      }]\n"
        "    }],\n"
        "    \"PreToolUse\": [{\n"
        "      \"matcher\": \"Bash\",\n"
        "      \"hooks\": [{ \"type\": \"command\", \"command\": \"policy-hook\" }]\n"
        "    }]\n"
        "  }\n"
        "}\n";

    wchar_t* previousCodexHome = nullptr;
    size_t previousCodexHomeLength = 0;
    _wdupenv_s(&previousCodexHome, &previousCodexHomeLength, L"CODEX_HOME");
    SetEnvironmentVariableW(L"CODEX_HOME", root.c_str());
    ok &= Expect(CodexQuotaBarInstaller::HookConfig::ResolveHookFilePath() == hooksFile,
                 L"只解析 CODEX_HOME 下的 hooks.json");
    SetEnvironmentVariableW(L"CODEX_HOME",
                            previousCodexHomeLength > 0 ? previousCodexHome : nullptr);
    free(previousCodexHome);

    Write(hooksFile, original);

    auto installed = CodexQuotaBarInstaller::HookConfig::Install(hooksFile, app);
    ok &= Expect(installed.success && installed.changed, L"首次安装 Hook");
    std::string content = Read(hooksFile);
    ok &= Expect(IsValidJson(content), L"安装后仍是有效 JSON");
    ok &= Expect(content.find("existing-hook") != std::string::npos &&
                 content.find("policy-hook") != std::string::npos &&
                 content.find("customMetadata") != std::string::npos &&
                 content.find("9007199254740993") != std::string::npos,
                  L"保留既有 Hook 与元数据");
    ok &= Expect(content.find("\"SessionStart\"") != std::string::npos &&
                 content.find("\"Stop\"") != std::string::npos &&
                 content.find("\"SessionEnd\"") != std::string::npos,
                 L"写入三个生命周期事件");
    ok &= Expect(content.find("--hook Stop") != std::string::npos,
                 L"Stop Hook 调用主程序");
    ok &= Expect(content.find("startup|resume|clear|compact") != std::string::npos,
                 L"SessionStart 覆盖全部启动来源");
    ok &= Expect(content.find("config.toml") == std::string::npos,
                 L"Hook 文件不引用 config.toml");
    size_t backupCount = 0;
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        if (entry.path().extension() == L".bak") ++backupCount;
    }
    ok &= Expect(backupCount == 0, L"不创建 config 或 Hook 备份文件");

    installed = CodexQuotaBarInstaller::HookConfig::Install(hooksFile, app);
    content = Read(hooksFile);
    ok &= Expect(installed.success && IsValidJson(content) &&
                 Count(content, "--hook Stop") == 2,
                 L"重复安装保持幂等");

    auto removed = CodexQuotaBarInstaller::HookConfig::Remove(hooksFile);
    content = Read(hooksFile);
    ok &= Expect(removed.success && removed.changed, L"卸载 Hook");
    ok &= Expect(IsValidJson(content) &&
                 content.find("Codex-Quota-Bar.exe") == std::string::npos &&
                 content.find("existing-hook") != std::string::npos &&
                 content.find("policy-hook") != std::string::npos &&
                 content.find("customMetadata") != std::string::npos,
                 L"卸载仅移除本软件 Hook");

    const std::string malformed = "{ \"hooks\": { \"Stop\": [ }";
    Write(hooksFile, malformed);
    installed = CodexQuotaBarInstaller::HookConfig::Install(hooksFile, app);
    ok &= Expect(!installed.success && Read(hooksFile) == malformed,
                 L"JSON 损坏时拒绝覆盖");

    Write(hooksFile, "{ \"hooks\": false }\n");
    installed = CodexQuotaBarInstaller::HookConfig::Install(hooksFile, app);
    ok &= Expect(!installed.success && Read(hooksFile) == "{ \"hooks\": false }\n",
                 L"hooks 字段类型错误时拒绝覆盖");

    std::filesystem::remove(hooksFile, error);
    installed = CodexQuotaBarInstaller::HookConfig::Install(hooksFile, app);
    ok &= Expect(installed.success && std::filesystem::exists(hooksFile) &&
                 IsValidJson(Read(hooksFile)),
                 L"不存在时创建独立 hooks.json");
    removed = CodexQuotaBarInstaller::HookConfig::Remove(hooksFile);
    ok &= Expect(removed.success && removed.changed &&
                 !std::filesystem::exists(hooksFile),
                 L"卸载后删除仅由本软件创建的空 Hook 文件");

    std::filesystem::remove_all(root, error);
    if (ok) std::wcout << L"HookConfig tests passed.\n";
    return ok ? 0 : 1;
}
