#pragma once

#include "Graphics/D2DCommon.h"

namespace CodexQuotaBar {

    struct ThemePalette {
        D2D1_COLOR_F surface;
        D2D1_COLOR_F statsCardBg;
        D2D1_COLOR_F statsCardBorder;
        D2D1_COLOR_F text;
        D2D1_COLOR_F muted;
        D2D1_COLOR_F trackBg;
        D2D1_COLOR_F progressBlue;
        D2D1_COLOR_F progressGreen;
        D2D1_COLOR_F progressYellow;
        D2D1_COLOR_F progressOrange;
        D2D1_COLOR_F progressRed;
        D2D1_COLOR_F border;
        D2D1_COLOR_F divider;
        D2D1_COLOR_F chevron;
        D2D1_COLOR_F syncSuccess;
        D2D1_COLOR_F syncIdle;
        D2D1_COLOR_F syncBusy;

        // 淡青白主题：纯白卡片底、中性深灰主文字、浅灰辅助文字、红橙黄绿状态梯度
        static ThemePalette Light();
    };

} // namespace CodexQuotaBar
