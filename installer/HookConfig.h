#pragma once

#include <filesystem>
#include <string>

namespace CodexQuotaBarInstaller::HookConfig {

    struct Result {
        bool success = false;
        bool changed = false;
        std::wstring error;
    };

    std::filesystem::path ResolveHookFilePath();
    Result Install(const std::filesystem::path& hookFilePath,
                   const std::filesystem::path& appPath);
    Result Remove(const std::filesystem::path& hookFilePath);

} // namespace CodexQuotaBarInstaller::HookConfig
