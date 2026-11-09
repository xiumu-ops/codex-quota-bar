#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace CodexQuotaBar {

    struct TaskbarMetrics {
        HWND hTaskbar = NULL;
        RECT taskbarRect = {};
        RECT monitorRect = {};
        RECT workAreaRect = {};
        float dpiScale = 1.0f;
        bool isHorizontal = true;
        bool isAtBottom = true;
        bool isAtTop = false;
        bool autoHide = false;
    };

    struct WeatherWidgetPlacement {
        RECT widgetRect = {};          // Detected or computed weather widget slot
        POINT dockPosition = {};       // Target top-left position for mini bar
        bool exactWidgetFound = false; // True if live UIA element or child window was found
    };

    class TaskbarPlacement {
    public:
        // Pure geometry / placement calculation (inline for fast calling and unit testing)
        static inline POINT ComputeDockPosition(
            const RECT& slotRect,
            const RECT& taskbarRect,
            bool isHorizontal,
            SIZE miniSize,
            bool centerInSlot = true)
        {
            POINT pos = {};
            if (isHorizontal) {
                const int tbH = taskbarRect.bottom - taskbarRect.top;
                pos.y = taskbarRect.top + (tbH - miniSize.cy) / 2;

                const int slotW = slotRect.right - slotRect.left;
                if (centerInSlot && slotW > miniSize.cx) {
                    pos.x = slotRect.left + (slotW - miniSize.cx) / 2;
                } else {
                    pos.x = slotRect.left;
                }
            } else {
                const int tbW = taskbarRect.right - taskbarRect.left;
                if (tbW > miniSize.cx) {
                    pos.x = taskbarRect.left + (tbW - miniSize.cx) / 2;
                } else {
                    pos.x = taskbarRect.left;
                }

                const int slotH = slotRect.bottom - slotRect.top;
                if (centerInSlot && slotH > miniSize.cy) {
                    pos.y = slotRect.top + (slotH - miniSize.cy) / 2;
                } else {
                    pos.y = slotRect.top;
                }
            }
            return pos;
        }

        static inline POINT ClampToMonitorBounds(
            POINT pt,
            SIZE sz,
            const RECT& monitorRect)
        {
            POINT clamped = pt;
            const int monW = monitorRect.right - monitorRect.left;
            const int monH = monitorRect.bottom - monitorRect.top;

            if (sz.cx >= monW) {
                clamped.x = monitorRect.left;
            } else {
                clamped.x = (std::max)(monitorRect.left, (std::min)(clamped.x, monitorRect.right - sz.cx));
            }

            if (sz.cy >= monH) {
                clamped.y = monitorRect.top;
            } else {
                clamped.y = (std::max)(monitorRect.top, (std::min)(clamped.y, monitorRect.bottom - sz.cy));
            }
            return clamped;
        }

        static inline bool IsNearDockPosition(
            POINT currentPos,
            POINT dockPos,
            int snapThreshold)
        {
            return (std::abs(currentPos.x - dockPos.x) <= snapThreshold &&
                    std::abs(currentPos.y - dockPos.y) <= snapThreshold);
        }

        // System query and composite detection
        static TaskbarMetrics GetTaskbarMetrics();
        static int GetTaskbarAlignment();
        static WeatherWidgetPlacement FindWeatherWidgetPlacement(SIZE miniPhysicalSize);
    };

} // namespace CodexQuotaBar
