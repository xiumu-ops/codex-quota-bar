#include "UI/TaskbarPlacement.h"
#include "Core/Constants.h"
#include "Core/DpiHelper.h"

#include <ole2.h>
#include <uiautomation.h>
#include <wrl/client.h>
#include <cstdlib>
#include <cwchar>

#pragma comment(lib, "uiautomationcore.lib")

namespace CodexQuotaBar {
namespace {

    constexpr PROPERTYID kPropAutomationId = 30011;
    constexpr PROPERTYID kPropClassName = 30012;
    constexpr PROPERTYID kPropIsOffscreen = 30022;
    using Microsoft::WRL::ComPtr;

    struct ComApartment {
        HRESULT status = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ~ComApartment() { if (SUCCEEDED(status)) CoUninitialize(); }
    };

    struct ScopedVariant {
        VARIANT value = {};
        explicit ScopedVariant(const wchar_t* text) {
            value.vt = VT_BSTR;
            value.bstrVal = SysAllocString(text);
        }
        ~ScopedVariant() { VariantClear(&value); }
        ScopedVariant(const ScopedVariant&) = delete;
        ScopedVariant& operator=(const ScopedVariant&) = delete;
    };

    bool GetWidgetBounds(const RECT& rect, const RECT& taskbar, RECT& widget) {
        RECT visible = {};
        if (!IntersectRect(&visible, &rect, &taskbar)) return false;
        const LONG width = visible.right - visible.left;
        const LONG height = visible.bottom - visible.top;
        const LONG trayWidth = taskbar.right - taskbar.left;
        const LONG trayHeight = taskbar.bottom - taskbar.top;
        const bool horizontal = trayWidth >= trayHeight;
        if (width < 16 || height < 16 ||
            (horizontal ? width > 6 * trayHeight : height > 6 * trayWidth)) return false;
        widget = visible;
        return true;
    }

    ComPtr<IUIAutomationCondition> ButtonCondition(
        IUIAutomation& automation, PROPERTYID property, const wchar_t* const* names, size_t count)
    {
        ComPtr<IUIAutomationCondition> combined;
        for (size_t i = 0; i < count; ++i) {
            const ScopedVariant name(names[i]);
            ComPtr<IUIAutomationCondition> match;
            if (!name.value.bstrVal || FAILED(automation.CreatePropertyCondition(
                    property, name.value, match.GetAddressOf()))) continue;
            if (!combined) {
                combined = std::move(match);
            } else {
                ComPtr<IUIAutomationCondition> next;
                if (SUCCEEDED(automation.CreateOrCondition(
                        combined.Get(), match.Get(), next.GetAddressOf()))) combined = std::move(next);
            }
        }
        return combined;
    }

    bool TryFindWeatherWidgetUIA(const TaskbarMetrics& metrics, RECT& widget, std::stop_token stop) {
        if (stop.stop_requested() || !IsWindow(metrics.hTaskbar)) return false;

        ComPtr<IUIAutomation2> automation;
        if (FAILED(CoCreateInstance(__uuidof(CUIAutomation8), nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(automation.GetAddressOf()))) ||
            FAILED(automation->put_ConnectionTimeout(600)) ||
            FAILED(automation->put_TransactionTimeout(600))) return false;

        ComPtr<IUIAutomationElement> taskbar;
        if (stop.stop_requested() ||
            FAILED(automation->ElementFromHandle(metrics.hTaskbar, taskbar.GetAddressOf())) ||
            !taskbar) return false;

        constexpr const wchar_t* ids[] = { L"WidgetsButton", L"TaskbarAugmentedEntryPointButton" };
        constexpr const wchar_t* classes[] = {
            L"Taskbar.AugmentedEntryPointButton", L"AugmentedEntryPointButton"
        };
        VARIANT visible = {};
        visible.vt = VT_BOOL;
        visible.boolVal = VARIANT_FALSE;
        ComPtr<IUIAutomationCondition> onScreen;
        if (FAILED(automation->CreatePropertyCondition(kPropIsOffscreen, visible, onScreen.GetAddressOf()))) {
            return false;
        }

        const auto find = [&](PROPERTYID property, const wchar_t* const* names, size_t count) {
            if (stop.stop_requested()) return false;
            auto button = ButtonCondition(*automation.Get(), property, names, count);
            ComPtr<IUIAutomationCondition> filter;
            ComPtr<IUIAutomationElement> element;
            RECT rect = {};
            return button && SUCCEEDED(automation->CreateAndCondition(
                button.Get(), onScreen.Get(), filter.GetAddressOf())) && !stop.stop_requested() &&
                SUCCEEDED(taskbar->FindFirst(TreeScope_Descendants, filter.Get(), element.GetAddressOf())) &&
                element && !stop.stop_requested() &&
                SUCCEEDED(element->get_CurrentBoundingRectangle(&rect)) &&
                GetWidgetBounds(rect, metrics.taskbarRect, widget);
        };
        // Prefer exact button IDs. A list or taskbar-wide container isn't a dock target.
        return find(kPropAutomationId, ids, _countof(ids)) || find(kPropClassName, classes, _countof(classes));
    }

    std::optional<RECT> TryFindWeatherWidgetChildWindow(const TaskbarMetrics& metrics, std::stop_token stop) {
        if (stop.stop_requested() || !IsWindow(metrics.hTaskbar)) return {};
        struct Context {
            RECT taskbar;
            std::stop_token stop;
            std::optional<RECT> result;
        } context{ metrics.taskbarRect, stop, {} };
        EnumChildWindows(metrics.hTaskbar, [](HWND hwnd, LPARAM param) -> BOOL {
            auto& ctx = *reinterpret_cast<Context*>(param);
            if (ctx.stop.stop_requested()) return FALSE;
            if (!IsWindowVisible(hwnd)) return TRUE;
            wchar_t className[128] = {};
            GetClassNameW(hwnd, className, _countof(className));
            // Class lookup does not send WM_GETTEXT to Explorer or match the
            // captions of unrelated applications pinned to the taskbar.
            if (_wcsicmp(className, L"TrayDynamicContentWindow") != 0 &&
                _wcsicmp(className, L"FeedsButton") != 0 &&
                _wcsicmp(className, L"WidgetsButton") != 0 &&
                _wcsicmp(className, L"TaskbarAugmentedEntryPointButton") != 0) return TRUE;
            RECT rect = {};
            RECT visible = {};
            if (GetWindowRect(hwnd, &rect) && GetWidgetBounds(rect, ctx.taskbar, visible)) {
                ctx.result = visible;
                return FALSE;
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&context));
        return context.result;
    }

    bool IsWindows11Taskbar() {
        static const bool windows11 = [] {
            wchar_t build[24] = {};
            DWORD bytes = sizeof(build);
            return RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                    L"CurrentBuildNumber", RRF_RT_REG_SZ, nullptr, build, &bytes) == ERROR_SUCCESS &&
                    std::wcstoul(build, nullptr, 10) >= 22000;
        }();
        return windows11;
    }

    HWND FindTaskbarForMonitor(HMONITOR monitor) {
        const HWND primary = FindWindowW(L"Shell_TrayWnd", nullptr);
        if (primary && MonitorFromWindow(primary, MONITOR_DEFAULTTONEAREST) == monitor) return primary;
        HWND secondary = nullptr;
        while ((secondary = FindWindowExW(nullptr, secondary, L"Shell_SecondaryTrayWnd", nullptr))) {
            if (MonitorFromWindow(secondary, MONITOR_DEFAULTTONEAREST) == monitor) return secondary;
        }
        return nullptr;
    }

} // namespace

    TaskbarMetrics TaskbarPlacement::GetTaskbarMetrics(POINT anchor, float fallbackDpiScale) {
        TaskbarMetrics metrics;
        const HMONITOR monitor = MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info = { sizeof(info) };
        if (monitor && GetMonitorInfoW(monitor, &info)) {
            metrics.monitorRect = info.rcMonitor;
            metrics.workAreaRect = info.rcWork;
        } else {
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &metrics.workAreaRect, 0);
            metrics.monitorRect = metrics.workAreaRect;
        }
        metrics.hTaskbar = FindTaskbarForMonitor(monitor);
        const UINT dpi = metrics.hTaskbar ? GetDpiForWindow(metrics.hTaskbar) : 0;
        metrics.dpiScale = dpi ? static_cast<float>(dpi) / BASE_DPI : fallbackDpiScale;

        RECT raw = {};
        if (metrics.hTaskbar && GetWindowRect(metrics.hTaskbar, &raw) &&
            raw.right > raw.left && raw.bottom > raw.top) {
            metrics.isHorizontal = raw.right - raw.left >= raw.bottom - raw.top;
            metrics.taskbarRect = NormalizeTaskbarRect(raw, metrics.monitorRect, metrics.isHorizontal,
                ScaleEven(24.0f, metrics.dpiScale),
                ScaleEven(IsWindows11Taskbar() ? 48.0f : 40.0f, metrics.dpiScale));
        } else {
            metrics.hTaskbar = nullptr;
        }
        return metrics;
    }

    int TaskbarPlacement::GetTaskbarAlignment() {
        DWORD alignment = 0;
        DWORD bytes = sizeof(alignment);
        if (RegGetValueW(HKEY_CURRENT_USER,
                L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
                L"TaskbarAl", RRF_RT_REG_DWORD, nullptr, &alignment, &bytes) == ERROR_SUCCESS &&
            alignment <= 1) return static_cast<int>(alignment);
        return IsWindows11Taskbar() ? 1 : 0;
    }

    std::optional<RECT> TaskbarPlacement::ProbeWeatherWidget(
        const TaskbarMetrics& metrics, std::stop_token stop) {
        try {
            const ComApartment com;
            RECT rect = {};
            // Respect an existing STA and never call CoUninitialize for a
            // failed initialization. Win32 fallback does not require COM.
            if ((SUCCEEDED(com.status) || com.status == RPC_E_CHANGED_MODE) &&
                TryFindWeatherWidgetUIA(metrics, rect, stop)) return rect;
        } catch (...) {
            // The Win32 fallback is independent of UI Automation.
        }
        return TryFindWeatherWidgetChildWindow(metrics, stop);
    }

    WeatherWidgetPlacement TaskbarPlacement::FindWeatherWidgetPlacement(
        SIZE miniPhysicalSize, const TaskbarMetrics& metrics, const std::optional<RECT>& widgetRect)
    {
        WeatherWidgetPlacement placement = {};
        if (!metrics.hTaskbar) {
            const auto& work = metrics.workAreaRect;
            const LONG margin = ScaleEven(12.0f, metrics.dpiScale);
            placement.dockPosition = ClampToMonitorBounds(
                { work.right - miniPhysicalSize.cx - margin, work.bottom - miniPhysicalSize.cy - margin },
                miniPhysicalSize, work);
            return placement;
        }

        if (widgetRect) {
            placement.widgetRect = *widgetRect;
            placement.exactWidgetFound = true;
        } else {
            placement.exactWidgetFound = false;
            RECT slotRect = {};
            if (metrics.isHorizontal) {
                const int alignment = GetTaskbarAlignment();
                if (alignment != 0 && metrics.hTaskbar == FindWindowW(L"Shell_TrayWnd", nullptr)) {
                    // Windows 11 居中对齐时：天气槽位于任务栏最左侧，左留白 12 DIP
                    const int marginX = ScaleEven(12.0f, metrics.dpiScale);
                    slotRect.left = metrics.taskbarRect.left + marginX;
                    slotRect.right = slotRect.left + miniPhysicalSize.cx;
                    slotRect.top = metrics.taskbarRect.top;
                    slotRect.bottom = metrics.taskbarRect.bottom;
                } else {
                    // Windows 11 左对齐或 Windows 10 时：位于通知区左侧
                    HWND hTray = FindWindowExW(metrics.hTaskbar, NULL, L"TrayNotifyWnd", NULL);
                    RECT rcTray = {};
                    const int marginX = ScaleEven(12.0f, metrics.dpiScale);
                    if (hTray && IsWindow(hTray) && GetWindowRect(hTray, &rcTray) &&
                        rcTray.left > metrics.taskbarRect.left) {
                        slotRect.right = rcTray.left - marginX;
                        slotRect.left = slotRect.right - miniPhysicalSize.cx;
                    } else {
                        const int fallbackMargin = ScaleEven(160.0f, metrics.dpiScale);
                        slotRect.right = metrics.taskbarRect.right - fallbackMargin;
                        slotRect.left = slotRect.right - miniPhysicalSize.cx;
                    }
                    slotRect.top = metrics.taskbarRect.top;
                    slotRect.bottom = metrics.taskbarRect.bottom;
                }
            } else {
                slotRect.left = metrics.taskbarRect.left;
                slotRect.right = metrics.taskbarRect.right;
                slotRect.top = metrics.taskbarRect.top + ScaleEven(12.0f, metrics.dpiScale);
                slotRect.bottom = slotRect.top + miniPhysicalSize.cy;
            }
            placement.widgetRect = slotRect;
        }

        placement.dockPosition = ComputeDockPosition(
            placement.widgetRect,
            metrics.taskbarRect,
            metrics.isHorizontal,
            miniPhysicalSize,
            placement.exactWidgetFound);
        placement.dockPosition = ClampToMonitorBounds(
            placement.dockPosition, miniPhysicalSize, metrics.monitorRect);

        return placement;
    }

} // namespace CodexQuotaBar
