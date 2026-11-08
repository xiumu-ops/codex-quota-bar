#include "Core/Appearance.h"
#include "Core/Layout.h"
#include "Core/Settings.h"

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
    Expect(DefaultAppearanceColors().size() == 19, "complete default color template");

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
    Expect(defaultAppearance.fontFamily == L"Microsoft YaHei UI", "default font family is Microsoft YaHei UI");

    Expect(IsValidBackgroundTransparency(0), "opaque background accepted");
    Expect(IsValidBackgroundTransparency(90), "maximum transparency accepted");
    Expect(!IsValidBackgroundTransparency(-1), "negative transparency rejected");
    Expect(!IsValidBackgroundTransparency(91), "excessive transparency rejected");
    int transparency = -1;
    Expect(TryParseBackgroundTransparency(30.0, transparency), "integer number parsed");
    Expect(transparency == 30, "parsed transparency value");
    Expect(!TryParseBackgroundTransparency(30.5, transparency), "fraction rejected");

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
    Expect(MiniBarPhysicalWidth(twoQuotas, 1.5625f) == 176,
           "fractional DPI and user scaling preserve equal physical slots");
    Expect(MiniBarPhysicalWidth(oneQuota, 1.5625f) == 88,
           "single mini slot matches half of dual width at fractional scale");

    const MiniBarMetrics miniAt75 = ScaledMiniBarMetrics(0.75f);
    Expect(!ShouldUseSmallWindowCorner(true, 0.75f),
           "mini bar maintains unified standard round corner preference");
    Expect(!ShouldUseSmallWindowCorner(false, 0.75f),
           "standard bar maintains unified standard round corner preference");
    Expect(miniAt75.outerCornerRadius == 8.0f,
           "75% mini outer corner matches unified 96-DPI DWMWCP_ROUND (8px)");
    Expect(miniAt75.contentInset == 2.0f,
           "mini content inset follows 75% user scale");
    Expect(miniAt75.pillCornerRadius == 6.0f,
           "75% mini pill corner stays concentric with 8px outer corner");
    Expect(miniAt75.dividerThickness == 1.0f,
           "mini divider remains one visible physical pixel at 75%");
    Expect(miniAt75.outerBorderThickness == 1.0f,
           "mini border remains one visible physical pixel at 75%");
    Expect(MiniDividerHeight(21, miniAt75) == 5.0f,
           "75% divider spans the concentric pill's straight center section");

    const MiniBarMetrics miniAt100 = ScaledMiniBarMetrics(1.0f);
    Expect(miniAt100.outerCornerRadius == 8.0f,
           "100% mini outer corner matches 96-DPI DWMWCP_ROUND (8px)");
    Expect(miniAt100.contentInset == 2.0f,
           "100% mini content inset stays 2px");
    Expect(miniAt100.pillCornerRadius == 6.0f,
           "100% mini pill corner stays concentric with 8px outer corner");
    Expect(MiniDividerHeight(28, miniAt100) == 12.0f,
           "100% divider spans the concentric pill's straight center section");

    const MiniBarMetrics miniAt125 = ScaledMiniBarMetrics(1.25f);
    Expect(miniAt125.outerCornerRadius == 8.0f,
           "125% user scale at 96 DPI keeps 8px outer corner aligned with DWM");
    Expect(miniAt125.contentInset == 3.0f,
           "mini content inset follows 125% user scale");
    Expect(miniAt125.pillCornerRadius == 5.0f,
           "125% mini pill corner stays concentric with 8px outer corner and 3px inset");
    Expect(miniAt125.dividerThickness == 1.0f,
           "mini divider snaps to a physical pixel at 125%");
    Expect(miniAt125.outerBorderThickness == 1.0f,
           "mini border snaps to a physical pixel at 125%");
    Expect(MiniDividerHeight(35, miniAt125) == 19.0f,
           "125% divider spans the concentric pill's straight center section");

    const MiniBarMetrics miniHiDpiSmall = ScaledMiniBarMetricsForWindow(1.5f, 0.75f, true);
    Expect(miniHiDpiSmall.outerCornerRadius == 12.0f,
           "150% DPI + 75% mini outer corner matches unified DWMWCP_ROUND (8px * 1.5 = 12px)");
    Expect(miniHiDpiSmall.contentInset == 2.0f,
           "150% DPI + 75% mini inset rounds to 2px");
    Expect(miniHiDpiSmall.pillCornerRadius == 10.0f,
           "150% DPI + 75% mini pill stays concentric (12 - 2 = 10px)");

    const MiniBarMetrics miniHiDpiLarge = ScaledMiniBarMetricsForWindow(2.0f, 1.25f, true);
    Expect(miniHiDpiLarge.outerCornerRadius == 16.0f,
           "200% DPI + 125% mini outer corner matches DWMWCP_ROUND (16px)");
    Expect(miniHiDpiLarge.contentInset == 5.0f,
           "200% DPI + 125% mini inset rounds to 5px");
    Expect(miniHiDpiLarge.pillCornerRadius == 11.0f,
           "200% DPI + 125% mini pill stays concentric (16 - 5 = 11px)");

    const float monitorDpis[] = { 1.0f, 1.25f, 1.5f, 1.75f, 2.0f };
    for (float dpi : monitorDpis) {
        for (float userScale : USER_SCALE_LEVELS) {
            const float uiScale = dpi * userScale;
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

    Expect(IsSupportedUserScale(0.75), "minimum UI scale accepted");
    Expect(IsSupportedUserScale(1.25), "maximum UI scale accepted");
    Expect(!IsSupportedUserScale(0.8), "unsupported UI scale rejected");
    Expect(!IsSupportedUserScale(2.0), "out-of-menu UI scale rejected");

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

    std::cout << "Appearance tests passed\n";
    return 0;
}
