#pragma once

#include "Common.h"

struct Theme {
    D2D1_COLOR_F fg{};
    D2D1_COLOR_F fgSecondary{};
    D2D1_COLOR_F fgTertiary{};
    D2D1_COLOR_F bg{};
    D2D1_COLOR_F input{};
    D2D1_COLOR_F secondary{};
    D2D1_COLOR_F primary{};
    D2D1_COLOR_F primaryFg{};
    D2D1_COLOR_F canvas{};
    D2D1_COLOR_F border{};
    D2D1_COLOR_F accent{};
    D2D1_COLOR_F accentFg{};
    D2D1_COLOR_F success{};
    D2D1_COLOR_F contentBorder{};
    D2D1_COLOR_F card{};
    bool dark = false;
    int index = 0;

    void Load(const SnowAirHost* host, void* user);
    /* 弹窗取消按钮的统一底色（普通/悬停），编辑卡片和信封弹窗复用。 */
    D2D1_COLOR_F PopupCancelBg(bool hover) const;
    static Theme Fallback(bool dark);
    static Theme RackTheme(int index);
    static Theme MenuStyle(bool dark);
    static Theme HostUi(const SnowAirHost* host, void* user);
};
