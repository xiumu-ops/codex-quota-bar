#pragma once

#include <windows.h>
#include <utility>

namespace CodexQuotaBar {
    class WinHandle {
    public:
        explicit WinHandle(HANDLE handle = nullptr) noexcept : m_handle(handle) {}
        ~WinHandle() { if (*this) CloseHandle(m_handle); }
        WinHandle(const WinHandle&) = delete;
        WinHandle& operator=(const WinHandle&) = delete;
        WinHandle(WinHandle&& other) noexcept : m_handle(std::exchange(other.m_handle, nullptr)) {}
        explicit operator bool() const noexcept { return m_handle && m_handle != INVALID_HANDLE_VALUE; }
        HANDLE Get() const noexcept { return m_handle; }

    private:
        HANDLE m_handle;
    };
}
