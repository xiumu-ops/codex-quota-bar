#include "Core/Logger.h"
#include "Core/ConfigStore.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <filesystem>
#include <fstream>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <iterator>
#include <mutex>
#include <string>

namespace CodexQuotaBar {
namespace {

    constexpr std::uintmax_t kMaxLogBytes = 256 * 1024;
    std::mutex g_logMutex;

    const char* LevelText(LogLevel level) {
        switch (level) {
        case LogLevel::Info: return "INFO";
        case LogLevel::Warning: return "WARN";
        case LogLevel::Error: return "ERROR";
        }
        return "UNKNOWN";
    }

    bool ToUtf8(std::wstring_view wide, std::string& utf8) {
        if (wide.empty()) {
            utf8.clear();
            return true;
        }
        if (wide.size() > static_cast<size_t>((std::numeric_limits<int>::max)())) return false;
        const int sourceLength = static_cast<int>(wide.size());
        const int required = WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), sourceLength,
            nullptr, 0, nullptr, nullptr);
        if (required <= 0) return false;
        utf8.assign(static_cast<size_t>(required), '\0');
        return WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), sourceLength,
            utf8.data(), required, nullptr, nullptr) == required;
    }

} // namespace

    void WriteLog(LogLevel level, std::wstring_view message) noexcept {
        try {
            wchar_t disabled[2] = {};
            if (GetEnvironmentVariableW(
                    L"CODEX_QUOTA_DISABLE_LOG", disabled,
                    static_cast<DWORD>(std::size(disabled))) > 0 && disabled[0] == L'1') {
                return;
            }
            const std::lock_guard<std::mutex> lock(g_logMutex);
            const std::filesystem::path directory(ConfigStore::DataDirectory());
            if (directory.empty()) return;

            std::error_code error;
            std::filesystem::create_directories(directory, error);
            if (error) return;

            const std::filesystem::path logPath = directory / L"diagnostic.log";
            const std::filesystem::path previousPath = directory / L"diagnostic.previous.log";
            const std::uintmax_t size = std::filesystem::file_size(logPath, error);
            if (!error && size >= kMaxLogBytes) {
                std::filesystem::remove(previousPath, error);
                error.clear();
                std::filesystem::rename(logPath, previousPath, error);
            }

            std::string utf8;
            if (!ToUtf8(message, utf8)) utf8 = "<message encoding failed>";

            SYSTEMTIME time = {};
            GetLocalTime(&time);
            char prefix[128] = {};
            sprintf_s(
                prefix, "%04u-%02u-%02u %02u:%02u:%02u.%03u [%s] [pid=%lu tid=%lu] ",
                static_cast<unsigned>(time.wYear), static_cast<unsigned>(time.wMonth),
                static_cast<unsigned>(time.wDay), static_cast<unsigned>(time.wHour),
                static_cast<unsigned>(time.wMinute), static_cast<unsigned>(time.wSecond),
                static_cast<unsigned>(time.wMilliseconds), LevelText(level),
                GetCurrentProcessId(), GetCurrentThreadId());

            std::ofstream file(logPath, std::ios::binary | std::ios::app);
            if (!file.is_open()) return;
            file << prefix << utf8 << "\r\n";
        } catch (...) {
            // 日志绝不能影响主流程或异常清理。
        }
    }

} // namespace CodexQuotaBar
