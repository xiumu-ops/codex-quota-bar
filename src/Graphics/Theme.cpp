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
        return p;
    }

} // namespace CodexQuotaBar
