#include "Graphics/Theme.h"

namespace CodexQuotaBar {

    ThemePalette ThemePalette::Light() {
        ThemePalette p;
        p.surface = D2D1::ColorF(0xFFFFFF);            // 纯白卡片底
        p.statsCardBg = D2D1::ColorF(0xFAFAFA);        // 子卡片极淡灰（与纯白底区分层次）
        p.statsCardBorder = D2D1::ColorF(0xE0E0E0);    // 浅灰子卡片边框
        p.text = D2D1::ColorF(0x404040);               // 柔和中性深灰主文字
        p.muted = D2D1::ColorF(0x757575);              // 浅灰辅助文字
        p.trackBg = D2D1::ColorF(0xF7F7F7);            // 进度条底色
        p.progressBlue = D2D1::ColorF(0x757575);       // 未返回占位：浅灰
        p.progressGreen = D2D1::ColorF(0x159957);      // 充足：绿
        p.progressYellow = D2D1::ColorF(0xD9A900);     // 中量：加深黄
        p.progressOrange = D2D1::ColorF(0xD76B26);     // 低中量：橙
        p.progressRed = D2D1::ColorF(0xC00000);        // 告急：红
        p.border = D2D1::ColorF(0xFFFFFF);             // 纯白外描边
        p.divider = D2D1::ColorF(0xE0E0E0);            // 分隔线（与子卡片边框同色）
        p.chevron = D2D1::ColorF(0x5F5F5F);            // 中灰折叠箭头
        p.syncSuccess = D2D1::ColorF(0x159957);        // 同步成功：绿
        p.syncIdle = D2D1::ColorF(0xA8A8A8);           // 尚未同步：浅灰
        p.syncBusy = D2D1::ColorF(0xD9A900);           // 正在同步：加深黄
        p.menuHover = D2D1::ColorF(0xF0F0F0);          // 菜单悬停背景
        p.menuDivider = D2D1::ColorF(0xEAEAEA);        // 菜单分隔线
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
            else if (name == L"OuterBorder") p.border = color;
            else if (name == L"Divider") p.divider = color;
            else if (name == L"Chevron") p.chevron = color;
            else if (name == L"SyncSuccess") p.syncSuccess = color;
            else if (name == L"SyncIdle") p.syncIdle = color;
            else if (name == L"SyncBusy") p.syncBusy = color;
            else if (name == L"MenuHover") p.menuHover = color;
            else if (name == L"MenuDivider") p.menuDivider = color;
        }
        return p;
    }

} // namespace CodexQuotaBar
