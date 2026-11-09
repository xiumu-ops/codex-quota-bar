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
#include <optional>
#include <stop_token>

namespace CodexQuotaBar {

    struct TaskbarMetrics {
        HWND hTaskbar = NULL;
        RECT taskbarRect = {};
        RECT monitorRect = {};
        RECT workAreaRect = {};
        float dpiScale = 1.0f;
        bool isHorizontal = true;
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

        static inline POINT HoldAtMonitorSeam(
            POINT position, SIZE size, const RECT& current, const RECT& next, LONG margin)
        {
            const POINT center = { position.x + size.cx / 2, position.y + size.cy / 2 };
            const bool right = current.right <= next.left;
            const bool left = current.left >= next.right;
            const bool below = current.bottom <= next.top;
            const bool above = current.top >= next.bottom;
            if ((right && center.x < next.left + margin) ||
                (left && center.x >= next.right - margin) ||
                (below && center.y < next.top + margin) ||
                (above && center.y >= next.bottom - margin)) {
                // Keep only the center on the old side. The rest of the window
                // can straddle the seam while the cursor travels the margin.
                if (right) position.x = (std::min)(position.x, current.right - 1 - size.cx / 2);
                if (left) position.x = (std::max)(position.x, current.left + 1 - size.cx / 2);
                if (below) position.y = (std::min)(position.y, current.bottom - 1 - size.cy / 2);
                if (above) position.y = (std::max)(position.y, current.top + 1 - size.cy / 2);
            }
            return position;
        }

        static inline RECT NormalizeTaskbarRect(
            RECT raw, const RECT& monitor, bool horizontal, LONG minimum, LONG fallback)
        {
            RECT visible = {};
            IntersectRect(&visible, &raw, &monitor);
            const LONG rawThickness = horizontal ? raw.bottom - raw.top : raw.right - raw.left;
            const LONG visibleThickness = horizontal
                ? visible.bottom - visible.top : visible.right - visible.left;
            if (visibleThickness == rawThickness && visibleThickness >= minimum) return visible;

            const LONG thickness = (std::min)(rawThickness >= minimum ? rawThickness : fallback,
                horizontal ? monitor.bottom - monitor.top : monitor.right - monitor.left);
            if (horizontal) {
                const bool top = raw.top + rawThickness / 2 < monitor.top + (monitor.bottom - monitor.top) / 2;
                return top ? RECT{ monitor.left, monitor.top, monitor.right, monitor.top + thickness }
                           : RECT{ monitor.left, monitor.bottom - thickness, monitor.right, monitor.bottom };
            }
            const bool left = raw.left + rawThickness / 2 < monitor.left + (monitor.right - monitor.left) / 2;
            return left ? RECT{ monitor.left, monitor.top, monitor.left + thickness, monitor.bottom }
                        : RECT{ monitor.right - thickness, monitor.top, monitor.right, monitor.bottom };
        }

        // System query and composite detection
        static TaskbarMetrics GetTaskbarMetrics(POINT anchor, float fallbackDpiScale);
        static int GetTaskbarAlignment();
        static std::optional<RECT> ProbeWeatherWidget(const TaskbarMetrics& metrics, std::stop_token stop = {});
        static WeatherWidgetPlacement FindWeatherWidgetPlacement(
            SIZE miniPhysicalSize, const TaskbarMetrics& metrics, const std::optional<RECT>& widgetRect);
    };

} // namespace CodexQuotaBar
