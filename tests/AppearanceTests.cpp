#include "Core/Appearance.h"

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

    std::cout << "Appearance tests passed\n";
    return 0;
}
