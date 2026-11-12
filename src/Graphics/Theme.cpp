#include "Graphics/Theme.h"

namespace CodexQuotaBar {

    ThemePalette ThemePalette::Light() {
        ThemePalette p;
        p.surface = D2D1::ColorF(0xFFFFFF);
        p.statsCardBg = D2D1::ColorF(0xF5F7F6);
        p.statsCardBorder = p.statsCardBg;
        p.text = D2D1::ColorF(0x242C29);
        p.muted = D2D1::ColorF(0x626E68);
        p.trackBg = D2D1::ColorF(0xE9EEEB);
        p.progressBlue = p.muted;
        p.progressGreen = D2D1::ColorF(0x20945E);
        p.progressYellow = D2D1::ColorF(0xB99637);
        p.progressOrange = D2D1::ColorF(0xC78043);
        p.progressRed = D2D1::ColorF(0xCA4D54);
        p.progressText = { D2D1::ColorF(0x146B42), D2D1::ColorF(0x725713),
                           D2D1::ColorF(0x934A1A), D2D1::ColorF(0x9C3035) };
        p.miniFill = { D2D1::ColorF(0xCBE6D5), D2D1::ColorF(0xEEE3C3),
                      D2D1::ColorF(0xF1DCC7), D2D1::ColorF(0xEFC5C7) };
        p.border = D2D1::ColorF(0xE6EBE8);
        p.divider = D2D1::ColorF(0xEDF0EE);
        p.chevron = p.muted;
        p.syncSuccess = p.progressGreen;
        p.syncIdle = D2D1::ColorF(0xA8A8A8);
        p.syncBusy = p.progressYellow;
        p.menuHover = D2D1::ColorF(0xEEF3F0);
        p.menuDivider = p.divider;
        return p;
    }

    ThemePalette ThemePalette::Dark() {
        ThemePalette p = Light();
        p.surface = D2D1::ColorF(0x202426);
        p.statsCardBg = D2D1::ColorF(0x292F2C);
        p.statsCardBorder = p.statsCardBg;
        p.text = D2D1::ColorF(0xEDF2EF);
        p.muted = D2D1::ColorF(0xABB9B1);
        p.trackBg = D2D1::ColorF(0x353E38);
        p.progressBlue = p.muted;
        p.progressGreen = D2D1::ColorF(0x65BE8A);
        p.progressYellow = D2D1::ColorF(0xC6AD61);
        p.progressOrange = D2D1::ColorF(0xD7A576);
        p.progressRed = D2D1::ColorF(0xE88B92);
        p.progressText = { D2D1::ColorF(0xA4E8BA), D2D1::ColorF(0xF0DA99),
                           D2D1::ColorF(0xF9CEAC), D2D1::ColorF(0xFFC1C4) };
        p.miniFill = { D2D1::ColorF(0x315442), D2D1::ColorF(0x504A32),
                      D2D1::ColorF(0x584333), D2D1::ColorF(0x61373B) };
        p.border = D2D1::ColorF(0x3B443E);
        p.divider = D2D1::ColorF(0x363E38);
        p.chevron = p.muted;
        p.syncSuccess = p.progressGreen;
        p.syncBusy = p.progressYellow;
        p.menuHover = D2D1::ColorF(0x343E37);
        p.menuDivider = p.divider;
        return p;
    }

    ThemePalette ThemePalette::Custom(const AppearanceSettings& settings) {
        ThemePalette p = Light();
        for (const auto& [name, value] : settings.colors) {
            uint32_t rgb = 0;
            if (!TryParseAppearanceColor(value, rgb)) continue;
            const D2D1_COLOR_F color = D2D1::ColorF(rgb);
            if (name == L"Surface") p.surface = color;
            else if (name == L"StatsCardBackground") p.statsCardBg = color;
            else if (name == L"StatsCardBorder") p.statsCardBorder = color;
            else if (name == L"Text") p.text = color;
            else if (name == L"MutedText") p.muted = color;
            else if (name == L"TrackBackground") p.trackBg = color;
            else if (name == L"Unavailable") p.progressBlue = color;
            else if (name == L"ProgressHigh") p.progressGreen = color;
            else if (name == L"ProgressMedium") p.progressYellow = color;
            else if (name == L"ProgressLow") p.progressOrange = color;
            else if (name == L"ProgressCritical") p.progressRed = color;
            else if (name == L"ProgressHighText") p.progressText[0] = color;
            else if (name == L"ProgressMediumText") p.progressText[1] = color;
            else if (name == L"ProgressLowText") p.progressText[2] = color;
            else if (name == L"ProgressCriticalText") p.progressText[3] = color;
            else if (name == L"OuterBorder") p.border = color;
            else if (name == L"Divider") p.divider = color;
            else if (name == L"Chevron") p.chevron = color;
            else if (name == L"SyncSuccess") p.syncSuccess = color;
            else if (name == L"SyncIdle") p.syncIdle = color;
            else if (name == L"SyncBusy") p.syncBusy = color;
            else if (name == L"MenuHover") p.menuHover = color;
            else if (name == L"MenuDivider") p.menuDivider = color;
        }
        const ThemePalette defaults = Light();
        const D2D1_COLOR_F accents[] = {
            p.progressGreen, p.progressYellow, p.progressOrange, p.progressRed
        };
        const D2D1_COLOR_F defaultAccents[] = {
            defaults.progressGreen, defaults.progressYellow,
            defaults.progressOrange, defaults.progressRed
        };
        const auto sameColor = [](const D2D1_COLOR_F& a, const D2D1_COLOR_F& b) {
            return a.r == b.r && a.g == b.g && a.b == b.b;
        };
        for (size_t index = 0; index < p.miniFill.size(); ++index) {
            if (!sameColor(accents[index], defaultAccents[index]) ||
                !sameColor(p.trackBg, defaults.trackBg)) {
                const auto& accent = accents[index];
                p.miniFill[index] = D2D1::ColorF(
                    p.trackBg.r * 0.8f + accent.r * 0.2f,
                    p.trackBg.g * 0.8f + accent.g * 0.2f,
                    p.trackBg.b * 0.8f + accent.b * 0.2f);
            }
        }
        return p;
    }

} // namespace CodexQuotaBar
