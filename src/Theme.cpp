#include "Theme.h"
#include "Util.h"

namespace {

} /* namespace */

Theme Theme::Fallback(bool dark)
{
    Theme theme;
    theme.dark = dark;
    if (dark) {
        theme.fg = util::Rgb(0xE9EAEE);
        theme.fgSecondary = util::Rgb(0xB9BCC4);
        theme.fgTertiary = util::Rgb(0x8A8E99);
        theme.bg = util::Rgb(0x2A2B2F);
        theme.canvas = util::Rgb(0x1D1E21);
        theme.border = util::Rgb(0x474A52);
        theme.accent = util::Rgb(0x6EA8FF);
        theme.accentFg = util::Rgb(0x0E1116);
        theme.success = util::Rgb(0x5BD18B);
    } else {
        theme.fg = util::Rgb(0x1E2228);
        theme.fgSecondary = util::Rgb(0x59616D);
        theme.fgTertiary = util::Rgb(0x8A929E);
        theme.bg = util::Rgb(0xFFFFFF);
        theme.canvas = util::Rgb(0xF4F6F9);
        theme.border = util::Rgb(0xD6DBE2);
        theme.accent = util::Rgb(0x2F6FEB);
        theme.accentFg = util::Rgb(0xFFFFFF);
        theme.success = util::Rgb(0x1F9D55);
    }
    theme.contentBorder = theme.border;
    theme.card = theme.bg;
    return theme;
}

Theme Theme::RackTheme(int index)
{
    Theme theme;
    const int requested = std::clamp(index, 0, 3);
    theme.index = requested;
    // 界面顺序是 银灰、奶白、木棉、深沉；这里把界面序号映射到原来的色板定义顺序。
    static const int palette[4] = {1, 0, 2, 3};
    switch (palette[requested]) {
    case 1: /* 石墨 */
        theme.dark = true;
        theme.fg = util::Rgb(0xF2F3F5);
        theme.fgSecondary = util::Rgb(0xA7ABB2);
        theme.fgTertiary = util::Rgb(0x777C84);
        theme.bg = util::Rgb(0x1D1F22);
        theme.canvas = util::Rgb(0x131416);
        theme.border = util::Rgb(0x373B40);
        theme.accent = util::Rgb(0x6FA8FF);
        theme.accentFg = util::Rgb(0x0B1220);
        theme.success = util::Rgb(0x5BD18B);
        break;
    case 2: /* 赤陶 */
        theme.dark = false;
        theme.fg = util::Rgb(0x332018);
        theme.fgSecondary = util::Rgb(0x7B6154);
        theme.fgTertiary = util::Rgb(0x9B8779);
        theme.bg = util::Rgb(0xFFF8F0);
        theme.canvas = util::Rgb(0xEFE2D4);
        theme.border = util::Rgb(0xD8C1AE);
        theme.accent = util::Rgb(0xB84A28);
        theme.accentFg = util::Rgb(0xFFF8F0);
        theme.success = util::Rgb(0x4E7A46);
        break;
    case 3: /* 深海 */
        theme.dark = true;
        theme.fg = util::Rgb(0xE8F5F2);
        theme.fgSecondary = util::Rgb(0x9DBDB8);
        theme.fgTertiary = util::Rgb(0x6E918C);
        theme.bg = util::Rgb(0x123236);
        theme.canvas = util::Rgb(0x0C2427);
        theme.border = util::Rgb(0x2A4D51);
        theme.accent = util::Rgb(0x5CC8B6);
        theme.accentFg = util::Rgb(0x08201F);
        theme.success = util::Rgb(0x6DD3A1);
        break;
    default: /* 纸白 */
        theme.dark = false;
        theme.fg = util::Rgb(0x24211B);
        theme.fgSecondary = util::Rgb(0x6B6257);
        theme.fgTertiary = util::Rgb(0x948879);
        theme.bg = util::Rgb(0xFFFCF5);
        theme.canvas = util::Rgb(0xF4F0E6);
        theme.border = util::Rgb(0xD9CFC0);
        theme.accent = util::Rgb(0xB65C34);
        theme.accentFg = util::Rgb(0xFFFCF5);
        theme.success = util::Rgb(0x2F7D55);
        break;
    }
    theme.contentBorder = theme.border;
    theme.card = theme.bg;
    return theme;
}

Theme Theme::MenuStyle(bool dark)
{
    // 轻雪的菜单色板按 0xRRGGBBAA 编码（alpha 在低字节）。
    auto argb = [](uint32_t v) {
        return D2D1::ColorF(static_cast<float>((v >> 24) & 0xFF) / 255.0f,
                            static_cast<float>((v >> 16) & 0xFF) / 255.0f,
                            static_cast<float>((v >> 8) & 0xFF) / 255.0f,
                            static_cast<float>(v & 0xFF) / 255.0f);
    };
    Theme theme;
    theme.dark = dark;
    if (dark) {
        theme.bg = argb(0x1F1F1FFF);
        theme.fg = argb(0xF5F5F5E6);
        theme.fgSecondary = argb(0xF5F5F5C0);
        theme.fgTertiary = argb(0x888888FF);
        theme.border = argb(0xFFFFFF1A);
        theme.accent = argb(0x363635FF);
        theme.input = theme.accent;
        theme.secondary = argb(0x262626FF);
        theme.primary = argb(0xFAFAFAFF);
        theme.primaryFg = argb(0x171717FF);
        theme.canvas = theme.bg;
        theme.accentFg = theme.fg;
        theme.success = argb(0x5BD18BFF);
        theme.contentBorder = argb(0x262626FF);
        theme.card = theme.bg;
    } else {
        theme.bg = argb(0xFFFFFFFF);
        theme.fg = argb(0x0B0C0EE0);
        theme.fgSecondary = argb(0x0B0C0EC0);
        theme.fgTertiary = argb(0x888888FF);
        theme.border = argb(0x0000001A);
        theme.accent = argb(0xF4F4F5FF);
        theme.input = theme.accent;
        theme.secondary = argb(0xF4F4F5FF);
        theme.primary = argb(0x171717FF);
        theme.primaryFg = argb(0xFAFAFAFF);
        theme.canvas = theme.bg;
        theme.accentFg = theme.fg;
        theme.success = argb(0x2F7D55FF);
        theme.contentBorder = argb(0xEEEEEEFF);
        theme.card = theme.bg;
    }
    return theme;
}

Theme Theme::HostUi(const SnowAirHost* host, void* user)
{
    auto rgba = [](uint32_t v) {
        return D2D1::ColorF(static_cast<float>((v >> 24) & 0xFF) / 255.0f,
                            static_cast<float>((v >> 16) & 0xFF) / 255.0f,
                            static_cast<float>((v >> 8) & 0xFF) / 255.0f,
                            static_cast<float>(v & 0xFF) / 255.0f);
    };
    const bool dark = host && host->is_dark ? host->is_dark(user) != 0 : false;
    Theme theme = MenuStyle(dark);
    if (!host || !host->theme_color) return theme;
    const Theme fallback = theme;
    theme.fg = rgba(host->theme_color(user, SA_THEME_FG));
    theme.fgSecondary = rgba(host->theme_color(user, SA_THEME_FG_SECONDARY));
    theme.fgTertiary = rgba(host->theme_color(user, SA_THEME_FG_TERTIARY));
    theme.bg = rgba(host->theme_color(user, SA_THEME_POPOVER));
    theme.input = rgba(host->theme_color(user, SA_THEME_INPUT));
    theme.secondary = rgba(host->theme_color(user, SA_THEME_SECONDARY));
    theme.primary = rgba(host->theme_color(user, SA_THEME_PRIMARY));
    theme.primaryFg = rgba(host->theme_color(user, SA_THEME_PRIMARY_FG));
    theme.canvas = rgba(host->theme_color(user, SA_THEME_CANVAS));
    theme.border = rgba(host->theme_color(user, SA_THEME_BORDER));
    theme.accent = rgba(host->theme_color(user, SA_THEME_ACCENT));
    theme.contentBorder = rgba(host->theme_color(user, SA_THEME_CONTENT_BORDER));
    theme.card = rgba(host->theme_color(user, SA_THEME_BG));
    if (theme.bg.a < 0.01f) theme.bg = fallback.bg;
    if (theme.fg.a < 0.01f) theme.fg = fallback.fg;
    if (theme.fgSecondary.a < 0.01f) theme.fgSecondary = fallback.fgSecondary;
    if (theme.fgTertiary.a < 0.01f) theme.fgTertiary = fallback.fgTertiary;
    if (theme.border.a < 0.01f) theme.border = fallback.border;
    if (theme.input.a < 0.01f) theme.input = fallback.input;
    if (theme.secondary.a < 0.01f) theme.secondary = fallback.secondary;
    if (theme.primary.a < 0.01f) theme.primary = fallback.primary;
    if (theme.primaryFg.a < 0.01f) theme.primaryFg = fallback.primaryFg;
    if (theme.accent.a < 0.01f) theme.accent = fallback.accent;
    if (theme.contentBorder.a < 0.01f) theme.contentBorder = fallback.contentBorder;
    if (theme.card.a < 0.01f) theme.card = fallback.card;
    return theme;
}

void Theme::Load(const SnowAirHost* host, void* user)
{
    int themeIndex = 0;
    if (host && host->theme_override) {
        themeIndex = host->theme_override(user);
    }
    if (themeIndex < 0 && host && host->config_get_int) {
        themeIndex = host->config_get_int(user, L"rack.theme", 0);
    }
    *this = RackTheme(std::clamp(themeIndex, 0, 3));
}
