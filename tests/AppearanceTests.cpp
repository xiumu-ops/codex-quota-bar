#include "Core/Appearance.h"
#include "Core/Layout.h"
#include "Core/Settings.h"
#include "UI/TaskbarPlacement.h"
#include "Graphics/Theme.h"

#include <cstdlib>
#include <iostream>
#include <string>

using namespace CodexQuotaBar;

namespace {

    void Expect(bool condition, const char* message) {
        if (!condition) {
            std::cerr << "FAIL: " << message << '\n';
            std::exit(1);
        }
    }

} // namespace

int main() {
    uint32_t rgb = 0;
    Expect(TryParseAppearanceColor(L"#159957", rgb), "valid RGB color");
    Expect(rgb == 0x159957, "parsed RGB value");
    Expect(TryParseAppearanceColor(L"#aBcDeF", rgb), "mixed-case RGB color");
    Expect(rgb == 0xABCDEF, "mixed-case parsed RGB value");
    Expect(!TryParseAppearanceColor(L"159957", rgb), "missing hash rejected");
    Expect(!TryParseAppearanceColor(L"#FFF", rgb), "short color rejected");
    Expect(!TryParseAppearanceColor(L"#GG0000", rgb), "non-hex color rejected");
    Expect(!TryParseAppearanceColor(L"#11223344", rgb), "alpha color rejected");

    Expect(IsSupportedAppearanceColor(L"Surface"), "known color field");
    Expect(IsSupportedAppearanceColor(L"MenuHover"), "menu color field");
    Expect(!IsSupportedAppearanceColor(L"surface"), "color fields are case-sensitive");
    Expect(!IsSupportedAppearanceColor(L"Accent"), "unknown color field rejected");
    Expect(DefaultAppearanceColors().size() == 23, "complete default color template");
    Expect(IsSupportedAppearanceColor(L"ProgressMediumText"), "separate percentage text color");
    const auto luminance = [](const D2D1_COLOR_F& color) {
        const auto linear = [](float channel) {
            return channel <= 0.04045f ? channel / 12.92f
                : std::pow((channel + 0.055f) / 1.055f, 2.4f);
        };
        return 0.2126f * linear(color.r) + 0.7152f * linear(color.g) + 0.0722f * linear(color.b);
    };
    for (const auto& palette : { ThemePalette::Light(), ThemePalette::Dark() }) {
        for (size_t index = 0; index < palette.progressText.size(); ++index) {
            const float text = luminance(palette.progressText[index]);
            for (const auto& background : { palette.trackBg, palette.miniFill[index], palette.surface }) {
                const float fill = luminance(background);
                const float contrast = ((std::max)(text, fill) + 0.05f) / ((std::min)(text, fill) + 0.05f);
                Expect(contrast >= 4.5f, "default percentage text contrasts with both filled and empty areas");
            }
        }
    }

    Expect(IsValidFontFamilyName(L"Microsoft YaHei UI"), "valid font family");
    Expect(IsValidFontFamilyName(L"Segoe UI"), "valid Segoe UI font");
    Expect(IsValidFontFamilyName(L"HarmonyOS Sans SC"), "valid HarmonyOS font");
    Expect(IsValidFontFamilyName(L"Cascadia Code"), "valid Cascadia Code font");
    Expect(IsValidFontFamilyName(L"Consolas"), "valid Consolas font");
    Expect(IsValidFontFamilyName(L"JetBrains Mono"), "valid JetBrains Mono font");
    Expect(IsValidFontFamilyName(L"思源黑体"), "unicode font family");
    Expect(!IsValidFontFamilyName(L""), "empty font family rejected");
    Expect(!IsValidFontFamilyName(L"Bad\nFont"), "control character rejected");
    Expect(!IsValidFontFamilyName(std::wstring(129, L'A')), "long font family rejected");

    AppearanceSettings defaultAppearance;
    Expect(defaultAppearance.mode == AppearanceMode::Light, "Light is the default theme name");
    Expect(defaultAppearance.fontFamily == L"Microsoft YaHei UI", "default font family is Microsoft YaHei UI");

    Expect(IsValidBackgroundTransparency(0), "opaque background accepted");
    Expect(IsValidBackgroundTransparency(90), "maximum transparency accepted");
    Expect(!IsValidBackgroundTransparency(-1), "negative transparency rejected");
    Expect(!IsValidBackgroundTransparency(91), "excessive transparency rejected");
    int transparency = -1;
    Expect(TryParseBackgroundTransparency(30.0, transparency), "integer number parsed");
    Expect(transparency == 30, "parsed transparency value");
    Expect(!TryParseBackgroundTransparency(30.5, transparency), "fraction rejected");
    Expect(!IsSupportedUserScale(1e308) && !IsSupportedUserScale(1e20),
           "out-of-range user scale is rejected before integer conversion");

    const RECT monitor = { -1920, 0, 0, 1080 };
    const RECT hiddenBottom = TaskbarPlacement::NormalizeTaskbarRect(
        { -1920, 1078, 0, 1150 }, monitor, true, 12, 72);
    const RECT expectedBottom = { -1920, 1008, 0, 1080 };
    Expect(EqualRect(&hiddenBottom, &expectedBottom),
           "bottom auto-hide taskbar is restored to its full thickness");
    const RECT hiddenLeft = TaskbarPlacement::NormalizeTaskbarRect(
        { -1920, 0, -1918, 1080 }, monitor, false, 12, 72);
    const RECT expectedLeft = { -1920, 0, -1848, 1080 };
    Expect(EqualRect(&hiddenLeft, &expectedLeft),
           "vertical auto-hide sliver uses the monitor's DPI-scaled thickness");

    QuotaSnapshot noQuota;
    Expect(MiniQuotaSlotCount(noQuota) == 1, "empty mini bar keeps one slot");
    Expect(MiniBarLogicalWidth(noQuota) == MINI_SINGLE_WIDTH,
           "empty mini bar uses the accepted half width");

    QuotaSnapshot oneQuota;
    oneQuota.window.available = true;
    Expect(MiniQuotaSlotCount(oneQuota) == 1, "single quota uses one mini slot");
    Expect(MiniBarLogicalWidth(oneQuota) == MINI_SINGLE_WIDTH,
           "single quota uses half-width mini bar");

    QuotaSnapshot weeklyOnly;
    weeklyOnly.weekly.available = true;
    Expect(MiniBarLogicalWidth(weeklyOnly) == MINI_SINGLE_WIDTH,
           "weekly-only quota uses half-width mini bar");

    QuotaSnapshot twoQuotas;
    twoQuotas.window.available = true;
    twoQuotas.weekly.available = true;
    Expect(MiniQuotaSlotCount(twoQuotas) == 2, "two quotas use two mini slots");
    Expect(MiniBarLogicalWidth(twoQuotas) == MINI_DUAL_WIDTH,
           "two quotas use full-width mini bar");
    Expect(MiniBarPhysicalWidth(twoQuotas, 0.75f) == 84,
           "75% dual mini bar combines two equal physical slots");
    Expect(MiniBarPhysicalWidth(twoQuotas, 1.125f) == 126,
           "112.5% dual mini bar combines two equal physical slots");
    Expect(MiniBarPhysicalWidth(twoQuotas, 1.5625f) == 178,
           "fractional DPI and user scaling preserve equal physical slots");
    Expect(MiniBarPhysicalWidth(oneQuota, 1.5625f) == 88,
           "single mini outer width stays even at fractional scale");

    const MiniBarMetrics miniAt80 = ScaledMiniBarMetrics(0.8f);
    Expect(!ShouldUseSmallWindowCorner(true, 0.8f),
           "mini bar maintains unified standard round corner preference");
    Expect(!ShouldUseSmallWindowCorner(false, 0.8f),
           "standard bar maintains unified standard round corner preference");
    Expect(miniAt80.outerCornerRadius == 8.0f,
           "80% mini outer corner matches unified 96-DPI DWMWCP_ROUND (8px)");
    Expect(miniAt80.contentInset == 4.0f,
           "mini content inset follows 80% user scale");
    Expect(miniAt80.pillCornerRadius == 4.0f,
           "80% mini pill corner stays concentric with 8px outer corner");
    Expect(miniAt80.centerGap == 4.0f,
           "mini center gap rounds to even physical pixels at 80%");
    Expect(miniAt80.outerBorderThickness == 2.0f,
           "mini border is even and pixel-aligned at 80%");

    const MiniBarMetrics miniAt100 = ScaledMiniBarMetrics(1.0f);
    Expect(miniAt100.outerCornerRadius == 8.0f,
           "100% mini outer corner matches 96-DPI DWMWCP_ROUND (8px)");
    Expect(miniAt100.contentInset == 4.0f,
           "100% mini content inset is four pixels");
    Expect(miniAt100.pillCornerRadius == 4.0f,
           "100% mini pill corner stays concentric with 8px outer corner");
    Expect(miniAt100.centerGap == 4.0f,
           "100% center gap is four empty pixels");
    Expect(MiniDualSlotWidth(112.0f, miniAt100) == 50.0f,
           "100% dual bar has two equal fifty-pixel slots");

    const MiniBarMetrics miniAt120 = ScaledMiniBarMetrics(1.2f);
    Expect(miniAt120.outerCornerRadius == 8.0f,
           "120% user scale at 96 DPI keeps 8px outer corner aligned with DWM");
    Expect(miniAt120.contentInset == 6.0f,
           "mini content inset follows 120% user scale");
    Expect(miniAt120.pillCornerRadius == 2.0f,
           "120% mini pill corner stays concentric with 8px outer corner");

    const MiniBarMetrics miniHiDpiSmall = ScaledMiniBarMetricsForWindow(1.5f, 0.8f, true);
    Expect(miniHiDpiSmall.outerCornerRadius == 12.0f,
           "150% DPI + 80% mini outer corner matches unified DWMWCP_ROUND (8px * 1.5 = 12px)");
    Expect(miniHiDpiSmall.contentInset == 6.0f,
           "150% DPI + 80% mini inset rounds to 6px");
    Expect(miniHiDpiSmall.pillCornerRadius == 6.0f,
           "150% DPI + 80% mini pill stays concentric (12 - 6 = 6px)");

    const MiniBarMetrics miniHiDpiLarge = ScaledMiniBarMetricsForWindow(2.0f, 1.2f, true);
    Expect(miniHiDpiLarge.outerCornerRadius == 16.0f,
           "200% DPI + 120% mini outer corner matches DWMWCP_ROUND (16px)");
    Expect(miniHiDpiLarge.contentInset == 10.0f,
           "200% DPI + 120% mini inset rounds to 10px");
    Expect(miniHiDpiLarge.pillCornerRadius == 6.0f,
           "200% DPI + 120% mini pill stays concentric (16 - 10 = 6px)");

    const float monitorDpis[] = { 1.0f, 1.25f, 1.5f, 1.75f, 2.0f };
    struct MiniDpiCase {
        float monitorScale;
        LONG dualWidth;
        LONG height;
        LONG taskbarHeight;
    };
    const MiniDpiCase miniDpiCases[] = {
        { 1.0f, 134, 34, 48 },
        { 1.25f, 170, 42, 60 },
        { 1.5f, 204, 50, 72 },
        { 1.75f, 236, 60, 84 },
    };
    for (const auto& test : miniDpiCases) {
        const float uiScale = test.monitorScale * 1.2f;
        const SIZE size = { MiniBarPhysicalWidth(twoQuotas, uiScale), MiniBarPhysicalHeight(uiScale) };
        Expect(size.cx == test.dualWidth && size.cy == test.height,
               "mini bar has exact physical dimensions at each target display DPI");

        // A monitor to the left of the primary has negative physical coordinates.
        const RECT taskbar = { -1920, 1080 - test.taskbarHeight, 0, 1080 };
        const RECT restored = TaskbarPlacement::NormalizeTaskbarRect(
            { -1920, 1078, 0, 1080 }, monitor, true, ScaleEven(24.0f, test.monitorScale),
            ScaleEven(48.0f, test.monitorScale));
        Expect(EqualRect(&restored, &taskbar),
               "hidden taskbar thickness matches the display scale");
        const RECT slot = { -1900, taskbar.top, -1600, taskbar.bottom };
        const POINT dock = TaskbarPlacement::ComputeDockPosition(slot, taskbar, true, size);
        Expect(dock.x - slot.left == slot.right - (dock.x + size.cx) &&
                   dock.y - taskbar.top == taskbar.bottom - (dock.y + size.cy),
               "taskbar docking keeps equal physical margins at each target DPI");
    }
    for (float dpi : monitorDpis) {
        for (float userScale : USER_SCALE_LEVELS) {
            const float uiScale = dpi * userScale;
            Expect(MiniBarPhysicalWidth(oneQuota, uiScale) % 2 == 0 &&
                       MiniBarPhysicalWidth(twoQuotas, uiScale) % 2 == 0 &&
                       MiniBarPhysicalHeight(uiScale) % 2 == 0,
                   "mini bar dimensions stay on even physical pixels across user and display scales");
            const auto metrics = ScaledMiniBarMetricsForWindow(dpi, userScale);
            const float slotWidth = MiniDualSlotWidth(
                static_cast<float>(MiniBarPhysicalWidth(twoQuotas, uiScale)), metrics);
            Expect(slotWidth > 0.0f && std::floor(slotWidth) == slotWidth &&
                       static_cast<int>(slotWidth) % 2 == 0 &&
                       static_cast<int>(metrics.contentInset) % 2 == 0 &&
                       static_cast<int>(metrics.pillCornerRadius) % 2 == 0 &&
                       static_cast<int>(metrics.outerCornerRadius) % 2 == 0 &&
                       static_cast<int>(metrics.outerBorderThickness) % 2 == 0 &&
                       static_cast<int>(metrics.centerGap) % 2 == 0 &&
                       MiniBarPhysicalHeight(uiScale) > metrics.contentInset * 2.0f,
                   "mini content remains positive, equal and pixel-aligned at every scale");
            float previousFill = 0.0f;
            for (int percent = 0; percent <= 100; ++percent) {
                const float fill = RoundEvenPixel(slotWidth * percent / 100.0f);
                Expect(fill >= previousFill && fill <= slotWidth && std::fmod(fill, 2.0f) == 0.0f,
                       "progress stays monotonic, bounded and even at every percentage");
                previousFill = fill;
            }
            const float cardWidth = SnapPx(static_cast<float>(BAR_WIDTH), uiScale) -
                2.0f * SnapPx(StandardLayout::kPadX, uiScale);
            float previousColumn = 0.0f;
            for (int column = 1; column <= 3; ++column) {
                const float edge = RoundEvenPixel(cardWidth * column / 3.0f);
                Expect(edge > previousColumn && std::fmod(edge - previousColumn, 2.0f) == 0.0f,
                       "expanded columns share aligned even edges without cumulative rounding");
                previousColumn = edge;
            }
            const int expandedH = ExpandedPhysicalHeight(uiScale);
            const int bottomCardEnd =
                static_cast<int>(SnapPx(static_cast<float>(COLLAPSED_HEIGHT), uiScale)) +
                static_cast<int>(SnapPx(StandardLayout::kCardGapTop, uiScale)) +
                static_cast<int>(SnapPx(StandardLayout::kCardHeight, uiScale)) * 2 +
                static_cast<int>(SnapPx(StandardLayout::kCardGap, uiScale));
            Expect(expandedH - bottomCardEnd ==
                       static_cast<int>(SnapPx(StandardLayout::kCardGapBottom, uiScale)),
                   "expanded physical height preserves exact bottom card margin at every scale");
        }
    }

    Expect(IsSupportedUserScale(0.2), "minimum UI scale accepted (20%)");
    Expect(IsSupportedUserScale(1.8), "maximum UI scale accepted (180%)");
    Expect(IsSupportedUserScale(1.0), "default UI scale accepted (100%)");
    Expect(IsSupportedUserScale(0.8), "80% UI scale accepted");
    Expect(IsSupportedUserScale(1.2), "120% UI scale accepted");
    Expect(!IsSupportedUserScale(0.75), "unsupported fractional UI scale rejected");
    Expect(!IsSupportedUserScale(0.3), "odd 10% interval UI scale rejected");
    Expect(!IsSupportedUserScale(0.1), "below 20% UI scale rejected");
    Expect(!IsSupportedUserScale(2.0), "above 180% UI scale rejected");

    int interval = 0;
    Expect(TryParseSupportedRefreshInterval(30.0, interval) && interval == 30,
           "supported refresh interval accepted");
    Expect(!TryParseSupportedRefreshInterval(3.0, interval),
           "unsupported refresh interval rejected");
    Expect(!TryParseSupportedRefreshInterval(5.5, interval),
           "fractional refresh interval rejected");

    int integer = 0;
    Expect(TryParseConfigInteger(-120.0, integer) && integer == -120,
           "integer window coordinate accepted");
    Expect(!TryParseConfigInteger(2.7, integer),
           "fractional config integer rejected");

    // TaskbarPlacement 任务栏天气组件贴靠与居中几何测试
    const RECT tbRect = { 0, 1032, 1920, 1080 }; // 标准 1080p 底部 48px 任务栏
    const RECT slotLeft = { 12, 1032, 152, 1080 }; // Windows 11 居中对齐时左侧 140px 天气槽
    const SIZE miniDual = { 112, 28 };
    const SIZE miniSingle = { 56, 28 };

    // 1. 实测组件存在时：居中贴靠 (centerInSlot = true)
    const POINT dockDualCentered = TaskbarPlacement::ComputeDockPosition(slotLeft, tbRect, true, miniDual, true);
    Expect(dockDualCentered.y == 1042, "mini dual bar vertically centered in 48px taskbar (1032 + (48-28)/2 = 1042)");
    Expect(dockDualCentered.x == 26, "mini dual bar horizontally centered inside 140px weather slot (12 + (140-112)/2 = 26)");

    const POINT dockSingleCentered = TaskbarPlacement::ComputeDockPosition(slotLeft, tbRect, true, miniSingle, true);
    Expect(dockSingleCentered.y == 1042, "mini single bar vertically centered in taskbar");
    Expect(dockSingleCentered.x == 54, "mini single bar horizontally centered inside weather slot (12 + (140-56)/2 = 54)");

    // 2. 几何回退模式：统一左对齐锚定 (centerInSlot = false)
    const POINT dockDualAnchored = TaskbarPlacement::ComputeDockPosition(slotLeft, tbRect, true, miniDual, false);
    Expect(dockDualAnchored.y == 1042, "mini dual bar vertically centered in taskbar");
    Expect(dockDualAnchored.x == 12, "mini dual bar left-anchored to weather slot left edge (12px)");

    const POINT dockSingleAnchored = TaskbarPlacement::ComputeDockPosition(slotLeft, tbRect, true, miniSingle, false);
    Expect(dockSingleAnchored.y == 1042, "mini single bar vertically centered in taskbar");
    Expect(dockSingleAnchored.x == 12, "mini single bar left-anchored to weather slot left edge (12px)");

    // 3. 垂直任务栏测试
    const RECT tbVertical = { 0, 0, 60, 1080 };
    const RECT slotVertical = { 0, 12, 60, 152 };
    const POINT dockVertical = TaskbarPlacement::ComputeDockPosition(slotVertical, tbVertical, false, miniDual, true);
    Expect(dockVertical.x == 0, "vertical taskbar aligns horizontally when slot <= bar width");
    Expect(dockVertical.y == 12 + (140 - 28) / 2, "vertical taskbar centers vertically inside slot");

    // 4. 屏幕监视器边界钳制测试
    const RECT monRect = { 0, 0, 1920, 1080 };
    const POINT inside = TaskbarPlacement::ClampToMonitorBounds(dockDualCentered, miniDual, monRect);
    Expect(inside.x == dockDualCentered.x && inside.y == dockDualCentered.y, "in-bounds dock position is preserved");

    const POINT offLeft = { -50, 1042 };
    Expect(TaskbarPlacement::ClampToMonitorBounds(offLeft, miniDual, monRect).x == 0,
           "offscreen left clamped to monitor left");

    const POINT offRight = { 1900, 1042 };
    Expect(TaskbarPlacement::ClampToMonitorBounds(offRight, miniDual, monRect).x == 1920 - 112,
           "offscreen right clamped to monitor right minus bar width");

    const POINT offTop = { 26, -20 };
    Expect(TaskbarPlacement::ClampToMonitorBounds(offTop, miniDual, monRect).y == 0,
           "offscreen top clamped to monitor top");

    const POINT offBottom = { 26, 1200 };
    Expect(TaskbarPlacement::ClampToMonitorBounds(offBottom, miniDual, monRect).y == 1080 - 28,
           "offscreen bottom clamped to monitor bottom");

    // The adjacent display starts at y=500. After dragging, the window
    // must fit on the selected work area instead of spanning the gap.
    const POINT inDisplayGap = { 1780, 425 };
    const RECT leftWorkArea = { 0, 0, 1920, 1040 };
    const POINT settled = TaskbarPlacement::ClampToMonitorBounds(
        inDisplayGap, SIZE{ BAR_WIDTH, COLLAPSED_HEIGHT }, leftWorkArea);
    Expect(settled.x == 1540 && settled.y == 425,
           "settled window fits entirely on the selected offset display");

    const RECT rightMonitor = { 1920, 0, 3840, 1080 };
    const RECT upperMonitor = { 0, -1080, 1920, 0 };
    const SIZE standardBar = { BAR_WIDTH, COLLAPSED_HEIGHT };
    for (const auto& test : miniDpiCases) {
        const LONG margin = Scale(16.0f, test.monitorScale);
        const POINT crossing = { 1731, 30 };
        const POINT held = TaskbarPlacement::HoldAtMonitorSeam(
            crossing, standardBar, monRect, rightMonitor, margin);
        Expect(held.x == 1729 && held.y == crossing.y,
               "horizontal seam holds the center without pushing the whole bar back");
        const POINT pastMargin = { 1730 + margin, 30 };
        const POINT released = TaskbarPlacement::HoldAtMonitorSeam(
            pastMargin, standardBar, monRect, rightMonitor, margin);
        Expect(released.x == pastMargin.x && released.y == pastMargin.y,
               "cursor motion resumes at the monitor's scaled seam margin");
        const POINT crossingUp = { 42, -COLLAPSED_HEIGHT / 2 - 1 };
        const POINT heldUp = TaskbarPlacement::HoldAtMonitorSeam(
            crossingUp, standardBar, monRect, upperMonitor, margin);
        Expect(heldUp.x == crossingUp.x && heldUp.y == -COLLAPSED_HEIGHT / 2 + 1,
               "vertical seam holds the center without changing horizontal position");
    }

    const SIZE oversized = { 2500, 1200 };
    const POINT clampedOver = TaskbarPlacement::ClampToMonitorBounds(POINT{ 100, 100 }, oversized, monRect);
    Expect(clampedOver.x == 0 && clampedOver.y == 0,
           "oversized dimensions clamped to monitor top-left");

    // 5. 吸附检测测试
    Expect(TaskbarPlacement::IsNearDockPosition(POINT{ 26, 1042 }, dockDualCentered, 24),
           "exact dock position triggers snap");
    Expect(TaskbarPlacement::IsNearDockPosition(POINT{ 35, 1050 }, dockDualCentered, 24),
           "within snap threshold triggers snap");
    Expect(!TaskbarPlacement::IsNearDockPosition(POINT{ 70, 1042 }, dockDualCentered, 24),
           "beyond snap threshold does not trigger snap");

    std::cout << "Appearance and TaskbarPlacement tests passed\n";
    return 0;
}
