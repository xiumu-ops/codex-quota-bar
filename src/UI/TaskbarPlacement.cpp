#include "UI/TaskbarPlacement.h"
#include "Core/Constants.h"
#include "Core/DpiHelper.h"
#include "Core/Logger.h"

#include <shellapi.h>
#include <ole2.h>
#include <uiautomation.h>
#include <wrl/client.h>

namespace CodexQuotaBar {
namespace {

    bool TryFindWeatherWidgetUIA(HWND hTaskbar, const RECT& taskbarRect, RECT& widgetRect) {
        if (!hTaskbar || !IsWindow(hTaskbar)) return false;

        Microsoft::WRL::ComPtr<IUIAutomation> automation;
        HRESULT hr = CoCreateInstance(
            __uuidof(CUIAutomation),
            nullptr,
            CLSCTX_INPROC_SERVER,
            __uuidof(IUIAutomation),
            reinterpret_cast<void**>(automation.GetAddressOf()));

        if (FAILED(hr) || !automation) return false;

        Microsoft::WRL::ComPtr<IUIAutomationElement> taskbarElem;
        hr = automation->ElementFromHandle(hTaskbar, taskbarElem.GetAddressOf());
        if (FAILED(hr) || !taskbarElem) return false;

        const wchar_t* const idCandidates[] = {
            L"WidgetsButton",
            L"TaskbarAugmentedEntryPointButton",
            L"Widgets"
        };
        const wchar_t* const classCandidates[] = {
            L"Taskbar.AugmentedEntryPointButton",
            L"AugmentedEntryPointButton",
            L"Taskbar.AugmentedEntryPointListView"
        };

        std::vector<Microsoft::WRL::ComPtr<IUIAutomationCondition>> conditions;

        for (const wchar_t* idName : idCandidates) {
            VARIANT varId;
            VariantInit(&varId);
            varId.vt = VT_BSTR;
            varId.bstrVal = SysAllocString(idName);
            if (varId.bstrVal) {
                Microsoft::WRL::ComPtr<IUIAutomationCondition> cond;
                if (SUCCEEDED(automation->CreatePropertyCondition(
                        UIA_AutomationIdPropertyId, varId, cond.GetAddressOf())) && cond) {
                    conditions.push_back(cond);
                }
                VariantClear(&varId);
            }
        }

        for (const wchar_t* className : classCandidates) {
            VARIANT varClass;
            VariantInit(&varClass);
            varClass.vt = VT_BSTR;
            varClass.bstrVal = SysAllocString(className);
            if (varClass.bstrVal) {
                Microsoft::WRL::ComPtr<IUIAutomationCondition> cond;
                if (SUCCEEDED(automation->CreatePropertyCondition(
                        UIA_ClassNamePropertyId, varClass, cond.GetAddressOf())) && cond) {
                    conditions.push_back(cond);
                }
                VariantClear(&varClass);
            }
        }

        if (conditions.empty()) return false;

        Microsoft::WRL::ComPtr<IUIAutomationCondition> searchCond = conditions[0];
        for (size_t i = 1; i < conditions.size(); ++i) {
            Microsoft::WRL::ComPtr<IUIAutomationCondition> combined;
            if (SUCCEEDED(automation->CreateOrCondition(
                    searchCond.Get(), conditions[i].Get(), combined.GetAddressOf())) && combined) {
                searchCond = combined;
            }
        }

        if (!searchCond) return false;

        Microsoft::WRL::ComPtr<IUIAutomationElement> widgetElem;
        hr = taskbarElem->FindFirst(
            TreeScope_Descendants, searchCond.Get(), widgetElem.GetAddressOf());

        if (SUCCEEDED(hr) && widgetElem) {
            BOOL isOffscreen = FALSE;
            if (SUCCEEDED(widgetElem->get_CurrentIsOffscreen(&isOffscreen)) && isOffscreen) {
                return false;
            }

            RECT rect = {};
            hr = widgetElem->get_CurrentBoundingRectangle(&rect);
            if (SUCCEEDED(hr) && (rect.right > rect.left) && (rect.bottom > rect.top)) {
                RECT inter = {};
                if (IntersectRect(&inter, &rect, &taskbarRect) &&
                    (inter.right - inter.left >= 16) && (inter.bottom - inter.top >= 16)) {
                    widgetRect = rect;
                    return true;
                }
            }
        }

        return false;
    }

    bool TryFindWeatherWidgetChildWindow(HWND hTaskbar, const RECT& taskbarRect, RECT& widgetRect) {
        if (!hTaskbar || !IsWindow(hTaskbar)) return false;

        struct EnumCtx {
            RECT taskbarRect = {};
            RECT foundRect = {};
            bool found = false;
        } ctx;
        ctx.taskbarRect = taskbarRect;

        EnumChildWindows(
            hTaskbar,
            [](HWND hWnd, LPARAM lParam) -> BOOL {
                if (!IsWindowVisible(hWnd)) return TRUE;
                auto* pCtx = reinterpret_cast<EnumCtx*>(lParam);
                wchar_t className[128] = {};
                wchar_t windowText[128] = {};
                GetClassNameW(hWnd, className, 128);
                GetWindowTextW(hWnd, windowText, 128);

                if (wcsstr(className, L"Feeds") ||
                    wcsstr(className, L"Weather") ||
                    wcsstr(className, L"Widget") ||
                    wcsstr(windowText, L"Feeds") ||
                    wcsstr(windowText, L"Weather") ||
                    wcsstr(windowText, L"Widget")) {
                    RECT rc = {};
                    if (GetWindowRect(hWnd, &rc) && (rc.right > rc.left) && (rc.bottom > rc.top)) {
                        RECT inter = {};
                        if (IntersectRect(&inter, &rc, &pCtx->taskbarRect) &&
                            (inter.right - inter.left >= 16) && (inter.bottom - inter.top >= 16)) {
                            pCtx->foundRect = rc;
                            pCtx->found = true;
                            return FALSE;
                        }
                    }
                }
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&ctx));

        if (ctx.found) {
            widgetRect = ctx.foundRect;
            return true;
        }
        return false;
    }

} // namespace

    TaskbarMetrics TaskbarPlacement::GetTaskbarMetrics() {
        TaskbarMetrics metrics = {};
        HWND hTaskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
        metrics.hTaskbar = hTaskbar;

        HMONITOR hMon = nullptr;
        if (hTaskbar && IsWindow(hTaskbar)) {
            GetWindowRect(hTaskbar, &metrics.taskbarRect);
            hMon = MonitorFromWindow(hTaskbar, MONITOR_DEFAULTTOPRIMARY);
        } else {
            hMon = MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
        }

        MONITORINFO mi = { sizeof(MONITORINFO) };
        if (hMon && GetMonitorInfoW(hMon, &mi)) {
            metrics.monitorRect = mi.rcMonitor;
            metrics.workAreaRect = mi.rcWork;
        } else {
            RECT rcWork = {};
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &rcWork, 0);
            metrics.monitorRect = rcWork;
            metrics.workAreaRect = rcWork;
        }

        if (metrics.taskbarRect.right <= metrics.taskbarRect.left ||
            metrics.taskbarRect.bottom <= metrics.taskbarRect.top) {
            metrics.taskbarRect.left = metrics.monitorRect.left;
            metrics.taskbarRect.right = metrics.monitorRect.right;
            metrics.taskbarRect.top = metrics.workAreaRect.bottom;
            metrics.taskbarRect.bottom = metrics.monitorRect.bottom;
            if (metrics.taskbarRect.top >= metrics.taskbarRect.bottom) {
                metrics.taskbarRect.top = metrics.monitorRect.bottom - 48;
                metrics.taskbarRect.bottom = metrics.monitorRect.bottom;
            }
        }

        UINT dpi = 0;
        if (hTaskbar && IsWindow(hTaskbar)) {
            dpi = GetDpiForWindow(hTaskbar);
        }
        if (dpi == 0) {
            dpi = GetDpiForSystem();
        }
        if (dpi == 0) dpi = BASE_DPI;
        metrics.dpiScale = static_cast<float>(dpi) / static_cast<float>(BASE_DPI);

        APPBARDATA abd = { sizeof(APPBARDATA) };
        abd.hWnd = hTaskbar;
        const UINT_PTR state = SHAppBarMessage(ABM_GETSTATE, &abd);
        metrics.autoHide = ((state & ABS_AUTOHIDE) != 0);

        const int tbW = metrics.taskbarRect.right - metrics.taskbarRect.left;
        int tbH = metrics.taskbarRect.bottom - metrics.taskbarRect.top;
        metrics.isHorizontal = (tbW >= tbH);
        metrics.isAtBottom = (metrics.taskbarRect.bottom >= metrics.monitorRect.bottom &&
                              metrics.taskbarRect.top > metrics.monitorRect.top);
        metrics.isAtTop = (metrics.taskbarRect.top <= metrics.monitorRect.top &&
                           metrics.taskbarRect.bottom < metrics.monitorRect.bottom);

        // 当开启任务栏自动隐藏时，隐藏状态下 taskbarRect 高度通常缩小至 2~4px。
        // 为确保吸附与停靠坐标稳定，按系统基准高度重构实际任务栏槽位几何。
        const int minTbHeight = ScaleEven(24.0f, metrics.dpiScale);
        if (metrics.isHorizontal && tbH < minTbHeight) {
            const int defaultTbHeight = ScaleEven(48.0f, metrics.dpiScale);
            if (metrics.isAtTop) {
                metrics.taskbarRect.top = metrics.monitorRect.top;
                metrics.taskbarRect.bottom = metrics.monitorRect.top + defaultTbHeight;
            } else {
                metrics.taskbarRect.top = metrics.monitorRect.bottom - defaultTbHeight;
                metrics.taskbarRect.bottom = metrics.monitorRect.bottom;
            }
        }

        return metrics;
    }

    int TaskbarPlacement::GetTaskbarAlignment() {
        DWORD alignment = 1;
        DWORD size = sizeof(alignment);
        DWORD type = REG_DWORD;
        HKEY hKey = nullptr;
        if (RegOpenKeyExW(
                HKEY_CURRENT_USER,
                L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
                0,
                KEY_READ,
                &hKey) == ERROR_SUCCESS) {
            if (RegQueryValueExW(
                    hKey,
                    L"TaskbarAl",
                    nullptr,
                    &type,
                    reinterpret_cast<LPBYTE>(&alignment),
                    &size) == ERROR_SUCCESS && type == REG_DWORD) {
                RegCloseKey(hKey);
                return static_cast<int>(alignment);
            }
            RegCloseKey(hKey);
        }
        return 1;
    }

    WeatherWidgetPlacement TaskbarPlacement::FindWeatherWidgetPlacement(
        SIZE miniPhysicalSize,
        float dpiScale,
        float uiScale)
    {
        static_cast<void>(dpiScale);
        static_cast<void>(uiScale);
        WeatherWidgetPlacement placement = {};
        const TaskbarMetrics metrics = GetTaskbarMetrics();

        RECT foundRect = {};
        if (TryFindWeatherWidgetUIA(metrics.hTaskbar, metrics.taskbarRect, foundRect)) {
            placement.widgetRect = foundRect;
            placement.exactWidgetFound = true;
        } else if (TryFindWeatherWidgetChildWindow(metrics.hTaskbar, metrics.taskbarRect, foundRect)) {
            placement.widgetRect = foundRect;
            placement.exactWidgetFound = true;
        } else {
            placement.exactWidgetFound = false;
            RECT slotRect = {};
            if (metrics.isHorizontal) {
                const int alignment = GetTaskbarAlignment();
                if (alignment != 0) {
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
