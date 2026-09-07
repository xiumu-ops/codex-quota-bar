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
    Expect(IsValidFontFamilyName(L"思源黑体"), "unicode font family");
    Expect(!IsValidFontFamilyName(L""), "empty font family rejected");
    Expect(!IsValidFontFamilyName(L"Bad\nFont"), "control character rejected");
    Expect(!IsValidFontFamilyName(std::wstring(129, L'A')), "long font family rejected");

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

    const MiniBarMetrics miniAt75 = ScaledMiniBarMetrics(0.75f);
    Expect(miniAt75.outerCornerRadius == 6.0f,
           "mini outer corner follows 75% user scale");
    Expect(miniAt75.contentInset == 3.0f,
           "mini content inset follows 75% user scale");
    Expect(miniAt75.pillCornerRadius == 3.75f,
           "mini pill corner follows 75% user scale");
    Expect(miniAt75.dividerThickness == 0.75f,
           "mini divider follows 75% user scale");

    const MiniBarMetrics miniAt125 = ScaledMiniBarMetrics(1.25f);
    Expect(miniAt125.outerCornerRadius == 10.0f,
           "mini outer corner follows 125% user scale");
    Expect(miniAt125.contentInset == 5.0f,
           "mini content inset follows 125% user scale");
    Expect(miniAt125.pillCornerRadius == 6.25f,
           "mini pill corner follows 125% user scale");
    Expect(miniAt125.dividerThickness == 1.25f,
           "mini divider follows 125% user scale");

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
